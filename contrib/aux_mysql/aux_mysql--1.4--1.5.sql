CREATE OR REPLACE FUNCTION mysql.uuid()
RETURNS text
AS
$$
BEGIN
    return gen_random_uuid();
END;
$$
LANGUAGE plpgsql;


create or replace function mysql.setval(regclass, bigint, boolean)
returns pg_catalog.int8
as 'MODULE_PATHNAME', 'mysSetval3Oid'
language C
STRICT;

CREATE OR REPLACE FUNCTION mysql.to_base64(text)
RETURNS text
AS $$SELECT pg_catalog.encode(pg_catalog.convert_to($1, 'UTF8'), 'base64')$$
LANGUAGE SQL IMMUTABLE STRICT;

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
