/*-------------------------------------------------------------------------
 *
 * mysql_packet.c
 *    MySQL packet-layer I/O: header read/write, sequence tracking,
 *    multi-packet reassembly, and ERR-packet formatting.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/adapter/mysql/mysql_packet.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "adapter/mysql/mysql_packet.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include <errno.h>
#include <stdarg.h>

/* ----------------------------------------------------------------
 *    MysPacketState
 * ----------------------------------------------------------------
 */
struct MysPacketState
{
    Port       *port;               /* MyProcPort (socket, secure_read/write) */
    MemoryContext memctx;           /* connection-lifetime memory context    */
    uint8       seq;                /* next expected client sequence number    */
    uint8       server_seq;         /* next server sequence number to send     */
    void       *auth_state;         /* MysAuthState during handshake, else NULL */
    uint32      client_capabilities; /* client capability flags from login     */
    uint32      client_max_packet_size; /* client's declared max_packet_size,
                                          * 0 if the client didn't send one or
                                          * declared no limit               */
    uint64      found_rows;         /* FOUND_ROWS() session counter            */
    bool        result_set_started;  /* true once column metadata has been sent */
    uint64      last_insert_id;     /* LAST_INSERT_ID() session value          */
    uint64      row_count;          /* ROW_COUNT() session value (-1 = unset)  */
    MysWarning *warnings;           /* suppressed NOTICE/WARNING/INFO list     */
    int         nwarnings;          /* entries in warnings[]                   */
    int         warnings_cap;       /* allocated size of warnings[]            */
    bool        stmt_had_warnings;  /* current statement produced >= 1 warning */
};

/* ----------------------------------------------------------------
 *    Lifecycle
 * ----------------------------------------------------------------
 */
MysPacketState *
mysql_packet_create(Port *port)
{
    MysPacketState *ps;

    ps = (MysPacketState *) palloc0(sizeof(MysPacketState));
    ps->port = port;
    /*
     * The packet state outlives individual statements (SHOW WARNINGS reads
     * diagnostics from an earlier statement), so keep connection-lifetime
     * allocations in the context that created it rather than in a
     * statement-scoped context.
     */
    ps->memctx = CurrentMemoryContext;
    ps->seq = 0;           /* client starts at seq 0 after server greeting   */
    ps->server_seq = 0;    /* greeting is seq 0                              */

    return ps;
}

void
mysql_packet_free(MysPacketState *ps)
{
    if (ps != NULL)
    {
        mysql_packet_reset_warning_count(ps);
        pfree(ps);
    }
}

/* ----------------------------------------------------------------
 *    Low-level read
 * ----------------------------------------------------------------
 */
