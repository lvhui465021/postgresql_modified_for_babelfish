/*-------------------------------------------------------------------------
 *
 * mys_executor.h
 *		HEADER for oracle executor engine
 *
 *
 * src/include/executor/ora_executor.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_EXECUTOR_H
#define MYS_EXECUTOR_H

#include "executor/executor_api.h"
#include "nodes/parsenodes.h"

/* Mysql executor engine */
extern const ExecutorRoutine *GetMysExecutorEngine(void);

/* in mys_execMain.c */
extern void mys_ExecutorStart(QueryDesc *queryDesc, int eflags);
extern void ExecWithCheckOptions2(WCOKind kind, ResultRelInfo *resultRelInfo,
								  TupleTableSlot *slot, EState *estate);

/* in mys_execProcnode.c */
extern PlanState *mys_ExecInitNode(Plan *node, EState *estate, int eflags);
extern void mys_ExecEndNode(PlanState *node);
extern void mys_ExecReScan(PlanState *node);

/* PG18: execute_once removed */
extern void mys_ExecutorRun(QueryDesc *queryDesc,
					 ScanDirection direction, uint64 count);

#endif							/* MYS_EXECUTOR_H */