/*-------------------------------------------------------------------------
 *
 * mysql_stmt.c
 *    MySQL COM_STMT_PREPARE lifecycle and metadata encoding.
 *
 * A wire prepared statement owns a saved CachedPlanSource.  Its parser
 * setup hook is deliberately MySQL-specific, so invalidation/revalidation
 * does not silently analyze a MySQL '?' parameter as PostgreSQL syntax.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "access/xact.h"
#include "adapter/mysql/mysql_packet.h"
#include "adapter/mysql/mysql_stmt.h"
#include "adapter/mysql/systemVar.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "parser/analyze.h"
#include "parser/parse_param.h"
#include "parser/parsereng.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "utils/elog.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/plancache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "tcop/utility.h"

typedef struct MysStmt
{
	uint32			id;
	MemoryContext	context;
	CachedPlanSource *plansource;
	Oid			   *param_types;
	int				num_params;
	uint8		   *mysql_types;
	uint8		   *unsigned_flags;
	bool			have_mysql_types;
	StringInfoData *long_data;
	bool		   *has_long_data;
	bool			long_data_invalid;
	char	   *cursor_portal_name;
} MysStmt;

typedef struct MysStmtEntry
{
	uint32			id;
	MysStmt		   *stmt;
} MysStmtEntry;

typedef struct MysStmtSession
{
	MemoryContext	context;
	HTAB		   *statements;
	uint32			next_id;
} MysStmtSession;

static MysStmtSession *mys_stmt_session = NULL;

static MysStmtSession *mys_stmt_get_session(void);
static void mys_stmt_parser_setup(ParseState *pstate, void *arg);
static uint8 mys_stmt_mysql_type(Oid typid);
static void mys_stmt_append_lenenc(StringInfo buf, const char *value, int len);
static void mys_stmt_send_column(MysPacketState *ps, const char *name,
								 Oid typid, int32 typmod);
static void mys_stmt_send_metadata_end(MysPacketState *ps);
static void mys_stmt_finish_command(MysPacketState *ps);
static MysStmt *mys_stmt_lookup(uint32 id, bool missing_ok);
static uint32 mys_stmt_read_id(StringInfo inBuf);
static void mys_stmt_require(StringInfo inBuf, int pos, int need);
static uint64 mys_stmt_read_le(const char *data, int bytes);
static uint64 mys_stmt_read_lenenc(StringInfo inBuf, int *pos);
static char *mys_stmt_decode_param(StringInfo inBuf, int *pos,
											 uint8 mysql_type, bool is_unsigned, Oid pgtype);
static char *mys_stmt_decode_long_data(MysStmt *stmt, int paramno,
													 Oid pgtype);
static ParamListInfo mys_stmt_bind_params(MysStmt *stmt, StringInfo inBuf,
												bool *open_cursor);
static void mys_stmt_clear_long_data(MysStmt *stmt);
static Portal mys_stmt_get_cursor_portal(MysStmt *stmt);
static void mys_stmt_close_cursor(MysStmt *stmt);
static const char *mys_stmt_cursor_name(MysStmt *stmt);

typedef struct MysStmtDestReceiver
{
	DestReceiver pub;
	MysPacketState *ps;
	bool		send_header;
} MysStmtDestReceiver;

static bool mys_stmt_receive_slot(TupleTableSlot *slot, DestReceiver *self);
static void mys_stmt_rstartup(DestReceiver *self, int operation,
									  TupleDesc typeinfo);
static void mys_stmt_rshutdown(DestReceiver *self);
static void mys_stmt_rdestroy(DestReceiver *self);
static DestReceiver *mys_stmt_create_receiver(MysPacketState *ps,
											 bool send_header);
static void mys_stmt_send_result_header(MysPacketState *ps, TupleDesc desc,
											 bool send_metadata_end);
static void mys_stmt_send_cursor_end(MysPacketState *ps, uint16 status);
static uint16 mys_stmt_server_status(uint16 extra_status);
static void mys_stmt_append_binary_value(StringInfo buf, Oid typid, Datum value);

typedef struct MysStmtUnknownParamContext
{
	Oid		   *param_types;
	int			num_params;
} MysStmtUnknownParamContext;

static Node *mys_stmt_default_unknown_param_mutator(Node *node, void *arg);
static Query *mys_stmt_default_unknown_params(Query *query,
														Oid *param_types, int num_params);
static bool mys_stmt_resolve_ambiguous_numeric_params_walker(Node *node,
																			 void *arg);
static void mys_stmt_resolve_ambiguous_numeric_params(RawStmt *rawstmt);

static MysStmtSession *
mys_stmt_get_session(void)
{
	HASHCTL		ctl;
	MemoryContext oldcontext;

	if (mys_stmt_session != NULL)
		return mys_stmt_session;

	mys_stmt_session = MemoryContextAllocZero(TopMemoryContext,
															 sizeof(MysStmtSession));
	mys_stmt_session->context = AllocSetContextCreate(TopMemoryContext,
																		 "MySQL prepared statements",
																		 ALLOCSET_DEFAULT_SIZES);
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(uint32);
	ctl.entrysize = sizeof(MysStmtEntry);
	ctl.hcxt = mys_stmt_session->context;
	oldcontext = MemoryContextSwitchTo(mys_stmt_session->context);
	mys_stmt_session->statements = hash_create("MySQL prepared statements",
																 16, &ctl,
																 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	MemoryContextSwitchTo(oldcontext);
	mys_stmt_session->next_id = 1;
	return mys_stmt_session;
}

static void
mys_stmt_parser_setup(ParseState *pstate, void *arg)
{
	MysStmt    *stmt = (MysStmt *) arg;

	pstate->p_parser_routine = parserengine;
	setup_parse_fixed_parameters(pstate, stmt->param_types, stmt->num_params);
}

/*
 * PostgreSQL deliberately rejects an unresolved external parameter (for
 * example SELECT $1).  MySQL accepts the corresponding '?' marker and
 * treats it as a string until a surrounding expression provides a narrower
 * type.  Preserve every type inferred by PostgreSQL, and default only the
 * still-unknown parameters to text before rewrite/planning.  The saved
 * parser setup hook above then applies the same types on plan revalidation.
 */
