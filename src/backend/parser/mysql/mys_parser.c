/*-------------------------------------------------------------------------
 *
 * mys_parser.c
 *    MySQL-compatibility parser entry point.
 *
 * Initializes the MySQL flex scanner (mys_scan.l), drives the bison
 * grammar (mys_gram.y) via mys_yyparse(), and returns the raw parse
 * tree.  The ParserRoutine is registered in parsereng.c.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/parser/mysql/mys_parser.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

/*
 * The MySQL grammar interface is included early: the generated mys_gram.h
 * declares a bison token (MYS_ENGINE) that would collide with OpenSSL's
 * ENGINE type pulled in by headers below (fmgr.h / libpq-be.h via
 * parsereng.h).  The token was renamed to avoid the clash, and keeping
 * this include first preserves that invariant.
 */
#include "parser/mysql/mys_gramparse.h"

#include "fmgr.h"                  /* PG_MODULE_MAGIC */
#include "mb/pg_wchar.h"
#include "parser/parser.h"
#include "parser/parsereng.h"      /* RegisterParserRoutine */
#include "parser/scansup.h"
#include "parser/mysql/mys_expr_transform.h"
#include "parser/mysql/mys_parse_utilcmd.h"
#include "parser/mysql/mys_parser.h"
#include "parser/mysql/mys_parser_exports.h"
#include "parser/mysql/mys_scanner.h"

/* in mys_keywords.c -- generated keyword token map */
#include "parser/mysql/mys_keywords.h"

PG_MODULE_MAGIC;

/* Save previous token for BINARY/REGEXP lookahead */
static int prev_token;

/* Forward declarations for Unicode escape helpers */
static bool check_uescapechar(unsigned char escape);
static char *str_udeescape(const char *str, char escape,
						   int position, core_yyscan_t yyscanner);
static unsigned int hexval(unsigned char c);
static void check_unicode_value(pg_wchar c);

/*
 * mys_yylex  --  MySQL scanner wrapper with lookahead logic.
 *
 * Called by the bison-generated mys_yyparse() to obtain the next token.
 * Handles multi-token lookahead for NOT_LA, NULLS_LA, WITH_LA, UIDENT,
 * USCONST, and BINARY_LA transformations.
 */
int
mys_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner)
{
	mys_yy_extra_type *yyextra = mys_yyget_extra(yyscanner);
	int			cur_token;
	int			next_token;
	int			cur_token_length;
	YYLTYPE		cur_yylloc;

	/* Get next token --- we might already have it */
	if (yyextra->have_lookahead)
	{
		cur_token = yyextra->lookahead_token;
		lvalp->core_yystype = yyextra->lookahead_yylval;
		*llocp = yyextra->lookahead_yylloc;
		if (yyextra->lookahead_end)
			*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;
		yyextra->have_lookahead = false;
	}
	else
		cur_token = mys_core_yylex(&(lvalp->core_yystype), llocp, yyscanner);

	/*
	 * If this token isn't one that requires lookahead, just return it.
	 */
	switch (cur_token)
	{
		case NOT:
			cur_token_length = 3;
			break;
		case NULLS_P:
			cur_token_length = 5;
			break;
		case WITH:
			cur_token_length = 4;
			break;
		case UIDENT:
		case USCONST:
			cur_token_length = strlen(yyextra->core_yy_extra.scanbuf + *llocp);
			break;
		case BINARY:
			cur_token_length = 6;
			break;

		default:
			prev_token = cur_token;
			return cur_token;
	}

	/*
	 * Identify end+1 of current token.  mys_core_yylex() has temporarily
	 * stored a '\0' here, and will undo that when we call it again.
	 */
	yyextra->lookahead_end = yyextra->core_yy_extra.scanbuf +
		*llocp + cur_token_length;
	Assert(*(yyextra->lookahead_end) == '\0');

	/*
	 * Save and restore *llocp around the call so error reports point to
	 * the current token, not the next one.
	 */
	cur_yylloc = *llocp;

	/* Get next token, saving outputs into lookahead variables */
	next_token = mys_core_yylex(&(yyextra->lookahead_yylval), llocp, yyscanner);
	yyextra->lookahead_token = next_token;
	yyextra->lookahead_yylloc = *llocp;

	*llocp = cur_yylloc;

	/* Revert the un-truncation of the current token */
	yyextra->lookahead_hold_char = *(yyextra->lookahead_end);
	*(yyextra->lookahead_end) = '\0';

	yyextra->have_lookahead = true;

	/* Replace cur_token if needed, based on lookahead */
	switch (cur_token)
	{
		case NOT:
			switch (next_token)
			{
				case BETWEEN:
				case IN_P:
				case LIKE:
				case ILIKE:
				case SIMILAR:
					cur_token = NOT_LA;
					break;
			}
			break;

		case NULLS_P:
			switch (next_token)
			{
				case FIRST_P:
				case LAST_P:
					cur_token = NULLS_LA;
					break;
			}
			break;

		case WITH:
			switch (next_token)
			{
				case TIME:
				case ORDINALITY:
				case ROLLUP:
					cur_token = WITH_LA;
					break;
			}
			break;

		case UIDENT:
		case USCONST:
			{
				/* Look ahead for UESCAPE */
				if (next_token == UESCAPE)
				{
					const char *escstr;

					cur_yylloc = *llocp;
					*(yyextra->lookahead_end) = yyextra->lookahead_hold_char;

					next_token = mys_core_yylex(&(yyextra->lookahead_yylval),
											llocp, yyscanner);

					if (next_token != SCONST)
						scanner_yyerror("UESCAPE must be followed by a simple string literal",
										yyscanner);

					escstr = yyextra->lookahead_yylval.str;
					if (strlen(escstr) != 1 || !check_uescapechar(escstr[0]))
						scanner_yyerror("invalid Unicode escape character",
										yyscanner);

					*llocp = cur_yylloc;

					lvalp->core_yystype.str =
						str_udeescape(lvalp->core_yystype.str,
									  escstr[0], *llocp, yyscanner);

					yyextra->have_lookahead = false;
				}
				else
				{
					lvalp->core_yystype.str =
						str_udeescape(lvalp->core_yystype.str,
									  '\\', *llocp, yyscanner);
				}

				if (cur_token == UIDENT)
				{
					truncate_identifier(lvalp->core_yystype.str,
										strlen(lvalp->core_yystype.str),
										true);
					cur_token = IDENT;
				}
				else if (cur_token == USCONST)
				{
					cur_token = SCONST;
				}
			}
			break;

		case BINARY:
			if (prev_token == REGEXP)
				cur_token = BINARY_LA;
			break;
	}

	prev_token = cur_token;
	return cur_token;
}

