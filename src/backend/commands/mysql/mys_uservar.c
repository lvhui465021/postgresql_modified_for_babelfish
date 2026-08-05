/*-------------------------------------------------------------------------
 *
 * mys_uservar.c
 *	  MySQL user variable session state
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/mysql/mys_uservar.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "adapter/mysql/systemVar.h"
#include "utils/builtins.h"
#include "commands/mysql/mys_uservar.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#define VAR_VALUE_EXTEND_BYTES 1

typedef struct MysqlUserVariableItem
{
    /* dynahash.c requires key to be first field */
    char varName[NAMEDATALEN];
    bytea *varValue;
	Oid varValueType;
    bool isnull;
} MysqlUserVariableItem;

static HTAB *mysql_user_variables = NULL;

static void initMysqlUserVariableHashTable(void);
static Datum byteastring2bytea(char *byteastring, bool isDigit);
static Datum bitstring2bytea(char *bitstring, bool isDigit);
static Datum boolstring2bytea(char *boolstring, bool isDigit);
static Datum cstring2bytea(char *cstring, bool isDigit);
static bytea *copyUserVarValue(bytea *varValue);

PG_FUNCTION_INFO_V1(mys_get_user_var);
PG_FUNCTION_INFO_V1(mys_set_user_var);
PG_FUNCTION_INFO_V1(mys_get_session_time_zone);
PG_FUNCTION_INFO_V1(mys_get_global_time_zone);
PG_FUNCTION_INFO_V1(mys_get_system_variable);


void 
clearUserVars(void)
{
    hash_destroy(mysql_user_variables);
    mysql_user_variables = NULL;
}

Datum
mys_get_user_var(PG_FUNCTION_ARGS)
{
	char	   *user_var_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bytea	   *value = mysGetUserVarValueInternal(user_var_name);
	int			value_len;

	if (value == NULL)
		PG_RETURN_NULL();

	value_len = VARSIZE_ANY_EXHDR(value);
	PG_RETURN_TEXT_P(cstring_to_text_with_len(VARDATA_ANY(value), value_len));
}

/*
 * This is intentionally a volatile SQL function.  A MySQL SQL PREPARE can
 * contain @@session.time_zone, and its value must be read when EXECUTE runs,
 * rather than being folded into the prepared query's parse tree.
 */
Datum
mys_get_session_time_zone(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(MysGetSessionTimeZone()));
}

Datum
mys_get_global_time_zone(PG_FUNCTION_ARGS)
{
	char	   *value = MysGetGlobalTimeZone();
	text	   *result = cstring_to_text(value);

	pfree(value);
	PG_RETURN_TEXT_P(result);
}

Datum
mys_get_system_variable(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		is_session = PG_GETARG_BOOL(1);
	char	   *value = NULL;
	char	   *static_value = NULL;

	/*
	 * Compatibility probes with fixed literal results resolve without the
	 * catalog/session machinery (which requires the aux_mysql extension's
	 * mys_informa_schema).  This mirrors the literal folding the dialect
	 * parser used to do for these names before they were degraded to this
	 * function call.
	 */
	if (pg_strcasecmp(name, "autocommit") == 0 ||
		pg_strcasecmp(name, "session.autocommit") == 0 ||
		pg_strcasecmp(name, "local.autocommit") == 0)
		static_value = MysAutocommitEnabled() ? "1" : "0";
	else if (pg_strcasecmp(name, "character_set_client") == 0 ||
			 pg_strcasecmp(name, "character_set_connection") == 0 ||
			 pg_strcasecmp(name, "character_set_results") == 0 ||
			 pg_strcasecmp(name, "character_set_server") == 0)
		static_value = "utf8mb4";
	else if (pg_strcasecmp(name, "collation_connection") == 0 ||
			 pg_strcasecmp(name, "session.collation_connection") == 0 ||
			 pg_strcasecmp(name, "collation_server") == 0 ||
			 pg_strcasecmp(name, "collation_database") == 0)
		static_value = "utf8mb4_general_ci";
	else if (pg_strcasecmp(name, "max_allowed_packet") == 0)
		static_value = "16777216";
	else if (pg_strcasecmp(name, "version_comment") == 0 ||
			 pg_strcasecmp(name, "version") == 0)
		static_value = mysql_server_version;

	if (static_value != NULL)
	{
		pfree(name);
		PG_RETURN_TEXT_P(cstring_to_text(static_value));
	}

	/*
	 * Session/global time zone resolve through session state rather than
	 * the extension catalog, so they work in a bare kernel (e.g. the wire
	 * protocol tests before aux_mysql is created).
	 */
	if (pg_strcasecmp(name, "time_zone") == 0 ||
		pg_strcasecmp(name, "session.time_zone") == 0 ||
		pg_strcasecmp(name, "local.time_zone") == 0)
	{
		pfree(name);
		PG_RETURN_TEXT_P(cstring_to_text(MysGetSessionTimeZone()));
	}
	if (pg_strcasecmp(name, "global.time_zone") == 0)
	{
		char	   *tz = MysGetGlobalTimeZone();

		pfree(name);
		PG_RETURN_TEXT_P(cstring_to_text(tz));
	}

	/*
	 * Strip the session./local./global. prefix before resolving the value;
	 * getSystemVariableValueForSelect expects the bare variable name.
	 */
	{
		char	   *orig_name = name;

		if (pg_strncasecmp(name, "session.", 8) == 0)
			name += 8;
		else if (pg_strncasecmp(name, "local.", 6) == 0)
			name += 6;
		else if (pg_strncasecmp(name, "global.", 7) == 0)
		{
			name += 7;
			is_session = false;
		}

		getSystemVariableValueForSelect(name, is_session, &value);
		pfree(orig_name);
	}
	if (value == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(value));
}