static Node *
mys_stmt_default_unknown_param_mutator(Node *node, void *arg)
{
	MysStmtUnknownParamContext *context = arg;

	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXTERN && param->paramtype == UNKNOWNOID)
		{
			Param	   *newparam;

			if (param->paramid <= 0 || param->paramid > context->num_params)
				elog(ERROR, "invalid MySQL prepared statement parameter number");
			newparam = copyObject(param);
			newparam->paramtype = TEXTOID;
			newparam->paramcollid = get_typcollation(TEXTOID);
			context->param_types[param->paramid - 1] = TEXTOID;
			return (Node *) newparam;
		}
		return copyObject(node);
	}
	if (IsA(node, Query))
		return (Node *) query_tree_mutator((Query *) node,
											 mys_stmt_default_unknown_param_mutator,
											 context, 0);
	return expression_tree_mutator(node, mys_stmt_default_unknown_param_mutator,
										 context);
}

static Query *
mys_stmt_default_unknown_params(Query *query, Oid *param_types, int num_params)
{
	MysStmtUnknownParamContext context;

	context.param_types = param_types;
	context.num_params = num_params;
	return query_tree_mutator(query, mys_stmt_default_unknown_param_mutator,
									  &context, 0);
}

/*
 * A MySQL parameter marker has no fixed type until its context (or the
 * execute packet) supplies one.  PostgreSQL can infer the type of "? + 1",
 * but it rejects "? + ?" before the post-analysis UNKNOWN-to-text fallback
 * above has a chance to run.  MySQL 8.4 prepares the latter as a numeric
 * expression (and returns a DOUBLE result for integer bindings).  Make that
 * narrow, otherwise ambiguous numeric form explicit in the raw tree so the
 * normal PG18 analysis and cached-plan paths can proceed.
 *
 * Do not default every marker up front: doing so would discard useful
 * contextual inference for strings, dates, columns, and a marker paired
 * with an explicitly typed literal.
 */
static bool
mys_stmt_resolve_ambiguous_numeric_params_walker(Node *node, void *arg)
{
	A_Expr		 *expr;
	const char *opname;
	TypeCast	 *leftcast;
	TypeCast	 *rightcast;

	(void) arg;
	if (node == NULL)
		return false;
	/*
	 * The core raw-expression walker deliberately knows nothing about the
	 * MySQL-only raw nodes.  They are lowered by the grammar to
	 * pg_catalog.mys_get_user_var / mys_set_user_var / etc. FuncCall nodes;
	 * this narrow pre-analysis pass only needs to visit the RHS of a user
	 * assignment, where an ambiguous ? + ? may still occur.
	 */
	if (IsA(node, FuncCall))
	{
		FuncCall   *fn = (FuncCall *) node;
		ListCell   *lc;

		/* user/system variable reads are leaf references */
		if (list_length(fn->funcname) == 2 &&
			pg_strcasecmp(strVal(linitial(fn->funcname)), "pg_catalog") == 0)
		{
			char	   *fname = strVal(lsecond(fn->funcname));

			if (pg_strcasecmp(fname, "mys_get_user_var") == 0 ||
				pg_strcasecmp(fname, "mys_get_system_variable") == 0)
				return false;
		}

		foreach(lc, fn->args)
		{
			if (mys_stmt_resolve_ambiguous_numeric_params_walker(
					(Node *) lfirst(lc), arg))
				return true;
		}
		return false;
	}
	if (IsA(node, A_Expr))
	{
		expr = (A_Expr *) node;
		if (expr->kind == AEXPR_OP && list_length(expr->name) == 1 &&
			IsA(expr->lexpr, ParamRef) && IsA(expr->rexpr, ParamRef))
		{
			opname = strVal(linitial(expr->name));
			if (strcmp(opname, "+") == 0 || strcmp(opname, "-") == 0 ||
				strcmp(opname, "*") == 0 || strcmp(opname, "/") == 0 ||
				strcmp(opname, "%") == 0)
			{
				leftcast = makeNode(TypeCast);
				leftcast->arg = expr->lexpr;
				leftcast->typeName = makeTypeNameFromOid(FLOAT8OID, -1);
				leftcast->location = exprLocation(expr->lexpr);
				rightcast = makeNode(TypeCast);
				rightcast->arg = expr->rexpr;
				rightcast->typeName = makeTypeNameFromOid(FLOAT8OID, -1);
				rightcast->location = exprLocation(expr->rexpr);
				expr->lexpr = (Node *) leftcast;
				expr->rexpr = (Node *) rightcast;
			}
		}
	}
	return raw_expression_tree_walker(node,
										 mys_stmt_resolve_ambiguous_numeric_params_walker,
										 arg);
}

static void
mys_stmt_resolve_ambiguous_numeric_params(RawStmt *rawstmt)
{
	(void) raw_expression_tree_walker(rawstmt->stmt,
											 mys_stmt_resolve_ambiguous_numeric_params_walker,
											 NULL);
}

static uint8
mys_stmt_mysql_type(Oid typid)
{
	HeapTuple	typetup;
	Form_pg_type typeform;

	/*
	 * Private ENUM/SET domains are user-defined and therefore have normal
	 * object OIDs.  Keep builtin and synthetic result types on the fast path:
	 * prepared system-variable expressions can retain special non-domain
	 * descriptors while their plans are being revalidated.
	 */
	if (typid >= FirstNormalObjectId)
	{
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
	}

	switch (typid)
	{
		case BOOLOID:
			return 1;               /* MYSQL_TYPE_TINY */
		case INT2OID:
			return 2;               /* MYSQL_TYPE_SHORT */
		case INT4OID:
			return 3;               /* MYSQL_TYPE_LONG */
		case INT8OID:
			return 8;               /* MYSQL_TYPE_LONGLONG */
		case FLOAT4OID:
			return 4;               /* MYSQL_TYPE_FLOAT */
		case FLOAT8OID:
			return 5;               /* MYSQL_TYPE_DOUBLE */
		case NUMERICOID:
			return 246;             /* MYSQL_TYPE_NEWDECIMAL */
		case DATEOID:
			return 10;              /* MYSQL_TYPE_DATE */
		case TIMEOID:
			return 11;              /* MYSQL_TYPE_TIME */
		case TIMESTAMPOID:
			return 12;              /* MYSQL_TYPE_DATETIME */
		case TIMESTAMPTZOID:
			return 7;               /* MYSQL_TYPE_TIMESTAMP */
		case BYTEAOID:
			return 252;             /* MYSQL_TYPE_BLOB */
		case BPCHAROID:
			return 254;             /* MYSQL_TYPE_STRING */
		default:
			return 253;             /* MYSQL_TYPE_VAR_STRING */
	}
}

static void
mys_stmt_append_lenenc(StringInfo buf, const char *value, int len)
{
	if (len < 251)
		appendStringInfoChar(buf, (char) len);
	else if (len < 65536)
	{
		appendStringInfoChar(buf, 0xfc);
		appendStringInfoChar(buf, (char) (len & 0xff));
		appendStringInfoChar(buf, (char) ((len >> 8) & 0xff));
	}
	else
	{
		appendStringInfoChar(buf, 0xfd);
		appendStringInfoChar(buf, (char) (len & 0xff));
		appendStringInfoChar(buf, (char) ((len >> 8) & 0xff));
		appendStringInfoChar(buf, (char) ((len >> 16) & 0xff));
	}
	appendBinaryStringInfo(buf, value, len);
}

