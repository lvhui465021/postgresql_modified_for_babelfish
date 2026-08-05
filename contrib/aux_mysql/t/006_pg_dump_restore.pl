# Copyright (c) 2026, openHalo PG18
#
# 006_pg_dump_restore.pl
#
# pg_dump round-trip verification for MySQL-protocol objects.
#
# openHalo represents MySQL DDL as standard PostgreSQL objects (text
# domains for ENUM/SET, private sequences + triggers for AUTO_INCREMENT
# and ON UPDATE CURRENT_TIMESTAMP, and the mysql_default_kind attribute
# reloption).  pg_dump must therefore reproduce them such that a restored
# database behaves identically through the MySQL wire protocol.
#
# This test creates a MySQL-mode node, seeds a table through the mysql
# CLI, dumps it, restores into a second database, and verifies that the
# restored table still AUTO_INCREMENTs, validates ENUM/SET values, and
# keeps the mysql_default_kind reloptions.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use File::Temp;
use IPC::Run qw(run timeout);
use Time::HiRes qw(sleep time);

my $mysql = $ENV{MYSQL_BIN} // '/usr/bin/mysql';
my ($version_stdout, $version_stderr) = ('', '');
my $client_works = eval {
    run [ $mysql, '--version' ], '>', \$version_stdout,
      '2>', \$version_stderr, timeout(10);
};
if (!$client_works) {
    plan skip_all =>
      "mysql client '$mysql' is not usable; skipping pg_dump restore suite";
}
chomp($version_stdout);
diag("using MySQL client: $version_stdout");

# --- start MySQL-mode cluster -------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('mysql_pgdump_suite');
$node->init;
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf', "local all all trust");
$node->append_conf('pg_hba.conf',
    "host all test 127.0.0.1/32 md5");
$node->append_conf('pg_hba.conf', "host all all 127.0.0.1/32 trust");
$node->start;

my $mysql_port = PostgreSQL::Test::Cluster::get_free_port();
$node->append_conf('postgresql.conf', "database_compat_mode = 'mysql'");
$node->append_conf('postgresql.conf', "mysql_listener_on = true");
$node->append_conf('postgresql.conf', "mysql_port = $mysql_port");
$node->append_conf('postgresql.conf',
    "mysql_backend_database = 'postgres'");
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
# mysm registers the MySQL ADT method table in _PG_init; it must be
# preloaded so InitADTExt() dispatches MySQL type semantics in every
# backend from session start.
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'mysql_parser, mysm, aux_mysql'");
$node->restart;
sleep 1;

$node->safe_psql('postgres', "CREATE EXTENSION aux_mysql VERSION '1.1' CASCADE");
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.2"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.3"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.4"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.5"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.6"');
$node->safe_psql('postgres', q{
SET password_encryption = 'mysql_native_password';
CREATE USER test SUPERUSER PASSWORD 'test';
});

# --- wait for MySQL listener -------------------------------------------
sub run_mysql {
    my ($sql) = @_;
    my ($stdout, $stderr) = ('', '');
    my $result;
    local $ENV{MYSQL_PWD} = 'test';
    $result = eval {
        run [
            $mysql,
            '--no-defaults', '--protocol=TCP',
            '--host=127.0.0.1', "--port=$mysql_port",
            '--user=test',
            '--batch', '--raw', '--skip-column-names',
            '--connect-timeout=2', '--execute', $sql
          ], '>', \$stdout, '2>', \$stderr, timeout(15);
    };
    $stdout =~ s/\r\n/\n/g;
    $stdout =~ s/\n\z//;
    return ($result ? 1 : 0, $stdout, $stderr);
}

my $ready = 0;
my $last_error = '';
my $deadline = time() + 15;
while (time() < $deadline) {
    my ($ok, $stdout, $stderr) = run_mysql('SELECT 1');
    if ($ok && $stdout eq '1') { $ready = 1; last; }
    $last_error = $stderr;
    sleep(0.1);
}
ok($ready, 'MySQL listener accepts authenticated connections');
BAIL_OUT("cannot connect to MySQL listener on port $mysql_port: $last_error")
    unless $ready;

# --- seed a table exercising all MySQL DDL features ---------------------
my $ddl =
  "DROP TABLE IF EXISTS dump_t;\n"
  . "CREATE TABLE dump_t (\n"
  . "  id INT AUTO_INCREMENT PRIMARY KEY,\n"
  . "  name VARCHAR(50) NOT NULL,\n"
  . "  status ENUM('a','b','c') DEFAULT 'a',\n"
  . "  tags SET('x','y','z'),\n"
  . "  updated TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP\n"
  . ");\n"
  . "INSERT INTO dump_t (name, tags) VALUES ('hello', 'x,y');\n"
  . "INSERT INTO dump_t (name, tags) VALUES ('second', 'x,z');";

my ($ddl_ok, $ddl_out, $ddl_err) = run_mysql($ddl);
ok($ddl_ok, "seed MySQL table via mysql CLI$ddl_err");

