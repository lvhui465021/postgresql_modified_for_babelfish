/*-------------------------------------------------------------------------
 *
 * mysql_auth.c
 *    MySQL protocol-10 greeting and mysql_native_password authentication.
 *
 * Greeting format (protocol version 10):
 *   1 byte   protocol version (10)
 *   N bytes  server version, NUL-terminated
 *   4 bytes  connection (thread) ID
 *   8 bytes  auth-plugin-data-part-1 (scramble prefix)
 *   1 byte   filler (0x00)
 *   2 bytes  capability flags (lower 16 bits)
 *   1 byte   character set (33 = utf8mb4)
 *   2 bytes  status flags
 *   2 bytes  capability flags (upper 16 bits)
 *   1 byte   length of auth-plugin-data (always 21 for mysql_native_password)
 *   10 bytes reserved (0x00)
 *   N bytes  auth-plugin-data-part-2 (scramble suffix, 12+1 for length=21)
 *   N bytes  auth-plugin name (NUL-terminated), if CLIENT_PLUGIN_AUTH set
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/adapter/mysql/mysql_auth.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "adapter/mysql/mysql_auth.h"
#include "adapter/mysql/mysql_packet.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/hba.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq.h"
#include "libpq/pqcomm.h"           /* pq_getmsgstring, etc. (may borrow)  */
#include "miscadmin.h"
#include "postmaster/protocol_routine.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "utils/elog.h"

#include <openssl/sha.h>

#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------
 *    Capability flags we advertise
 * ----------------------------------------------------------------
 */
#define CLIENT_LONG_PASSWORD            0x00000001
#define CLIENT_FOUND_ROWS               0x00000002
#define CLIENT_LONG_FLAG                0x00000004
#define CLIENT_CONNECT_WITH_DB          0x00000008
#define CLIENT_PROTOCOL_41              0x00000200
#define CLIENT_TRANSACTIONS             0x00002000
#define CLIENT_SECURE_CONNECTION        0x00008000
#define CLIENT_MULTI_STATEMENTS         0x00010000
#define CLIENT_MULTI_RESULTS            0x00020000
#define CLIENT_PS_MULTI_RESULTS         0x00040000
#define CLIENT_PLUGIN_AUTH              0x00080000
#define CLIENT_PLUGIN_AUTH_LENENC       0x00200000
#define CLIENT_OPTIONAL_RESULTSET_META  0x00400000
#define CLIENT_SESSION_TRACK            0x00800000
#define CLIENT_DEPRECATE_EOF            0x01000000

/*
 * Advertise protocol-41 with modern extensions.  We intentionally
 * keep this minimal (DEPRECATE_EOF only in HI word) so that protocol
 * decisions are driven by the server's own advertised set.  Client
 * capabilities are masked against this set in the DestReceiver and
 * end_command callbacks.
 */
#define MYSQL_CAPABILITY_LO   (uint16)(                             \
        CLIENT_LONG_PASSWORD  | CLIENT_FOUND_ROWS    |                 \
        CLIENT_LONG_FLAG      | CLIENT_CONNECT_WITH_DB |               \
        CLIENT_PROTOCOL_41    | CLIENT_TRANSACTIONS   |                \
        CLIENT_SECURE_CONNECTION)
#define MYSQL_CAPABILITY_HI   (uint16)(                             \
        (CLIENT_MULTI_STATEMENTS >> 16)      |                          \
        (CLIENT_MULTI_RESULTS >> 16)         |                          \
        (CLIENT_PS_MULTI_RESULTS >> 16)      |                          \
        (CLIENT_PLUGIN_AUTH >> 16)           |                          \
        (CLIENT_PLUGIN_AUTH_LENENC >> 16)    |                          \
	(CLIENT_OPTIONAL_RESULTSET_META >> 16) |                       \
        (CLIENT_DEPRECATE_EOF >> 16) | (CLIENT_SESSION_TRACK >> 16))

