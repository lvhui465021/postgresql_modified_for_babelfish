/* aux_mysql 1.5 -> 1.6
 *
 * The MySQL JSON functions (mysql.json for int2/int4/int8/float4/float8/
 * numeric and mysql.mys_json_object) moved out of the kernel pg_proc.dat
 * into this extension, backed by the mysm shared library.  Instances
 * upgraded from 1.5 previously resolved these from the kernel catalog;
 * redeclare them here so the extension owns them.
 */
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

CREATE OR REPLACE FUNCTION mysql.to_base64(text)
RETURNS text
AS $$SELECT pg_catalog.encode(pg_catalog.convert_to($1, 'UTF8'), 'base64')$$
LANGUAGE SQL IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.load_file(text)
RETURNS text
AS $$
BEGIN
    RETURN pg_catalog.pg_read_file($1);
EXCEPTION WHEN OTHERS THEN
    RETURN NULL;
END;
$$
STABLE STRICT LANGUAGE plpgsql;

CREATE OR REPLACE VIEW mys_informa_schema.db_status AS
SELECT name::varchar(256) AS variable_name,
       setting::text AS value
FROM pg_catalog.pg_settings;
GRANT ALL PRIVILEGES ON mys_informa_schema.db_status TO public;

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

CREATE OR REPLACE FUNCTION mysql.get_lock(text, int)
RETURNS int
STRICT VOLATILE LANGUAGE plpgsql
AS $function$
DECLARE
    lock_key bigint := pg_catalog.hashtextextended($1, 0);
    deadline timestamptz;
BEGIN
    IF $2 < 0 THEN
        RETURN NULL;
    ELSIF pg_catalog.pg_try_advisory_lock(lock_key) THEN
        RETURN 1;
    ELSIF $2 = 0 THEN
        RETURN 0;
    END IF;
    deadline := clock_timestamp() + make_interval(secs => $2);
    WHILE clock_timestamp() < deadline LOOP
        PERFORM pg_catalog.pg_sleep(LEAST(
            0.1,
            GREATEST(0.0, EXTRACT(EPOCH FROM deadline - clock_timestamp()))));
        IF pg_catalog.pg_try_advisory_lock(lock_key) THEN
            RETURN 1;
        END IF;
    END LOOP;
    RETURN 0;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.release_lock(text)
RETURNS int
STRICT VOLATILE LANGUAGE plpgsql
AS $function$
BEGIN
    RETURN CASE WHEN pg_catalog.pg_advisory_unlock(
        pg_catalog.hashtextextended($1, 0)) THEN 1 ELSE 0 END;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.is_free_lock(text)
RETURNS int
STRICT VOLATILE LANGUAGE plpgsql
AS $function$
DECLARE
    lock_key bigint := pg_catalog.hashtextextended($1, 0);
BEGIN
    IF pg_catalog.pg_advisory_unlock(lock_key) THEN
        PERFORM pg_catalog.pg_advisory_lock(lock_key);
        RETURN 0;
    ELSIF pg_catalog.pg_try_advisory_lock(lock_key) THEN
        PERFORM pg_catalog.pg_advisory_unlock(lock_key);
        RETURN 1;
    ELSE
        RETURN 0;
    END IF;
END;
$function$;

CREATE OR REPLACE FUNCTION mysql.is_used_lock(text)
RETURNS int
STRICT VOLATILE LANGUAGE plpgsql
AS $function$
DECLARE
    lock_key bigint := pg_catalog.hashtextextended($1, 0);
BEGIN
    IF pg_catalog.pg_advisory_unlock(lock_key) THEN
        PERFORM pg_catalog.pg_advisory_lock(lock_key);
        RETURN pg_catalog.pg_backend_pid();
    ELSIF pg_catalog.pg_try_advisory_lock(lock_key) THEN
        PERFORM pg_catalog.pg_advisory_unlock(lock_key);
        RETURN NULL;
    ELSE
        RETURN 0;
    END IF;
END;
$function$;

/*
 * SHOW WARNINGS support: report the previous statement's suppressed
 * NOTICE/WARNING/INFO diagnostics (Level / Code / Message), mirroring
 * MySQL's SHOW WARNINGS result shape.  Backed by mysql_show_warnings()
 * in the aux_mysql shared library, which reads the session diagnostics
 * area retained by mysql_send_error().
 */
CREATE OR REPLACE FUNCTION mysql.show_warnings()
RETURNS TABLE(Level pg_catalog.text, Code pg_catalog.int4, Message pg_catalog.text)
AS '$libdir/aux_mysql', 'mysql_show_warnings'
LANGUAGE C STABLE;
