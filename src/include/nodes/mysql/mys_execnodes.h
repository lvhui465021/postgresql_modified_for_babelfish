/*-------------------------------------------------------------------------
 *
 * mys_execnodes.h
 *	  definitions for executor state nodes for MySQL mode
 *
 *
 * IDENTIFICATION
 *	  src/include/nodes/mysql/mys_execnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_EXECNODES_H
#define MYS_EXECNODES_H

#include "nodes/execnodes.h"

/*
 * PG16 execnodes.h already defines MERGE_INSERT(0x01), MERGE_UPDATE(0x02),
 * MERGE_DELETE(0x04), and MergeActionState. MySQL-specific additions
 * can be placed here when needed.
 */

#endif							/* MYS_EXECNODES_H */
