/*-------------------------------------------------------------------------
 *
 * user.c
 *	  Extend user_var routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/user.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"


PG_FUNCTION_INFO_V1(getCurrentUser);
Datum
getCurrentUser(PG_FUNCTION_ARGS)
{
    if ((MyProcPort != NULL) && (MyProcPort->user_name != NULL))
    {
        char userInfo[256];
        snprintf(userInfo, 256, "%s@%%", MyProcPort->user_name);
        PG_RETURN_TEXT_P(cstring_to_text(userInfo));
    }
    else 
    {
        PG_RETURN_NULL();
    }
}


PG_FUNCTION_INFO_V1(getSessionUser);
Datum
getSessionUser(PG_FUNCTION_ARGS)
{
    if ((MyProcPort != NULL) && 
        (MyProcPort->user_name != NULL) && 
        (MyProcPort->remote_host != NULL))
    {
        char userInfo[256];
        snprintf(userInfo, 256, "%s@%s", MyProcPort->user_name, MyProcPort->remote_host);
        PG_RETURN_TEXT_P(cstring_to_text(userInfo));
    }
    else 
    {
        PG_RETURN_NULL();
    }
}

