/*-------------------------------------------------------------------------
 *
 * mys_adtext.c
 *    MySQL ADT compatibility: ADT extension method table for MySQL.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/mysql/mys_adtext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "utils/adtext.h"
#include "utils/fmgrprotos.h"
#include "utils/mysql/mys_adtext.h"
#include "utils/mysql/mys_date.h"
#include "utils/mysql/mys_timestamp.h"
#include "utils/pg_locale.h"

/*
 * MySQL's own DECIMAL/CHAR limits, enforced nowhere else: mys_gram.y lowers
 * DECIMAL/DEC/NUMERIC(p,s) and CHAR(n) straight to PostgreSQL's "numeric"
 * and "bpchar" system types (SystemTypeName()), so absent this the only
 * limit ever checked is PostgreSQL's own (numeric precision up to 1000,
 * bpchar length up to ~10M) -- silently accepting DDL MySQL itself would
 * reject with ER_TOO_BIG_PRECISION/ER_TOO_BIG_SCALE/ER_TOO_BIG_DISPLAYWIDTH.
 */
#define MYS_NUMERIC_MAX_PRECISION	65
#define MYS_NUMERIC_MAX_SCALE		30
#define MYS_CHAR_MAX_LENGTH			255

static void
mys_validate_var_datatype_scale(const TypeName *typeName, Type typ)
{
	Oid			datatype_oid = ((Form_pg_type) GETSTRUCT(typ))->oid;
	int			typmod[2] = {-1, -1};
	int			count = 0;
	ListCell   *l;

	if (datatype_oid != NUMERICOID && datatype_oid != BPCHAROID)
		return;

	foreach(l, typeName->typmods)
	{
		Node	   *tm = (Node *) lfirst(l);

		if (count >= 2)
			break;

		if (IsA(tm, A_Const) && IsA(&((A_Const *) tm)->val, Integer))
			typmod[count++] = intVal(&((A_Const *) tm)->val);
	}

	if (datatype_oid == NUMERICOID)
	{
		int			precision = typmod[0];
		int			scale = typmod[1];

		if (precision == -1)	/* bare NUMERIC/DECIMAL, no typmod given */
			return;

		if (precision < 1 || precision > MYS_NUMERIC_MAX_PRECISION)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Too big precision %d specified for 'decimal'. Maximum is %d.",
							precision, MYS_NUMERIC_MAX_PRECISION)));

		if (scale == -1)
			scale = 0;

		if (scale < 0 || scale > MYS_NUMERIC_MAX_SCALE || scale > precision)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Too big scale %d specified for 'decimal'. Maximum is %d.",
							scale, Min(precision, MYS_NUMERIC_MAX_SCALE))));
	}
	else						/* BPCHAROID */
	{
		int			length = typmod[0];

		if (length > MYS_CHAR_MAX_LENGTH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Column length too big for 'char' (max = %d); use VARCHAR, BLOB or TEXT instead",
							MYS_CHAR_MAX_LENGTH)));
	}
}

/*
 * MySQL's REPLACE() is documented to match byte-for-byte regardless of the
 * column's collation -- unlike POSITION()/LOCATE(), which correctly inherit
 * case-insensitivity from a nondeterministic mysql.case_insensitive
 * collation via PostgreSQL 18's native nondeterministic-collation support
 * (no override needed there; see the fix.md investigation this registration
 * is based on). Left unregistered, replace_text()'s fallback path would
 * apply that same nondeterministic-collation case folding to REPLACE() too,
 * which MySQL's own documentation explicitly says does not happen.
 */
static bool
mys_replace_non_deterministic(text *t1, text *t2, text *t3, Oid collid, text **result)
{
	if (pg_newlocale_from_collation(collid)->deterministic)
		return false;

	/*
	 * Re-run replace_text() forced to a deterministic collation. The
	 * recursive call reaches this same function with a deterministic
	 * collid, which returns false immediately, so this terminates after one
	 * extra frame and falls through to replace_text()'s ordinary
	 * byte-for-byte matching loop.
	 */
	*result = DatumGetTextPP(DirectFunctionCall3Coll(replace_text,
													  C_COLLATION_OID,
													  PointerGetDatum(t1),
													  PointerGetDatum(t2),
													  PointerGetDatum(t3)));
	return true;
}

static const ADTExtMethod mys_adtext = {
	ADTEXT_METHOD_HEADER_INIT,
	.pre_numeric_in = NULL,
	.post_numeric_out = NULL,
	.pre_time_in = mys_pre_time_in,
	.post_time_out = mys_post_time_out,
	.pre_timetz_in = NULL,
	.post_timetz_out = NULL,
	.pre_timestamp_in = NULL,
	.post_timestamp_out = NULL,
	.date_in = mys_date_in,
	.timestamp_in = mys_timestamp_in,
	.allow_zero_length_char_typmod = true,
	.expr_typmod = NULL,
	.coalesce_typmod = NULL,
	.validate_var_datatype_scale = mys_validate_var_datatype_scale,
	.param_collation = NULL,
	.default_collation = NULL,
	.strpos_non_deterministic = NULL,
	.replace_non_deterministic = mys_replace_non_deterministic,
	.adjust_numeric_result = NULL,
	.detect_numeric_overflow = NULL,
	.identity_datatype = NULL,
	.sequence_datatype = NULL,
	.sortby_nulls = NULL,
	.unique_constraint_nulls_ordering = NULL
};

/*
 * InitMysADTExt
 *
 * Register the MySQL ADT method table with the kernel so that MySQL-mode
 * backends dispatch to it.  Called during backend startup (and from the
 * module's _PG_init once the type layer is externalized to a loadable
 * library).
 */
void
InitMysADTExt(void)
{
	RegisterADTExt(COMPAT_PROTOCOL_MYSQL, &mys_adtext);
}
