/*-------------------------------------------------------------------------
 *
 * mys_parse_utilcmd.h
 *    MySQL parser support declarations.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_parse_utilcmd.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_PARSE_UTILCMD_H
#define MYS_PARSE_UTILCMD_H

#include "parser/parse_node.h"

struct AttrMap;					/* avoid including attmap.h here */

/* PG18 MySQL shim: first explicit schema on the active search_path. */
extern Oid getCurrentNamespaceOid(void);

extern List *mys_expandTableLikeClause(RangeVar *heapRel, 
                                       TableLikeClause *table_like_clause);
extern List *mys_transformCreateStmt(CreateStmt *stmt, const char *queryString);
extern AlterTableStmt *mys_transformAlterTableStmt(Oid relid, 
                                                   AlterTableStmt *stmt,
                                                   const char *queryString,
                                                   List **beforeStmts, 
                                                   List **afterStmts);
extern CreateSeqStmt *createSeq(char *namespace, char *relName, 
                                List *seqOptions, Oid seqTypeId, Oid ownerId);
extern AlterSeqStmt *createAlterSeq(char *namespaceName, char *relName, 
                                    char *colName);
extern AlterTableCmd *setDefaultValWithSeq(char *namespaceName, char *seqName, 
                                           char *colName);
extern CreateFunctionStmt *createTriggerFunc(char *namespace, 
                                             char *funcName, 
                                             char *funcBody);
extern CreateTrigStmt *createTrigger(char *namespace, char *relName, 
                                     char *trigFuncName, char *trigName, 
                                     int16 trigTiming, int16 trigEvents);
extern CreateFunctionStmt *createAutoIncrementTriggerFunc(char *namespace, 
                                                          char *relName, 
                                                          char *colName);
extern CreateTrigStmt *createAutoIncrementTrigger(char *namespace, 
                                                  char *relName);
extern CreateFunctionStmt *createAutoUpdateTimeStampTriggerFunc(char *namespace, 
                                                                char *relName, 
                                                                char *colName);
extern CreateTrigStmt *createAutoUpdateTimeStampTrigger(char *namespace, 
                                                        char *relName, 
                                                        char *colName);
extern AlterObjectDependsStmt *bindTriggerFunctionToTrigger(List *funcName, 
                                                            RangeVar *relation, 
                                                            char *trigName);
extern AlterObjectDependsStmt *bindTriggerToSeq(RangeVar *relation, 
                                                char *trigName, 
                                                char *nameSpace, 
                                                char *seqName);
extern AlterObjectDependsStmt *bindTriggerToColumn(RangeVar *relation, 
                                                   char *trigName, 
                                                   char *colName);
extern bool existAutoUpdateTrigOnThisAtt(Relation relation,
                                         FormData_pg_attribute *att);

/* ParserRoutine callbacks (lowered MySQL SELECT INTO / ON CONFLICT). */
extern Query *mys_transformOptionalSelectInto(ParseState *pstate, Node *parseTree);
extern void mys_transformOnConflictArbiter(ParseState *pstate,
										   OnConflictClause *onConflictClause,
										   List **arbiterExpr,
										   Node **arbiterWhere,
										   Oid *constraint);

#endif							/* MYS_PARSE_UTILCMD_H */
