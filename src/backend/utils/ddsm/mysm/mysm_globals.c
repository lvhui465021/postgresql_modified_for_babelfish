/*
 * mysm_globals.c — Define openHalo globals used by openHalo mysm code.
 *
 * These globals are populated by the MySQL protocol adapter at runtime.
 * In PG18, the authoritative values are in MysPacketState; these globals
 * exist only for API compatibility with the openHalo mysm source.
 */
#include "postgres.h"

/* Session state globals — set by capture_session_state / adapter */
long long affectedRows = -1;
unsigned long foundRows = 0;
unsigned long lastInsertID = 0;

/* MySQL version string — mirrors mysql_server_version GUC */
char *halo_mysql_version = "8.4.10-openhalo-1.0";
