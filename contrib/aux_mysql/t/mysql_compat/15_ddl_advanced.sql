-- MySQL-protocol compatibility suite: advanced DDL forms and object lifecycle.

DROP DATABASE IF EXISTS mysql_compat_ddl_advanced;
CREATE DATABASE mysql_compat_ddl_advanced;
USE mysql_compat_ddl_advanced;

CREATE TEMPORARY TABLE mysql_adv_temp (
  id INT AUTO_INCREMENT PRIMARY KEY,
  value_int INT
);
INSERT INTO mysql_adv_temp(value_int) VALUES (1), (2);
TRUNCATE TABLE mysql_adv_temp;
INSERT INTO mysql_adv_temp(value_int) VALUES (3);
SELECT 'temporary_table_truncate_restart' AS test_name,
       COUNT(*) = 1 AND MIN(id) = 1 AND MIN(value_int) = 3 AS passed
FROM mysql_adv_temp;

CREATE TABLE mysql_adv_base (
  id INT AUTO_INCREMENT PRIMARY KEY,
  code VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL,
  value_int INT NOT NULL DEFAULT 2,
  doubled INT GENERATED ALWAYS AS (value_int * 2) STORED,
  CONSTRAINT mysql_adv_code_unique UNIQUE KEY (code),
  CONSTRAINT mysql_adv_value_check CHECK (value_int >= 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO mysql_adv_base(code) VALUES ('AbC');
SELECT 'generated_check_default_collation' AS test_name,
       id = 1 AND value_int = 2 AND doubled = 4 AND code = 'abc' AS passed
FROM mysql_adv_base;

CREATE TABLE mysql_adv_like LIKE mysql_adv_base;
SELECT 'create_table_like' AS test_name,
       COUNT(*) = 4 AS passed
FROM mys_informa_schema.columns
WHERE table_schema = 'mysql_compat_ddl_advanced'
  AND table_name = 'mysql_adv_like';

ALTER TABLE mysql_adv_base ADD COLUMN note VARCHAR(40) DEFAULT 'new';
ALTER TABLE mysql_adv_base MODIFY COLUMN note VARCHAR(80) DEFAULT 'modified';
ALTER TABLE mysql_adv_base CHANGE COLUMN note renamed_note VARCHAR(80) DEFAULT 'changed';
ALTER TABLE mysql_adv_base RENAME COLUMN renamed_note TO note;
SELECT 'alter_column_lifecycle' AS test_name,
       note = 'new'
       AND (SELECT column_default = '''changed'''
            FROM mys_informa_schema.columns
            WHERE table_schema = 'mysql_compat_ddl_advanced'
              AND table_name = 'mysql_adv_base' AND column_name = 'note') AS passed
FROM mysql_adv_base;

CREATE INDEX mysql_adv_index_old ON mysql_adv_base(value_int);
ALTER TABLE mysql_adv_base RENAME INDEX mysql_adv_index_old TO mysql_adv_index_new;
SELECT 'rename_index' AS test_name,
       COUNT(*) = 1 AS passed
FROM mys_informa_schema.indexs
WHERE schema_name = 'mysql_compat_ddl_advanced'
  AND table_name = 'mysql_adv_base'
  AND key_name LIKE '%mysql_adv_index_new';
DROP INDEX mysql_adv_index_new ON mysql_adv_base;
SELECT 'drop_index' AS test_name,
       COUNT(*) = 0 AS passed
FROM mys_informa_schema.indexs
WHERE schema_name = 'mysql_compat_ddl_advanced'
  AND table_name = 'mysql_adv_base'
  AND key_name LIKE '%mysql_adv_index_new';

RENAME TABLE mysql_adv_base TO mysql_adv_renamed;
ALTER TABLE mysql_adv_renamed RENAME TO mysql_adv_base;
SELECT 'rename_table_forms' AS test_name,
       COUNT(*) = 1 AS passed
FROM mys_informa_schema.tables
WHERE table_schema = 'mysql_compat_ddl_advanced'
  AND table_name = 'mysql_adv_base';

CREATE ALGORITHM=MERGE SQL SECURITY INVOKER VIEW mysql_adv_view AS
  SELECT id, code, doubled FROM mysql_adv_base;
CREATE OR REPLACE ALGORITHM=TEMPTABLE SQL SECURITY DEFINER VIEW mysql_adv_view AS
  SELECT id, code, doubled FROM mysql_adv_base;
SELECT 'create_or_replace_view' AS test_name,
       COUNT(*) = 1 AND MIN(doubled) = 4 AS passed FROM mysql_adv_view;

DROP VIEW mysql_adv_view;
ALTER TABLE mysql_adv_base DROP COLUMN note;
SELECT 'drop_column' AS test_name,
       COUNT(*) = 0 AS passed
FROM mys_informa_schema.columns
WHERE table_schema = 'mysql_compat_ddl_advanced'
  AND table_name = 'mysql_adv_base'
  AND column_name = 'note';

DROP DATABASE mysql_compat_ddl_advanced;
