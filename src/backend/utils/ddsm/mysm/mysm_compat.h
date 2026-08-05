#ifndef MYSM_COMPAT_H
#define MYSM_COMPAT_H

/*
 * mysm_compat.h — PG14→PG18 API adaptation for openHalo mysm code.
 *
 * openHalo's mysm was written for PG14 and uses headers/custom types
 * that differ from PG18.  This header provides the missing pieces so
 * the original implementation logic is preserved unchanged.
 */

#include "postgres.h"
#include "fmgr.h"
#include "varatt.h"
#include "utils/varlena.h"
#include "utils/pg_locale.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"

/* openHalo custom includes → PG18 equivalents */
/* openhalo.h → standard PG types already provided by postgres.h */
/* adapter/mysql/adapter.h → session state accessors below */
#include "adapter/mysql/mysql_packet.h"
#include "adapter/mysql/mysql_protocol.h"

/* OID constants that openHalo uses but PG18 doesn't auto-export */
#ifndef VARCHAROID
#define VARCHAROID 1043
#endif
#ifndef BPCHAROID
#define BPCHAROID 1042
#endif
#ifndef TIMETZOID
#define TIMETZOID 1266
#endif
#ifndef BITOID
#define BITOID 1560
#endif
#ifndef BYTEAOID
#define BYTEAOID 17
#endif

/* openHalo-specific collation ID */
#define caseInsensitiveId  DEFAULT_COLLATION_OID
#define POSTGRES_EPOCH_JDATE  UNIX_EPOCH_JDATE

/* openHalo session globals — preserved for API compatibility */
/* These are used by rowCount/mysFoundRows/mysLastInsertId in strfuncs.c */
extern long long affectedRows;
extern unsigned long foundRows;
extern unsigned long lastInsertID;
extern char *halo_mysql_version;

/* PG14→PG18: functions moved to internal or renamed */
/* interval_justify_hours exists in PG18 but has no public header */
extern Datum interval_justify_hours(PG_FUNCTION_ARGS);
extern Datum numeric_in(PG_FUNCTION_ARGS);
extern Datum numeric_round(PG_FUNCTION_ARGS);

/* PG14→PG18: lc_collate_is_c is a regular function in PG18 pg_locale */
/* check_collation_set is void(Oid) in PG18 pg_locale.h */

#endif /* MYSM_COMPAT_H */

/* PG14→PG18: these were public in PG14, now static in like.c/like_match.c.
   Provide compatible declarations so the original openHalo code compiles. */
extern int Generic_Text_IC_like(text *str, text *pat, Oid collation);
extern int MatchText(const char *t, int tlen, const char *p, int plen, pg_locale_t locale);
extern bool lc_collate_is_c(Oid collation);

/* uuid_short internal */
extern unsigned long long getUuidShort(void);

