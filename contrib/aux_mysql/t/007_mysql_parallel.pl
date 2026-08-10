# Copyright (c) 2026, openHalo PG18
#
# 007_mysql_parallel.pl
#
# Parallel-worker dialect propagation verification for the MySQL-compatible
# tree (fusion: parallel.c FixedParallelState field merge).
#
# The fusion tree's parallel.c restores MyCompatMode (protocol_kind) and
# re-runs InitParserEngine()/InitADTExt() in every parallel worker.  This
# only matters if MySQL-mode work can actually reach a worker: the
# MySQL-schema division operators (mysql.div_int4 & friends) were
# registered without a PARALLEL clause, which defaults to PARALLEL UNSAFE
# and silently forces any query touching `a / b` out of parallelism.
#
# This test therefore verifies end to end:
#   1. all MySQL division functions are PARALLEL SAFE (regression guard);
#   2. MySQL-mode EXPLAIN shows a Gather (parallel) plan for a/2 queries;
#   3. a parallel sum(a/2) returns the exact MySQL-decimal result, proving
#      the worker restored MySQL semantics (not PG integer division);
#   4. a parallel worker is actually launched for a MySQL-mode query.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use FindBin;
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
      "mysql client '$mysql' is not usable; skipping MySQL parallel suite";
}
chomp($version_stdout);
diag("using MySQL client: $version_stdout");

# --- start MySQL-mode cluster -------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('mysql_parallel_suite');
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
          ], '>', \$stdout, '2>', \$stderr, timeout(20);
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

# --- regression guard: division operators must be parallel-safe ---------
# Without PARALLEL SAFE the mysql./ and mysql.// operator functions default
# to PARALLEL UNSAFE and MySQL-mode division queries silently drop out of
# parallelism, defeating the parallel.c protocol_kind propagation.
my $safe_count = $node->safe_psql('postgres', q{
SELECT count(*) FROM pg_proc
WHERE pronamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'mysql')
  AND proname LIKE 'div\_%' AND proparallel = 's'
});
is($safe_count, '6',
   'all six MySQL division functions are PARALLEL SAFE');

# --- seed a table large enough to cross the parallel threshold ----------
# 20M rows keeps the parallel sum(a/2) running for a second or more, giving
# the pg_stat_activity poll below a realistic window to observe a worker.
$node->safe_psql('postgres',
    "DROP TABLE IF EXISTS par_t;"
  . "CREATE TABLE par_t AS SELECT generate_series(1, 20000000) AS a;"
  . "ANALYZE par_t;");

# --- parallel plan is chosen for a/2 under MySQL mode -------------------
my ($explain_ok, $explain_out, $explain_err) =
    run_mysql('EXPLAIN SELECT count(*), sum(a/2) FROM par_t;');
ok($explain_ok, "MySQL-mode EXPLAIN succeeds$explain_err");
like($explain_out, qr/Gather/,
     'MySQL-mode a/2 query uses a parallel (Gather) plan');

# --- MySQL division semantics survive parallel execution ----------------
# sum(a/2) with MySQL decimal division = (1+2+...+20000000)/2
# = 20000000*20000001/4 = 100000005000000.0000.  PG integer division would
# yield 99999995000000 (10M smaller).  A worker that failed to restore
# MySQL semantics would therefore produce the wrong total.
my $expected = '100000005000000.0000';
my ($sum_ok, $sum_out, $sum_err) =
    run_mysql('SELECT sum(a/2) FROM par_t;');
ok($sum_ok, "parallel sum(a/2) executes$sum_err");
is($sum_out, $expected,
   'parallel execution preserves MySQL decimal division semantics');

# --- a parallel worker is really launched -------------------------------
# EXPLAIN ANALYZE reports Workers Launched directly, which is far more
# reliable than racing to observe the short-lived worker via
# pg_stat_activity.  Combined with test 6 (exact MySQL decimal result) and
# test 4 (Gather in the plan), Workers Launched >= 1 proves the worker ran
# the MySQL-mode division with restored MySQL semantics.
my ($an_ok, $an_out, $an_err) =
    run_mysql('EXPLAIN ANALYZE SELECT count(*), sum(a/2) FROM par_t;');
ok($an_ok, "MySQL-mode EXPLAIN ANALYZE succeeds$an_err");
like($an_out, qr/Workers Launched: [1-9]/,
     'parallel worker actually launched for MySQL-mode query');
diag("EXPLAIN ANALYZE output:\n$an_out") if $an_out ne '';

# --- teardown ----------------------------------------------------------
$node->stop;
done_testing();
