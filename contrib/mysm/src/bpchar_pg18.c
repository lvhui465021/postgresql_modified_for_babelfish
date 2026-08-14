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
#include "utils/fmgrprotos.h"
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
	Oid			collation = PG_GET_COLLATION();

	/*
	 * Delegate to the kernel's LIKE matcher (varlena.c / textlike) so
	 * wildcard semantics follow the operand collation: case/accent folding
	 * under a nondeterministic collation (mysql.case_insensitive) and
	 * character-based '_' advancement for multi-byte encodings.  MySQL
	 * defines LIKE by the column's collation, not by raw bytes.  The
	 * previous byte-wise matcher never consulted PG_GET_COLLATION(), so
	 * CHAR columns stayed case-sensitive under mysql.case_insensitive and
	 * '_' consumed one byte instead of one character.
	 */
	PG_RETURN_BOOL(DatumGetBool(DirectFunctionCall2Coll(textlike, collation,
														PointerGetDatum(left),
														PointerGetDatum(pattern))));
}

PG_FUNCTION_INFO_V1(bpcharnlike);
Datum
bpcharnlike(PG_FUNCTION_ARGS)
{
	text	   *left = trim_trailing_spaces(PG_GETARG_BPCHAR_PP(0));
	text	   *pattern = PG_GETARG_TEXT_PP(1);
	Oid			collation = PG_GET_COLLATION();

	PG_RETURN_BOOL(!DatumGetBool(DirectFunctionCall2Coll(textlike, collation,
														PointerGetDatum(left),
														PointerGetDatum(pattern))));
}
