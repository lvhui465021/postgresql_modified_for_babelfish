-- \echo aux_mysql extension loading 1.1

-- -----------------------------------------------------------------------------
-- Schemas
-- -----------------------------------------------------------------------------

GRANT USAGE ON SCHEMA mysql TO PUBLIC;

CREATE SCHEMA IF NOT EXISTS performance_schema;
GRANT USAGE ON SCHEMA performance_schema TO PUBLIC;

CREATE SCHEMA IF NOT EXISTS sys;
GRANT USAGE ON SCHEMA sys TO PUBLIC;

CREATE SCHEMA IF NOT EXISTS mys_sys;
GRANT USAGE ON SCHEMA mys_sys TO PUBLIC;

-- MySQL DDL lowering uses these compatibility collations for its default
-- case/accent handling.  They must be extension-owned so a fresh install
-- does not depend on out-of-band bootstrap SQL.
CREATE COLLATION mysql.case_insensitive
    (provider = icu, locale = '@colStrength=secondary', deterministic = false);
CREATE COLLATION mysql.ignore_accents
    (provider = icu, locale = 'und-u-ks-level1-kc-true', deterministic = false);

-- -----------------------------------------------------------------------------
-- MySQL-compatible type domains
-- -----------------------------------------------------------------------------

CREATE DOMAIN mysql.tinyint AS pg_catalog.int2 CHECK ((-128 <= VALUE) AND (VALUE <= 127));
CREATE DOMAIN mysql."tinyint signed" AS pg_catalog.int2 CHECK ((-128 <= VALUE) AND (VALUE <= 127));
CREATE DOMAIN mysql."tinyint unsigned" AS pg_catalog.int2 CHECK ((0 <= VALUE) AND (VALUE <= 255));
CREATE DOMAIN mysql.tinyint1 AS pg_catalog.int2 CHECK ((-128 <= VALUE) AND (VALUE <= 127));
CREATE DOMAIN mysql."tinyint1 signed" AS pg_catalog.int2 CHECK ((-128 <= VALUE) AND (VALUE <= 127));
CREATE DOMAIN mysql."tinyint1 unsigned" AS pg_catalog.int2 CHECK ((0 <= VALUE) AND (VALUE <= 255));

CREATE DOMAIN mysql.smallint AS pg_catalog.int2;
CREATE DOMAIN mysql."smallint signed" AS pg_catalog.int2;
CREATE DOMAIN mysql."smallint unsigned" AS pg_catalog.int4 CHECK ((0 <= VALUE) AND (VALUE <= 65535));

CREATE DOMAIN mysql.mediumint AS pg_catalog.int4 CHECK ((-8388608 <= VALUE) AND (VALUE <= 8388607));
CREATE DOMAIN mysql."mediumint signed" AS pg_catalog.int4 CHECK ((-8388608 <= VALUE) AND (VALUE <= 8388607));
CREATE DOMAIN mysql."mediumint unsigned" AS pg_catalog.int4 CHECK ((0 <= VALUE) AND (VALUE <= 16777215));

CREATE DOMAIN mysql."int signed" AS pg_catalog.int4;
CREATE DOMAIN mysql."int unsigned" AS pg_catalog.int8 CHECK ((0 <= VALUE) AND (VALUE <= 4294967295));

CREATE DOMAIN mysql.int AS pg_catalog.int4;
CREATE DOMAIN mysql.integer AS pg_catalog.int4;

CREATE DOMAIN mysql."bigint signed" AS pg_catalog.int8;
CREATE DOMAIN mysql."bigint unsigned" AS pg_catalog.int8 CHECK ((0 <= VALUE) AND (VALUE <= 9223372036854775807));

CREATE DOMAIN mysql.real AS pg_catalog.float8;
CREATE DOMAIN mysql.double AS pg_catalog.float8;

CREATE DOMAIN mysql.datetime AS pg_catalog.timestamp;

CREATE DOMAIN mysql.binary AS pg_catalog.bytea;
CREATE DOMAIN mysql.varbinary AS pg_catalog.bytea;
CREATE DOMAIN mysql.blob AS pg_catalog.bytea;

CREATE DOMAIN mysql.year_ AS pg_catalog.int4 CHECK ((1901 <= VALUE) AND (VALUE <= 2155));

-- -----------------------------------------------------------------------------
-- Cast overrides: pg_catalog type casts for MySQL compatibility
--
-- NOTE: These UPDATE pg_cast statements change castcontext globally,
-- affecting ALL databases, not just the current one.  This is necessary
-- for MySQL WHERE-clause semantics (e.g. "WHERE int_col" treating
-- non-zero as true).  The downgrade script (1.1->1.0) restores the
-- original explicit-only castcontext.
-- -----------------------------------------------------------------------------

-- int -> boolean (MySQL: non-zero is true, zero is false)
UPDATE pg_cast SET castcontext = 'i' WHERE castsource = 23 AND casttarget = 16;
UPDATE pg_cast SET castcontext = 'a' WHERE castsource = 16 AND casttarget = 23;

-- -----------------------------------------------------------------------------
-- Boolean-to-integer casts (MySQL compatibility)
-- -----------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION mysql.cast_boolean_to_tinyint(pg_catalog.bool)
RETURNS int2
AS $$
BEGIN
    IF $1 THEN
        RETURN 1;
    ELSE
        RETURN 0;
    END IF;
END;
$$ LANGUAGE plpgsql STRICT;
CREATE CAST (pg_catalog.bool AS int2) WITH FUNCTION mysql.cast_boolean_to_tinyint AS ASSIGNMENT;

CREATE OR REPLACE FUNCTION mysql.cast_boolean_to_bigint(pg_catalog.bool)
RETURNS int8
AS $$
BEGIN
    IF $1 THEN
        RETURN 1;
    ELSE
        RETURN 0;
    END IF;
END;
$$ LANGUAGE plpgsql STRICT;
CREATE CAST (pg_catalog.bool AS int8) WITH FUNCTION mysql.cast_boolean_to_bigint AS ASSIGNMENT;

-- -----------------------------------------------------------------------------
-- MySQL-compatible scalar functions: v1.0 builtins
-- -----------------------------------------------------------------------------

-- repeat(text, int) -- wraps pg_catalog.repeat in the mysql schema.
-- MySQL REPEAT('ab', 3) => 'ababab'
CREATE OR REPLACE FUNCTION mysql.repeat(text, int)
RETURNS text
AS 'SELECT pg_catalog.repeat($1, $2)'
LANGUAGE SQL IMMUTABLE STRICT;

-- unhex(text) -- MySQL UNHEX('616263') => x'616263'::bytea
CREATE OR REPLACE FUNCTION mysql.unhex(text)
RETURNS bytea
AS $$SELECT pg_catalog.decode($1, 'hex')$$
LANGUAGE SQL IMMUTABLE STRICT;

-- to_base64(bytea) -- MySQL TO_BASE64(x'616263') => 'YWJj'
CREATE OR REPLACE FUNCTION mysql.to_base64(bytea)
RETURNS text
AS $$SELECT pg_catalog.encode($1, 'base64')$$
LANGUAGE SQL IMMUTABLE STRICT;

-- to_base64(text) -- MySQL TO_BASE64('abc') => 'YWJj'
CREATE OR REPLACE FUNCTION mysql.to_base64(text)
RETURNS text
AS $$SELECT pg_catalog.encode(pg_catalog.convert_to($1, 'UTF8'), 'base64')$$
LANGUAGE SQL IMMUTABLE STRICT;

-- from_base64(text) -- MySQL FROM_BASE64('YWJj') => x'616263'::bytea
CREATE OR REPLACE FUNCTION mysql.from_base64(text)
RETURNS bytea
AS $$SELECT pg_catalog.decode($1, 'base64')$$
LANGUAGE SQL IMMUTABLE STRICT;

-- =============================================================================
-- M3: MySQL-compatible information_schema (mys_informa_schema)
-- =============================================================================

-- -----------------------------------------------------------------------------
-- mys_informa_schema: MySQL clients query this schema for metadata.
-- The MySQL wire-protocol layer rewrites "information_schema.X" to
-- "mys_informa_schema.X" at parse time.
-- -----------------------------------------------------------------------------

CREATE SCHEMA IF NOT EXISTS mys_informa_schema;
GRANT USAGE ON SCHEMA mys_informa_schema TO PUBLIC;

-- -----------------------------------------------------------------------------
-- Helper: mysql.amend_def_val -- strip type casts and nextval from defaults
-- -----------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION mysql.amend_def_val(pg_catalog.text)
RETURNS pg_catalog.text
AS $$
DECLARE
    ret pg_catalog.text;
    tmp pg_catalog.int4;
BEGIN
    tmp := position('nextval' in $1);
    IF 1 = tmp THEN
        ret := NULL;
    ELSE
        tmp := position('::' in $1);
        IF 0 < tmp THEN
            ret := substring($1 from 1 for (tmp - 1));
            IF lower(ret) = 'null' THEN
                ret := NULL;
            END IF;
        ELSE
            ret := $1;
        END IF;
    END IF;
    RETURN ret;
END
$$ LANGUAGE plpgsql STRICT;

-- -----------------------------------------------------------------------------
-- Helper: mysql.get_seq_in_index -- sequence position of a column in an index
-- -----------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION mysql.get_seq_in_index(pg_catalog.text, pg_catalog.int2vector)
RETURNS pg_catalog.int2
AS $$
DECLARE
    items pg_catalog.text[];
    itemsNum int;
