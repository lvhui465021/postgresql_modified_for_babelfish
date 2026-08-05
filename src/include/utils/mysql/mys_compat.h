/*-------------------------------------------------------------------------
 *
 * mys_compat.h
 *    MySQL ADT compatibility: compatibility function declarations.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_compat.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_COMPAT_H
#define MYS_COMPAT_H

#include "fmgr.h"

Datum mys_repeat(PG_FUNCTION_ARGS);

#endif							/* MYS_COMPAT_H */