/* Scramble length: part1 = 8, part2 = 12, total = 20. */
#define MYSQL_SCRAMBLE_LEN          20
#define MYSQL_SCRAMBLE_PART1_LEN    8
#define MYSQL_SCRAMBLE_PART2_LEN    12
#define MYSQL_AUTH_PLUGIN_DATA_LEN  21   /* includes a trailing NUL */

/* Default utf8mb4 character set. */
#define MYSQL_CHARSET_UTF8MB4       45

/*
 * Per-connection auth state stored in port->protocol_state during the
 * login exchange.  Freed after authentication completes.
 */
typedef struct MysAuthState
{
    uint8       scramble[MYSQL_SCRAMBLE_LEN];   /* 20-byte random challenge */
    char        auth_plugin_data[MYSQL_AUTH_PLUGIN_DATA_LEN];  /* 8+13, NUL-padded */

    /*
     * Saved HandshakeResponse41 fields.  The phase-A exchange must not make
     * an authentication decision: HBA matching needs catalog access and is
     * therefore performed by mysql_perform_authentication() after
     * InitPostgres().
     */
    uint8       initial_auth_response[SHA256_DIGEST_LENGTH];
    size_t      initial_auth_response_len;
    char       *client_auth_plugin_name;

    /* AuthSwitchResponse for mysql_native_password, if one is required. */
    uint8       native_auth_response[MYSQL_SCRAMBLE_LEN];
    size_t      native_auth_response_len;
    /* port->user_name and port->compat_database_name already set. */
} MysAuthState;

/* ----------------------------------------------------------------
 *    Helper: generate random bytes
 * ----------------------------------------------------------------
 */
static void
generate_scramble(uint8 *buf, size_t len)
{
    /*
     * The mysql_native_password challenge-response scheme's security rests
     * on this nonce being unpredictable. random()/srandom() is not a
     * cryptographic PRNG -- it's a poor fit for a 20-byte value meant to
     * carry that much entropy. Use the same strong RNG the kernel itself
     * uses for the equivalent MD5/SCRAM/cancel-key values (libpq/auth.c,
     * auth-scram.c, postgres.c).
     */
    if (!pg_strong_random(buf, len))
        ereport(ERROR,
                (errmsg("could not generate random MySQL auth scramble")));

    /* We avoid NUL bytes so that the auth-plugin-data is clean. */
    for (size_t i = 0; i < len; i++)
    {
        while (buf[i] == 0x00)
        {
            if (!pg_strong_random(&buf[i], 1))
                ereport(ERROR,
                        (errmsg("could not generate random MySQL auth scramble")));
        }
    }
}

/* ----------------------------------------------------------------
 *    mysql_send_greeting
 *
 * Builds and sends the protocol-10 handshake packet.
 * Stores the scramble in port->protocol_state.
 * ----------------------------------------------------------------
 */
