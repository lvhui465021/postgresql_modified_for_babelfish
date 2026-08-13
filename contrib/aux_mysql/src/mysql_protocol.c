/*-------------------------------------------------------------------------
 *
 * mysql_protocol.c
 *    MySQL ProtocolRoutine vtable: lifecycle, I/O, DestReceiver, and
 *    error-encoding callbacks.
 *
 * A single const ProtocolRoutine instance is created and registered at
 * _PG_init time so that postmaster startup can resolve it.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/adapter/mysql/mysql_protocol.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "access/relation.h"
#include "commands/mysql/mys_tablecmds.h"
#include "commands/sequence.h"
#include "access/xact.h"
#include "adapter/mysql/mysql_auth.h"
#include "adapter/mysql/mysql_command.h"
#include "adapter/mysql/mysql_packet.h"
#include "adapter/mysql/mysql_protocol.h"
#include "adapter/mysql/mysql_stmt.h"
#include "adapter/mysql/mys_session_state_exports.h"
#include "adapter/mysql/systemVar.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsereng.h"
#include "postmaster/protocol_routine.h"
#include "tcop/mysql/mys_utility.h"
#include "tcop/tcopprot.h"
#include "tcop/dest.h"
#include "utils/builtins.h"
#include "utils/attoptcache.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "fmgr.h"
#include "utils/guc.h"
#include "utils/syscache.h"

#include <string.h>

/* ----------------------------------------------------------------
 *    Forward declarations
 * ----------------------------------------------------------------
 */
static void mysql_init(Port *port);
static int  mysql_startup_exchange(Port *port);
static void mysql_authenticate(Port *port);

static int  mysql_read_command_cb(StringInfo inBuf);
static ProtocolCommandResult mysql_process_command(int *command,
                                                    StringInfo inBuf);
static void mysql_comm_reset(void);
static bool mysql_is_reading_msg(void);
static void mysql_send_backend_key_data_noop(int pid, const uint8 *key,
                                              int keylen);
static void mysql_session_initialize(Port *port);
static void mysql_set_remote_dest_receiver_params(DestReceiver *receiver,
                                                   struct PortalData *portal);

static DestReceiver *mysql_create_dest_receiver(CommandDest dest);
static void mysql_end_command(const QueryCompletion *qc,
                              CommandDest dest,
                              bool force_undecorated_output);
static void mysql_null_command(CommandDest dest);
static void mysql_send_ready_for_query(CommandDest dest);

static void mysql_send_error(ErrorData *edata);
static void mysql_report_parameter_status(const char *name, const char *value);
static int mysql_write_lenenc_uint64(char *buf, uint64 value);
static void mysql_field_list(StringInfo inBuf);
static void mysql_set_option(StringInfo inBuf);
static bool mysql_allow_multi_statements(void);
static bool mysql_simple_query_statement_ends_xact(void);
static void mysql_set_simple_query_more_results(bool more);
static void mysql_before_simple_query_statement(Node *stmt);
static void mysql_capture_session_state(QueryCompletion *qc);

/* State for the current result within one COM_QUERY message. */
static bool mysql_simple_query_more_results = false;

/* ----------------------------------------------------------------
 *    Per-connection packet state accessor
 * ----------------------------------------------------------------
 */
static inline MysPacketState *
mysql_ps(void)
{
    return (MysPacketState *) MyProcPort->protocol_state;
}

static uint16
mysql_server_status(uint16 extra_status)
{
	uint16		status = 0;

	if (MysAutocommitEnabled())
		status |= 0x0002; /* SERVER_STATUS_AUTOCOMMIT */
	if (IsTransactionBlock())
		status |= 0x0001; /* SERVER_STATUS_IN_TRANS */
	if (mysql_simple_query_more_results)
		status |= 0x0008; /* SERVER_MORE_RESULTS_EXISTS */
	return status | extra_status;
}

static bool
mysql_allow_multi_statements(void)
{
	return (mysql_negotiated_caps(mysql_ps()) & MYSQL_CAP_MULTI_STATEMENTS) != 0;
}

static bool
mysql_simple_query_statement_ends_xact(void)
{
	/*
	 * Always complete the current command.  When autocommit is off, the
	 * explicit PG18 block created lazily below remains open across commands.
	 */
	return true;
}

static void
mysql_set_simple_query_more_results(bool more)
{
	mysql_simple_query_more_results = more;
}


static void
mysql_before_simple_query_statement(Node *stmt)
{
	/*
	 * Statement-boundary diagnostics: MySQL keeps the previous statement's
	 * warnings readable via SHOW WARNINGS, so a new statement that is NOT
	 * SHOW WARNINGS drops the inherited list before it runs (the list is
	 * re-populated by any NOTICE/WARNING/INFO this statement raises).
	 *
	 * mys_raw_parser() records the exact Node* of any statement that came
	 * from reparsing the SHOW WARNINGS marker query in
	 * mysql_show_warnings_preserve_stmts, so it reads -- not clears -- the
	 * list.  Matching by this statement's own identity (rather than a
	 * single flag consumed by whichever statement dispatches next) is
	 * required for multi-statement batches: the whole batch is parsed in
	 * one shot before any statement in it is dispatched, so a flag would be
	 * satisfied by the wrong statement whenever SHOW WARNINGS isn't first
	 * in the batch.
	 */
	if (list_member_ptr(mysql_show_warnings_preserve_stmts, stmt))
	{
		mysql_show_warnings_preserve_stmts =
			list_delete_ptr(mysql_show_warnings_preserve_stmts, stmt);
		mysql_packet_reset_stmt_warning_state(mysql_ps());
	}
	else
		mysql_packet_reset_warning_count(mysql_ps());

	if (IsA(stmt, TransactionStmt))
	{
		TransactionStmt *xact = castNode(TransactionStmt, stmt);

		/*
		 * MySQL BEGIN commits an active autocommit=0 transaction before it
		 * starts the new explicit transaction.  Finish that PG18 command here
		 * so the normal utility handler can safely call BeginTransactionBlock.
		 */
		if (!MysAutocommitEnabled() && IsTransactionBlock() &&
			(xact->kind == TRANS_STMT_BEGIN || xact->kind == TRANS_STMT_START))
		{
			(void) EndTransactionBlock(false);
			ProtocolFinishCommand();
			ProtocolStartCommand();
		}
		return;
	}

	/* SET itself only changes session state. */
	if (IsA(stmt, VariableSetStmt))
		return;

	/*
	 * All other statements require a transaction in PostgreSQL.  When
	 * autocommit is off, start an implicit transaction block for any
	 * statement — DDL included.  (MySQL DDL auto-commits, but PG DDL
	 * needs a transaction for catalog access during sequence/trigger
	 * creation, etc.)
	 */
	if (!MysAutocommitEnabled() && !IsTransactionBlock())
		BeginTransactionBlock();
}

/*
 * Called from exec_simple_query just before finish_xact_command(), while
 * the transaction is still active.  Capture session-local state that
 * depends on active-transaction resources (e.g. lastval() for sequences).
 */
static void
mysql_capture_session_state(QueryCompletion *qc)
{
	CommandTag	tag = qc->commandTag;

	/* ROW_COUNT() — simple passthrough from nprocessed. */
	mysql_packet_set_row_count(mysql_ps(), qc->nprocessed);

	/*
	 * LAST_INSERT_ID(): derive the first generated AUTO_INCREMENT value
	 * from lastval() while the transaction is still open.
	 */
	if (tag == CMDTAG_INSERT && qc->nprocessed > 0)
	{
		Datum		last_id_datum = (Datum) 0;
		bool		have_last_id = false;
		uint64		last_id = 0;

		PG_TRY();
		{
			last_id_datum = lastval(NULL);
			have_last_id = true;
		}
		PG_CATCH();
		{
			FlushErrorState();
		}
		PG_END_TRY();

		if (have_last_id)
		{
			int64 value = DatumGetInt64(last_id_datum);

			if (value > 0 && (uint64) value >= qc->nprocessed)
				last_id = (uint64) value - qc->nprocessed + 1;
		}

		mysql_packet_set_last_insert_id(mysql_ps(), last_id);
	}
}

