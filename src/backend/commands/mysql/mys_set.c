/*-------------------------------------------------------------------------
 *
 * mys_set.c
 *    Runtime validation and normalization for MySQL ENUM and SET columns.
 *
 * A MySQL SET is stored as its comma-separated textual representation.  The
 * parser creates a schema-local domain and a table-level CHECK which invokes
 * mys_check_set() with the declared labels.  Keeping the functions in the
 * backend makes CREATE TABLE independent of whether the optional
 * aux_mysql extension has already been installed.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/mysql/mys_set.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#ifdef USE_ICU
#include "utils/pg_locale.h"
#endif

PG_FUNCTION_INFO_V1(mys_check_set);
PG_FUNCTION_INFO_V1(mys_normalize_set);
PG_FUNCTION_INFO_V1(mys_check_enum);
PG_FUNCTION_INFO_V1(mys_normalize_enum);
PG_FUNCTION_INFO_V1(mys_check_set_with_profile);
PG_FUNCTION_INFO_V1(mys_normalize_set_with_profile);
PG_FUNCTION_INFO_V1(mys_check_enum_with_profile);
PG_FUNCTION_INFO_V1(mys_normalize_enum_with_profile);

/*
 * MySQL's nonbinary string comparisons ignore trailing spaces.  Do not use
 * isspace(): only ASCII space has this behavior for the compatible profiles.
 */
static int
mys_label_trimmed_len(const char *value, int value_len)
{
	while (value_len > 0 && value[value_len - 1] == ' ')
		value_len--;
	return value_len;
}

MysLabelProfile
mys_label_profile_from_name(const char *name)
{
	if (strcmp(name, MYS_LABEL_PROFILE_AI) == 0)
		return MYS_LABEL_PROFILE_KIND_AI;
	if (strcmp(name, MYS_LABEL_PROFILE_AS_CS) == 0)
		return MYS_LABEL_PROFILE_KIND_AS_CS;
	if (strcmp(name, MYS_LABEL_PROFILE_BINARY) == 0)
		return MYS_LABEL_PROFILE_KIND_BINARY;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown MySQL ENUM/SET label profile \"%s\"", name)));
	pg_unreachable();
}

/*
 * Compare labels with the collation profile selected by the MySQL parser.
 * The return value follows strcmp() and is exported so DDL duplicate checks
 * use precisely the same rules as data validation.
 */
int
mys_compare_enum_set_labels(const char *left, int left_len,
						const char *right, int right_len,
						MysLabelProfile profile)
{
	int			compare_len;
	int			result;

	if (profile != MYS_LABEL_PROFILE_KIND_BINARY)
	{
		left_len = mys_label_trimmed_len(left, left_len);
		right_len = mys_label_trimmed_len(right, right_len);
	}

	if (profile == MYS_LABEL_PROFILE_KIND_AI)
	{
#ifdef USE_ICU
		static UCollator *primary_collator = NULL;
		UErrorCode	status = U_ZERO_ERROR;
		UCollationResult collation_result;

		if (primary_collator == NULL)
		{
			/* "und" is ICU's locale-neutral Unicode collation. */
			primary_collator = ucol_open("und", &status);
			if (U_FAILURE(status))
				ereport(ERROR,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("could not open ICU primary collator: %s",
							u_errorName(status))));

			status = U_ZERO_ERROR;
			ucol_setStrength(primary_collator, UCOL_PRIMARY);
			ucol_setAttribute(primary_collator, UCOL_NORMALIZATION_MODE,
							  UCOL_ON, &status);
			if (U_FAILURE(status))
				ereport(ERROR,
					(errcode(ERRCODE_SYSTEM_ERROR),
					 errmsg("could not configure ICU primary collator: %s",
							u_errorName(status))));

		}

		status = U_ZERO_ERROR;
		collation_result = ucol_strcollUTF8(primary_collator,
										left, left_len, right, right_len,
										&status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					 errmsg("could not compare MySQL ENUM/SET labels with ICU: %s",
							u_errorName(status))));
		return (int) collation_result;
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MySQL accent-insensitive ENUM/SET labels require ICU support")));
#endif
	}

	compare_len = Min(left_len, right_len);
	result = memcmp(left, right, compare_len);
	if (result != 0)
		return result;
	return (left_len > right_len) - (left_len < right_len);
}

