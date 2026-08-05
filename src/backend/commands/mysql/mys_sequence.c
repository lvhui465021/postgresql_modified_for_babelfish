/*-------------------------------------------------------------------------
 *
 * mys_sequence.c
 *    MySQL AUTO_INCREMENT sequence bridge.
 *
 * AUTO_INCREMENT is represented by PostgreSQL sequences.  The MySQL layer
 * only needs a stable kernel entry point that can be called both by the DDL
 * implementation and by the mysm shared library; sequence locking, ACLs,
 * WAL, and transaction checks remain in PostgreSQL's setval implementation.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/commands/mysql/mys_sequence.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/mysql/mys_sequence.h"
#include "commands/sequence.h"
#include "utils/builtins.h"

int64
mys_setval3_oid(Oid seqOid, int64 next, bool isCalled)
{
	Datum result;

	result = DirectFunctionCall3(setval3_oid,
								ObjectIdGetDatum(seqOid),
								Int64GetDatum(next),
								BoolGetDatum(isCalled));

	return DatumGetInt64(result);
}
