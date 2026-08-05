-- MySQL-protocol compatibility suite: routines, CALL and routine-backed views.

DROP DATABASE IF EXISTS mysql_compat_routines;
CREATE DATABASE mysql_compat_routines;
USE mysql_compat_routines;

CREATE TABLE mysql_routine_log (
  id INT PRIMARY KEY,
  value_int INT NOT NULL
);

CREATE FUNCTION mysql_routine_add(a INT, b INT)
RETURNS INT DETERMINISTIC
RETURN a + b;

DELIMITER $$
CREATE PROCEDURE mysql_routine_add_log(IN p_id INT, IN p_value INT)
MODIFIES SQL DATA
BEGIN ATOMIC
  INSERT INTO mysql_routine_log(id, value_int)
  VALUES (p_id, p_value);
END$$
DELIMITER ;

CALL mysql_routine_add_log(1, 42);
SELECT 'stored_function_procedure_call' AS test_name,
       mysql_routine_add(19, 23) = 42
       AND (SELECT value_int = 42 FROM mysql_routine_log WHERE id = 1)
       AS passed;

CREATE OR REPLACE SQL SECURITY INVOKER VIEW mysql_routine_view AS
SELECT id, value_int, mysql_routine_add(value_int, 1) AS calculated
FROM mysql_routine_log;
SELECT 'routine_in_view' AS test_name,
       calculated = 43 AS passed FROM mysql_routine_view WHERE id = 1;

SHOW CREATE FUNCTION mysql_routine_add;
SHOW CREATE PROCEDURE mysql_routine_add_log;
SHOW CREATE VIEW mysql_routine_view;

SELECT 'routine_metadata' AS test_name,
       COUNT(*) = 2 AS passed
FROM mys_informa_schema.routines
WHERE routine_schema = 'mysql_compat_routines'
  AND routine_name IN ('mysql_routine_add', 'mysql_routine_add_log');

DROP DATABASE mysql_compat_routines;
