/*-------------------------------------------------------------------------
 *
 * protocol_extension.h
 *	  Exports and definitions for Loadable Protocol Extensions
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/protocol_extension.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROTOCOL_EXTENSION_H
#define _PROTOCOL_EXTENSION_H

#include "libpq/libpq.h"

/* Functions in postmaster.c */
extern PGDLLEXPORT int	listen_have_free_slot(void);
extern PGDLLEXPORT void	listen_add_socket(pgsocket fd);

/*
 * listen_add_protocol_socket -- like listen_add_socket(), but also stamps
 * the new socket's CompatibilityProtocolKind so that ServerLoop's accept
 * dispatch (see postmaster.c) sets Port->protocol_kind correctly for
 * connections accepted on it.  The accept/close callbacks for the socket
 * are resolved from the ProtocolRoutine registered for kind
 * (see protocol_routine.h).
 */
extern PGDLLEXPORT void	listen_add_protocol_socket(pgsocket fd,
								CompatibilityProtocolKind kind);

#endif							/* _PROTOCOL_EXTENSION_H */
