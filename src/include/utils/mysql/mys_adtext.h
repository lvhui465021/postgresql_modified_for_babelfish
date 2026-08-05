/*-------------------------------------------------------------------------
 *
 * mys_adtext.h
 *    MySQL ADT compatibility: ADT extension method declarations.
 *
 * The MySQL ADT method table is registered with the kernel via
 * RegisterADTExt() in mys_adtext.c's _PG_init; this header exists to
 * document the module boundary.  The table itself is defined in the
 * module, not exposed to the kernel.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/mysql/mys_adtext.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_ADTEXT_H
#define MYS_ADTEXT_H

#include "utils/adtextapi.h"

/* Register the MySQL ADT method table (called at backend startup). */
extern void InitMysADTExt(void);

#endif							/* MYS_ADTEXT_H */
