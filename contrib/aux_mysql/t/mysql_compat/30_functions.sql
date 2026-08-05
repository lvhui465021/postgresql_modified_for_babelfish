-- MySQL-protocol compatibility suite: public scalar, aggregate, date and lock APIs.

DROP DATABASE IF EXISTS mysql_compat_functions;
CREATE DATABASE mysql_compat_functions;
USE mysql_compat_functions;

SELECT 'string_basics' AS test_name,
       mysql.concat('a', NULL, 'b') IS NULL
       AND mysql.concat('a', 1, 'b') = 'a1b'
       AND mysql.concat_ws('-', 'a', NULL, 'b') = 'a-b'
       AND mysql.left('abcdef', 3) = 'abc'
       AND mysql.right('abcdef', 2) = 'ef'
       AND mysql.substr('abcdef', 2, 3) = 'bcd'
       AND mysql.substring_index('a,b,c', ',', 2) = 'a,b' AS passed;
SELECT 'string_search_pad_case' AS test_name,
       mysql.instr('abcdef', 'cd') = 3
       AND mysql.locate('cd', 'abcdef') = 3
       AND mysql.lpad('x', 3, '0') = '00x'
       AND mysql.rpad('x', 3, '0') = 'x00'
       AND mysql.lcase('AbC') = 'abc' AND mysql.ucase('AbC') = 'ABC'
       AND repeat(CAST('ab' AS CHAR), 3) = 'ababab' AS passed;
SELECT 'string_set_and_numeric' AS test_name,
       mysql.elt(2, 'a', 'b', 'c') = 'b'
       AND mysql.field('b', 'a', 'b', 'c') = 2
       AND mysql.find_in_set('b', 'a,b,c') = 2
       AND mysql.make_set(5, 'a', 'b', 'c') = 'a,c'
       AND mysql.export_set(5, 'Y', 'N', ',', 3) = 'Y,N,Y'
       AND hex(255) = 'FF' AS passed;
SELECT 'string_misc' AS test_name,
       mysql.insert('abcdef', 2, 3, 'XYZ') = 'aXYZef'
       AND REPLACE('abcabc', 'a', 'x') = 'xbcxbc'
       AND REVERSE('abc') = 'cba'
       AND mysql.strcmp('a', 'b') = -1
       AND mysql.space(3) = '   '
       AND mysql.bin(10) = '1010'
       AND mysql.bit_count(7) = 3
       AND mysql.conv('10', 10, 2) = '1010'
       AND mysql.format(1234.567, 2) = '1234.57' AS passed;

SELECT 'conditional_numeric' AS test_name,
       mysql.ifnull(NULL, 7) = 7
       AND mysql.if(1, 'yes', 'no') = 'yes'
       AND mysql.isnull(NULL) = 1
       AND NULLIF(3, 3) IS NULL AND COALESCE(NULL, NULL, 9) = 9
       AND GREATEST(2, 9, 4) = 9 AND LEAST(2, 9, 4) = 2
       AND ABS(-12) = 12
       AND ceil(1.2) = 2 AND floor(1.8) = 1
       AND ceiling(1.2) = 2
       AND mysql.round(1.25, 1) = 1.3
       AND mysql.truncate(1.29, 1) = 1.2
       AND mysql.mod(11, 3) = 2 AND pow(2, 3) = 8 AND power(2, 3) = 8
       AND mysql.sqrt(9) = 3 AND LOG(2, 8) = 3
       AND mysql.log2(8) = 3 AS passed;
SELECT 'random_range' AS test_name, RAND(7) >= 0 AND RAND(7) < 1 AS passed;
SELECT 'string_length_aliases' AS test_name,
       LENGTH('abc') = 3 AND CHAR_LENGTH('a中') = 2
       AND BIT_LENGTH('A') = 8
       AND SUBSTRING('abcdef', 2, 3) = 'bcd'
       AND MID('abcdef', 2, 3) = 'bcd'
       AND LOCATE('a', 'banana', 3) = 4 AS passed;
