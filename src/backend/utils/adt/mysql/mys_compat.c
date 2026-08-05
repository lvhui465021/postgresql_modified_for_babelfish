/*-------------------------------------------------------------------------
 *
 * mys_compat.c
 *    MySQL ADT compatibility: compatible functions (repeat, etc.).
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/mysql/mys_compat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/mysql/mys_compat.h"

#include "catalog/pg_type.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "varatt.h"
#include "utils/varbit.h"

/********************************************************************
 *
 * mys_repeat
 *
 * Syntax:
 *
 *	 text repeat(any type, int val)
 *
 * Purpose:
 *
 *	Repeat string by val.
 *
 ********************************************************************/

/*
 * mys_repeat - MySQL-compatible repeat function
 *
 * Accepts anyelement type and converts it to text, then repeats it.
 * This handles bit, bytea, and other types in a MySQL-compatible way.
 */
Datum
mys_repeat(PG_FUNCTION_ARGS)
{
	Datum		input = PG_GETARG_DATUM(0);
	Oid			input_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	int32		count = PG_GETARG_INT32(1);
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *input_str;
	text	   *result;
	int			slen, tlen;
	int			i;
	char	   *cp;

	if (count < 0)
		count = 0;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	getTypeOutputInfo(input_type, &typoutput, &typIsVarlena);
	input_str = OidOutputFunctionCall(typoutput, input);

	/* For bit type, convert binary representation to actual bytes */
	if (input_type == BITOID || input_type == VARBITOID)
	{
		VarBit	   *bits;
		int			bytelen;
		bits8	   *data;

		bits = DatumGetVarBitP(input);
		bytelen = VARBITBYTES(bits);
		data = VARBITS(bits);
		pg_verifymbstr((const char *) data, bytelen, false);
		input_str = (char *) palloc(bytelen + 1);
		memcpy(input_str, data, bytelen);
		input_str[bytelen] = '\0';
		slen = bytelen;
	}
	else
	{
		slen = strlen(input_str);
	}

	if (unlikely(pg_mul_s32_overflow(count, slen, &tlen)) ||
		unlikely(pg_add_s32_overflow(tlen, VARHDRSZ, &tlen)) ||
		unlikely(!AllocSizeIsValid(tlen)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("requested length too large")));

	result = (text *) palloc(tlen);
	SET_VARSIZE(result, tlen);
	cp = VARDATA(result);

	for (i = 0; i < count; i++)
	{
		memcpy(cp, input_str, slen);
		cp += slen;
		CHECK_FOR_INTERRUPTS();
	}

	PG_RETURN_TEXT_P(result);
}
