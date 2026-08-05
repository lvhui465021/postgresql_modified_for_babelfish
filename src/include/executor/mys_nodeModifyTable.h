/*-------------------------------------------------------------------------
 *
 * ora_nodeModifyTable.c
 *	  Support routines to handle ModifyTable nodes in Oracle mode
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/include/executor/ora_nodeModifyTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_NODEMODIFYTABLE_H
#define MYS_NODEMODIFYTABLE_H

#include "nodes/execnodes.h"

extern ModifyTableState *mys_ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags);
extern void mys_ExecEndModifyTable(ModifyTableState *node);
extern void mys_ExecReScanModifyTable(ModifyTableState *node);
extern void ExecInitMergeTupleSlots(ModifyTableState *mtstate, ResultRelInfo *resultRelInfo);

#endif							/* ORA_NODEMODIFYTABLE_H */