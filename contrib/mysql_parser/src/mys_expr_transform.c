#include "postgres.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "commands/mysql/mys_uservar.h"
#include "parser/parserapi.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/mysql/mys_expr_transform.h"
#include "adapter/mysql/systemVar.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

/*
 * MySQL exposes the source spelling of unaliased literals as their result
 * column label (for example, SELECT 1 produces a column named "1").  The
 * PostgreSQL default is "?column?".  This hook only handles expressions for
 * which MySQL has a dialect-specific label and lets FigureColname() handle
 * every other raw node.
 */
char *
mys_figure_colname(Node *expr)
{
	A_Const    *constant;

	if (expr == NULL)
		return NULL;

	if (IsA(expr, A_Const))
	{
		constant = (A_Const *) expr;
		if (constant->isnull)
			return pstrdup("NULL");

		switch (constant->val.node.type)
		{
			case T_Integer:
				return psprintf("%d", constant->val.ival.ival);
			case T_Float:
				return pstrdup(constant->val.fval.fval);
			case T_String:
				return pstrdup(constant->val.sval.sval);
			case T_Boolean:
				return pstrdup(constant->val.boolval.boolval ? "TRUE" : "FALSE");
			case T_BitString:
				return pstrdup(constant->val.bsval.bsval);
			default:
				return NULL;
		}
	}

	/*
	 * Degraded user/system variable references are FuncCall nodes:
	 * pg_catalog.mys_get_user_var('name') and
	 * pg_catalog.mys_get_system_variable('name', bool).  Restore the
	 * MySQL @name / @@name result-column labels.
	 */
	if (IsA(expr, FuncCall))
	{
		FuncCall   *fn = (FuncCall *) expr;
		A_Const    *arg;
		char	   *fname;

		if (list_length(fn->funcname) == 2 &&
			pg_strcasecmp(strVal(linitial(fn->funcname)), "pg_catalog") == 0)
		{
			fname = strVal(lsecond(fn->funcname));
			if ((pg_strcasecmp(fname, "mys_get_user_var") == 0 ||
				 pg_strcasecmp(fname, "mys_set_user_var") == 0 ||
				 pg_strcasecmp(fname, "mys_get_system_variable") == 0) &&
				list_length(fn->args) >= 1 &&
				IsA(linitial(fn->args), A_Const))
			{
				arg = (A_Const *) linitial(fn->args);
				if (!arg->isnull && arg->val.node.type == T_String)
					return psprintf("%s%s",
									pg_strcasecmp(fname, "mys_get_system_variable") == 0 ?
									"@@" : "@",
									arg->val.sval.sval);
			}
		}
	}

	return NULL;
}

/*
 * Lower MySQL-specific raw expression nodes to standard PG nodes.
 *
 * System variables with real session semantics are lowered to volatile
 * builtins here.  Read-only compatibility probes retain their historic
 * literal results.  UserVarRef and UserVarAssign are lowered here too.
 */
