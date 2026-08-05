/*-------------------------------------------------------------------------
 *
 * time_func.c
 *	  Extend time_func routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/time_func.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/mysql/mys_date.h"
#include "utils/timestamp.h"
#include "varatt.h"


PG_FUNCTION_INFO_V1(subtime);

/*
 * SUBTIME()
 * returns expr1 − expr2 expressed as a value in the same
 * format as expr1. expr1 is a time or datetime expression, and expr2 is a
 * time expression.
 */
Datum
subtime(PG_FUNCTION_ARGS)
{
    text *result = NULL;
    text *text0 = PG_GETARG_TEXT_PP(0);
    char *str0 = VARDATA_ANY(text0);
    int str0Len = VARSIZE_ANY_EXHDR(text0);
    char oriCha0 = str0[str0Len];
    struct pg_tm tt0;
    struct pg_tm *tm0 = &tt0;
    fsec_t fsec0 = 0;
    int sign0 = 1;
    text *text1 = PG_GETARG_TEXT_PP(1);
    char *str1 = VARDATA_ANY(text1);
    int str1Len = VARSIZE_ANY_EXHDR(text1);
    char oriCha1 = str1[str1Len];
    struct pg_tm tt1;
    struct pg_tm *tm1 = &tt1;
    fsec_t fsec1 = 0;
    int sign1 = 1;

    tm0->tm_year = 0;
    tm1->tm_year = 0;
    str0[str0Len] = '\0';
    str1[str1Len] = '\0';
    mys_pre_time_in_for_subtime(str0, tm0, &fsec0, &sign0);
    mys_pre_time_in_for_subtime(str1, tm1, &fsec1, &sign1);
    str0[str0Len] = oriCha0;
    str1[str1Len] = oriCha1;

    if (tm1->tm_year > 0)
    {
        PG_RETURN_NULL();
    }

    if (tm0->tm_year > 0)
    {
        // timestamp - time
        Timestamp tempResult;
        Timestamp ts0;
        TimeADT t1;
        tm2timestamp(tm0, fsec0, NULL, &ts0);
        tm2time(tm1, fsec1, &t1);
        tempResult = sign0 * ts0 - sign1 * t1;
        result = cstring_to_text(DatumGetCString(DirectFunctionCall1(timestamp_out, TimestampGetDatum(tempResult))));
    }
    else
    {
        // time - time
        char *resultStr;
        TimeADT tempResult;
        struct pg_tm tempTm;
        fsec_t tempFsec;
        TimeADT t0;
        TimeADT t1;
        tm2time(tm0, fsec0, &t0);
        tm2time(tm1, fsec1, &t1);
        tempResult = sign0 * t0 - sign1 * t1;
        resultStr = DatumGetCString(DirectFunctionCall1(time_out, TimeADTGetDatum(tempResult)));

        time2tm(tempResult, &tempTm, &tempFsec);
        if (tempTm.tm_hour > MYS_MAX_TIME_HOUR || tempTm.tm_hour < MYS_MAX_TIME_HOUR * -1)
        {
            elog(ERROR, "Truncated incorrect time value: \'%s\'", resultStr);
        }

        result = cstring_to_text(resultStr);
    }

    PG_RETURN_TEXT_P(result);
}


PG_FUNCTION_INFO_V1(addtime);

/*
 * ADDTIME()
 * adds expr2 to expr1 and returns the result.
 * expr1 is a time or datetime expression, and expr2 is a time expression.
 */
