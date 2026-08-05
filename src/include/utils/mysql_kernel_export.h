/*-------------------------------------------------------------------------
 *
 * mysql_kernel_export.h
 *    Kernel symbols exported for MySQL-compatibility loadable modules.
 *
 * This header documents the "kernel -> compatibility library" contract
 * (G3): symbols that the PostgreSQL core guarantees to export so that
 * independently-built MySQL-compatibility modules (mysm, and the future
 * protocol / parser shared libraries) can link against the backend.
 *
 * The backend is linked with --export-dynamic on ELF platforms, so every
 * global symbol is already resolvable at runtime.  The contract here is
 * about ABI stability: these symbols must keep their signatures and must
 * not be made static, or a compatibility module would silently break.
 *
 * The companion CI check (src/tools/check_mysql_kernel_exports.sh)
 * verifies the full imported-symbol set of every MySQL-compat .so
 * against the postgres binary; this header is the human-readable,
 * curated view of the load-bearing subset.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql_kernel_export.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYSQL_KERNEL_EXPORT_H
#define MYSQL_KERNEL_EXPORT_H

#include "fmgr.h"
#include "nodes/nodes.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

/*
 * Authentication: the MySQL wire-protocol module verifies passwords using
 * the kernel-side mysql_native_password implementation.
 */
extern int mysql_native_password_verify(const char *role,
										const char *shadow_pass,
										const char **logdetail);

/*
 * ADT / type-input hooks: the compatibility layer substitutes MySQL
 * date/time semantics by wiring these through the ADTExtMethod table.
 * The delegatee functions live in the kernel; the table itself is
 * consumed by date.c / timestamp.c / numeric.c / varchar.c.
 */
extern Datum mys_date_in(PG_FUNCTION_ARGS);
extern Datum mys_timestamp_in(PG_FUNCTION_ARGS);
extern TimeADT mys_pre_time_in(PG_FUNCTION_ARGS);
extern char *mys_post_time_out(PG_FUNCTION_ARGS);

/*
 * Session context: the MySQL module reads the dialect mode and the wire
 * protocol kind of the current backend.
 */
extern CompatibilityProtocolKind MyCompatMode;

/*
 * openHalo-specific kernel helpers consumed by the mysm shared library
 * (kept in the kernel because other kernel-side MySQL code depends on
 * them).  Moving any of these to a module would require re-homing the
 * kernel callers first.
 */
extern int mys_sqlMode;
extern bool mys_strToDatetimeInternal(const char *str, int timeFlags,
									  struct pg_tm *tm, fsec_t *fsec,
									  int *tz, bool *haveTz);
extern int mys_DecodeStringDatetimeForCompare(const char *str,
											  int timeFlags,
											  struct pg_tm *tm,
											  fsec_t *fsec, int *tzp,
											  bool *haveTz);
extern TimeADT mys_pre_time_in_for_subtime(const char *str,
										   struct pg_tm *tm,
										   fsec_t *fsec, int *sign);
extern int64 mys_setval3_oid(Oid seqoid, int64 newval, bool is_called);
extern char *getSystemVariableValueForSelect(const char *name);
extern void setSystemVariableValue(const char *name, const char *value,
								   bool isGlobal);
extern bool varValueIsDigit(const char *value);

#endif							/* MYSQL_KERNEL_EXPORT_H */
