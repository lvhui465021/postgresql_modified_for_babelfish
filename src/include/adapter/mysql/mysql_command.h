/*-------------------------------------------------------------------------
 *
 * mysql_command.h
 *    MySQL COM command dispatch for the read-command / process-command
 *    ProtocolRoutine callbacks.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/adapter/mysql/mysql_command.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYSQL_COMMAND_H
#define MYSQL_COMMAND_H

#include "libpq/libpq-be.h"

/* MySQL COM command codes (subset used in M1). */
#define COM_SLEEP               0x00
#define COM_QUIT                0x01
#define COM_INIT_DB             0x02
#define COM_QUERY               0x03
#define COM_FIELD_LIST          0x04
#define COM_PING                0x0E
#define COM_STMT_PREPARE        0x16
#define COM_STMT_EXECUTE        0x17
#define COM_STMT_SEND_LONG_DATA 0x18
#define COM_STMT_CLOSE          0x19
#define COM_STMT_RESET          0x1A
#define COM_SET_OPTION          0x1B
#define COM_STMT_FETCH          0x1C

/* Internal representation passed from read_command to process_command. */
#define MYSQL_PSEUDO_QUERY      0x03    /* mapped to PqMsg_Query   */
#define MYSQL_PSEUDO_QUIT       'X'     /* mapped to PqMsg_Quit   */
#define MYSQL_PSEUDO_PING       'Y'     /* immediate OK response  */
#define MYSQL_PSEUDO_STMT_PREPARE 'p'
#define MYSQL_PSEUDO_STMT_EXECUTE 'e'
#define MYSQL_PSEUDO_STMT_CLOSE   'c'
#define MYSQL_PSEUDO_STMT_RESET   'r'
#define MYSQL_PSEUDO_STMT_SEND_LONG_DATA 'l'
#define MYSQL_PSEUDO_STMT_FETCH   'f'
#define MYSQL_PSEUDO_FIELD_LIST   'F'
#define MYSQL_PSEUDO_SET_OPTION   'O'

struct MysPacketState;

/*
 * Read one MySQL command from the socket, assembling split packets.
 * The decoded command is written to *command and the text (if any) is
 * placed in inBuf.  Returns the packet sequence number of the first
 * packet, or -1 on EOF.
 */
extern int  mysql_read_command(struct MysPacketState *ps,
                               int *command, StringInfo inBuf);

#endif   /* MYSQL_COMMAND_H */
