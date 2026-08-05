/*-------------------------------------------------------------------------
 *
 * mys_parser_exports.h
 *    G3 cross-boundary contract: kernel -> mysql_parser module.
 *
 * The MySQL parser ships as a loadable shared module (mysql_parser.so).
 * The kernel cannot reference its symbols at link time (a program cannot
 * resolve an undefined symbol against a module loaded later via
 * shared_preload_libraries), so every kernel call into the module goes
 * through this table.  The slot variable (mys_parser_exports) is defined
 * in the kernel and NULL until the module's _PG_init() fills it in,
 * keeping the dependency direction module -> kernel.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_parser_exports.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSER_EXPORTS_H
#define MYS_PARSER_EXPORTS_H

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "common/kwlookup.h"

typedef struct MysParserExports
{
	/* DDL transformation helpers (mys_parse_utilcmd.c). */
	List *(*mys_transformCreateStmt)(CreateStmt *stmt, const char *queryString);
	AlterTableStmt *(*mys_transformAlterTableStmt)(Oid relid,
												   AlterTableStmt *stmt,
												   const char *queryString,
												   List **beforeStmts,
												   List **afterStmts);
	List *(*mys_expandTableLikeClause)(RangeVar *heapRel,
									   TableLikeClause *table_like_clause);

	/* Namespace helper (mys_namespace.c). */
	Oid (*getCurrentNamespaceOid)(void);

	/* AUTO_INCREMENT / ON UPDATE CURRENT_TIMESTAMP builders. */
	CreateFunctionStmt *(*createAutoIncrementTriggerFunc)(char *namespaceName,
														  char *relName,
														  char *colName);
	CreateFunctionStmt *(*createAutoUpdateTimeStampTriggerFunc)(char *namespace,
																char *relName,
																char *colName);
	CreateTrigStmt *(*createAutoUpdateTimeStampTrigger)(char *namespace,
														char *relName,
														char *colName);

	/* MySQL keyword list (mys_keywords.c). */
	const ScanKeywordList *MysScanKeywords;
	const uint8 *MysScanKeywordCategories;
} MysParserExports;

/* Defined in the kernel (parsereng.c), filled by the mysql_parser module. */
extern PGDLLIMPORT MysParserExports *mys_parser_exports;

#endif							/* MYS_PARSER_EXPORTS_H */
