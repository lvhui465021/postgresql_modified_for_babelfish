/*-------------------------------------------------------------------------
 *
 * adtext.c
 *    Extension dispatch for ADT Data Types.
 *
 * Selects the appropriate ADT Extension method table (standard PostgreSQL
 * or MySQL) based on the active protocol and database mode.  Called once
 * during backend startup.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/adtext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "parser/parsereng.h"
#include "postmaster/compatibility.h"
#include "utils/adtext.h"

#include "miscadmin.h"
#include "libpq/libpq-be.h"


void InitADTExt(void);
const ADTExtMethod *adtext = NULL;

static const ADTExtMethod standard_adtext;

/*
 * Standard ADT Extension: all hooks are NULL (pass-through to built-in
 * PostgreSQL implementations).
 */
static const ADTExtMethod standard_adtext = {
	ADTEXT_METHOD_HEADER_INIT,
	.pre_numeric_in = NULL,
	.post_numeric_out = NULL,
	.pre_time_in = NULL,
	.post_time_out = NULL,
	.pre_timetz_in = NULL,
	.post_timetz_out = NULL,
	.pre_timestamp_in = NULL,
	.post_timestamp_out = NULL,
	.date_in = NULL,
	.timestamp_in = NULL,
	.allow_zero_length_char_typmod = false,
	.expr_typmod = NULL,
	.coalesce_typmod = NULL,
	.validate_var_datatype_scale = NULL,
	.param_collation = NULL,
	.default_collation = NULL,
	.strpos_non_deterministic = NULL,
	.replace_non_deterministic = NULL,
	.adjust_numeric_result = NULL,
	.detect_numeric_overflow = NULL,
	.identity_datatype = NULL,
	.sequence_datatype = NULL,
	.sortby_nulls = NULL,
	.unique_constraint_nulls_ordering = NULL
};

const ADTExtMethod *
GetStandardADTExt(void)
{
	return &standard_adtext;
}


/*
 * RegisterADTExt / UnregisterADTExt
 *
 * Register (or unregister) a dialect's ADT method table.  The MySQL
 * compatibility module registers its table during _PG_init; the kernel
 * dispatches to it by compatibility kind and otherwise uses the standard
 * pass-through table.  Keeping the table in a module -- rather than the
 * kernel statically referencing it -- lets the type layer ship as a
 * loadable library.
 */
void
RegisterADTExt(CompatibilityProtocolKind kind, const ADTExtMethod *table)
{
	Assert(kind >= 0 && kind < COMPAT_PROTOCOL_KIND_MAX);

	/*
	 * See adtextapi.h's ABI guard comment: a registrant built against a
	 * stale copy of this header (different field count/order) would
	 * otherwise silently read/misinterpret memory past what it actually
	 * initialized. Fail loudly at registration time instead.
	 */
	if (table->magic != ADTEXT_METHOD_MAGIC ||
		table->struct_size != sizeof(ADTExtMethod))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ADTExtMethod ABI mismatch for compatibility kind %d",
						(int) kind),
				 errdetail("Module supplied magic 0x%08X size %u version %u; "
						   "kernel expects magic 0x%08X size %zu version %u.",
						   table->magic, table->struct_size, table->version,
						   ADTEXT_METHOD_MAGIC, sizeof(ADTExtMethod),
						   ADTEXT_METHOD_VERSION),
				 errhint("Rebuild the module against the installed PostgreSQL headers.")));

	RegisterCompatibilityADTExt(kind, table);
}

void
UnregisterADTExt(CompatibilityProtocolKind kind)
{
	Assert(kind >= 0 && kind < COMPAT_PROTOCOL_KIND_MAX);
	UnregisterCompatibilityADTExt(kind);
}


/*
 * InitADTExt
 *
 * Selects the ADT extension table based on the backend's dialect context
 * (MyCompatMode).  The dialect's registered table is used when one exists;
 * otherwise the standard pass-through table is used.
 */
void
InitADTExt(void)
{
	const CompatibilityRoutine *compat =
		GetCompatibilityRoutine(MyCompatMode);

	adtext = compat != NULL ? compat->adtext : NULL;
	if (adtext == NULL)
		adtext = GetStandardADTExt();
}
