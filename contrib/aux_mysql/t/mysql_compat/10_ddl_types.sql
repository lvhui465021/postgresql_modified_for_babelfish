-- MySQL-protocol compatibility suite: type aliases, DDL options and constraints.

DROP DATABASE IF EXISTS mysql_compat_ddl;
CREATE DATABASE mysql_compat_ddl;
USE mysql_compat_ddl;

CREATE TABLE mysql_ddl_types (
  id INT UNSIGNED ZEROFILL AUTO_INCREMENT PRIMARY KEY,
  tiny_signed TINYINT,
  tiny_unsigned TINYINT UNSIGNED,
  small_unsigned SMALLINT UNSIGNED,
  medium_signed MEDIUMINT,
  medium_unsigned MEDIUMINT UNSIGNED,
  int_unsigned INT UNSIGNED,
  big_unsigned BIGINT UNSIGNED,
  year_value YEAR,
  bool_value BOOL,
  boolean_value BOOLEAN,
  bit_value BIT(4),
  real_value REAL,
  double_value DOUBLE,
  decimal_value DECIMAL(10,2),
  char_value CHAR(8),
  dt DATETIME,
  date_value DATE,
  time_value TIME,
  bin_value BINARY(4),
  varbin_value VARBINARY(8),
  blob_value BLOB,
  tinyblob_value TINYBLOB,
  mediumblob_value MEDIUMBLOB,
  longblob_value LONGBLOB,
  tinytext_value TINYTEXT,
  text_value TEXT,
  mediumtext_value MEDIUMTEXT,
  longtext_value LONGTEXT,
  enum_value ENUM('red', 'green', 'blue'),
  set_value SET('read', 'write', 'admin'),
  name VARCHAR(32) COMMENT 'column comment',
  touched TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY mysql_ddl_types_name_key (name),
  KEY mysql_ddl_types_date_key (date_value),
  FULLTEXT KEY mysql_ddl_types_text_key (text_value)
) ENGINE=InnoDB AUTO_INCREMENT=100 COMMENT='table comment'
  ROW_FORMAT=DYNAMIC STATS_AUTO_RECALC=1
  STATS_PERSISTENT=1 STATS_SAMPLE_PAGES=1;

INSERT INTO mysql_ddl_types
  (tiny_signed, tiny_unsigned, small_unsigned, medium_signed, medium_unsigned, int_unsigned,
   big_unsigned, year_value, bool_value, boolean_value, bit_value, real_value, double_value,
   decimal_value, char_value, dt, date_value, time_value, bin_value,
   varbin_value, blob_value, tinyblob_value, mediumblob_value, longblob_value,
   tinytext_value, text_value, mediumtext_value, longtext_value, enum_value,
   set_value, name)
VALUES
  (-128, 255, 65535, -8388608, 16777215, 4294967295, 1, 2024, true, true,
   B'1010', 1.5, 2.5, 1234.56, 'char', '2024-02-03 04:05:06',
   '2024-02-03', '04:05:06', 'AB', 'CD', 'blob', 'tinyblob', 'mediumblob',
   'longblob', 'tinytext', 'text', 'mediumtext', 'longtext', 'green',
   'read,write', 'types');

SELECT 'mysql_integer_aliases' AS test_name,
       id = 100 AND tiny_signed = -128 AND tiny_unsigned = 255
       AND small_unsigned = 65535
       AND medium_signed = -8388608 AND medium_unsigned = 16777215
       AND int_unsigned = 4294967295 AND big_unsigned = 1 AS passed
FROM mysql_ddl_types WHERE name = 'types';
SELECT 'datetime_binary_blob_year' AS test_name,
       dt = '2024-02-03 04:05:06' AND year_value = 2024
       AND bool_value = 1 AND boolean_value = 1 AND bit_value = B'1010'
       AND real_value = 1.5 AND double_value = 2.5
       AND decimal_value = 1234.56
       AND char_value = 'char' AND date_value = '2024-02-03'
       AND time_value = '04:05:06'
       AND blob_value = 'blob' AND varbin_value = 'CD'
       AND tinytext_value = 'tinytext' AND text_value = 'text'
       AND mediumtext_value = 'mediumtext' AND longtext_value = 'longtext'
       AND enum_value = 'green' AND set_value = 'read,write' AS passed
FROM mysql_ddl_types WHERE name = 'types';

-- extra MySQL storage table options (from UDB-TX Perl inline tests)
CREATE TABLE mysql_ddl_side_path (
  id INT PRIMARY KEY, payload VARCHAR(20)
) ENGINE=InnoDB AVG_ROW_LENGTH=1024 CHECKSUM=1 COMPRESSION='zlib'
  CONNECTION='side' DELAY_KEY_WRITE=1 ENCRYPTION='N' KEY_BLOCK_SIZE=8
  MAX_ROWS=10 MIN_ROWS=1 PACK_KEYS=1 PASSWORD='x';
INSERT INTO mysql_ddl_side_path VALUES (1, 'side');
SELECT 'storage_table_options_ignored' AS test_name,
       payload = 'side' AS passed FROM mysql_ddl_side_path WHERE id = 1;

CREATE TABLE mysql_ddl_json (id INT PRIMARY KEY, json_value JSON);
INSERT INTO mysql_ddl_json VALUES (1, JSON_OBJECT('key', 1));
SELECT 'json_column_type' AS test_name,
       mysql.json_unquote(json_value->'key') = '1' AS passed
FROM mysql_ddl_json WHERE id = 1;

