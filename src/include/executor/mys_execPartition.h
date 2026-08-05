
/*-------------------------------------------------------------------------
 *
 * ora_execPartition.h
 *		Support routines for partitioning in Oracle mode
 *
 *

 *
 * IDENTIFICATION
 *	  src/include/executor/ora_execPartition.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_EXECPARTITION_H
#define MYS_EXECPARTITION_H

#include "executor/execPartition.h"
#include "nodes/execnodes.h"

extern ResultRelInfo *mys_ExecFindPartition(ModifyTableState *mtstate,
										    ResultRelInfo *rootResultRelInfo,
										    PartitionTupleRouting *proute,
										    TupleTableSlot *slot,
										    EState *estate);

#endif                              /* ORA_EXECPARTITION_H */