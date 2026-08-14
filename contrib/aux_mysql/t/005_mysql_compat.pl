# Copyright (c) 2026, openHalo PG18
#
# 005_mysql_compat.pl
#
# MySQL wire-protocol SQL compatibility regression suite.
# Starts a PostgreSQL node with a MySQL listener, creates a
# mysql_native_password-authenticated user, and delegates the
# full mysql_compat SQL suite to run_mysql_compat.sh.
#
# The suite (shell runner) handles:
#   - CREATE/DROP DATABASE lifecycle
#   - executing each .sql file through the mysql CLI
#   - assertion extraction (test_name + passed columns)
#   - output comparison against expected/*.out baselines
#
# To regenerate expected baselines, set:
#   MYSQL_COMPAT_UPDATE_EXPECTED=1

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
use File::Temp qw(tempdir);
use IPC::Run qw(run timeout);
use Time::HiRes qw(sleep time);

# --- locate mysql CLI --------------------------------------------------
my $mysql = $ENV{MYSQL_BIN} // '/usr/bin/mysql';
my ($version_stdout, $version_stderr) = ('', '');
my $client_works = eval {
    run [ $mysql, '--version' ], '>', \$version_stdout,
      '2>', \$version_stderr, timeout(10);
};

if (!$client_works) {
    plan skip_all =>
      "mysql client '$mysql' is not usable; skipping MySQL SQL compatibility suite";
}
chomp($version_stdout);
diag("using MySQL client: $version_stdout");

# --- start cluster -----------------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('mysql_compat_suite');
$node->init;

# The initdb template already contains a broad localhost trust rule.  Replace
# it so the MySQL login below actually exercises the native-password/md5 path.
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

# --- create extension and test user ------------------------------------
$node->safe_psql('postgres',
    "CREATE EXTENSION aux_mysql VERSION '1.1' CASCADE");
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.2"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.3"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.4"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.5"');
$node->safe_psql('postgres', 'ALTER EXTENSION aux_mysql UPDATE TO "1.6"');
$node->safe_psql(
    'postgres', q{
SET password_encryption = 'mysql_native_password';
CREATE USER test SUPERUSER PASSWORD 'test';
});

# With protocol-aware verifier storage, the MySQL native-password
# verifier lives in pg_authid.rolpasswordext and is deliberately
# invisible through pg_shadow, which only exposes rolpassword.  The
# MySQL login below is the real proof that the verifier is present and
# usable.
my $ext_check = $node->safe_psql(
    'postgres', q{
SELECT rolpasswordext LIKE 'mysql_native_password:%'
FROM pg_authid
WHERE rolname = 'test';
});
is($ext_check, 't', 'test role stores mysql_native_password in rolpasswordext');

my $pw_check = $node->safe_psql(
    'postgres', q{
SELECT passwd LIKE 'mysql_native_password:%'
FROM pg_shadow
WHERE usename = 'test';
});
is($pw_check, '', 'mysql_native_password verifier is not exposed via pg_shadow');

# --- PG mode keeps standard integer division ---------------------------
# The mysql./ and mysql.// division operators live in the mysql schema,
# which is only ahead of pg_catalog inside MySQL-protocol sessions
# (mysql_session_initialize sets search_path).  A plain psql connection
# must still resolve 5/2 to pg_catalog int4div (=> 2), proving the
# MySQL-schema operators do not leak into PG mode.
my $pg_division = $node->safe_psql(
    'postgres', q{
SELECT 5 / 2;
});
is($pg_division, '2', 'PG mode: 5/2 remains integer division');

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

# --- run full SQL compatibility suite ---------------------------------
my $suite_runner = "$FindBin::RealBin/mysql_compat/run_mysql_compat.sh";
my $update_expected = $ENV{MYSQL_COMPAT_UPDATE_EXPECTED} // 0;

SKIP: {
    skip 'MYSQL_COMPAT_SKIP_SUITE is set', 1
      if $ENV{MYSQL_COMPAT_SKIP_SUITE};

    my $actual_dir = tempdir('mysql_compat_actual_XXXX', CLEANUP => 1);
    my ($stdout, $stderr) = ('', '');
    my $result;

    {
        local $ENV{MYSQL_BIN}      = $mysql;
        local $ENV{MYSQL_HOST}     = '127.0.0.1';
        local $ENV{MYSQL_PORT}     = $mysql_port;
        local $ENV{MYSQL_USER}     = 'test';
        local $ENV{MYSQL_DATABASE} = 'unvdb_mysqldb';
        local $ENV{MYSQL_PWD}      = 'test';
        local $ENV{MYSQL_COMPAT_ACTUAL_DIR}    = $actual_dir;
        local $ENV{MYSQL_COMPAT_UPDATE_EXPECTED} = $update_expected;
        local $ENV{MYSQL_TEST_TIMEOUT} = 30;

        $result = eval {
            run [ 'bash', $suite_runner ], '>', \$stdout, '2>', \$stderr,
              timeout(420);
        };
    }

    ok($result, 'MySQL SQL compatibility suite passes');
    if (!$result) {
        diag("suite stdout: $stdout") if $stdout ne '';
        diag("suite stderr: $stderr") if $stderr ne '';
        diag("suite exception: $@")   if $@ ne '';
    }
}

# --- teardown ----------------------------------------------------------
$node->stop;
done_testing();