CREATE TABLE mysql_ddl_zero_length (varchar_zero VARCHAR(0), char_zero CHAR(0));
INSERT INTO mysql_ddl_zero_length VALUES ('', ''), (NULL, NULL);
SELECT 'zero_length_char_varchar' AS test_name,
       (SELECT COUNT(*) = 1 FROM mysql_ddl_zero_length
        WHERE varchar_zero = '' AND char_zero = '')
       AND (SELECT COUNT(*) = 2
            FROM information_schema.columns
            WHERE table_schema = 'mysql_compat_ddl'
              AND table_name = 'mysql_ddl_zero_length'
              AND ((column_name = 'varchar_zero' AND column_type = 'varchar(0)')
                   OR (column_name = 'char_zero' AND column_type = 'char(0)'))) AS passed;

CREATE TABLE mysql_ddl_partitioned (id INT, value_int INT)
PARTITION BY RANGE (id);
CREATE TABLE mysql_ddl_partition_0 PARTITION OF mysql_ddl_partitioned
FOR VALUES FROM (0) TO (10);
INSERT INTO mysql_ddl_partitioned VALUES (1, 10);
SELECT 'partition_of' AS test_name,
       (SELECT COUNT(*) = 1 FROM mysql_ddl_partition_0
        WHERE id = 1 AND value_int = 10) AS passed;

CREATE TABLE mysql_ddl_trigger_source (id INT PRIMARY KEY, value_int INT);
CREATE TABLE mysql_ddl_trigger_audit (id INT PRIMARY KEY, value_int INT);
CREATE TRIGGER mysql_ddl_after_insert AFTER INSERT ON mysql_ddl_trigger_source
FOR EACH ROW INSERT INTO mysql_ddl_trigger_audit(id, value_int)
VALUES (NEW.id, NEW.value_int);
SELECT p.proname, p.prosrc
FROM pg_proc p
WHERE p.proname LIKE '__mysql_trigger%';
INSERT INTO mysql_ddl_trigger_source VALUES (1, 10);
SELECT 'simple_trigger' AS test_name,
       (SELECT COUNT(*) = 1 FROM mysql_ddl_trigger_audit
        WHERE id = 1 AND value_int = 10) AS passed;

UPDATE mysql_ddl_types SET touched = '2000-01-01 00:00:00' WHERE name = 'types';
UPDATE mysql_ddl_types SET name = 'types-updated' WHERE name = 'types';
SELECT 'on_update_current_timestamp' AS test_name,
       touched > '2000-01-01 00:00:00' AS passed
FROM mysql_ddl_types WHERE name = 'types-updated';

ALTER TABLE mysql_ddl_types ALGORITHM=INPLACE;
ALTER TABLE mysql_ddl_types COMMENT='altered table comment' ALGORITHM=INPLACE;
ALTER TABLE mysql_ddl_types ADD COLUMN alter_value INT DEFAULT 7;
ALTER TABLE mysql_ddl_types MODIFY COLUMN alter_value BIGINT DEFAULT 7;
ALTER TABLE mysql_ddl_types CHANGE COLUMN alter_value altered_value BIGINT DEFAULT 7;
SELECT 'alter_add_modify_change' AS test_name, altered_value = 7 AS passed
FROM mysql_ddl_types WHERE name = 'types-updated';

CREATE INDEX mysql_ddl_types_name_index ON mysql_ddl_types (name)
  COMMENT 'ignored index comment' LOCK=NONE INVISIBLE;
CREATE FULLTEXT INDEX mysql_ddl_types_fulltext ON mysql_ddl_types (name)
  COMMENT 'ignored fulltext modifier';
CREATE SPATIAL INDEX mysql_ddl_types_spatial ON mysql_ddl_types (name);
SHOW INDEX FROM mysql_ddl_types;

CREATE TABLE mysql_ddl_parent (id INT PRIMARY KEY);
CREATE TABLE mysql_ddl_child_cascade (
  id INT PRIMARY KEY, parent_id INT NOT NULL,
  CONSTRAINT mysql_ddl_child_cascade_fk FOREIGN KEY (parent_id)
    REFERENCES mysql_ddl_parent(id) ON UPDATE CASCADE ON DELETE CASCADE
);
CREATE TABLE mysql_ddl_child_null (
  id INT PRIMARY KEY, parent_id INT NULL,
  CONSTRAINT mysql_ddl_child_null_fk FOREIGN KEY (parent_id)
    REFERENCES mysql_ddl_parent(id) ON UPDATE SET NULL ON DELETE SET NULL
);
INSERT INTO mysql_ddl_parent VALUES (1);
INSERT INTO mysql_ddl_child_cascade VALUES (1, 1);
INSERT INTO mysql_ddl_child_null VALUES (1, 1);
UPDATE mysql_ddl_parent SET id = 2 WHERE id = 1;
SELECT 'foreign_key_update_actions' AS test_name,
       (SELECT parent_id = 2 FROM mysql_ddl_child_cascade WHERE id = 1)
       AND (SELECT parent_id IS NULL FROM mysql_ddl_child_null WHERE id = 1) AS passed;
DELETE FROM mysql_ddl_parent WHERE id = 2;
SELECT 'foreign_key_delete_actions' AS test_name,
       (SELECT COUNT(*) = 0 FROM mysql_ddl_child_cascade)
       AND (SELECT parent_id IS NULL FROM mysql_ddl_child_null WHERE id = 1) AS passed;

DROP DATABASE mysql_compat_ddl;
