/*-------------------------------------------------------------------------
 *
 * compatibility.c
 *    Unified per-dialect compatibility routine registry.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/postmaster/compatibility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "postmaster/compatibility.h"

static CompatibilityRoutine compatibility_routines[COMPAT_PROTOCOL_KIND_MAX];

static inline bool
compatibility_kind_is_valid(CompatibilityProtocolKind kind)
{
	return kind >= COMPAT_PROTOCOL_POSTGRES &&
		kind < COMPAT_PROTOCOL_KIND_MAX;
}

const CompatibilityRoutine *
GetCompatibilityRoutine(CompatibilityProtocolKind kind)
{
	if (!compatibility_kind_is_valid(kind))
		return NULL;

	return &compatibility_routines[kind];
}

void
RegisterCompatibilityParser(CompatibilityProtocolKind kind,
							 const struct ParserRoutine *routine)
{
	Assert(compatibility_kind_is_valid(kind));
	compatibility_routines[kind].parser = routine;
}

void
RegisterCompatibilityADTExt(CompatibilityProtocolKind kind,
							const struct ADTExtMethod *method)
{
	Assert(compatibility_kind_is_valid(kind));
	compatibility_routines[kind].adtext = method;
}

void
UnregisterCompatibilityADTExt(CompatibilityProtocolKind kind)
{
	Assert(compatibility_kind_is_valid(kind));
	compatibility_routines[kind].adtext = NULL;
}

void
RegisterCompatibilityProtocol(CompatibilityProtocolKind kind,
							  const struct ProtocolRoutine *routine)
{
	Assert(compatibility_kind_is_valid(kind));
	compatibility_routines[kind].protocol = routine;
}