SELECT 'hex_string' AS test_name,
       HEX(CAST('abc' AS CHAR)) = '616263' AS passed;
SELECT 'encoding_functions' AS test_name,
       UNHEX(CAST('616263' AS CHAR)) = CAST('abc' AS BINARY)
       AND FROM_BASE64(CAST('YWJj' AS CHAR)) = CAST('abc' AS BINARY) AS passed;
SELECT 'base64_text' AS test_name,
       TO_BASE64(CAST('abc' AS CHAR)) = 'YWJj' AS passed;
SELECT 'unsigned_cast' AS test_name,
       CAST(18446744073709551615 AS UNSIGNED) = 18446744073709551615 AS passed;
SELECT 'load_file_missing_returns_null' AS test_name,
       LOAD_FILE('/mysql-compat-file-does-not-exist') IS NULL AS passed;
SELECT 'repeat_untyped_literal' AS test_name,
       repeat('ab', 3) = 'ababab' AS passed;
SELECT 'json' AS test_name,
       json_object('key', 1) IS NOT NULL
       AND json(42) IS NOT NULL
       AND mysql.json_unquote(json_object('key', 'text')->'key') = 'text'
       AND json_object('key', 'text')->>'key' = 'text'
       AND json_object('key', 'text')->>'$.key' = 'text' AS passed;

SELECT 'date_time_arithmetic' AS test_name,
       mysql.date_add('2024-01-01', INTERVAL 2 DAY) = '2024-01-03'
       AND mysql.date_sub('2024-01-03', INTERVAL 2 DAY) = '2024-01-01'
       AND mysql.adddate('2024-01-01', 2) = '2024-01-03'
       AND mysql.subdate('2024-01-03', 2) = '2024-01-01'
       AND mysql.datediff('2024-01-03', '2024-01-01') = 2 AS passed;
SELECT 'date_time_formatting' AS test_name,
       mysql.date_format('2024-02-03 04:05:06', '%Y-%m-%d') = '2024-02-03'
       AND mysql.str_to_date('2024-02-03', '%Y-%m-%d') = '2024-02-03'
       AND mysql.timestampadd('DAY', 2, '2024-01-01 00:00:00') = '2024-01-03 00:00:00'
       AND mysql.timestampdiff('DAY', '2024-01-01', '2024-01-03') = 2
       AND mysql.maketime(1, 2, 3) = '01:02:03' AS passed;
SELECT 'time_arithmetic' AS test_name,
       mysql.addtime('2024-01-01 01:00:00', '02:03:04') = '2024-01-01 03:03:04'
       AND mysql.subtime('2024-01-01 03:03:04', '02:03:04') = '2024-01-01 01:00:00'
       AND mysql.timediff('2024-01-01 03:03:04', '2024-01-01 01:00:00') = '02:03:04'
       AND mysql.last_day('2024-02-03') = '2024-02-29' AS passed;
SELECT 'date_time_extractors' AS test_name,
       DATE('2024-02-03 04:05:06') = '2024-02-03'
       AND mysql.day('2024-02-03') = 3 AND mysql.month('2024-02-03') = 2
       AND mysql.year('2024-02-03') = 2024 AND mysql.quarter('2024-02-03') = 1
       AND mysql.weekday('2024-02-05') = 0 AND mysql.weekofyear('2024-01-01') = 1
       AND HOUR('12:34:56') = 12 AND MINUTE('12:34:56') = 34
       AND SECOND('12:34:56') = 56
       AND MICROSECOND('12:34:56.123456') = 123456
       AND EXTRACT(YEAR FROM '2024-02-03') = 2024
       AND mysql.time_to_sec('01:02:03') = 3723
       AND mysql.sec_to_time(3723) = '01:02:03' AS passed;