void
mysql_send_greeting(MysPacketState *ps, Port *port)
{
    MysAuthState *auth;
    char        payload[256];
    int         pos = 0;
    int         server_version_len;
    uint32      thread_id;
    uint16      cap_lo = MYSQL_CAPABILITY_LO;
    uint16      cap_hi = MYSQL_CAPABILITY_HI;
    uint32      capability;
    extern char *mysql_server_version;
    const char *server_version = mysql_server_version;
    const char *auth_plugin_name = "caching_sha2_password";
    int         auth_plugin_name_len;
    uint8       charset = MYSQL_CHARSET_UTF8MB4;
    uint16      status_flags = 0x0002;   /* SERVER_STATUS_AUTOCOMMIT */
    uint8       filler = 0x00;
    int         reserved_len = 10;

    /* Allocate per-connection auth state. */
    auth = (MysAuthState *) MemoryContextAllocZero(TopMemoryContext,
                                                    sizeof(MysAuthState));
    generate_scramble(auth->scramble, MYSQL_SCRAMBLE_LEN);

    /* Build auth_plugin_data: part1 (8 bytes) + NUL + part2 (12 bytes). */
    memcpy(auth->auth_plugin_data, auth->scramble, MYSQL_SCRAMBLE_PART1_LEN);
    auth->auth_plugin_data[MYSQL_SCRAMBLE_PART1_LEN] = '\0';
    memcpy(auth->auth_plugin_data + MYSQL_SCRAMBLE_PART1_LEN + 1,
           auth->scramble + MYSQL_SCRAMBLE_PART1_LEN,
           MYSQL_SCRAMBLE_PART2_LEN);

    /* Store auth state in the packet state — do NOT overwrite
     * port->protocol_state (which must remain the MysPacketState). */
    mysql_packet_set_auth_state(ps, (void *) auth);

    /* Assemble packet payload. */
    server_version_len = (int) strlen(server_version);
    auth_plugin_name_len = (int) strlen(auth_plugin_name);

    /*
     * payload is a fixed 256-byte stack buffer; every other field this
     * function writes into it is a small constant (auth-plugin-data,
     * capability/status flags, the fixed "caching_sha2_password" plugin
     * name, etc: well under 128 bytes total). mysql_server_version is the
     * one GUC-controlled, administrator-editable field going into this
     * buffer, with no length constraint of its own (it's a plain string
     * GUC) -- clamp it here unconditionally rather than rely on whoever
     * sets the GUC to keep it short.
     */
    if (server_version_len > (int) sizeof(payload) - 128)
        server_version_len = (int) sizeof(payload) - 128;
    thread_id = (uint32)(MyProcPid & 0xFFFFFFFF);

    capability = ((uint32) cap_hi << 16) | (uint32) cap_lo;

    /* Protocol version */
    payload[pos++] = 10;

    /* Server version + NUL */
    memcpy(payload + pos, server_version, server_version_len);
    pos += server_version_len;
    payload[pos++] = '\0';

    /*
     * Connection ID, capability, and status fields are all multi-byte
     * integers that the MySQL wire protocol mandates little-endian
     * regardless of server architecture -- write them byte-by-byte rather
     * than memcpy'ing the host's native representation (correct on
     * little-endian hosts like x86_64/aarch64, but wrong on a big-endian
     * one).
     */

    /* Connection ID */
    payload[pos++] = (char) (thread_id & 0xFF);
    payload[pos++] = (char) ((thread_id >> 8) & 0xFF);
    payload[pos++] = (char) ((thread_id >> 16) & 0xFF);
    payload[pos++] = (char) ((thread_id >> 24) & 0xFF);

    /* Auth-plugin-data part 1 */
    memcpy(payload + pos, auth->auth_plugin_data, MYSQL_SCRAMBLE_PART1_LEN);
    pos += MYSQL_SCRAMBLE_PART1_LEN;

    /* Filler */
    payload[pos++] = (char) filler;

    /* Capability flags (lower 16) */
    payload[pos++] = (char) (cap_lo & 0xFF);
    payload[pos++] = (char) ((cap_lo >> 8) & 0xFF);

    /* Character set */
    payload[pos++] = (char) charset;

    /* Status flags */
    payload[pos++] = (char) (status_flags & 0xFF);
    payload[pos++] = (char) ((status_flags >> 8) & 0xFF);

    /* Capability flags (upper 16) */
    payload[pos++] = (char) (cap_hi & 0xFF);
    payload[pos++] = (char) ((cap_hi >> 8) & 0xFF);

    /* Length of auth-plugin-data */
    payload[pos++] = (char) MYSQL_AUTH_PLUGIN_DATA_LEN;

    /* Reserved (10 bytes of 0x00) */
    memset(payload + pos, 0, reserved_len);
    pos += reserved_len;

    /* Auth-plugin-data part 2 (13 bytes: 12 scramble + 1 NUL for length=21) */
    memcpy(payload + pos,
           auth->auth_plugin_data + MYSQL_SCRAMBLE_PART1_LEN + 1,
           MYSQL_SCRAMBLE_PART2_LEN);
    pos += MYSQL_SCRAMBLE_PART2_LEN;
    /* auth_plugin_data_len=21 means part2 is 13 bytes (12 scramble + NUL) */
    payload[pos++] = '\0';

    /* Auth plugin name + NUL */
    if (capability & CLIENT_PLUGIN_AUTH)
    {
        memcpy(payload + pos, auth_plugin_name, auth_plugin_name_len);
        pos += auth_plugin_name_len;
        payload[pos++] = '\0';
    }

    mysql_packet_write(ps, payload, (size_t) pos);

    /*
     * After sending the greeting (seq 0), the client will respond at
     * seq 1.  Set ps->seq to 1 so mysql_packet_read accepts the login
     * packet without a sequence-number mismatch error.
     */
    mysql_packet_set_seq(ps, 1);
}