/* COM_SET_OPTION only controls CLIENT_MULTI_STATEMENTS on this protocol. */
static void
mysql_set_option(StringInfo inBuf)
{
	uint16		option;
	uint32		caps;
	uint16		status;

	if (inBuf->len != 2)
		ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("invalid MySQL COM_SET_OPTION packet")));
	option = (uint16) (unsigned char) inBuf->data[0] |
		((uint16) (unsigned char) inBuf->data[1] << 8);
	caps = mysql_packet_get_client_caps(mysql_ps());
	if (option == 0) /* MYSQL_OPTION_MULTI_STATEMENTS_ON */
		caps |= MYSQL_CAP_MULTI_STATEMENTS;
	else if (option == 1) /* MYSQL_OPTION_MULTI_STATEMENTS_OFF */
		caps &= ~MYSQL_CAP_MULTI_STATEMENTS;
	else
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("unsupported MySQL COM_SET_OPTION value %u", option)));
	mysql_packet_set_client_caps(mysql_ps(), caps);
	status = mysql_server_status(0);
	if (mysql_negotiated_caps(mysql_ps()) & MYSQL_CAP_DEPRECATE_EOF)
	{
		char		eof_ok[7] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8), 0x00, 0x00};

		mysql_packet_write(mysql_ps(), eof_ok, sizeof(eof_ok));
	}
	else
	{
		char		eof[5] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8)};

		mysql_packet_write(mysql_ps(), eof, sizeof(eof));
	}
	pq_flush();
	mysql_packet_reset_seq(mysql_ps());
	mysql_packet_set_server_seq(mysql_ps(), 1);
}

/* MySQL length-encoded integer used by OK affected_rows fields. */
static int
mysql_write_lenenc_uint64(char *buf, uint64 value)
{
	int			pos = 0;

	if (value < 251)
		buf[pos++] = (char) value;
	else if (value <= UINT16_MAX)
	{
		buf[pos++] = (char) 0xfc;
		buf[pos++] = (char) (value & 0xff);
		buf[pos++] = (char) ((value >> 8) & 0xff);
	}
	else if (value <= 0xFFFFFF)		/* 0xfd 3-byte form holds up to 2^24-1 */
	{
		buf[pos++] = (char) 0xfd;
		buf[pos++] = (char) (value & 0xff);
		buf[pos++] = (char) ((value >> 8) & 0xff);
		buf[pos++] = (char) ((value >> 16) & 0xff);
	}
	else							/* >= 2^24: 0xfe 8-byte form */
	{
		buf[pos++] = (char) 0xfe;
		for (int i = 0; i < 8; i++)
			buf[pos++] = (char) ((value >> (i * 8)) & 0xff);
	}
	return pos;
}

static bool
mysql_field_name_matches(const char *name, const char *pattern)
{
	/* MySQL FIELD_LIST uses SQL LIKE wildcards, with backslash escaping. */
	if (*pattern == '\0')
		return true;
	if (*pattern == '\\')
		return pattern[1] != '\0' && *name != '\0' &&
			pg_tolower((unsigned char) *name) == pg_tolower((unsigned char) pattern[1]) &&
			mysql_field_name_matches(name + 1, pattern + 2);
	if (*pattern == '%')
		return mysql_field_name_matches(name, pattern + 1) ||
			(*name != '\0' && mysql_field_name_matches(name + 1, pattern));
	if (*pattern == '_')
		return *name != '\0' && mysql_field_name_matches(name + 1, pattern + 1);
	return *name != '\0' &&
		pg_tolower((unsigned char) *name) == pg_tolower((unsigned char) *pattern) &&
		mysql_field_name_matches(name + 1, pattern + 1);
}

static uint8
mysql_field_mysql_type(Oid typid)
{
	HeapTuple	typetup;
	Form_pg_type typeform;

	/* MySQL ENUM/SET are represented by private collatable text domains. */
	/* Some synthetic protocol expressions retain an unresolved type OID. */
	if (!OidIsValid(typid))
		return 253;
	typetup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(typetup))
	{
		typeform = (Form_pg_type) GETSTRUCT(typetup);
		if (typeform->typtype == TYPTYPE_DOMAIN &&
			typeform->typbasetype == TEXTOID &&
			strncmp(NameStr(typeform->typname), "enum_", 5) == 0)
		{
			ReleaseSysCache(typetup);
			return 247;              /* MYSQL_TYPE_ENUM */
		}
		if (typeform->typtype == TYPTYPE_DOMAIN &&
			typeform->typbasetype == TEXTOID &&
			strncmp(NameStr(typeform->typname), "set_", 4) == 0)
		{
			ReleaseSysCache(typetup);
			return 248;              /* MYSQL_TYPE_SET */
		}
		ReleaseSysCache(typetup);
	}

	switch (typid)
	{
		case BOOLOID: return 1;
		case INT2OID: return 2;
		case INT4OID: return 3;
		case INT8OID: return 8;
		case FLOAT4OID: return 4;
		case FLOAT8OID: return 5;
		case NUMERICOID: return 246;
		case BPCHAROID: return 254;
		case BYTEAOID: return 252;
		case TIMESTAMPOID: return 12;
		case TIMESTAMPTZOID: return 7;
		case DATEOID: return 10;
		case TIMEOID: return 11;
		case JSONOID: return 245;
		case BITOID:
		case VARBITOID: return 16;
		default: return 253;
	}
}

static void
mysql_field_append_lenenc(StringInfo buf, const char *value)
{
	Size len = strlen(value);

	if (len >= 251)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("MySQL field metadata string is too long")));
	appendStringInfoChar(buf, (char) len);
	appendBinaryStringInfo(buf, value, len);
}

/*
 * Append a 4-byte little-endian integer, as the MySQL wire protocol
 * mandates for fields like column length regardless of server
 * architecture -- unlike appendBinaryStringInfo(buf, (char *) &v, 4), this
 * is correct on big-endian hosts too.
 */
static void
mysql_field_append_int4_le(StringInfo buf, int32 value)
{
	uint8 le[4];

	le[0] = (uint8) (value & 0xFF);
	le[1] = (uint8) ((value >> 8) & 0xFF);
	le[2] = (uint8) ((value >> 16) & 0xFF);
	le[3] = (uint8) ((value >> 24) & 0xFF);
	appendBinaryStringInfo(buf, (char *) le, 4);
}

/*
 * mysqld_list_fields() restores the table's default record before sending
 * field metadata.  PostgreSQL keeps only an analyzed default expression, so
 * this is deliberately a limited literal projection.  In particular it
 * cannot recover MySQL's distinction between DEFAULT 7 and DEFAULT (7): the
 * latter is a default expression and MySQL sends its type's reset value.
 * Do not treat this constant folding as a complete MySQL default-record
 * implementation; that requires persisted MySQL default provenance.
 */
static char *
mysql_field_default_reset_value(Form_pg_attribute attr)
{
	switch (attr->atttypid)
	{
		case DATEOID:
			return pstrdup("0000-00-00");
		case TIMEOID:
		case TIMETZOID:
			return pstrdup("00:00:00");
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return pstrdup("0000-00-00 00:00:00");
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			return pstrdup("0");
		default:
			return pstrdup("");
	}
}

