/*-------------------------------------------------------------------------
 *
 * mys_scanner.h
 *    Declarations for the MySQL-compatibility scanner (mys_scan.l).
 *
 * The MySQL scanner uses the same core_yy_extra_type as the PG standard
 * scanner, layered under mys_yy_extra_type, but has its own flex prefix
 * (mys_core_yy) and keyword list (MysScanKeywords).
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_scanner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_SCANNER_H
#define MYS_SCANNER_H

#include "parser/scanner.h"
#include "parser/mysql/mys_keywords.h"

extern PGDLLIMPORT const uint16 MysScanKeywordTokens[];

/* Entry points in parser/mysql/mys_scan.l */
extern core_yyscan_t mys_scanner_init(const char *str,
								  core_yy_extra_type *yyext,
								  const ScanKeywordList *keywordlist,
								  const uint16 *keyword_tokens);
extern void mys_scanner_finish(core_yyscan_t yyscanner);
extern int	mys_core_yylex(core_YYSTYPE *lvalp, YYLTYPE *llocp,
					   core_yyscan_t yyscanner);
extern int	mys_scanner_errposition(int location, core_yyscan_t yyscanner);
extern void mys_setup_scanner_errposition_callback(ScannerCallbackState *scbstate,
											   core_yyscan_t yyscanner,
											   int location);
extern void mys_cancel_scanner_errposition_callback(ScannerCallbackState *scbstate);
extern void mys_scanner_yyerror(const char *message, core_yyscan_t yyscanner);
extern char *mys_make_pl_block_str(core_yyscan_t yyscanner, int start);

#endif							/* MYS_SCANNER_H */
