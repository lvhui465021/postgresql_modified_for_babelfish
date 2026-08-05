/*-------------------------------------------------------------------------
 *
 * mys_execMain.c
 *	  top level executor interface routines for MySQL mode
 *
 * Ported from openHalo_MySQL (PG16) and adapted for PG18.
 * PG18 changes:
 *   - execute_once parameter removed from ExecutorRun
 *   - Delegates to standard_ExecutorStart / standard_ExecutorRun
 *     (InitPlan / ExecutePlan are static in PG18)
 *
 * IDENTIFICATION
 *	  src/backend/executor/mys_execMain.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/mys_executor.h"

/*
 * mys_ExecutorStart
 *		MySQL-mode wrapper for ExecutorStart.
 *		Currently delegates to the standard implementation.
 */
void
mys_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	standard_ExecutorStart(queryDesc, eflags);
}

/*
 * mys_ExecutorRun
 *		MySQL-mode wrapper for ExecutorRun.
 *		PG18: execute_once parameter removed.
 *		Currently delegates to the standard implementation.
 */
void
mys_ExecutorRun(QueryDesc *queryDesc,
				ScanDirection direction, uint64 count)
{
	standard_ExecutorRun(queryDesc, direction, count);
}