BEGIN
    items := string_to_array($2::text, ' ');
    itemsNum := array_length(items, 1);
    FOR i IN 1..itemsNum LOOP
        IF ($1 = items[i]) THEN
            RETURN i;
        END IF;
    END LOOP;
    RETURN 0;
END
$$ LANGUAGE plpgsql STRICT;

-- -----------------------------------------------------------------------------
-- View: mys_informa_schema.schemata
-- Maps to MySQL: SHOW DATABASES / SELECT * FROM information_schema.schemata
-- -----------------------------------------------------------------------------

CREATE OR REPLACE VIEW mys_informa_schema.schemata AS
SELECT
    'def'::varchar(256)                            AS CATALOG_NAME,
    d.datname::varchar(256)                        AS SCHEMA_NAME,
    'utf8mb4'::varchar(256)                        AS DEFAULT_CHARACTER_SET_NAME,
    'utf8mb4_general_ci'::varchar(256)             AS DEFAULT_COLLATION_NAME,
    NULL::varchar(256)                             AS SQL_PATH
FROM pg_catalog.pg_database d
ORDER BY 1, 2;

-- -----------------------------------------------------------------------------
-- View: mys_informa_schema.tables
-- Maps to MySQL: SHOW TABLES / SELECT * FROM information_schema.tables
-- -----------------------------------------------------------------------------

CREATE OR REPLACE VIEW mys_informa_schema.tables AS
SELECT
    'def'::varchar(256)                            AS TABLE_CATALOG,
    ns.nspname::varchar(256)                       AS TABLE_SCHEMA,
    cl.relname::varchar(256)                       AS TABLE_NAME,
    CASE cl.relkind
        WHEN 'r' THEN 'BASE TABLE'
        WHEN 'p' THEN 'BASE TABLE'
        WHEN 'v' THEN 'VIEW'
        ELSE 'BASE TABLE'
    END::varchar(256)                              AS TABLE_TYPE,
    'InnoDB'::varchar(256)                         AS ENGINE,
    10::bigint                                     AS VERSION,
    'Dynamic'::varchar(256)                        AS ROW_FORMAT,
    cl.reltuples::bigint                           AS TABLE_ROWS,
    100::bigint                                    AS AVG_ROW_LENGTH,
    16384::bigint                                  AS DATA_LENGTH,
    16384::bigint                                  AS MAX_DATA_LENGTH,
    16384::bigint                                  AS INDEX_LENGTH,
    16384::bigint                                  AS DATA_FREE,
    NULL::bigint                                   AS AUTO_INCREMENT,
    NULL::timestamp                                AS CREATE_TIME,
    NULL::timestamp                                AS UPDATE_TIME,
    NULL::timestamp                                AS CHECK_TIME,
    'utf8mb4_general_ci'::varchar(256)             AS TABLE_COLLATION,
    NULL::bigint                                   AS CHECKSUM,
    ''::varchar(256)                               AS CREATE_OPTIONS,
    pg_catalog.obj_description(cl.oid)::varchar(512) AS TABLE_COMMENT
FROM pg_catalog.pg_class cl
JOIN pg_catalog.pg_namespace ns ON ns.oid = cl.relnamespace
WHERE cl.relkind IN ('r', 'p', 'v')
  AND ns.nspname NOT IN ('pg_catalog', 'information_schema')
  AND ns.nspname !~ '^pg_'
ORDER BY 1, 2, 3;

-- -----------------------------------------------------------------------------
-- View: mys_informa_schema.columns
-- Maps to MySQL: SHOW COLUMNS FROM / DESCRIBE / SELECT * FROM
--                information_schema.columns
-- -----------------------------------------------------------------------------

CREATE OR REPLACE VIEW mys_informa_schema.columns AS
SELECT
    'def'::varchar(256)                            AS TABLE_CATALOG,
    ns.nspname::varchar(256)                       AS TABLE_SCHEMA,
    cl.relname::varchar(256)                       AS TABLE_NAME,
    att.attname::varchar(256)                      AS COLUMN_NAME,
    att.attnum::bigint                             AS ORDINAL_POSITION,
    mysql.amend_def_val(
        pg_catalog.pg_get_expr(ad.adbin, ad.adrelid)
    )::text                                        AS COLUMN_DEFAULT,
    CASE WHEN att.attnotnull THEN 'NO' ELSE 'YES'
    END::varchar(8)                                AS IS_NULLABLE,
    -- Map pg_catalog type names to MySQL type names
    CASE ty.typname
        WHEN 'int2'    THEN 'smallint'
        WHEN 'int4'    THEN 'int'
        WHEN 'int8'    THEN 'bigint'
        WHEN 'float4'  THEN 'float'
        WHEN 'float8'  THEN 'double'
        WHEN 'bool'    THEN 'tinyint'
        WHEN 'varchar' THEN 'varchar'
        WHEN 'bpchar'  THEN 'char'
        WHEN 'text'    THEN 'text'
        WHEN 'bytea'   THEN 'blob'
        WHEN 'time'    THEN 'time'
        WHEN 'date'    THEN 'date'
        WHEN 'timetz'  THEN 'time'
        WHEN 'timestamp'   THEN 'datetime'
        WHEN 'timestamptz' THEN 'datetime'
        WHEN 'numeric' THEN 'decimal'
        WHEN 'bit'     THEN 'bit'
        WHEN 'varbit'  THEN 'bit'
        ELSE ty.typname
    END::varchar(64)                               AS DATA_TYPE,
    CASE WHEN ty.typname IN ('varchar', 'bpchar')
         THEN att.atttypmod - 4
         ELSE NULL
    END::bigint                                    AS CHARACTER_MAXIMUM_LENGTH,
    NULL::bigint                                   AS CHARACTER_OCTET_LENGTH,
    NULL::bigint                                   AS NUMERIC_PRECISION,
    NULL::bigint                                   AS NUMERIC_SCALE,
    NULL::bigint                                   AS DATETIME_PRECISION,
    'utf8mb4'::varchar(256)                        AS CHARACTER_SET_NAME,
    'utf8mb4_general_ci'::varchar(256)             AS COLLATION_NAME,
    -- MySQL COLUMN_TYPE (includes length/precision)
    CASE ty.typname
        WHEN 'int2'    THEN 'smallint'
        WHEN 'int4'    THEN 'int(11)'
        WHEN 'int8'    THEN 'bigint(20)'
        WHEN 'float4'  THEN 'float'
        WHEN 'float8'  THEN 'double'
        WHEN 'bool'    THEN 'tinyint(1)'
        WHEN 'varchar' THEN pg_catalog.concat('varchar(', att.atttypmod - 4, ')')
        WHEN 'bpchar'  THEN pg_catalog.concat('char(', att.atttypmod - 4, ')')
        WHEN 'text'    THEN 'text'
        WHEN 'bytea'   THEN 'blob'
        WHEN 'time'    THEN 'time'
        WHEN 'date'    THEN 'date'
        WHEN 'timetz'  THEN 'time'
        WHEN 'timestamp'   THEN 'datetime'
        WHEN 'timestamptz' THEN 'datetime'
        WHEN 'numeric' THEN 'decimal'
        WHEN 'bit'     THEN pg_catalog.concat('bit(', att.atttypmod, ')')
        WHEN 'varbit'  THEN pg_catalog.concat('bit(',
                             CASE WHEN att.atttypmod = -1 THEN 1
                                  ELSE att.atttypmod END, ')')
        ELSE ty.typname
    END::varchar(256)                              AS COLUMN_TYPE,
    ''::text                                       AS EXTRA
FROM pg_catalog.pg_attribute att
JOIN pg_catalog.pg_class     cl  ON cl.oid = att.attrelid
JOIN pg_catalog.pg_namespace ns  ON ns.oid = cl.relnamespace
JOIN pg_catalog.pg_type      ty  ON ty.oid = att.atttypid
LEFT JOIN pg_catalog.pg_attrdef ad
    ON ad.adrelid = att.attrelid AND ad.adnum = att.attnum
WHERE att.attnum > 0
  AND NOT att.attisdropped
  AND cl.relkind IN ('r', 'p', 'v')
  AND ns.nspname NOT IN ('pg_catalog', 'information_schema')
  AND ns.nspname !~ '^pg_'
ORDER BY 1, 2, 3, 5;

-- -----------------------------------------------------------------------------
-- View: mys_informa_schema.statistics
-- Maps to MySQL: SHOW INDEX / SELECT * FROM information_schema.statistics
-- -----------------------------------------------------------------------------

CREATE OR REPLACE VIEW mys_informa_schema.statistics AS
SELECT
    'def'::varchar(256)                            AS TABLE_CATALOG,
    ns.nspname::varchar(256)                       AS TABLE_SCHEMA,
    cl.relname::varchar(256)                       AS TABLE_NAME,
    CASE WHEN ind.indisunique THEN 0 ELSE 1
    END::bigint                                    AS NON_UNIQUE,
    ns.nspname::varchar(256)                       AS INDEX_SCHEMA,
    ci.relname::varchar(256)                       AS INDEX_NAME,
    mysql.get_seq_in_index(
        att.attnum::pg_catalog.text,
        ind.indkey
    )::bigint                                      AS SEQ_IN_INDEX,
    att.attname::varchar(256)                      AS COLUMN_NAME,
    'A'::varchar(256)                              AS COLLATION,
    0::bigint                                      AS CARDINALITY,
    NULL::bigint                                   AS SUB_PART,
    NULL::varchar(256)                             AS PACKED,
    CASE WHEN att.attnotnull THEN '' ELSE 'YES'
    END::varchar(256)                              AS NULLABLE,
    am.amname::varchar(256)                        AS INDEX_TYPE,
    ''::varchar(256)                               AS COMMENT,
    ''::varchar(256)                               AS INDEX_COMMENT
