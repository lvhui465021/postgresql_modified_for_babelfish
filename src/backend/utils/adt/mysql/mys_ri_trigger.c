/*-------------------------------------------------------------------------
 *
 * mys_ri_trigger.c
 *    MySQL ADT compatibility: backtick-quoted name generation for RI triggers.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/mysql/mys_ri_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/mysql/mys_ri_trigger.h"

void
mys_quoteOneName(char *buffer, const char *name)
{
	/* Rather than trying to be smart, just always quote it. */
	*buffer++ = '`';
	while (*name)
	{
		if (*name == '`')
			*buffer++ = '`';
		*buffer++ = *name++;
	}
	*buffer++ = '`';
	*buffer = '\0';
}
