/*-------------------------------------------------------------------------
 *
 * protocol_routine.c
 *    Protocol-routine registry: maps CompatibilityProtocolKind to a
 *    ProtocolRoutine vtable, and provides the per-backend accessor.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/postmaster/protocol_routine.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/libpq-be.h"
#include "miscadmin.h"               /* MyProcPort */
#include "postmaster/compatibility.h"
#include "postmaster/protocol_routine.h"

/* ----------------------------------------------------------------
 *    Protocol routine registry
 *
 * The built-in PG protocol is a kernel fallback and is also inserted into
 * the unified compatibility registry on first use, so initdb and other
 * standalone invocations do not depend on module loading.
 * ----------------------------------------------------------------
 */
static const ProtocolRoutine StandardProtocolRoutine = {
    .kind = COMPAT_PROTOCOL_POSTGRES,
    .name = "PostgreSQL",
};

/* Hook for additional protocol listeners (MySQL, TDS, ...). */
listen_init_hook_type listen_init_hook = NULL;

/*
 * RegisterProtocolRoutine  –  register a protocol routine in the global
 * registry.  Called by each protocol's init function.
 */
void
RegisterProtocolRoutine(const ProtocolRoutine *routine)
{
	Assert(routine != NULL);
	Assert(CompatibilityProtocolKindIsValid(routine->kind));
	RegisterCompatibilityProtocol(routine->kind, routine);
}

/* ----------------------------------------------------------------
 *    GetCurrentProtocolRoutine
 *
 * Returns the ProtocolRoutine assigned to the current backend's Port.
 * Must only be called after MyProcPort has been initialised.
 * ----------------------------------------------------------------
 */
const ProtocolRoutine *
GetCurrentProtocolRoutine(void)
{
    /*
     * During single-user mode (initdb, standalone backend), MyProcPort is
     * NULL.  Return the built-in PG routine so that all dispatch points
     * fall through to standard behaviour.
     */
    if (MyProcPort == NULL || MyProcPort->protocol_routine == NULL)
        return &StandardProtocolRoutine;

    return MyProcPort->protocol_routine;
}

/* ----------------------------------------------------------------
 *    AssignProtocolRoutine
 *
 * Resolve the Port's protocol_kind to a registered ProtocolRoutine
 * and store it in port->protocol_routine.  Called from pq_init()
 * after the Port has been set up and protocol_kind has been copied
 * from the ClientSocket.
 * ----------------------------------------------------------------
 */
void
AssignProtocolRoutine(Port *port)
{
    Assert(port != NULL);
    Assert(CompatibilityProtocolKindIsValid(port->protocol_kind));

	/*
	 * If no routine has been registered for the PG protocol yet (e.g. during
	 * initdb where _PG_init is not invoked for statically-linked code),
	 * register the built-in standard routine on first use.  Inspect the
	 * registry slot directly because GetProtocolRoutine() deliberately
	 * provides the same standard fallback to callers.
	 */
	{
		const CompatibilityRoutine *compat =
			GetCompatibilityRoutine(COMPAT_PROTOCOL_POSTGRES);

		if (compat == NULL || compat->protocol == NULL)
			RegisterProtocolRoutine(&StandardProtocolRoutine);
	}

	port->protocol_routine = GetProtocolRoutine(port->protocol_kind);

    if (port->protocol_routine == NULL)
        elog(FATAL, "no ProtocolRoutine registered for protocol kind %d",
             (int) port->protocol_kind);
}

/* ----------------------------------------------------------------
 *    GetProtocolRoutine
 *
 * Returns the registered ProtocolRoutine for the given protocol kind.  The
 * standard PostgreSQL routine is returned as a kernel fallback even before
 * its lazy registry insertion; unregistered compatibility kinds return NULL.
 * ----------------------------------------------------------------
 */
const ProtocolRoutine *
GetProtocolRoutine(CompatibilityProtocolKind kind)
{
	if (!CompatibilityProtocolKindIsValid(kind))
		return NULL;

	{
		const CompatibilityRoutine *compat = GetCompatibilityRoutine(kind);

		if (compat != NULL && compat->protocol != NULL)
			return compat->protocol;
	}

	/* The standard protocol is a kernel fallback, not a module contract. */
	return kind == COMPAT_PROTOCOL_POSTGRES ? &StandardProtocolRoutine : NULL;
}

/* ----------------------------------------------------------------
 *    CompatibilityProtocolKindIsValid
 * ----------------------------------------------------------------
 */
bool
CompatibilityProtocolKindIsValid(CompatibilityProtocolKind kind)
{
    return (kind >= COMPAT_PROTOCOL_POSTGRES &&
            kind < COMPAT_PROTOCOL_KIND_MAX);
}

/*
 * _PG_init  –  register the built-in PG protocol when the backend image is
 * initialized.  AssignProtocolRoutine() retains the lazy fallback for
 * statically-linked standalone callers that do not invoke this entry point.
 */
void
_PG_init(void)
{
    RegisterProtocolRoutine(&StandardProtocolRoutine);
}