Datum
mys_set_user_var(PG_FUNCTION_ARGS)
{
	char	   *user_var_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			value_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	Datum		value;

	if (!OidIsValid(value_type))
		elog(ERROR, "could not determine MySQL user variable value type");

	if (PG_ARGISNULL(1))
	{
		mysSetUserVarForPl(user_var_name, (Datum) 0, value_type, true);
		PG_RETURN_NULL();
	}

	value = PG_GETARG_DATUM(1);
	mysSetUserVarForPl(user_var_name, value, value_type, false);
	PG_RETURN_DATUM(value);
}


bool
varValueIsDigit(Datum varValue, Oid varValueType)
{
    bool isDigit = false;

    if (varValueType == BYTEAOID)
    {
        /*
         * 确保传入的varValue，内存使用palloc分配
         */
        bytea *vlena = DatumGetByteaPP(varValue);
        size_t userVarValueExtendByteOffset = VARSIZE_ANY(vlena);

        isDigit = (((char *)vlena)[userVarValueExtendByteOffset] == '1') ? true : false;
    }
    else if (varValueType == BOOLOID)
    {
        isDigit = true;
    }
    else if (varValueType == BITOID || varValueType == VARBITOID)
    {
        isDigit = true;
    }
    else
    {
        char ncategory;
		bool nispreferred;

        get_type_category_preferred(varValueType, &ncategory, &nispreferred);

        isDigit = (ncategory == TYPCATEGORY_NUMERIC) ? true : false;
    }

    return isDigit;
}


void
mysSetUserVarInternal(char *userVarName, char *userVarValue, Oid varValueType, bool isDigit, bool isNull)
{
    MysqlUserVariableItem *entry;
    bool found;
    MemoryContext oldContext;

    /* Initialize the hash table, if necessary */
	if (!mysql_user_variables)
		initMysqlUserVariableHashTable();

    entry = (MysqlUserVariableItem *) hash_search(mysql_user_variables,
                                                  userVarName,
                                                  HASH_ENTER,
                                                  &found);

    
    if (found)
    {
        if (!entry->isnull)
        {
            pfree(entry->varValue);
            entry->varValue = NULL;
        }
    }
    else
    {
        /* Nothing to do */
    }

    oldContext = MemoryContextSwitchTo(TopMemoryContext);
	entry->varValueType = varValueType;

    if (!isNull)
    {
        Oid baseVarValueType = getBaseType(varValueType);
        bytea *result = NULL;

        if (baseVarValueType == BYTEAOID)
        {
            result = DatumGetByteaPP(byteastring2bytea(userVarValue, isDigit));
        }
        else if (baseVarValueType == BITOID || baseVarValueType == VARBITOID)
        {
            result = DatumGetByteaPP(bitstring2bytea(userVarValue, isDigit));
        }
        else if (baseVarValueType == BOOLOID)
        {
            result = DatumGetByteaPP(boolstring2bytea(userVarValue, isDigit));
        }
        else
        {
            result = DatumGetByteaPP(cstring2bytea(userVarValue, isDigit));
        }

        entry->varValue = result;
        entry->isnull = false;
    }
    else
    {
        entry->isnull = true;
    }

    MemoryContextSwitchTo(oldContext);
}


