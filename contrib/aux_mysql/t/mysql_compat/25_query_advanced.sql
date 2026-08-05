-- MySQL-protocol compatibility suite: advanced query, DML and session state.

DROP DATABASE IF EXISTS mysql_compat_query_advanced;
CREATE DATABASE mysql_compat_query_advanced;
USE mysql_compat_query_advanced;

CREATE TABLE mysql_query_target (
  id INT AUTO_INCREMENT PRIMARY KEY,
  code VARCHAR(20) NOT NULL UNIQUE,
  category VARCHAR(20) NOT NULL,
  amount INT NOT NULL DEFAULT 0,
  KEY mysql_query_amount_idx(amount)
);
CREATE TABLE mysql_query_source (id INT PRIMARY KEY, amount INT NOT NULL);
CREATE TABLE mysql_query_archive (id INT, code VARCHAR(20), amount INT);

INSERT INTO mysql_query_target(code, category, amount) VALUES
  ('a', 'x', 10), ('b', 'x', 20), ('c', 'y', 30), ('d', 'y', 40);
SELECT 'last_insert_id' AS test_name, LAST_INSERT_ID() = 1 AS passed;

INSERT INTO mysql_query_source VALUES (1, 101), (2, 202);
INSERT INTO mysql_query_archive
SELECT id, code, amount FROM mysql_query_target WHERE category = 'x';
SELECT 'insert_select' AS test_name,
       COUNT(*) = 2 AND SUM(amount) = 30 AS passed FROM mysql_query_archive;

SELECT 'join_query' AS test_name,
       COUNT(*) = 2 AND SUM(s.amount) = 303 AS passed
FROM mysql_query_target t JOIN mysql_query_source s ON s.id = t.id;
SELECT 'group_having' AS test_name,
       COUNT(*) = 2 AS passed
FROM (
  SELECT category FROM mysql_query_target
  GROUP BY category HAVING SUM(amount) > 0
) AS grouped;
SELECT 'subquery_exists_in' AS test_name,
       COUNT(*) = 2 AS passed
FROM mysql_query_target t
WHERE t.id IN (SELECT id FROM mysql_query_source)
  AND EXISTS (SELECT 1 FROM mysql_query_source s WHERE s.id = t.id);

WITH mysql_query_cte AS (
  SELECT category, SUM(amount) AS total_amount
  FROM mysql_query_target GROUP BY category
)
SELECT 'cte' AS test_name,
       COUNT(*) = 2 AND SUM(total_amount) = 100 AS passed
FROM mysql_query_cte;

SELECT 'union_all' AS test_name,
       COUNT(*) = 2 AS passed
FROM (
  SELECT code FROM mysql_query_target WHERE id = 1
  UNION ALL
  SELECT code FROM mysql_query_target WHERE id = 2
) AS union_rows;

SELECT SQL_NO_CACHE 'query_cache_and_force_index' AS test_name,
       COUNT(*) = 1 AS passed
FROM mysql_query_target FORCE INDEX(mysql_query_amount_idx)
WHERE amount = 10;

UPDATE mysql_query_target
SET amount = amount + 100
WHERE category = 'y'
ORDER BY id LIMIT 1;
SELECT 'row_count_after_update' AS test_name, ROW_COUNT() = 1 AS passed;
SELECT 'update_order_limit' AS test_name,
       SUM(amount) = 170 AS passed
FROM mysql_query_target WHERE category = 'y';

SET @mysql_query_sql = 'SELECT ? + ? AS prepared_sum';
SET @mysql_query_arg1 = 19;
SET @mysql_query_arg2 = 23;
PREPARE mysql_query_stmt FROM @mysql_query_sql;
EXECUTE mysql_query_stmt
USING @mysql_query_arg1, @mysql_query_arg2;
DEALLOCATE PREPARE mysql_query_stmt;
SELECT 'prepared_execute_using' AS test_name,
       @mysql_query_arg1 + @mysql_query_arg2 = 42 AS passed;

START TRANSACTION;
INSERT INTO mysql_query_target(code, category, amount) VALUES ('tx', 't', 1);
SAVEPOINT mysql_query_savepoint;
UPDATE mysql_query_target SET amount = 2 WHERE code = 'tx';
ROLLBACK TO SAVEPOINT mysql_query_savepoint;
COMMIT;
SELECT 'transaction_savepoint' AS test_name,
       amount = 1 AS passed FROM mysql_query_target WHERE code = 'tx';

DELETE FROM mysql_query_target WHERE category = 'y' ORDER BY id LIMIT 1;
SELECT 'delete_order_limit' AS test_name,
       COUNT(*) = 1 AS passed FROM mysql_query_target WHERE category = 'y';

SELECT SQL_CALC_FOUND_ROWS id
FROM mysql_query_target ORDER BY id LIMIT 2;
SELECT 'calc_found_rows' AS test_name, FOUND_ROWS() > 0 AS passed;

SET sql_mode = '';
SELECT 'nonstrict_empty_numeric' AS test_name, '' + 0 = 0 AS passed;
SET sql_mode = DEFAULT;

DROP DATABASE mysql_compat_query_advanced;
