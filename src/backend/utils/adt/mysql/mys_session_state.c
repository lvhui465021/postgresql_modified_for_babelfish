/*-------------------------------------------------------------------------
 *
 * mys_session_state.c
 *    SQL-callable MySQL session-state helpers (registered in pg_proc.dat).
 *
 * These functions must live in the kernel because fmgr.c references them
 * at link time; the per-connection packet state they report lives in the
 * loadable aux_mysql module, reached via the G3 table
 * mys_session_state_exports (see mys_session_state_exports.h).
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/mysql/mys_session_state.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "adapter/mysql/mys_session_state_exports.h"
#include "fmgr.h"
#include "utils/builtins.h"

/* G3 slot, filled by the aux_mysql module's _PG_init(). */
MysSessionStateExports *mys_session_state_exports = NULL;

PG_FUNCTION_INFO_V1(mys_found_rows);

Datum
mys_found_rows(PG_FUNCTION_ARGS)
{
	uint64		rows = 0;

	if (mys_session_state_exports != NULL &&
		mys_session_state_exports->found_rows != NULL)
		rows = mys_session_state_exports->found_rows();
	PG_RETURN_INT64((int64) rows);
}

PG_FUNCTION_INFO_V1(mys_last_insert_id);

Datum
mys_last_insert_id(PG_FUNCTION_ARGS)
{
	uint64		value = 0;

	if (mys_session_state_exports != NULL &&
		mys_session_state_exports->last_insert_id != NULL)
		value = mys_session_state_exports->last_insert_id();
	PG_RETURN_INT64((int64) value);
}

PG_FUNCTION_INFO_V1(mys_row_count);

Datum
mys_row_count(PG_FUNCTION_ARGS)
{
	uint64		count = 0;

	if (mys_session_state_exports != NULL &&
		mys_session_state_exports->row_count != NULL)
		count = mys_session_state_exports->row_count();
	PG_RETURN_INT64((int64) count);
}
