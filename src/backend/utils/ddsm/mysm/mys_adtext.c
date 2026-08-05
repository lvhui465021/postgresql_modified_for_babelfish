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

#include "utils/adtext.h"
#include "utils/mysql/mys_adtext.h"
#include "utils/mysql/mys_date.h"
#include "utils/mysql/mys_timestamp.h"


static const ADTExtMethod mys_adtext = {
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
	.allow_zero_length_char_typmod = true
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