bool
mysql_packet_read(MysPacketState *ps, char **payload, size_t *len)
{
    uint8       header[MYSQL_PACKET_HEADER_SIZE];
    ssize_t     nread;
    size_t      bytes_read;
    uint32      payload_len;
    uint8       pkt_seq;
    char       *buf = NULL;
    size_t      total_len = 0;
    Port       *port = ps->port;

    /*
     * A logical MySQL message is split into 0xffffff-byte packets.  The
     * terminating packet is shorter than that limit; an exact multiple is
     * terminated by an empty packet.  Keep the packet sequence advancing for
     * every fragment, but return one contiguous command payload to callers.
     */
    for (;;)
    {
        /* secure_read() is allowed to return a short read. */
        bytes_read = 0;
        while (bytes_read < MYSQL_PACKET_HEADER_SIZE)
        {
            do {
                nread = secure_read(port, header + bytes_read,
                                    MYSQL_PACKET_HEADER_SIZE - bytes_read);
            } while (nread < 0 && (errno == EINTR || errno == EAGAIN));
            if (nread > 0)
            {
                bytes_read += (size_t) nread;
                continue;
            }
            if (nread < 0)
                ereport(COMMERROR,
                        (errcode_for_socket_access(),
                         errmsg("could not read MySQL packet header: %m")));
            if (buf != NULL)
                pfree(buf);
            return false;
        }

        payload_len = (uint32) header[0]
                    | ((uint32) header[1] << 8)
                    | ((uint32) header[2] << 16);
        pkt_seq = header[3];

        if (pkt_seq != ps->seq)
        {
            ereport(COMMERROR,
                    (errmsg("MySQL packet sequence number mismatch: expected %u, got %u",
                            ps->seq, pkt_seq)));
            if (buf != NULL)
                pfree(buf);
            return false;
        }
        ps->seq = (ps->seq + 1) & 0xFF;

        if ((size_t) payload_len > MaxAllocSize - total_len - 1)
        {
            ereport(COMMERROR,
                    (errmsg("MySQL packet payload is too large")));
            if (buf != NULL)
                pfree(buf);
            return false;
        }
        if (total_len + (size_t) payload_len > (size_t) mysql_max_allowed_packet)
        {
            ereport(COMMERROR,
                    (errmsg("MySQL packet payload exceeds mysql_max_allowed_packet (%d bytes)",
                            mysql_max_allowed_packet)));
            if (buf != NULL)
                pfree(buf);

            /*
             * Tell the client why we're about to hang up instead of just
             * dropping the connection: real MySQL servers respond to an
             * over-limit packet with ER_NET_PACKET_TOO_LARGE (1153) before
             * closing, so well-behaved drivers can distinguish "packet too
             * large" from a generic lost connection instead of retrying a
             * doomed request.  The header for this fragment was already
             * validated and consumed above, so ps->seq is the correct next
             * sequence number for our response.
             */
            ps->server_seq = ps->seq;
            mysql_packet_write_err(ps, 1153, "08S01",
                                   "Got a packet bigger than 'max_allowed_packet' bytes");
            return false;
        }
        if (buf == NULL)
            buf = palloc((size_t) payload_len + 1);
        else
            buf = repalloc(buf, total_len + (size_t) payload_len + 1);

        bytes_read = 0;
        while (bytes_read < payload_len)
        {
            do {
                nread = secure_read(port, buf + total_len + bytes_read,
                                    payload_len - bytes_read);
            } while (nread < 0 && (errno == EINTR || errno == EAGAIN));
            if (nread > 0)
            {
                bytes_read += (size_t) nread;
                continue;
            }

            pfree(buf);
            if (nread < 0)
                ereport(COMMERROR,
                        (errcode_for_socket_access(),
                         errmsg("could not read MySQL packet payload: %m")));
            return false;
        }
        total_len += payload_len;

        if (payload_len < MYSQL_MAX_PAYLOAD_LENGTH)
            break;
    }

    /* The server response continues the same command packet sequence. */
    ps->server_seq = ps->seq;
    buf[total_len] = '\0';
    *payload = buf;
    *len = total_len;
    return true;
}

/* ----------------------------------------------------------------
 *    Low-level write
 * ----------------------------------------------------------------
 */
static void
mysql_write_raw(MysPacketState *ps, const char *payload, size_t len)
{
    uint8       header[MYSQL_PACKET_HEADER_SIZE];
    ssize_t     written;
    size_t      bytes_written;
    Port       *port = ps->port;

    header[0] = (uint8) (len & 0xFF);
    header[1] = (uint8) ((len >> 8) & 0xFF);
    header[2] = (uint8) ((len >> 16) & 0xFF);
    header[3] = ps->server_seq;

    ps->server_seq = (ps->server_seq + 1) & 0xFF;

    bytes_written = 0;
    while (bytes_written < MYSQL_PACKET_HEADER_SIZE)
    {
        do {
            written = secure_write(port, header + bytes_written,
                                   MYSQL_PACKET_HEADER_SIZE - bytes_written);
        } while (written < 0 && (errno == EINTR || errno == EAGAIN));
        if (written > 0)
        {
            bytes_written += (size_t) written;
            continue;
        }
        goto write_fail;
    }

    if (len > 0)
    {
        bytes_written = 0;
        while (bytes_written < len)
        {
            do {
                written = secure_write(port, payload + bytes_written,
                                       len - bytes_written);
            } while (written < 0 && (errno == EINTR || errno == EAGAIN));
            if (written > 0)
            {
                bytes_written += (size_t) written;
                continue;
            }
            goto write_fail;
        }
    }

    return;

write_fail:
    ereport(COMMERROR,
            (errcode_for_socket_access(),
             errmsg("could not write to MySQL client: %m")));
    /* The caller is expected to handle the dead connection. */
}

void
mysql_packet_write(MysPacketState *ps, const char *payload, size_t len)
{
    /*
     * Mirror mysql_packet_read(): every 0xffffff-byte fragment gets its own
     * header/sequence number, and an exact multiple is followed by an empty
     * terminator packet.
     */
    while (len >= MYSQL_MAX_PAYLOAD_LENGTH)
    {
        mysql_write_raw(ps, payload, MYSQL_MAX_PAYLOAD_LENGTH);
        payload += MYSQL_MAX_PAYLOAD_LENGTH;
        len -= MYSQL_MAX_PAYLOAD_LENGTH;
    }
    mysql_write_raw(ps, payload, len);
}

