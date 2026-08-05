/*-------------------------------------------------------------------------
 *
 * mys_gramparse.h
 *    Shared types for the MySQL scanner and grammar.
 *
 * The MySQL parser uses its own flex scanner (mys_scan.l) and bison
 * grammar (mys_gram.y), independent of the PG standard base_yyparser.
 * This header defines the scanner/grammar interface types.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_gramparse.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_GRAMPARSE_H
#define MYS_GRAMPARSE_H

#include "nodes/parsenodes.h"
#include "parser/mysql/mys_scanner.h"

/* MUST INCLUDE AFTER mys_scanner.h */
#include "mys_gram.h"

typedef struct mys_yy_extra_type
{
	/*
	 * Fields used by the core scanner.
	 */
	core_yy_extra_type core_yy_extra;

	/*
	 * State variables for mys_yylex().
	 */
	bool		have_lookahead; /* is lookahead info valid? */
	int			lookahead_token;	/* one-token lookahead */
	core_YYSTYPE lookahead_yylval;	/* yylval for lookahead token */
	YYLTYPE		lookahead_yylloc;	/* yylloc for lookahead token */
	char	   *lookahead_end;	/* end of current token */
	char		lookahead_hold_char;	/* to be put back at *lookahead_end */

	/*
	 * State variables that belong to the grammar.
	 */
	List	   *parsetree;		/* final parse result is delivered here */
} mys_yy_extra_type;

extern int	mys_yylex(YYSTYPE *lvalp, YYLTYPE *llocp,
					   core_yyscan_t yyscanner);

#define mys_yyget_extra(yyscanner) (*((mys_yy_extra_type **) (yyscanner)))

extern void mys_parser_init(mys_yy_extra_type *yyext);
extern int	mys_yyparse(core_yyscan_t yyscanner);

#endif							/* MYS_GRAMPARSE_H */