static char *
mysql_field_default_value(Relation rel, TupleDesc desc, Form_pg_attribute attr)
{
	Node	   *expr;
	Const	  *constant;
	AttributeOpts *options;
	Oid		outputfunc;
	bool		isvarlena;
	char	   *result;
	int		len;

	if (!attr->atthasdef)
		return NULL;
	options = get_attribute_options(RelationGetRelid(rel), attr->attnum);
	if (options != NULL &&
		options->mysql_default_kind == MYSQL_DEFAULT_KIND_EXPRESSION)
		return attr->attnotnull ? mysql_field_default_reset_value(attr) : NULL;
	expr = TupleDescGetDefault(desc, attr->attnum);
	if (expr == NULL)
		return NULL;
	expr = eval_const_expressions(NULL, expr);
	expr = strip_implicit_coercions(expr);
	if (!IsA(expr, Const))
		return NULL;
	constant = (Const *) expr;
	if (constant->constisnull)
		return NULL;
	getTypeOutputInfo(constant->consttype, &outputfunc, &isvarlena);
	result = OidOutputFunctionCall(outputfunc, constant->constvalue);

	/*
	 * PostgreSQL bpchar (CHAR) stores blank-padded values.  MySQL
	 * COM_FIELD_LIST must return the default as declared in the DDL,
	 * not the storage representation.  Strip trailing spaces for
	 * bpchar defaults so that CHAR(8) DEFAULT 'fixed' surfaces as
	 * 'fixed' rather than 'fixed   '.
	 */
	if (attr->atttypid == BPCHAROID)
	{
		len = strlen(result);
		while (len > 0 && result[len - 1] == ' ')
			result[--len] = '\0';
	}
	return result;
}

static void
mysql_field_send_definition(MysPacketState *ps, const char *schema,
								const char *table, Relation rel, TupleDesc desc,
								Form_pg_attribute attr)
{
	StringInfoData buf;
	char	   *default_value;
	int32 collen = attr->atttypmod > 0 ? attr->atttypmod : 256;

	initStringInfo(&buf);
	mysql_field_append_lenenc(&buf, "def");
	mysql_field_append_lenenc(&buf, schema);
	mysql_field_append_lenenc(&buf, table);
	mysql_field_append_lenenc(&buf, table);
	mysql_field_append_lenenc(&buf, NameStr(attr->attname));
	mysql_field_append_lenenc(&buf, NameStr(attr->attname));
	appendStringInfoChar(&buf, 0x0c);
	appendStringInfoChar(&buf, 0x2d); appendStringInfoChar(&buf, 0x00);
	mysql_field_append_int4_le(&buf, collen);
	appendStringInfoChar(&buf, mysql_field_mysql_type(attr->atttypid));
	appendStringInfoChar(&buf, attr->attnotnull ? 0x01 : 0x00);
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	default_value = mysql_field_default_value(rel, desc, attr);
	if (default_value == NULL)
		appendStringInfoChar(&buf, 0xfb);
	else
	{
		mysql_field_append_lenenc(&buf, default_value);
		pfree(default_value);
	}
	mysql_packet_write(ps, buf.data, buf.len);
	pfree(buf.data);
}

static void
mysql_field_list(StringInfo inBuf)
{
	char *separator;
	char *table;
	char *pattern;
	RangeVar *rv;
	Relation rel;
	TupleDesc desc;
	const char *schema;

	if (inBuf->len == 0 || (separator = memchr(inBuf->data, '\0', inBuf->len)) == NULL ||
		separator == inBuf->data || memchr(separator + 1, '\0',
		inBuf->len - (separator + 1 - inBuf->data)) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid COM_FIELD_LIST payload")));
	table = pnstrdup(inBuf->data, separator - inBuf->data);
	pattern = pnstrdup(separator + 1, inBuf->len - (separator + 1 - inBuf->data));

	/*
	 * COM_FIELD_LIST is not a MySQL SQL statement, but it still reads
	 * PostgreSQL catalogs and relcache.  This direct protocol handler bypasses
	 * the normal PostgresMain command switch, so establish only its internal
	 * command/timeout lifecycle here; it emits no SQL completion packet.
	 */
	ProtocolStartCommand();
	rv = makeRangeVar(NULL, table, -1);
	rel = relation_openrv(rv, AccessShareLock);
	desc = RelationGetDescr(rel);
	schema = get_namespace_name(RelationGetNamespace(rel));
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (!attr->attisdropped &&
			mysql_field_name_matches(NameStr(attr->attname), pattern))
			mysql_field_send_definition(mysql_ps(), schema,
									RelationGetRelationName(rel), rel, desc, attr);
	}
	relation_close(rel, AccessShareLock);
	ProtocolFinishCommand();
	{
		uint16 status = mysql_server_status(0);

	if (mysql_negotiated_caps(mysql_ps()) & MYSQL_CAP_DEPRECATE_EOF)
	{
		char eof_ok[7] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8), 0x00, 0x00};
		mysql_packet_write(mysql_ps(), eof_ok, sizeof(eof_ok));
	}
	else
	{
		char eof[5] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8)};
		mysql_packet_write(mysql_ps(), eof, sizeof(eof));
	}
	}
	pq_flush();
	mysql_packet_reset_seq(mysql_ps());
	mysql_packet_set_server_seq(mysql_ps(), 1);
}

/* ----------------------------------------------------------------
 *    Lifecycle callbacks
 * ----------------------------------------------------------------
 */
static void
mysql_init(Port *port)
{
    MysPacketState *ps;

    /*
     * Set the backend type early so that pgstat tracks our I/O operations.
     * This mirrors openHalo's adapter.c:startServer which also sets
     * MyBackendType = B_BACKEND.  Without this, the first catalog access
     * during authentication (SearchSysCache→read from disk) triggers an
     * assertion failure in pgstat_tracks_io_op() when cassert is enabled.
     */
    MyBackendType = B_BACKEND;

    ps = mysql_packet_create(port);
    port->protocol_state = (void *) ps;
}

static int
mysql_startup_exchange(Port *port)
{
    MysPacketState *ps = (MysPacketState *) port->protocol_state;
    int            status;

    Assert(ps != NULL);

    /*
     * Set the socket to blocking mode for the handshake and keep it that
     * way for the lifetime of the MySQL session.  Our packet I/O callbacks
     * use direct secure_read/secure_write with EAGAIN retry, bypassing the
     * PG FeBeWaitSet.  A non-blocking socket causes spurious ENOTSOCK
     * failures during result-set writing.
     */
    port->noblock = false;

    /* Phase A1: Send the MySQL handshake greeting. */
    mysql_send_greeting(ps, port);

    /* Phase A2: Read and verify the login response. */
    status = mysql_verify_login(ps, port);

    /*
     * Keep the socket blocking for MySQL connections.  Our packet I/O
     * callbacks (mysql_read_command_cb, mysql_packet_write, …) use direct
     * secure_read/secure_write with EAGAIN retry, bypassing the PG
     * FeBeWaitSet.  A non-blocking socket causes spurious ENOTSOCK
     * failures during result-set writing.
     */
    port->noblock = false;

    return status;
}

static void
mysql_authenticate(Port *port)
{
    /*
     * Phase B of MySQL authentication.  startup_exchange already read
     * the login packet and extracted user_name / compat_database_name.
     * Now InitPostgres has run, so we have catalog access and can
     * verify the password.
     */
    mysql_perform_authentication(mysql_ps(), port);
}

/* ----------------------------------------------------------------
 *    Command I/O callbacks
 * ----------------------------------------------------------------
 */
static int
mysql_read_command_cb(StringInfo inBuf)
{
    MysPacketState *ps = (MysPacketState *) MyProcPort->protocol_state;
    int            command;
    int            len;

    Assert(ps != NULL);

    len = mysql_read_command(ps, &command, inBuf);
    if (len < 0)
        return -1;

    /*
     * Return the pseudo-command directly.  The PostgresMain loop stores
     * this as 'firstchar' and passes it to ProtocolProcessCommand().
     */
    return command;
}

