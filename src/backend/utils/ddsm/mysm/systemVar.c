/*-------------------------------------------------------------------------
 *
 * systemVar.c
 *	  Extend system_var routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/systemVar.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "adapter/mysql/systemVar.h"


PG_FUNCTION_INFO_V1(setSystemVariable);
Datum
setSystemVariable(PG_FUNCTION_ARGS)
{
    char *varName;
    char *varValue;
    bool isSessionSystemVar;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_BOOL(false);
    }
    if (PG_ARGISNULL(2))
    {
        PG_RETURN_BOOL(false);
    }

	varName = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (!PG_ARGISNULL(1))
    {
		varValue = text_to_cstring(PG_GETARG_TEXT_PP(1));
    }
    else 
    {
        varValue = NULL;
    }
    isSessionSystemVar = DatumGetBool(PG_GETARG_DATUM(2));
    setSystemVariableValue(varName, varValue, isSessionSystemVar);
    PG_RETURN_BOOL(true);
}


PG_FUNCTION_INFO_V1(getSystemVariable);
Datum
getSystemVariable(PG_FUNCTION_ARGS)
{
    char *varName;
    bool isSessionSystemVar;
    char *varValue;

	varName = text_to_cstring(PG_GETARG_TEXT_PP(0));
    isSessionSystemVar = DatumGetBool(PG_GETARG_DATUM(1));
    getSystemVariableValueForSelect(varName, 
                                    isSessionSystemVar, 
                                    &varValue);
    if (varValue != NULL)
    {
        PG_RETURN_TEXT_P(cstring_to_text(varValue));
    }
    else 
    {
        PG_RETURN_NULL();
    }
}


PG_FUNCTION_INFO_V1(getENV);
Datum
getENV(PG_FUNCTION_ARGS)
{
    char *varName = NULL;
    char *envValue = NULL;
	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("environment variable must not be null")));
	}
    
	varName = text_to_cstring(PG_GETARG_TEXT_PP(0));
    envValue = getenv(varName);
    if (envValue == NULL)
        elog(ERROR, "The environment variable %s is not set yet.", varName);

    PG_RETURN_TEXT_P(cstring_to_text(envValue));
}
