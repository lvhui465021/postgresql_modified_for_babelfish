-- MySQL-protocol compatibility suite: DML forms, joins and query expressions.

DROP DATABASE IF EXISTS mysql_compat_dml;
CREATE DATABASE mysql_compat_dml;
USE mysql_compat_dml;

CREATE TABLE mysql_dml_target (
  id INT AUTO_INCREMENT PRIMARY KEY,
  code VARCHAR(20) UNIQUE,
  value_int INT NOT NULL DEFAULT 0,
  note VARCHAR(40) DEFAULT ''
);

INSERT LOW_PRIORITY INTO mysql_dml_target (code, value_int, note)
  VALUES ('a', 1, 'initial'), ('b', 2, 'initial'), ('c', 2, 'initial');
INSERT HIGH_PRIORITY INTO mysql_dml_target (code, value_int, note)
  VALUES ('high', 5, 'high priority');
INSERT DELAYED INTO mysql_dml_target (code, value_int, note)
  VALUES ('delayed', 6, 'delayed');
INSERT INTO mysql_dml_target SET code = 'set', value_int = 4, note = 'insert set';
INSERT INTO mysql_dml_target (code, value_int, note) VALUES ('a', 10, 'duplicate')
  ON DUPLICATE KEY UPDATE value_int = VALUES(value_int), note = VALUES(note);
INSERT IGNORE INTO mysql_dml_target (code, value_int, note) VALUES ('a', 99, 'ignored');
INSERT IGNORE INTO mysql_dml_target (code, value_int, note)
  VALUES ('a', 999, 'ignored duplicate'), ('ignore-new', 7, 'inserted by ignore');
REPLACE INTO mysql_dml_target (id, code, value_int, note)
  VALUES (2, 'b', 20, 'replaced');
REPLACE LOW_PRIORITY INTO mysql_dml_target (id, code, value_int, note)
  VALUES (3, 'c', 30, 'replace low priority');
REPLACE INTO mysql_dml_target SET id = 4, code = 'high', value_int = 40,
  note = 'replace set';
SELECT 'insert_replace_conflict' AS test_name,
       (SELECT value_int = 10 AND note = 'duplicate' FROM mysql_dml_target WHERE code = 'a')
       AND (SELECT value_int = 20 AND note = 'replaced' FROM mysql_dml_target WHERE code = 'b')
       AND (SELECT value_int = 30 FROM mysql_dml_target WHERE code = 'c')
       AND (SELECT value_int = 40 FROM mysql_dml_target WHERE code = 'high')
       AND (SELECT COUNT(*) = 1 FROM mysql_dml_target WHERE code = 'ignore-new')
       AND (SELECT COUNT(*) = 7 FROM mysql_dml_target) AS passed;

CREATE TABLE mysql_dml_source (id INT PRIMARY KEY, value_int INT, note VARCHAR(40));
INSERT INTO mysql_dml_source VALUES (1, 100, 'one'), (2, 200, 'two');
UPDATE mysql_dml_target AS t
  INNER JOIN mysql_dml_source AS s ON t.id = s.id
SET t.value_int = s.value_int, t.note = s.note;
SELECT 'single_target_update_join' AS test_name,
       (SELECT value_int = 100 AND note = 'one' FROM mysql_dml_target WHERE id = 1)
       AND (SELECT value_int = 200 AND note = 'two' FROM mysql_dml_target WHERE id = 2) AS passed;

UPDATE LOW_PRIORITY mysql_dml_target SET note = 'low priority' WHERE code = 'a';
SELECT 'update_low_priority' AS test_name,
       note = 'low priority' FROM mysql_dml_target WHERE code = 'a';

SELECT value_int INTO @mysql_dml_selected
FROM mysql_dml_target WHERE code = 'a';
SELECT 'select_into_user_variable' AS test_name,
       @mysql_dml_selected = 100 AS passed;
SELECT 'distinctrow_limit_count' AS test_name,
       (SELECT value_int FROM (
        SELECT DISTINCTROW value_int FROM mysql_dml_target ORDER BY value_int LIMIT 1
        ) AS distinct_count) = 4 AS passed;
SELECT 'limit_offset_count' AS test_name,
       (SELECT value_int FROM (
          SELECT value_int FROM mysql_dml_target ORDER BY value_int LIMIT 1, 1
        ) AS offset_count) = 6 AS passed;
SELECT 'limit_count_offset' AS test_name,
       (SELECT group_concat(id ORDER BY id) FROM (
          SELECT id FROM mysql_dml_target ORDER BY id LIMIT 2 OFFSET 1
        ) AS limit_offset) = '2,3' AS passed;

SET @mysql_prepare_sql := 'SELECT COUNT(*) FROM mysql_dml_target';
PREPARE mysql_dml_stmt FROM @mysql_prepare_sql;
EXECUTE mysql_dml_stmt;
DEALLOCATE PREPARE mysql_dml_stmt;

DELETE LOW_PRIORITY IGNORE FROM mysql_dml_target WHERE code = 'delayed';
SELECT 'delete_options' AS test_name,
       COUNT(*) = 0 AS passed FROM mysql_dml_target WHERE code = 'delayed';

SELECT 'operators_and_coercion' AS test_name,
       ('12' + 3 = 15) AND ('12' * 2 = 24) AND ('12' DIV 5 = 2)
       AND ('12' % 5 = 2) AND (5 & 3 = 1) AND (5 | 2 = 7)
       AND (5 ^ 3 = 6) AND (8 >> 2 = 2) AND (1 << 3 = 8)
       AND ('10' - 2 = 8) AND ('10' / 2 = 5)
       AND ('10' = 10) AND ('2' < 10) AS passed;
SET sql_mode = '';
SELECT 'numeric_prefix_conversion' AS test_name,
       '12abc' + 0 = 12 AS passed;
SET sql_mode = DEFAULT;
SELECT 'logical_operators' AS test_name,
       (1 AND 2) = 1 AND (0 OR 2) = 1 AND (1 XOR 1) = 0 AS passed;
SELECT 'pattern_operators' AS test_name,
       ('abc' LIKE 'a%') AND ('abc' REGEXP '^a')
       AND NOT ('abc' NOT REGEXP '^a')
       AND ('AbC' RLIKE '(?i)^abc$') AS passed;

-- null-safe equality from UDB-TX Perl inline tests
SELECT 'null_safe_equality' AS test_name,
       (1 <=> 1) = 1 AND (NULL <=> NULL) = 1 AND (1 <=> NULL) = 0 AS passed;

-- boolean literal display (MySQL returns 1/0)
SELECT 'boolean_literals' AS test_name,
       true = 1 AND false = 0 AS passed;

SELECT 'literal_like_case_insensitive' AS test_name,
       'AbC' LIKE 'a%' AS passed;

DROP DATABASE mysql_compat_dml;