/*
 * Append a 4-byte little-endian integer, as the MySQL wire protocol
 * mandates for fields like column length regardless of server
 * architecture -- unlike appendBinaryStringInfo(buf, (char *) &v, 4), this
 * is correct on big-endian hosts too.
 */
static void
mys_stmt_append_int4_le(StringInfo buf, int32 value)
{
	uint8 le[4];

	le[0] = (uint8) (value & 0xFF);
	le[1] = (uint8) ((value >> 8) & 0xFF);
	le[2] = (uint8) ((value >> 16) & 0xFF);
	le[3] = (uint8) ((value >> 24) & 0xFF);
	appendBinaryStringInfo(buf, (char *) le, 4);
}

static void
mys_stmt_send_column(MysPacketState *ps, const char *name, Oid typid,
					 int32 typmod)
{
	StringInfoData buf;
	int32		collen = typmod > 0 ? typmod : 256;

	initStringInfo(&buf);
	mys_stmt_append_lenenc(&buf, "def", 3);
	mys_stmt_append_lenenc(&buf, "", 0);
	mys_stmt_append_lenenc(&buf, "", 0);
	mys_stmt_append_lenenc(&buf, "", 0);
	mys_stmt_append_lenenc(&buf, name, strlen(name));
	mys_stmt_append_lenenc(&buf, "", 0);
	appendStringInfoChar(&buf, 0x0c);
	appendStringInfoChar(&buf, 0x2d);
	appendStringInfoChar(&buf, 0x00);
	mys_stmt_append_int4_le(&buf, collen);
	appendStringInfoChar(&buf, mys_stmt_mysql_type(typid));
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	appendStringInfoChar(&buf, 0x00);
	mysql_packet_write(ps, buf.data, buf.len);
	pfree(buf.data);
}

static void
mys_stmt_send_metadata_end(MysPacketState *ps)
{
	if (!(mysql_negotiated_caps(ps) & MYSQL_CAP_DEPRECATE_EOF))
	{
		uint16		status = mys_stmt_server_status(0);
		char		eof[5] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8)};

		mysql_packet_write(ps, eof, sizeof(eof));
	}
}

static void
mys_stmt_finish_command(MysPacketState *ps)
{
	pq_flush();
	mysql_packet_reset_seq(ps);
	mysql_packet_set_server_seq(ps, 1);
}

static MysStmt *
mys_stmt_lookup(uint32 id, bool missing_ok)
{
	MysStmtSession *session = mys_stmt_get_session();
	MysStmtEntry *entry;

	entry = hash_search(session->statements, &id, HASH_FIND, NULL);
	if (entry == NULL && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("unknown MySQL prepared statement handler (%u)", id)));
	return entry == NULL ? NULL : entry->stmt;
}

/*
 * A held portal is deliberately found by name for every wire command.  It
 * can outlive the transaction that created it, while a raw Portal pointer
 * cannot safely be retained across abort cleanup.
 */
static const char *
mys_stmt_cursor_name(MysStmt *stmt)
{
	MemoryContext oldcontext;

	if (stmt->cursor_portal_name != NULL)
		return stmt->cursor_portal_name;
	oldcontext = MemoryContextSwitchTo(stmt->context);
	stmt->cursor_portal_name = psprintf("<mysql-stmt-%d-cursor-%u>",
											MyProcPid, stmt->id);
	MemoryContextSwitchTo(oldcontext);
	return stmt->cursor_portal_name;
}

static Portal
mys_stmt_get_cursor_portal(MysStmt *stmt)
{
	if (stmt->cursor_portal_name == NULL)
		return NULL;
	return GetPortalByName(stmt->cursor_portal_name);
}

static void
mys_stmt_close_cursor(MysStmt *stmt)
{
	Portal		portal = mys_stmt_get_cursor_portal(stmt);

	if (portal != NULL)
		PortalDrop(portal, false);
}

static uint32
mys_stmt_read_id(StringInfo inBuf)
{
	const unsigned char *p;

	if (inBuf->len != 4)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid MySQL prepared statement command length")));
	p = (const unsigned char *) inBuf->data;
	return (uint32) p[0] | ((uint32) p[1] << 8) |
		((uint32) p[2] << 16) | ((uint32) p[3] << 24);
}

static void
mys_stmt_require(StringInfo inBuf, int pos, int need)
{
	if (pos < 0 || need < 0 || pos > inBuf->len || need > inBuf->len - pos)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("truncated MySQL COM_STMT_EXECUTE packet")));
}

static uint64
mys_stmt_read_le(const char *data, int bytes)
{
	uint64		result = 0;

	for (int i = 0; i < bytes; i++)
		result |= ((uint64) (unsigned char) data[i]) << (i * 8);
	return result;
}

static uint64
mys_stmt_read_lenenc(StringInfo inBuf, int *pos)
{
	unsigned char tag;
	int			bytes;

	mys_stmt_require(inBuf, *pos, 1);
	tag = (unsigned char) inBuf->data[(*pos)++];
	if (tag < 0xfb)
		return tag;
	if (tag == 0xfc)
		bytes = 2;
	else if (tag == 0xfd)
		bytes = 3;
	else if (tag == 0xfe)
		bytes = 8;
	else
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length-encoded MySQL parameter value")));
	mys_stmt_require(inBuf, *pos, bytes);
	{
		uint64 result = mys_stmt_read_le(inBuf->data + *pos, bytes);
		*pos += bytes;
		return result;
	}
}

static void
mys_stmt_clear_long_data(MysStmt *stmt)
{
	if (stmt->long_data == NULL)
		return;
	for (int i = 0; i < stmt->num_params; i++)
	{
		if (stmt->long_data[i].data != NULL)
			pfree(stmt->long_data[i].data);
		memset(&stmt->long_data[i], 0, sizeof(StringInfoData));
		stmt->has_long_data[i] = false;
	}
	stmt->long_data_invalid = false;
}

