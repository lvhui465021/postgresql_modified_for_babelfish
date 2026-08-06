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
#include "nodes/plannodes.h"		/* Plan */
#include "parser/parse_type.h"		/* Type, ParseState, parse/prim nodes */
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
 * Dialect type/collation/typmod dispatch callback types (W5). Each mirrors
 * a Babelfish global hook of identical signature (babelfish_extensions/
 * contrib/babelfishpg_tsql/src/{hooks.c,pl_handler.c}) so a single
 * implementation can be assigned to either the global hook or the vtable
 * slot with no cast. See adtextapi.h's struct-field comments below for
 * what each one dispatches and known contract pitfalls (in particular
 * default_collation and coalesce_typmod: neither callback can signal
 * "I don't recognize this input", so a registrant must return/replicate
 * the standard-PG answer for inputs it doesn't own).
 */
typedef int32 (*expr_typmod_function) (Plan *plan, Node *expr);
typedef int32 (*coalesce_typmod_function) (const CoalesceExpr *cexpr);
typedef void (*validate_var_datatype_scale_function) (const TypeName *typeName, Type typ);
typedef Oid (*param_collation_function) (Param *param);
typedef Oid (*default_collation_function) (Type typ, bool handle_pg_type);
typedef bool (*strpos_non_deterministic_function) (text *t1, text *t2, Oid collid, int *result);
typedef bool (*replace_non_deterministic_function) (text *t1, text *t2, text *t3, Oid collid, text **result);
typedef Datum (*adjust_numeric_result_function) (Plan *plan, Node *expr, Datum result, bool result_isnull, Oid result_type, int32 result_typmod);
typedef bool (*detect_numeric_overflow_function) (int weight, int dscale, int first_block, int numeric_base);
typedef void (*identity_datatype_function) (ParseState *pstate, ColumnDef *column);
typedef void (*sequence_datatype_function) (ParseState *pstate, Oid *newtypid, bool for_identity, DefElem *as_type, DefElem **max_value, DefElem **min_value);
typedef void (*sortby_nulls_function) (SortGroupClause *sortcl, bool reverse);
typedef SortByNulls (*unique_constraint_nulls_ordering_function) (ConstrType constraint_type, SortByDir ordering);

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
#define ADTEXT_METHOD_VERSION  2             /* bump on every field append */

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

	/*
	 * Dialect type/collation/typmod dispatch (W5).  Each slot mirrors a
	 * Babelfish global hook of identical signature; the kernel consults the
	 * slot first and only falls back to the global hook when the slot is
	 * NULL, so a MySQL connection never runs T-SQL type semantics merely
	 * because babelfishpg_tsql happens to be loaded in the same cluster.
	 *
	 * default_collation and coalesce_typmod cannot signal "not mine": the
	 * callback's return value unconditionally replaces the kernel's own
	 * answer (there is no out-parameter or sentinel meaning "I decline").
	 * A registrant MUST return/replicate the standard-PG answer for any
	 * input it does not specifically own, or it silently changes behavior
	 * for types/expressions unrelated to its own dialect.
	 *
	 * coalesce_typmod specifically: neither the kernel's own
	 * coalesce_typmod_hook_impl-style implementations nor this slot's
	 * contract can tell "T-SQL ISNULL()" apart from a plain COALESCE() on
	 * their own -- that distinction is cexpr->tsql_is_null, a field set
	 * exclusively by the T-SQL grammar (never true for a MySQL- or
	 * PG-parsed CoalesceExpr). The kernel call site
	 * (nodeFuncs.c:exprTypmod(), T_CoalesceExpr) gates only the *legacy
	 * hook* fallback on cexpr->tsql_is_null; the vtable slot itself is NOT
	 * gated on it. A vtable registrant is called for every CoalesceExpr in
	 * its own dialect (not just ISNULL()-flagged ones) and is responsible
	 * for returning the standard-PG answer when it doesn't want to special-
	 * case a given node -- see the "cannot signal not mine" note above.
	 */

	/* Typmod of an expression the standard rules can't type (exprTypmod_hook) */
	expr_typmod_function					expr_typmod;
	/* Typmod of a COALESCE whose arms disagree (coalesce_typmod_hook); see tsql_is_null note above */
	coalesce_typmod_function				coalesce_typmod;
	/*
	 * Reject out-of-range precision/scale in a declared type
	 * (validate_var_datatype_scale_hook). Neither the legacy Babelfish
	 * implementation nor this slot has an internal dialect check; the
	 * kernel call site (parse_type.c, typenameTypeMod()) gates only the
	 * legacy hook fallback on sql_dialect == SQL_DIALECT_TSQL, so that
	 * hook keeps applying T-SQL's numeric(p,s) limits (p<=38) only on
	 * T-SQL connections. The vtable slot itself is NOT gated on
	 * sql_dialect -- do not add that gate here. Dialect correctness for
	 * this slot comes entirely from which ADTExtMethod is active
	 * (MyCompatMode/RegisterADTExt); sql_dialect only distinguishes PG
	 * from T-SQL (a MySQL connection is SQL_DIALECT_PG too), so gating the
	 * slot on it would make a MySQL registrant's implementation
	 * permanently unreachable.
	 */
	validate_var_datatype_scale_function	validate_var_datatype_scale;
	/* Collation to stamp on an extern Param (handle_param_collation_hook) */
	param_collation_function				param_collation;
	/*
	 * Dialect default collation for a pg_type tuple
	 * (handle_default_collation_hook). Babelfish's own implementation is
	 * only self-guarded when called with handle_pg_type=false
	 * (parse_type.c's call site); the handle_pg_type=true call site
	 * (lsyscache.c's get_typcollation(), used on every default-collation
	 * type lookup) reaches an internal branch with no dialect check at all
	 * -- it is safe today only because it bottoms out in
	 * BABELFISH_CLUSTER_COLLATION_OID(), which self-guards two modules away
	 * in babelfishpg_common. A future registrant should not assume
	 * call-site safety here; verify the specific Babelfish implementation
	 * you point this at self-guards on its own before relying on it.
	 */
	default_collation_function				default_collation;
	/* position()/strpos() under a non-deterministic collation (pltsql_strpos_non_determinstic_hook) */
	strpos_non_deterministic_function		strpos_non_deterministic;
	/* replace() under a non-deterministic collation (pltsql_replace_non_determinstic_hook) */
	replace_non_deterministic_function		replace_non_deterministic;
	/* Re-round/re-scale a numeric result to dialect rules (adjust_numeric_result_hook) */
	adjust_numeric_result_function			adjust_numeric_result;
	/* Dialect numeric overflow test on an unpacked NumericVar (detect_numeric_overflow_hook) */
	detect_numeric_overflow_function		detect_numeric_overflow;
	/* Validate/adjust the type of an IDENTITY column (pltsql_identity_datatype_hook) */
	identity_datatype_function				identity_datatype;
	/* Resolve CREATE SEQUENCE ... AS <type> and its min/max (pltsql_sequence_datatype_hook) */
	sequence_datatype_function				sequence_datatype;
	/* NULLS FIRST/LAST default for ORDER BY (sortby_nulls_hook) */
	sortby_nulls_function					sortby_nulls;
	/*
	 * NULLS ordering for a UNIQUE/PK index column
	 * (pltsql_unique_constraint_nulls_ordering_hook). The underlying
	 * Babelfish implementation has no internal dialect check of its own; the
	 * kernel call site (parse_utilcmd.c, transformIndexConstraint()) gates
	 * only the legacy hook fallback on sql_dialect == SQL_DIALECT_TSQL. The
	 * vtable slot itself must NOT be gated on sql_dialect -- dialect
	 * correctness for the slot comes from which ADTExtMethod is active
	 * (MyCompatMode/RegisterADTExt), and sql_dialect cannot distinguish a
	 * MySQL connection from plain PG, so gating the slot on it would make
	 * a MySQL registrant's implementation permanently unreachable.
	 */
	unique_constraint_nulls_ordering_function unique_constraint_nulls_ordering;
} ADTExtMethod;

#endif							/* ADTEXTAPI_H */
