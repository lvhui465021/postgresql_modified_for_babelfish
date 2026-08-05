-- MySQL-protocol compatibility suite: SHOW/DESCRIBE, metadata schemas and locks.

DROP DATABASE IF EXISTS mysql_compat_metadata;
CREATE DATABASE mysql_compat_metadata;
USE mysql_compat_metadata;

CREATE TABLE mysql_metadata_table (
  id INT AUTO_INCREMENT PRIMARY KEY,
  code VARCHAR(20) NOT NULL UNIQUE,
  value_int INT DEFAULT 0 COMMENT 'metadata column'
) COMMENT='metadata table';
CREATE INDEX mysql_metadata_value_index ON mysql_metadata_table (value_int);
CREATE ALGORITHM=MERGE SQL SECURITY INVOKER VIEW mysql_metadata_view AS
  SELECT id, code, value_int FROM mysql_metadata_table;
CREATE FUNCTION mysql_metadata_function() RETURNS INT RETURN 1;
CREATE PROCEDURE mysql_metadata_procedure() BEGIN ATOMIC END;
INSERT INTO mysql_metadata_table (code, value_int) VALUES ('one', 1);
SELECT 'routine_invocation' AS test_name,
       mysql_metadata_function() = 1 AS passed;
CALL mysql_metadata_procedure();

SELECT 'view_modifiers' AS test_name,
       COUNT(*) = (SELECT COUNT(*) FROM mysql_metadata_table) AS passed
FROM mysql_metadata_view;
SHOW TABLES;
SHOW FULL TABLES;
SHOW TABLES LIKE 'mysql_metadata_%';
SHOW FULL TABLES LIKE 'mysql_metadata_%';
SHOW COLUMNS FROM mysql_metadata_table;
SHOW FULL COLUMNS FROM mysql_metadata_table;
SHOW FIELDS FROM mysql_metadata_table;
SHOW FIELDS FROM mysql_metadata_table LIKE 'id';
DESC mysql_metadata_table;
DESC mysql_metadata_table code;
DESCRIBE mysql_metadata_table;
SHOW INDEX FROM mysql_metadata_table;
SHOW INDEXES FROM mysql_metadata_table;
SHOW KEYS FROM mysql_metadata_table;
SHOW CREATE TABLE mysql_metadata_table;
SHOW CREATE VIEW mysql_metadata_view;
SHOW CREATE FUNCTION mysql.version;
SHOW CREATE FUNCTION mysql_metadata_function;
SHOW CREATE PROCEDURE mysql_metadata_procedure;
SHOW CREATE TRIGGER aitmysql_metadata_table;
SHOW DATABASES;
SHOW DATABASES LIKE 'mysql_compat_%';
SHOW CHARACTER SET;
SHOW COLLATION;
SHOW ENGINES;
SHOW STORAGE ENGINES;
SHOW PLUGINS;
SHOW SESSION VARIABLES LIKE 'sql_mode';
SHOW GLOBAL VARIABLES LIKE 'sql_mode';
SHOW STATUS LIKE 'server_version';
SHOW SESSION STATUS LIKE 'server_version';
SHOW GLOBAL STATUS LIKE 'server_version';
SHOW PROCESSLIST;
SHOW FULL PROCESSLIST;
SHOW TRIGGERS;

SELECT 'metadata_tables' AS test_name,
       COUNT(*) = 1 AS passed
FROM mys_informa_schema.tables
WHERE table_schema = 'mysql_compat_metadata'
  AND table_name IN ('mysql_metadata_table', 'mysql_metadata_view');
SELECT 'information_schema_columns' AS test_name,
       COUNT(*) = 3 AS passed
FROM information_schema.columns
WHERE table_schema = 'mysql_compat_metadata' AND table_name = 'mysql_metadata_table';
SELECT 'metadata_indexes' AS test_name,
       COUNT(*) = 3 AS passed
FROM mys_informa_schema.indexs
WHERE schema_name = 'mysql_compat_metadata'
  AND table_name = 'mysql_metadata_table';
SELECT 'information_schema_statistics' AS test_name,
       COUNT(*) >= 2 AS passed
FROM information_schema.statistics
WHERE table_schema = 'mysql_compat_metadata'
  AND table_name = 'mysql_metadata_table';
SELECT 'metadata_view' AS test_name,
       COUNT(*) = 1 AS passed
FROM mys_informa_schema.views
WHERE table_schema = 'mysql_compat_metadata'
  AND table_name = 'mysql_metadata_view';
SELECT 'metadata_routines' AS test_name,
       COUNT(*) = 2 AS passed
FROM mys_informa_schema.routines
WHERE routine_schema = 'mysql_compat_metadata'
  AND routine_name IN ('mysql_metadata_function', 'mysql_metadata_procedure');
SELECT 'performance_schema_queryable' AS test_name,
       COUNT(*) >= 0 AS passed FROM performance_schema.accounts;
SELECT 'sys_schema_queryable' AS test_name,
       COUNT(*) >= 0 AS passed FROM sys.innodb_lock_waits;

LOCK TABLES mysql_metadata_table LOW_PRIORITY WRITE, mysql_metadata_view READ;
UNLOCK TABLES;

CHECKSUM TABLE mysql_metadata_table;
ANALYZE TABLE mysql_metadata_table;
CHECK TABLE mysql_metadata_table;
OPTIMIZE TABLE mysql_metadata_table;
REPAIR TABLE mysql_metadata_table;
FLUSH TABLES;
RESET QUERY CACHE;

SELECT 'admin_commands_leave_table_usable' AS test_name,
       COUNT(*) = 1 AS passed FROM mysql_metadata_table;

DROP DATABASE mysql_compat_metadata;