static char *
mys_stmt_decode_long_data(MysStmt *stmt, int paramno, Oid pgtype)
{
	StringInfoData *data = &stmt->long_data[paramno];

	Assert(stmt->has_long_data[paramno]);
	if (pgtype == BYTEAOID)
	{
		static const char hex[] = "0123456789abcdef";
		char	   *result = palloc((Size) data->len * 2 + 3);

		result[0] = '\\';
		result[1] = 'x';
		for (int i = 0; i < data->len; i++)
		{
			unsigned char byte = (unsigned char) data->data[i];

			result[2 + i * 2] = hex[byte >> 4];
			result[3 + i * 2] = hex[byte & 0x0f];
		}
		result[2 + data->len * 2] = '\0';
		return result;
	}
	if (memchr(data->data, '\0', data->len) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("NUL byte is not supported in textual MySQL long data")));
	return pnstrdup(data->data, data->len);
}

static char *
mys_stmt_decode_param(StringInfo inBuf, int *pos, uint8 mysql_type,
					  bool is_unsigned, Oid pgtype)
{
	uint64		value;
	int			bytes;

	switch (mysql_type)
	{
		case 1:                 /* MYSQL_TYPE_TINY */
			bytes = 1;
			break;
		case 2:                 /* MYSQL_TYPE_SHORT */
		case 13:                /* MYSQL_TYPE_YEAR */
			bytes = 2;
			break;
		case 3:                 /* MYSQL_TYPE_LONG */
		case 9:                 /* MYSQL_TYPE_INT24 */
			bytes = 4;
			break;
		case 8:                 /* MYSQL_TYPE_LONGLONG */
			bytes = 8;
			break;
		default:
			bytes = 0;
			break;
	}
	if (bytes != 0)
	{
		mys_stmt_require(inBuf, *pos, bytes);
		value = mys_stmt_read_le(inBuf->data + *pos, bytes);
		*pos += bytes;
		if (is_unsigned)
			return psprintf("%llu", (unsigned long long) value);
		switch (bytes)
		{
			case 1:
				return psprintf("%d", (int) (int8) value);
			case 2:
				return psprintf("%d", (int) (int16) value);
			case 4:
				return psprintf("%d", (int32) value);
			case 8:
				return psprintf(INT64_FORMAT, (int64) value);
		}
	}
	if (mysql_type == 4)      /* MYSQL_TYPE_FLOAT */
	{
		uint32		u;
		float4		f;

		mys_stmt_require(inBuf, *pos, 4);
		u = (uint32) mys_stmt_read_le(inBuf->data + *pos, 4);
		memcpy(&f, &u, sizeof(f));
		*pos += 4;
		return psprintf("%.9g", f);
	}
	if (mysql_type == 5)      /* MYSQL_TYPE_DOUBLE */
	{
		uint64		u;
		float8		f;

		mys_stmt_require(inBuf, *pos, 8);
		u = mys_stmt_read_le(inBuf->data + *pos, 8);
		memcpy(&f, &u, sizeof(f));
		*pos += 8;
		return psprintf("%.17g", f);
	}
	if (mysql_type == 10 || mysql_type == 7 || mysql_type == 12)
	{
		unsigned char len;
		uint64		year;
		unsigned char mon;
		unsigned char day;
		unsigned char hour = 0;
		unsigned char min = 0;
		unsigned char sec = 0;
		uint64		usec = 0;

		mys_stmt_require(inBuf, *pos, 1);
		len = (unsigned char) inBuf->data[(*pos)++];
		if (len == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
					 errmsg("zero MySQL date/datetime parameter is not supported")));
		if (len != 4 && len != 7 && len != 11)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid MySQL date/datetime parameter length")));
		if (mysql_type == 10 && len != 4)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid MySQL DATE parameter length")));
		mys_stmt_require(inBuf, *pos, len);
		year = mys_stmt_read_le(inBuf->data + *pos, 2);
		mon = (unsigned char) inBuf->data[*pos + 2];
		day = (unsigned char) inBuf->data[*pos + 3];
		if (len >= 7)
		{
			hour = (unsigned char) inBuf->data[*pos + 4];
			min = (unsigned char) inBuf->data[*pos + 5];
			sec = (unsigned char) inBuf->data[*pos + 6];
		}
		if (len == 11)
			usec = mys_stmt_read_le(inBuf->data + *pos + 7, 4);
		*pos += len;
		if (mysql_type == 10)
			return psprintf("%04llu-%02u-%02u", (unsigned long long) year,
							mon, day);
		if (usec != 0)
			return psprintf("%04llu-%02u-%02u %02u:%02u:%02u.%06llu",
							(unsigned long long) year, mon, day, hour, min, sec,
							(unsigned long long) usec);
		return psprintf("%04llu-%02u-%02u %02u:%02u:%02u",
						(unsigned long long) year, mon, day, hour, min, sec);
	}
	if (mysql_type == 11)     /* MYSQL_TYPE_TIME */
	{
		unsigned char len;
		bool		negative;
		uint64		days;
		unsigned char hour;
		unsigned char min;
		unsigned char sec;
		uint64		usec = 0;

		mys_stmt_require(inBuf, *pos, 1);
		len = (unsigned char) inBuf->data[(*pos)++];
		if (len == 0)
			return pstrdup("00:00:00");
		if (len != 8 && len != 12)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid MySQL TIME parameter length")));
		mys_stmt_require(inBuf, *pos, len);
		negative = inBuf->data[*pos] != 0;
		days = mys_stmt_read_le(inBuf->data + *pos + 1, 4);
		hour = (unsigned char) inBuf->data[*pos + 5];
		min = (unsigned char) inBuf->data[*pos + 6];
		sec = (unsigned char) inBuf->data[*pos + 7];
		if (len == 12)
			usec = mys_stmt_read_le(inBuf->data + *pos + 8, 4);
		*pos += len;
		if (usec != 0)
			return psprintf("%s%llu:%02u:%02u.%06llu", negative ? "-" : "",
							(unsigned long long) (days * 24 + hour), min, sec,
							(unsigned long long) usec);
		return psprintf("%s%llu:%02u:%02u", negative ? "-" : "",
						(unsigned long long) (days * 24 + hour), min, sec);
	}
	if (mysql_type == 0 || mysql_type == 15 || mysql_type == 245 ||
		mysql_type == 246 || mysql_type == 249 || mysql_type == 250 ||
		mysql_type == 251 || mysql_type == 252 || mysql_type == 253 ||
		mysql_type == 254 || mysql_type == 247 || mysql_type == 248 ||
		mysql_type == 255)
	{
		uint64		len = mys_stmt_read_lenenc(inBuf, pos);
		char	   *result;

		if (len > (uint64) (inBuf->len - *pos) || len > MaxAllocSize - 1 ||
			(pgtype == BYTEAOID && len > (MaxAllocSize - 3) / 2))
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid MySQL parameter string length")));
		if (pgtype == BYTEAOID)
		{
			static const char hex[] = "0123456789abcdef";

			result = palloc((Size) len * 2 + 3);
			result[0] = '\\';
			result[1] = 'x';
			for (uint64 i = 0; i < len; i++)
			{
				unsigned char byte = (unsigned char) inBuf->data[*pos + i];
				result[2 + i * 2] = hex[byte >> 4];
				result[3 + i * 2] = hex[byte & 0x0f];
			}
			result[2 + len * 2] = '\0';
		}
		else if (memchr(inBuf->data + *pos, '\0', (size_t) len) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("NUL byte is not supported in textual MySQL parameters")));
		else
			result = pnstrdup(inBuf->data + *pos, (int) len);
		*pos += (int) len;
		return result;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("Incorrect arguments to mysqld_stmt_execute")));
	return NULL;
}

