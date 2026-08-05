/*-------------------------------------------------------------------------
 *
 * createas.h
 *	  prototypes for createas.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/createas.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATEAS_H
#define CREATEAS_H

#include "catalog/objectaddress.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "tcop/dest.h"
#include "utils/queryenvironment.h"


extern ObjectAddress ExecCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
									   ParamListInfo params, QueryEnvironment *queryEnv,
									   QueryCompletion *qc);

extern int	GetIntoRelEFlags(IntoClause *intoClause);

extern DestReceiver *CreateIntoRelDestReceiver(IntoClause *intoClause);

extern bool CreateTableAsRelExists(CreateTableAsStmt *ctas);

/*
 * ExecCreateTableAs_post_hook -- optional callback invoked after the target
 * relation of CREATE TABLE AS / SELECT INTO has been created, before the
 * executor cleanup.  MySQL compatibility registers a hook here to add
 * ON UPDATE triggers inherited from source columns.
 */
typedef void (*ExecCreateTableAs_post_hook_type) (ParseState *pstate,
												  struct Query *query,
												  Oid target_relid);
extern PGDLLEXPORT ExecCreateTableAs_post_hook_type ExecCreateTableAs_post_hook;

#endif							/* CREATEAS_H */