bool
mys_transform_expr_node(ParseState *pstate, Node *expr, Node **result)
{
    if (expr == NULL)
        return false;

    switch (nodeTag(expr))
    {
	case T_A_Expr:
		{
			A_Expr *aexpr = (A_Expr *) expr;
			const char *opname = NULL;

			/* MySQL <=> is SQL's null-safe equality operator. */
			if (aexpr->kind == AEXPR_OP && list_length(aexpr->name) == 1 &&
				strcmp(strVal(linitial(aexpr->name)), "<=>") == 0)
			{
				aexpr->kind = AEXPR_NOT_DISTINCT;
				aexpr->name = list_make1(makeString("="));
				*result = transformExpr(pstate, (Node *) aexpr,
										pstate->p_expr_kind);
				return true;
			}

			if (aexpr->kind == AEXPR_OP && list_length(aexpr->name) == 1)
				opname = strVal(linitial(aexpr->name));

			/*
			 * MySQL writes JSON paths as '$.key', while PostgreSQL's json
			 * ->> operator treats the right operand as one literal key.  Lower
			 * the path form to the extension helper before normal operator
			 * resolution; bare PostgreSQL-style keys retain native semantics.
			 */
			if (opname != NULL && strcmp(opname, "->>") == 0 &&
				aexpr->rexpr != NULL && IsA(aexpr->rexpr, A_Const))
			{
				A_Const *path = (A_Const *) aexpr->rexpr;
				FuncCall *fn;

				if (!path->isnull && path->val.node.type == T_String &&
					strncmp(path->val.sval.sval, "$.", 2) == 0)
				{
					fn = makeNode(FuncCall);
					fn->funcname = list_make2(makeString("mysql"),
										 makeString("json_path_extract_text"));
					fn->args = list_make2(aexpr->lexpr, aexpr->rexpr);
					fn->agg_order = NIL;
					fn->agg_filter = NULL;
					fn->over = NULL;
					fn->agg_within_group = false;
					fn->agg_star = false;
					fn->agg_distinct = false;
					fn->func_variadic = false;
					fn->funcformat = COERCE_EXPLICIT_CALL;
					fn->location = aexpr->location;
					*result = transformExpr(pstate, (Node *) fn,
										pstate->p_expr_kind);
					return true;
				}
			}

			/*
			 * In non-strict mode MySQL converts an empty string to zero in a
			 * numeric expression.  Do this while the raw literal and session
			 * sql_mode are still visible; PG18's normal operator resolver can
			 * then select its native numeric operator without weakening any
			 * PostgreSQL input function or cast globally.
			 */
			if (opname != NULL &&
				(strcmp(opname, "+") == 0 || strcmp(opname, "-") == 0 ||
				 strcmp(opname, "*") == 0 || strcmp(opname, "/") == 0 ||
				 strcmp(opname, "%") == 0) &&
				(mys_sqlMode & (MYS_MODE_STRICT_TRANS_TABLES |
								MYS_MODE_STRICT_ALL_TABLES)) == 0)
			{
				Node **operands[2] = {&aexpr->lexpr, &aexpr->rexpr};
				int i;

				for (i = 0; i < lengthof(operands); i++)
				{
					Node *operand = *operands[i];

					if (operand != NULL && IsA(operand, A_Const))
					{
						A_Const *constant = (A_Const *) operand;

						if (!constant->isnull &&
							constant->val.node.type == T_String &&
							constant->val.sval.sval[0] == '\0')
						{
							constant->val.node.type = T_Integer;
							constant->val.ival.ival = 0;
						}
						else if (!constant->isnull &&
							constant->val.node.type == T_String)
						{
							const char *literal = constant->val.sval.sval;
							char *endptr;
							double numeric;

							errno = 0;
							numeric = strtod(literal, &endptr);
							while (isspace((unsigned char) *endptr))
								endptr++;

							/* MySQL non-strict arithmetic consumes a numeric prefix. */
							if (endptr != literal && *endptr != '\0' &&
								errno != ERANGE && isfinite(numeric))
							{
								if (numeric >= INT_MIN && numeric <= INT_MAX &&
									floor(numeric) == numeric)
								{
									constant->val.node.type = T_Integer;
									constant->val.ival.ival = (int) numeric;
								}
								else
								{
									constant->val.node.type = T_Float;
									constant->val.fval.fval = psprintf("%.17g", numeric);
								}
							}
						}
					}
				}
			}
			break;
		}

	case T_FuncCall:
		{
			FuncCall *fn = (FuncCall *) expr;
			bool		is_mysql_concat = false;

			/*
			 * The grammar lowers @name to pg_catalog.mys_get_user_var('name').
			 * Coerce the text result to the stored variable type so that
			 * arithmetic/comparisons on a numeric user variable resolve to
			 * the native operator (e.g. SET @n := 5; SELECT @n + 1).  The
			 * FuncCall is transformed first, then wrapped in an explicit
			 * cast to the stored type.
			 */
			if (list_length(fn->funcname) == 2 &&
				pg_strcasecmp(strVal(linitial(fn->funcname)), "pg_catalog") == 0 &&
				pg_strcasecmp(strVal(lsecond(fn->funcname)), "mys_get_user_var") == 0 &&
				list_length(fn->args) == 1 && IsA(linitial(fn->args), A_Const))
			{
				A_Const    *name_const = (A_Const *) linitial(fn->args);
				Oid			valueType;
				Node	   *transformed;
				Node	   *coerced;

				if (!name_const->isnull &&
					name_const->val.node.type == T_String &&
					OidIsValid(valueType = mysGetUserVarTypeInternal(
								   name_const->val.sval.sval)))
				{
					/*
					 * Transform the mys_get_user_var call without re-entering
					 * this hook (the degraded FuncCall would recurse), then
					 * cast the text result to the stored variable type.
					 */
					const ParserRoutine *save_routine = pstate->p_parser_routine;

					pstate->p_parser_routine = NULL;
					transformed = transformExpr(pstate, (Node *) fn,
												pstate->p_expr_kind);
					pstate->p_parser_routine = save_routine;

					coerced = coerce_to_target_type(pstate, transformed,
													exprType(transformed),
													getBaseType(valueType), -1,
													COERCION_EXPLICIT,
													COERCE_EXPLICIT_CAST,
													fn->location);
					if (coerced == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_CANNOT_COERCE),
								 errmsg("cannot coerce MySQL user variable")));
					*result = coerced;
					return true;
				}
			}

			/*
			 * PG16 routed function resolution through mys_ParseFuncOrColumn(),
			 * which renamed these MySQL spellings before catalog lookup.  PG18's
			 * ParserRoutine exposes the earlier expression hook instead, so do
			 * the same raw-name lowering here and let normal resolution continue.
			 */
			if (list_length(fn->funcname) == 1)
			{
				char *fname = strVal(linitial(fn->funcname));

				if (pg_strcasecmp(fname, "json_object") == 0)
					fn->funcname = list_make1(makeString("mys_json_object"));
				else if (pg_strcasecmp(fname, "json_array") == 0)
					fn->funcname = list_make1(makeString("json_build_array"));
				/*
				 * DATABASE()/SCHEMA() are MySQL synonyms for "the database
				 * selected by the last USE".  UsedbStmt (mys_gram.y) maps
				 * USE to `SET search_path = '<name>', "$user", public,
				 * mysql, pg_catalog`, so the equivalent here is PostgreSQL's
				 * own current_schema(): the first name in search_path that
				 * resolves to a schema that actually exists -- exactly
				 * DATABASE()'s "no database selected" -> NULL semantics too.
				 */
				else if (fn->args == NIL &&
						 (pg_strcasecmp(fname, "database") == 0 ||
						  pg_strcasecmp(fname, "schema") == 0))
					fn->funcname = list_make1(makeString("current_schema"));
			}

			if (fn->args != NIL && fn->agg_order == NIL &&
				fn->agg_filter == NULL && fn->over == NULL &&
				!fn->agg_star && !fn->agg_distinct)
			{
				if (list_length(fn->funcname) == 1)
					is_mysql_concat =
						pg_strcasecmp(strVal(linitial(fn->funcname)), "concat") == 0;
				else if (list_length(fn->funcname) == 2)
					is_mysql_concat =
						pg_strcasecmp(strVal(linitial(fn->funcname)), "mysql") == 0 &&
						pg_strcasecmp(strVal(lsecond(fn->funcname)), "concat") == 0;
			}

			/*
			 * MySQL CONCAT converts every argument to text before concatenating.
			 * A VARIADIC text[] extension function cannot request those explicit
			 * casts itself, so preserve the raw call and cast each argument here.
			 */
			if (is_mysql_concat)
			{
				List	   *args = NIL;
				ListCell   *lc;

				foreach(lc, fn->args)
				{
					TypeCast *cast = makeNode(TypeCast);

					cast->arg = lfirst(lc);
					cast->typeName = makeTypeName(pstrdup("text"));
					cast->location = fn->location;
					args = lappend(args, cast);
				}
				fn->args = args;
				return false;
			}

			/*
			 * Intercept VERSION() in MySQL protocol: return the MySQL
			 * server version from GUC instead of PG's built-in version().
			 * Match only the unqualified, no-argument form so that
			 * pg_catalog.version() still returns the PG version.
			 */
			if (fn->args == NIL && fn->agg_order == NIL &&
				fn->agg_filter == NULL && fn->over == NULL &&
				!fn->agg_star && !fn->agg_distinct &&
				list_length(fn->funcname) == 1)
			{
				char *fname = strVal(linitial(fn->funcname));

				if (pg_strcasecmp(fname, "version") == 0)
				{
					*result = (Node *) make_const(pstate,
						(A_Const *) makeStringConst(
							pstrdup(mysql_server_version),
							fn->location));
					return true;
				}
			}
			break;
		}

    default:
        break;
    }

    return false;
}
