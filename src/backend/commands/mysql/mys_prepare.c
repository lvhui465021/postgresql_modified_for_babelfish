/*-------------------------------------------------------------------------
 *
 * mys_prepare.c
 *	  MySQL COM_STMT_PREPARE / COM_STMT_EXECUTE support
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/mysql/mys_prepare.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/mysql/mys_uservar.h"
#include "commands/prepare.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "parser/parsereng.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/utility.h"

static bool mysUtilityCanPrepare(Node *parsetree);

/*
 * Implements the 'PREPARE' utility statement for MySQL.
 */
void
mys_PrepareQuery(ParseState *pstate, PrepareStmt *stmt,
			     int stmt_location, int stmt_len)
{
    List *raw_parsetree_list;
	RawStmt *rawstmt;
    char *prepareStmt;
	CachedPlanSource *plansource;
	Oid	*argtypes = NULL;
	int	nargs;
	Query *query;
	List *query_list;
	int i;

	/*
	 * Disallow empty-string statement name (conflicts with protocol-level
	 * unnamed statement).
	 */
	if (!stmt->name || stmt->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
				 errmsg("invalid statement name: must not be empty")));

	DropPreparedStatement(stmt->name, false);

	/*
	 * Need to wrap the contained statement in a RawStmt node to pass it to
	 * parse analysis.
	 */
    prepareStmt = NULL;
    if (nodeTag(stmt->query) == T_String)
    {
		prepareStmt = strVal(stmt->query);
    }
    else
    {
        char *userVarName = mys_extract_user_var_name(stmt->query);

        if (userVarName != NULL)
        {
            bytea *userVarValue = mysGetUserVarValueInternal(userVarName);

            if (userVarValue != NULL)
            {
                char *byte = VARDATA_ANY(userVarValue);
                size_t byteLen = VARSIZE_ANY_EXHDR(userVarValue);

                prepareStmt = pnstrdup(byte, byteLen);

                if (pg_verifymbstr(byte, byteLen, true))
                {
                    /* Nothing to do */
                }
                else
                {
                    elog(ERROR, "user variable %s is not a string literal", userVarName);
                }
            }
            else
            {
                elog(ERROR, "user variable %s is NULL", userVarName);
            }
        }
        else
        {
            elog(ERROR, "unrecognized PREPARE source");
        }
    }

	/*
	 * The SQL text carried by MySQL PREPARE is MySQL dialect text.  In
	 * particular, '?' is a MySQL parameter marker, so parsing it through the
	 * standard PostgreSQL entry point loses the parameter before analysis.
	 */
	raw_parsetree_list = parserengine->raw_parse(prepareStmt,
																 RAW_PARSE_DEFAULT);
	if (list_length(raw_parsetree_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot prepare multiple statements")));
	rawstmt = (RawStmt *)linitial(raw_parsetree_list);
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	/*
	 * Create the CachedPlanSource before we do parse analysis, since it needs
	 * to see the unmodified raw parse tree.
	 */
	plansource = CreateCachedPlan(rawstmt, prepareStmt,
								  CMD_UNKNOWN);

	/* Transform list of TypeNames to array of type OIDs */
	nargs = list_length(stmt->argtypes);

	if (nargs)
	{
		ListCell   *l;

		argtypes = (Oid *) palloc(nargs * sizeof(Oid));
		i = 0;

		foreach(l, stmt->argtypes)
		{
			TypeName   *tn = lfirst(l);
			Oid			toid = typenameTypeId(pstate, tn);

			argtypes[i++] = toid;
		}
	}

	/*
	 * Analyze the statement using these parameter types (any parameters
	 * passed in from above us will not be visible to it), allowing
	 * information about unknown parameters to be deduced from context.
	 */
	query = parse_analyze_varparams_with_routine(rawstmt, prepareStmt,
																						 &argtypes, &nargs,
																						 pstate->p_queryEnv,
																						 parserengine);
    plansource->commandTag = CreateCommandTag((Node *)query);

	/*
	 * Check that all parameter types were determined.
	 */
	for (i = 0; i < nargs; i++)
	{
		Oid			argtype = argtypes[i];

		if (argtype == InvalidOid || argtype == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of parameter $%d",
							i + 1)));
	}

	/*
	 * grammar only allows PreparableStmt, so this check should be redundant
	 */
	switch (query->commandType)
	{
		case CMD_SELECT:
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			/* OK */
			break;
        case CMD_UTILITY:
            if (mysUtilityCanPrepare(query->utilityStmt))
            {
                /* OK */
            }
            else
            {
                ereport(ERROR,
					    (errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
					     errmsg("This command is not supported in the prepared statement protocol yet")));
            }
            break;
		default:
            /* should not reach here */
			ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
                     errmsg("This command is not supported in the prepared statement protocol yet")));
			break;
	}

	/* Rewrite the query. The result could be 0, 1, or many queries. */
	query_list = QueryRewrite(query);

	/* Finish filling in the CachedPlanSource */
	CompleteCachedPlan(plansource,
					   query_list,
					   NULL,
					   argtypes,
					   nargs,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* allow parallel mode */
					   true);	/* fixed result */

	/*
	 * Save the results.
	 */
	StorePreparedStatement(stmt->name,
						   plansource,
						   true);
}

static bool
mysUtilityCanPrepare(Node *parsetree)
{
    switch (nodeTag(parsetree))
    {
        case T_VariableSetStmt:
            return true;
        
        case T_CreateStmt:
        case T_CreateTableAsStmt:
            return true;
        
        case T_AlterTableStmt:
            return true;

        case T_CallStmt:
            return true;
		
		case T_ViewStmt:
			return true;
		
		case T_DropStmt:
			{
				DropStmt *stmt = (DropStmt *)parsetree;
				if (stmt->removeType == OBJECT_INDEX ||
					stmt->removeType == OBJECT_SCHEMA ||
					stmt->removeType == OBJECT_TABLE ||
					stmt->removeType == OBJECT_VIEW ||
					stmt->removeType == OBJECT_SEQUENCE)
				{
					return true;
				}
				else
				{
					return false;
				}
			}
        
        default:
            return false;
    }
}