static ParamListInfo
mys_stmt_bind_params(MysStmt *stmt, StringInfo inBuf, bool *open_cursor)
{
	ParamListInfo params;
	int			pos = 9;
	int			null_bytes;
	unsigned char flags;
	uint32		iterations;

	mys_stmt_require(inBuf, 4, 5);
	flags = (unsigned char) inBuf->data[4];
	iterations = (uint32) mys_stmt_read_le(inBuf->data + 5, 4);
	if (flags != 0 && flags != 1)
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("unsupported MySQL prepared statement cursor flags %u",
					(unsigned int) flags)));
	if (iterations != 1)
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("MySQL COM_STMT_EXECUTE iterations must be 1")));
	*open_cursor = flags == 1;
	if (stmt->long_data_invalid)
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid COM_STMT_SEND_LONG_DATA parameter")));
	if (stmt->num_params == 0)
	{
		if (inBuf->len != pos)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected data after a parameterless COM_STMT_EXECUTE")));
		return NULL;
	}

	null_bytes = (stmt->num_params + 7) / 8;
	mys_stmt_require(inBuf, pos, null_bytes + 1);
	pos += null_bytes;
	if ((unsigned char) inBuf->data[pos++] != 0)
	{
		mys_stmt_require(inBuf, pos, stmt->num_params * 2);
		if (stmt->mysql_types == NULL)
		{
			stmt->mysql_types = MemoryContextAlloc(stmt->context, stmt->num_params);
			stmt->unsigned_flags = MemoryContextAlloc(stmt->context, stmt->num_params);
		}
		for (int i = 0; i < stmt->num_params; i++)
		{
			stmt->mysql_types[i] = (uint8) inBuf->data[pos++];
			stmt->unsigned_flags[i] = (uint8) inBuf->data[pos++];
		}
		stmt->have_mysql_types = true;
	}
	else if (!stmt->have_mysql_types)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("COM_STMT_EXECUTE omitted parameter types before they were bound")));

	params = makeParamList(stmt->num_params);
	for (int i = 0; i < stmt->num_params; i++)
	{
		bool		isnull = (((unsigned char) inBuf->data[9 + i / 8] >>
																			(i % 8)) & 1) != 0;

		params->params[i].ptype = stmt->param_types[i];
		params->params[i].pflags = PARAM_FLAG_CONST;
		params->params[i].isnull = isnull;
		if (stmt->has_long_data != NULL && stmt->has_long_data[i])
		{
			Oid			inputfunc;
			Oid			typioparam;
			char	   *text;

			params->params[i].isnull = false;
			text = mys_stmt_decode_long_data(stmt, i, stmt->param_types[i]);
			getTypeInputInfo(stmt->param_types[i], &inputfunc, &typioparam);
			params->params[i].value = OidInputFunctionCall(inputfunc, text,
																						 typioparam, -1);
		}
		else if (!isnull)
		{
			Oid			inputfunc;
			Oid			typioparam;
			char	   *text;

			text = mys_stmt_decode_param(inBuf, &pos, stmt->mysql_types[i],
																	 (stmt->unsigned_flags[i] & 0x80) != 0,
																	 stmt->param_types[i]);
			getTypeInputInfo(stmt->param_types[i], &inputfunc, &typioparam);
			params->params[i].value = OidInputFunctionCall(inputfunc, text,
																				 typioparam, -1);
		}
	}
	if (pos != inBuf->len)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("trailing data in COM_STMT_EXECUTE packet")));
	return params;
}

static void
mys_stmt_send_result_header(MysPacketState *ps, TupleDesc desc,
								bool send_metadata_end)
{
	StringInfoData header;

	initStringInfo(&header);
	if (desc->natts < 251)
		appendStringInfoChar(&header, (char) desc->natts);
	else
	{
		appendStringInfoChar(&header, 0xfc);
		appendStringInfoChar(&header, (char) (desc->natts & 0xff));
		appendStringInfoChar(&header, (char) ((desc->natts >> 8) & 0xff));
	}
	if (mysql_negotiated_caps(ps) & MYSQL_CAP_OPTIONAL_RESULTSET_METADATA)
		appendStringInfoChar(&header, 1); /* RESULTSET_METADATA_FULL */
	mysql_packet_write(ps, header.data, header.len);
	pfree(header.data);
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		mys_stmt_send_column(ps, NameStr(attr->attname), attr->atttypid,
													 attr->atttypmod);
	}
	if (send_metadata_end)
		mys_stmt_send_metadata_end(ps);
}

/* End a cursor execute/fetch response with the MySQL server-status flags. */
static void
mys_stmt_send_cursor_end(MysPacketState *ps, uint16 status)
{
	if (mysql_negotiated_caps(ps) & MYSQL_CAP_DEPRECATE_EOF)
	{
		char		ok[7] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8), 0x00, 0x00};

		mysql_packet_write(ps, ok, sizeof(ok));
	}
	else
	{
		char		eof[5] = {0xfe, 0x00, 0x00,
			(char) (status & 0xff), (char) (status >> 8)};

		mysql_packet_write(ps, eof, sizeof(eof));
	}
}

static uint16
mys_stmt_server_status(uint16 extra_status)
{
	uint16		status = 0;

	if (MysAutocommitEnabled())
		status |= 0x0002; /* SERVER_STATUS_AUTOCOMMIT */

	if (IsTransactionBlock())
		status |= 0x0001; /* SERVER_STATUS_IN_TRANS */
	return status | extra_status;
}