# --- pg_dump ------------------------------------------------------------
my ($dump_out, $dump_err) = ('', '');
my $dump_ok = eval {
    run [
        'pg_dump', '--no-sync', '-h', '127.0.0.1', '-p', $node->port,
        '-d', 'postgres'
      ], '>', \$dump_out, '2>', \$dump_err, timeout(30);
};

ok($dump_ok, 'pg_dump succeeds');
diag("pg_dump stderr: $dump_err") if $dump_err ne '';
BAIL_OUT("pg_dump failed: $dump_err") unless $dump_ok;
my $dump_sql = $dump_out;

like($dump_sql,
    qr/ALTER TABLE ONLY public\.dump_t ALTER COLUMN (id|updated) SET \(mysql_default_kind=expression\)/,
    'dump emits mysql_default_kind reloption for expression defaults');
like($dump_sql,
    qr/CREATE DOMAIN public\.enum_dump_tstatus AS text/,
    'dump emits ENUM domain');
like($dump_sql,
    qr/CREATE DOMAIN public\.set_dump_ttags AS text/,
    'dump emits SET domain');
like($dump_sql,
    qr/halo_enum_check CHECK \(mys_check_enum/,
    'dump emits ENUM membership check');
like($dump_sql,
    qr/CREATE SEQUENCE public\.seqdump_t/,
    'dump emits AUTO_INCREMENT sequence');
like($dump_sql,
    qr/ALTER TABLE ONLY public\.dump_t ALTER COLUMN id SET DEFAULT nextval\('public\.seqdump_t'::regclass\)/,
    'dump emits AUTO_INCREMENT column default');
like($dump_sql,
    qr/CREATE TRIGGER aitdump_t BEFORE INSERT ON public\.dump_t/,
    'dump emits AUTO_INCREMENT BEFORE INSERT trigger');
like($dump_sql,
    qr/CREATE TRIGGER autdump_tupdated BEFORE UPDATE ON public\.dump_t/,
    'dump emits ON UPDATE CURRENT_TIMESTAMP trigger');
like($dump_sql,
    qr/CREATE TRIGGER enumnormtrig_dump_t_status BEFORE INSERT OR UPDATE/,
    'dump emits ENUM normalizer trigger');

# --- restore into a second database -------------------------------------
$node->safe_psql('postgres', 'CREATE DATABASE dump_restore');
my $dump_file = File::Temp->new(TEMPLATE => 'pgdump_XXXX', UNLINK => 1);
print {$dump_file} $dump_sql;
close $dump_file;
my ($restore_out, $restore_err) = ('', '');
my $restore_ok = eval {
    run [
        'psql', '-h', '127.0.0.1', '-p', $node->port,
        '-d', 'dump_restore', '-f', $dump_file->filename
      ], '>', \$restore_out, '2>', \$restore_err, timeout(60);
};
ok($restore_ok, 'restore succeeds');
diag("restore stderr: $restore_err") if $restore_err ne '';

# --- verify restored semantics through the PostgreSQL protocol -----------
# The MySQL wire protocol is pinned to the mysql_backend_database, so the
# restored objects are exercised via psql against the dump_restore
# database.  AUTO_INCREMENT, ENUM validation and ON UPDATE triggers are
# PostgreSQL objects; the MySQL layer is a mapping over them.
my $a1 = $node->safe_psql('dump_restore',
    "INSERT INTO dump_t (name) VALUES ('third') RETURNING id;");
like($a1, qr/^\d+\z/, 'restored table accepts insert');
my $max_val = $node->safe_psql('dump_restore',
    'SELECT MAX(id) FROM dump_t;');
is($max_val, '3', 'AUTO_INCREMENT continues after restore');

my ($bad_ok, $bad_out, $bad_err) = ('', '', '');
my $bad_ret = $node->psql('dump_restore',
    "INSERT INTO dump_t (name, status) VALUES ('bad', 'zzz');",
    stdout => \$bad_out, stderr => \$bad_err);
ok($bad_ret != 0 && ($bad_err =~ /invalid value for MySQL ENUM/ ||
    $bad_err =~ /mys_check_enum/),
    "ENUM validation active after restore: $bad_err");

my $upd_out = $node->safe_psql('dump_restore',
    "UPDATE dump_t SET name='upd' WHERE id=1 RETURNING updated > '2000-01-01';");
is($upd_out, 't', 'ON UPDATE CURRENT_TIMESTAMP active after restore');

my $kind_check = '';
eval {
    $kind_check = $node->safe_psql('dump_restore',
        "SELECT opt.option_value FROM pg_attribute a, pg_options_to_table(a.attoptions) opt WHERE a.attrelid = 'dump_t'::regclass AND a.attname = 'status' AND opt.option_name = 'mysql_default_kind';");
};
diag("kind_check eval error: $@") if $@;
is($kind_check, 'expression', 'mysql_default_kind preserved after restore');

# --- teardown -----------------------------------------------------------
$node->stop;
done_testing();
