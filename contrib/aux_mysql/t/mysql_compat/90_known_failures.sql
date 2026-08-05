-- Known compatibility defects.  Execute with mysql --force.  Error statements
-- and assertions returning 0 are the current baseline; a fix must change the
-- corresponding expected/90_known_failures.out entry and move the case into a
-- positive SQL file.

-- OpenHalo and PG18 both report an indeterminate collation when a catalog
-- name is compared directly with a MySQL-collated VARCHAR value.
CREATE TEMPORARY TABLE mysql_known_api_name(function_name VARCHAR(64));
INSERT INTO mysql_known_api_name VALUES ('version');
SELECT 'catalog_name_collation' AS test_name, COUNT(*) = 1 AS passed
FROM mysql_known_api_name r JOIN pg_proc p ON p.proname = r.function_name;
DROP TEMPORARY TABLE mysql_known_api_name;

DROP DATABASE IF EXISTS mysql_compat_known_metadata;
CREATE DATABASE mysql_compat_known_metadata;
USE mysql_compat_known_metadata;

CREATE TABLE mysql_known_indexed (id INT PRIMARY KEY, value_int INT);

-- Multi-target UPDATE is gated in the executor but still rejected here.
CREATE TABLE mysql_known_update_a(id INT PRIMARY KEY, value_int INT);
CREATE TABLE mysql_known_update_b(id INT PRIMARY KEY, value_int INT);
INSERT INTO mysql_known_update_a VALUES (1, 10);
INSERT INTO mysql_known_update_b VALUES (1, 20);
UPDATE mysql_known_update_a AS a, mysql_known_update_b AS b
SET a.value_int = 11, b.value_int = 21
WHERE a.id = b.id;

DROP DATABASE mysql_compat_known_metadata;