static void
mys_stmt_append_binary_value(StringInfo buf, Oid typid, Datum value)
{
	uint64		u;

	switch (typid)
	{
		case BOOLOID:
			appendStringInfoChar(buf, DatumGetBool(value) ? 1 : 0);
			return;
		case INT2OID:
			u = (uint16) DatumGetInt16(value);
			break;
		case INT4OID:
			u = (uint32) DatumGetInt32(value);
			break;
		case INT8OID:
			u = (uint64) DatumGetInt64(value);
			break;
		case FLOAT4OID:
		{
			float4 f = DatumGetFloat4(value);
			uint32 bits;
			memcpy(&bits, &f, sizeof(bits));
			u = bits;
			break;
		}
		case FLOAT8OID:
		{
			float8 f = DatumGetFloat8(value);
			memcpy(&u, &f, sizeof(u));
			break;
		}
		case DATEOID:
		{
			DateADT date = DatumGetDateADT(value);
			int		year;
			int		month;
			int		day;

			if (DATE_NOT_FINITE(date))
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("infinite PostgreSQL date cannot be encoded for MySQL")));
			j2date(date + POSTGRES_EPOCH_JDATE, &year, &month, &day);
			appendStringInfoChar(buf, 4);
			appendStringInfoChar(buf, (char) (year & 0xff));
			appendStringInfoChar(buf, (char) ((year >> 8) & 0xff));
			appendStringInfoChar(buf, (char) month);
			appendStringInfoChar(buf, (char) day);
			return;
		}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			struct pg_tm tm;
			fsec_t		fsec;
			int			tz;
			Timestamp ts = typid == TIMESTAMPOID ? DatumGetTimestamp(value) :
				DatumGetTimestampTz(value);

			if (TIMESTAMP_NOT_FINITE(ts) ||
				timestamp2tm(ts, typid == TIMESTAMPTZOID ? &tz : NULL,
							 &tm, &fsec, NULL, NULL) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("infinite PostgreSQL timestamp cannot be encoded for MySQL")));
			appendStringInfoChar(buf, fsec == 0 ? 7 : 11);
			appendStringInfoChar(buf, (char) (tm.tm_year & 0xff));
			appendStringInfoChar(buf, (char) ((tm.tm_year >> 8) & 0xff));
			appendStringInfoChar(buf, (char) tm.tm_mon);
			appendStringInfoChar(buf, (char) tm.tm_mday);
			appendStringInfoChar(buf, (char) tm.tm_hour);
			appendStringInfoChar(buf, (char) tm.tm_min);
			appendStringInfoChar(buf, (char) tm.tm_sec);
			if (fsec != 0)
			{
				u = (uint64) fsec;
				for (int i = 0; i < 4; i++)
					appendStringInfoChar(buf, (char) ((u >> (i * 8)) & 0xff));
			}
			return;
		}
		case TIMEOID:
		{
			TimeADT time = DatumGetTimeADT(value);
			bool	negative = time < 0;
			uint64		usecs = negative ? (uint64) -time : (uint64) time;
			uint64		days = usecs / USECS_PER_DAY;
			uint64		rest = usecs % USECS_PER_DAY;
			uint64		hour = rest / USECS_PER_HOUR;
			uint64		min = (rest % USECS_PER_HOUR) / USECS_PER_MINUTE;
			uint64		sec = (rest % USECS_PER_MINUTE) / USECS_PER_SEC;
			uint64		fsec = rest % USECS_PER_SEC;

			appendStringInfoChar(buf, fsec == 0 ? 8 : 12);
			appendStringInfoChar(buf, negative ? 1 : 0);
			for (int i = 0; i < 4; i++)
				appendStringInfoChar(buf, (char) ((days >> (i * 8)) & 0xff));
			appendStringInfoChar(buf, (char) hour);
			appendStringInfoChar(buf, (char) min);
			appendStringInfoChar(buf, (char) sec);
			if (fsec != 0)
				for (int i = 0; i < 4; i++)
					appendStringInfoChar(buf, (char) ((fsec >> (i * 8)) & 0xff));
			return;
		}
		case BYTEAOID:
		{
			bytea	   *bytes = DatumGetByteaP(value);

			mys_stmt_append_lenenc(buf, VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));
			return;
		}
		default:
		{
			Oid			outputfunc;
			bool		isvarlena;
			char	   *text;

			getTypeOutputInfo(typid, &outputfunc, &isvarlena);
			text = OidOutputFunctionCall(outputfunc, value);
			mys_stmt_append_lenenc(buf, text, strlen(text));
			pfree(text);
			return;
		}
	}
	for (int i = 0; i < (typid == INT2OID ? 2 :
									  typid == INT4OID || typid == FLOAT4OID ? 4 : 8); i++)
		appendStringInfoChar(buf, (char) ((u >> (i * 8)) & 0xff));
}

static bool
mys_stmt_receive_slot(TupleTableSlot *slot, DestReceiver *self)
{
	MysStmtDestReceiver *receiver = (MysStmtDestReceiver *) self;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	StringInfoData row;
	int			null_bytes = (desc->natts + 9) / 8;

	initStringInfo(&row);
	appendStringInfoChar(&row, 0x00);
	for (int i = 0; i < null_bytes; i++)
		appendStringInfoChar(&row, 0x00);
	for (int i = 0; i < desc->natts; i++)
	{
		bool		isnull;
		Datum		value = slot_getattr(slot, i + 1, &isnull);

		if (isnull)
			row.data[1 + (i + 2) / 8] |= 1 << ((i + 2) % 8);
		else
			mys_stmt_append_binary_value(&row, TupleDescAttr(desc, i)->atttypid,
															 value);
	}
	mysql_packet_write(receiver->ps, row.data, row.len);
	pfree(row.data);
	return true;
}

static void
mys_stmt_rstartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	MysStmtDestReceiver *receiver = (MysStmtDestReceiver *) self;

	(void) operation;
	if (receiver->send_header)
	{
		mys_stmt_send_result_header(receiver->ps, typeinfo, true);
		mysql_packet_set_result_started(receiver->ps, true);
	}
}

static void
mys_stmt_rshutdown(DestReceiver *self)
{
	/* mysql_end_command() supplies the only final EOF/OK packet. */
	(void) self;
}

static void
mys_stmt_rdestroy(DestReceiver *self)
{
	pfree(self);
}

static DestReceiver *
mys_stmt_create_receiver(MysPacketState *ps, bool send_header)
{
	MysStmtDestReceiver *receiver = palloc0(sizeof(MysStmtDestReceiver));

	receiver->pub.receiveSlot = mys_stmt_receive_slot;
	receiver->pub.rStartup = mys_stmt_rstartup;
	receiver->pub.rShutdown = mys_stmt_rshutdown;
	receiver->pub.rDestroy = mys_stmt_rdestroy;
	receiver->pub.mydest = DestRemoteExecute;
	receiver->ps = ps;
	receiver->send_header = send_header;
	return (DestReceiver *) receiver;
}

