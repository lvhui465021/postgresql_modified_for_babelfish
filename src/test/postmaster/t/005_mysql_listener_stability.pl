#!/usr/bin/perl

# 005_mysql_listener_stability.pl
#
# Exercise the MySQL listener's connection lifecycle independently of the
# functional wire-protocol test.  In particular, a client may vanish while
# the server is waiting for HandshakeResponse41, or may close a fully
# authenticated session without COM_QUIT.  Neither case may strand a client
# backend or prevent subsequent MySQL connections from being served.

use strict;
use warnings;
use Digest::SHA qw(sha1);
use IO::Socket::INET;
use Time::HiRes qw(time);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub mysql_recv_packet
{
	my ($sock) = @_;
	my $header = '';
	my $n = $sock->sysread($header, 4);
	die "read MySQL packet header: " . (defined $n ? $n : 'undef') . ", $!"
	  unless defined $n && $n == 4;

	my $len = ord(substr($header, 0, 1))
	  | (ord(substr($header, 1, 1)) << 8)
	  | (ord(substr($header, 2, 1)) << 16);
	my $payload = '';
	while (length($payload) < $len)
	{
		$n = $sock->sysread($payload, $len - length($payload), length($payload));
		die "read MySQL packet payload: " . (defined $n ? $n : 'undef') . ", $!"
		  unless defined $n && $n > 0;
	}
	return (ord(substr($header, 3, 1)), $payload);
}

sub mysql_send_packet
{
	my ($sock, $payload, $seq) = @_;
	my $len = length($payload);
	my $frame = pack('C3C', $len & 0xff, ($len >> 8) & 0xff,
		($len >> 16) & 0xff, $seq) . $payload;
	my $sent = 0;
	while ($sent < length($frame))
	{
		my $n = $sock->syswrite($frame, length($frame) - $sent, $sent);
		die "write MySQL packet: $!" unless defined $n && $n > 0;
		$sent += $n;
	}
}

sub mysql_open_greeting
{
	my ($port) = @_;
	my $sock = IO::Socket::INET->new(
		PeerAddr => '127.0.0.1', PeerPort => $port, Proto => 'tcp', Timeout => 10)
	  or die "connect MySQL listener: $!";
	$sock->autoflush(1);
	my ($seq, $greeting) = mysql_recv_packet($sock);
	die "unexpected MySQL greeting sequence $seq" unless $seq == 0;
	return ($sock, $greeting);
}

sub mysql_open_authenticated
{
	my ($port) = @_;
	my ($sock, $greeting) = mysql_open_greeting($port);
	my $version_end = index($greeting, "\0", 5);
	die 'malformed MySQL greeting version string' if $version_end < 0;
	my $offset = $version_end + 1 + 4;
	my $part1 = substr($greeting, $offset, 8);
	$offset += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10;
	my $challenge = $part1 . substr($greeting, $offset, 12);

	my $stage1 = sha1('test123');
	my $token = $stage1 ^ sha1($challenge . sha1($stage1));
	my $caps = 0x19bfa285;
	my $login = pack('V', $caps) . pack('V', 0x00ffffff) . pack('C', 0x2d)
	  . ("\0" x 23) . "mysql_user\0" . pack('C', length($token)) . $token
	  . "mysql_native_password\0";
	mysql_send_packet($sock, $login, 1);
	my ($seq, $response) = mysql_recv_packet($sock);
	die "MySQL authentication reply sequence $seq, expected 2" unless $seq == 2;
	die 'MySQL authentication did not return OK'
	  unless length($response) > 0 && ord(substr($response, 0, 1)) == 0;
	return $sock;
}

sub mysql_ping
{
	my ($sock) = @_;
	mysql_send_packet($sock, "\x0e", 0);
	my ($seq, $reply) = mysql_recv_packet($sock);
	return $seq == 1 && length($reply) > 0 && ord(substr($reply, 0, 1)) == 0;
}

sub mysql_query_packet
{
	my ($sock, $query) = @_;
	mysql_send_packet($sock, "\x03" . $query, 0);
	return mysql_recv_packet($sock);
}

my $node = PostgreSQL::Test::Cluster->new('mysql_listener_stability');
my $mysql_port = PostgreSQL::Test::Cluster::get_free_port();
$node->init;
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf', "local all all trust");
$node->append_conf('pg_hba.conf',
	"host postgres mysql_user 127.0.0.1/32 md5");
$node->append_conf('pg_hba.conf', "host all all 127.0.0.1/32 trust");
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
$node->append_conf('postgresql.conf', "database_compat_mode = 'mysql'");
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'mysql_parser, mysm, aux_mysql'");
$node->append_conf('postgresql.conf', "mysql_listener_on = true");
$node->append_conf('postgresql.conf', "mysql_port = $mysql_port");
$node->append_conf('postgresql.conf', "mysql_backend_database = 'postgres'");
$node->append_conf('postgresql.conf', "statement_timeout = '500ms'");
$node->start;

