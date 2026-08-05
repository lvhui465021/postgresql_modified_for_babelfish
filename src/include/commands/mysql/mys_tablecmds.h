/*-------------------------------------------------------------------------
 *
 * mys_tablecmds.h
 *	  MySQL table commands routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/include/commands/mysql/mys_tablecmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_TABLECMDS_H
#define MYS_TABLECMDS_H

#include "nodes/parsenodes.h"
#include "storage/lock.h"

struct AlterTableUtilityContext;	/* avoid including tcop/utility.h here */

LOCKMODE mysAlterTableGetLockLevel(List *cmds);
void mysAlterTable(AlterTableStmt *stmt, LOCKMODE lockmode,
		           struct AlterTableUtilityContext *context);
void mysPreProcessDropStmt(DropStmt *dropStmt);
List *mysPreProcessRenameStmt(RenameStmt *renameStmt);
List *mysPreProcessCreateViewStmt(ViewStmt *stmt);
void mysProcessAutoIncForRenameAtt(Relation targetRel, char *oldColName, char *newColName, List **stmts);
void mysProcessSetEnumForRenameAtt(Relation targetRel, char *oldColName, char *newColName, List **stmts);
Oid mysGetColumnOnUpdateNowTrig(Relation rel, char *colName);
char *mysCheckIndexName(Oid relOid, char *oriIndexName);
char *mysBuildInternalIndexName(Oid relOid, char *oriIndexName);
char *mysBuildCheckNameForSet(void);
char *mysBuildSeqName(char *tableName);
char *mysBuildTrigNameForAutoInc(char *tableName);
char *mysBuildTrigFuncNameForAutoInc(char *tableName);
char *mysBuildTrigNameForAutoIncJump(char *seqName);
char *mysBuildTrigFuncNameForAutoIncJump(char *seqName);
char *mysBuildTrigNameForOnUpdateNow(char *tableName, char *colName);
char *mysBuildTrigFuncNameForOnUpdateNow(char *tableName, char *colName);
char *mysBuildSetDomainName(char *tableName, char *colName);
char *mysBuildEnumDomainName(char *tableName, char *colName);
Oid getColumnDefaultSeq(Relation rel, const char *colName);
char *mysBuildPartitionTableName(char *tableName, char *pTableName);
void mysSetColumnDefaultKind(Oid relid, AttrNumber attnum, char kind);

/* Register the CTAS post-hook used for MySQL ON UPDATE trigger inheritance. */
extern void InitMysCtasHook(void);

#endif                          /* MYS_TABLECMDS_H */