/* ----------------------------------------------------------------
 *    parse_client_capabilities
 *
 * Reads the 4-byte client capability word from the login packet.
 * Returns true if the packet contains CLIENT_PROTOCOL_41.
 * ----------------------------------------------------------------
 */
static bool
parse_client_capabilities(const char *payload, size_t len,
                          uint32 *capability, char **charset)
{
    if (len < 4)
        return false;
    *capability = ((uint32)(uint8)payload[0])
                | ((uint32)(uint8)payload[1] << 8)
                | ((uint32)(uint8)payload[2] << 16)
                | ((uint32)(uint8)payload[3] << 24);
    *charset = NULL;   /* not extracted separately for M1 */
    return true;
}

/* ----------------------------------------------------------------
 *    mysql_verify_login
 *
 * Read login packet, verify mysql_native_password response against
 * the challenge stored in port->protocol_state, perform HBA lookup,
 * and fill in port->user_name / port->compat_database_name.
 * ----------------------------------------------------------------
 */
int
mysql_verify_login(MysPacketState *ps, Port *port)
{
    MysAuthState *auth = (MysAuthState *) mysql_packet_get_auth_state(ps);
    char       *payload;
    size_t      len;
    uint32      client_cap;
    uint32      max_packet_size;
    char       *charset_ptr;
    const char *pos_ptr;
    size_t      remaining;
    const char *username = NULL;
    const char *auth_response = NULL;
    size_t      auth_response_len = 0;
    const char *schema_name = NULL;
    const char *auth_plugin_name = NULL;

    /* Read the login (handshake response) packet. */
    if (!mysql_packet_read(ps, &payload, &len))
    {
        ereport(COMMERROR,
                (errmsg("failed to read MySQL login packet")));
        return STATUS_ERROR;
    }

    if (len < 36)
    {
        ereport(COMMERROR,
                (errmsg("MySQL login packet too short (%zu bytes)", len)));
        pfree(payload);
        return STATUS_ERROR;
    }

    /*
     * The handshake sequence is shared: greeting=0, login=1, response=2.
     * After mysql_packet_read consumed the login packet (client seq 1),
     * set server_seq to 2 so that any ERR/OK packet written from this
     * function or from mysql_perform_authentication carries the correct
     * sequence number.
     */
    mysql_packet_set_server_seq(ps, 2);

    pos_ptr = payload;
    remaining = len;

    /* --- Parse client capability flags (4 bytes) --- */
    if (!parse_client_capabilities(pos_ptr, remaining, &client_cap, &charset_ptr))
    {
        pfree(payload);
        return STATUS_ERROR;
    }

    /* Store client capabilities so DestReceiver / end_command can consult them. */
    mysql_packet_set_client_caps(ps, client_cap);

    pos_ptr += 4;
    remaining -= 4;

    /* --- Max packet size (4 bytes) --- */
    if (remaining < 4) { pfree(payload); return STATUS_ERROR; }
    memcpy(&max_packet_size, pos_ptr, 4);
    pos_ptr += 4;
    remaining -= 4;

    /* --- Character set (1 byte) --- */
    if (remaining < 1) { pfree(payload); return STATUS_ERROR; }
    pos_ptr += 1;
    remaining -= 1;

    /* --- Reserved (23 bytes of 0x00) --- */
    if (remaining < 23) { pfree(payload); return STATUS_ERROR; }
    pos_ptr += 23;
    remaining -= 23;

    /* --- Username (NUL-terminated) --- */
    username = pos_ptr;
    {
        size_t ulen = strnlen(pos_ptr, remaining);
        if (ulen >= remaining) { pfree(payload); return STATUS_ERROR; }
        pos_ptr += ulen + 1;
        remaining -= ulen + 1;
    }

    /* --- Auth response (length-encoded) --- */
    if (remaining < 1) { pfree(payload); return STATUS_ERROR; }
    {
        uint8 auth_len = (uint8) *pos_ptr;
        pos_ptr += 1;
        remaining -= 1;
        if (remaining < auth_len) { pfree(payload); return STATUS_ERROR; }
        auth_response = pos_ptr;
        auth_response_len = auth_len;
        pos_ptr += auth_len;
        remaining -= auth_len;
    }

    /* --- Schema name (NUL-terminated, if CLIENT_CONNECT_WITH_DB) --- */
    if ((client_cap & CLIENT_CONNECT_WITH_DB) && remaining > 0)
    {
        size_t slen = strnlen(pos_ptr, remaining);
        if (slen > 0 && slen < remaining)
        {
            schema_name = pos_ptr;
            pos_ptr += slen + 1;
            remaining -= slen + 1;
        }
    }


    /* --- Auth plugin name (NUL-terminated, if CLIENT_PLUGIN_AUTH) --- */
    if ((client_cap & CLIENT_PLUGIN_AUTH) && remaining > 0)
    {
        size_t plen = strnlen(pos_ptr, remaining);

        if (plen >= remaining)
        {
            mysql_packet_write_err(ps, 1043, "08S01",
                                   "Bad handshake: unterminated authentication plugin name");
            pfree(payload);
            return STATUS_ERROR;
        }
        if (plen > 0)
            auth_plugin_name = pos_ptr;
    }

    /* --- Validate --- */
    if (username == NULL || strlen(username) == 0)
    {
        mysql_packet_write_err(ps, 1045, "28000",
                               "Access denied for user '' (no username)");
        pfree(payload);
        return STATUS_ERROR;
    }

    /*
     * Keep the wire exchange deliberately free of authentication decisions.
     * HBA matching happens after InitPostgres(), when catalog access is
     * available.  In particular, do not send AuthSwitchRequest here: an HBA
     * rejection must be the first response to the HandshakeResponse41.
     */
    if (auth_response_len <= sizeof(auth->initial_auth_response))
        memcpy(auth->initial_auth_response, auth_response, auth_response_len);
    auth->initial_auth_response_len = auth_response_len;

    if (auth_plugin_name != NULL)
        auth->client_auth_plugin_name =
            MemoryContextStrdup(TopMemoryContext, auth_plugin_name);

    /* Set port fields — these are needed before InitPostgres().
     * port->database_name must always be set because BackendInitialize
     * dereferences it (and PostgresMain passes it to InitPostgres).
     * MySQL connections use the configured mysql_backend_database (default
     * "postgres"). This runs in the forked backend after GUC initialization
     * (mysql_startup_exchange -> BackendInitialize), and the GUC is
     * PGC_POSTMASTER, so its value is already settled and safe to read here. */
    port->user_name = MemoryContextStrdup(TopMemoryContext, username);
    port->database_name = MemoryContextStrdup(TopMemoryContext,
                                               (mysql_backend_database != NULL &&
                                                mysql_backend_database[0] != '\0')
                                               ? mysql_backend_database : "postgres");
    if (schema_name != NULL && schema_name[0] != '\0')
        port->compat_database_name = MemoryContextStrdup(TopMemoryContext,
                                                          schema_name);

    pfree(payload);

    /*
     * We return STATUS_OK so that BackendInitialize proceeds to PostgresMain,
     * which will call InitPostgres and then the authenticate callback.
     * The actual password verification happens in mysql_perform_authentication().
     */
    return STATUS_OK;
}