void
mysql_packet_write_ok(MysPacketState *ps,
                      const char *payload, size_t len,
                      uint8 header_byte)
{
    /*
     * Send the payload as a single MySQL packet.  The caller is responsible
     * for including the OK/ERR/EOF header byte (0x00 / 0xFF / 0xFE) as the
     * first byte of payload.  The header_byte parameter is informational
     * (unused in the framing itself) — kept for API compatibility.
     */
    (void) header_byte;
    mysql_packet_write(ps, payload, len);
}

void
mysql_packet_write_err(MysPacketState *ps,
                       uint16 errcode,
                       const char *sqlstate,
                       const char *fmt, ...)
{
    /*
     * Build a MySQL ERR packet:
     *   1 byte  header (0xFF)
     *   2 bytes error code (little-endian)
     *   1 byte  SQLSTATE marker '#'
     *   5 bytes SQLSTATE string (e.g. "28000")
     *   N bytes human-readable message (no trailing NUL, but we include one
     *     for ease of construction and subtract it at the end.)
     */
    char        msgbuf[512];
    va_list     args;
    int         msglen;
    size_t      payload_len;
    char       *payload;
    int         pos = 0;

    va_start(args, fmt);
    msglen = vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);

    /*
     * vsnprintf() returns the length the message *would* have had if
     * msgbuf were unbounded (or a negative value on encoding failure) --
     * not the number of bytes actually written into msgbuf.  Clamp to
     * what's actually there before treating it as a length to copy out of
     * msgbuf below, or this reads adjacent stack memory into the packet
     * sent to the client.
     */
    if (msglen < 0)
        msglen = 0;
    else if (msglen >= (int) sizeof(msgbuf))
        msglen = sizeof(msgbuf) - 1;

    /*
     * MySQL ERR packet payload:
     *   1 byte  header (0xFF)
     *   2 bytes error code (little-endian)
     *   1 byte  SQLSTATE marker '#'
     *   5 bytes SQLSTATE string (e.g. "28000")
     *   N bytes human-readable message (no trailing NUL)
     *
     * Note: mysql_packet_write_ok() ignores its header_byte parameter,
     * so the 0xFF marker MUST be embedded in the payload here.  EOF (0xFE)
     * and OK (0x00) packets follow the same convention — the caller bakes
     * the header into the payload.
     */
    payload_len = 1 + 2 + 1 + 5 + (size_t) msglen;
    payload = (char *) palloc(payload_len);

    payload[pos++] = (char) 0xFF;   /* ERR marker */
    payload[pos++] = (char) (errcode & 0xFF);
    payload[pos++] = (char) ((errcode >> 8) & 0xFF);
    payload[pos++] = '#';
    if (sqlstate != NULL)
    {
        memcpy(payload + pos, sqlstate, 5);
        pos += 5;
    }
    else
    {
        memset(payload + pos, ' ', 5);
        pos += 5;
    }
    memcpy(payload + pos, msgbuf, (size_t) msglen);
    pos += msglen;

    mysql_packet_write_ok(ps, payload, (size_t) pos, 0xFF);
    pfree(payload);
}

void
mysql_packet_reset_seq(MysPacketState *ps)
{
    if (ps != NULL)
    {
        ps->seq = 0;
        ps->server_seq = 0;
    }
}

void
mysql_packet_set_seq(MysPacketState *ps, uint8 seq)
{
	if (ps != NULL)
		ps->seq = seq;
}

void
mysql_packet_set_server_seq(MysPacketState *ps, uint8 seq)
{
    if (ps != NULL)
        ps->server_seq = seq;
}

void
mysql_packet_set_auth_state(MysPacketState *ps, void *state)
{
    if (ps != NULL)
        ps->auth_state = state;
}

void *
mysql_packet_get_auth_state(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->auth_state;
    return NULL;
}

void
mysql_packet_set_client_caps(MysPacketState *ps, uint32 caps)
{
    if (ps != NULL)
        ps->client_capabilities = caps;
}

uint32
mysql_packet_get_client_caps(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->client_capabilities;
    return 0;
}

