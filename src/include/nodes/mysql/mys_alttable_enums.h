/*-------------------------------------------------------------------------
 *
 * mys_alttable_enums.h
 *    MySQL-specific AlterTableType and ConstraintType enum extensions.
 *
 * PG18's AlterTableType enum includes the MySQL-specific values used by this
 * parser.  Keep this header for MySQL DDL declarations, but do not remap
 * those values: collapsing MODIFY/CHANGE into AT_AddColumn loses the
 * operation's lifecycle semantics before utility execution.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/nodes/mysql/mys_alttable_enums.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_ALTTABLE_ENUMS_H
#define MYS_ALTTABLE_ENUMS_H

#include "nodes/parsenodes.h"

/*
 * CONSTR_AUTOINC and CONSTR_KEY are native ConstrType values in PG18's
 * parsenodes.h.  Do not alias them to CONSTR_DEFAULT: doing so erases the
 * MySQL grammar's AUTO_INCREMENT marker before utility transformation.
 */

/* MySQL ignore statement flag */
extern bool isIgnoreStmt;

#endif /* MYS_ALTTABLE_ENUMS_H */
