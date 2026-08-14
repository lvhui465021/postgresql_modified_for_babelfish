/*-------------------------------------------------------------------------
 *
 * compatibility.h
 *    Unified per-dialect compatibility routine registry.
 *
 * A compatibility kind owns one registry entry containing the parser,
 * ADT, and wire-protocol contracts for that dialect.  The implementations
 * may still be supplied by different loadable modules; registration of an
 * individual slot updates this single entry.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/postmaster/compatibility.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMPATIBILITY_H
#define COMPATIBILITY_H

#include "libpq/libpq-be.h"

struct ADTExtMethod;
struct ParserRoutine;
struct ProtocolRoutine;

typedef struct CompatibilityRoutine
{
	const struct ParserRoutine  *parser;
	const struct ADTExtMethod   *adtext;
	const struct ProtocolRoutine *protocol;

	/*
	 * listen_init -- called once during postmaster startup so this dialect
	 * can open its listener socket(s).  The postmaster invokes the slots in
	 * protocol-kind order, so startup is deterministic regardless of
	 * shared_preload_libraries order; NULL means "no loadable listener".
	 */
	void		(*listen_init) (void);
} CompatibilityRoutine;

extern const CompatibilityRoutine *GetCompatibilityRoutine(
	CompatibilityProtocolKind kind);

extern void RegisterCompatibilityParser(CompatibilityProtocolKind kind,
									 const struct ParserRoutine *routine);
extern void RegisterCompatibilityADTExt(CompatibilityProtocolKind kind,
									const struct ADTExtMethod *method);
extern void UnregisterCompatibilityADTExt(CompatibilityProtocolKind kind);
extern void RegisterCompatibilityProtocol(CompatibilityProtocolKind kind,
									  const struct ProtocolRoutine *routine);
extern void RegisterListenInitRoutine(CompatibilityProtocolKind kind,
								   void (*routine) (void));

#endif						/* COMPATIBILITY_H */
