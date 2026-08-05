-- MySQL-protocol compatibility suite: current aux_mysql inventory.
-- Runtime-based checks (PG18 baseline, not hardcoded UDB-TX numbers).
-- The required API manifest contains user-visible function names;
-- internal cast/operator helpers are covered by the signature counts.

DROP TEMPORARY TABLE IF EXISTS mysql_compat_required_api;
CREATE TEMPORARY TABLE mysql_compat_required_api (
  function_name VARCHAR(64) PRIMARY KEY
);

INSERT INTO mysql_compat_required_api(function_name) VALUES
  ('adddate'), ('addtime'), ('bin'), ('bit_count'),
  ('concat'), ('concat_ws'), ('conv'), ('convert_tz'),
  ('curdate'), ('current_user'), ('curtime'),
  ('database'), ('date'), ('date_add'), ('date_format'), ('date_sub'),
  ('datediff'), ('day'), ('dayname'), ('dayofmonth'), ('dayofweek'),
  ('dayofyear'), ('elt'), ('export_set'), ('field'),
  ('find_in_set'), ('floor'), ('format'), ('found_rows'), ('from_days'),
  ('from_unixtime'), ('get_lock'), ('hour'), ('if'), ('ifnull'), ('insert'),
  ('instr'), ('is_free_lock'), ('isnull'), ('json_unquote'),
  ('last_day'), ('last_insert_id'), ('lcase'), ('left'), ('length'),
  ('locate'), ('log'), ('log2'), ('lpad'), ('make_set'),
  ('makedate'), ('maketime'), ('microsecond'), ('mid'), ('minute'), ('mod'),
  ('month'), ('monthname'), ('now'), ('period_add'),
  ('period_diff'), ('quarter'), ('rand'), ('release_lock'), ('right'),
  ('round'), ('row_count'), ('rpad'), ('sec_to_time'), ('second'),
  ('session_user'), ('sleep'), ('space'), ('sqrt'), ('str_to_date'),
  ('strcmp'), ('subdate'), ('substr'), ('substring'), ('substring_index'),
  ('subtime'), ('sysdate'), ('time_format'), ('time_to_sec'),
  ('timediff'), ('timestamp'), ('timestampadd'), ('timestampdiff'),
  ('truncate'), ('ucase'), ('unix_timestamp'), ('utc_date'),
  ('utc_time'), ('utc_timestamp'), ('uuid'), ('uuid_short'),
  ('version'), ('week'), ('weekday'), ('weekofyear'), ('year'), ('yearweek');

-- Extension is installed with a valid version
SELECT 'extension_version' AS test_name,
       extversion IS NOT NULL AND extversion::numeric >= 1.0 AS passed
FROM pg_extension WHERE extname = 'aux_mysql';

-- mysql schema has a significant number of functions (>100)
SELECT 'mysql_function_catalog_baseline' AS test_name,
       COUNT(DISTINCT p.proname) > 100
       AND COUNT(*) > 100
       AND SUM(p.prokind = 'f') > 100 AS passed
FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
WHERE n.nspname = 'mysql';

-- Required public API functions must all exist
SELECT 'required_public_api_present' AS test_name,
       COUNT(*) = 0 AS passed
FROM mysql_compat_required_api r
WHERE NOT EXISTS (
  SELECT 1
  FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
  WHERE n.nspname = 'mysql'
    AND CAST(p.proname AS BINARY) = CAST(r.function_name AS BINARY)
);

-- mys_informa_schema relations exist
SELECT 'compatibility_schema_relation_baseline' AS test_name,
       COUNT(*) >= 3 AS passed
FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE n.nspname = 'mys_informa_schema'
  AND c.relkind IN ('r', 'v', 'm');

DROP TEMPORARY TABLE mysql_compat_required_api;