FROM pg_catalog.pg_index     ind
JOIN pg_catalog.pg_class     cl  ON cl.oid = ind.indrelid
JOIN pg_catalog.pg_class     ci  ON ci.oid = ind.indexrelid
JOIN pg_catalog.pg_namespace ns  ON ns.oid = cl.relnamespace
JOIN pg_catalog.pg_am        am  ON am.oid = ci.relam
JOIN pg_catalog.pg_attribute att ON att.attrelid = cl.oid
                                 AND att.attnum = ANY(ind.indkey)
WHERE cl.relkind IN ('r', 'p')
  AND ns.nspname NOT IN ('pg_catalog', 'information_schema')
  AND ns.nspname !~ '^pg_'
ORDER BY 1, 2, 3, 4, 7;

-- View: mys_informa_schema.db_status
-- SHOW STATUS is backed by PostgreSQL's live GUC catalog.  This is a
-- compatibility surface, not a second server-status store.
CREATE OR REPLACE VIEW mys_informa_schema.db_status AS
SELECT name::varchar(256) AS variable_name,
       setting::text AS value
FROM pg_catalog.pg_settings;

-- -----------------------------------------------------------------------------
-- Grants for mys_informa_schema
-- -----------------------------------------------------------------------------

GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA mys_informa_schema TO PUBLIC;

-- -----------------------------------------------------------------------------
-- MySQL-compatible scalar functions: v1.1 string functions
-- -----------------------------------------------------------------------------

-- CONCAT(str1, str2, ...).  Unlike PostgreSQL concat(), MySQL returns NULL
-- whenever any argument is NULL.
CREATE OR REPLACE FUNCTION mysql.concat(VARIADIC text[])
RETURNS text
AS $$SELECT CASE WHEN array_position($1, NULL) IS NULL
                 THEN array_to_string($1, '')
            END$$
LANGUAGE SQL IMMUTABLE;

-- CONCAT_WS(sep, str1, str2, ...)
CREATE OR REPLACE FUNCTION mysql.concat_ws(text, VARIADIC text[])
RETURNS text
AS 'SELECT pg_catalog.concat_ws($1, VARIADIC $2)'
LANGUAGE SQL IMMUTABLE;

-- FORMAT(X, D) -- format number with D decimal places
-- MySQL FORMAT(12332.123456, 4) => '12,332.1235'
CREATE OR REPLACE FUNCTION mysql.format(numeric, int)
RETURNS text
AS $$
SELECT pg_catalog.to_char(
           $1,
           pg_catalog.concat(
               repeat('9', 30),
               'FM',
               CASE WHEN $2 > 0
                    THEN pg_catalog.concat('.', repeat('9', $2))
                    ELSE ''
               END))
$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.format(float8, int)
RETURNS text
AS $$
SELECT mysql.format($1::numeric, $2)
$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.format(int8, int)
RETURNS text
AS $$
SELECT mysql.format($1::numeric, $2)
$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.format(int4, int)
RETURNS text
AS $$
SELECT mysql.format($1::numeric, $2)
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- HEX(N_or_S) -- returns hexadecimal string
CREATE OR REPLACE FUNCTION mysql.hex(int8)
RETURNS text
AS 'SELECT pg_catalog.upper(pg_catalog.to_hex($1))'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.hex(int4)
RETURNS text
AS 'SELECT pg_catalog.upper(pg_catalog.to_hex($1))'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.hex(bytea)
RETURNS text
AS 'SELECT pg_catalog.upper(pg_catalog.encode($1, ''hex''))'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.hex(text)
RETURNS text
AS 'SELECT pg_catalog.upper(pg_catalog.encode($1::bytea, ''hex''))'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.bin(int8)
RETURNS text
AS $$WITH RECURSIVE bits(value, result) AS (
         SELECT $1, ''::text
         UNION ALL
         SELECT value >> 1, result || (value & 1::int8)::text
         FROM bits WHERE value > 0
     )
     SELECT CASE WHEN $1 = 0 THEN '0' ELSE reverse(result) END
     FROM bits WHERE value = 0$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.bin(int4)
RETURNS text
AS 'SELECT mysql.bin($1::int8)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.bit_count(int8)
RETURNS int
AS $$SELECT count(*)::int
     FROM generate_series(0, 63) AS g(bit)
     WHERE ($1 & (1::int8 << bit)) <> 0$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.bit_count(int4)
RETURNS int
AS 'SELECT mysql.bit_count($1::int8)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.conv(text, int, int)
RETURNS text
AS $$WITH RECURSIVE parsed(position, value) AS (
         SELECT 1, 0::int8
         UNION ALL
         SELECT position + 1,
                value * $2 + strpos('0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ',
                                    upper(substr($1, position, 1))) - 1
         FROM parsed
         WHERE position <= length($1)
     ), converted(value, result) AS (
         SELECT value, ''::text
         FROM parsed WHERE position = length($1) + 1
         UNION ALL
         SELECT value / $3,
                substr('0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ',
                       (value % $3)::int + 1, 1) || result
         FROM converted WHERE value > 0
     )
     SELECT CASE WHEN value = 0 AND result = '' THEN '0' ELSE result END
     FROM converted WHERE value = 0$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.ifnull(anycompatible, anycompatible)
RETURNS anycompatible
AS 'SELECT COALESCE($1, $2)'
LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION mysql.ifnull(int4, int4)
RETURNS int4
AS 'SELECT COALESCE($1, $2)'
LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION mysql.if(boolean, anycompatible, anycompatible)
RETURNS anycompatible
AS 'SELECT CASE WHEN $1 THEN $2 ELSE $3 END'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.if(int4, anycompatible, anycompatible)
RETURNS anycompatible
AS 'SELECT CASE WHEN $1 <> 0 THEN $2 ELSE $3 END'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.if(int4, text, text)
RETURNS text
AS 'SELECT CASE WHEN $1 <> 0 THEN $2 ELSE $3 END'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.isnull(anyelement)
RETURNS int
AS 'SELECT CASE WHEN $1 IS NULL THEN 1 ELSE 0 END'
LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION mysql.isnull(text)
RETURNS int
AS 'SELECT CASE WHEN $1 IS NULL THEN 1 ELSE 0 END'
LANGUAGE SQL IMMUTABLE;

-- Kept compatible with the UDB-TX PG16 implementation.  JSON values are
-- represented by PostgreSQL's json type, whose scalar-string text form is
-- quoted; remove that outer quoting for MySQL JSON_UNQUOTE().
CREATE OR REPLACE FUNCTION mysql.json_unquote(json)
RETURNS text
AS 'SELECT trim(''"'' FROM $1::text)'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION mysql.json_path_extract_text(json, text)
RETURNS text
AS $$
SELECT CASE WHEN $2 LIKE '$.%'
            THEN pg_catalog.json_extract_path_text(
                     $1,
                     VARIADIC pg_catalog.string_to_array(
                         pg_catalog.substr($2, 3), '.'))
            ELSE pg_catalog.json_extract_path_text($1, $2)
       END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- INSTR(str, substr) -- returns position of first occurrence
CREATE OR REPLACE FUNCTION mysql.instr(text, text)
RETURNS int
AS $$
SELECT CASE WHEN $2 = '' THEN 1
            WHEN pg_catalog.strpos($1, $2) = 0 THEN 0
            ELSE pg_catalog.strpos($1, $2) END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- LEFT(str, len) and RIGHT(str, len)
CREATE OR REPLACE FUNCTION mysql.left(text, int)
RETURNS text
AS $$SELECT CASE WHEN $2 <= 0 THEN ''
                 ELSE pg_catalog.substr($1, 1, $2) END$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.right(text, int)
RETURNS text
AS $$SELECT CASE WHEN $2 <= 0 THEN ''
                 WHEN $2 >= pg_catalog.char_length($1) THEN $1
                 ELSE pg_catalog.substr($1, pg_catalog.char_length($1) - $2 + 1) END$$
LANGUAGE SQL IMMUTABLE STRICT;

-- SUBSTR(str, pos, len) uses one-based positions; negative positions count
-- backwards from the end, as in MySQL.
CREATE OR REPLACE FUNCTION mysql.substr(text, int, int)
RETURNS text
AS $$SELECT CASE WHEN $2 = 0 OR $3 <= 0 THEN ''
                 WHEN $2 > 0 THEN pg_catalog.substr($1, $2, $3)
                 ELSE pg_catalog.substr($1, pg_catalog.char_length($1) + $2 + 1, $3)
            END$$
LANGUAGE SQL IMMUTABLE STRICT;

-- LOCATE(substr, str [, pos])
CREATE OR REPLACE FUNCTION mysql.locate(text, text)
RETURNS int
AS 'SELECT mysql.locate($1, $2, 1)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.locate(text, text, int)
RETURNS int
AS $$SELECT CASE WHEN $3 <= 0 THEN 0
                 WHEN pg_catalog.strpos(pg_catalog.substr($2, $3), $1) = 0 THEN 0
                 ELSE $3 - 1 + pg_catalog.strpos(pg_catalog.substr($2, $3), $1)
            END$$