static MysLabelProfile
mys_label_profile_from_text(text *profile)
{
	char	   *profile_name = text_to_cstring(profile);

	return mys_label_profile_from_name(profile_name);
}

static bool
mys_set_label_is_declared(Datum *labels, bool *nulls, int nlabels,
					  const char *candidate, int candidate_len,
					  MysLabelProfile profile)
{
	int			i;

	for (i = 0; i < nlabels; i++)
	{
		text	   *label;
		int			label_len;

		if (nulls[i])
			continue;
		label = DatumGetTextPP(labels[i]);
		label_len = VARSIZE_ANY_EXHDR(label);
		if (mys_compare_enum_set_labels(VARDATA_ANY(label), label_len,
										candidate, candidate_len, profile) == 0)
			return true;
	}

	return false;
}

static int
mys_set_label_index(Datum *labels, bool *nulls, int nlabels,
					const char *candidate, int candidate_len,
					MysLabelProfile profile)
{
	int			i;

	for (i = 0; i < nlabels; i++)
	{
		text	   *label;
		int			label_len;

		if (nulls[i])
			continue;
		label = DatumGetTextPP(labels[i]);
		label_len = VARSIZE_ANY_EXHDR(label);
		if (mys_compare_enum_set_labels(VARDATA_ANY(label), label_len,
										candidate, candidate_len, profile) == 0)
			return i;
	}

	return -1;
}

static Datum
mys_check_set_internal(ArrayType *declared, text *value,
					   MysLabelProfile profile)
{
	Datum	   *labels;
	bool	   *nulls;
	int			nlabels;
	const char *input = VARDATA_ANY(value);
	int			input_len = VARSIZE_ANY_EXHDR(value);
	int			start = 0;
	int			pos;

	deconstruct_array(declared, TEXTOID, -1, false, TYPALIGN_INT,
					  &labels, &nulls, &nlabels);

	/* The empty string represents an empty MySQL SET. */
	if (input_len == 0)
		PG_RETURN_BOOL(true);

	for (pos = 0; pos <= input_len; pos++)
	{
		int			token_len;
		int			previous_start;
		int			previous_pos;

		if (pos != input_len && input[pos] != ',')
			continue;

		token_len = pos - start;
		if (token_len == 0 ||
			!mys_set_label_is_declared(labels, nulls, nlabels,
								   input + start, token_len, profile))
			PG_RETURN_BOOL(false);

		/* MySQL stores a SET without duplicate members. */
		previous_start = 0;
		for (previous_pos = 0; previous_pos < start; previous_pos++)
		{
			if (input[previous_pos] != ',')
				continue;
			if (mys_compare_enum_set_labels(input + previous_start,
											previous_pos - previous_start,
											input + start, token_len, profile) == 0)
				PG_RETURN_BOOL(false);
			previous_start = previous_pos + 1;
		}
		start = pos + 1;
	}

	PG_RETURN_BOOL(true);
}

Datum
mys_check_set(PG_FUNCTION_ARGS)
{
	ArrayType  *declared = PG_GETARG_ARRAYTYPE_P(0);
	text	   *value = PG_GETARG_TEXT_PP(1);
	MysLabelProfile profile = MYS_LABEL_PROFILE_KIND_BINARY;

	if (PG_NARGS() == 3)
		profile = mys_label_profile_from_text(PG_GETARG_TEXT_PP(2));

	return mys_check_set_internal(declared, value, profile);
}