void
mysql_stmt_prepare(MysPacketState *ps, StringInfo inBuf)
{
	MysStmtSession *session;
	MysStmt    *stmt;
	MysStmtEntry *entry;
	List	   *raw_list;
	RawStmt    *rawstmt;
	Query	   *query;
	List	   *query_list;
	Oid	   *param_types = NULL;
	int			num_params = 0;
	bool		snapshot_set = false;
	uint32		id;
	char		ok[12];
	int			pos = 0;
	MemoryContext oldcontext;

	if (inBuf->len == 0 || memchr(inBuf->data, '\0', inBuf->len) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid MySQL prepared statement text")));

	ProtocolStartCommand();
	raw_list = pg_parse_query_with_routine(inBuf->data, parserengine);
	if (list_length(raw_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot prepare multiple statements")));
	rawstmt = linitial_node(RawStmt, raw_list);
	mys_stmt_resolve_ambiguous_numeric_params(rawstmt);

	session = mys_stmt_get_session();
	oldcontext = MemoryContextSwitchTo(session->context);
	stmt = palloc0(sizeof(MysStmt));
	stmt->context = AllocSetContextCreate(session->context,
															  "MySQL prepared statement",
															  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(oldcontext);

	stmt->plansource = CreateCachedPlan(rawstmt, inBuf->data,
																 CreateCommandTag(rawstmt->stmt));
	if (analyze_requires_snapshot(rawstmt))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}
	/*
	 * Do not use pg_analyze_and_rewrite_varparams_with_routine() here: its
	 * PostgreSQL-facing contract rejects unresolved parameters.  MySQL's wire
	 * prepare permits them, so default only those remaining UNKNOWN after
	 * dialect-aware variable analysis and then rewrite the adjusted tree.
	 */
	query = parse_analyze_varparams_with_routine(rawstmt, inBuf->data,
																 &param_types, &num_params, NULL,
																 parserengine);
	query = mys_stmt_default_unknown_params(query, param_types, num_params);
	query_list = QueryRewrite(query);
	if (snapshot_set)
		PopActiveSnapshot();

	stmt->num_params = num_params;
	if (num_params > 0)
	{
		oldcontext = MemoryContextSwitchTo(stmt->context);
		stmt->param_types = palloc_array(Oid, num_params);
		memcpy(stmt->param_types, param_types, sizeof(Oid) * num_params);
		MemoryContextSwitchTo(oldcontext);
	}
	CompleteCachedPlan(stmt->plansource, query_list, NULL, param_types,
														 num_params, mys_stmt_parser_setup, stmt,
														 CURSOR_OPT_PARALLEL_OK, true);
	SaveCachedPlan(stmt->plansource);

	id = session->next_id++;
	if (id == 0)
		id = session->next_id++;
	stmt->id = id;
	entry = hash_search(session->statements, &id, HASH_ENTER, NULL);
	entry->stmt = stmt;

	ok[pos++] = 0x00;
	ok[pos++] = (char) (id & 0xff);
	ok[pos++] = (char) ((id >> 8) & 0xff);
	ok[pos++] = (char) ((id >> 16) & 0xff);
	ok[pos++] = (char) ((id >> 24) & 0xff);
	ok[pos++] = stmt->plansource->resultDesc ?
		(char) (stmt->plansource->resultDesc->natts & 0xff) : 0;
	ok[pos++] = stmt->plansource->resultDesc ?
		(char) ((stmt->plansource->resultDesc->natts >> 8) & 0xff) : 0;
	ok[pos++] = (char) (num_params & 0xff);
	ok[pos++] = (char) ((num_params >> 8) & 0xff);
	ok[pos++] = 0x00;
	ok[pos++] = 0x00;
	ok[pos++] = 0x00;

	mysql_packet_write(ps, ok, pos);
	for (int i = 0; i < num_params; i++)
		mys_stmt_send_column(ps, "?", stmt->param_types[i], -1);
	mys_stmt_send_metadata_end(ps);
	if (stmt->plansource->resultDesc != NULL)
	{
		TupleDesc desc = stmt->plansource->resultDesc;

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(desc, i);

			mys_stmt_send_column(ps, NameStr(attr->attname), attr->atttypid,
													 attr->atttypmod);
		}
		mys_stmt_send_metadata_end(ps);
	}
	/* Metadata lookup for private domains requires the command owner. */
	ProtocolFinishCommand();
	mys_stmt_finish_command(ps);
}

void
mysql_stmt_execute(MysPacketState *ps, StringInfo inBuf)
{
	uint32		id;
	MysStmt    *stmt;
	Portal		portal;
	CachedPlan *cplan;
	ParamListInfo params;
	DestReceiver *receiver;
	QueryCompletion qc;
	MemoryContext oldcontext;
	bool		snapshot_set = false;
	bool		open_cursor;
	char	   *query_string;

	if (inBuf->len < 9)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid MySQL COM_STMT_EXECUTE packet")));
	id = (uint32) mys_stmt_read_le(inBuf->data, 4);
	stmt = mys_stmt_lookup(id, false);

	ProtocolStartCommand();
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));

	/* Match COM_QUERY: the first transactional prepared DML starts the
	 * transaction that autocommit=0 keeps open across protocol commands. */
	if (!MysAutocommitEnabled() && !IsTransactionBlock() &&
		(stmt->plansource->commandTag == CMDTAG_INSERT ||
		 stmt->plansource->commandTag == CMDTAG_UPDATE ||
		 stmt->plansource->commandTag == CMDTAG_DELETE ||
		 stmt->plansource->commandTag == CMDTAG_MERGE))
		BeginTransactionBlock();

	/* A re-execute implicitly closes the previous cursor for this statement. */
	mys_stmt_close_cursor(stmt);
	if (inBuf->data[4] == 1)
		portal = CreatePortal(mys_stmt_cursor_name(stmt), false, false);
	else
		portal = CreatePortal("", true, true);
	portal->visible = false;
	if (inBuf->data[4] == 1)
		portal->cursorOptions |= CURSOR_OPT_HOLD | CURSOR_OPT_NO_SCROLL;
	oldcontext = MemoryContextSwitchTo(portal->portalContext);
	params = mys_stmt_bind_params(stmt, inBuf, &open_cursor);
	MemoryContextSwitchTo(oldcontext);

	if (stmt->num_params > 0 ||
		(stmt->plansource->raw_parse_tree != NULL &&
		 analyze_requires_snapshot(stmt->plansource->raw_parse_tree)))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot_set = true;
	}
	cplan = GetCachedPlan(stmt->plansource, params, NULL, NULL);
	query_string = MemoryContextStrdup(portal->portalContext,
																 stmt->plansource->query_string);
	/* Do not insert throwing code between GetCachedPlan and PortalDefineQuery. */
	PortalDefineQuery(portal, NULL, query_string, stmt->plansource->commandTag,
															cplan->stmt_list, cplan);
	if (snapshot_set)
		PopActiveSnapshot();
	PortalStart(portal, params, 0, InvalidSnapshot);

	if (open_cursor && portal->tupDesc != NULL)
	{
		/*
		 * MySQL cursors are materialized at EXECUTE time and remain readable
		 * after COMMIT or ROLLBACK.  This exact core operation releases the
		 * current-xact ownership after filling the hold store.
		 */
		PortalHoldForProtocol(portal);
		mys_stmt_send_result_header(ps, portal->tupDesc, false);
		mys_stmt_clear_long_data(stmt);
		ProtocolFinishCommand();
		mys_stmt_send_cursor_end(ps, mys_stmt_server_status(0x0040));
		mys_stmt_finish_command(ps);
		return;
	}

	receiver = mys_stmt_create_receiver(ps, true);
	(void) PortalRun(portal, FETCH_ALL, true, receiver, receiver, &qc);
	receiver->rDestroy(receiver);
	PortalDrop(portal, false);
	mys_stmt_clear_long_data(stmt);
	/*
	 * A direct COM_STMT_EXECUTE bypasses exec_simple_query(), which normally
	 * advances the command counter before the next statement in a transaction.
	 * Without this, an autocommit=0 connection cannot see its own prepared DML
	 * through its following COM_QUERY.
	 */
	CommandCounterIncrement();

	/* Match exec_simple_query(): surface commit errors before result completion. */
	ProtocolFinishCommand();
	EndCommand(&qc, DestRemoteExecute, false);
	pq_flush();
}

