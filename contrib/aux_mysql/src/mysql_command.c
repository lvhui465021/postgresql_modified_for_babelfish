/*-------------------------------------------------------------------------
 *
 * mysql_command.c
 *    MySQL COM command read and dispatch for the ProtocolRoutine
 *    read_command / process_command callbacks.
 *
 * The read_command callback reads a COM packet off the wire and converts
 * it into a pseudo "firstchar" value that the PostgresMain switch loop
 * already understands (PqMsg_Query / PqMsg_Quit / …).  This avoids
 * rewriting the main loop for MySQL.
 *
 * COM_QUERY and COM_INIT_DB are mapped to PqMsg_Query after prepending
 * appropriate lowering.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/adapter/mysql/mysql_command.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "adapter/mysql/mysql_command.h"
#include "adapter/mysql/mysql_packet.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "postmaster/protocol_routine.h"
#include "utils/elog.h"

#include <string.h>

/* ----------------------------------------------------------------
 *    mysql_read_command
 *
 * Read one MySQL COM packet and convert it into a pseudo PG wire
 * message type stored in *command, with text payload in inBuf.
 *
 * Returns the byte count of the first packet payload, or -1 on EOF.
 * ----------------------------------------------------------------
 */
int
mysql_read_command(MysPacketState *ps, int *command, StringInfo inBuf)
{
    char       *payload;
    size_t      len;
    uint8       com;

    if (!mysql_packet_read(ps, &payload, &len))
        return -1;

    if (len < 1)
    {
        /* Empty packet — treat as an empty COM_QUERY. */
        pfree(payload);
        *command = MYSQL_PSEUDO_QUERY;
        resetStringInfo(inBuf);
        return 0;
    }

    com = (uint8) payload[0];

    switch (com)
    {
    case COM_QUERY:
        *command = MYSQL_PSEUDO_QUERY;
        /*
         * Pass the SQL text (after the 1-byte command code).
         * The PG protocol expects a NUL-terminated string and pq_getmsgend()
         * checks cursor == len after pq_getmsgstring() reads it.  We must
         * include the NUL in the length so that the end-of-message check
         * succeeds.
         */
        resetStringInfo(inBuf);
        if (len > 1)
        {
            /*
             * Pass the SQL text through unchanged.  mysql_process_command()
             * handles init probes (SELECT 1, @@version_comment) inline with
             * pre-built MySQL packets.  DestReceiver path will be fixed
             * separately for arbitrary queries.
             */
            appendBinaryStringInfo(inBuf, payload + 1, (int)(len - 1));
        }
        /* Include the trailing NUL in the buffer length. */
        enlargeStringInfo(inBuf, 1);
        inBuf->data[inBuf->len] = '\0';
        inBuf->len++;
        break;

    case COM_INIT_DB:
        /* COM_INIT_DB maps non-backend names to a MySQL USE/schema query. */
        resetStringInfo(inBuf);
        if (len > 1)
        {
            char *dbname = pnstrdup(payload + 1, (int)(len - 1));

            /*
             * MySQL connections are backed by a fixed PostgreSQL database.
             * Re-selecting that database is a successful no-op; lowering it
             * to USE would instead put a nonexistent schema before public
             * and make pg_catalog the first usable creation namespace.
             */
            if (strcmp(dbname, MyProcPort->database_name) == 0)
            {
                *command = MYSQL_PSEUDO_PING;
                pfree(dbname);
                break;
            }
            *command = MYSQL_PSEUDO_QUERY;
            appendStringInfo(inBuf, "USE `%s`", dbname);
            pfree(dbname);
        }
        else
            *command = MYSQL_PSEUDO_QUERY;
        /* PqMsg_Query consumes a NUL-terminated string inside message len. */
        enlargeStringInfo(inBuf, 1);
        inBuf->data[inBuf->len] = '\0';
        inBuf->len++;
        break;

    case COM_QUIT:
        *command = MYSQL_PSEUDO_QUIT;
        resetStringInfo(inBuf);
        break;

    case COM_PING:
        *command = MYSQL_PSEUDO_PING;
        resetStringInfo(inBuf);
        break;

    case COM_STMT_PREPARE:
        *command = MYSQL_PSEUDO_STMT_PREPARE;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_STMT_EXECUTE:
        *command = MYSQL_PSEUDO_STMT_EXECUTE;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_STMT_CLOSE:
        *command = MYSQL_PSEUDO_STMT_CLOSE;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_STMT_RESET:
        *command = MYSQL_PSEUDO_STMT_RESET;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_STMT_SEND_LONG_DATA:
        *command = MYSQL_PSEUDO_STMT_SEND_LONG_DATA;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_STMT_FETCH:
        *command = MYSQL_PSEUDO_STMT_FETCH;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_SET_OPTION:
        *command = MYSQL_PSEUDO_SET_OPTION;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    case COM_FIELD_LIST:
        *command = MYSQL_PSEUDO_FIELD_LIST;
        resetStringInfo(inBuf);
        if (len > 1)
            appendBinaryStringInfo(inBuf, payload + 1, (int) (len - 1));
        break;

    default:
        ereport(COMMERROR,
                (errmsg("unsupported MySQL COM command 0x%02x", com)));
        pfree(payload);
        return -1;
    }

    pfree(payload);
    return (int) len;
}