void
mysSetUserVarForPl(char *userVarName, Datum userVarValue, Oid varValueType, bool isNull)
{
    MysqlUserVariableItem *entry;
    bool found;
    MemoryContext oldContext;

    /* Initialize the hash table, if necessary */
	if (!mysql_user_variables)
		initMysqlUserVariableHashTable();

    entry = (MysqlUserVariableItem *) hash_search(mysql_user_variables,
                                                  userVarName,
                                                  HASH_ENTER,
                                                  &found);

    
    if (found)
    {
        if (!entry->isnull)
        {
            pfree(entry->varValue);
            entry->varValue = NULL;
        }
    }
    else
    {
        /* Nothing to do */
    }

    oldContext = MemoryContextSwitchTo(TopMemoryContext);
	entry->varValueType = varValueType;

    if (!isNull)
    {
        Oid baseVarValueType = getBaseType(varValueType);
        Oid typOutput = InvalidOid;
        bool typIsVarlena;
        bool isDigit;
        char *varValue;
        bytea *result = NULL;

        getTypeOutputInfo(varValueType, &typOutput, &typIsVarlena);
        varValue = OidOutputFunctionCall(typOutput, userVarValue);
        isDigit = varValueIsDigit(userVarValue, varValueType);

        if (baseVarValueType == BYTEAOID)
        {
            result = DatumGetByteaPP(byteastring2bytea(varValue, isDigit));
        }
        else if (baseVarValueType == BITOID || baseVarValueType == VARBITOID)
        {
            result = DatumGetByteaPP(bitstring2bytea(varValue, isDigit));
        }
        else if (baseVarValueType == BOOLOID)
        {
            result = DatumGetByteaPP(boolstring2bytea(varValue, isDigit));
        }
        else
        {
            result = DatumGetByteaPP(cstring2bytea(varValue, isDigit));
        }

        entry->varValue = result;
        entry->isnull = false;
    }
    else
    {
        entry->isnull = true;
    }

    MemoryContextSwitchTo(oldContext);
}


bytea *
mysGetUserVarValueInternal(char *userVarName)
{
    MysqlUserVariableItem *entry;
    bool found;
    bytea *result;

    /* Initialize the hash table, if necessary */
	if (!mysql_user_variables)
		initMysqlUserVariableHashTable();
    
    entry = (MysqlUserVariableItem *) hash_search(mysql_user_variables,
                                                  userVarName,
                                                  HASH_FIND,
                                                  &found);
    
    result = NULL;

    if (found)
    {
        if (entry->isnull == false)
        {
            result = copyUserVarValue(entry->varValue);
        }
    }

    return result;
}

Oid
mysGetUserVarTypeInternal(char *userVarName)
{
	MysqlUserVariableItem *entry;
	bool		found;

	if (!mysql_user_variables)
		return InvalidOid;

	entry = (MysqlUserVariableItem *) hash_search(mysql_user_variables,
													  userVarName,
													  HASH_FIND,
													  &found);
	return found ? entry->varValueType : InvalidOid;
}


static void
initMysqlUserVariableHashTable(void)
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(MysqlUserVariableItem);
    
	mysql_user_variables = hash_create("MySQL User Variables",
                                       1024,
                                       &hash_ctl,
                                       HASH_ELEM | HASH_STRINGS);
}


static Datum
byteastring2bytea(char *byteastring, bool isDigit)
{
    bytea *result;

    if ((byteastring[0] == '\\') && (byteastring[1] == 'x'))
    {
        /* 
         * bytea_output == BYTEA_OUTPUT_HEX
         * "\x3135"
         */
        size_t len = strlen(byteastring);
        int bc;

		bc = (len - 2) / 2 + VARHDRSZ + VAR_VALUE_EXTEND_BYTES;	/* maximum possible length */
		result = palloc(bc);
		bc = hex_decode(byteastring + 2, len - 2, VARDATA(result));
		SET_VARSIZE(result, bc + VARHDRSZ); /* actual length */
        
        if (isDigit)
        {
            ((char *)result)[bc + VARHDRSZ] = '1';
        }
        else
        {
            ((char *)result)[bc + VARHDRSZ] = '\0';
        }
    }
    else
    {
        elog(ERROR, "Invalid string format for bytea_output(BYTEA_OUTPUT_ESCAPE)");
    }

    PG_RETURN_BYTEA_P(result);
}


