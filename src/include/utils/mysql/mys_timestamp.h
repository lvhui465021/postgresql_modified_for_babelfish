/*-------------------------------------------------------------------------
 *
 * mys_timestamp.h
 *    MySQL ADT compatibility: timestamp parsing declarations.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_timestamp.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_TIMESTAMP_H
#define MYS_TIMESTAMP_H

Datum mys_timestamp_in(PG_FUNCTION_ARGS);
bool mys_decodeStringDatetime(char *str, struct pg_tm *tm, fsec_t *fsec,
                              int *tzp, int flags, bool *haveTz);
bool mys_DecodeStringDatetimeForCompare(char *str, struct pg_tm *tm, fsec_t *fsec, int flags);
bool mys_strToDatetimeInternal(char *str, int flags, Timestamp *result);
Timestamp mys_numberToDatetimeInternal(int64 number, int flags);
TimeADT mys_numberToTimeInternal(int64 number);
bool mysCheckDate(const struct pg_tm *tm, bool not_zero_date, int flags);

#endif							/* MYS_TIMESTAMP_H */

