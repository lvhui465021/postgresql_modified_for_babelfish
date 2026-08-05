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
	.allow_zero_length_char_typmod = false
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
