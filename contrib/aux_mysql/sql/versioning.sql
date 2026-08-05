CREATE EXTENSION aux_mysql VERSION '1.1';

SELECT extversion
FROM pg_extension
WHERE extname = 'aux_mysql';

ALTER EXTENSION aux_mysql UPDATE TO '1.2';

SELECT extversion
FROM pg_extension
WHERE extname = 'aux_mysql';

ALTER EXTENSION aux_mysql UPDATE TO '1.3';

SELECT extversion
FROM pg_extension
WHERE extname = 'aux_mysql';

ALTER EXTENSION aux_mysql UPDATE TO '1.4';

SELECT extversion
FROM pg_extension
WHERE extname = 'aux_mysql';

ALTER EXTENSION aux_mysql UPDATE TO '1.5';

SELECT extversion
FROM pg_extension
WHERE extname = 'aux_mysql';

ALTER EXTENSION aux_mysql UPDATE TO '1.6';

SELECT extversion
FROM pg_extension
WHERE extname = 'aux_mysql';

SELECT mysql.uuid() IS NOT NULL AS uuid_generated;

CREATE SEQUENCE aux_mysql_versioning_seq;
SELECT mysql.setval('aux_mysql_versioning_seq'::regclass, 5, true);
SELECT nextval('aux_mysql_versioning_seq');
DROP SEQUENCE aux_mysql_versioning_seq;

DROP EXTENSION aux_mysql;
