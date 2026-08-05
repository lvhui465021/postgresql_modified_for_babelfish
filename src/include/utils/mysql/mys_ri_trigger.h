/*-------------------------------------------------------------------------
 *
 * mys_ri_trigger.h
 *    MySQL ADT compatibility: backtick quoting for RI triggers.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_ri_trigger.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_RI_TRIGGER_H
#define MYS_RI_TRIGGER_H

void mys_quoteOneName(char *buffer, const char *name);

#endif							/* MYS_RI_TRIGGER_H */

