-- MySQL-protocol compatibility suite: session, database and variable semantics.
-- Prerequisite: connect through the MySQL listener as a superuser to a database
-- that has aux_mysql installed (normally a database cloned from template1).

SELECT 'extension_installed' AS test_name,
       COUNT(*) = 1 AND COALESCE(MAX(extversion), 'missing') <> 'missing' AS passed
FROM pg_extension WHERE extname = 'aux_mysql';
SELECT 'protocol_listener_on' AS test_name,
       current_setting('mysql_listener_on') = 'on'
       AND current_setting('mysql_port') IS NOT NULL
       AND current_setting('mysql_max_allowed_packet') IS NOT NULL
       AS passed;
SELECT 'packet_and_charset_variables' AS test_name,
       @@max_allowed_packet > 0 AND @@character_set_client = 'utf8mb4'
       AND @@collation_connection = 'utf8mb4_general_ci' AS passed;

DROP DATABASE IF EXISTS mysql_compat_session;
CREATE DATABASE mysql_compat_session;
SHOW DATABASES;
USE mysql_compat_session;

SELECT 'database' AS test_name, DATABASE() = 'mysql_compat_session' AS passed;
SELECT 'schema_database_alias' AS test_name, SCHEMA() = DATABASE() AS passed;
SELECT 'session_identity' AS test_name,
       USER() IS NOT NULL AND CURRENT_USER() IS NOT NULL
       AND SESSION_USER() IS NOT NULL AS passed;
SELECT 'version' AS test_name, mysql.version() IS NOT NULL AS passed;
SELECT 'boolean_display' AS test_name, true = 1 AND false = 0 AS passed;
SELECT 'null_safe_equal' AS test_name,
       1 <=> 1 AND NULL <=> NULL AND NOT (1 <=> NULL) AS passed;

SET @mysql_compat_number := 42, @mysql_compat_text := 'session value';
SELECT 'user_variables' AS test_name,
       @mysql_compat_number = 42 AND @mysql_compat_text = 'session value' AS passed;
SELECT @mysql_compat_number := @mysql_compat_number + 1;
SELECT 'user_variable_expression' AS test_name,
       @mysql_compat_number = 43 AS passed;

SET sql_mode = DEFAULT;
SELECT 'system_variable_bare' AS test_name, @@sql_mode IS NOT NULL AS passed;
SET SESSION sql_mode = @@session.sql_mode;
SELECT 'system_variable_session' AS test_name,
       @@session.sql_mode = @@sql_mode AS passed;
SELECT 'system_variable_global' AS test_name,
       @@global.sql_mode IS NOT NULL AS passed;
SET NAMES utf8mb4;
SELECT 'set_names' AS test_name, @@character_set_client IS NOT NULL AS passed;

CREATE TABLE mysql_session_tx (id INT PRIMARY KEY);
BEGIN;
INSERT INTO mysql_session_tx VALUES (1);
ROLLBACK;
SELECT 'transaction_rollback' AS test_name,
       COUNT(*) = 0 AS passed FROM mysql_session_tx;
BEGIN;
INSERT INTO mysql_session_tx VALUES (2);
COMMIT;
SELECT 'transaction_commit' AS test_name,
       COUNT(*) = 1 AS passed FROM mysql_session_tx WHERE id = 2;

DROP DATABASE mysql_compat_session;
SELECT 'database_cleanup' AS test_name,
       COUNT(*) = 0 AS passed FROM pg_database WHERE datname = 'mysql_compat_session';