/* ----------------------------------------------------------------
 *    mysql_perform_authentication
 *
 * Phase B: HBA lookup and password verification.  Called from
 * ProtocolRoutine.authenticate after InitPostgres has catalog access.
 * ----------------------------------------------------------------
 */
void
mysql_perform_authentication(MysPacketState *ps, Port *port)
{
    MysAuthState *auth = (MysAuthState *) mysql_packet_get_auth_state(ps);
    char         *shadow_pass;
    const char   *logdetail = NULL;
    const char   *plugin_name;
    const uint8  *auth_response;
    size_t        auth_response_len;
    int           auth_result;


    if (auth == NULL)
    {
        ereport(FATAL,
                (errcode(ERRCODE_PROTOCOL_VIOLATION),
                 errmsg("MySQL authentication failed: no auth state")));
    }

    /*
     * HBA matching intentionally occurs here, after InitPostgres().  The
     * MySQL listener supports only the PostgreSQL md5 HBA method, because
     * the stored mysql_native_password verifier is a challenge response and
     * cannot faithfully implement trust, SCRAM, password, or external auth.
     */
    hba_getauthmethod((hbaPort *) port);
    CHECK_FOR_INTERRUPTS();

    switch (port->hba->auth_method)
    {
        case uaMD5:
            break;

        case uaReject:
        case uaImplicitReject:
            ereport(FATAL,
                    (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                     errmsg("MySQL authentication failed for user \"%s\"",
                            port->user_name)));
            break;

        default:
            ereport(FATAL,
                    (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                     errmsg("MySQL authentication method \"%s\" is not supported",
                            hba_authname(port->hba->auth_method))));
            break;
    }

    /*
     * The initial auth response format depends on the server-advertised
     * plugin (always caching_sha2_password), not the client's declared
     * plugin name.  Detect by response length:
     *
     *   0 bytes  → client wants immediate AuthSwitch (e.g.
     *              --default-auth=mysql_native_password)
     *   20 bytes → mysql_native_password was used directly (legacy)
     *   32 bytes → caching_sha2_password was used → AuthSwitch needed
     */
    plugin_name = auth->client_auth_plugin_name;

    if (auth->initial_auth_response_len == MYSQL_SCRAMBLE_LEN)
    {
        /* Client sent native password directly (legacy / older client). */
        auth_response = auth->initial_auth_response;
        auth_response_len = auth->initial_auth_response_len;
    }
    else if (auth->initial_auth_response_len == 0 ||
             auth->initial_auth_response_len == SHA256_DIGEST_LENGTH)
    {
        char        switch_pkt[64];
        char       *switch_response;
        size_t      switch_response_len;
        int         switch_pos = 0;
        const char *target_plugin = "mysql_native_password";
        size_t      target_plugin_len = strlen(target_plugin);

        /*
         * The server's AuthSwitchRequest is seq 2.  MySQL 8.4.10's native
         * plugin receives exactly scramble[20] followed by one trailing NUL.
         */
        switch_pkt[switch_pos++] = (char) 0xFE;
        memcpy(switch_pkt + switch_pos, target_plugin, target_plugin_len);
        switch_pos += target_plugin_len;
        switch_pkt[switch_pos++] = '\0';
        memcpy(switch_pkt + switch_pos, auth->scramble, MYSQL_SCRAMBLE_LEN);
        switch_pos += MYSQL_SCRAMBLE_LEN;
        switch_pkt[switch_pos++] = '\0';

        mysql_packet_set_server_seq(ps, 2);
        mysql_packet_write(ps, switch_pkt, (size_t) switch_pos);
        mysql_packet_set_seq(ps, 3);

        if (!mysql_packet_read(ps, &switch_response, &switch_response_len))
            ereport(FATAL,
                    (errcode(ERRCODE_PROTOCOL_VIOLATION),
                     errmsg("could not read MySQL AuthSwitchResponse")));

        /* Client seq 3 was consumed; all following server replies are seq 4. */
        mysql_packet_set_server_seq(ps, 4);
        if (switch_response_len != MYSQL_SCRAMBLE_LEN)
        {
            pfree(switch_response);
            ereport(FATAL,
                    (errcode(ERRCODE_PROTOCOL_VIOLATION),
                     errmsg("invalid mysql_native_password AuthSwitchResponse length")));
        }

        memcpy(auth->native_auth_response, switch_response, MYSQL_SCRAMBLE_LEN);
        auth->native_auth_response_len = MYSQL_SCRAMBLE_LEN;
        pfree(switch_response);
        auth_response = auth->native_auth_response;
        auth_response_len = auth->native_auth_response_len;
    }
    else
        ereport(FATAL,
                (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                 errmsg("MySQL authentication plugin \"%s\" is not supported",
                        plugin_name)));

    shadow_pass = get_role_password(port->user_name, &logdetail);
    if (shadow_pass == NULL)
    {
        ereport(FATAL,
                (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
                 errmsg("MySQL authentication failed for user \"%s\"",
                        port->user_name)));
    }

    auth_result = mysql_native_password_verify(port->user_name, shadow_pass,
                                                auth_response,
                                                auth_response_len,
                                                auth->scramble,
                                                MYSQL_SCRAMBLE_LEN,
                                                &logdetail);
    pfree(shadow_pass);

    if (auth_result != STATUS_OK)
    {
        ereport(FATAL,
                (errcode(ERRCODE_INVALID_PASSWORD),
                 errmsg("MySQL authentication failed for user \"%s\": password mismatch",
                        port->user_name)));
    }


    /*
     * Send the MySQL OK packet.  This is the server's response to the login
     * packet — the client has been waiting for this since sending its
     * handshake response.
     *
     * Sequence numbers during handshake are shared: greeting=0, login=1,
     * OK=2 (or greeting=0, login=1, auth-switch=2, client=3, OK=4).
     * server_seq is 2 for direct native authentication and 4 after a switch.
     */
    set_authn_id(port, port->user_name);
    if (ClientAuthentication_hook)
        (*ClientAuthentication_hook) (port, STATUS_OK);

    {
        char ok[7];
        int  okpos = 0;
        ok[okpos++] = 0x00;                      /* OK header */
        ok[okpos++] = 0x00;                      /* affected rows = 0 */
        ok[okpos++] = 0x00;                      /* last insert id = 0 */
        ok[okpos++] = 0x02; ok[okpos++] = 0x00;  /* status: autocommit */
        ok[okpos++] = 0x00; ok[okpos++] = 0x00;  /* warnings: 0 */
        mysql_packet_write_ok(ps, ok, (size_t) okpos, 0x00);
    }

    /*
     * Flush the OK packet to the client immediately.  secure_write() may
     * buffer data in the SSL layer; without this flush the client never
     * sees the OK, waits forever, and eventually closes the connection.
     * openHalo's netTransceiver->finishWriteToBufFlush serves the same role.
     */
    pq_flush();

    /*
     * The handshake is complete.  Reset both sequence counters for the
     * command phase.  The client will send commands at seq 0 and the
     * server responds starting at seq 1 (shared sequence space).
     */
    mysql_packet_reset_seq(ps);
    mysql_packet_set_server_seq(ps, 1);

    explicit_bzero(auth->initial_auth_response,
                   sizeof(auth->initial_auth_response));
    explicit_bzero(auth->native_auth_response,
                   sizeof(auth->native_auth_response));
    if (auth->client_auth_plugin_name != NULL)
        pfree(auth->client_auth_plugin_name);
    pfree(auth);
    mysql_packet_set_auth_state(ps, NULL);

    /*
     * The MysPacketState (port->protocol_state) stays alive for the
     * lifetime of the session — it is used by every I/O callback.
     * Do NOT free it here.
     */
}
