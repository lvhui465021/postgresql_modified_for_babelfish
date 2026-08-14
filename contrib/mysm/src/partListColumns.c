/*-------------------------------------------------------------------------
 *
 * partListColumns.c
 *	  Extend strfuncs routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/partListColumns.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include <ctype.h>
#include <limits.h>
#include <strings.h>

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static FmgrInfo *build_concat_foutcache(FunctionCallInfo fcinfo, int argidx);


PG_FUNCTION_INFO_V1(partListColumns);
Datum
partListColumns(PG_FUNCTION_ARGS)
{
	text *result;
    StringInfoData str;
    FmgrInfo *foutcache;

    initStringInfo(&str);
    foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
    if (foutcache == NULL)
    {
        foutcache = build_concat_foutcache(fcinfo, 0);
    }

    appendStringInfoString(&str, "(");
    for (int i = 0; i < PG_NARGS(); i++)
    {
        if (0 < i)
        {
            appendStringInfoString(&str, ",");
        }

        if (!PG_ARGISNULL(i))
        {
            Datum value = PG_GETARG_DATUM(i);
            Oid valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
            Oid valBaseType = getBaseType(valtype);

            if (valBaseType != BYTEAOID)
            {
                appendStringInfoString(&str, 
                                       OutputFunctionCall(&foutcache[i], value));
            }
            else 
            {
                bytea *vlena = DatumGetByteaPP(value);
                char *byte = VARDATA_ANY(vlena);
                size_t byteLen = VARSIZE_ANY_EXHDR(vlena);
                pg_verifymbstr(byte, byteLen, false);
                appendBinaryStringInfo(&str, byte, byteLen);
            }
        }
    }
    appendStringInfoString(&str, ")");

    result = cstring_to_text_with_len(str.data, str.len);
    pfree(str.data);

    PG_RETURN_TEXT_P(result);
}


static FmgrInfo *
build_concat_foutcache(FunctionCallInfo fcinfo, int argidx)
{
	FmgrInfo   *foutcache;
	int			i;

	/* We keep the info in fn_mcxt so it survives across calls */
	foutcache = (FmgrInfo *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												PG_NARGS() * sizeof(FmgrInfo));

	for (i = argidx; i < PG_NARGS(); i++)
	{
		Oid			valtype;
		Oid			typOutput;
		bool		typIsVarlena;

		valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		if (!OidIsValid(valtype))
			elog(ERROR, "could not determine data type of concat() input");

		getTypeOutputInfo(valtype, &typOutput, &typIsVarlena);
		fmgr_info_cxt(typOutput, &foutcache[i], fcinfo->flinfo->fn_mcxt);
	}

	fcinfo->flinfo->fn_extra = foutcache;

	return foutcache;
}