/* Unicode escape helpers */
static bool
check_uescapechar(unsigned char escape)
{
	if (isxdigit(escape)
		|| escape == '+'
		|| escape == '\''
		|| escape == '"'
		|| scanner_isspace(escape))
		return false;
	return true;
}

static unsigned int
hexval(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 0xA;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 0xA;
	elog(ERROR, "invalid hexadecimal digit");
	return 0;
}

static void
check_unicode_value(pg_wchar c)
{
	if (!is_valid_unicode_codepoint(c))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid Unicode escape value")));
}

static char *
str_udeescape(const char *str, char escape,
			  int position, core_yyscan_t yyscanner)
{
	const char *in;
	char	   *new,
			   *out;
	size_t		new_len;
	pg_wchar	pair_first = 0;
	ScannerCallbackState scbstate;

	new_len = strlen(str) + MAX_UNICODE_EQUIVALENT_STRING + 1;
	new = palloc(new_len);

	in = str;
	out = new;
	while (*in)
	{
		size_t		out_dist = out - new;

		if (out_dist > new_len - (MAX_UNICODE_EQUIVALENT_STRING + 1))
		{
			new_len *= 2;
			new = repalloc(new, new_len);
			out = new + out_dist;
		}

		if (in[0] == escape)
		{
			setup_scanner_errposition_callback(&scbstate, yyscanner,
											   in - str + position + 3);
			if (in[1] == escape)
			{
				if (pair_first)
					goto invalid_pair;
				*out++ = escape;
				in += 2;
			}
			else if (isxdigit((unsigned char) in[1]) &&
					 isxdigit((unsigned char) in[2]) &&
					 isxdigit((unsigned char) in[3]) &&
					 isxdigit((unsigned char) in[4]))
			{
				pg_wchar	unicode;

				unicode = (hexval(in[1]) << 12) +
					(hexval(in[2]) << 8) +
					(hexval(in[3]) << 4) +
					hexval(in[4]);
				check_unicode_value(unicode);
				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) out);
					out += strlen(out);
				}
				in += 5;
			}
			else if (in[1] == '+' &&
					 isxdigit((unsigned char) in[2]) &&
					 isxdigit((unsigned char) in[3]) &&
					 isxdigit((unsigned char) in[4]) &&
					 isxdigit((unsigned char) in[5]) &&
					 isxdigit((unsigned char) in[6]) &&
					 isxdigit((unsigned char) in[7]))
			{
				pg_wchar	unicode;

				unicode = (hexval(in[2]) << 20) +
					(hexval(in[3]) << 16) +
					(hexval(in[4]) << 12) +
					(hexval(in[5]) << 8) +
					(hexval(in[6]) << 4) +
					hexval(in[7]);
				check_unicode_value(unicode);
				if (pair_first)
				{
					if (is_utf16_surrogate_second(unicode))
					{
						unicode = surrogate_pair_to_codepoint(pair_first, unicode);
						pair_first = 0;
					}
					else
						goto invalid_pair;
				}
				else if (is_utf16_surrogate_second(unicode))
					goto invalid_pair;

				if (is_utf16_surrogate_first(unicode))
					pair_first = unicode;
				else
				{
					pg_unicode_to_server(unicode, (unsigned char *) out);
					out += strlen(out);
				}
				in += 8;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid Unicode escape"),
						 errhint("Unicode escapes must be \\XXXX or \\+XXXXXX.")));

			cancel_scanner_errposition_callback(&scbstate);
		}
		else
		{
			if (pair_first)
				goto invalid_pair;
			*out++ = *in++;
		}
	}

	if (pair_first)
		goto invalid_pair;

	*out = '\0';
	return new;

