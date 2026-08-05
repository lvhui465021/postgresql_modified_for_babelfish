-- MySQL-protocol compatibility suite: CTAS propagation of ON UPDATE timestamps.

DROP DATABASE IF EXISTS mysql_compat_ctas;
CREATE DATABASE mysql_compat_ctas;
USE mysql_compat_ctas;

CREATE TABLE mysql_ctas_source (
  id INT,
  ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  value_int INT
);
INSERT INTO mysql_ctas_source VALUES (1, CURRENT_TIMESTAMP, 10);

CREATE TABLE mysql_ctas_direct AS
  SELECT ts AS renamed_ts, value_int FROM mysql_ctas_source;
SELECT 'ctas_direct_trigger' AS test_name, COUNT(*) > 0 AS passed
FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
WHERE c.relname = 'mysql_ctas_direct' AND t.tgname LIKE 'aut%';
UPDATE mysql_ctas_direct SET renamed_ts = '2000-01-01 00:00:00', value_int = value_int + 1;
UPDATE mysql_ctas_direct SET value_int = value_int + 1;
SELECT 'ctas_direct_update' AS test_name,
       renamed_ts > '2000-01-01 00:00:00' AS passed FROM mysql_ctas_direct;

CREATE TABLE mysql_ctas_unchanged AS
  SELECT ts AS renamed_ts, value_int FROM mysql_ctas_source;
UPDATE mysql_ctas_unchanged SET renamed_ts = '2000-01-01 00:00:00';
UPDATE mysql_ctas_unchanged SET value_int = value_int;
SELECT 'ctas_unchanged_row' AS test_name,
       renamed_ts = '2000-01-01 00:00:00' AS passed FROM mysql_ctas_unchanged;

CREATE TABLE mysql_ctas_expression AS
  SELECT ts + INTERVAL 1 SECOND AS derived_ts FROM mysql_ctas_source;
SELECT 'ctas_expression_no_trigger' AS test_name, COUNT(*) = 0 AS passed
FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
WHERE c.relname = 'mysql_ctas_expression' AND t.tgname LIKE 'aut%';

CREATE TABLE mysql_ctas_other (id INT, ts TIMESTAMP);
INSERT INTO mysql_ctas_other VALUES (1, CURRENT_TIMESTAMP);
CREATE TABLE mysql_ctas_join AS
  SELECT o.ts FROM mysql_ctas_source s
  JOIN mysql_ctas_other o ON o.id = s.id;
SELECT 'ctas_join_no_wrong_trigger' AS test_name, COUNT(*) = 0 AS passed
FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
WHERE c.relname = 'mysql_ctas_join' AND t.tgname LIKE 'aut%';

DROP DATABASE mysql_compat_ctas;
