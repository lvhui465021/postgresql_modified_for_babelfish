/*-------------------------------------------------------------------------
 *
 * mys_varlena.h
 *    MySQL ADT compatibility: identifier splitting declarations.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_varlena.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_VARLENA_H
#define MYS_VARLENA_H

#include "nodes/pg_list.h"

bool mys_SplitIdentifierString(char *rawstring, char separator, List **namelist);
Datum mys_hex(PG_FUNCTION_ARGS);

#endif							/* MYS_VARLENA_H */

