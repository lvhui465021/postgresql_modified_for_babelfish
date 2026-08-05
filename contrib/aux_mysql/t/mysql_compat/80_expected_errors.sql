-- Standard negative tests.  Execute with mysql --force so every statement is
-- attempted.  Every error below is expected MySQL-compatible rejection.

DROP DATABASE IF EXISTS mysql_compat_expected_errors;
CREATE DATABASE mysql_compat_expected_errors;
USE mysql_compat_expected_errors;

CREATE TABLE mysql_negative_values (
  id INT PRIMARY KEY,
  required_value INT NOT NULL,
  unique_value VARCHAR(10) UNIQUE,
  enum_value ENUM('a', 'b'),
  positive_value INT CHECK (positive_value > 0),
  tiny_unsigned TINYINT UNSIGNED
);
INSERT INTO mysql_negative_values VALUES (1, 1, 'one', 'a', 1, 1);

INSERT INTO mysql_negative_values VALUES (1, 2, 'two', 'b', 2, 2);
INSERT INTO mysql_negative_values VALUES (2, 2, 'one', 'b', 2, 2);
INSERT INTO mysql_negative_values VALUES (3, NULL, 'three', 'a', 3, 3);
INSERT INTO mysql_negative_values VALUES (4, 4, 'four', 'a', -1, 4);
INSERT INTO mysql_negative_values VALUES (5, 5, 'five', 'not-in-enum', 5, 5);
INSERT INTO mysql_negative_values VALUES (6, 6, 'six', 'a', 6, 256);

SELECT DATE('2024-02-30');
SELECT DATE('0000-00-00');
SELECT TIMESTAMP('2024-13-01 00:00:00');
CREATE TABLE mysql_negative_values(id INT);
USE mysql_compat_database_does_not_exist;

DROP DATABASE mysql_compat_expected_errors;
