/*-------------------------------------------------------------------------
 *
 * mys_executor.c
 *		Mysql Executor Engine
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/mys_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/mys_executor.h"
#include "executor/mys_execPartition.h"
#include "nodes/nodes.h"

/* 
 * define mysql executor engine
 */
static PartitionRoutine mys_partition_engine = {
    .ExecFindPartition = mys_ExecFindPartition
};

/* PG18: T_ExecutorRoutine not defined; using T_Invalid */
static const ExecutorRoutine mys_executor_engine = {
    .type = T_Invalid,

    .ExecutorStart = mys_ExecutorStart,
    .ExecutorRun = mys_ExecutorRun,
    .ExecInitNode = mys_ExecInitNode,
    .ExecEndNode = mys_ExecEndNode,
    .ExecReScan = mys_ExecReScan,

    .partition = &mys_partition_engine
};

/*
 * GetMysExecutorEngine
 *      get mysql executor engine
 */
const ExecutorRoutine *
GetMysExecutorEngine(void)
{
    return &mys_executor_engine;
}