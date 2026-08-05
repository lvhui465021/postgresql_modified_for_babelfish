/*-------------------------------------------------------------------------
 *
 * mys_date.h
 *    MySQL ADT compatibility: date/time parsing declarations.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_date.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_DATE_H
#define MYS_DATE_H

#include "fmgr.h"
#include "utils/date.h"

// year, month, day, hour, minute, second, microsecond, timezone
#define MYS_MAX_TIMESTAMP_PARTS 8
#define MYS_MAX_TIME_ZONE_HOURS 14
#define MYS_MAX_TIME_HOUR 838

#define MYS_TIME_FUZZY_DATE 1
#define MYS_TIME_DATETIME_ONLY 2
#define MYS_TIME_NO_NSEC_ROUNDING 4
#define MYS_TIME_NO_DATE_FRAC_WARN 8
#define MYS_TIME_NO_ZERO_IN_DATE 16
#define MYS_TIME_NO_ZERO_DATE 32
#define MYS_TIME_INVALID_DATES 64

Datum mys_date_in(PG_FUNCTION_ARGS);
TimeADT mys_pre_time_in(PG_FUNCTION_ARGS);
char *mys_post_time_out(PG_FUNCTION_ARGS);
void mys_pre_time_in_for_subtime(char *str, struct pg_tm *tm, fsec_t *fsec, int *sign);

#endif							/* MYS_DATE_H */