static ProtocolCommandResult
mysql_process_command(int *command, StringInfo inBuf)
{
	if (command == NULL)
		return PROTOCOL_COMMAND_PASSTHROUGH;
	mysql_simple_query_more_results = false;

    switch (*command)
    {
    case MYSQL_PSEUDO_QUERY:
        if (inBuf->len > 0)
        {
            /*
             * Intercept MySQL SET NAMES / SET CHARACTER SET commands.
             * These are sent by most MySQL drivers during connection
             * initialisation.  Our MySQL parser produces a
             * VariableSetStmt (mysql._ prefix) which the PG18 utility path does not
             * handle, so respond with a clean MySQL OK instead of
             * letting the query pipeline fail with "unrecognized node
             * type: 441".
             */
            {
                int query_len = inBuf->len > 1 ? inBuf->len - 1 : 0;
                int log_len = Min(query_len, 1024);

                elog(LOG, "MYSQL_QUERY: [%.*s]%s", log_len, inBuf->data,
                     query_len > log_len ? "... [truncated]" : "");

                /*
                 * MySQL 8.x CLI sends "select $$" as a session-tracking
                 * capability probe during connection initialisation.
                 * PG interprets $$ as a dollar-quote start, causing a
                 * syntax error that corrupts the MySQL protocol packet
                 * sequence counter.  Intercept the probe here and return
                 * a proper MySQL ER_BAD_FIELD_ERROR so the CLI continues
                 * gracefully.
                 */
                {
                    const char *q = inBuf->data;
                    int qlen = query_len;

                    /* Skip leading whitespace */
                    while (qlen > 0 && (*q == ' ' || *q == '\t' ||
                                        *q == '\n' || *q == '\r'))
                    { q++; qlen--; }

                    if (qlen >= 9 && pg_strncasecmp(q, "select $$", 9) == 0)
                    {
                        mysql_packet_write_err(mysql_ps(), 1054, "42S22",
                            "Unknown column '$$' in 'field list'");
                        pq_flush();
                        mysql_packet_reset_seq(mysql_ps());
                        mysql_packet_set_server_seq(mysql_ps(), 1);
                        return PROTOCOL_COMMAND_HANDLED;
                    }
                }
            }

            /*
             * All non-empty queries, including SET, flow through the MySQL
             * parser pipeline so session state can affect transaction
             * lifetime and the completion status word.
             */
            *command = 'Q';
            return PROTOCOL_COMMAND_PASSTHROUGH;
        }

        /*
         * Empty query: send a generic MySQL OK so that the client
         * protocol state machine keeps progressing.  Mirrors openHalo's
         * behavior for zero-length COM_QUERY payloads.
         */
        {
            char ok[7];
            int  pos = 0;
            ok[pos++] = 0x00;
            ok[pos++] = 0x00;
            ok[pos++] = 0x00;
            ok[pos++] = 0x02; ok[pos++] = 0x00;
            ok[pos++] = 0x00; ok[pos++] = 0x00;
            mysql_packet_write_ok(mysql_ps(), ok, (size_t) pos, 0x00);
            pq_flush();
            mysql_packet_reset_seq(mysql_ps());
            mysql_packet_set_server_seq(mysql_ps(), 1);
        }
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_QUIT:
        *command = 'X';
        return PROTOCOL_COMMAND_PASSTHROUGH;
    case MYSQL_PSEUDO_PING:
        {
            char ok[7];
            int  pos = 0;
            ok[pos++] = 0x00;
            ok[pos++] = 0x00;
            ok[pos++] = 0x00;
            ok[pos++] = 0x02; ok[pos++] = 0x00;
            ok[pos++] = 0x00; ok[pos++] = 0x00;
            mysql_packet_write_ok(mysql_ps(), ok, (size_t) pos, 0x00);
            pq_flush();
            mysql_packet_reset_seq(mysql_ps());
            mysql_packet_set_server_seq(mysql_ps(), 1);
        }
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_STMT_PREPARE:
        mysql_stmt_prepare(mysql_ps(), inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_STMT_CLOSE:
        mysql_stmt_close(mysql_ps(), inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_STMT_RESET:
        mysql_stmt_reset(mysql_ps(), inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_STMT_EXECUTE:
        mysql_stmt_execute(mysql_ps(), inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_STMT_SEND_LONG_DATA:
        mysql_stmt_send_long_data(mysql_ps(), inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_STMT_FETCH:
        mysql_stmt_fetch(mysql_ps(), inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_SET_OPTION:
        mysql_set_option(inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    case MYSQL_PSEUDO_FIELD_LIST:
        mysql_field_list(inBuf);
        return PROTOCOL_COMMAND_HANDLED;
    default:
        return PROTOCOL_COMMAND_PASSTHROUGH;
    }
}
static void
mysql_comm_reset(void)
{
    /*
     * No-op: mirror openHalo's .comm_reset = NULL.
     * MySQL protocol does NOT reset sequence numbers between commands.
     * Both ps->seq (read) and ps->server_seq (write) track independently
     * and naturally wrap at 256, which is correct per protocol spec.
     * Resetting here would break the sequence for end_command's EOF/OK.
     */
}
static bool
mysql_is_reading_msg(void)
{
    /* MySQL clients don't have a "message in progress" state like PG. */
    return false;
}
static void
mysql_send_backend_key_data_noop(int pid, const uint8 *key, int keylen)
{
    /* MySQL protocol does not use PG cancel-key packets. */
}
static void
mysql_session_initialize(Port *port)
{
    /* Backend processes normally serve one session, but reset explicitly. */
    MysSetAutocommit(true);
	MysInitSessionTimeZone();
	/*
	 * MySQL's own default transaction isolation level is REPEATABLE READ
	 * (InnoDB's default since MySQL 5.x, unchanged through 8.4); the
	 * instance-wide default_transaction_isolation stays PostgreSQL's own
	 * "read committed" (SQL Server's default too, confirmed empirically
	 * against a real T-SQL connection -- see FUSION_PLAN.md P2-16), so PG
	 * and TDS connections are untouched by this.  Session-scoped only
	 * (PGC_S_SESSION), same as the search_path override below: a MySQL
	 * client that never issues its own SET SESSION TRANSACTION ISOLATION
	 * LEVEL now still gets MySQL-correct semantics, without editing the
	 * shared postgresql.conf default that every protocol reads.
	 */
	(void) set_config_option("default_transaction_isolation", "repeatable read",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SET, true, 0, false);
	/*
	 * Align search_path with openHalo's adapter.c for MySQL connections.
	 *
	 * openHalo sets:
	 *   search_path = "<dbname>, \"$user\", public, mysql, pg_catalog"
	 *
	 * where <dbname> is the PostgreSQL database the MySQL backend is
	 * pinned to (mysql_backend_database, default "postgres").  This
	 * keeps the backend database as the default namespace.  A PostgreSQL
	 * database does not normally imply a same-named schema, so put writable
	 * user namespaces before mysql and pg_catalog.  Otherwise an initial
	 * unqualified CREATE TABLE would select the mysql schema (when installed)
	 * or pg_catalog (when it is not), rather than public.
	 *
	 * mysql remains on the path for unqualified compatibility functions, after
	 * application namespaces in the same way a selected MySQL database takes
	 * precedence over built-ins.
	 *
	 * psql connections are unaffected — they use the standard PG
	 * protocol routine which does not have a session_initialize
	 * callback and keeps the default "$user", public.
	 */
	if (port->database_name && strlen(port->database_name) > 0)
	{
		char	new_search_path[1024];
		snprintf(new_search_path, sizeof(new_search_path),
				 "%s, \"$user\", public, mysql, pg_catalog",
				 port->database_name);
		(void) set_config_option("search_path", new_search_path,
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SET, true, 0, false);
	}
}
static void
mysql_set_remote_dest_receiver_params(DestReceiver *receiver,
                                       struct PortalData *portal)
{
    /*
     * No-op for MySQL DestReceiver.  SetRemoteDestReceiverParams() is
     * specific to the standard PG printtup DestReceiver (DR_printtup)
     * and would overwrite our MysDRState fields (ps, ncols, started)
     * if called here.  MySQL result-set parameters are handled directly
     * by mysDR_rStartup / mysDR_receiveSlot.
     */
    (void) receiver;
    (void) portal;
}
/* ----------------------------------------------------------------
 *    DestReceiver callbacks
 * ----------------------------------------------------------------
 */
/*
 * MySQL text-protocol DestReceiver.
 *
 * Sends MySQL text protocol result sets: column-count → column definitions
 * → EOF → row data → EOF.  Type mapping covers common scalar types.
 */
typedef struct MysDRState
{
    DestReceiver pub;           /* must be first */
    MysPacketState *ps;         /* packet I/O */
    bool         started;       /* result-set metadata already sent */
    int          ncols;         /* number of columns */
    TupleDesc    desc;          /* result descriptor saved at rStartup, so a
                                 * zero-row result can still emit column
                                 * metadata at rShutdown */
} MysDRState;
/*
 * Send the text-protocol result-set header: column-count, one
 * ColumnDefinition41 block per column, and the metadata EOF.  Called from
 * mysDR_receiveSlot on the first row and from mysDR_rShutdown for a
 * zero-row result set -- MySQL defines column metadata by the query's
 * result descriptor, independent of whether any data rows are produced, so
 * a zero-row SELECT / RETURNING must still emit it.
 */
static void
mys_send_result_metadata(MysDRState *dr, TupleDesc desc)
{
    int         ncols = desc->natts;
    uint32      caps = mysql_negotiated_caps(dr->ps);
    int         i;

    /*
     * With CLIENT_OPTIONAL_RESULTSET_METADATA, the result-set header
     * contains column_count followed by metadata_follows (1 for the full
     * ColumnDefinition41 block).  libmysqlclient reads column_count first.
     */
    {
        char colhdr[20];
        int  pos = 0;
        if (ncols < 251)
        {
            colhdr[pos++] = (char) ncols;
        }
        else if (ncols < 65536)
        {
            colhdr[pos++] = (char) 0xFC;
            colhdr[pos++] = (char) (ncols & 0xFF);
            colhdr[pos++] = (char) ((ncols >> 8) & 0xFF);
        }
        else
        {
            colhdr[pos++] = (char) 0xFD;
            colhdr[pos++] = (char) (ncols & 0xFF);
            colhdr[pos++] = (char) ((ncols >> 8) & 0xFF);
            colhdr[pos++] = (char) ((ncols >> 16) & 0xFF);
        }
        if (caps & MYSQL_CAP_OPTIONAL_RESULTSET_METADATA)
            colhdr[pos++] = 1;    /* RESULTSET_METADATA_FULL */
        mysql_packet_write_ok(dr->ps, colhdr, (size_t) pos, 0x00);
    }
    for (i = 0; i < ncols; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(desc, i);
        StringInfoData colbuf;
        initStringInfo(&colbuf);
        /*
         * MySQL ColumnDefinition41 packet (text protocol).
         * All string fields are length-encoded.
         */
        /* catalog */
        appendStringInfoChar(&colbuf, 3);
        appendStringInfoString(&colbuf, "def");
        /* schema = "" */
        appendStringInfoChar(&colbuf, 0);
        /* table alias (use empty for computed columns) */
        appendStringInfoChar(&colbuf, 0);
        /* org_table = "" */
        appendStringInfoChar(&colbuf, 0);
        /* col_name */
        {
            const char *cname = NameStr(attr->attname);
            int         clen = (int) strlen(cname);
            appendStringInfoChar(&colbuf, (char) clen);
            appendBinaryStringInfo(&colbuf, cname, clen);
        }
        /*
         * A PostgreSQL target-list entry does not preserve MySQL's
         * original column identity.  It is therefore an expression from
         * the protocol's point of view, including @@system variables:
         * leave org_name empty, as MySQL 8.4 does for those fields.
         */
        appendStringInfoChar(&colbuf, 0);
        /* length of fixed fields (always 0x0c = 12) */
        appendStringInfoChar(&colbuf, 0x0c);
        /* charset: utf8mb4 = 45 */
        appendStringInfoChar(&colbuf, 0x2D); appendStringInfoChar(&colbuf, 0x00);
        /* column length */
        {
            int32 collen = attr->atttypmod > 0 ? attr->atttypmod : 256;
            mysql_field_append_int4_le(&colbuf, collen);
        }
        /* type: map PostgreSQL type/domain → MySQL type */
        appendStringInfoChar(&colbuf,
                             mysql_field_mysql_type(attr->atttypid));
        /* flags */
        appendStringInfoChar(&colbuf, 0x00); appendStringInfoChar(&colbuf, 0x00);
        /* decimals */
        appendStringInfoChar(&colbuf, 0x00);
        /* filler */
        appendStringInfoChar(&colbuf, 0x00); appendStringInfoChar(&colbuf, 0x00);

        mysql_packet_write_ok(dr->ps, colbuf.data, colbuf.len, 0x00);
        pfree(colbuf.data);
    }
    /*
     * After column definitions: send EOF (or skip if DEPRECATE_EOF).
     * openHalo's sendEOFPacketNoFlush is a no-op when DEPRECATE_EOF
     * is negotiated -- the client knows the metadata section ends
     * after the last ColumnDefinition41 packet.
     */
    if (!(caps & MYSQL_CAP_DEPRECATE_EOF))
    {
        uint16 status = mysql_server_status(0);
        char eof[5] = {0xFE, 0x00, 0x00,
            (char) (status & 0xff), (char) (status >> 8)};
        mysql_packet_write_ok(dr->ps, eof, 5, 0xFE);
    }
    dr->started = true;

    /*
     * Track that a real result set (not CTAS via DestIntoRel) has begun.
     * mysql_end_command uses this to send EOF (for SELECT) vs OK.
     */
    mysql_packet_set_result_started(dr->ps, true);

    /*
     * Flush column metadata before sending row data.  MySQL CLI 8.4.10
     * pipelines the dollar-quote probe (select $$) immediately after
     * receiving column metadata for @@version_comment.  Without an
     * explicit flush here, the metadata and first row may travel in
     * the same TCP segment, and the CLI sends its probe before the
     * server has finished processing the probe — causing a mixed
     * sequence-number stream that confuses libmysqlclient.
     */
    pq_flush();
}

static bool
mysDR_receiveSlot(TupleTableSlot *slot, DestReceiver *self)
{
    MysDRState *dr = (MysDRState *) self;
    int         ncols = slot->tts_tupleDescriptor->natts;
    int         i;

    if (!dr->started)
        mys_send_result_metadata(dr, slot->tts_tupleDescriptor);

    /* Send a data row. */
    {
        StringInfoData rowbuf;
        initStringInfo(&rowbuf);

        for (i = 0; i < ncols; i++)
        {
            bool isnull;
            Datum d = slot_getattr(slot, i + 1, &isnull);
            if (isnull)
            {
                appendStringInfoChar(&rowbuf, 0xFB);  /* NULL */
            }
            else
            {
                char *str;
                bool isvarlena;
                Oid typid;
                Oid typoutput;
                FmgrInfo finfo;
                int slen;

                typid = TupleDescAttr(slot->tts_tupleDescriptor, i)->atttypid;
                getTypeOutputInfo(typid, &typoutput, &isvarlena);
                fmgr_info(typoutput, &finfo);
                str = OutputFunctionCall(&finfo, d);

                /*
                 * MySQL clients expect boolean values as 1/0, not t/f.
                 * Convert PostgreSQL boolean output to MySQL format.
                 */
                if (typid == BOOLOID)
                {
                    if (str[0] == 't')
                    {
                        pfree(str);
                        str = pstrdup("1");
                    }
                    else
                    {
                        pfree(str);
                        str = pstrdup("0");
                    }
                }
                slen = (int) strlen(str);
                /* Length-encoded string */
                if (slen < 251)
                {
                    appendStringInfoChar(&rowbuf, (char) slen);
                }
                else if (slen < 65536)
                {
                    appendStringInfoChar(&rowbuf, 0xFC);
                    appendStringInfoChar(&rowbuf, (char)(slen & 0xFF));
                    appendStringInfoChar(&rowbuf, (char)((slen >> 8) & 0xFF));
                }
                else if (slen < (1 << 24))  /* 0xFD holds up to 2^24-1 */
                {
                    appendStringInfoChar(&rowbuf, 0xFD);
                    appendStringInfoChar(&rowbuf, (char)(slen & 0xFF));
                    appendStringInfoChar(&rowbuf, (char)((slen >> 8) & 0xFF));
                    appendStringInfoChar(&rowbuf, (char)((slen >> 16) & 0xFF));
                }
                else                        /* >= 2^24: 0xFE 8-byte form */
                {
                    uint64  ulen = (uint64) slen;

                    appendStringInfoChar(&rowbuf, 0xFE);
                    for (int bi = 0; bi < 8; bi++)
                        appendStringInfoChar(&rowbuf,
                                             (char) ((ulen >> (bi * 8)) & 0xFF));
                }
                appendBinaryStringInfo(&rowbuf, str, slen);
                pfree(str);
            }
        }

        mysql_packet_write_ok(dr->ps, rowbuf.data, rowbuf.len, 0x00);
        pfree(rowbuf.data);
    }

    return true;
}

static void
mysDR_rStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
    MysDRState *dr = (MysDRState *) self;
    dr->ncols = typeinfo->natts;
    /*
     * Keep a private copy of the result descriptor so a zero-row result
     * set can still emit column metadata at rShutdown (MySQL defines
     * metadata by the descriptor, not by whether rows were produced).
     */
    dr->desc = CreateTupleDescCopy(typeinfo);
    dr->started = false;
}

static void
mysDR_rShutdown(DestReceiver *self)
{
    MysDRState *dr = (MysDRState *) self;
    /*
     * A zero-row result set still carries column metadata: MySQL defines
     * it by the query's result descriptor, not by whether rows were
     * produced.  mysDR_receiveSlot never ran for it, so emit the
     * column-count / ColumnDefinition41 / EOF block now, which also marks
     * result_set_started so mysql_end_command() finishes the result set
     * with EOF rather than a DML-style OK.
     *
     * This deliberately does NOT send the final completion packet (EOF
     * for a result set, OK for a DML statement) -- that stays the
     * responsibility of mysql_end_command(), mirroring openHalo's
     * endCommand, so a packet is not duplicated here.
     *
     * On an execution error this callback is not reached (the error
     * propagates past ExecutorEnd), so the client sees a bare ERR packet
     * -- matching MySQL, which never emits a half result set before an
     * error.
     */
    if (!dr->started)
        mys_send_result_metadata(dr, dr->desc);

    if (dr->desc != NULL)
    {
        FreeTupleDesc(dr->desc);
        dr->desc = NULL;
    }
}

static void
mysDR_rDestroy(DestReceiver *self)
{
    pfree(self);
}

static DestReceiver *
mysql_create_dest_receiver(CommandDest dest)
{
    if (dest == DestRemote || dest == DestRemoteExecute ||
        dest == DestRemoteSimple)
    {
        MysDRState *dr = (MysDRState *) palloc0(sizeof(MysDRState));
        dr->pub.receiveSlot = mysDR_receiveSlot;
        dr->pub.rStartup = mysDR_rStartup;
        dr->pub.rShutdown = mysDR_rShutdown;
        dr->pub.rDestroy = mysDR_rDestroy;
        dr->pub.mydest = dest;
        dr->ps = mysql_ps();
        return (DestReceiver *) dr;
    }
    return standard_CreateDestReceiver(dest);
}

static void
mysql_end_command(const QueryCompletion *qc,
                  CommandDest dest,
                  bool force_undecorated_output)
{
    if (dest == DestRemote || dest == DestRemoteExecute ||
        dest == DestRemoteSimple)
    {
		uint32      caps = mysql_negotiated_caps(mysql_ps());
		uint16		status = mysql_server_status(0);

		/*
		 * NOTICE/WARNING/INFO messages suppressed by mysql_send_error()
		 * during this statement.  The OK/EOF warning-count field is 16
		 * bits; clamp instead of wrapping if something pathological
		 * generated more than 65535 of them.
		 */
		uint16      warnings = (uint16) Min(mysql_packet_get_warning_count(mysql_ps()),
		                                    65535);

        /*
         * Mirror openHalo's endCommand: a statement that actually sent a
         * result set (column definitions + rows via mysDR_receiveSlot)
         * gets EOF; everything else gets a DML-style OK.  This must key
         * off result_set_started alone, not commandTag == CMDTAG_SELECT:
         *
         *   - PG18 ExecCreateTableAs uses CMDTAG_SELECT for CTAS, but no
         *     result set was ever started (DestIntoRel) -- result_started
         *     stays false, so CTAS correctly falls through to OK.
         *   - DELETE/UPDATE ... RETURNING tag as CMDTAG_DELETE/UPDATE, not
         *     CMDTAG_SELECT, but DO start a real result set through the
         *     same mysDR_receiveSlot path SELECT uses.  Gating on
         *     commandTag here previously sent a DML OK packet after the
         *     client had already received column metadata and rows
         *     expecting a terminating EOF, desyncing the session (the
         *     client CLI hangs waiting for the packet it never got).
         */
        if (mysql_packet_get_result_started(mysql_ps()))
        {
			/* Track row count for FOUND_ROWS() */
			mysql_packet_set_found_rows(mysql_ps(), qc->nprocessed);

            if (caps & MYSQL_CAP_DEPRECATE_EOF)
            {
                /*
                 * DEPRECATE_EOF OK packet (7-byte payload, packet < 9 so
                 * is_eof_packet() returns true; both old- and new-style
                 * clients interpret this as end-of-result-set correctly).
                 */
                char ok[7];
                int  pos = 0;
                ok[pos++] = 0xFE;                    /* header */
                ok[pos++] = 0x00;                    /* affected_rows (lenenc 0) */
                ok[pos++] = 0x00;                    /* last_insert_id (lenenc 0) */
                ok[pos++] = (char) (status & 0xff);
                ok[pos++] = (char) (status >> 8);
                ok[pos++] = (char) (warnings & 0xff);
                ok[pos++] = (char) (warnings >> 8);
                mysql_packet_write_ok(mysql_ps(), ok, (size_t) pos, 0xFE);
            }
            else
            {
                /* Traditional EOF to mark end of result set. */
                char eof[5] = {0xFE,
                    (char) (warnings & 0xff), (char) (warnings >> 8),
                    (char) (status & 0xff), (char) (status >> 8)};
                mysql_packet_write_ok(mysql_ps(), eof, 5, 0xFE);
            }
        }
        else
        {
            /*
             * Send a MySQL OK packet for INSERT/UPDATE/DELETE and other
             * non-SELECT commands.
             */
            char ok[256];
            int  pos = 0;
            /*
             * LAST_INSERT_ID() and ROW_COUNT() were captured before
             * finish_xact_command() via capture_session_state, while
             * the transaction (and lastval()) is still valid.
             */
            uint64 last_id = mysql_packet_get_last_insert_id(mysql_ps());


			ok[pos++] = 0x00;                    /* OK header */
			pos += mysql_write_lenenc_uint64(ok + pos, qc->nprocessed);
			pos += mysql_write_lenenc_uint64(ok + pos, last_id);
			ok[pos++] = (char) (status & 0xff);
			ok[pos++] = (char) (status >> 8);
            ok[pos++] = (char) (warnings & 0xff);
            ok[pos++] = (char) (warnings >> 8);

            mysql_packet_write_ok(mysql_ps(), ok, (size_t) pos, 0x00);
        }

        /*
         * The warning count just sent belongs to this statement.  The
         * retained diagnostics are kept so SHOW WARNINGS can report them
         * (the next statement's boundary in mysql_before_simple_query_
         * statement drops them); only the "current statement produced a
         * warning" marker is cleared, so the next statement's first
         * diagnostic starts a fresh list.
         */
        mysql_packet_reset_stmt_warning_state(mysql_ps());

        /*
         * Reset sequence numbers for the next command.
         * MySQL protocol resets seq after each command completes.
         */
		if (!mysql_simple_query_more_results)
		{
			mysql_packet_reset_seq(mysql_ps());
			mysql_packet_set_server_seq(mysql_ps(), 1);
			mysql_packet_set_result_started(mysql_ps(), false);
		}
    }
    else
    {
        standard_EndCommand(qc, dest, force_undecorated_output);
    }
}

static void
mysql_null_command(CommandDest dest)
{
    if (dest == DestRemote || dest == DestRemoteExecute ||
        dest == DestRemoteSimple)
    {
        /* Empty query → MySQL OK. */
        uint16 status = mysql_server_status(0);
        uint16 warnings = (uint16) Min(mysql_packet_get_warning_count(mysql_ps()),
                                       65535);
        char ok[7];
        int  pos = 0;
        ok[pos++] = 0x00;
        ok[pos++] = 0x00;
        ok[pos++] = 0x00;
        ok[pos++] = (char) (status & 0xff);
        ok[pos++] = (char) (status >> 8);
        ok[pos++] = (char) (warnings & 0xff);
        ok[pos++] = (char) (warnings >> 8);
        mysql_packet_write_ok(mysql_ps(), ok, (size_t) pos, 0x00);
        /* Keep retained diagnostics for SHOW WARNINGS, like end_command. */
        mysql_packet_reset_stmt_warning_state(mysql_ps());

        /*
         * Reset sequence numbers for the next command, mirroring
         * mysql_end_command.  A comment-only or all-whitespace statement
         * (non-empty query text that parses to zero raw statements) takes
         * this path instead of mysql_end_command, and without this reset
         * the next command's client packet arrives with seq=0 while the
         * server still expects the previous command's continuation seq,
         * producing "MySQL packet sequence number mismatch" and dropping
         * the connection.
         */
		if (!mysql_simple_query_more_results)
		{
			mysql_packet_reset_seq(mysql_ps());
			mysql_packet_set_server_seq(mysql_ps(), 1);
			mysql_packet_set_result_started(mysql_ps(), false);
		}
    }
    else
    {
        standard_NullCommand(dest);
    }
}

static void
mysql_send_ready_for_query(CommandDest dest)
{
    /*
     * MySQL protocol has no explicit "ready for query" packet.  The auth
     * OK (sent by the authenticate callback) and the query-completion OK
     * (sent by end_command) already serve as implicit ready signals.
     *
     * We still flush the output buffer to make sure all pending writes
     * reach the client before the backend blocks on the next read.
     */
    if (dest == DestRemote || dest == DestRemoteExecute ||
        dest == DestRemoteSimple)
    {
        pq_flush();
    }
    else
    {
        standard_ReadyForQuery(dest);
    }
}

/* ----------------------------------------------------------------
 *    Error / GUC callbacks
 * ----------------------------------------------------------------
 */
/*
 * Map a PostgreSQL ErrorData to its MySQL wire-protocol error code and
 * SQLSTATE.  Shared by mysql_send_error() (which emits the ERR packet for
 * real errors) and the SHOW WARNINGS machinery (which reports the same
 * code for suppressed NOTICE/WARNING/INFO diagnostics).
 */
static void
mysql_map_error(ErrorData *edata, uint16 *errcode, const char **sqlstate)
{
	bool		mysql_label_duplicate =
		(strncmp(edata->message, "ENUM contains duplicate value ", 30) == 0 ||
		 strncmp(edata->message, "SET contains duplicate value ", 29) == 0);
	bool		mysql_label_invalid =
		(strcmp(edata->message, "invalid value for MySQL ENUM") == 0 ||
		 strcmp(edata->message, "invalid value for MySQL SET") == 0);

	if (mysql_label_duplicate)
	{
		*errcode = 1291;            /* ER_DUPLICATED_VALUE_IN_TYPE */
		*sqlstate = "HY000";
	}
	else if (mysql_label_invalid)
	{
		*errcode = 1265;            /* ER_DATA_TRUNCATED in strict mode */
		*sqlstate = "01000";
	}
	else if (strncmp(edata->message, "Too big precision ", 18) == 0)
	{
		/*
		 * mys_validate_var_datatype_scale() (mys_adtext.c) rejects MySQL
		 * DDL conditions that PostgreSQL's own limits would silently
		 * accept.  Those ereports carry ERRCODE_INVALID_PARAMETER_VALUE,
		 * which the generic mapping below would collapse into
		 * ER_WRONG_ARGUMENTS (1210); give each condition its real MySQL
		 * error code instead, so drivers/migration tools see the correct
		 * numeric code and SQLSTATE.  "Too big scale" covers both
		 * scale > 30 and scale > precision -- real MySQL reports both as
		 * ER_TOO_BIG_SCALE (1425).
		 */
		*errcode = 1426;            /* ER_TOO_BIG_PRECISION */
		*sqlstate = "42000";
	}
	else if (strncmp(edata->message, "Too big scale ", 14) == 0)
	{
		*errcode = 1425;            /* ER_TOO_BIG_SCALE */
		*sqlstate = "42000";
	}
	else if (strncmp(edata->message, "Column length too big ", 22) == 0)
	{
		*errcode = 1074;            /* ER_TOO_BIG_FIELDLENGTH */
		*sqlstate = "42000";
	}
	else
    switch (edata->sqlerrcode)
    {
    case ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION:
    case ERRCODE_INVALID_PASSWORD:
    case ERRCODE_INSUFFICIENT_PRIVILEGE:
        *errcode = 1045;
        *sqlstate = "28000";
        break;
    case ERRCODE_PROTOCOL_VIOLATION:
        *errcode = 1043;
        *sqlstate = "08S01";
        break;
    case ERRCODE_UNDEFINED_TABLE:
        *errcode = 1146;
        *sqlstate = "42S02";
        break;
    case ERRCODE_UNDEFINED_COLUMN:
        *errcode = 1054;
        *sqlstate = "42S22";
        break;
	case ERRCODE_INVALID_SCHEMA_NAME:
		*errcode = 1049;        /* ER_BAD_DB_ERROR */
		*sqlstate = "42000";
		break;
    case ERRCODE_DUPLICATE_TABLE:
        *errcode = 1050;
        *sqlstate = "42S01";
        break;
    case ERRCODE_UNIQUE_VIOLATION:
        *errcode = 1062;        /* ER_DUP_ENTRY */
        *sqlstate = "23000";
        break;
    case ERRCODE_NOT_NULL_VIOLATION:
        *errcode = 1048;        /* ER_BAD_NULL_ERROR */
        *sqlstate = "23000";
        break;
    case ERRCODE_CHECK_VIOLATION:
        *errcode = 3819;        /* ER_CHECK_CONSTRAINT_VIOLATED */
        *sqlstate = "HY000";
        break;
    case ERRCODE_INVALID_DATETIME_FORMAT:
    case ERRCODE_DATETIME_VALUE_OUT_OF_RANGE:
        *errcode = 1292;        /* ER_TRUNCATED_WRONG_VALUE */
        *sqlstate = "22007";
        break;
    case ERRCODE_INVALID_REGULAR_EXPRESSION:
        *errcode = 1139;        /* ER_REGEXP_ERROR */
        *sqlstate = "42000";
        break;
    case ERRCODE_UNDEFINED_FUNCTION:
        *errcode = 1305;        /* ER_SP_DOES_NOT_EXIST */
        *sqlstate = "42000";
        break;
    case ERRCODE_DATATYPE_MISMATCH:
    case ERRCODE_CANNOT_COERCE:
        *errcode = 1210;        /* ER_WRONG_ARGUMENTS */
        *sqlstate = "HY000";
        break;
    case ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE:
    case ERRCODE_INVALID_TEXT_REPRESENTATION:
        *errcode = 1210;        /* ER_WRONG_ARGUMENTS */
        *sqlstate = "HY000";
        break;
    case ERRCODE_SYNTAX_ERROR:
        *errcode = 1064;
        *sqlstate = "42000";
        break;
    case ERRCODE_UNDEFINED_PSTATEMENT:
        *errcode = 1243;        /* ER_UNKNOWN_STMT_HANDLER */
        *sqlstate = "HY000";
        break;
    case ERRCODE_INVALID_CURSOR_STATE:
        *errcode = 1325;        /* ER_STMT_HAS_NO_OPEN_CURSOR */
        *sqlstate = "24000";
        break;
    case ERRCODE_FEATURE_NOT_SUPPORTED:
        *errcode = 1295;        /* ER_UNSUPPORTED_PS */
        *sqlstate = "HY000";
        break;
    case ERRCODE_INVALID_PARAMETER_VALUE:
        *errcode = 1210;        /* ER_WRONG_ARGUMENTS */
        *sqlstate = "HY000";
        break;
    default:
        *errcode = 1105;        /* ER_UNKNOWN_ERROR */
        *sqlstate = "HY000";
        break;
    }
}

static void
mysql_send_error(ErrorData *edata)
{
    uint16      errcode;
    const char *sqlstate;

	/*
	 * Suppress non-error messages (NOTICE, WARNING, INFO) on the MySQL
	 * protocol.  The MySQL wire protocol has no concept of a server-side
	 * notice or warning frame independent of the command-completion OK
	 * packet.  Sending every PG NOTICE as an ERR (0xFF) packet would
	 * break IF [NOT] EXISTS semantics — the client would see ERROR
	 * where MySQL would return a successful OK with a warning count.
	 *
	 * Only ERROR, FATAL, and PANIC severities produce a real ERR packet.
	 * Each suppressed diagnostic is retained in the session diagnostics
	 * area (with its MySQL error code and message text) so SHOW WARNINGS
	 * can report it; the next completion packet's warning-count field
	 * reflects how many were suppressed.
	 */
	if (edata->elevel < ERROR)
	{
		mysql_map_error(edata, &errcode, &sqlstate);
		mysql_packet_add_warning(mysql_ps(), edata->elevel, errcode,
								 sqlstate, edata->message);
		return;
	}

	mysql_map_error(edata, &errcode, &sqlstate);

	mysql_simple_query_more_results = false;
    mysql_packet_write_err(mysql_ps(), errcode, sqlstate,
                           "%s", edata->message);

    /* Error paths do not reach mysql_send_ready_for_query(). */
    pq_flush();

    /*
     * Reset sequence numbers for the next command, just like
     * mysql_end_command does after a successful query.
     */
    mysql_packet_reset_seq(mysql_ps());
    mysql_packet_set_server_seq(mysql_ps(), 1);
    mysql_packet_set_result_started(mysql_ps(), false);
    mysql_packet_reset_warning_count(mysql_ps());
}

static void
mysql_report_parameter_status(const char *name, const char *value)
{
    /*
     * MySQL clients do not expect PostgreSQL ParameterStatus messages
     * ('S' packets).  Silently drop them.  In future we may translate
     * selected GUC reports into MySQL session-track messages.
     */
}

/* ----------------------------------------------------------------
 *    ProtocolRoutine instance
 * ----------------------------------------------------------------
 */
ProtocolRoutine MySQLProtocolRoutine = {
    .kind = COMPAT_PROTOCOL_MYSQL,
    .name = "MySQL",

    .init = mysql_init,
    .startup_exchange = mysql_startup_exchange,
    .authenticate = mysql_authenticate,
    .mainfunc = NULL,               /* use standard PostgresMain loop  */

    .read_command = mysql_read_command_cb,
    .process_command = mysql_process_command,
    .comm_reset = mysql_comm_reset,
    .is_reading_msg = mysql_is_reading_msg,

    .session_initialize = mysql_session_initialize,
    .send_backend_key_data = mysql_send_backend_key_data_noop,

    .create_dest_receiver = mysql_create_dest_receiver,
    .set_remote_dest_receiver_params = mysql_set_remote_dest_receiver_params,
    .end_command = mysql_end_command,
    .null_command = mysql_null_command,
    .send_ready_for_query = mysql_send_ready_for_query,

    .allow_multi_statements = mysql_allow_multi_statements,
    .simple_query_statement_ends_xact = mysql_simple_query_statement_ends_xact,
    .set_simple_query_more_results = mysql_set_simple_query_more_results,
    .before_simple_query_statement = mysql_before_simple_query_statement,
    .capture_session_state = mysql_capture_session_state,

    .send_error = mysql_send_error,
    .report_parameter_status = mysql_report_parameter_status,

    .process_utility = mys_standard_ProcessUtility,

    /*
     * parser_routine is filled at module load (see aux_mysql_init.c
     * _PG_init): the MySQL parser ships as a loadable module, so it
     * cannot be referenced here.
     */
};

/*
 * G3 getters for the kernel SQL helpers (mys_found_rows etc.): report
 * the current connection's packet state.  Published through
 * mys_session_state_exports by _PG_init (aux_mysql_init.c).
 */
static uint64
mys_session_found_rows(void)
{
	return mysql_packet_get_found_rows(mysql_ps());
}

static uint64
mys_session_last_insert_id(void)
{
	return mysql_packet_get_last_insert_id(mysql_ps());
}

static uint64
mys_session_row_count(void)
{
	return mysql_packet_get_row_count(mysql_ps());
}

/* Published to the kernel via mys_session_state_exports (see below). */
MysSessionStateExports mys_session_state_exports_data = {
	.found_rows = mys_session_found_rows,
	.last_insert_id = mys_session_last_insert_id,
	.row_count = mys_session_row_count,
};

/*
 * mysql.show_warnings()
 *
 * Backing implementation of SHOW WARNINGS: report the diagnostics
 * (Level / Code / Message) retained for the previous statement, mirroring
 * MySQL's SHOW WARNINGS result shape.  SHOW COUNT(*) WARNINGS (mys_gram.y)
 * reparses to a COUNT(*) wrapper over this same SRF rather than a separate
 * backing function.
 *
 * Level is "Warning" for WARNING, "Error" for ERROR/FATAL/PANIC, and
 * "Note" for NOTICE/INFO (MySQL never retains ERROR here, since
 * mysql_send_error() only stores sub-ERROR severities).  Code is the
 * MySQL wire-protocol error code and Message the PG error text.
 */
PG_FUNCTION_INFO_V1(mysql_show_warnings);
Datum
mysql_show_warnings(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;
	MemoryContext per_query_ctx;
	MysWarning *warnings = NULL;
	int			nwarnings;
	int			i;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		(rsinfo->allowedModes & SFRM_Materialize) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("return type must be a row type")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = CreateTupleDescCopy(tupdesc);

	MemoryContextSwitchTo(oldcontext);

	nwarnings = mysql_packet_get_warnings(mysql_ps(), &warnings);
	for (i = 0; i < nwarnings; i++)
	{
		Datum		values[3];
		bool		nulls[3] = {false, false, false};
		const char *level_str;

		switch (warnings[i].level)
		{
			case ERROR:
				level_str = "Error";
				break;
			case WARNING:
				level_str = "Warning";
				break;
			case NOTICE:
			case INFO:
			default:
				level_str = "Note";
				break;
		}
		values[0] = CStringGetTextDatum(level_str);
		values[1] = Int32GetDatum((int32) warnings[i].errcode);
		values[2] = CStringGetTextDatum(warnings[i].message);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