void
mysql_stmt_close(MysPacketState *ps, StringInfo inBuf)
{
	MysStmtSession *session = mys_stmt_get_session();
	uint32		id = mys_stmt_read_id(inBuf);
	MysStmtEntry *entry;

	entry = hash_search(session->statements, &id, HASH_FIND, NULL);
	if (entry != NULL)
	{
		ProtocolStartCommand();
		mys_stmt_close_cursor(entry->stmt);
		DropCachedPlan(entry->stmt->plansource);
		MemoryContextDelete(entry->stmt->context);
		hash_search(session->statements, &id, HASH_REMOVE, NULL);
		ProtocolFinishCommand();
	}
	/* COM_STMT_CLOSE has no response packet. */
	mys_stmt_finish_command(ps);
}

void
mysql_stmt_reset(MysPacketState *ps, StringInfo inBuf)
{
	uint32		id = mys_stmt_read_id(inBuf);
	uint16		status;
	char		ok[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	MysStmt    *stmt = mys_stmt_lookup(id, false);

	ProtocolStartCommand();
	mys_stmt_close_cursor(stmt);
	mys_stmt_clear_long_data(stmt);
	ProtocolFinishCommand();
	status = mys_stmt_server_status(0);
	ok[3] = (char) (status & 0xff);
	ok[4] = (char) (status >> 8);
	mysql_packet_write(ps, ok, sizeof(ok));
	mys_stmt_finish_command(ps);
}

/*
 * COM_STMT_SEND_LONG_DATA intentionally has no response.  Its data is
 * appended per parameter and consumed by the next execute packet, where a
 * long-data parameter has no inline binary value.
 */
void
mysql_stmt_send_long_data(MysPacketState *ps, StringInfo inBuf)
{
	uint32		id;
	uint16		paramno;
	MysStmt    *stmt;
	MemoryContext oldcontext;
	int			data_len;

	if (inBuf->len < 6)
	{
		/* The official protocol is silent; retain the error for EXECUTE. */
		mys_stmt_finish_command(ps);
		return;
	}
	id = (uint32) mys_stmt_read_le(inBuf->data, 4);
	paramno = (uint16) mys_stmt_read_le(inBuf->data + 4, 2);
	stmt = mys_stmt_lookup(id, true);
	if (stmt == NULL)
	{
		mys_stmt_finish_command(ps);
		return;
	}
	if (paramno >= stmt->num_params)
	{
		stmt->long_data_invalid = true;
		mys_stmt_finish_command(ps);
		return;
	}
	data_len = inBuf->len - 6;
	if (stmt->long_data == NULL)
	{
		oldcontext = MemoryContextSwitchTo(stmt->context);
		stmt->long_data = palloc0_array(StringInfoData, stmt->num_params);
		stmt->has_long_data = palloc0_array(bool, stmt->num_params);
		MemoryContextSwitchTo(oldcontext);
	}
	if (stmt->long_data[paramno].data == NULL)
	{
		oldcontext = MemoryContextSwitchTo(stmt->context);
		initStringInfo(&stmt->long_data[paramno]);
		MemoryContextSwitchTo(oldcontext);
	}
	oldcontext = MemoryContextSwitchTo(stmt->context);
	appendBinaryStringInfo(&stmt->long_data[paramno], inBuf->data + 6,
													 data_len);
	MemoryContextSwitchTo(oldcontext);
	stmt->has_long_data[paramno] = true;
	mys_stmt_finish_command(ps);
}

void
mysql_stmt_fetch(MysPacketState *ps, StringInfo inBuf)
{
	uint32		id;
	uint32		num_rows;
	MysStmt    *stmt;
	Portal		portal;
	DestReceiver *receiver;
	uint64		nprocessed;
	uint16		status;

	if (inBuf->len != 8)
		ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("invalid MySQL COM_STMT_FETCH packet")));
	id = (uint32) mys_stmt_read_le(inBuf->data, 4);
	num_rows = (uint32) mys_stmt_read_le(inBuf->data + 4, 4);
	stmt = mys_stmt_lookup(id, false);
	portal = mys_stmt_get_cursor_portal(stmt);
	if (portal == NULL)
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_CURSOR_STATE),
			 errmsg("MySQL prepared statement %u has no open cursor", id)));

	ProtocolStartCommand();
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
			(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
			 errmsg("current transaction is aborted, commands ignored until end of transaction block")));

	if (num_rows != 0)
	{
		receiver = mys_stmt_create_receiver(ps, false);
		nprocessed = PortalRunFetch(portal, FETCH_FORWARD, (long) num_rows,
									  receiver);
		receiver->rDestroy(receiver);
		(void) nprocessed;
	}
	status = portal->atEnd ? 0x0080 : 0x0040;
	if (portal->atEnd)
		mys_stmt_close_cursor(stmt);
	ProtocolFinishCommand();
	mys_stmt_send_cursor_end(ps, mys_stmt_server_status(status));
	mys_stmt_finish_command(ps);
}
