/*-------------------------------------------------------------------------
 *
 * mys_session_state_exports.h
 *    G3 contract: kernel SQL helpers -> aux_mysql packet state.
 *
 * mys_found_rows / mys_last_insert_id / mys_row_count are SQL functions
 * registered in pg_proc.dat and therefore must stay in the kernel
 * (fmgr.c references them at link time).  Their data lives in the
 * aux_mysql module's per-connection packet state, so the kernel calls
 * through this table, filled in by aux_mysql's _PG_init().
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/adapter/mysql/mys_session_state_exports.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_SESSION_STATE_EXPORTS_H
#define MYS_SESSION_STATE_EXPORTS_H

#include "c.h"

typedef struct MysSessionStateExports
{
	uint64		(*found_rows) (void);
	uint64		(*last_insert_id) (void);
	uint64		(*row_count) (void);
} MysSessionStateExports;

/* Defined in the kernel (utils/adt/mysql/mys_session_state.c). */
extern PGDLLIMPORT MysSessionStateExports *mys_session_state_exports;

/* Filled by the aux_mysql module (mysql_protocol.c); published above. */
extern MysSessionStateExports mys_session_state_exports_data;

#endif							/* MYS_SESSION_STATE_EXPORTS_H */
