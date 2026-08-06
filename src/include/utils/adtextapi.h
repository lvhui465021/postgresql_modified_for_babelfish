/*-------------------------------------------------------------------------
 *
 * adtextapi.h
 *    Extension API for ADT Data Types.
 *
 * Provides a hook table so that dialect-specific type implementations
 * (MySQL, openHalo, etc.) can override the standard PostgreSQL ADT
 * functions for date, time, timestamp, numeric, and varchar handling.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/adtextapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef ADTEXTAPI_H
#define ADTEXTAPI_H

#include "fmgr.h"
#include "utils/date.h"

/*
 * Callback function types for ADT extension points.
 */
typedef char *(*pre_numeric_in_function)(char *str);
typedef char *(*post_numeric_out_function)(char *str);
typedef TimeADT (*pre_time_in_function)(PG_FUNCTION_ARGS);
typedef char *(*post_time_out_function)(PG_FUNCTION_ARGS);
typedef char *(*pre_timetz_in_function)(char *str);
typedef char *(*post_timetz_out_function)(void *time);
typedef char *(*pre_timestamp_in_function)(char *str);
typedef void (*post_timestamp_out_function)(void *ts, int style, char *buf);
typedef Datum (*date_in_function)(PG_FUNCTION_ARGS);
typedef Datum (*timestamp_in_function)(PG_FUNCTION_ARGS);

/*
 * ABI guard.
 *
 * ADTExtMethod crosses a shared-library boundary: the kernel's own
 * standard_adtext is compiled into the postgres binary, but a dialect
 * module (mysm.so, and potentially a babelfish_extensions module in the
 * future) supplies its own const ADTExtMethod instance from a separately
 * compiled .so, built against whatever adtextapi.h it happened to have on
 * disk at build time. src/tools/check_mysql_kernel_exports.sh (the "G3"
 * kernel<->module symbol contract check) only verifies that imported
 * *symbol names* resolve -- it cannot and does not check struct layout, so
 * it would report success even if a module's ADTExtMethod has a different
 * field count/order than the kernel's. A stale module would then silently
 * read garbage past the end of its own initializer (fields the kernel adds
 * later) or misinterpret which callback is which. magic/struct_size are
 * populated by every registrant and checked by RegisterADTExt() so that
 * mismatch is a loud, deterministic ereport(ERROR) at module-load time
 * instead of an undiagnosable crash or wrong-answer bug at query time.
 * version is not checked; it exists purely as a human-readable "which
 * layout is this" marker in error messages.
 *
 * Struct-layout policy (read before touching this struct):
 *   1. New fields are appended at the end ONLY. Never insert, reorder, or
 *      remove a field -- if a slot becomes obsolete, leave it in place,
 *      always NULL, with a comment saying so.
 *   2. Bump ADTEXT_METHOD_VERSION on every append.
 *   3. Every ADTExtMethod instance (standard_adtext, mys_adtext, and any
 *      future registrant) must initialize magic/struct_size/version via
 *      ADTEXT_METHOD_HEADER_INIT -- do not set these three fields by hand.
 *   4. Do not add unnamed/reserved padding "for future use". Appending is
 *      already safe and (per point 3) detectable; a named-but-unused slot
 *      only invites a future author to repurpose it with the wrong
 *      signature and no version bump, defeating the whole point of this
 *      guard.
 */
#define ADTEXT_METHOD_MAGIC    0x41445845u   /* "ADXE" */
#define ADTEXT_METHOD_VERSION  1             /* bump on every field append */

#define ADTEXT_METHOD_HEADER_INIT \
	.magic = ADTEXT_METHOD_MAGIC, \
	.struct_size = sizeof(ADTExtMethod), \
	.version = ADTEXT_METHOD_VERSION

typedef struct ADTExtMethod
{
	/*
	 * ABI guard fields.  MUST be first, MUST be set via
	 * ADTEXT_METHOD_HEADER_INIT by every registrant.  See the policy
	 * comment above.
	 */
	uint32						magic;			/* ADTEXT_METHOD_MAGIC */
	uint32						struct_size;	/* sizeof(ADTExtMethod) as the registrant saw it */
	uint32						version;		/* ADTEXT_METHOD_VERSION, diagnostics only */

	/* Forward-compatible ADT slots retained for future dialect modules. */
	pre_numeric_in_function		pre_numeric_in;
	post_numeric_out_function	post_numeric_out;
	pre_time_in_function		pre_time_in;
	post_time_out_function		post_time_out;
	pre_timetz_in_function		pre_timetz_in;
	post_timetz_out_function	post_timetz_out;
	pre_timestamp_in_function	pre_timestamp_in;
	post_timestamp_out_function post_timestamp_out;
	date_in_function			date_in;
	timestamp_in_function		timestamp_in;
	bool						allow_zero_length_char_typmod;
} ADTExtMethod;

#endif							/* ADTEXTAPI_H */
