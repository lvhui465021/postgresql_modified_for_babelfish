/*-------------------------------------------------------------------------
 *
 * mys_varlena.c
 *    MySQL ADT compatibility: identifier splitting and HEX function.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/mysql/mys_varlena.c
 *
 *------------------------------------------------------------------------- */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/pg_list.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "varatt.h"
#include "utils/varbit.h"
#include "utils/mysql/mys_varlena.h"

/*
 * SplitIdentifierString --- parse a string containing identifiers
 *
 * This is the guts of textToQualifiedNameList, and is exported for use in
 * other situations such as parsing GUC variables.  In the GUC case, it's
 * important to avoid memory leaks, so the API is designed to minimize the
 * amount of stuff that needs to be allocated and freed.
 *
 * Inputs:
 *	rawstring: the input string; must be overwritable!	On return, it's
 *			   been modified to contain the separated identifiers.
 *	separator: the separator punctuation expected between identifiers
 *			   (typically '.' or ',').  Whitespace may also appear around
 *			   identifiers.
 * Outputs:
 *	namelist: filled with a palloc'd list of pointers to identifiers within
 *			  rawstring.  Caller should list_free() this even on error return.
 *
 * Returns true if okay, false if there is a syntax error in the string.
 *
 * Note that an empty string is considered okay here, though not in
 * textToQualifiedNameList.
 */

bool
mys_SplitIdentifierString(char *rawstring, char separator,
					      List **namelist)
{
	char	   *nextp = rawstring;
	bool		done = false;

	*namelist = NIL;

	while (scanner_isspace(*nextp))
		nextp++;				/* skip leading whitespace */

	if (*nextp == '\0')
		return true;			/* allow empty string */

	/* At the top of the loop, we are at start of a new identifier. */
	do
	{
		char	   *curname;
		char	   *endp;

		if (*nextp == '`')
		{
			/* Quoted name --- collapse quote-quote pairs, no downcasing */
			curname = nextp + 1;
			for (;;)
			{
				endp = strchr(nextp + 1, '`');
				if (endp == NULL)
					return false;	/* mismatched quotes */
				if (endp[1] != '`')
					break;		/* found end of quoted name */
				/* Collapse adjacent quotes into one quote, and look again */
				memmove(endp, endp + 1, strlen(endp));
				nextp = endp;
			}
			/* endp now points at the terminating quote */
			nextp = endp + 1;
		}
		else
		{
			/* Unquoted name --- extends to separator or whitespace */
			char	   *downname;
			int			len;

			curname = nextp;
			while (*nextp && *nextp != separator &&
				   !scanner_isspace(*nextp))
				nextp++;
			endp = nextp;
			if (curname == nextp)
				return false;	/* empty unquoted name not allowed */

			/*
			 * Downcase the identifier, using same code as main lexer does.
			 *
			 * XXX because we want to overwrite the input in-place, we cannot
			 * support a downcasing transformation that increases the string
			 * length.  This is not a problem given the current implementation
			 * of downcase_truncate_identifier, but we'll probably have to do
			 * something about this someday.
			 */
			len = endp - curname;
			downname = downcase_truncate_identifier(curname, len, false);
			Assert(strlen(downname) <= len);
			strncpy(curname, downname, len);	/* strncpy is required here */
			pfree(downname);
		}

		while (scanner_isspace(*nextp))
			nextp++;			/* skip trailing whitespace */

		if (*nextp == separator)
		{
			nextp++;
			while (scanner_isspace(*nextp))
				nextp++;		/* skip leading whitespace for next */
			/* we expect another name, so done remains false */
		}
		else if (*nextp == '\0')
			done = true;
		else
			return false;		/* invalid syntax */

		/* Now safe to overwrite separator with a null */
		*endp = '\0';

		/* Truncate name if it's overlength */
		truncate_identifier(curname, strlen(curname), false);

		/*
		 * Finished isolating current name --- add it to list
		 */
		*namelist = lappend(*namelist, curname);

		/* Loop back if we didn't reach end of string */
	} while (!done);

	return true;
}