invalid_pair:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid Unicode surrogate pair"),
			 scanner_errposition(in - str + position + 3,
								 yyscanner)));
	return NULL;
}

/*
 * mys_raw_parser  --  parse a MySQL SQL string.
 *
 * Initializes the MySQL flex scanner, drives the bison grammar, and
 * returns the raw parse tree (List of RawStmt).  Returns NIL on error.
 */
List *
mys_raw_parser(const char *str, RawParseMode mode)
{
	core_yyscan_t yyscanner;
	mys_yy_extra_type yyextra;
	int			yyresult;
	List	   *result = NIL;
	ListCell   *lc;

	/* initialize the flex scanner */
	yyscanner = mys_scanner_init(str, &yyextra.core_yy_extra,
							 &MysScanKeywords, MysScanKeywordTokens);

	if (mode == RAW_PARSE_DEFAULT)
		yyextra.have_lookahead = false;
	else
	{
		/* this array is indexed by RawParseMode enum */
		static const int mode_token[] = {
			0,					/* RAW_PARSE_DEFAULT */
			MODE_TYPE_NAME,		/* RAW_PARSE_TYPE_NAME */
			MODE_PLPGSQL_EXPR,	/* RAW_PARSE_PLPGSQL_EXPR */
			MODE_PLPGSQL_ASSIGN1,	/* RAW_PARSE_PLPGSQL_ASSIGN1 */
			MODE_PLPGSQL_ASSIGN2,	/* RAW_PARSE_PLPGSQL_ASSIGN2 */
			MODE_PLPGSQL_ASSIGN3	/* RAW_PARSE_PLPGSQL_ASSIGN3 */
		};

		yyextra.have_lookahead = true;
		yyextra.lookahead_token = mode_token[mode];
		yyextra.lookahead_yylloc = 0;
		yyextra.lookahead_end = NULL;
	}

	/* initialize the bison parser */
	mys_parser_init(&yyextra);

	/* Parse! */
	yyresult = mys_yyparse(yyscanner);

	/* Clean up (release memory) */
	mys_scanner_finish(yyscanner);

	if (yyresult)				/* error */
		return NIL;

	/*
	 * A few MySQL grammar actions lower directly to standard PostgreSQL SQL
	 * text.  Reparse those replacement strings with the standard parser
	 * before handing the raw parse list to analysis/utility dispatch.
	 */
	foreach(lc, yyextra.parsetree)
	{
		RawStmt    *rawstmt = lfirst_node(RawStmt, lc);

		if (IsA(rawstmt->stmt, String))
			result = list_concat(result,
							 raw_parser(strVal(rawstmt->stmt), RAW_PARSE_DEFAULT));
		else
			result = lappend(result, rawstmt);
	}

	return result;
}

/*
 * MySQLParserRoutine
 *
 * The dialect vtable exposed to the kernel through RegisterParserRoutine()
 * in _PG_init.  It lives here so the MySQL parser can ship as a loadable
 * module (mysql_parser.so) instead of being compiled into the backend;
 * see parsereng.h and the mysm module for the same pattern.
 */
const ParserRoutine MySQLParserRoutine = {
	.raw_parse = mys_raw_parser,
	.transformOptionalSelectInto = mys_transformOptionalSelectInto,
	.transformOnConflictArbiter = mys_transformOnConflictArbiter,
	.transform_expr_node = mys_transform_expr_node,
	.figure_colname = mys_figure_colname,
};

/*
 * G3 exports: kernel-facing entry points into this module.  The kernel
 * cannot link against the module's symbols, so it calls through this
 * table; the slot is defined in the kernel and filled here at load time.
 */
MysParserExports mys_parser_exports_data = {
	.mys_transformCreateStmt = mys_transformCreateStmt,
	.mys_transformAlterTableStmt = mys_transformAlterTableStmt,
	.mys_expandTableLikeClause = mys_expandTableLikeClause,
	.getCurrentNamespaceOid = getCurrentNamespaceOid,
	.createAutoIncrementTriggerFunc = createAutoIncrementTriggerFunc,
	.createAutoUpdateTimeStampTriggerFunc = createAutoUpdateTimeStampTriggerFunc,
	.createAutoUpdateTimeStampTrigger = createAutoUpdateTimeStampTrigger,
	.MysScanKeywords = &MysScanKeywords,
	.MysScanKeywordCategories = MysScanKeywordCategories,
};

/*
 * Module load hook: register the MySQL dialect parser and publish the
 * kernel-facing exports.  The kernel dispatches by MyCompatMode in
 * InitParserEngine().
 */
void
_PG_init(void)
{
	RegisterParserRoutine(COMPAT_PROTOCOL_MYSQL, &MySQLParserRoutine);
	mys_parser_exports = &mys_parser_exports_data;
}