static Datum
bitstring2bytea(char *bitstring, bool isDigit)
{
    int bitLen = strlen(bitstring);
    int byteLen = (bitLen + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    int needPrePadBitNum = byteLen * BITS_PER_BYTE - bitLen;
    char *rp;
    int i;
    int j;
    bytea *result;

    result = (bytea *) palloc0(byteLen + VARHDRSZ + VAR_VALUE_EXTEND_BYTES);
    SET_VARSIZE(result, byteLen + VARHDRSZ);

    rp = VARDATA(result);

    for (i = 0; i < (BITS_PER_BYTE - needPrePadBitNum); i++)
    {
        rp[0] = rp[0] * 2 + (bitstring[i] - '0');
    }

    for (j = 1; j < byteLen; j++)
    {
        int end = i + BITS_PER_BYTE;
        for (; i < end; i++)
        {
            rp[j] = rp[j] * 2 + (bitstring[i] - '0');
        }
    }

    if (isDigit)
    {
        ((char *)result)[byteLen + VARHDRSZ] = '1';
    }
    else
    {
        ((char *)result)[byteLen + VARHDRSZ] = '\0';
    }

    PG_RETURN_BYTEA_P(result);
}


static Datum
boolstring2bytea(char *boolstring, bool isDigit)
{
    char *p;
    bytea *result;

    result = (bytea *) palloc(1 + VARHDRSZ + VAR_VALUE_EXTEND_BYTES);
    SET_VARSIZE(result, 1 + VARHDRSZ);

    p = VARDATA(result);
    *p = (boolstring[0] == '1') ? '1' : '0';

    if (isDigit)
    {
        ((char *)result)[1 + VARHDRSZ] = '1';
    }
    else
    {
        ((char *)result)[1 + VARHDRSZ] = '\0';
    }

    PG_RETURN_BYTEA_P(result);
}


static Datum
cstring2bytea(char *cstring, bool isDigit)
{
    int cstringLen = strlen(cstring);
    char *tp;
    char *rp;
    bytea *result;

    result = (bytea *) palloc(cstringLen + VARHDRSZ + VAR_VALUE_EXTEND_BYTES);
    SET_VARSIZE(result, cstringLen + VARHDRSZ);

    tp = cstring;
    rp = VARDATA(result);

    while (*tp != '\0')
    {
        *rp++ = *tp++;
    }

    if (isDigit)
    {
        ((char *)result)[cstringLen + VARHDRSZ] = '1';
    }
    else
    {
        ((char *)result)[cstringLen + VARHDRSZ] = '\0';
    }

    PG_RETURN_BYTEA_P(result);
}


static bytea *
copyUserVarValue(bytea *varValue)
{
    bytea *vlena = varValue;
    char *byte = VARDATA_ANY(vlena);
    size_t byteLen = VARSIZE_ANY_EXHDR(vlena);
    bool isDigit = varValueIsDigit(PointerGetDatum(varValue), BYTEAOID);
    char *p;
    bytea *result;

    result = palloc(byteLen + VARHDRSZ + VAR_VALUE_EXTEND_BYTES);
    SET_VARSIZE(result, byteLen + VARHDRSZ);

    p = VARDATA(result);
    memcpy(p, byte, byteLen);

    if (isDigit)
    {
        ((char *)result)[byteLen + VARHDRSZ] = '1';
    }
    else
    {
        ((char *)result)[byteLen + VARHDRSZ] = '\0';
    }

    return result;
}

/*
 * mys_extract_user_var_name
 *
 * Extract the variable name from a degraded MySQL user-variable reference.
 * The grammar now emits pg_catalog.mys_get_user_var('name') FuncCall nodes
 * instead of a dedicated UserVarRef node; callers that previously read
 * UserVarRef.userVarName use this to recover the name.
 *
 * Returns a pstrdup'd name, or NULL if expr is not such a call.
 */
char *
mys_extract_user_var_name(Node *expr)
{
	FuncCall   *fn;
	A_Const    *arg;
	char	   *fname;

	if (expr == NULL || !IsA(expr, FuncCall))
		return NULL;
	fn = (FuncCall *) expr;

	/* Two-part name pg_catalog.mys_get_user_var. */
	if (list_length(fn->funcname) != 2)
		return NULL;
	if (pg_strcasecmp(strVal(linitial(fn->funcname)), "pg_catalog") != 0)
		return NULL;
	fname = strVal(lsecond(fn->funcname));
	if (pg_strcasecmp(fname, "mys_get_user_var") != 0)
		return NULL;

	/* Single text argument holding the variable name. */
	if (list_length(fn->args) != 1)
		return NULL;
	arg = linitial_node(A_Const, fn->args);
	if (arg->isnull || arg->val.node.type != T_String)
		return NULL;

	return pstrdup(arg->val.sval.sval);
}
