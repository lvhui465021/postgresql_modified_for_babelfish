/*-------------------------------------------------------------------------
 *
 * protocol_routine.h
 *    Protocol-routine dispatch interface for multi-protocol backends.
 *
 * A ProtocolRoutine is a vtable of callbacks that let a wire protocol
 * (PostgreSQL, MySQL, TDS, …) plug into the backend lifecycle at the
 * connection-init, command-I/O, session, and DestReceiver layers.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/postmaster/protocol_routine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROTOCOL_ROUTINE_H
#define PROTOCOL_ROUTINE_H

#include "libpq/libpq-be.h"          /* CompatibilityProtocolKind, Port        */
#include "tcop/dest.h"               /* CommandDest, DestReceiver             */
#include "tcop/utility.h"            /* ProcessUtility_hook_type               */

/* forward declarations */
struct QueryCompletion;
struct ParserRoutine;
struct PortalData;
struct ErrorData;
struct Node;

/* ----------------------------------------------------------------
 *    ProtocolCommandResult
 *
 * Returned by process_command():
 *   PASSTHROUGH  – caller should handle the command via standard path
 *   HANDLED      – protocol consumed the command; skip standard handling
 * ----------------------------------------------------------------
 */
typedef enum ProtocolCommandResult
{
    PROTOCOL_COMMAND_PASSTHROUGH,
    PROTOCOL_COMMAND_HANDLED
} ProtocolCommandResult;

/* ----------------------------------------------------------------
 *    ProtocolRoutine
 *
 * Each wire protocol provides one const instance of this struct.
 * A NULL callback means "use the standard PostgreSQL behaviour".
 *
 * Two notes for registrants of a non-standard kind:
 *  - The command-I/O wrappers (read_command/process_command/comm_reset/
 *    is_reading_msg) and session_initialize/send_backend_key_data treat a
 *    NULL member on a non-standard kind as an error.  A protocol whose
 *    mainfunc owns the whole backend loop (TDS) never reaches those
 *    wrappers, but must still supply explicit no-op/false stubs rather
 *    than NULL to satisfy the contract.
 *  - The five simple-query multi-statement policy fields
 *    (allow_multi_statements .. capture_session_state) and parser_routine
 *    /process_utility are legitimately NULL for a protocol that does not
 *    use the PG simple-query path or the raw-parse/DDL dispatch points.
 * ----------------------------------------------------------------
 */
typedef struct ProtocolRoutine
{
    CompatibilityProtocolKind kind;
    const char *name;                  /* human-readable, for error messages  */

    /* --- lifecycle hooks (called during backend startup) --- */
    void        (*init)(Port *port);
    int         (*startup_exchange)(Port *port);   /* 0 = ok, -1 = reject   */
    void        (*mainfunc)(Port *port);

    /* --- command I/O hooks --- */
    int         (*read_command)(StringInfo inBuf);
    ProtocolCommandResult (*process_command)(int *command, StringInfo inBuf);
    void        (*comm_reset)(void);
    bool        (*is_reading_msg)(void);

    /* --- session hooks --- */
    void        (*session_initialize)(Port *port);
    void        (*send_backend_key_data)(int pid, const uint8 *key, int keylen);

    /* --- DestReceiver hooks --- */
    DestReceiver *(*create_dest_receiver)(CommandDest dest);
    void        (*set_remote_dest_receiver_params)(DestReceiver *receiver,
                                                    struct PortalData *portal);
    void        (*end_command)(const QueryCompletion *qc,
                                CommandDest dest,
                                bool force_undecorated_output);
    void        (*null_command)(CommandDest dest);
    void        (*send_ready_for_query)(CommandDest dest);

    /* --- simple-query multi-statement policy / result framing --- */
    bool        (*allow_multi_statements)(void);
    bool        (*simple_query_statement_ends_xact)(void);
    void        (*set_simple_query_more_results)(bool more);
    void        (*before_simple_query_statement)(struct Node *stmt);
    void        (*capture_session_state)(QueryCompletion *qc);

    /* --- error / GUC hooks --- */
    void        (*send_error)(struct ErrorData *edata);
    void        (*report_parameter_status)(const char *name, const char *value);

    /* --- utility-command execution --- */
    ProcessUtility_hook_type process_utility;

    /* --- parser selection --- */
    const struct ParserRoutine *parser_routine;

    /* --- authentication hook --- */
    void        (*authenticate)(Port *port);

    /* --- postmaster-side listener/socket hooks (NULL = standard) --- */
    int         (*accept)(pgsocket server_fd, ClientSocket *client_sock);
    int         (*close)(pgsocket server_fd);

    /*
     * direct_ssl_handshake -- run a protocol-specific pre-startup SSL
     * handshake (TDS PRELOGIN).  NULL means "use the standard
     * ProcessSSLStartup() probe"; when set it takes precedence over
     * startup_exchange for that phase only.
     */
    int         (*direct_ssl_handshake)(Port *port);
} ProtocolRoutine;

/*
 * A Port's protocol_kind and protocol_routine are resolved together:
 * protocol_kind is stamped by the postmaster at accept() time from the
 * listener socket the connection arrived on (see ServerLoop() in
 * postmaster.c and listen_add_protocol_socket() in protocol_extension.h),
 * and protocol_routine is resolved from protocol_kind by
 * AssignProtocolRoutine(), which pq_init() calls for every connection.
 * A non-standard kind therefore always has a registered ProtocolRoutine
 * by the time the backend starts; AssignProtocolRoutine() FATALs rather
 * than silently continuing if a kind has no registered routine.
 *
 * The old ProtocolExtensionConfig mechanism (Babelfish's parallel
 * per-protocol dispatch table) is gone: its TDS user now registers a
 * ProtocolRoutine like every other dialect, and the postmaster resolves
 * listener accept/close through the same registry.
 */

/* ----------------------------------------------------------------
 *    Global registry and accessors
 * ----------------------------------------------------------------
 */
extern const ProtocolRoutine *GetCurrentProtocolRoutine(void);
extern const ProtocolRoutine *GetProtocolRoutine(CompatibilityProtocolKind kind);
extern void   AssignProtocolRoutine(Port *port);
extern void   RegisterProtocolRoutine(const ProtocolRoutine *routine);
extern bool   CompatibilityProtocolKindIsValid(CompatibilityProtocolKind kind);

/*
 * ListenProtocolServerPort -- open a listener socket for a registered
 * protocol kind on the given address/port.  Called by the postmaster for
 * built-in protocols and by loadable modules (via RegisterListenInitRoutine)
 * for compatibility listeners such as MySQL; exported so modules can open
 * their listener from the postmaster's address space.
 */
extern int  ListenProtocolServerPort(CompatibilityProtocolKind kind, int family,
									 const char *hostName, unsigned short portNumber,
									 const char *unixSocketName);

/* ----------------------------------------------------------------
 *    Helper functions called from postgres.c / dest.c
 * ----------------------------------------------------------------
 */
extern void ProtocolSetRemoteDestReceiverParams(DestReceiver *receiver,
                                                struct PortalData *portal);

#endif   /* PROTOCOL_ROUTINE_H */