void
mysql_packet_set_max_packet_size(MysPacketState *ps, uint32 size)
{
    if (ps != NULL)
        ps->client_max_packet_size = size;
}

uint32
mysql_packet_get_max_packet_size(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->client_max_packet_size;
    return 0;
}

void
mysql_packet_set_found_rows(MysPacketState *ps, uint64 count)
{
    if (ps != NULL)
        ps->found_rows = count;
}

uint64
mysql_packet_get_found_rows(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->found_rows;
    return 0;
}

void
mysql_packet_set_result_started(MysPacketState *ps, bool started)
{
    if (ps != NULL)
        ps->result_set_started = started;
}

bool
mysql_packet_get_result_started(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->result_set_started;
    return false;
}

void
mysql_packet_set_last_insert_id(MysPacketState *ps, uint64 value)
{
    if (ps != NULL)
        ps->last_insert_id = value;
}

uint64
mysql_packet_get_last_insert_id(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->last_insert_id;
    return 0;
}

void
mysql_packet_set_row_count(MysPacketState *ps, uint64 count)
{
    if (ps != NULL)
        ps->row_count = count;
}

uint64
mysql_packet_get_row_count(MysPacketState *ps)
{
    if (ps != NULL)
        return ps->row_count;
    return 0;
}

void
mysql_packet_add_warning(MysPacketState *ps, int elevel, uint16 errcode,
                         const char *sqlstate, const char *message)
{
    MysWarning *w;
    MemoryContext oldctx;

    if (ps == NULL)
        return;

    /*
     * The first diagnostic of a statement replaces the previous statement's
     * retained list -- MySQL's per-statement warning set is overwritten by
     * the next statement that produces diagnostics, not appended to it.
     */
    if (!ps->stmt_had_warnings)
    {
        mysql_packet_reset_warning_count(ps);
        ps->stmt_had_warnings = true;
    }

    if (ps->nwarnings >= ps->warnings_cap)
    {
        int         new_cap = ps->warnings_cap == 0 ? 8 : ps->warnings_cap * 2;

        oldctx = MemoryContextSwitchTo(ps->memctx);
        /*
         * repalloc() requires an already-palloc'd pointer -- unlike libc
         * realloc(), it cannot take NULL, and ps->warnings is still NULL
         * the first time any warning is ever added on this connection
         * (palloc0'd by mysql_packet_create()).  Use palloc for that first
         * allocation and repalloc only to grow an existing buffer.
         */
        if (ps->warnings == NULL)
            ps->warnings = (MysWarning *) palloc(sizeof(MysWarning) * new_cap);
        else
            ps->warnings = (MysWarning *) repalloc(ps->warnings,
                                                   sizeof(MysWarning) * new_cap);
        ps->warnings_cap = new_cap;
        MemoryContextSwitchTo(oldctx);
    }
    w = &ps->warnings[ps->nwarnings];
    w->level = elevel;
    w->errcode = errcode;
    if (sqlstate != NULL)
        memcpy(w->sqlstate, sqlstate, 5);
    else
        memset(w->sqlstate, ' ', 5);
    w->sqlstate[5] = '\0';
    /*
     * The list outlives the statement that produced it (SHOW WARNINGS may
     * read it several statements later), so copy the text into the
     * connection-lifetime memory context rather than the statement's
     * transient context.
     */
    oldctx = MemoryContextSwitchTo(ps->memctx);
    w->message = pstrdup(message != NULL ? message : "");
    MemoryContextSwitchTo(oldctx);
    ps->nwarnings++;
}

uint32
mysql_packet_get_warning_count(MysPacketState *ps)
{
    if (ps != NULL)
        return (uint32) ps->nwarnings;
    return 0;
}

int
mysql_packet_get_warnings(MysPacketState *ps, MysWarning **warnings)
{
    if (ps != NULL)
    {
        *warnings = ps->warnings;
        return ps->nwarnings;
    }
    *warnings = NULL;
    return 0;
}

void
mysql_packet_reset_warning_count(MysPacketState *ps)
{
    int         i;

    if (ps == NULL)
        return;
    for (i = 0; i < ps->nwarnings; i++)
        if (ps->warnings[i].message != NULL)
            pfree(ps->warnings[i].message);
    ps->nwarnings = 0;
    ps->stmt_had_warnings = false;
}

void
mysql_packet_reset_stmt_warning_state(MysPacketState *ps)
{
    if (ps != NULL)
        ps->stmt_had_warnings = false;
}