LANGUAGE SQL IMMUTABLE STRICT;

-- LCASE(str)
CREATE OR REPLACE FUNCTION mysql.lcase(text)
RETURNS text
AS 'SELECT pg_catalog.lower($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- UCASE(str)
CREATE OR REPLACE FUNCTION mysql.ucase(text)
RETURNS text
AS 'SELECT pg_catalog.upper($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- LPAD(str, len, padstr)
CREATE OR REPLACE FUNCTION mysql.lpad(text, int, text)
RETURNS text
AS 'SELECT pg_catalog.lpad($1, $2, $3)'
LANGUAGE SQL IMMUTABLE STRICT;

-- RPAD(str, len, padstr)
CREATE OR REPLACE FUNCTION mysql.rpad(text, int, text)
RETURNS text
AS 'SELECT pg_catalog.rpad($1, $2, $3)'
LANGUAGE SQL IMMUTABLE STRICT;

-- LTRIM(str)
CREATE OR REPLACE FUNCTION mysql.ltrim(text)
RETURNS text
AS 'SELECT pg_catalog.ltrim($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- RTRIM(str)
CREATE OR REPLACE FUNCTION mysql.rtrim(text)
RETURNS text
AS 'SELECT pg_catalog.rtrim($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- SPACE(n)
CREATE OR REPLACE FUNCTION mysql.space(int)
RETURNS text
AS 'SELECT pg_catalog.repeat('' '', $1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- STRCMP(expr1, expr2) -- returns 0 if same, -1 if expr1 < expr2, 1 if expr1 > expr2
CREATE OR REPLACE FUNCTION mysql.strcmp(text, text)
RETURNS int
AS $$
SELECT CASE WHEN $1 < $2 THEN -1
            WHEN $1 > $2 THEN 1
            ELSE 0 END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- SUBSTRING_INDEX(str, delim, count)
CREATE OR REPLACE FUNCTION mysql.substring_index(text, text, int)
RETURNS text
AS $$
SELECT CASE
    WHEN $3 > 0 THEN pg_catalog.array_to_string((pg_catalog.string_to_array($1, $2))[1:$3], $2)
    WHEN $3 < 0 THEN pg_catalog.array_to_string(
        (pg_catalog.string_to_array($1, $2))
        [pg_catalog.array_length(pg_catalog.string_to_array($1, $2), 1) + $3 + 1 :
         pg_catalog.array_length(pg_catalog.string_to_array($1, $2), 1)],
        $2)
    ELSE ''
END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- ELT(N, str1, str2, ...), FIELD(str, str1, str2, ...), and FIND_IN_SET.
CREATE OR REPLACE FUNCTION mysql.elt(int, VARIADIC text[])
RETURNS text
AS $$SELECT CASE WHEN $1 BETWEEN 1 AND cardinality($2) THEN $2[$1] END$$
LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION mysql.field(text, VARIADIC text[])
RETURNS int
AS $$SELECT COALESCE(array_position($2, $1), 0)$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.find_in_set(text, text)
RETURNS int
AS $$SELECT CASE WHEN $1 = '' THEN 0
                 ELSE COALESCE(array_position(string_to_array($2, ','), $1), 0)
            END$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.make_set(int8, VARIADIC text[])
RETURNS text
AS $$SELECT COALESCE(string_agg(value, ',' ORDER BY ordinality), '')
     FROM unnest($2) WITH ORDINALITY AS u(value, ordinality)
     WHERE ($1 & (1::int8 << ((ordinality - 1)::int))) <> 0$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.export_set(int8, text, text, text, int)
RETURNS text
AS $$SELECT string_agg(CASE WHEN ($1 & (1::int8 << (i::int))) <> 0 THEN $2 ELSE $3 END,
                            $4 ORDER BY i)
     FROM generate_series(0, GREATEST($5 - 1, 0)) AS g(i)$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.convert_bigint_to_varbit(pg_catalog.int8)
RETURNS pg_catalog.varbit
AS '$libdir/mysm', 'convertBigintToVarbit'
LANGUAGE C STRICT IMMUTABLE;

-- MySQL-compatible JSON functions.  These used to be registered in the
-- kernel pg_proc.dat (json int2/int4/int8/float4/float8/numeric and
-- mys_json_object); they now live in the mysm shared library.
CREATE OR REPLACE FUNCTION mysql.json(pg_catalog.int2)
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_to_json'
LANGUAGE C STABLE STRICT;
CREATE OR REPLACE FUNCTION mysql.json(pg_catalog.int4)
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_to_json'
LANGUAGE C STABLE STRICT;
CREATE OR REPLACE FUNCTION mysql.json(pg_catalog.int8)
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_to_json'
LANGUAGE C STABLE STRICT;
CREATE OR REPLACE FUNCTION mysql.json(pg_catalog.float4)
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_to_json'
LANGUAGE C STABLE STRICT;
CREATE OR REPLACE FUNCTION mysql.json(pg_catalog.float8)
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_to_json'
LANGUAGE C STABLE STRICT;
CREATE OR REPLACE FUNCTION mysql.json(pg_catalog.numeric)
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_to_json'
LANGUAGE C STABLE STRICT;
CREATE OR REPLACE FUNCTION mysql.mys_json_object(VARIADIC "any")
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_json_object'
LANGUAGE C STABLE CALLED ON NULL INPUT;
CREATE OR REPLACE FUNCTION mysql.mys_json_object()
RETURNS pg_catalog.json
AS '$libdir/mysm', 'mys_json_object_noargs'
LANGUAGE C STABLE CALLED ON NULL INPUT;

CREATE OR REPLACE FUNCTION mysql.cast_bigint_to_varbit(pg_catalog.int8)
RETURNS pg_catalog.varbit
AS
$$
BEGIN
    RETURN mysql.convert_bigint_to_varbit($1);
END;
$$
LANGUAGE plpgsql STRICT;

CREATE CAST (pg_catalog.int8 AS pg_catalog.varbit)
WITH FUNCTION mysql.cast_bigint_to_varbit AS ASSIGNMENT;

CREATE OR REPLACE FUNCTION mysql.insert(text, int, int, text)
RETURNS text
AS $$SELECT CASE WHEN $2 < 1 OR $2 > char_length($1) THEN $1
                 ELSE substr($1, 1, $2 - 1) || $4 || substr($1, $2 + GREATEST($3, 0))
            END$$
LANGUAGE SQL IMMUTABLE STRICT;

-- TRIM with custom remstr (BOTH | LEADING | TRAILING)
-- MySQL TRIM([{BOTH|LEADING|TRAILING} [remstr] FROM] str)
CREATE OR REPLACE FUNCTION mysql.trim(text)
RETURNS text
AS 'SELECT pg_catalog.btrim($1)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.trim(text, text)
RETURNS text
AS 'SELECT pg_catalog.btrim($1, $2)'
LANGUAGE SQL IMMUTABLE STRICT;

-- -----------------------------------------------------------------------------
-- MySQL-compatible scalar functions: v1.1 math functions
-- -----------------------------------------------------------------------------

-- CEILING(X)
CREATE OR REPLACE FUNCTION mysql.ceiling(numeric)
RETURNS numeric
AS 'SELECT pg_catalog.ceil($1)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.ceiling(float8)
RETURNS float8
AS 'SELECT pg_catalog.ceil($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- CRC32(str) -- MySQL CRC32
-- PostgreSQL has no built-in CRC32; use a plpgsql implementation
CREATE OR REPLACE FUNCTION mysql.crc32(text)
RETURNS int8
AS $$
DECLARE
    c bigint := 4294967295;  -- 0xFFFFFFFF
    i int;
    ch int;