static Datum
mys_normalize_set_internal(ArrayType *declared, text *value,
						   MysLabelProfile profile)
{
	Datum	   *labels;
	bool	   *nulls;
	bool	   *selected;
	int			nlabels;
	const char *input = VARDATA_ANY(value);
	int			input_len = VARSIZE_ANY_EXHDR(value);
	int			start = 0;
	int			pos;
	int			i;
	StringInfoData normalized;

	deconstruct_array(declared, TEXTOID, -1, false, TYPALIGN_INT,
					  &labels, &nulls, &nlabels);
	selected = palloc0(sizeof(bool) * nlabels);

	if (input_len != 0)
	{
		for (pos = 0; pos <= input_len; pos++)
		{
			int			member_len;
			int			member_index;

			if (pos != input_len && input[pos] != ',')
				continue;

			member_len = pos - start;
			member_index = member_len == 0 ? -1 :
				mys_set_label_index(labels, nulls, nlabels, input + start,
									member_len, profile);
			if (member_index < 0)
				ereport(ERROR,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("invalid value for MySQL SET")));
			selected[member_index] = true;
			start = pos + 1;
		}
	}

	initStringInfo(&normalized);
	for (i = 0; i < nlabels; i++)
	{
		text	   *label;

		if (!selected[i] || nulls[i])
			continue;
		if (normalized.len != 0)
			appendStringInfoChar(&normalized, ',');
		label = DatumGetTextPP(labels[i]);
		appendBinaryStringInfo(&normalized, VARDATA_ANY(label),
							   VARSIZE_ANY_EXHDR(label));
	}

	PG_RETURN_TEXT_P(cstring_to_text_with_len(normalized.data, normalized.len));
}

Datum
mys_normalize_set(PG_FUNCTION_ARGS)
{
	ArrayType  *declared = PG_GETARG_ARRAYTYPE_P(0);
	text	   *value = PG_GETARG_TEXT_PP(1);
	MysLabelProfile profile = MYS_LABEL_PROFILE_KIND_BINARY;

	if (PG_NARGS() == 3)
		profile = mys_label_profile_from_text(PG_GETARG_TEXT_PP(2));

	return mys_normalize_set_internal(declared, value, profile);
}

Datum
mys_check_enum(PG_FUNCTION_ARGS)
{
	ArrayType  *declared = PG_GETARG_ARRAYTYPE_P(0);
	text	   *value = PG_GETARG_TEXT_PP(1);
	MysLabelProfile profile = mys_label_profile_from_text(PG_GETARG_TEXT_PP(2));
	Datum	   *labels;
	bool	   *nulls;
	int			nlabels;

	deconstruct_array(declared, TEXTOID, -1, false, TYPALIGN_INT,
					  &labels, &nulls, &nlabels);
	PG_RETURN_BOOL(mys_set_label_is_declared(labels, nulls, nlabels,
										  VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value),
										  profile));
}

Datum
mys_normalize_enum(PG_FUNCTION_ARGS)
{
	ArrayType  *declared = PG_GETARG_ARRAYTYPE_P(0);
	text	   *value = PG_GETARG_TEXT_PP(1);
	MysLabelProfile profile = mys_label_profile_from_text(PG_GETARG_TEXT_PP(2));
	Datum	   *labels;
	bool	   *nulls;
	int			nlabels;
	int			label_index;
	text	   *label;

	deconstruct_array(declared, TEXTOID, -1, false, TYPALIGN_INT,
					  &labels, &nulls, &nlabels);
	label_index = mys_set_label_index(labels, nulls, nlabels,
										 VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value),
										 profile);
	if (label_index < 0)
		ereport(ERROR,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("invalid value for MySQL ENUM")));

	label = DatumGetTextPP(labels[label_index]);
	PG_RETURN_TEXT_P(cstring_to_text_with_len(VARDATA_ANY(label),
											VARSIZE_ANY_EXHDR(label)));
}

/* Keep 2-argument legacy and 3-argument profile pg_proc entries distinct. */
Datum
mys_check_set_with_profile(PG_FUNCTION_ARGS)
{
	return mys_check_set(fcinfo);
}

Datum
mys_normalize_set_with_profile(PG_FUNCTION_ARGS)
{
	return mys_normalize_set(fcinfo);
}

Datum
mys_check_enum_with_profile(PG_FUNCTION_ARGS)
{
	return mys_check_enum(fcinfo);
}

Datum
mys_normalize_enum_with_profile(PG_FUNCTION_ARGS)
{
	return mys_normalize_enum(fcinfo);
}
