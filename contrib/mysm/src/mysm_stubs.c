/*
 * mysm_stubs.c — Provide PG18 implementations for symbols that were
 * available in PG14 openHalo but removed/changed in PG18.
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/pg_locale.h"
#include "catalog/pg_collation_d.h"
#include "utils/mysql/mys_adtext.h"
PG_MODULE_MAGIC;

/*
 * _PG_init
 *
 * Register the MySQL ADT method table so that MySQL-mode backends
 * dispatch type-input/output through it.  mysm is loaded via
 * shared_preload_libraries in MySQL-mode clusters; the registration runs
 * in the postmaster before any backend forks.
 */
void
_PG_init(void)
{
	InitMysADTExt();
}

/* lc_collate_is_c — PG14 public → PG18: wrapper using pg_locale internals */
bool
lc_collate_is_c(Oid collation)
{
    if (!OidIsValid(collation))
        return true;
    /* PG18: the "C" collation has OID = DEFAULT_COLLATION_OID */
    return (collation == DEFAULT_COLLATION_OID);
}

/* User variable internal stubs — openHalo's user var module provides these.
   In PG18, user variables are handled by the adapter directly. */
void *mysGetUserVarValueInternal(const char *name) { return NULL; }
void  mysSetUserVarInternal(const char *name, const char *value) { }
void  clearUserVars(void) { }
