/*-------------------------------------------------------------------------
 *
 * mysql_packet.h
 *    MySQL packet-layer I/O for the compatibility adapter.
 *
 * The MySQL wire protocol wraps every payload in a 4-byte header:
 *    [3 bytes little-endian payload length][1 byte sequence number]
 *
 * Sequence numbers start at 0 for the greeting and increment per-packet
 * on both the client→server and server→client directions independently.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/adapter/mysql/mysql_packet.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYSQL_PACKET_H
#define MYSQL_PACKET_H

#include "libpq/libpq-be.h"

/* Maximum payload bytes in a single MySQL packet (2^24 - 1). */
#define MYSQL_PACKET_HEADER_SIZE     4
#define MYSQL_MAX_PAYLOAD_LENGTH     ((1 << 24) - 1)

/*
 * Opaque handle for MySQL packet-layer state (sequence counter, write buffer).
 */
typedef struct MysPacketState MysPacketState;

/* Create a packet-layer handle bound to the given Port. */
extern MysPacketState *mysql_packet_create(Port *port);

/* Release all resources without flushing. */
extern void mysql_packet_free(MysPacketState *ps);

/* --- raw I/O (used during the startup exchange) -------------------- */

/* Read one complete MySQL message into a freshly palloc'd buffer, reassembling
 * any 0xffffff-byte protocol fragments.
 * *payload receives the buffer, *len the number of payload bytes.
 * Returns true on success; on EOF or protocol error the caller must
 * close the connection (we ereport a SQL-facing error). */
extern bool mysql_packet_read(MysPacketState *ps,
                              char **payload, size_t *len);

/* Write one logical message, splitting it into protocol packets as needed. */
extern void mysql_packet_write(MysPacketState *ps,
                               const char *payload, size_t len);

/* Write + flush an OK/EOF packet (header byte 0x00 or 0xFE). */
extern void mysql_packet_write_ok(MysPacketState *ps,
                                  const char *payload, size_t len,
                                  uint8 header);

/* Write + flush an ERR packet (header byte 0xFF). */
extern void mysql_packet_write_err(MysPacketState *ps,
                                   uint16 errcode,
                                   const char *sqlstate,
                                   const char *fmt, ...)
            pg_attribute_printf(4, 5);

/* Reset client sequence-number tracking (called on error recovery). */
extern void mysql_packet_reset_seq(MysPacketState *ps);

/* Set the expected client sequence number (used after greeting). */
extern void mysql_packet_set_seq(MysPacketState *ps, uint8 seq);

/* Set the server sequence number for the next outgoing packet. */
extern void mysql_packet_set_server_seq(MysPacketState *ps, uint8 seq);

/* Store / retrieve opaque per-connection auth state. */
extern void mysql_packet_set_auth_state(MysPacketState *ps, void *state);
extern void *mysql_packet_get_auth_state(MysPacketState *ps);

/* Store / retrieve client capability flags (from login packet). */
extern void mysql_packet_set_client_caps(MysPacketState *ps, uint32 caps);
extern uint32 mysql_packet_get_client_caps(MysPacketState *ps);

/* Store / retrieve FOUND_ROWS() session counter. */
extern void mysql_packet_set_found_rows(MysPacketState *ps, uint64 count);
extern uint64 mysql_packet_get_found_rows(MysPacketState *ps);

/* Track whether a result set has been started (column metadata sent). */
extern void mysql_packet_set_result_started(MysPacketState *ps, bool started);
extern bool mysql_packet_get_result_started(MysPacketState *ps);

/* Store / retrieve MySQL connection-local LAST_INSERT_ID() and ROW_COUNT(). */
extern void mysql_packet_set_last_insert_id(MysPacketState *ps, uint64 value);
extern uint64 mysql_packet_get_last_insert_id(MysPacketState *ps);
extern void mysql_packet_set_row_count(MysPacketState *ps, uint64 count);
extern uint64 mysql_packet_get_row_count(MysPacketState *ps);

/*
 * MySQL capability flags (subset relevant to our adapter).
 * Full list: https://dev.mysql.com/doc/dev/mysql-server/latest/group__group__cs__capabilities__flags.html
 */
#define MYSQL_CAP_LONG_PASSWORD              0x00000001
#define MYSQL_CAP_FOUND_ROWS                 0x00000002
#define MYSQL_CAP_LONG_FLAG                  0x00000004
#define MYSQL_CAP_CONNECT_WITH_DB            0x00000008
#define MYSQL_CAP_PROTOCOL_41                0x00000200
#define MYSQL_CAP_TRANSACTIONS               0x00002000
#define MYSQL_CAP_MULTI_STATEMENTS           0x00010000
#define MYSQL_CAP_MULTI_RESULTS              0x00020000
#define MYSQL_CAP_PS_MULTI_RESULTS           0x00040000
#define MYSQL_CAP_SECURE_CONNECTION          0x00008000
#define MYSQL_CAP_PLUGIN_AUTH                0x00080000
#define MYSQL_CAP_PLUGIN_AUTH_LENENC         0x00200000
#define MYSQL_CAP_OPTIONAL_RESULTSET_METADATA 0x00400000
#define MYSQL_CAP_SESSION_TRACK              0x00800000
#define MYSQL_CAP_DEPRECATE_EOF              0x01000000

/*
 * Server advertised capabilities — the feature set our server promises to
 * support.  Protocol decisions (DeprecateEOF, OptionalResultsetMetadata,
 * SessionTrack, etc.) MUST use negotiated_caps = server_caps & client_caps,
 * never the raw client capability flags.
 *
 * Kept in sync with the greeting built by mysql_auth.c.
 * See MYSQL_CAPABILITY_LO / MYSQL_CAPABILITY_HI in mysql_auth.c.
 */
#define MYSQL_SERVER_CAPABILITY \
	((uint32)(                                                             \
         MYSQL_CAP_LONG_PASSWORD       | MYSQL_CAP_FOUND_ROWS         |      \
         MYSQL_CAP_LONG_FLAG           | MYSQL_CAP_CONNECT_WITH_DB    |      \
         MYSQL_CAP_PROTOCOL_41         | MYSQL_CAP_TRANSACTIONS       |      \
         MYSQL_CAP_SECURE_CONNECTION | MYSQL_CAP_MULTI_STATEMENTS |      \
         MYSQL_CAP_MULTI_RESULTS | MYSQL_CAP_PS_MULTI_RESULTS |            \
         MYSQL_CAP_PLUGIN_AUTH         | MYSQL_CAP_PLUGIN_AUTH_LENENC |      \
	 MYSQL_CAP_OPTIONAL_RESULTSET_METADATA | MYSQL_CAP_DEPRECATE_EOF |   \
	 MYSQL_CAP_SESSION_TRACK))

/*
 * Return the negotiated capability set (server-advertised & client-requested).
 */
static inline uint32
mysql_negotiated_caps(MysPacketState *ps)
{
	return mysql_packet_get_client_caps(ps) & MYSQL_SERVER_CAPABILITY;
}

#endif   /* MYSQL_PACKET_H */
