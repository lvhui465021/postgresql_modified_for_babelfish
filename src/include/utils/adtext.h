/*-------------------------------------------------------------------------
 *
 * adtext.h
 *    Global ADT Extension instance and initialization.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/utils/adtext.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ADTEXT_H
#define ADTEXT_H

#include "libpq/libpq-be.h"		/* CompatibilityProtocolKind */
#include "utils/adtextapi.h"

/* ADT Extension global instance (set by InitADTExt) */
extern const ADTExtMethod *adtext;

/* Initialize the global ADT Extension based on protocol / database mode */
extern void InitADTExt(void);

/* Return the standard (pass-through) ADT Extension */
extern const ADTExtMethod *GetStandardADTExt(void);

/*
 * Dialect ADT registration.
 *
 * A dialect that wants to substitute type-input/output semantics (MySQL
 * date/time, numeric, varchar) registers its ADTExtMethod table with
 * RegisterADTExt() at library load time, instead of the kernel statically
 * linking against the dialect implementation.  RegisterADTExt() is
 * idempotent: the last registered table for a compatibility kind wins.
 */
extern void RegisterADTExt(CompatibilityProtocolKind kind,
						   const ADTExtMethod *table);
extern void UnregisterADTExt(CompatibilityProtocolKind kind);

#endif							/* ADTEXT_H */