/********************************************************************
 *
 * mys_hex
 *
 * Syntax:
 *
 *	 text hex(any type)
 *
 * Purpose:
 *
 *	MySQL-compatible HEX function.
 *	For numeric types, converts to integer then to hex string.
 *	For string/binary types, hex-encodes each byte.
 *
 ********************************************************************/

Datum
mys_hex(PG_FUNCTION_ARGS)
{
	Datum		input;
	Oid			input_type;
	text	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	input = PG_GETARG_DATUM(0);
	input_type = get_fn_expr_argtype(fcinfo->flinfo, 0);

	switch (input_type)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		{
			/* Convert to int64 then to hex string */
			Datum		int8val;
			int64		value;
			char		buf[32];
			char	   *ptr;
			static const char digits[] = "0123456789ABCDEF";
			uint64		uvalue;

			if (input_type == INT2OID)
				int8val = DirectFunctionCall1(int82, input);
			else if (input_type == INT4OID)
				int8val = DirectFunctionCall1(int84, input);
			else if (input_type == INT8OID)
				int8val = input;
			else if (input_type == FLOAT4OID)
				int8val = DirectFunctionCall1(dtoi8,
											  DirectFunctionCall1(ftod, input));
			else if (input_type == FLOAT8OID)
				int8val = DirectFunctionCall1(dtoi8, input);
			else
				int8val = DirectFunctionCall1(numeric_int8, input);

			value = DatumGetInt64(int8val);
			uvalue = (uint64) value;

			ptr = buf + sizeof(buf) - 1;
			*ptr = '\0';

			if (uvalue == 0)
			{
				*--ptr = '0';
			}
			else
			{
				while (uvalue > 0)
				{
					*--ptr = digits[uvalue & 0xF];
					uvalue >>= 4;
				}
			}

			result = cstring_to_text(ptr);
			PG_RETURN_TEXT_P(result);
		}

		case BITOID:
		case VARBITOID:
		{
			/* Hex-encode raw bytes of bit string */
			VarBit	   *bits;
			int			bytelen;
			bits8	   *data;
			char	   *hexstr;

			bits = DatumGetVarBitP(input);
			bytelen = VARBITBYTES(bits);
			data = VARBITS(bits);

			hexstr = (char *) palloc(bytelen * 2 + 1);
			hex_encode((const char *) data, bytelen, hexstr);
			hexstr[bytelen * 2] = '\0';

			/* MySQL HEX() returns uppercase */
			{
				int		k;
				for (k = 0; k < bytelen * 2; k++)
					if (hexstr[k] >= 'a' && hexstr[k] <= 'f')
						hexstr[k] -= 32;
			}

			result = cstring_to_text(hexstr);
			pfree(hexstr);
			PG_RETURN_TEXT_P(result);
		}

		default:
		{
			/* For text/bytea/other types, hex-encode the output string */
			Oid			typoutput;
			bool		typIsVarlena;
			char	   *input_str;
			int			slen;
			char	   *hexstr;

			getTypeOutputInfo(input_type, &typoutput, &typIsVarlena);
			input_str = OidOutputFunctionCall(typoutput, input);
			slen = strlen(input_str);

			hexstr = (char *) palloc(slen * 2 + 1);
			hex_encode(input_str, slen, hexstr);
			hexstr[slen * 2] = '\0';

			/* MySQL HEX() returns uppercase */
			{
				int		k;
				for (k = 0; k < slen * 2; k++)
					if (hexstr[k] >= 'a' && hexstr[k] <= 'f')
						hexstr[k] -= 32;
			}

			result = cstring_to_text(hexstr);
			pfree(input_str);
			pfree(hexstr);
			PG_RETURN_TEXT_P(result);
		}
	}
}
