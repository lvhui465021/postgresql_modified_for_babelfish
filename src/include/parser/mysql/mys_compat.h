/*-------------------------------------------------------------------------
 *
 * mys_compat.h
 *    Compatibility declarations for symbols that the openHalo analyze code
 *    expects from openHalo headers that are not (yet) present in PG18.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_compat.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_COMPAT_H
#define MYS_COMPAT_H

#include "nodes/pg_list.h"

/* GUC: enable MySQL-style multi-table UPDATE.  Set false for M3 baseline. */
extern bool halo_mysql_support_multiple_table_update;

#endif /* MYS_COMPAT_H */
