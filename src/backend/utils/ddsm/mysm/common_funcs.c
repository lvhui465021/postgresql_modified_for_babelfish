
/*-------------------------------------------------------------------------
 *
 * common_funcs.c
 *	  MySQL sequence process
 *
 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/common_funcs.c
 *
 *-------------------------------------------------------------------------
 */


#include "mysm_compat.h"

#include "commands/sequence.h"
#include "utils/mysql_kernel_export.h"


PG_FUNCTION_INFO_V1(mysSetval3Oid);
Datum
mysSetval3Oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);
	bool		iscalled = PG_GETARG_BOOL(2);

	PG_RETURN_INT64(mys_setval3_oid(relid, next, iscalled));
}
