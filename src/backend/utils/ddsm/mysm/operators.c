/*-------------------------------------------------------------------------
 *
 * operators.c
 *	  Extend operators routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/operators.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include "fmgr.h"
#include "adapter/mysql/systemVar.h"
#include "datatype/timestamp.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"
#include "utils/mysql/mys_date.h"
#include "utils/mysql/mys_timestamp.h"

PG_FUNCTION_INFO_V1(boolCondAndboolCImp);
Datum
boolCondAndboolCImp(PG_FUNCTION_ARGS)
{
    bool arg1 = PG_GETARG_BOOL(0);
    bool arg2 = PG_GETARG_BOOL(1);

    if (PG_ARGISNULL(0))
    {
        if (PG_ARGISNULL(1))
        {
            PG_RETURN_NULL();
        }
        else
        {
            if (arg2 == false)
            {
                PG_RETURN_BOOL(false);
            }
            else
            {
                PG_RETURN_NULL();
            }
        }
    }
    else if (PG_ARGISNULL(1))
    {
        if (PG_ARGISNULL(0))
        {
            PG_RETURN_NULL();
        }
        else
        {
            if (arg1 == false)
            {
                PG_RETURN_BOOL(false);
            }
            else
            {
                PG_RETURN_NULL();
            }
        }
    }
    else
    {
        PG_RETURN_BOOL(arg1 && arg2);
    }
}


PG_FUNCTION_INFO_V1(boolOrBool);
Datum
boolOrBool(PG_FUNCTION_ARGS)
{
    bool arg1 = PG_GETARG_BOOL(0);
    bool arg2 = PG_GETARG_BOOL(1);

    if (!PG_ARGISNULL(0))
    {
        if (!PG_ARGISNULL(1))
        {
            PG_RETURN_BOOL(arg1 || arg2);
        }
        else
        {
            if (arg1)
            {
                PG_RETURN_BOOL(arg1);
            }
            else 
            {
                PG_RETURN_NULL();
            }
        }
    }
    else 
    {
        if (!PG_ARGISNULL(1))
        {
            if (arg2)
            {
                PG_RETURN_BOOL(arg2);
            }
            else 
            {
                PG_RETURN_NULL();
            }
        }
        else 
        {
            PG_RETURN_NULL();
        }
    }
}


static int
mys_timestampCmpText(Timestamp arg1, text *arg2)
{
    char *argString1 = (char *)DirectFunctionCall1(timestamp_out, TimestampGetDatum(arg1));
    int argStringLen1 = strlen(argString1);
	char* argString2 = text_to_cstring(arg2);
    int argStringLen2 = 0;
    int timeFlags = MYS_TIME_FUZZY_DATE | MYS_TIME_INVALID_DATES;
    struct pg_tm tt;
    struct pg_tm *tm = &tt;
    fsec_t fsec = 0;
    int result;

    if (mys_sqlMode & MYS_MODE_NO_ZERO_IN_DATE)
    {
        timeFlags |= MYS_TIME_NO_ZERO_IN_DATE;
    }

    if (mys_sqlMode & MYS_MODE_NO_ZERO_DATE)
    {
        timeFlags |= MYS_TIME_NO_ZERO_DATE;
    }

    if (mys_DecodeStringDatetimeForCompare(argString2, tm, &fsec, timeFlags))
    {
        char buf[MAXDATELEN + 1];

        EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf);
        argString2 = pstrdup(buf);
    }

    argStringLen2 = strlen(argString2);
    
    result = memcmp(argString1, argString2, Min(argStringLen1, argStringLen2));
    if ((result == 0) && (argStringLen1 != argStringLen2))
    {
        result = (argStringLen1 < argStringLen2) ? -1 : 1;
    }

    return result;
}


static int
mys_textCmpTimestamp(text *arg1, Timestamp arg2)
{
	char *argString1 = text_to_cstring(arg1);
    int argStringLen1 = 0;
    char *argString2 = (char *)DirectFunctionCall1(timestamp_out, TimestampGetDatum(arg2));
    int argStringLen2 = strlen(argString2);
    int timeFlags = MYS_TIME_FUZZY_DATE | MYS_TIME_INVALID_DATES;
    struct pg_tm tt;
    struct pg_tm *tm = &tt;
    fsec_t fsec = 0;
    int result;

    if (mys_sqlMode & MYS_MODE_NO_ZERO_IN_DATE)
    {
        timeFlags |= MYS_TIME_NO_ZERO_IN_DATE;
    }

    if (mys_sqlMode & MYS_MODE_NO_ZERO_DATE)
    {
        timeFlags |= MYS_TIME_NO_ZERO_DATE;
    }

    if (mys_DecodeStringDatetimeForCompare(argString1, tm, &fsec, timeFlags))
    {
        char buf[MAXDATELEN + 1];

        EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf);
        argString1 = pstrdup(buf);
    }

    argStringLen1 = strlen(argString1);
    
    result = memcmp(argString1, argString2, Min(argStringLen1, argStringLen2));
    if ((result == 0) && (argStringLen1 != argStringLen2))
    {
        result = (argStringLen1 < argStringLen2) ? -1 : 1;
    }

    return result;
}


PG_FUNCTION_INFO_V1(mys_timestampGtText);
Datum
mys_timestampGtText(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_timestampCmpText(PG_GETARG_TIMESTAMP(0), PG_GETARG_TEXT_P(1)) > 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_timestampGeText);
Datum
mys_timestampGeText(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_timestampCmpText(PG_GETARG_TIMESTAMP(0), PG_GETARG_TEXT_P(1)) >= 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_timestampLtText);
Datum
mys_timestampLtText(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_timestampCmpText(PG_GETARG_TIMESTAMP(0), PG_GETARG_TEXT_P(1)) < 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_timestampLeText);
Datum
mys_timestampLeText(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_timestampCmpText(PG_GETARG_TIMESTAMP(0), PG_GETARG_TEXT_P(1)) <= 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_timestampEqText);
Datum
mys_timestampEqText(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_timestampCmpText(PG_GETARG_TIMESTAMP(0), PG_GETARG_TEXT_P(1)) == 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_textGtTimestamp);
Datum
mys_textGtTimestamp(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_textCmpTimestamp(PG_GETARG_TEXT_P(0), PG_GETARG_TIMESTAMP(1)) > 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_textGeTimestamp);
Datum
mys_textGeTimestamp(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_textCmpTimestamp(PG_GETARG_TEXT_P(0), PG_GETARG_TIMESTAMP(1)) >= 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_textLtTimestamp);
Datum
mys_textLtTimestamp(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_textCmpTimestamp(PG_GETARG_TEXT_P(0), PG_GETARG_TIMESTAMP(1)) < 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_textLeTimestamp);
Datum
mys_textLeTimestamp(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_textCmpTimestamp(PG_GETARG_TEXT_P(0), PG_GETARG_TIMESTAMP(1)) <= 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_textEqTimestamp);
Datum
mys_textEqTimestamp(PG_FUNCTION_ARGS)
{
    bool result;

    result = (mys_textCmpTimestamp(PG_GETARG_TEXT_P(0), PG_GETARG_TIMESTAMP(1)) == 0);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(mys_textPlInterval);
Datum
mys_textPlInterval(PG_FUNCTION_ARGS)
{
    Timestamp leftArg;
	char *leftArgString = text_to_cstring(PG_GETARG_TEXT_P(0));
    int timeFlags = MYS_TIME_NO_ZERO_DATE;
    Timestamp result;

    if (mys_sqlMode & MYS_MODE_INVALID_DATES)
    {
        timeFlags |= MYS_TIME_INVALID_DATES;
    }

    if (mys_strToDatetimeInternal(leftArgString, timeFlags, &leftArg))
    {
        result = DirectFunctionCall2(timestamp_pl_interval,
                                     TimestampGetDatum(leftArg),
                                     PointerGetDatum(PG_GETARG_INTERVAL_P(1)));
    }
    else
    {
        elog(ERROR, "Incorrect datetime value for mys_textPlInterval: %s", leftArgString);
    }

    PG_RETURN_TIMESTAMP(result);
}


PG_FUNCTION_INFO_V1(mys_intervalPlText);
Datum
mys_intervalPlText(PG_FUNCTION_ARGS)
{
    Timestamp rightArg;
	char *rightArgString = text_to_cstring(PG_GETARG_TEXT_P(1));
    int timeFlags = MYS_TIME_NO_ZERO_DATE;
    Timestamp result;

    if (mys_sqlMode & MYS_MODE_INVALID_DATES)
    {
        timeFlags |= MYS_TIME_INVALID_DATES;
    }

    if (mys_strToDatetimeInternal(rightArgString, timeFlags, &rightArg))
    {
        result = DirectFunctionCall2(timestamp_pl_interval,
                                     TimestampGetDatum(rightArg),
                                     PointerGetDatum(PG_GETARG_INTERVAL_P(0)));
    }
    else
    {
        elog(ERROR, "Incorrect datetime value for mys_intervalPlText: %s", rightArgString);
    }

    PG_RETURN_TIMESTAMP(result);
}


PG_FUNCTION_INFO_V1(mys_textMiInterval);
Datum
mys_textMiInterval(PG_FUNCTION_ARGS)
{
    Timestamp leftArg;
	char *leftArgString = text_to_cstring(PG_GETARG_TEXT_P(0));
    int timeFlags = MYS_TIME_NO_ZERO_DATE;
    Timestamp result;

    if (mys_sqlMode & MYS_MODE_INVALID_DATES)
    {
        timeFlags |= MYS_TIME_INVALID_DATES;
    }

    if (mys_strToDatetimeInternal(leftArgString, timeFlags, &leftArg))
    {
        Interval *span = PG_GETARG_INTERVAL_P(1);
        Interval tspan;
        
        tspan.month = -span->month;
        tspan.day = -span->day;
        tspan.time = -span->time;

        result = DirectFunctionCall2(timestamp_pl_interval,
                                     TimestampGetDatum(leftArg),
                                     PointerGetDatum(&tspan));
    }
    else
    {
        elog(ERROR, "Incorrect datetime value for mys_textMiInterval: %s", leftArgString);
    }

    PG_RETURN_TIMESTAMP(result);
}
