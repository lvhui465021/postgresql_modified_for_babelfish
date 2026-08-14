#include "postgres.h"
/*-------------------------------------------------------------------------
 *
 * uuid_short.c
 *	  Extend user_var routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/user_short.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include "fmgr.h"
#include "utils/builtins.h"
/* int8.h → builtins.h */
#include "adapter/mysql/uuidShort.h"

//#include <ctype.h>
//#include <stdio.h>
//#include <sys/sysinfo.h>
//#include <time.h>
//
//
//static unsigned long long serverID = 0;
//static unsigned long long getServerID();
//static unsigned long long getServerStartupTimeSecs();
//static unsigned long long getIncrementedVariable();
//
//
//static unsigned long long 
//getServerID()
//{
//    unsigned long long ret;
//
//    int rd = 0;
//    srand((unsigned int)time(NULL));
//    while (true)
//    {
//        rd = rand();
//        if (10000 < rd)
//        {
//            break;
//        }
//    }
//    ret = (unsigned long long)(rd % 255);
//
//    return ret;
//}
//
//
//static unsigned long long 
//getServerStartupTimeSecs()
//{
//    time_t bootTime;
//    time_t curTime;
//    struct sysinfo info;
//
//    time(&curTime);
//
//    if (sysinfo(&info)) 
//    {
//        elog(ERROR, "Failed to general uuid_short value");
//    }
//
//    if (curTime > info.uptime) 
//    {
//        bootTime = curTime - info.uptime;
//    }
//    else
//    {
//        bootTime = info.uptime - curTime;
//    }
//
//    return (unsigned long long)bootTime;
//}
//
//
//static unsigned long long 
//getIncrementedVariable()
//{
//    static unsigned long long incrementedVariable = 0;
//
//    ++incrementedVariable;
//
//    return incrementedVariable;
//}
//
//
//PG_FUNCTION_INFO_V1(uuidShort);
//Datum
//uuidShort(PG_FUNCTION_ARGS)
//{
//    char ret[64];
//    unsigned long long uuidShort = 0;
//    unsigned long long svrStartupTimeSecs = 0;
//    unsigned long long incrementedVariable = 0;
//
//    if (serverID <= 0)
//    {
//        serverID = getServerID();
//    }
//    svrStartupTimeSecs = getServerStartupTimeSecs();
//    incrementedVariable = getIncrementedVariable();
//    uuidShort = ((serverID & 255) << 56) + 
//        (svrStartupTimeSecs << 24 ) + 
//        incrementedVariable;
//    snprintf(ret, 64, "%llu", uuidShort);
//
//    PG_RETURN_TEXT_P(cstring_to_text(ret));
//}


PG_FUNCTION_INFO_V1(uuidShort);
Datum
uuidShort(PG_FUNCTION_ARGS)
{
    char ret[64];
    unsigned long long uuidShort = 0;

    uuidShort = getUuidShort();
    snprintf(ret, 64, "%llu", uuidShort);

    PG_RETURN_TEXT_P(cstring_to_text(ret));
}