SELECT 'calendar_functions' AS test_name,
       mysql.dayname('2024-02-05') = 'Monday'
       AND mysql.monthname('2024-02-05') = 'February'
       AND mysql.dayofmonth('2024-02-05') = 5
       AND mysql.dayofweek('2024-02-04') = 1
       AND mysql.dayofyear('2024-02-01') = 32
       AND mysql.period_add(202401, 2) = 202403
       AND mysql.period_diff(202403, 202401) = 2
       AND MAKEDATE(2024, 60) = '2024-02-29'
       AND FROM_DAYS(TO_DAYS('2024-02-29')) = '2024-02-29' AS passed;
SELECT 'date_time_current' AS test_name,
       NOW() IS NOT NULL AND SYSDATE() IS NOT NULL
       AND CURDATE() IS NOT NULL AND CURTIME() IS NOT NULL
       AND UTC_DATE() IS NOT NULL AND UTC_TIME() IS NOT NULL
       AND UTC_TIMESTAMP() IS NOT NULL AS passed;
SELECT 'time_format' AS test_name,
       TIME_FORMAT('12:34:56', '%H:%i:%s') = '12:34:56' AS passed;
SELECT 'date_time_additional' AS test_name,
       CONVERT_TZ('2024-01-01 00:00:00', '+00:00', '+01:00')
         = '2024-01-01 01:00:00'
       AND TIMESTAMP('2024-01-01', '01:02:03') = '2024-01-01 01:02:03'
       AND WEEK('2024-01-01', 1) = 1
       AND YEARWEEK('2024-01-01') = 202353 AS passed;
SELECT 'unix_uuid_identity' AS test_name,
       mysql.unix_timestamp('1970-01-02 00:00:00')
         - mysql.unix_timestamp('1970-01-01 00:00:00') = 86400
       AND mysql.from_unixtime(0) IS NOT NULL
       AND mysql.uuid() IS NOT NULL
       AND CAST(mysql.uuid_short() AS DECIMAL(20,0)) > 0
       AND mysql.database() = 'mysql_compat_functions'
       AND mysql.current_user() IS NOT NULL AND mysql.session_user() IS NOT NULL AS passed;

CREATE TABLE mysql_function_aggregate (grp INT, value_text VARCHAR(20), value_int INT);
INSERT INTO mysql_function_aggregate VALUES (1, 'b', 2), (1, 'a', 1), (2, 'c', 3);
SELECT 'aggregate_functions' AS test_name,
       (SELECT group_concat(value_text ORDER BY value_text SEPARATOR ',')
        FROM mysql_function_aggregate WHERE grp = 1) = 'a,b'
       AND (SELECT sum(value_int) FROM mysql_function_aggregate WHERE grp = 1) = 3
       AND (SELECT mysql.avg(CAST(value_int AS CHAR))
            FROM mysql_function_aggregate WHERE grp = 1) = 1.5 AS passed;

CREATE TABLE mysql_function_bit_aggregate (grp INT, value_bit BIT(4));
INSERT INTO mysql_function_bit_aggregate VALUES
  (1, B'0001'), (1, B'0011'), (1, B'0111');
SELECT 'bit_aggregate_functions' AS test_name,
       BIT_AND(value_bit) = B'0001'
       AND BIT_OR(value_bit) = B'0111'
       AND BIT_XOR(value_bit) = B'0101' AS passed
FROM mysql_function_bit_aggregate WHERE grp = 1;

SELECT 'named_locks' AS test_name, mysql.get_lock('mysql_compat_lock', 0) = 1 AS passed;
SELECT 'named_lock_state' AS test_name,
       mysql.is_used_lock('mysql_compat_lock') IS NOT NULL
       AND mysql.is_free_lock('mysql_compat_lock') = 0 AS passed;
SELECT 'named_lock_release' AS test_name, mysql.release_lock('mysql_compat_lock') = 1 AS passed;
SELECT 'named_lock_free' AS test_name,
       mysql.is_free_lock('mysql_compat_lock') = 1 AS passed;
SELECT 'sleep_zero' AS test_name, mysql.sleep(0) = 0 AS passed;
SELECT 'mysql_version_contract' AS test_name,
       mysql.version() = '8.4.10-openhalo-1.0' AS passed;

DROP DATABASE mysql_compat_functions;