BEGIN
    FOR i IN 1..pg_catalog.length($1) LOOP
        ch := pg_catalog.ascii(pg_catalog.substr($1, i, 1));
        c := (c # ch) & 4294967295;
        FOR j IN 0..7 LOOP
            IF (c & 1) = 1 THEN
                c := ((c >> 1) # 3988292384) & 4294967295;
            ELSE
                c := (c >> 1) & 4294967295;
            END IF;
        END LOOP;
    END LOOP;
    RETURN (c # 4294967295) & 4294967295;
END;
$$ LANGUAGE plpgsql IMMUTABLE STRICT;

-- DEGREES(X)
CREATE OR REPLACE FUNCTION mysql.degrees(float8)
RETURNS float8
AS 'SELECT pg_catalog.degrees($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- FLOOR(X)
CREATE OR REPLACE FUNCTION mysql.floor(numeric)
RETURNS numeric
AS 'SELECT pg_catalog.floor($1)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.floor(float8)
RETURNS float8
AS 'SELECT pg_catalog.floor($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- LN(X)
CREATE OR REPLACE FUNCTION mysql.ln(float8)
RETURNS float8
AS 'SELECT pg_catalog.ln($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- LOG2(X)
CREATE OR REPLACE FUNCTION mysql.log2(float8)
RETURNS float8
AS 'SELECT pg_catalog.log(2.0::numeric, $1::numeric)::float8'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.log2(numeric)
RETURNS numeric
AS 'SELECT pg_catalog.log(2.0, $1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- LOG10(X)
CREATE OR REPLACE FUNCTION mysql.log10(float8)
RETURNS float8
AS 'SELECT pg_catalog.log($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- MOD(N, M)
CREATE OR REPLACE FUNCTION mysql.mod(numeric, numeric)
RETURNS numeric
AS 'SELECT pg_catalog.mod($1, $2)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.mod(float8, float8)
RETURNS float8
AS 'SELECT pg_catalog.mod($1::numeric, $2::numeric)::float8'
LANGUAGE SQL IMMUTABLE STRICT;

-- PI()
CREATE OR REPLACE FUNCTION mysql.pi()
RETURNS float8
AS 'SELECT pg_catalog.pi()'
LANGUAGE SQL IMMUTABLE;

-- POW(X, Y)
CREATE OR REPLACE FUNCTION mysql.pow(float8, float8)
RETURNS float8
AS 'SELECT pg_catalog.power($1, $2)'
LANGUAGE SQL IMMUTABLE STRICT;

-- RADIANS(X)
CREATE OR REPLACE FUNCTION mysql.radians(float8)
RETURNS float8
AS 'SELECT pg_catalog.radians($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- RAND([N])
CREATE OR REPLACE FUNCTION mysql.rand()
RETURNS float8
AS 'SELECT pg_catalog.random()'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION mysql.rand(int4)
RETURNS float8
AS 'SELECT (((($1::int8 * 1103515245 + 12345) % 2147483647 + 2147483647) % 2147483647)::float8 / 2147483647.0)'
LANGUAGE SQL IMMUTABLE STRICT;

-- ROUND(X[, D])
CREATE OR REPLACE FUNCTION mysql.round(numeric, int DEFAULT 0)
RETURNS numeric
AS 'SELECT pg_catalog.round($1, $2)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.round(float8, int DEFAULT 0)
RETURNS float8
AS 'SELECT pg_catalog.round($1::numeric, $2)::float8'
LANGUAGE SQL IMMUTABLE STRICT;

-- SIGN(X)
CREATE OR REPLACE FUNCTION mysql.sign(numeric)
RETURNS numeric
AS 'SELECT pg_catalog.sign($1)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.sign(float8)
RETURNS float8
AS 'SELECT pg_catalog.sign($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- SQRT(X)
CREATE OR REPLACE FUNCTION mysql.sqrt(numeric)
RETURNS numeric
AS 'SELECT pg_catalog.sqrt($1)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.sqrt(float8)
RETURNS float8
AS 'SELECT pg_catalog.sqrt($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- TRUNCATE(X, D)
CREATE OR REPLACE FUNCTION mysql.truncate(numeric, int)
RETURNS numeric
AS 'SELECT pg_catalog.trunc($1, $2)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.truncate(float8, int)
RETURNS float8
AS 'SELECT pg_catalog.trunc($1::numeric, $2)::float8'
LANGUAGE SQL IMMUTABLE STRICT;

-- -----------------------------------------------------------------------------
-- MySQL-compatible scalar functions: v1.1 date/time functions
-- -----------------------------------------------------------------------------

-- CURDATE()
CREATE OR REPLACE FUNCTION mysql.curdate()
RETURNS date
AS 'SELECT current_date'
LANGUAGE SQL;

-- CURTIME()
CREATE OR REPLACE FUNCTION mysql.curtime()
RETURNS time
AS 'SELECT current_time'
LANGUAGE SQL;

-- CURRENT_DATE (alias)
CREATE OR REPLACE FUNCTION mysql.current_date()
RETURNS date
AS 'SELECT current_date'
LANGUAGE SQL;

-- CURRENT_TIME (alias)
CREATE OR REPLACE FUNCTION mysql.current_time()
RETURNS time
AS 'SELECT current_time'
LANGUAGE SQL;

-- DATE_ADD(date, INTERVAL expr unit) -- simplified: takes date/timestamp + interval
CREATE OR REPLACE FUNCTION mysql.date_add(timestamp, pg_catalog.interval)
RETURNS timestamp
AS 'SELECT $1 + $2'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.date_add(date, pg_catalog.interval)
RETURNS date
AS 'SELECT ($1 + $2)::date'
LANGUAGE SQL IMMUTABLE STRICT;

-- PG16 UDB-TX supplies a text overload so an untyped MySQL date literal does
-- not ambiguously match date and timestamp overloads.  PG18's native
-- timestamp input accepts the ISO literal syntax used by MySQL clients.
CREATE OR REPLACE FUNCTION mysql.date_add(text, pg_catalog.interval)
RETURNS timestamp
AS 'SELECT $1::pg_catalog.timestamp + $2'
LANGUAGE SQL IMMUTABLE STRICT;

-- DATE_SUB(date, INTERVAL expr unit) -- simplified: takes date/timestamp - interval
CREATE OR REPLACE FUNCTION mysql.date_sub(timestamp, pg_catalog.interval)
RETURNS timestamp
AS 'SELECT $1 - $2'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.date_sub(date, pg_catalog.interval)
RETURNS date
AS 'SELECT ($1 - $2)::date'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.date_sub(text, pg_catalog.interval)
RETURNS timestamp
AS 'SELECT $1::pg_catalog.timestamp - $2'
LANGUAGE SQL IMMUTABLE STRICT;

-- ADDDATE/SUBDATE are MySQL aliases with an integer-day form.  Retain the
-- PG16 text-first dispatch so bare literals select a single overload.
CREATE OR REPLACE FUNCTION mysql.adddate(text, integer)
RETURNS timestamp
AS 'SELECT $1::pg_catalog.timestamp + (''1 day''::pg_catalog.interval * $2)'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.subdate(text, integer)
RETURNS timestamp
AS 'SELECT $1::pg_catalog.timestamp - (''1 day''::pg_catalog.interval * $2)'
LANGUAGE SQL IMMUTABLE STRICT;

-- DATEDIFF(expr1, expr2) -- returns expr1 - expr2 in days
CREATE OR REPLACE FUNCTION mysql.datediff(date, date)
RETURNS int
AS 'SELECT $1 - $2'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.datediff(timestamp, timestamp)
RETURNS int
AS 'SELECT ($1::date - $2::date)'
LANGUAGE SQL IMMUTABLE STRICT;

-- Match the PG16 compatibility overload used for untyped MySQL date
-- literals, while using PG18's native ISO date input conversion.
CREATE OR REPLACE FUNCTION mysql.datediff(text, text)
RETURNS int
AS 'SELECT $1::pg_catalog.date - $2::pg_catalog.date'
LANGUAGE SQL IMMUTABLE STRICT;

-- DAY(date)
CREATE OR REPLACE FUNCTION mysql.day(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''day'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.day(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''day'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- DAYNAME(date)
CREATE OR REPLACE FUNCTION mysql.dayname(date)
RETURNS text
AS 'SELECT pg_catalog.btrim(pg_catalog.to_char($1, ''Day''))'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.dayname(timestamp)
RETURNS text
AS 'SELECT pg_catalog.btrim(pg_catalog.to_char($1::date, ''Day''))'
LANGUAGE SQL IMMUTABLE STRICT;

-- DAYOFMONTH(date)
CREATE OR REPLACE FUNCTION mysql.dayofmonth(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''day'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- DAYOFWEEK(date) -- MySQL: 1=Sunday, 7=Saturday; PG: 0=Sunday, 6=Saturday
CREATE OR REPLACE FUNCTION mysql.dayofweek(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''dow'', $1)::int + 1'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.dayofweek(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''dow'', $1)::int + 1'
LANGUAGE SQL IMMUTABLE STRICT;

-- DAYOFYEAR(date)
CREATE OR REPLACE FUNCTION mysql.dayofyear(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''doy'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.dayofyear(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''doy'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- HOUR(time)
CREATE OR REPLACE FUNCTION mysql.hour(time)
RETURNS int
AS 'SELECT pg_catalog.date_part(''hour'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.hour(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''hour'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- MINUTE(time)
CREATE OR REPLACE FUNCTION mysql.minute(time)
RETURNS int
AS 'SELECT pg_catalog.date_part(''minute'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.minute(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''minute'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- MONTH(date)
CREATE OR REPLACE FUNCTION mysql.month(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''month'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.month(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''month'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- MONTHNAME(date)
CREATE OR REPLACE FUNCTION mysql.monthname(date)
RETURNS text
AS 'SELECT pg_catalog.btrim(pg_catalog.to_char($1, ''Month''))'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.monthname(timestamp)
RETURNS text
AS 'SELECT pg_catalog.btrim(pg_catalog.to_char($1::date, ''Month''))'
LANGUAGE SQL IMMUTABLE STRICT;

-- NOW()
CREATE OR REPLACE FUNCTION mysql.now()
RETURNS timestamp
AS 'SELECT pg_catalog.now()'
LANGUAGE SQL;

-- CURRENT_TIMESTAMP (alias)
CREATE OR REPLACE FUNCTION mysql.current_timestamp()
RETURNS timestamp
AS 'SELECT pg_catalog.now()'
LANGUAGE SQL;

-- QUARTER(date)
CREATE OR REPLACE FUNCTION mysql.quarter(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''quarter'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.quarter(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''quarter'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- SECOND(time)
CREATE OR REPLACE FUNCTION mysql.second(time)
RETURNS int
AS 'SELECT pg_catalog.floor(pg_catalog.date_part(''second'', $1))::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.second(timestamp)
RETURNS int
AS 'SELECT pg_catalog.floor(pg_catalog.date_part(''second'', $1))::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- SYSDATE() -- returns execution time (like clock_timestamp)
CREATE OR REPLACE FUNCTION mysql.sysdate()
RETURNS timestamp
AS 'SELECT pg_catalog.clock_timestamp()'
LANGUAGE SQL;

-- TIMESTAMPDIFF(unit, datetime_expr1, datetime_expr2)
CREATE OR REPLACE FUNCTION mysql.timestampdiff(text, timestamp, timestamp)
RETURNS int8
AS $$
SELECT CASE pg_catalog.lower($1)
    WHEN 'microsecond' THEN (pg_catalog.date_part('epoch', $3 - $2) * 1000000)::int8
    WHEN 'second' THEN pg_catalog.date_part('epoch', $3 - $2)::int8
    WHEN 'minute' THEN pg_catalog.date_part('epoch', $3 - $2)::int8 / 60
    WHEN 'hour' THEN pg_catalog.date_part('epoch', $3 - $2)::int8 / 3600
    WHEN 'day' THEN ($3::date - $2::date)
    WHEN 'week' THEN (($3::date - $2::date) / 7)
    WHEN 'month' THEN ((pg_catalog.date_part('year', $3)::int - pg_catalog.date_part('year', $2)::int) * 12
                       + pg_catalog.date_part('month', $3)::int - pg_catalog.date_part('month', $2)::int)
    WHEN 'quarter' THEN ((pg_catalog.date_part('year', $3)::int - pg_catalog.date_part('year', $2)::int) * 4
                         + pg_catalog.date_part('quarter', $3)::int - pg_catalog.date_part('quarter', $2)::int)
    WHEN 'year' THEN (pg_catalog.date_part('year', $3)::int - pg_catalog.date_part('year', $2)::int)
    ELSE 0
END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- TO_DAYS(date) -- number of days since year 0 (MySQL epoch)
CREATE OR REPLACE FUNCTION mysql.to_days(date)
RETURNS int
AS 'SELECT $1 - ''0001-01-01''::date + 366'
LANGUAGE SQL IMMUTABLE STRICT;

-- WEEK(date[, mode])
CREATE OR REPLACE FUNCTION mysql.week(date, int DEFAULT 0)
RETURNS int
AS $$
SELECT CASE $2
    WHEN 0 THEN pg_catalog.date_part('week', $1)::int  -- approximation
    WHEN 1 THEN pg_catalog.date_part('week', $1)::int
    ELSE pg_catalog.date_part('week', $1)::int
END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- WEEKDAY(date) -- MySQL: 0=Monday, 6=Sunday; PG dow: 0=Sunday, 6=Saturday
CREATE OR REPLACE FUNCTION mysql.weekday(date)
RETURNS int
AS $$
SELECT CASE pg_catalog.date_part('dow', $1)::int
    WHEN 0 THEN 6  -- Sunday
    ELSE pg_catalog.date_part('dow', $1)::int - 1
END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- YEAR(date)
CREATE OR REPLACE FUNCTION mysql.year(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''year'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.year(timestamp)
RETURNS int
AS 'SELECT pg_catalog.date_part(''year'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- YEARWEEK(date)
CREATE OR REPLACE FUNCTION mysql.yearweek(date)
RETURNS int
AS 'SELECT pg_catalog.date_part(''year'', $1)::int * 100 + pg_catalog.date_part(''week'', $1)::int'
LANGUAGE SQL IMMUTABLE STRICT;

-- -----------------------------------------------------------------------------
-- MySQL-compatible scalar functions: v1.1 crypto functions
-- -----------------------------------------------------------------------------

-- MD5(str)
CREATE OR REPLACE FUNCTION mysql.md5(text)
RETURNS text
AS 'SELECT pg_catalog.md5($1)'
LANGUAGE SQL IMMUTABLE STRICT;

-- SHA1(str) -- requires pgcrypto extension for digest()
CREATE OR REPLACE FUNCTION mysql.sha1(text)
RETURNS text
AS 'SELECT pg_catalog.encode(digest($1, ''sha1''), ''hex'')'
LANGUAGE SQL IMMUTABLE STRICT;

-- SHA2(str, hash_length) -- requires pgcrypto extension for digest()
CREATE OR REPLACE FUNCTION mysql.sha2(text, int)
RETURNS text
AS $$
SELECT CASE $2
    WHEN 0 THEN pg_catalog.encode(digest($1, 'sha256'), 'hex')
    WHEN 224 THEN pg_catalog.encode(digest($1, 'sha224'), 'hex')
    WHEN 256 THEN pg_catalog.encode(digest($1, 'sha256'), 'hex')
    WHEN 384 THEN pg_catalog.encode(digest($1, 'sha384'), 'hex')
    WHEN 512 THEN pg_catalog.encode(digest($1, 'sha512'), 'hex')
    ELSE NULL
END
$$
LANGUAGE SQL IMMUTABLE STRICT;

-- -----------------------------------------------------------------------------
-- MySQL-compatible scalar functions: v1.1 information functions
-- -----------------------------------------------------------------------------

-- DATABASE() — returns the current MySQL database, which maps to the
-- first non-system schema in the PostgreSQL search_path.  This is the
-- schema set by the last USE (SET search_path) command.
CREATE OR REPLACE FUNCTION mysql.database()
RETURNS text
AS 'SELECT pg_catalog.current_schema()'
LANGUAGE SQL;

-- SCHEMA() — synonym for DATABASE() per MySQL semantics
CREATE OR REPLACE FUNCTION mysql.schema()
RETURNS text
AS 'SELECT pg_catalog.current_schema()'
LANGUAGE SQL;

-- USER()
CREATE OR REPLACE FUNCTION mysql.user()
RETURNS text
AS 'SELECT current_user'
LANGUAGE SQL;

-- CURRENT_USER() and SESSION_USER() are MySQL information functions as well
-- as SQL keywords.  Keep schema-qualified forms available to clients.
CREATE OR REPLACE FUNCTION mysql.current_user()
RETURNS text
AS 'SELECT current_user'
LANGUAGE SQL;

CREATE OR REPLACE FUNCTION mysql.session_user()
RETURNS text
AS 'SELECT session_user'
LANGUAGE SQL;

-- VERSION()
CREATE OR REPLACE FUNCTION mysql.version()
RETURNS text
AS 'SELECT pg_catalog.current_setting(''mysql_server_version'')'
LANGUAGE SQL;

-- CONNECTION_ID()
CREATE OR REPLACE FUNCTION mysql.connection_id()
RETURNS int
AS 'SELECT pg_catalog.pg_backend_pid()'
LANGUAGE SQL;

-- LAST_INSERT_ID()
CREATE OR REPLACE FUNCTION mysql.last_insert_id()
RETURNS int8
AS 'SELECT pg_catalog.mys_last_insert_id()'
LANGUAGE SQL VOLATILE;

CREATE OR REPLACE FUNCTION mysql.row_count()
RETURNS int8
AS 'SELECT pg_catalog.mys_row_count()'
LANGUAGE SQL VOLATILE;

-- FOUND_ROWS()
CREATE OR REPLACE FUNCTION mysql.found_rows()
RETURNS int8
AS 'SELECT pg_catalog.mys_found_rows()'
LANGUAGE SQL VOLATILE;
-- PG16 UDB-TX DATE_FORMAT semantics, adapted to PG18's available week APIs.
CREATE OR REPLACE FUNCTION mysql.date_format(timestamp without time zone, text)
RETURNS text
AS
$$
DECLARE
    len int := pg_catalog.length($2);
    i int := 1;
    temp text := '';
    c text;
    n character;
    res text;
BEGIN
    WHILE i <= len LOOP
        c := substring($2 FROM i FOR 1);
        IF c = '%' AND i != len THEN
            n := (substring($2 FROM (i + 1) FOR 1))::character;
            SELECT INTO res CASE
                WHEN n = 'a' THEN pg_catalog.to_char($1, 'Dy')
                WHEN n = 'b' THEN pg_catalog.to_char($1, 'Mon')
                WHEN n = 'c' THEN pg_catalog.to_char($1, 'FMMM')
                WHEN n = 'D' THEN pg_catalog.to_char($1, 'FMDDth')
                WHEN n = 'd' THEN pg_catalog.to_char($1, 'DD')
                WHEN n = 'e' THEN pg_catalog.to_char($1, 'FMDD')
                WHEN n = 'f' THEN pg_catalog.to_char($1, 'US')
                WHEN n = 'H' THEN pg_catalog.to_char($1, 'HH24')
                WHEN n = 'h' THEN pg_catalog.to_char($1, 'HH12')
                WHEN n = 'I' THEN pg_catalog.to_char($1, 'HH12')
                WHEN n = 'i' THEN pg_catalog.to_char($1, 'MI')
                WHEN n = 'j' THEN pg_catalog.to_char($1, 'DDD')
                WHEN n = 'k' THEN pg_catalog.to_char($1, 'FMHH24')
                WHEN n = 'l' THEN pg_catalog.to_char($1, 'FMHH12')
                WHEN n = 'M' THEN pg_catalog.to_char($1, 'FMMonth')
                WHEN n = 'm' THEN pg_catalog.to_char($1, 'MM')
                WHEN n = 'p' THEN pg_catalog.to_char($1, 'AM')
                WHEN n = 'r' THEN pg_catalog.to_char($1, 'HH12:MI:SS AM')
                WHEN n = 'S' THEN pg_catalog.to_char($1, 'SS')
                WHEN n = 's' THEN pg_catalog.to_char($1, 'SS')
                WHEN n = 'T' THEN pg_catalog.to_char($1, 'HH24:MI:SS')
                WHEN n = 'U' THEN pg_catalog.lpad(mysql.week($1::date, 0)::text, 2, '0')
                WHEN n = 'u' THEN pg_catalog.lpad(mysql.week($1::date, 1)::text, 2, '0')
                WHEN n = 'V' THEN pg_catalog.lpad(mysql.week($1::date, 2)::text, 2, '0')
                WHEN n = 'v' THEN pg_catalog.lpad(mysql.week($1::date, 3)::text, 2, '0')
                WHEN n = 'W' THEN pg_catalog.to_char($1, 'FMDay')
                WHEN n = 'w' THEN EXTRACT(DOW FROM $1)::text
                WHEN n = 'X' THEN pg_catalog.to_char(($1::date + 1), 'IYYY')
                WHEN n = 'x' THEN pg_catalog.to_char($1::date, 'IYYY')
                WHEN n = 'Y' THEN pg_catalog.to_char($1, 'YYYY')
                WHEN n = 'y' THEN pg_catalog.to_char($1, 'YY')
                WHEN n = '%' THEN pg_catalog.to_char($1, '%')
                ELSE NULL
            END;

            if res is not null then
                temp := temp operator(pg_catalog.||) res;
                i := i + 2;
            else
                i := i + 1;
            end if;
        ELSE
            temp := temp operator(pg_catalog.||) c;
            i := i + 1;
        END IF;
    END LOOP;
    RETURN temp;
END
$$
IMMUTABLE STRICT LANGUAGE PLPGSQL;

CREATE OR REPLACE FUNCTION mysql.date_format(text, text)
RETURNS text
AS $$SELECT mysql.date_format($1::timestamp without time zone, $2)$$
LANGUAGE SQL IMMUTABLE STRICT;

-- PG16 UDB-TX system-variable catalogue.  The backend system-variable
-- implementation reads this relation during MySQL parser initialization.
create table mys_informa_schema.base_variables(
    variable_name varchar(128),
    def_value varchar(1024),
    conf_value varchar(1024),
    sess_global_type int,
    -- sess_global_type 0: global & session, 1:global only, 2:session only, 3:session only but can select global
    sess_def_val_from_global bool,
    is_read_write bool,
    valid_result varchar(256),
    select_result_rule varchar(256),
    show_result_rule varchar(256),
    primary key(variable_name)
);
insert into mys_informa_schema.base_variables values('autocommit', '1', '1', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('auto_increment_increment', '1', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('auto_increment_offset', '1', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('big_tables', '0', '0', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('character_set_client', 'utf8mb4', 'utf8mb4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('character_set_connection', 'utf8mb4', 'utf8mb4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('character_set_results', 'utf8mb4', 'utf8mb4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('character_set_server', 'utf8mb4', 'utf8mb4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('character_set_database', 'utf8mb4', 'utf8mb4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('collation_connection', 'utf8mb4_general_ci', 'utf8mb4_general_ci', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('collation_server', 'utf8mb4_general_ci', 'utf8mb4_general_ci', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('default_storage_engine', 'InnoDB', 'InnoDB', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('enforce_gtid_consistency', '0', '0', 1, true, true, '0|1', 'OFF|ON', 'OFF|ON');
insert into mys_informa_schema.base_variables values('explicit_defaults_for_timestamp', '0', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('foreign_key_checks', '1', '1', 0, false, true, null, null, null);
insert into mys_informa_schema.base_variables values('ft_max_word_len', '84', '84', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('have_profiling', 'YES', 'YES', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('init_connect', '', '', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_chunk_size', '134217728', '134217728', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_dump_at_shutdown', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_dump_now', '0', '0', 1, false, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_dump_pct', '25', '25', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_filename', 'ib_buffer_pool', 'ib_buffer_pool', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_instances', '8', '8', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_load_abort', '0', '0', 1, false, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_load_at_startup', '1', '1', 1, true, false, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_load_now', '0', '0', 1, false, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('innodb_buffer_pool_size', '1073741824', '1073741824', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_checksums', '1', '1', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_flush_log_at_trx_commit', '1', '2', 1, true, true, '0|1|2', '0|1|2', '0|1|2');
insert into mys_informa_schema.base_variables values('innodb_log_buffer_size', '2097152', '2097152', 1, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_log_file_size', '33554432', '33554432', 1, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_log_files_in_group', '3', '3', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_sort_buffer_size', '1048576', '1048576', 1, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_strict_mode', '1', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_thread_concurrency', '0', '0', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_thread_sleep_delay', '10000', '10000', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('interactive_timeout', '28800', '20000', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('join_buffer_size', '262144', '8388608', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('last_insert_id', '0', '0', 2, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('license', 'GPL', 'GPL', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('long_query_time', '10', '10', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('lower_case_file_system', '0', '0', 0, true, false, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('lower_case_table_names', '1', '1', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('max_allowed_packet', '4194304', '536870912', 1, false, true, null, null, null);
insert into mys_informa_schema.base_variables values('max_connections', '151', '100', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('max_length_for_sort_data', '1024', '1024', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('max_seeks_for_key', '18446744073709551615', '18446744073709551615', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('max_sp_recursion_depth', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('max_user_connections', '0', '0', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('myisam_sort_buffer_size', '8388608', '8388608', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('myisam_stats_method', '0', '0', 0, true, true, '0|1|2', 'nulls_unequal|nulls_equal|nulls_ignored', 'nulls_unequal|nulls_equal|nulls_ignored');
insert into mys_informa_schema.base_variables values('net_buffer_length', '16384', '16384', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('net_write_timeout', '60', '60', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('optimizer_prune_level', '1', '1', 0, true, true, '0|1', null, '0|1');
insert into mys_informa_schema.base_variables values('optimizer_switch', 'index_merge=on,index_merge_union=on,index_merge_sort_union=on,index_merge_intersection=on,engine_condition_pushdown=on,index_condition_pushdown=on,mrr=on,mrr_cost_based=on,block_nested_loop=on,batched_key_access=off,materialization=on,semijoin=on,loosescan=on,firstmatch=on,duplicateweedout=on,subquery_materialization_cost_based=on,use_index_extensions=on,condition_fanout_filter=on,derived_merge=on', 'index_merge=on,index_merge_union=on,index_merge_sort_union=on,index_merge_intersection=on,engine_condition_pushdown=on,index_condition_pushdown=on,mrr=on,mrr_cost_based=on,block_nested_loop=on,batched_key_access=off,materialization=on,semijoin=on,loosescan=on,firstmatch=on,duplicateweedout=on,subquery_materialization_cost_based=on,use_index_extensions=on,condition_fanout_filter=on,derived_merge=on', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('parser_max_mem_size', '18446744073709551615', '18446744073709551615', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('performance_schema', '0', '0', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('profiling_history_size', '15', '15', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('profiling', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('query_cache_limit', '1048576', '1048576', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('query_cache_min_res_unit', '4096', '4096', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('query_cache_size', '1048576', '1048576', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('query_cache_type', '0', '1', 0, true, true, '0|1|2', 'OFF|ON|DEMAND', 'OFF|ON|DEMAND');
insert into mys_informa_schema.base_variables values('query_cache_wlock_invalidate', '0', '1', 0, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('range_alloc_block_size', '4096', '4096', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('range_optimizer_max_mem_size', '8388608', '8388608', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('rbr_exec_mode', '0', '0', 3, true, true, '0|1', 'STRICT|IDEMPOTENT', 'STRICT|IDEMPOTENT');
insert into mys_informa_schema.base_variables values('read_buffer_size', '131072', '2097152', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('require_secure_transport', '0', '0', 1, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('secure_file_priv', '', '', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('session_track_gtids', '0', '0', 0, true, true, '0|1|2', 'OFF|OWN_GTID|ALL_GTIDS', 'OFF|OWN_GTID|ALL_GTIDS');
insert into mys_informa_schema.base_variables values('session_track_schema', '1', '1', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('session_track_state_change', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('session_track_system_variables', 'time_zone,autocommit,character_set_client,character_set_results,character_set_connection', 'time_zone,autocommit,character_set_client,character_set_results,character_set_connection', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('session_track_transaction_info', '0', '0', 0, true, true, '0|1', 'OFF|ON', 'OFF|ON');
insert into mys_informa_schema.base_variables values('show_create_table_verbosity', '0', '0', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('sort_buffer_size', '262144', '8388608', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sql_auto_is_null', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sql_mode', 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION', 'STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_AUTO_CREATE_USER,NO_ENGINE_SUBSTITUTION', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sql_notes', '1', '1', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_quote_show_create', '1', '1', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_safe_updates', '0', '0', 0, true, true, '0|1', null, 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_select_limit', '18446744073709551615', '18446744073709551615', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('system_time_zone', 'CST', 'CST', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('thread_cache_size', '9', '8', 1, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('time_zone', 'SYSTEM', 'SYSTEM', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('transaction_isolation', 'REPEATABLE-READ', 'REPEATABLE-READ', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('transaction_read_only', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('tx_isolation', 'REPEATABLE-READ', 'REPEATABLE-READ', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('tx_read_only', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('unique_checks', 'on', 'on', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('version_comment', 'MySQL Server (GPL)', 'MySQL Server (GPL)', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('wait_timeout', '28800', '20000', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_default_row_format', '2', '2', 1, true, true, '0|1|2|3', 'redundant|compact|dynamic|compressed', 'redundant|compact|dynamic|compressed');
insert into mys_informa_schema.base_variables values('innodb_file_format', '1', '1', 1, true, true, '0|1', 'Antelope|Barracuda', 'Antelope|Barracuda');
insert into mys_informa_schema.base_variables values('innodb_file_format_check', '1', '1', 0, true, false, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('innodb_file_format_max', '0', '1', 1, true, true, '0|1', 'Antelope|Barracuda', 'Antelope|Barracuda');
insert into mys_informa_schema.base_variables values('innodb_file_per_table', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('collation_database', 'utf8mb4_general_ci', 'utf8mb4_general_ci', 0, true, true, null, null, null);
INSERT INTO mys_informa_schema.base_variables VALUES ('datadir', '/data/unvdb/', '/data/unvdb/', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('unvdb_mysql_dummy_stmt_return_ok', '1', '1', 0, true, true, '0|1', '0|1', '0|1');
insert into mys_informa_schema.base_variables values('have_query_cache', '1', '1', 1, true, false, '0|1', 'NO|YES', 'NO|YES');
insert into mys_informa_schema.base_variables values('automatic_sp_privileges', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('check_proxy_users', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('completion_type', '0', '0', 1, true, true, '0|1|2', 'NO_CHAIN|CHAIN|RELEASE', 'NO_CHAIN|CHAIN|RELEASE');
insert into mys_informa_schema.base_variables values('concurrent_insert', '1', '1', 1, true, true, '0|1|2', 'NEVER|AUTO|ALWAYS', 'NEVER|AUTO|ALWAYS');
insert into mys_informa_schema.base_variables values('connect_timeout', '10', '10', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('core_file', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('default_authentication_plugin', '0', '0', 1, true, true, '0|1', 'mysql_native_password|sha256_password', 'mysql_native_password|sha256_password');
insert into mys_informa_schema.base_variables values('default_password_lifetime', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('default_tmp_storage_engine', '0', '0', 1, true, true, '0|1|2|3|4|5|6|7', 'InnoDB|MRG_MYISAM|MyISAM|BLACKHOLE|CSV|mem|ARCHIVE|FEDERATED', 'InnoDB|MRG_MYISAM|MyISAM|BLACKHOLE|CSV|mem|ARCHIVE|FEDERATED');
insert into mys_informa_schema.base_variables values('default_week_format', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('disabled_storage_engines', '0', '0', 1, true, true, '0|1', '0|1', ' | ');
insert into mys_informa_schema.base_variables values('disconnect_on_expired_password', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('block_encryption_mode', '0', '0', 1, true, true, '0|1|2|3|4|5', 'aes-128-ecb|aes-192-ecb|aes-256-ecb|aes-128-cbc|aes-192-cbc|aes-256-cbc', 'aes-128-ecb|aes-192-ecb|aes-256-ecb|aes-128-cbc|aes-192-cbc|aes-256-cbc');
insert into mys_informa_schema.base_variables values('bulk_insert_buffer_size', '8388608', '8388608', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('binlog_cache_size', '1048576', '1048576', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('binlog_checksum', '1', '1', 1, true, true, '0|1', 'NONE|CRC32', 'NONE|CRC32');
insert into mys_informa_schema.base_variables values('binlog_error_action', '1', '1', 1, true, true, '0|1', 'IGNORE_ERROR|ABORT_SERVER', 'IGNORE_ERROR|ABORT_SERVER');
insert into mys_informa_schema.base_variables values('binlog_format', '1', '1', 1, true, true, '0|1|2', 'STATEMENT|ROW|MIXED', 'STATEMENT|ROW|MIXED');
insert into mys_informa_schema.base_variables values('binlog_group_commit_sync_delay', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('binlog_group_commit_sync_no_delay_count', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('binlog_gtid_simple_recovery', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('binlog_order_commits', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('binlog_row_image', '0', '0', 1, true, true, '0|1|2', 'FULL|MINIMAL|NOBLOB', 'FULL|MINIMAL|NOBLOB');
insert into mys_informa_schema.base_variables values('binlog_rows_query_log_events', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('binlog_stmt_cache_size', '32768', '32768', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('character_set_filesystem', '0', '0', 1, true, true, '0|1|2', 'binary|utf8|ascii', 'binary|utf8|ascii');
insert into mys_informa_schema.base_variables values('character_set_system', '1', '1', 1, true, true, '0|1|2', 'binary|utf8|ascii', 'binary|utf8|ascii');
INSERT INTO mys_informa_schema.base_variables VALUES('character_sets_dir', '/data/unvdb/', '/data/unvdb/', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('delay_key_write', '0', '0', 1, true, true, '0|1|2', 'ON|OFF|ALL', 'ON|OFF|ALL');
insert into mys_informa_schema.base_variables values('div_precision_increment', '4', '4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('end_markers_in_json', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('eq_range_index_dive_limit', '200', '200', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('event_scheduler', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('expire_logs_days', '1', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('flush', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('flush_time', '0', '0', 0, true, true, null, null, null);
INSERT INTO mys_informa_schema.base_variables VALUES('ft_boolean_syntax', '+ -&gt;&lt;()~*:""&amp;|', '+ -&gt;&lt;()~*:""&amp;|', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('ft_min_word_len', '4', '4', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('ft_query_expansion_limit', '20', '20', 0, true, true, null, null, null);
INSERT INTO mys_informa_schema.base_variables VALUES('ft_stopword_file', '(built-in)', '(built-in)', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('general_log', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
INSERT INTO mys_informa_schema.base_variables VALUES('general_log_file', '/var/lib/mysql/data/hostname.log', '/var/lib/mysql/data/hostname.log', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('group_concat_max_len', '1024', '1024', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('gtid_executed_compression_period', '1000', '1000', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('gtid_mode', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
INSERT INTO mys_informa_schema.base_variables VALUES('gtid_next', 'AUTOMATIC', 'AUTOMATIC', 0, true, false, null, null, null);
INSERT INTO mys_informa_schema.base_variables VALUES('gtid_owned', '', '', 0, true, false, null, null, null);
INSERT INTO mys_informa_schema.base_variables VALUES('gtid_purged', '', '', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('host_cache_size', '228', '228', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('internal_tmp_disk_storage_engine', '0', '0', 1, true, true, '0|1', 'INNODB|MYISAM', 'INNODB|MYISAM');
insert into mys_informa_schema.base_variables values('keep_files_on_create', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('key_buffer_size', '4194304', '4194304', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('updatable_views_with_limit', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|YES');
insert into mys_informa_schema.base_variables values('thread_handling', '0', '0', 1, true, true, '0|1|2', 'one-thread-per-connection|one-thread-for-all-connections|pool-of-threads', 'one-thread-per-connection|one-thread-for-all-connections|pool-of-threads');
insert into mys_informa_schema.base_variables values('thread_stack', '262144', '262144', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('tmp_table_size', '16777216', '16777216', 0, true, true, null, null, null);
INSERT INTO mys_informa_schema.base_variables VALUES('tmpdir', '/tmp', '/tmp', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('transaction_alloc_block_size', '8192', '8192', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('transaction_prealloc_size', '4096', '4096', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('transaction_write_set_extraction', '0', '0', 1, true, true, '0|1|2', 'OFF|MURMUR32|XXHASH6', 'OFF|MURMUR32|XXHASH6');
insert into mys_informa_schema.base_variables values('table_definition_cache', '464', '464', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('table_open_cache', '128', '128', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('table_open_cache_instances', '16', '16', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sync_binlog', '1', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sync_master_info', '10000', '10000', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sync_relay_log', '10000', '10000', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sync_relay_log_info', '10000', '10000', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sync_frm', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('super_read_only', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('stored_program_cache', '256', '256', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sql_big_selects', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_buffer_result', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_log_bin', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_log_off', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('sql_slave_skip_counter', '0', '0', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('sql_warnings', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
INSERT INTO mys_informa_schema.base_variables VALUES('socket', '/tmp/mysql.sock', '/tmp/mysql.sock', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('slow_launch_time', '2', '2', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('slow_query_log', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
INSERT INTO mys_informa_schema.base_variables VALUES('slow_query_log_file', '/var/lib/mysql/data/slow.log', '/var/lib/mysql/data/slow.log', 0, true, false, null, null, null);
insert into mys_informa_schema.base_variables values('skip_external_locking', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('skip_name_resolve', '1', '1', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('skip_networking', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('skip_show_database', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('show_compatibility_56', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('sha256_password_proxy_users', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('server_id', '1', '1', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('server_id_bits', '32', '32', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('read_only', '0', '0', 1, true, true, '0|1', '0|1', 'OFF|ON');
insert into mys_informa_schema.base_variables values('read_rnd_buffer_size', '8388608', '8388608', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('query_alloc_block_size', '8192', '8192', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('query_prealloc_size', '8192', '8192', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('preload_buffer_size', '32768', '32768', 0, true, true, null, null, null);
insert into mys_informa_schema.base_variables values('innodb_lock_wait_timeout', '50', '50', 0, true, true, null, null, null);