$node->safe_psql('postgres',
	"CREATE USER mysql_user LOGIN PASSWORD 'test123'");
$node->safe_psql('postgres',
	"UPDATE pg_authid SET rolpassword = "
	. "'mysql_native_password:676243218923905cf94cb52a3c9d3eb30ce8e20d' "
	. "WHERE rolname = 'mysql_user'");

# A safe_psql connection is visible to its own pg_stat_activity query, so the
# baseline is stable as long as every disconnected MySQL backend is reaped.
my $client_backend_count = q{
SELECT count(*)
FROM pg_stat_activity
WHERE backend_type = 'client backend'
};
my $baseline = $node->safe_psql('postgres', $client_backend_count);
my $backends_at_most_baseline =
  "SELECT ($client_backend_count) <= $baseline";

# Closing after the greeting makes mysql_verify_login() see EOF while reading
# the login header.  A partial header covers the short-read path as well.
for (1 .. 6)
{
	my ($sock) = mysql_open_greeting($mysql_port);
	$sock->close();
}
for (1 .. 6)
{
	my ($sock) = mysql_open_greeting($mysql_port);
	my $written = $sock->syswrite("\x01\x00");
	die "write partial MySQL header: $!"
	  unless defined $written && $written == 2;
	$sock->close();
}

ok($node->poll_query_until('postgres', $backends_at_most_baseline),
	'pre-authentication disconnects leave no client backends behind');

# Keep several independently authenticated sessions open at once, prove each
# is still routable, then drop them without COM_QUIT.  This exercises the
# normal command-loop EOF cleanup rather than MySQL's graceful quit command.
my @sessions = map { mysql_open_authenticated($mysql_port) } 1 .. 8;
is(scalar @sessions, 8, 'eight MySQL sessions authenticate concurrently');
ok(mysql_ping($_), 'concurrent MySQL session remains usable') for @sessions;

# A timeout must become one recoverable MySQL ERR, not a stuck backend or a
# poisoned wire session.  The elapsed-time assertion distinguishes the
# server-side timeout from an immediate parser or function-lookup failure.
my $timeout_started = time;
my ($timeout_seq, $timeout_reply) =
  mysql_query_packet($sessions[0], 'SELECT pg_catalog.pg_sleep(2)');
my $timeout_elapsed = time - $timeout_started;
is($timeout_seq, 1, 'timed-out MySQL query uses response sequence 1');
is(ord(substr($timeout_reply, 0, 1)), 0xff,
	'timed-out MySQL query returns an ERR packet');
ok($timeout_elapsed >= 0.3 && $timeout_elapsed < 1.5,
	'MySQL query is interrupted by the configured statement timeout');
ok(mysql_ping($sessions[0]),
	'MySQL session remains usable after a statement-timeout error');

# Do the same while an explicit transaction is active.  A later SQL query,
# rather than a protocol-only PING, proves that ROLLBACK clears the aborted
# PostgreSQL transaction state on the MySQL connection.
my ($begin_seq, $begin_reply) = mysql_query_packet($sessions[1], 'BEGIN');
is($begin_seq, 1, 'explicit MySQL BEGIN uses response sequence 1');
is(ord(substr($begin_reply, 0, 1)), 0x00,
	'explicit MySQL BEGIN returns OK');
my ($xact_timeout_seq, $xact_timeout_reply) =
  mysql_query_packet($sessions[1], 'SELECT pg_catalog.pg_sleep(2)');
is($xact_timeout_seq, 1,
	'explicit-transaction timeout uses response sequence 1');
is(ord(substr($xact_timeout_reply, 0, 1)), 0xff,
	'explicit-transaction timeout returns an ERR packet');
my ($rollback_seq, $rollback_reply) = mysql_query_packet($sessions[1], 'ROLLBACK');
is($rollback_seq, 1, 'MySQL ROLLBACK after timeout uses response sequence 1');
is(ord(substr($rollback_reply, 0, 1)), 0x00,
	'MySQL ROLLBACK clears the timed-out transaction');
my ($post_rollback_header_seq, $post_rollback_header) =
  mysql_query_packet($sessions[1], 'SELECT 1');
my (undef, $post_rollback_column) = mysql_recv_packet($sessions[1]);
my (undef, $post_rollback_row) = mysql_recv_packet($sessions[1]);
mysql_recv_packet($sessions[1]);
is($post_rollback_header_seq, 1,
	'MySQL SQL query after timeout rollback starts at sequence 1');
is(substr($post_rollback_row, 1), '1',
	'MySQL SQL query succeeds after timeout rollback');
$_->close() for @sessions;

ok($node->poll_query_until('postgres', $backends_at_most_baseline),
	'non-COM_QUIT disconnects release all MySQL client backends');

my $final = mysql_open_authenticated($mysql_port);
ok(mysql_ping($final),
	'MySQL listener accepts and serves a new session after connection churn');
$final->close();
ok($node->poll_query_until('postgres', $backends_at_most_baseline),
	'final MySQL disconnect also leaves backend count at baseline');

done_testing();
