/*-------------------------------------------------------------------------
 *
 * mys_json.c
 *    MySQL ADT compatibility: JSON function wrappers.
 *
 * Lives in the mysm shared library so the MySQL type layer ships as a
 * loadable module rather than being compiled into the kernel.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/ddsm/mysm/mys_json.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "utils/mysql/mys_json.h"

PG_FUNCTION_INFO_V1(mys_json_object);
Datum
mys_json_object(PG_FUNCTION_ARGS)
{
    return json_build_object(fcinfo);
}

PG_FUNCTION_INFO_V1(mys_json_object_noargs);
Datum
mys_json_object_noargs(PG_FUNCTION_ARGS)
{
	return json_build_object_noargs(fcinfo);
}

/*
 * PG16 exposed json(int2/int4/int8/float4/float8/numeric) through six
 * type-specific wrappers.  PG18 removed those C entry points, but keeps the
 * generic to_json(anyelement) implementation.  The catalog compatibility
 * overloads retain the old SQL surface and delegate serialization to that
 * native PG18 path.
 */
PG_FUNCTION_INFO_V1(mys_to_json);
Datum
mys_to_json(PG_FUNCTION_ARGS)
{
	return to_json(fcinfo);
}
