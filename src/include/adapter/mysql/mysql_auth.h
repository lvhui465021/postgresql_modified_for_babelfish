/*-------------------------------------------------------------------------
 *
 * mysql_auth.h
 *    MySQL handshake (greeting) and mysql_native_password authentication.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/adapter/mysql/mysql_auth.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYSQL_AUTH_H
#define MYSQL_AUTH_H

#include "libpq/libpq-be.h"

struct MysPacketState;

/*
 * Send the MySQL protocol-10 greeting packet.
 *
 * The 20-byte scramble is generated randomly per-connection and stored
 * in port->protocol_state for the subsequent login-packet verification.
 */
extern void mysql_send_greeting(struct MysPacketState *ps, Port *port);

/*
 * Read and verify a MySQL login (handshake response) packet.
 *
 * On success the caller must set:
 *   port->user_name              (from the login packet)
 *   port->compat_database_name   (from the login packet schema, may be NULL)
 *
 * HBA / role lookup happens inside this function; the outcome is returned
 * as STATUS_OK or STATUS_ERROR.
 */
extern int  mysql_verify_login(struct MysPacketState *ps, Port *port);
extern void mysql_perform_authentication(struct MysPacketState *ps, Port *port);

#endif   /* MYSQL_AUTH_H */
