/*-------------------------------------------------------------------------
 *
 * mys_json.h
 *    MySQL ADT compatibility: JSON function declarations.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_json.h
 *
 *-------------------------------------------------------------------------
 */

 #ifndef MYS_JSON_H
 #define MYS_JSON_H
 
 #include "fmgr.h"
 
extern Datum mys_json_object(PG_FUNCTION_ARGS);
extern Datum mys_json_object_noargs(PG_FUNCTION_ARGS);
extern Datum mys_to_json(PG_FUNCTION_ARGS);
 #endif							/* MYS_JSON_H */
