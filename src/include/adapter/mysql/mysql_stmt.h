/*-------------------------------------------------------------------------
 *
 * mysql_stmt.h
 *    Server-side prepared statement support for the MySQL wire protocol.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYSQL_STMT_H
#define MYSQL_STMT_H

#include "lib/stringinfo.h"

struct MysPacketState;

extern void mysql_stmt_prepare(struct MysPacketState *ps, StringInfo inBuf);
extern void mysql_stmt_execute(struct MysPacketState *ps, StringInfo inBuf);
extern void mysql_stmt_close(struct MysPacketState *ps, StringInfo inBuf);
extern void mysql_stmt_reset(struct MysPacketState *ps, StringInfo inBuf);
extern void mysql_stmt_send_long_data(struct MysPacketState *ps,
                                      StringInfo inBuf);
extern void mysql_stmt_fetch(struct MysPacketState *ps, StringInfo inBuf);

#endif								/* MYSQL_STMT_H */