Datum
addtime(PG_FUNCTION_ARGS)
{
    text *result = NULL;
    text *text0 = PG_GETARG_TEXT_PP(0);
    char *str0 = VARDATA_ANY(text0);
    int str0Len = VARSIZE_ANY_EXHDR(text0);
    char oriCha0 = str0[str0Len];
    struct pg_tm tt0;
    struct pg_tm *tm0 = &tt0;
    fsec_t fsec0 = 0;
    int sign0 = 1;
    text *text1 = PG_GETARG_TEXT_PP(1);
    char *str1 = VARDATA_ANY(text1);
    int str1Len = VARSIZE_ANY_EXHDR(text1);
    char oriCha1 = str1[str1Len];
    struct pg_tm tt1;
    struct pg_tm *tm1 = &tt1;
    fsec_t fsec1 = 0;
    int sign1 = 1;

    tm0->tm_year = 0;
    tm1->tm_year = 0;
    str0[str0Len] = '\0';
    str1[str1Len] = '\0';
    mys_pre_time_in_for_subtime(str0, tm0, &fsec0, &sign0);
    mys_pre_time_in_for_subtime(str1, tm1, &fsec1, &sign1);
    str0[str0Len] = oriCha0;
    str1[str1Len] = oriCha1;

    if (tm1->tm_year > 0)
    {
        PG_RETURN_NULL();
    }

    if (tm0->tm_year > 0)
    {
        // timestamp + time
        Timestamp tempResult;
        Timestamp ts0;
        TimeADT t1;
        tm2timestamp(tm0, fsec0, NULL, &ts0);
        tm2time(tm1, fsec1, &t1);
        tempResult = sign0 * ts0 + sign1 * t1;
        result = cstring_to_text(DatumGetCString(DirectFunctionCall1(timestamp_out, TimestampGetDatum(tempResult))));
    }
    else
    {
        // time + time
        char *resultStr;
        TimeADT tempResult;
        struct pg_tm tempTm;
        fsec_t tempFsec;
        TimeADT t0;
        TimeADT t1;
        tm2time(tm0, fsec0, &t0);
        tm2time(tm1, fsec1, &t1);
        tempResult = sign0 * t0 + sign1 * t1;
        resultStr = DatumGetCString(DirectFunctionCall1(time_out, TimeADTGetDatum(tempResult)));

        time2tm(tempResult, &tempTm, &tempFsec);
        if (tempTm.tm_hour > MYS_MAX_TIME_HOUR || tempTm.tm_hour < MYS_MAX_TIME_HOUR * -1)
        {
            elog(ERROR, "Truncated incorrect time value: \'%s\'", resultStr);
        }

        result = cstring_to_text(resultStr);
    }

    PG_RETURN_TEXT_P(result);
}


PG_FUNCTION_INFO_V1(timediff);

/*
 * TIMEDIFF()
 * returns expr1 − expr2 expressed as a time value. 
 * expr1 and expr2 are time or date-and-time expressions, but both must be of the same type.
 */
Datum
timediff(PG_FUNCTION_ARGS)
{
    TimeADT result = 0;
    char *resultStr = NULL;
    struct pg_tm tempTm;
    fsec_t tempFsec;
    text *text0 = PG_GETARG_TEXT_PP(0);
    char *str0 = VARDATA_ANY(text0);
    int str0Len = VARSIZE_ANY_EXHDR(text0);
    char oriCha0 = str0[str0Len];
    struct pg_tm tt0;
    struct pg_tm *tm0 = &tt0;
    fsec_t fsec0 = 0;
    int sign0 = 1;
    text *text1 = PG_GETARG_TEXT_PP(1);
    char *str1 = VARDATA_ANY(text1);
    int str1Len = VARSIZE_ANY_EXHDR(text1);
    char oriCha1 = str1[str1Len];
    struct pg_tm tt1;
    struct pg_tm *tm1 = &tt1;
    fsec_t fsec1 = 0;
    int sign1 = 1;

    tm0->tm_year = 0;
    tm1->tm_year = 0;
    str0[str0Len] = '\0';
    str1[str1Len] = '\0';
    mys_pre_time_in_for_subtime(str0, tm0, &fsec0, &sign0);
    mys_pre_time_in_for_subtime(str1, tm1, &fsec1, &sign1);
    str0[str0Len] = oriCha0;
    str1[str1Len] = oriCha1;

    if ((tm0->tm_year > 0 && tm1->tm_year == 0) ||
        (tm0->tm_year == 0 && tm1->tm_year > 0))
    {
        PG_RETURN_NULL();
    }

    if (tm0->tm_year > 0)
    {
        // timestamp - timestamp
        Timestamp ts0;
        Timestamp ts1;
        tm2timestamp(tm0, fsec0, NULL, &ts0);
        tm2timestamp(tm1, fsec1, NULL, &ts1);
        result = sign0 * ts0 - sign1 * ts1;
    }
    else
    {
        // time - time
        TimeADT t0;
        TimeADT t1;
        tm2time(tm0, fsec0, &t0);
        tm2time(tm1, fsec1, &t1);
        result = sign0 * t0 - sign1 * t1;
    }

    resultStr = DatumGetCString(DirectFunctionCall1(time_out, TimeADTGetDatum(result)));

    time2tm(result, &tempTm, &tempFsec);
    if (tempTm.tm_hour > MYS_MAX_TIME_HOUR || tempTm.tm_hour < MYS_MAX_TIME_HOUR * -1)
    {
        elog(ERROR, "Truncated incorrect time value: \'%s\'", resultStr);
    }

    PG_RETURN_TIMEADT(result);
}
