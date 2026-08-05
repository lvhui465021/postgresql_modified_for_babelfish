/*
 * PG18 implementations of the aux_mysql bpchar helpers.
 *
 * The PG14 source included like.c and varchar.c directly, but their matching
 * internals are no longer public in PG18.  Keep the extension ABI while using
 * public varlena and collation APIs instead.
 */
#include "postgres.h"

#include "access/detoast.h"
#include "fmgr.h"
#include "varatt.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "utils/varlena.h"

static text *
trim_trailing_spaces(text *value)
{
	char   *data = VARDATA_ANY(value);
	int		len = VARSIZE_ANY_EXHDR(value);

	while (len > 0 && data[len - 1] == ' ')
		len--;

	return cstring_to_text_with_len(data, len);
}

static bool
mysql_like_match(const char *value, int value_len, const char *pattern,
				 int pattern_len)
{
	while (pattern_len > 0)
	{
		if (*pattern == '%')
		{
			while (pattern_len > 0 && *pattern == '%')
			{
				pattern++;
				pattern_len--;
			}
			if (pattern_len == 0)
				return true;
			while (value_len >= 0)
			{
				if (mysql_like_match(value, value_len, pattern, pattern_len))
					return true;
				if (value_len == 0)
					break;
				value++;
				value_len--;
			}
			return false;
		}
		if (value_len == 0)
			return false;
		if (*pattern != '_' && *pattern != *value)
			return false;
		pattern++;
		pattern_len--;
		value++;
		value_len--;
	}
	return value_len == 0;
}

PG_FUNCTION_INFO_V1(char_eq_char_for_date_format);
Datum
char_eq_char_for_date_format(PG_FUNCTION_ARGS)
{
	BpChar	 *left = PG_GETARG_BPCHAR_PP(0);
	BpChar	 *right = PG_GETARG_BPCHAR_PP(1);
	int		left_len = VARSIZE_ANY_EXHDR(left);
	int		right_len = VARSIZE_ANY_EXHDR(right);

	PG_RETURN_BOOL(left_len == right_len && left_len > 0 &&
				   VARDATA_ANY(left)[0] == VARDATA_ANY(right)[0]);
}

PG_FUNCTION_INFO_V1(textne_mys);
Datum
textne_mys(PG_FUNCTION_ARGS)
{
	text	   *left = trim_trailing_spaces(PG_GETARG_TEXT_PP(0));
	text	   *right = trim_trailing_spaces(PG_GETARG_TEXT_PP(1));
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = varstr_cmp(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left),
						VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right), collation) != 0;
	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(bpcharlike);
Datum
bpcharlike(PG_FUNCTION_ARGS)
{
	text	   *left = trim_trailing_spaces(PG_GETARG_BPCHAR_PP(0));
	text	   *pattern = PG_GETARG_TEXT_PP(1);

	PG_RETURN_BOOL(mysql_like_match(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left),
								  VARDATA_ANY(pattern), VARSIZE_ANY_EXHDR(pattern)));
}

PG_FUNCTION_INFO_V1(bpcharnlike);
Datum
bpcharnlike(PG_FUNCTION_ARGS)
{
	text	   *left = trim_trailing_spaces(PG_GETARG_BPCHAR_PP(0));
	text	   *pattern = PG_GETARG_TEXT_PP(1);

	PG_RETURN_BOOL(!mysql_like_match(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left),
								   VARDATA_ANY(pattern), VARSIZE_ANY_EXHDR(pattern)));
}
