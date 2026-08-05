#!/usr/bin/perl

# 004_mysql_protocol.pl
#
# End-to-end test of the MySQL wire-protocol adapter.
# Starts a PostgreSQL node with both a standard PG listener (port chosen by
# PostgresNode) and a MySQL TCP listener (mysql_port).  Uses raw MySQL
# packets (pure Perl) to verify the handshake, mysql_native_password
# authentication, COM_QUERY, COM_PING, COM_QUIT, and the M2 parser
# pipeline through to the MySQL DestReceiver.
#
# This test does NOT require the mysql CLI — everything is driven at the
# wire level so that we can validate exact packet formats.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# --- Helpers ----------------------------------------------------------

sub sha1 { require Digest::SHA; return Digest::SHA::sha1($_[0]); }
sub sha256 { require Digest::SHA; return Digest::SHA::sha256($_[0]); }

sub mysql_recv_packet {
    my ($sock) = @_;
    my $max_payload = 0xFFFFFF;
    my $payload = '';
    my $first_seq;
    my $expected_seq;
    my $plen;

    do {
        my $header = '';
        my $n = $sock->sysread($header, 4);
        die "recv header: sysread returned " . (defined $n ? $n : 'undef') . ", err=$!"
            unless defined $n && $n == 4;
        $plen = ord(substr($header, 0, 1))
              | (ord(substr($header, 1, 1)) << 8)
              | (ord(substr($header, 2, 1)) << 16);
        my $seq = ord(substr($header, 3, 1));
        if (!defined $first_seq) {
            $first_seq = $seq;
            $expected_seq = ($seq + 1) & 0xFF;
        } else {
            die "recv sequence: expected $expected_seq, got $seq"
                unless $seq == $expected_seq;
            $expected_seq = ($expected_seq + 1) & 0xFF;
        }
        my $remaining = $plen;
        while ($remaining > 0) {
            my $chunk = '';
            $n = $sock->sysread($chunk, $remaining);
            die "recv payload: need=$remaining, got " . (defined $n ? $n : 'undef') . ", err=$!"
                unless defined $n && $n > 0;
            $payload .= $chunk;
            $remaining -= $n;
        }
    } while ($plen == $max_payload);
    return ($first_seq, $payload);
}

sub mysql_recv {
    my ($sock) = @_;
    my ($seq, $payload) = mysql_recv_packet($sock);
    return $payload;
}

sub mysql_send_seq {
    my ($sock, $payload, $seq) = @_;
    my $max_payload = 0xFFFFFF;
    my $offset = 0;
    my $remaining = length($payload);
    my $send_empty_terminator =
        $remaining > 0 && $remaining % $max_payload == 0;

    # MySQL continuation packets use the next sequence ID; an exact multiple
    # of 0xffffff requires a final empty packet to terminate the message.
    do {
        my $plen = $remaining > $max_payload ? $max_payload : $remaining;
        my $chunk = substr($payload, $offset, $plen);
        my $hdr = pack('C3C', $plen & 0xFF, ($plen >> 8) & 0xFF,
                       ($plen >> 16) & 0xFF, $seq);
        my $frame = $hdr . $chunk;
        my $written = 0;

        while ($written < length($frame)) {
            my $n = $sock->syswrite($frame, length($frame) - $written,
                                    $written);
            die "send: $!" unless defined $n && $n > 0;
            $written += $n;
        }
        $offset += $plen;
        $remaining -= $plen;
        $seq = ($seq + 1) & 0xFF;
        $send_empty_terminator = 0 if $plen == 0;
    } while ($remaining > 0 || $send_empty_terminator);
}

sub mysql_open {
    my ($port) = @_;
    my $sock = IO::Socket::INET->new(
        PeerAddr => '127.0.0.1', PeerPort => $port,
        Proto => 'tcp', Timeout => 10,
    );
    die "connect MySQL socket: $!" unless $sock;
    $sock->autoflush(1);
    my ($seq, $greet) = mysql_recv_packet($sock);
    die "unexpected greeting sequence $seq" unless $seq == 0;

    my $ver_end = index($greet, "\0", 5);
    my $off = $ver_end + 1 + 4;
    my $part1 = substr($greet, $off, 8);
    $off += 8 + 1 + 2 + 1 + 2 + 2;
    my $auth_data_len = ord(substr($greet, $off, 1));
    $off += 1 + 10;
    my $part2_len = $auth_data_len - 8 - 1;
    $part2_len = 12 if $part2_len < 12;
    my $part2 = substr($greet, $off, $part2_len);
    return ($sock, $part1 . $part2);
}

sub mysql_login {
    my ($challenge, $plugin, $password, $caps, $user) = @_;
    my $stage1;
    my $token;

    $user //= 'mysql_user';

    if ($plugin eq 'caching_sha2_password') {
        $stage1 = sha256($password);
        my $stage2 = sha256($stage1);
        $token = $stage1 ^ sha256($stage2 . $challenge);
    }
    else {
        $stage1 = sha1($password);
        $token = $stage1 ^ sha1($challenge . sha1($stage1));
    }

    return pack('V', $caps)
         . pack('V', 0x00FFFFFF)
         . pack('C', 0x2D)
         . ("\x00" x 23)
         . $user . "\x00"
         . pack('C', length($token)) . $token
         . $plugin . "\x00";
}

sub mysql_error_code {
    my ($payload) = @_;
    return undef unless length($payload) >= 3 && ord(substr($payload, 0, 1)) == 0xFF;
    return ord(substr($payload, 1, 1)) | (ord(substr($payload, 2, 1)) << 8);
}

# Both a traditional EOF and its CLIENT_DEPRECATE_EOF replacement encode the
# status word immediately after the header and two zero length-encoded fields.
sub mysql_result_status {
    my ($payload) = @_;
    die "short MySQL result terminator" unless length($payload) >= 5;
    return unpack('v', substr($payload, 3, 2));
}

# A ColumnDefinition41 has six length-encoded strings before its fixed
# fields: catalog, schema, table, org_table, name, org_name.  Decode just
# enough of that layout to assert observable MySQL column labels rather than
# relying on a packet length or a PostgreSQL target-list implementation detail.
sub mysql_lenenc_string {
    my ($payload, $offset_ref) = @_;
    my $first = ord(substr($payload, $$offset_ref, 1));
    my $len;

    $$offset_ref++;
    if ($first < 0xfb) {
        $len = $first;
    }
    elsif ($first == 0xfc) {
        $len = unpack('v', substr($payload, $$offset_ref, 2));
        $$offset_ref += 2;
    }
    elsif ($first == 0xfd) {
        $len = ord(substr($payload, $$offset_ref, 1))
             | (ord(substr($payload, $$offset_ref + 1, 1)) << 8)
             | (ord(substr($payload, $$offset_ref + 2, 1)) << 16);
        $$offset_ref += 3;
    }
    else {
        die "unsupported length-encoded field in test packet";
    }
    my $value = substr($payload, $$offset_ref, $len);
    $$offset_ref += $len;
    return $value;
}

sub mysql_column_name {
    my ($payload) = @_;
    my $offset = 0;
    mysql_lenenc_string($payload, \$offset) for 1 .. 4;
    return mysql_lenenc_string($payload, \$offset);
}

sub mysql_column_type {
    my ($payload) = @_;
    my $offset = 0;
    mysql_lenenc_string($payload, \$offset) for 1 .. 6;
    $offset++;                 # length of the fixed ColumnDefinition41 part
    $offset += 2 + 4;          # character set and maximum column length
    return ord(substr($payload, $offset, 1));
}

sub mysql_column_default {
    my ($payload) = @_;
    my $offset = 0;
    mysql_lenenc_string($payload, \$offset) for 1 .. 6;
    # COM_FIELD_LIST has an 0x0c fixed-field marker followed by the complete
    # 12-byte ColumnDefinition41 fixed part: charset(2), length(4), type(1),
    # flags(2), decimals(1), filler(2).
    $offset += 13;
    return undef if ord(substr($payload, $offset, 1)) == 0xfb;
    return mysql_lenenc_string($payload, \$offset);
}

sub mysql_query_one_text {
    my ($sock, $sql) = @_;
    my $offset = 0;

    mysql_send_seq($sock, "\x03$sql", 0);
    my ($hdr_seq, $hdr) = mysql_recv_packet($sock);
    my $first_byte = ord(substr($hdr, 0, 1));

    # Handle ERR packet (query failed)
    if ($first_byte == 0xFF) {
        my $code = unpack('v', substr($hdr, 1, 2));
        my $msg = substr($hdr, 9);
        die "query '$sql' returned ERR $code: $msg";
    }

    # Handle OK packet (non-SELECT query, no result set)
    if ($first_byte == 0x00 || $first_byte == 0xFE) {
        return undef;
    }

    # Consume the ColumnDefinition41 packets (one per column)
    my $col_count = $first_byte;
    for (1 .. $col_count) {
        mysql_recv_packet($sock);
    }

    # Read the first data row, then consume the rest of the result set
    # (including the final OK/EOF terminator) so the packet stream stays
    # aligned for subsequent commands.  Returns undef for an empty result
    # set (terminator 0xFE without any preceding row).
    my $first_row_value;
    while (1) {
        my ($row_seq, $row_pkt) = mysql_recv_packet($sock);
        last if ord(substr($row_pkt, 0, 1)) == 0xFE;
        if (!defined $first_row_value) {
            $first_row_value = mysql_lenenc_string($row_pkt, \$offset);
        }
    }
    return $first_row_value;
}

# Send a DDL/DML statement and expect an OK response.  Dies on ERR.
sub mysql_query_ok {
    my ($sock, $sql) = @_;
    mysql_send_seq($sock, "\x03$sql", 0);
    my ($seq, $resp) = mysql_recv_packet($sock);
    my $marker = ord(substr($resp, 0, 1));
    if ($marker == 0xFF) {
        my $code = unpack('v', substr($resp, 1, 2));
        my $msg = substr($resp, 9);
        die "query '$sql' returned ERR $code: $msg";
    }
    return ($seq, $resp);
}

# Send a SELECT query.  Returns ($col_count, @rows) where each row is an
# array-ref of lenenc-decoded column values.  Consumes the full result set
# including the EOF/OK terminator.
sub mysql_query_select {
    my ($sock, $sql) = @_;

    mysql_send_seq($sock, "\x03$sql", 0);
    my ($hdr_seq, $hdr) = mysql_recv_packet($sock);
    my $first_byte = ord(substr($hdr, 0, 1));

    # Handle ERR packet (query failed)
    if ($first_byte == 0xFF) {
        my $code = unpack('v', substr($hdr, 1, 2));
        my $msg = substr($hdr, 9);
        die "query '$sql' returned ERR $code: $msg";
    }

    # Handle OK packet (for non-SELECT queries)
    if ($first_byte == 0x00 || $first_byte == 0xFE) {
        return (0);    # 0 columns, no rows (OK response)
    }

    my $col_count = $first_byte;

    # Consume column definitions
    for (1 .. $col_count) {
        my ($col_seq, $col) = mysql_recv_packet($sock);
    }

    # Read rows until the EOF/OK terminator.  The MySQL protocol normally uses
    # 0xFE for a result-set terminator with CLIENT_DEPRECATE_EOF, but an OK
    # packet (0x00) is also valid for servers/paths that do not negotiate that
    # capability.  A one-column empty-string row is exactly one NUL byte, so
    # only treat a multi-byte 0x00 packet as an OK terminator.
    my @rows;
    while (1) {
        my ($row_seq, $row_pkt) = mysql_recv_packet($sock);
        my $row_marker = ord(substr($row_pkt, 0, 1));
        last if $row_marker == 0xFE ||
            ($row_marker == 0x00 && length($row_pkt) >= 7);
        my @cols;
        my $off = 0;
        while ($off < length($row_pkt)) {
            my $marker = ord(substr($row_pkt, $off, 1));
            if ($marker == 0xfb) {
                $off++;                     # NULL marker, no data follows
                push @cols, undef;
            } else {
                push @cols, mysql_lenenc_string($row_pkt, \$off);
            }
        }
        push @rows, \@cols;
    }
    return ($col_count, @rows);
}

sub set_mysql_hba {
    my ($node, @host_lines) = @_;
    unlink($node->data_dir . '/pg_hba.conf');
    $node->append_conf('pg_hba.conf', "local all all trust");
    $node->append_conf('pg_hba.conf', $_) for @host_lines;
    $node->reload;
}

# --- Setup ------------------------------------------------------------

my $node = PostgreSQL::Test::Cluster->new('mysql_protocol_test');
$node->init;
# MySQL native-password verification is only permitted through an md5 HBA
# rule.  Keep local PostgreSQL test control connections available via trust.
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf', "local all all trust");
$node->append_conf('pg_hba.conf',
    "host postgres mysql_user 127.0.0.1/32 md5");
$node->append_conf('pg_hba.conf',
    "host postgres mysql_admin 127.0.0.1/32 md5");
$node->append_conf('pg_hba.conf', "host all all 127.0.0.1/32 trust");
$node->start;

# Configure MySQL listener on a non-default port to avoid conflicts
my $mysql_port = 3308;
$node->append_conf('postgresql.conf', "database_compat_mode = 'mysql'");
$node->append_conf('postgresql.conf', "mysql_listener_on = true");
$node->append_conf('postgresql.conf', "mysql_port = $mysql_port");
$node->append_conf('postgresql.conf', "mysql_backend_database = 'postgres'");
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
$node->append_conf('postgresql.conf', "max_parallel_workers = 2");
# mysm registers the MySQL ADT method table in _PG_init; preload so the
# MySQL type semantics apply from session start.
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'mysql_parser, mysm, aux_mysql'");
# Kill any stale listener on 3308
system("fuser -k $mysql_port/tcp 2>/dev/null");
$node->restart;

# Give the MySQL listener a moment to bind
sleep 1;

# Create a MySQL-authenticated user (mysql_native_password)
my $pg_conn = $node->connstr('postgres');
$node->safe_psql('postgres', "CREATE USER mysql_user LOGIN PASSWORD 'test123';");
$node->safe_psql('postgres',
    "UPDATE pg_authid SET rolpassword = 'mysql_native_password:676243218923905cf94cb52a3c9d3eb30ce8e20d' WHERE rolname = 'mysql_user';");
$node->safe_psql('postgres', "GRANT CREATE ON SCHEMA public TO mysql_user;");
my $mysql_admin_stage2 = unpack('H*', sha1(sha1('admin123')));
$node->safe_psql('postgres',
    "CREATE USER mysql_admin SUPERUSER LOGIN PASSWORD 'admin123';");
$node->safe_psql('postgres',
    "UPDATE pg_authid SET rolpassword = 'mysql_native_password:$mysql_admin_stage2' WHERE rolname = 'mysql_admin';");

# Create the case_insensitive ICU collation required by the MySQL parser
# for VARCHAR/CHAR column definitions.
$node->safe_psql('postgres',
    "CREATE COLLATION IF NOT EXISTS case_insensitive (provider = icu, locale = '\@colStrength=secondary', deterministic = false);");

# Create a public wrapper for FOUND_ROWS() so MySQL clients can call
# SELECT FOUND_ROWS() without the mysql. schema prefix.
$node->safe_psql('postgres',
    "CREATE OR REPLACE FUNCTION public.found_rows() RETURNS bigint
     LANGUAGE sql VOLATILE PARALLEL SAFE
     AS \$\$SELECT pg_catalog.mys_found_rows()\$\$");

# --- Connect with raw MySQL packets -----------------------------------

use IO::Socket::INET;
my $sock = IO::Socket::INET->new(
    PeerAddr => '127.0.0.1',
    PeerPort => $mysql_port,
    Proto    => 'tcp',
    Timeout  => 10,
);
BAIL_OUT("cannot connect to MySQL listener on 127.0.0.1:$mysql_port: $!")
    unless $sock;
$sock->autoflush(1);

# -- Greeting --
my $greet = mysql_recv($sock);
ok(length($greet) >= 44, 'MySQL greeting packet received');
my $proto_ver = ord(substr($greet, 0, 1));
is($proto_ver, 10, 'protocol version 10');

# Extract full scramble from the greeting
# Format: proto(1) + version(NUL) + thread_id(4) + part1(8) +
#         filler(1) + cap_lo(2) + charset(1) + status(2) + cap_hi(2) +
#         auth_data_len(1) + reserved(10) + part2(N) + plugin_name(NUL)
# part2 length = auth_data_len - 8 - 1 (minimum 12)
my $server_ver_end = index($greet, "\0", 5);
my $off = $server_ver_end + 1 + 4;
my $part1 = substr($greet, $off, 8);
$off += 8 + 1 + 2 + 1 + 2 + 2;
my $auth_data_len = ord(substr($greet, $off, 1));
$off += 1 + 10;
my $part2_len = $auth_data_len - 8 - 1;
$part2_len = 12 if $part2_len < 12;
my $part2 = substr($greet, $off, $part2_len);
my $challenge = $part1 . $part2;

# Sequence number after greeting: client sends at seq 1, then resets to 0 for commands
my $client_seq = 1;

# -- Auth: mysql_native_password --
my $p1 = sha1('test123');
my $token = $p1 ^ sha1($challenge . sha1($p1));

# Build login packet
# Includes the capability set emitted by the official MySQL 8.4.10 CLI.
my $caps = 0x19bfa285;
my $login = pack('V', $caps)               # client capabilities
          . pack('V', 0x00FFFFFF)            # max packet size
          . pack('C', 0x2D)                  # charset utf8mb4
          . ("\x00" x 23)                    # reserved
          . "mysql_user\x00"                 # username
          . pack('C', 20) . $token          # auth response
          . "mysql_native_password\x00";     # auth plugin name

mysql_send_seq($sock, $login, $client_seq);
my $auth_resp = mysql_recv($sock);
is(ord(substr($auth_resp, 0, 1)), 0x00, 'authentication OK');

# This is the exact result-set probe mysql(8.4) performs before select $$.
mysql_send_seq($sock, "\x03select \@\@version_comment limit 1", 0);
my ($version_cols_seq, $version_cols) = mysql_recv_packet($sock);
my ($version_def_seq, $version_def) = mysql_recv_packet($sock);
my ($version_row_seq, $version_row) = mysql_recv_packet($sock);
my ($version_end_seq, $version_end) = mysql_recv_packet($sock);
is_deeply([$version_cols_seq, $version_def_seq, $version_row_seq, $version_end_seq],
          [1, 2, 3, 4],
          'version_comment result set completes with contiguous sequences');
is(length($version_def), 39,
   'version_comment ColumnDefinition41 has no duplicate catalog field');
is(mysql_column_name($version_def), '@@version_comment',
   'version_comment ColumnDefinition41 preserves the MySQL system-variable label');
is(ord(substr($version_def, 33, 1)), 253,
   'version_comment ColumnDefinition41 reports VAR_STRING');
is(ord(substr($version_end, 0, 1)), 0xFE,
   'version_comment result set terminates with EOF/OK');

# MySQL 8.4 CLI uses this probe to detect dollar-quote support.  It must see
# a regular MySQL error packet and remain connected, not an EOF/short packet.
mysql_send_seq($sock, "\x03select \$\$", 0);
my ($dollar_seq, $dollar_err) = mysql_recv_packet($sock);
is($dollar_seq, 1, 'dollar-quote probe error uses command sequence 1');
is(mysql_error_code($dollar_err), 1054,
   'dollar-quote probe is returned as ER_BAD_FIELD_ERROR');

SKIP: {
    my $mysql_cli = '/usr/bin/mysql';
    skip 'official MySQL 8.4.10 CLI is not available', 2 unless -x $mysql_cli;

    local $ENV{MYSQL_PWD} = 'test123';
    open(my $cli, '-|', $mysql_cli,
         '--no-defaults', '--protocol=TCP', '--ssl-mode=DISABLED',
         '-h', '127.0.0.1', '-P', $mysql_port, '-u', 'mysql_user',
         '-N', '-e', 'SELECT 42')
        or die "start MySQL 8.4.10 CLI: $!";
    my $cli_output = do { local $/; <$cli> };
    close($cli);
    is($? >> 8, 0, 'official MySQL 8.4.10 CLI authenticates through AuthSwitch');
    is($cli_output, "42\n", 'official MySQL 8.4.10 CLI can execute a query');
}

# -- MyCompatMode propagation into parallel workers ---------------------
# The cast is deliberately evaluated for every scanned row, so the worker
# must use the MySQL ADT time input hook rather than the PostgreSQL fallback.
# Compare a serial execution with a forced parallel plan over the MySQL wire;
# this is the runtime form of the FixedParallelState protocol-kind contract.
$node->safe_psql('postgres', <<'SQL');
CREATE TABLE mysql_parallel_compat (raw_time text NOT NULL);
INSERT INTO mysql_parallel_compat
SELECT '-01:02:03' FROM generate_series(1, 100000);
ANALYZE mysql_parallel_compat;
ALTER TABLE mysql_parallel_compat SET (parallel_workers = 2);
GRANT SELECT ON mysql_parallel_compat TO mysql_user;
SQL

my $parallel_sql =
    'SELECT sum(EXTRACT(EPOCH FROM CAST(raw_time AS TIME))) ' .
    'FROM public.mysql_parallel_compat';
mysql_query_ok($sock, 'SET max_parallel_workers_per_gather = 0');
my ($serial_cols, @serial_rows) = mysql_query_select($sock, $parallel_sql);
is($serial_cols, 1, 'serial MySQL compatibility query returns one column');
is(scalar(@serial_rows), 1, 'serial MySQL compatibility query returns one row');

mysql_query_ok($sock, 'SET min_parallel_table_scan_size = 0');
mysql_query_ok($sock, 'SET parallel_setup_cost = 0');
mysql_query_ok($sock, 'SET parallel_tuple_cost = 0');
mysql_query_ok($sock, 'SET max_parallel_workers_per_gather = 2');
my ($plan_cols, @plan_rows) = mysql_query_select($sock,
    'EXPLAIN SELECT sum(EXTRACT(EPOCH FROM CAST(raw_time AS TIME))) ' .
    'FROM public.mysql_parallel_compat');
my $plan_text = join("\n", map { join(' ', @$_) } @plan_rows);
like($plan_text, qr/Parallel Seq Scan|Gather/,
     'forced MySQL query uses a parallel scan/gather plan');

my ($parallel_cols, @parallel_rows) = mysql_query_select($sock, $parallel_sql);
is($parallel_cols, 1, 'parallel MySQL compatibility query returns one column');
is_deeply(\@parallel_rows, \@serial_rows,
          'parallel MySQL ADT semantics equal serial semantics');

$node->safe_psql('postgres', 'DROP TABLE mysql_parallel_compat');

# -- Auth switch: caching_sha2_password -> mysql_native_password --
# The initial caching_sha2 response is deliberately a real 32-byte token.
# The response to AuthSwitchRequest is instead the raw 20-byte native token.
my $switch_sock = IO::Socket::INET->new(
    PeerAddr => '127.0.0.1',
    PeerPort => $mysql_port,
    Proto    => 'tcp',
    Timeout  => 10,
);
BAIL_OUT("cannot connect switch test socket: $!") unless $switch_sock;
$switch_sock->autoflush(1);

my ($switch_greet_seq, $switch_greet) = mysql_recv_packet($switch_sock);
is($switch_greet_seq, 0, 'auth-switch greeting uses sequence 0');
my $switch_ver_end = index($switch_greet, "\0", 5);
my $switch_off = $switch_ver_end + 1 + 4;
my $switch_part1 = substr($switch_greet, $switch_off, 8);
$switch_off += 8 + 1 + 2 + 1 + 2 + 2;
my $switch_auth_data_len = ord(substr($switch_greet, $switch_off, 1));
$switch_off += 1 + 10;
my $switch_part2_len = $switch_auth_data_len - 8 - 1;
$switch_part2_len = 12 if $switch_part2_len < 12;
my $switch_part2 = substr($switch_greet, $switch_off, $switch_part2_len);
my $switch_challenge = $switch_part1 . $switch_part2;
my $sha2_stage1 = sha256('test123');
my $sha2_stage2 = sha256($sha2_stage1);
my $sha2_token = $sha2_stage1 ^ sha256($sha2_stage2 . $switch_challenge);
my $switch_login = pack('V', $caps)
                 . pack('V', 0x00FFFFFF)
                 . pack('C', 0x2D)
                 . ("\x00" x 23)
                 . "mysql_user\x00"
                 . pack('C', length($sha2_token)) . $sha2_token
                 . "caching_sha2_password\x00";
mysql_send_seq($switch_sock, $switch_login, 1);

my ($switch_seq, $switch_payload) = mysql_recv_packet($switch_sock);
is($switch_seq, 2, 'AuthSwitchRequest uses sequence 2');
is(ord(substr($switch_payload, 0, 1)), 0xFE,
   'AuthSwitchRequest has 0xFE marker');
my $native_plugin = 'mysql_native_password';
is(substr($switch_payload, 1, length($native_plugin)), $native_plugin,
   'AuthSwitchRequest selects mysql_native_password');
my $seed_offset = 1 + length($native_plugin) + 1;
is(length($switch_payload), $seed_offset + 21,
   'AuthSwitchRequest contains a 21-byte plugin challenge');
is(ord(substr($switch_payload, -1, 1)), 0,
   'AuthSwitchRequest challenge has a trailing NUL');
my $switch_seed = substr($switch_payload, $seed_offset, 20);
my $switch_p1 = sha1('test123');
my $switch_token = $switch_p1 ^ sha1($switch_seed . sha1($switch_p1));
mysql_send_seq($switch_sock, $switch_token, 3);
my ($switch_ok_seq, $switch_ok) = mysql_recv_packet($switch_sock);
is($switch_ok_seq, 4, 'auth-switch OK uses sequence 4');
is(ord(substr($switch_ok, 0, 1)), 0x00,
   'raw 20-byte AuthSwitchResponse authenticates');
mysql_send_seq($switch_sock, "\x0E", 0);
my ($switch_ping_seq, $switch_ping) = mysql_recv_packet($switch_sock);
is($switch_ping_seq, 1, 'post-auth command response resets to sequence 1');
is(ord(substr($switch_ping, 0, 1)), 0x00,
   'post-auth COM_PING succeeds');
mysql_send_seq($switch_sock, "\x01", 0);
$switch_sock->close();

# -- COM_QUERY: SELECT 1 --
mysql_send_seq($sock, "\x03SELECT 1", 0);
$client_seq = 1;
my $col_count_pkt = mysql_recv($sock);
my $first_byte = ord(substr($col_count_pkt, 0, 1));

# Parse column count.  If the first byte is 0x00 or 0x01 it could be
# the OPTIONAL_RESULTSET_METADATA metadata_follows flag (0 = no metadata,
# 1 = full metadata).  Otherwise it is the length-encoded column count
# directly (values < 251).
my $ncols;
if ($first_byte == 0 || $first_byte == 1) {
    # Could be metadata_follows flag; if so, the real column count follows
    if (length($col_count_pkt) > 1) {
        $ncols = ord(substr($col_count_pkt, 1, 1));
    } else {
        $ncols = $first_byte;
    }
} else {
    $ncols = $first_byte;
}
is($ncols, 1, 'SELECT 1 returns 1 column');

# Column definition
my $col_def = mysql_recv($sock);
ok(length($col_def) > 10, 'column definition received');
is(mysql_column_name($col_def), '1',
   'SELECT literal column definition preserves the MySQL expression label');

# Metadata EOF (skipped with DEPRECATE_EOF) or EOF present
# Read until we get a row or EOF/OK
my $row_or_eof = mysql_recv($sock);
my $row_first_byte = ord(substr($row_or_eof, 0, 1));

if ($row_first_byte == 0xFE && length($row_or_eof) < 9) {
    # Traditional EOF - read row next
    my $row = mysql_recv($sock);
    ok(length($row) >= 2, 'row data received');
    is(ord(substr($row, 1, 1)), ord('1'), "row contains '1'");
    # Final EOF
    my $final = mysql_recv($sock);
    ok(ord(substr($final, 0, 1)) == 0xFE || ord(substr($final, 0, 1)) == 0x00,
       'final EOF/OK received');
} elsif ($row_first_byte < 251) {
    # DEPRECATE_EOF: no metadata EOF, this IS the row
    ok($row_first_byte >= 1, 'row length >= 1');
    my $row_val = substr($row_or_eof, 1, $row_first_byte);
    is($row_val, '1', "row contains '1'");
    # Final OK (DEPRECATE_EOF style: 0xFE header)
    my $final = mysql_recv($sock);
    ok(ord(substr($final, 0, 1)) == 0xFE, 'final DEPRECATE_EOF OK received');
}

# -- COM_QUERY: SELECT 'hello' --
mysql_send_seq($sock, "\x03SELECT 'hello' AS greeting", 0);
$col_count_pkt = mysql_recv($sock);
$first_byte = ord(substr($col_count_pkt, 0, 1));
$ncols = ($first_byte == 0 || $first_byte == 1) ? ord(substr($col_count_pkt, -1, 1)) : $first_byte;
is($ncols, 1, "SELECT 'hello' returns 1 column");
$col_def = mysql_recv($sock);
ok(length($col_def) > 10, "column def for 'greeting'");

# Skip optional metadata EOF
$row_or_eof = mysql_recv($sock);
$first_byte = ord(substr($row_or_eof, 0, 1));
if ($first_byte == 0xFE && length($row_or_eof) < 9) {
    $row_or_eof = mysql_recv($sock);
    $first_byte = ord(substr($row_or_eof, 0, 1));
}
my $row_val = substr($row_or_eof, 1, $first_byte);
is($row_val, 'hello', "row contains 'hello'");
# Consume final EOF/OK
mysql_recv($sock);

# COM_INIT_DB takes an EOF-terminated database name rather than SQL text;
# verify the command is lowered into a properly framed MySQL USE statement.
mysql_send_seq($sock, "\x02postgres", 0);
my ($init_db_seq, $init_db_ok) = mysql_recv_packet($sock);
is($init_db_seq, 1, 'COM_INIT_DB uses response sequence 1');
is(ord(substr($init_db_ok, 0, 1)), 0x00,
   'COM_INIT_DB accepts the current PostgreSQL database');

mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_field_list_wire (field_one INT NOT NULL, wild_value INT)",
    0);
my ($field_create_seq, $field_create_ok) = mysql_recv_packet($sock);
is($field_create_seq, 1, 'COM_FIELD_LIST fixture CREATE uses sequence 1');
is(ord(substr($field_create_ok, 0, 1)), 0x00,
   'COM_FIELD_LIST fixture CREATE succeeds');
mysql_send_seq($sock, "\x04mysql_field_list_wire\0wild%", 0);
my ($field_list_seq, $field_list_def) = mysql_recv_packet($sock);
my ($field_list_end_seq, $field_list_end) = mysql_recv_packet($sock);
is_deeply([$field_list_seq, $field_list_end_seq], [1, 2],
   'COM_FIELD_LIST sends definition then one closing EOF without a column count');
is(mysql_column_name($field_list_def), 'wild_value',
   'COM_FIELD_LIST wildcard returns the matching real column name');
is(ord(substr($field_list_end, 0, 1)), 0xfe,
   'COM_FIELD_LIST uses an EOF-identifier terminator');
is(length($field_list_end), 7,
   'COM_FIELD_LIST uses DEPRECATE_EOF terminator when negotiated');
is(substr($field_list_end, 3, 2), "\x02\x00",
   'COM_FIELD_LIST DEPRECATE_EOF terminator preserves autocommit status');
mysql_send_seq($sock, "\x04mysql_protocol_missing\0", 0);
my ($missing_field_list_seq, $field_list_err) = mysql_recv_packet($sock);
is($missing_field_list_seq, 1, 'missing COM_FIELD_LIST relation uses error sequence 1');
is(mysql_error_code($field_list_err), 1146,
   'missing COM_FIELD_LIST relation is ER_NO_SUCH_TABLE');
mysql_send_seq($sock, "\x0E", 0);
my ($after_field_list_seq, $after_field_list_ping) = mysql_recv_packet($sock);
is($after_field_list_seq, 1, 'packet sequence resets after COM_FIELD_LIST error');
is(ord(substr($after_field_list_ping, 0, 1)), 0x00,
   'COM_PING succeeds after COM_FIELD_LIST error');

# mysql_list_fields restores MySQL's table default record before encoding
# metadata.  Match that observable behavior for PostgreSQL literal defaults:
# no default and DEFAULT NULL are 0xfb, while integer and text constants are
# emitted as length-encoded values.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_field_default_wire (no_default INT, int_default INT DEFAULT 7, negative_default INT DEFAULT -3, null_default INT DEFAULT NULL)",
    0);
my ($field_default_create_seq, $field_default_create_ok) = mysql_recv_packet($sock);
is($field_default_create_seq, 1,
   'COM_FIELD_LIST default fixture CREATE uses sequence 1');
is(ord(substr($field_default_create_ok, 0, 1)), 0x00,
   'COM_FIELD_LIST default fixture CREATE succeeds');
mysql_send_seq($sock, "\x04mysql_field_default_wire\0", 0);
my @field_default_defs = map { scalar mysql_recv_packet($sock) } 1 .. 4;
my ($field_default_end_seq, $field_default_end) = mysql_recv_packet($sock);
is($field_default_end_seq, 5,
   'COM_FIELD_LIST default metadata terminator follows four definitions');
is_deeply([map { mysql_column_name($_) } @field_default_defs],
          [qw(no_default int_default negative_default null_default)],
          'COM_FIELD_LIST default fixture returns each real column');
is(mysql_column_default($field_default_defs[0]), undef,
   'COM_FIELD_LIST no default is a length-encoded NULL');
is(mysql_column_default($field_default_defs[1]), '7',
   'COM_FIELD_LIST integer default is a length-encoded literal');
is(mysql_column_default($field_default_defs[2]), '-3',
   'COM_FIELD_LIST folds a negative integer default before encoding');
is(mysql_column_default($field_default_defs[3]), undef,
   'COM_FIELD_LIST DEFAULT NULL is a length-encoded NULL');
$node->safe_psql('postgres',
    "CREATE TABLE mysql_field_default_text_wire (text_default text DEFAULT 'xy')");
mysql_send_seq($sock, "\x04mysql_field_default_text_wire\0", 0);
my ($field_text_default_seq, $field_text_default) = mysql_recv_packet($sock);
my ($field_text_default_end_seq, $field_text_default_end) = mysql_recv_packet($sock);
is_deeply([$field_text_default_seq, $field_text_default_end_seq], [1, 2],
   'COM_FIELD_LIST text default uses definition then terminator');
is(mysql_column_default($field_text_default), 'xy',
   'COM_FIELD_LIST text default is a length-encoded literal');

# Parentheses make a MySQL default an expression even if PostgreSQL reduces
# its raw expression to the same A_Const node.  mysql_list_fields restores a
# default record, so nullable expression defaults are NULL and NOT NULL ones
# expose their MySQL type reset value instead of the evaluated expression.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_field_expression_default_wire (nullable_value INT DEFAULT (7), nonnull_value INT NOT NULL DEFAULT (7))",
    0);
my ($expression_default_create_seq, $expression_default_create_ok) = mysql_recv_packet($sock);
is($expression_default_create_seq, 1,
   'expression-default COM_FIELD_LIST fixture CREATE uses sequence 1');
is(ord(substr($expression_default_create_ok, 0, 1)), 0x00,
   'expression-default COM_FIELD_LIST fixture CREATE succeeds');
is($node->safe_psql('postgres',
    "SELECT coalesce(attoptions::text, '') FROM pg_attribute WHERE attrelid = 'mysql_field_expression_default_wire'::regclass AND attname = 'nullable_value'"),
   '{mysql_default_kind=expression}',
   'expression-default CREATE persists MySQL provenance in attoptions');
mysql_send_seq($sock, "\x04mysql_field_expression_default_wire\0", 0);
my @expression_default_defs = map { scalar mysql_recv_packet($sock) } 1 .. 2;
mysql_recv_packet($sock);       # COM_FIELD_LIST terminator
is_deeply([map { mysql_column_default($_) } @expression_default_defs],
          [undef, '0'],
          'COM_FIELD_LIST preserves MySQL expression default-record reset values');

# ALTER ... SET/DROP DEFAULT and the MySQL MODIFY/CHANGE spellings all pass
# through distinct parser/utility paths.  In particular, MODIFY/CHANGE must
# retain DEFAULT (7) as an expression, not silently collapse it to DEFAULT 7.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_field_alter_default_wire (alter_value INT, drop_value INT DEFAULT 7, modify_value INT, change_source INT)",
    0);
my ($alter_default_create_seq, $alter_default_create_ok) = mysql_recv_packet($sock);
is($alter_default_create_seq, 1,
   'ALTER default COM_FIELD_LIST fixture CREATE uses sequence 1');
is(ord(substr($alter_default_create_ok, 0, 1)), 0x00,
   'ALTER default COM_FIELD_LIST fixture CREATE succeeds');
for my $ddl (
    'ALTER TABLE mysql_field_alter_default_wire ALTER COLUMN alter_value SET DEFAULT (7)',
    'ALTER TABLE mysql_field_alter_default_wire ALTER COLUMN drop_value DROP DEFAULT',
    'ALTER TABLE mysql_field_alter_default_wire MODIFY COLUMN modify_value INT NOT NULL DEFAULT (7)',
    'ALTER TABLE mysql_field_alter_default_wire CHANGE COLUMN change_source change_value INT NOT NULL DEFAULT (7)')
{
    mysql_send_seq($sock, "\x03$ddl", 0);
    my ($ddl_seq, $ddl_ok) = mysql_recv_packet($sock);
    is($ddl_seq, 1, "MySQL DDL uses response sequence 1: $ddl");
    is(ord(substr($ddl_ok, 0, 1)), 0x00, "MySQL DDL succeeds: $ddl");
}
mysql_send_seq($sock, "\x04mysql_field_alter_default_wire\0", 0);
my @alter_default_defs = map { scalar mysql_recv_packet($sock) } 1 .. 4;
mysql_recv_packet($sock);       # COM_FIELD_LIST terminator
is_deeply([map { mysql_column_name($_) } @alter_default_defs],
          [qw(alter_value drop_value modify_value change_value)],
          'COM_FIELD_LIST reflects CHANGE COLUMN rename after default updates');
is_deeply([map { mysql_column_default($_) } @alter_default_defs],
          [undef, undef, '0', '0'],
          'COM_FIELD_LIST preserves default provenance across ALTER/MODIFY/CHANGE');

# Extended COM_FIELD_LIST default-record type matrix.
# Cover the literal and expression (aka DEFAULT (<expr>)) paths for
# numeric, temporal, and string types that map to native PostgreSQL
# type OIDs (no mysql.tinyint1 / mysql.double domain dependencies).
# The wire metadata is verified against the mapped MySQL type constant
# and its reset value.  MySQL 8.4.10 wraps the NOT NULL reset value
# for each type in a length-encoded string; nullable expression
# defaults are 0xfb (NULL).
#
# Type→OID→MySQL mapping used here:
#   INT→int8→8 (LONGLONG), SMALLINT→int2→2 (SHORT),
#   DECIMAL→numeric→246 (NEWDECIMAL), DATE→10, TIME→11, TIMESTAMP→12,
#   VARCHAR→253, CHAR→254, TEXT→253

# -- Literal defaults across numeric, temporal and string types ----------
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_field_type_defaults ("
    . "dt DATE DEFAULT '2025-07-27',"
    . "tm TIME DEFAULT '18:30:00',"
    . "ts TIMESTAMP DEFAULT '2025-06-15 12:00:00',"
    . "iv INT DEFAULT 42,"
    . "bv BIGINT DEFAULT -3,"
    . "dv DECIMAL DEFAULT 3.14,"
    . "vc VARCHAR(32) DEFAULT 'hello',"
    . "ch CHAR(8) DEFAULT 'fixed',"
    . "tx TEXT DEFAULT 'lorem ipsum'"
    . ")",
    0);
my ($type_create_seq, $type_create_ok) = mysql_recv_packet($sock);
is($type_create_seq, 1,
   'COM_FIELD_LIST type-matrix fixture CREATE uses sequence 1');
is(ord(substr($type_create_ok, 0, 1)), 0x00,
   'COM_FIELD_LIST type-matrix fixture CREATE succeeds');
mysql_send_seq($sock, "\x04mysql_field_type_defaults\0", 0);
my @type_default_defs = map { scalar mysql_recv_packet($sock) } 1 .. 9;
mysql_recv_packet($sock);  # COM_FIELD_LIST terminator
is_deeply([map { mysql_column_name($_) } @type_default_defs],
          [qw(dt tm ts iv bv dv vc ch tx)],
          'COM_FIELD_LIST type-matrix fixture returns each real column');
is_deeply([map { mysql_column_type($_) } @type_default_defs],
          [10, 11, 12, 3, 8, 246, 253, 254, 253],
          'COM_FIELD_LIST maps types to MySQL wire type constants');
is_deeply([map { mysql_column_default($_) } @type_default_defs],
          ['2025-07-27', '18:30:00', '2025-06-15 12:00:00',
           '42', '-3', '3.14', 'hello', 'fixed', 'lorem ipsum'],
          'COM_FIELD_LIST literal defaults are the folded constant text');

# -- Expression (DEFAULT (...)) reset-value oracle -----------------------
# nullable expression default → 0xfb, NOT NULL expression default → type
# reset value.  NUMERIC/NOT NULL INT already covered by the expression-
# default fixture above; this adds temporal, varchar, and decimal reset
# values.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_field_expr_type_defaults ("
    . "dt_nn DATE NOT NULL DEFAULT ('2025-07-27'),"
    . "tm_nn TIME NOT NULL DEFAULT ('18:30:00'),"
    . "ts_nn TIMESTAMP NOT NULL DEFAULT ('2025-06-15 12:00:00'),"
    . "vc_null VARCHAR(16) DEFAULT ('hello'),"
    . "vc_nn VARCHAR(16) NOT NULL DEFAULT ('world'),"
    . "dv_null DECIMAL DEFAULT (3.14),"
    . "dv_nn DECIMAL NOT NULL DEFAULT (2.72)"
    . ")",
    0);
my ($expr_type_create_seq, $expr_type_create_ok) = mysql_recv_packet($sock);
is($expr_type_create_seq, 1,
   'COM_FIELD_LIST expression-type fixture CREATE uses sequence 1');
is(ord(substr($expr_type_create_ok, 0, 1)), 0x00,
   'COM_FIELD_LIST expression-type fixture CREATE succeeds');
mysql_send_seq($sock, "\x04mysql_field_expr_type_defaults\0", 0);
my @expr_type_defs = map { scalar mysql_recv_packet($sock) } 1 .. 7;
mysql_recv_packet($sock);  # COM_FIELD_LIST terminator
is_deeply([map { mysql_column_default($_) } @expr_type_defs],
          ['0000-00-00', '00:00:00', '0000-00-00 00:00:00',
           undef, '', undef, '0'],
          'COM_FIELD_LIST expression defaults use MySQL reset values per type');

# COM_FIELD_LIST predates CLIENT_DEPRECATE_EOF.  The response has no column
# count in either mode, but a client that did not negotiate the capability
# must receive the legacy five-byte EOF terminator.
my $legacy_caps = $caps & ~0x01000000; # CLIENT_DEPRECATE_EOF
my ($legacy_sock, $legacy_challenge) = mysql_open($mysql_port);
mysql_send_seq($legacy_sock,
    mysql_login($legacy_challenge, 'mysql_native_password', 'test123',
                $legacy_caps), 1);
my ($legacy_auth_seq, $legacy_auth) = mysql_recv_packet($legacy_sock);
is($legacy_auth_seq, 2, 'legacy-capability authentication OK uses sequence 2');
is(ord(substr($legacy_auth, 0, 1)), 0x00,
   'legacy-capability authentication succeeds');
mysql_send_seq($legacy_sock, "\x04mysql_field_list_wire\0wild%", 0);
my ($legacy_field_seq, $legacy_field_def) = mysql_recv_packet($legacy_sock);
my ($legacy_end_seq, $legacy_end) = mysql_recv_packet($legacy_sock);
is_deeply([$legacy_field_seq, $legacy_end_seq], [1, 2],
   'legacy COM_FIELD_LIST has a definition and one terminator');
is(mysql_column_name($legacy_field_def), 'wild_value',
   'legacy COM_FIELD_LIST returns the matching real column name');
is(substr($legacy_end, 0, 1), "\xFE",
   'legacy COM_FIELD_LIST uses an EOF-identifier terminator');
is(length($legacy_end), 5,
   'legacy COM_FIELD_LIST uses the traditional EOF terminator');
mysql_send_seq($legacy_sock, "\x03SELECT 51 AS legacy_one; SELECT 52 AS legacy_two", 0);
my @legacy_multi = map { [mysql_recv_packet($legacy_sock)] } 1 .. 10;
is_deeply([map { $_->[0] } @legacy_multi], [1 .. 10],
          'legacy multi-result metadata and rows use one contiguous sequence');
is_deeply([mysql_result_status($legacy_multi[2]->[1]),
           mysql_result_status($legacy_multi[4]->[1]),
           mysql_result_status($legacy_multi[7]->[1]),
           mysql_result_status($legacy_multi[9]->[1])],
          [0x000a, 0x000a, 0x0002, 0x0002],
          'legacy metadata and row EOF packets carry per-result MORE status');
is_deeply([substr($legacy_multi[3]->[1], 1),
           substr($legacy_multi[8]->[1], 1)], [51, 52],
          'legacy multi-result rows retain both text values');
mysql_send_seq($legacy_sock, "\x01", 0);
$legacy_sock->close();

# A client that did not negotiate CLIENT_MULTI_STATEMENTS must not have the
# first statement executed and the rest silently ignored.
my $no_multi_caps = $caps & ~0x00010000;
my ($no_multi_sock, $no_multi_challenge) = mysql_open($mysql_port);
mysql_send_seq($no_multi_sock,
    mysql_login($no_multi_challenge, 'mysql_native_password', 'test123',
                $no_multi_caps), 1);
mysql_recv_packet($no_multi_sock); # authentication OK
mysql_send_seq($no_multi_sock, "\x03SELECT 1; SELECT 2", 0);
my ($no_multi_seq, $no_multi_err) = mysql_recv_packet($no_multi_sock);
is($no_multi_seq, 1,
   'unnegotiated multi-statement request starts an error response at sequence 1');
is(mysql_error_code($no_multi_err), 1064,
   'unnegotiated multi-statement request is rejected as MySQL syntax error');
mysql_send_seq($no_multi_sock, "\x0E", 0);
my ($after_no_multi_seq, $after_no_multi_ping) = mysql_recv_packet($no_multi_sock);
is($after_no_multi_seq, 1,
   'packet sequence resets after unnegotiated multi-statement rejection');
is(ord(substr($after_no_multi_ping, 0, 1)), 0x00,
   'COM_PING succeeds after unnegotiated multi-statement rejection');
mysql_send_seq($no_multi_sock, "\x01", 0);
$no_multi_sock->close();

# A result row with a 0xffffff-byte payload below exercises server-side
# packet fragmentation.  The reciprocal client-to-server case is verified by
# the fragmented unknown COM_STMT_EXECUTE after a statement has been prepared.
my $max_mysql_payload = 0xFFFFFF;

# The reciprocal path uses a result-row payload of exactly 0xffffff bytes:
# a four-byte length-encoded string prefix plus 0xfffffb bytes of text.  The
# following completion packet must therefore start at sequence 5, after the
# row's data and zero-length terminator packets (sequences 3 and 4).
my $fragment_result_len = $max_mysql_payload - 4;
mysql_send_seq($sock,
    "\x03SELECT pg_catalog.repeat('x', $fragment_result_len)", 0);
my ($large_header_seq, $large_header) = mysql_recv_packet($sock);
my ($large_column_seq, $large_column) = mysql_recv_packet($sock);
my ($large_row_seq, $large_row) = mysql_recv_packet($sock);
my ($large_end_seq, $large_end) = mysql_recv_packet($sock);
is_deeply([$large_header_seq, $large_column_seq, $large_row_seq,
           $large_end_seq], [1, 2, 3, 5],
          'fragmented result row advances packet sequences across write fragments');
is(length($large_row), $max_mysql_payload,
   'fragmented result row is reassembled to its exact logical payload length');
is(substr($large_row, 0, 4), "\xFD\xFB\xFF\xFF",
   'fragmented result row preserves its length-encoded text prefix');

# User variables retain session state across COM_QUERY commands. Assignment
# returns the assigned expression, while a later reference is materialized
# from the same connection's user-variable store.
mysql_send_seq($sock, "\x03SELECT \@oh_user_var := 42", 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($user_assign_row_seq, $user_assign_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($user_assign_row_seq, 3, 'user-variable assignment row uses sequence 3');
is(substr($user_assign_row, 1), '42',
   'user-variable assignment returns the assigned value');

mysql_send_seq($sock, "\x03SELECT \@oh_user_var", 0);
mysql_recv_packet($sock);       # result-set header
my ($user_read_column_seq, $user_read_column) = mysql_recv_packet($sock);
my ($user_read_row_seq, $user_read_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($user_read_column_seq, 2, 'user-variable column definition uses sequence 2');
is(mysql_column_name($user_read_column), "\@oh_user_var",
   'user-variable column definition preserves the MySQL expression label');
is($user_read_row_seq, 3, 'user-variable read row uses sequence 3');
is(substr($user_read_row, 1), '42',
   'user-variable value persists across MySQL commands');

# SQL PREPARE is distinct from COM_STMT_PREPARE: MySQL stores dialect SQL
# text, then binds session user variables at EXECUTE time.
mysql_send_seq($sock,
    "\x03PREPARE mysql_sql_prepare_wire FROM 'SELECT lower(?) AS prepared_sum'", 0);
my ($sql_prepare_seq, $sql_prepare_ok) = mysql_recv_packet($sock);
is($sql_prepare_seq, 1, 'SQL PREPARE uses response sequence 1');
is(ord(substr($sql_prepare_ok, 0, 1)), 0x00,
   'SQL PREPARE accepts MySQL parameter-marker SQL text');
mysql_send_seq($sock, "\x03SET \@sql_prepare_arg := 41", 0);
my ($sql_set_seq, $sql_set_ok) = mysql_recv_packet($sock);
is($sql_set_seq, 1, 'SQL SET user variable uses response sequence 1');
is(ord(substr($sql_set_ok, 0, 1)), 0x00,
   'SQL SET user variable returns OK');
is(mysql_query_one_text($sock, 'SELECT @sql_prepare_arg'), '41',
   'SQL SET stores the user variable for subsequent commands');
mysql_send_seq($sock,
    "\x03EXECUTE mysql_sql_prepare_wire USING \@sql_prepare_arg", 0);
my ($sql_execute_header_seq, $sql_execute_header) = mysql_recv_packet($sock);
my ($sql_execute_column_seq, $sql_execute_column) = mysql_recv_packet($sock);
my ($sql_execute_row_seq, $sql_execute_row) = mysql_recv_packet($sock);
my ($sql_execute_final_seq, $sql_execute_final) = mysql_recv_packet($sock);
is($sql_execute_header_seq, 1, 'SQL EXECUTE result header uses sequence 1');
is(ord(substr($sql_execute_header, 0, 1)), 1,
   'SQL EXECUTE returns one result column');
is($sql_execute_column_seq, 2, 'SQL EXECUTE column definition uses sequence 2');
is($sql_execute_row_seq, 3, 'SQL EXECUTE row uses sequence 3');
my $sql_execute_offset = 0;
is(mysql_lenenc_string($sql_execute_row, \$sql_execute_offset), '41',
   'SQL EXECUTE binds a MySQL user variable and returns its result');
is($sql_execute_final_seq, 4, 'SQL EXECUTE final result terminator uses sequence 4');
mysql_send_seq($sock, "\x03DEALLOCATE PREPARE mysql_sql_prepare_wire", 0);
my ($sql_deallocate_seq, $sql_deallocate_ok) = mysql_recv_packet($sock);
is($sql_deallocate_seq, 1, 'SQL DEALLOCATE uses response sequence 1');
is(ord(substr($sql_deallocate_ok, 0, 1)), 0x00,
   'SQL DEALLOCATE removes the named prepared statement');

# time_zone is a real, connection-local MySQL session variable.  This slice
# deliberately owns only SYSTEM and fixed UTC offsets; it must not fall back
# to the uninstalled historical system-variable registry.
is(mysql_query_one_text($sock, 'SELECT @@session.time_zone'), 'SYSTEM',
   'session time_zone defaults to SYSTEM');
mysql_send_seq($sock, "\x03SET time_zone = '+8:00'", 0);
my ($time_zone_set_seq, $time_zone_set_ok) = mysql_recv_packet($sock);
is($time_zone_set_seq, 1, 'SET time_zone uses command sequence 1');
is(ord(substr($time_zone_set_ok, 0, 1)), 0x00,
   'SET time_zone returns OK');
is(mysql_query_one_text($sock, 'SELECT @@time_zone'), '+08:00',
   'time_zone normalizes a one-digit positive offset');
mysql_send_seq($sock, "\x03SET time_zone = '+14:01'", 0);
my ($invalid_time_zone_seq, $invalid_time_zone_error) = mysql_recv_packet($sock);
is($invalid_time_zone_seq, 1,
   'invalid time_zone setting starts a fresh error response sequence');
is(ord(substr($invalid_time_zone_error, 0, 1)), 0xFF,
   'invalid fixed offset returns a MySQL ERR packet');
is(mysql_query_one_text($sock, 'SELECT @@time_zone'), '+08:00',
   'invalid time_zone setting leaves the prior session value intact');
my $expected_tz_hour = (gmtime(time + 8 * 60 * 60))[2];
is(mysql_query_one_text($sock,
                        'SELECT EXTRACT(HOUR FROM CURRENT_TIMESTAMP)'),
   "$expected_tz_hour", 'time_zone changes PostgreSQL timestamp rendering');
mysql_send_seq($sock, "\x03SET SESSION time_zone = '+00:00'", 0);
mysql_recv_packet($sock);
is(mysql_query_one_text($sock, 'SELECT @@session.time_zone'), '+00:00',
   'SET SESSION time_zone uses the real session-variable path');
mysql_send_seq($sock, "\x03START TRANSACTION", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x03SET LOCAL time_zone = '+09:00'", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x03ROLLBACK", 0);
mysql_recv_packet($sock);
is(mysql_query_one_text($sock, 'SELECT @@session.time_zone'), '+09:00',
   'SET LOCAL time_zone remains a MySQL session setting after ROLLBACK');
my ($time_zone_other_sock, $time_zone_other_challenge) = mysql_open($mysql_port);
mysql_send_seq($time_zone_other_sock,
    mysql_login($time_zone_other_challenge, 'mysql_native_password',
                'test123', $caps), 1);
mysql_recv_packet($time_zone_other_sock);
is(mysql_query_one_text($time_zone_other_sock, 'SELECT @@time_zone'), 'SYSTEM',
   'time_zone state is isolated to one MySQL connection');
mysql_send_seq($time_zone_other_sock, "\x01", 0);
$time_zone_other_sock->close();
mysql_send_seq($sock, "\x03SET time_zone = '+00:00'", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x16SELECT \@\@session.time_zone", 0);
my ($time_zone_prepare_seq, $time_zone_prepare_ok) = mysql_recv_packet($sock);
is($time_zone_prepare_seq, 1,
   'time_zone COM_STMT_PREPARE starts a fresh response sequence');
my $time_zone_stmt_id = unpack('V', substr($time_zone_prepare_ok, 1, 4));
is(unpack('v', substr($time_zone_prepare_ok, 5, 2)), 1,
   'time_zone prepared query has one result column');
mysql_recv_packet($sock);       # result ColumnDefinition41
mysql_send_seq($sock, "\x03SET time_zone = '+09:00'", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock,
    "\x17" . pack('V', $time_zone_stmt_id) . "\x00" . pack('V', 1), 0);
mysql_recv_packet($sock);       # binary result-set header
mysql_recv_packet($sock);       # binary ColumnDefinition41
my ($time_zone_prepared_row_seq, $time_zone_prepared_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
my $time_zone_prepared_offset = 2; # binary row marker + NULL bitmap
is($time_zone_prepared_row_seq, 3,
   'time_zone prepared execution row uses binary-result sequence 3');
is(mysql_lenenc_string($time_zone_prepared_row, \$time_zone_prepared_offset),
   '+09:00', 'prepared @@session.time_zone is evaluated at EXECUTE time');
mysql_send_seq($sock, "\x19" . pack('V', $time_zone_stmt_id), 0);
mysql_send_seq($sock, "\x03SET time_zone = 'SYSTEM'", 0);
mysql_recv_packet($sock);
is(mysql_query_one_text($sock, 'SELECT @@session.time_zone'), 'SYSTEM',
   'SET time_zone SYSTEM restores the connection default');

# MySQL time_zone global state is a server-wide default for connections made
# after SET GLOBAL.  It does not alter the already-created admin session.
mysql_send_seq($sock, "\x03SET GLOBAL time_zone = '+08:00'", 0);
my ($global_denied_seq, $global_denied) = mysql_recv_packet($sock);
is($global_denied_seq, 1,
   'non-superuser SET GLOBAL time_zone starts an error response at sequence 1');
is(ord(substr($global_denied, 0, 1)), 0xFF,
   'non-superuser SET GLOBAL time_zone is rejected');
my ($global_admin_sock, $global_admin_challenge) = mysql_open($mysql_port);
mysql_send_seq($global_admin_sock,
    mysql_login($global_admin_challenge, 'mysql_native_password', 'admin123',
                $caps, 'mysql_admin'), 1);
my ($global_admin_login_seq, $global_admin_login) = mysql_recv_packet($global_admin_sock);
is($global_admin_login_seq, 2,
   'superuser MySQL session authenticates for SET GLOBAL time_zone');
is(ord(substr($global_admin_login, 0, 1)), 0x00,
   'superuser MySQL session authentication returns OK');
mysql_send_seq($global_admin_sock, "\x03SET GLOBAL time_zone = '+8:00'", 0);
my ($global_set_seq, $global_set_ok) = mysql_recv_packet($global_admin_sock);
is($global_set_seq, 1, 'SET GLOBAL time_zone uses command sequence 1');
is(ord(substr($global_set_ok, 0, 1)), 0x00,
   'superuser SET GLOBAL time_zone returns OK');
is(mysql_query_one_text($global_admin_sock, 'SELECT @@global.time_zone'), '+08:00',
   'global time_zone normalizes and is readable');
is(mysql_query_one_text($global_admin_sock, 'SELECT @@session.time_zone'), 'SYSTEM',
   'SET GLOBAL leaves an existing session time_zone unchanged');
my ($global_default_sock, $global_default_challenge) = mysql_open($mysql_port);
mysql_send_seq($global_default_sock,
    mysql_login($global_default_challenge, 'mysql_native_password', 'test123', $caps), 1);
mysql_recv_packet($global_default_sock);
is(mysql_query_one_text($global_default_sock, 'SELECT @@session.time_zone'), '+08:00',
   'new sessions inherit the current global time_zone default');
is(mysql_query_one_text($sock, 'SELECT @@session.time_zone'), 'SYSTEM',
   'SET GLOBAL does not change another existing session time_zone');
mysql_send_seq($global_admin_sock, "\x03SET GLOBAL time_zone = 'SYSTEM'", 0);
my ($global_reset_seq, $global_reset_ok) = mysql_recv_packet($global_admin_sock);
is($global_reset_seq, 1, 'SET GLOBAL time_zone SYSTEM uses command sequence 1');
is(ord(substr($global_reset_ok, 0, 1)), 0x00,
   'SET GLOBAL time_zone SYSTEM returns OK');
mysql_send_seq($global_default_sock, "\x01", 0);
$global_default_sock->close();
mysql_send_seq($global_admin_sock, "\x01", 0);
$global_admin_sock->close();

# A CLIENT_MULTI_STATEMENTS COM_QUERY is one wire exchange: result packet
# sequences remain contiguous, each non-final completion carries MORE, and
# MySQL autocommit commits each individual statement rather than applying
# PostgreSQL simple-query's historical implicit transaction block.
mysql_send_seq($sock, "\x03SELECT 41 AS multi_one; SELECT 42 AS multi_two", 0);
my ($multi_one_hdr_seq, $multi_one_hdr) = mysql_recv_packet($sock);
my ($multi_one_col_seq, $multi_one_col) = mysql_recv_packet($sock);
my ($multi_one_row_seq, $multi_one_row) = mysql_recv_packet($sock);
my ($multi_one_end_seq, $multi_one_end) = mysql_recv_packet($sock);
my ($multi_two_hdr_seq, $multi_two_hdr) = mysql_recv_packet($sock);
my ($multi_two_col_seq, $multi_two_col) = mysql_recv_packet($sock);
my ($multi_two_row_seq, $multi_two_row) = mysql_recv_packet($sock);
my ($multi_two_end_seq, $multi_two_end) = mysql_recv_packet($sock);
is_deeply([$multi_one_hdr_seq, $multi_one_col_seq, $multi_one_row_seq,
           $multi_one_end_seq, $multi_two_hdr_seq, $multi_two_col_seq,
           $multi_two_row_seq, $multi_two_end_seq], [1 .. 8],
          'two SELECT results retain one contiguous COM_QUERY sequence');
is_deeply([mysql_column_name($multi_one_col), mysql_column_name($multi_two_col)],
          [qw(multi_one multi_two)], 'multi-result metadata remains per result');
is_deeply([substr($multi_one_row, 1), substr($multi_two_row, 1)], [41, 42],
          'multi-result text rows retain their individual values');
is(mysql_result_status($multi_one_end), 0x000a,
   'non-final SELECT result carries AUTOCOMMIT and MORE_RESULTS_EXISTS');
is(mysql_result_status($multi_two_end), 0x0002,
   'final SELECT result clears MORE_RESULTS_EXISTS');

mysql_send_seq($sock, "\x03CREATE TABLE mysql_multi_result_wire (v INT)", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_multi_result_wire VALUES (7); SELECT v FROM mysql_multi_result_wire", 0);
my ($multi_dml_seq, $multi_dml_ok) = mysql_recv_packet($sock);
my ($multi_dml_hdr_seq, $multi_dml_hdr) = mysql_recv_packet($sock);
my ($multi_dml_col_seq, $multi_dml_col) = mysql_recv_packet($sock);
my ($multi_dml_row_seq, $multi_dml_row) = mysql_recv_packet($sock);
my ($multi_dml_end_seq, $multi_dml_end) = mysql_recv_packet($sock);
is_deeply([$multi_dml_seq, $multi_dml_hdr_seq, $multi_dml_col_seq,
           $multi_dml_row_seq, $multi_dml_end_seq], [1 .. 5],
          'DML then SELECT shares a contiguous response sequence');
is(mysql_result_status($multi_dml_ok), 0x000a,
   'non-final DML OK carries MORE_RESULTS_EXISTS');
is(substr($multi_dml_row, 1), '7', 'second multi-result observes committed DML');
is(mysql_result_status($multi_dml_end), 0x0002,
   'final result after DML clears MORE_RESULTS_EXISTS');

mysql_send_seq($sock, "\x03BEGIN; SELECT 9 AS multi_tx; COMMIT", 0);
my ($multi_begin_seq, $multi_begin_ok) = mysql_recv_packet($sock);
my ($multi_tx_hdr_seq, $multi_tx_hdr) = mysql_recv_packet($sock);
my ($multi_tx_col_seq, $multi_tx_col) = mysql_recv_packet($sock);
my ($multi_tx_row_seq, $multi_tx_row) = mysql_recv_packet($sock);
my ($multi_tx_end_seq, $multi_tx_end) = mysql_recv_packet($sock);
my ($multi_commit_seq, $multi_commit_ok) = mysql_recv_packet($sock);
is_deeply([$multi_begin_seq, $multi_tx_hdr_seq, $multi_tx_col_seq,
           $multi_tx_row_seq, $multi_tx_end_seq, $multi_commit_seq], [1 .. 6],
          'explicit transaction multi-query response sequence is contiguous');
is(mysql_result_status($multi_begin_ok), 0x000b,
   'BEGIN completion carries IN_TRANS and MORE_RESULTS_EXISTS');
is(mysql_result_status($multi_tx_end), 0x000b,
   'middle SELECT completion preserves IN_TRANS and MORE_RESULTS_EXISTS');
is(mysql_result_status($multi_commit_ok), 0x0002,
   'final COMMIT clears both IN_TRANS and MORE_RESULTS_EXISTS');

mysql_send_seq($sock, "\x03SELECT 1 AS before_error; SELECT no_such_multi_column; SELECT 3", 0);
mysql_recv_packet($sock);       # first result header
mysql_recv_packet($sock);       # first ColumnDefinition41
mysql_recv_packet($sock);       # first row
my ($multi_error_first_end_seq, $multi_error_first_end) = mysql_recv_packet($sock);
my ($multi_error_seq, $multi_error) = mysql_recv_packet($sock);
is($multi_error_first_end_seq, 4,
   'successful result before a multi-query error ends at sequence 4');
is(mysql_result_status($multi_error_first_end), 0x000a,
   'successful result before an error advertises MORE_RESULTS_EXISTS');
is($multi_error_seq, 5, 'multi-query ERR continues the existing packet sequence');
is(mysql_error_code($multi_error), 1054,
   'multi-query stops on the second statement error');
mysql_send_seq($sock, "\x0E", 0);
my ($after_multi_error_seq, $after_multi_error_ping) = mysql_recv_packet($sock);
is($after_multi_error_seq, 1, 'packet sequence resets after multi-query error');
is(ord(substr($after_multi_error_ping, 0, 1)), 0x00,
   'COM_PING succeeds after multi-query error');

# COM_SET_OPTION changes the negotiated multi-statement capability for this
# session without disturbing other capabilities such as DEPRECATE_EOF.
mysql_send_seq($sock, "\x1B" . pack('v', 1), 0); # MULTI_STATEMENTS_OFF
my ($multi_off_seq, $multi_off_end) = mysql_recv_packet($sock);
is($multi_off_seq, 1, 'COM_SET_OPTION OFF responds at sequence 1');
is(mysql_result_status($multi_off_end), 0x0002,
   'COM_SET_OPTION OFF returns an EOF-identifier completion');
mysql_send_seq($sock, "\x03SELECT 11; SELECT 12", 0);
my ($multi_off_error_seq, $multi_off_error) = mysql_recv_packet($sock);
is($multi_off_error_seq, 1,
   'COM_SET_OPTION OFF makes a multi-statement query fail at sequence 1');
is(mysql_error_code($multi_off_error), 1064,
   'COM_SET_OPTION OFF disables multi-statement parsing for the session');
mysql_send_seq($sock, "\x1B" . pack('v', 0), 0); # MULTI_STATEMENTS_ON
my ($multi_on_seq, $multi_on_end) = mysql_recv_packet($sock);
is($multi_on_seq, 1, 'COM_SET_OPTION ON responds at sequence 1');
is(mysql_result_status($multi_on_end), 0x0002,
   'COM_SET_OPTION ON returns an EOF-identifier completion');
mysql_send_seq($sock, "\x03SELECT 11; SELECT 12", 0);
my @multi_reenabled = map { [mysql_recv_packet($sock)] } 1 .. 8;
is_deeply([map { $_->[0] } @multi_reenabled], [1 .. 8],
          'COM_SET_OPTION ON restores contiguous multi-result framing');
is_deeply([mysql_result_status($multi_reenabled[3]->[1]),
           mysql_result_status($multi_reenabled[7]->[1])], [0x000a, 0x0002],
          're-enabled multi-query restores MORE_RESULTS_EXISTS transitions');

# The historical protocol-level SET shortcut must not swallow a following
# statement.  Route a multi-statement SET through the MySQL utility path.
mysql_send_seq($sock, "\x03SET NAMES 'utf8mb4'; SELECT 13 AS after_set", 0);
my ($set_multi_ok_seq, $set_multi_ok) = mysql_recv_packet($sock);
my ($set_multi_hdr_seq, $set_multi_hdr) = mysql_recv_packet($sock);
my ($set_multi_col_seq, $set_multi_col) = mysql_recv_packet($sock);
my ($set_multi_row_seq, $set_multi_row) = mysql_recv_packet($sock);
my ($set_multi_end_seq, $set_multi_end) = mysql_recv_packet($sock);
is_deeply([$set_multi_ok_seq, $set_multi_hdr_seq, $set_multi_col_seq,
           $set_multi_row_seq, $set_multi_end_seq], [1 .. 5],
          'SET followed by SELECT produces two framed results');
is(mysql_result_status($set_multi_ok), 0x000a,
   'SET result advertises MORE_RESULTS_EXISTS rather than swallowing SELECT');
is(mysql_column_name($set_multi_col), 'after_set',
   'SELECT after SET retains its result metadata');
is(substr($set_multi_row, 1), '13', 'SELECT after SET is executed');
is(mysql_result_status($set_multi_end), 0x0002,
   'SELECT after SET terminates the multi-result response');

# SET AUTOCOMMIT is connection state, not a driver-startup no-op.  MySQL 8.4
# does not enter a transaction merely by setting it off; the first DML starts
# one, and switching back on commits that pending work.
mysql_send_seq($sock, "\x03CREATE TABLE mysql_autocommit_wire (v INT)", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x03SET autocommit = 0", 0);
my ($autocommit_off_seq, $autocommit_off) = mysql_recv_packet($sock);
is($autocommit_off_seq, 1, 'SET autocommit=0 starts a fresh response sequence');
is(mysql_result_status($autocommit_off), 0x0000,
   'SET autocommit=0 clears AUTOCOMMIT without entering a transaction');
mysql_send_seq($sock, "\x03INSERT INTO mysql_autocommit_wire VALUES (10)", 0);
my ($autocommit_insert_seq, $autocommit_insert) = mysql_recv_packet($sock);
is($autocommit_insert_seq, 1, 'autocommit-off INSERT starts a fresh response sequence');
is(mysql_result_status($autocommit_insert), 0x0001,
   'first autocommit-off DML enters a transaction');
is($node->safe_psql('postgres',
                    'SELECT count(*) FROM mysql_autocommit_wire'), '0',
   'another connection cannot observe the uncommitted autocommit-off INSERT');
mysql_send_seq($sock, "\x03SET autocommit = 1", 0);
my ($autocommit_on_seq, $autocommit_on) = mysql_recv_packet($sock);
is($autocommit_on_seq, 1, 'SET autocommit=1 starts a fresh response sequence');
is(mysql_result_status($autocommit_on), 0x0002,
   'autocommit-off to on commits and restores AUTOCOMMIT');
is($node->safe_psql('postgres',
                    'SELECT count(*) FROM mysql_autocommit_wire'), '1',
   'autocommit-off to on makes the pending INSERT visible');

# In a multi-statement request status is per statement.  COMMIT while
# autocommit remains off clears IN_TRANS but keeps AUTOCOMMIT clear.
mysql_send_seq($sock,
    "\x03SET autocommit = 0; INSERT INTO mysql_autocommit_wire VALUES (20); COMMIT", 0);
my ($multi_auto_off_seq, $multi_auto_off) = mysql_recv_packet($sock);
my ($multi_auto_insert_seq, $multi_auto_insert) = mysql_recv_packet($sock);
my ($multi_auto_commit_seq, $multi_auto_commit) = mysql_recv_packet($sock);
is_deeply([$multi_auto_off_seq, $multi_auto_insert_seq, $multi_auto_commit_seq],
          [1, 2, 3], 'autocommit-off multi-query keeps one response sequence');
is_deeply([mysql_result_status($multi_auto_off),
           mysql_result_status($multi_auto_insert),
           mysql_result_status($multi_auto_commit)], [0x0008, 0x0009, 0x0000],
          'autocommit-off multi-query preserves MORE and transaction status per result');
is($node->safe_psql('postgres',
                    'SELECT count(*) FROM mysql_autocommit_wire WHERE v = 20'), '1',
   'COMMIT under autocommit=0 persists the multi-query INSERT');

# MySQL BEGIN first commits an active autocommit-off transaction, then starts
# a new one.  The later rollback must retain the first row but discard the
# second.
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_autocommit_wire VALUES (30); BEGIN; INSERT INTO mysql_autocommit_wire VALUES (31); ROLLBACK", 0);
my @begin_after_auto_off = map { [mysql_recv_packet($sock)] } 1 .. 4;
is_deeply([map { $_->[0] } @begin_after_auto_off], [1 .. 4],
          'BEGIN after autocommit-off DML keeps one multi-query response sequence');
is_deeply([map { mysql_result_status($_->[1]) } @begin_after_auto_off],
          [0x0009, 0x0009, 0x0009, 0x0000],
          'BEGIN commits the old autocommit-off transaction before opening the new one');
is($node->safe_psql('postgres',
                    q{SELECT string_agg(v::text, ',' ORDER BY v) FROM mysql_autocommit_wire}),
   '10,20,30', 'BEGIN boundary commits only the pre-BEGIN autocommit-off row');
mysql_send_seq($sock, "\x03SET autocommit = 1", 0);
mysql_recv_packet($sock);

# COM_STMT_EXECUTE must share the same autocommit-off transaction state as
# COM_QUERY DML, rather than committing each prepared execution on return.
mysql_send_seq($sock,
    "\x16INSERT INTO mysql_autocommit_wire VALUES (40)", 0);
my ($auto_prepare_seq, $auto_prepare_ok) = mysql_recv_packet($sock);
my $auto_stmt_id = unpack('V', substr($auto_prepare_ok, 1, 4));
is($auto_prepare_seq, 1, 'autocommit fixture COM_STMT_PREPARE uses sequence 1');
mysql_send_seq($sock, "\x03SET autocommit = 0", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock,
    "\x17" . pack('V', $auto_stmt_id) . "\x00" . pack('V', 1), 0);
my ($auto_execute_seq, $auto_execute_ok) = mysql_recv_packet($sock);
is($auto_execute_seq, 1, 'autocommit-off COM_STMT_EXECUTE uses sequence 1');
is(mysql_result_status($auto_execute_ok), 0x0001,
   'autocommit-off COM_STMT_EXECUTE enters a transaction');
is($node->safe_psql('postgres',
                    'SELECT count(*) FROM mysql_autocommit_wire WHERE v = 40'), '0',
   'prepared DML remains invisible before autocommit-on');
mysql_send_seq($sock, "\x03SET autocommit = 1", 0);
mysql_recv_packet($sock);
is($node->safe_psql('postgres',
                    'SELECT count(*) FROM mysql_autocommit_wire WHERE v = 40'), '1',
   'autocommit-on commits the pending prepared DML');

# -- COM_STMT_PREPARE: plan with a MySQL '?' marker --
# This must use the MySQL parser both for the first analysis and later plan
# revalidation.  The negotiated 8.4 client capabilities include
# CLIENT_DEPRECATE_EOF, so the two metadata sections have no EOF packets.
mysql_send_seq($sock, "\x16SELECT ? + 1", 0);
my ($prepare_ok_seq, $prepare_ok) = mysql_recv_packet($sock);
is($prepare_ok_seq, 1, 'COM_STMT_PREPARE OK uses sequence 1');
is(length($prepare_ok), 12, 'COM_STMT_PREPARE_OK has the protocol-41 size');
is(ord(substr($prepare_ok, 0, 1)), 0,
   'COM_STMT_PREPARE returns a prepare-ok marker');
my $prepared_id = unpack('V', substr($prepare_ok, 1, 4));
ok($prepared_id > 0, 'COM_STMT_PREPARE allocates a nonzero statement id');
is(unpack('v', substr($prepare_ok, 5, 2)), 1,
   'COM_STMT_PREPARE reports one result column');
is(unpack('v', substr($prepare_ok, 7, 2)), 1,
   'COM_STMT_PREPARE reports one parameter');
my ($prepare_param_seq, $prepare_param) = mysql_recv_packet($sock);
my ($prepare_col_seq, $prepare_col) = mysql_recv_packet($sock);
is_deeply([$prepare_param_seq, $prepare_col_seq], [2, 3],
          'COM_STMT_PREPARE metadata has contiguous sequences');
is(ord(substr($prepare_param, 0, 1)), 3,
   'COM_STMT_PREPARE parameter metadata is ColumnDefinition41');
is(ord(substr($prepare_col, 0, 1)), 3,
   'COM_STMT_PREPARE result metadata is ColumnDefinition41');

# A pair of otherwise untyped markers is numeric in MySQL.  In particular,
# Connector/J uses this shape for a forced server-side PreparedStatement;
# PostgreSQL must not reject it as unknown + unknown during PREPARE.
mysql_send_seq($sock, "\x16SELECT ? + ?", 0);
my ($pair_prepare_seq, $pair_prepare_ok) = mysql_recv_packet($sock);
is($pair_prepare_seq, 1,
   'two-marker COM_STMT_PREPARE OK uses sequence 1');
my $pair_stmt_id = unpack('V', substr($pair_prepare_ok, 1, 4));
is(unpack('v', substr($pair_prepare_ok, 7, 2)), 2,
   'two-marker arithmetic statement reports two parameters');
mysql_recv_packet($sock);       # first parameter ColumnDefinition41
mysql_recv_packet($sock);       # second parameter ColumnDefinition41
my ($pair_col_seq, $pair_col) = mysql_recv_packet($sock);
is($pair_col_seq, 4,
   'two-marker arithmetic result metadata follows both parameter definitions');
is(mysql_column_type($pair_col), 5,
   'two-marker arithmetic result is MySQL DOUBLE metadata');
my $pair_execute = "\x17" . pack('V', $pair_stmt_id) . "\x00" . pack('V', 1)
                 . "\x00" . "\x01" . "\x05\x00\x05\x00"
                 . pack('d<', 19.0) . pack('d<', 23.0);
mysql_send_seq($sock, $pair_execute, 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($pair_row_seq, $pair_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($pair_row_seq, 3,
   'two-marker arithmetic execute returns a binary row at sequence 3');
ok(abs(unpack('d<', substr($pair_row, 2, 8)) - 42.0) < 0.000001,
   'two-marker arithmetic executes as MySQL DOUBLE');

# COM_STMT_EXECUTE encodes parameter values in MySQL binary format, not as
# substituted SQL.  Execute once with an explicit LONG type and once with
# new_params_bound_flag=0 to verify per-statement type reuse.
my $execute = "\x17" . pack('V', $prepared_id) . "\x00" . pack('V', 1)
            . "\x00" . "\x01" . "\x03\x00" . pack('V', 41);
mysql_send_seq($sock, $execute, 0);
my ($execute_hdr_seq, $execute_hdr) = mysql_recv_packet($sock);
my ($execute_col_seq, $execute_col) = mysql_recv_packet($sock);
my ($execute_row_seq, $execute_row) = mysql_recv_packet($sock);
my ($execute_end_seq, $execute_end) = mysql_recv_packet($sock);
is_deeply([$execute_hdr_seq, $execute_col_seq, $execute_row_seq, $execute_end_seq],
          [1, 2, 3, 4], 'COM_STMT_EXECUTE responses use contiguous sequences');
is(ord(substr($execute_hdr, -1, 1)), 1,
   'COM_STMT_EXECUTE returns one result column');
is(ord(substr($execute_row, 0, 1)), 0,
   'COM_STMT_EXECUTE result uses a binary-row marker');
is(unpack('V', substr($execute_row, 2, 4)), 42,
   'COM_STMT_EXECUTE decodes a binary LONG parameter and returns binary LONG');
is(ord(substr($execute_end, 0, 1)), 0xFE,
   'COM_STMT_EXECUTE sends a single DEPRECATE_EOF completion packet');

my $execute_reuse = "\x17" . pack('V', $prepared_id) . "\x00" . pack('V', 1)
                  . "\x00" . "\x00" . pack('V', 42);
mysql_send_seq($sock, $execute_reuse, 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($reuse_row_seq, $reuse_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($reuse_row_seq, 3, 'type-reuse execute row retains result sequence');
is(unpack('V', substr($reuse_row, 2, 4)), 43,
   'COM_STMT_EXECUTE reuses the prior parameter type pair');

my $execute_null = "\x17" . pack('V', $prepared_id) . "\x00" . pack('V', 1)
                 . "\x01" . "\x00";
mysql_send_seq($sock, $execute_null, 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($null_row_seq, $null_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($null_row_seq, 3, 'NULL execute row retains binary result sequence');
is(ord(substr($null_row, 0, 1)), 0, 'NULL execute row uses binary-row marker');
is(ord(substr($null_row, 1, 1)), 0x04,
   'binary result NULL bitmap uses the required two-bit offset');

# ENUM, SET, and GEOMETRY parameters use the same length-encoded wire payload
# as strings.  Their target here is an inferred integer, which makes the
# decoded payload observable without claiming that arbitrary GIS values are
# text-equivalent.
for my $tag_case ([247, 45, 'ENUM'], [248, 46, 'SET'], [255, 47, 'GEOMETRY']) {
    my ($type, $value, $label) = @$tag_case;
    my $execute_tagged = "\x17" . pack('V', $prepared_id) . "\x00" . pack('V', 1)
                       . "\x00" . "\x01" . pack('CC', $type, 0)
                       . pack('C', length($value)) . $value;
    mysql_send_seq($sock, $execute_tagged, 0);
    mysql_recv_packet($sock);       # result-set header
    mysql_recv_packet($sock);       # ColumnDefinition41
    my (undef, $tagged_row) = mysql_recv_packet($sock);
    mysql_recv_packet($sock);       # final EOF/OK
    is(unpack('V', substr($tagged_row, 2, 4)), $value + 1,
       "COM_STMT_EXECUTE decodes length-encoded $label parameters");
}

# Connector/C 8.4.10 rejects MYSQL_TYPE_BIT as an input bind type; the server
# likewise returns ER_WRONG_ARGUMENTS rather than treating its raw bytes as a
# textual parameter.  Keep that protocol behavior explicit.
my $execute_bit = "\x17" . pack('V', $prepared_id) . "\x00" . pack('V', 1)
                . "\x00" . "\x01" . "\x10\x00" . "\x01\x0f";
mysql_send_seq($sock, $execute_bit, 0);
my ($bit_param_seq, $bit_param_err) = mysql_recv_packet($sock);
is($bit_param_seq, 1, 'unsupported MYSQL_TYPE_BIT parameter uses error sequence 1');
is(mysql_error_code($bit_param_err), 1210,
   'unsupported MYSQL_TYPE_BIT parameter is ER_WRONG_ARGUMENTS like MySQL 8.4');

# RESET is an acknowledged command and must leave the statement usable with
# a newly supplied type/value pair.
mysql_send_seq($sock, "\x1A" . pack('V', $prepared_id), 0);
my ($reset_seq, $reset_ok) = mysql_recv_packet($sock);
is($reset_seq, 1, 'COM_STMT_RESET OK uses sequence 1');
is(ord(substr($reset_ok, 0, 1)), 0x00, 'COM_STMT_RESET returns OK');
my $execute_after_reset = "\x17" . pack('V', $prepared_id) . "\x00" . pack('V', 1)
                        . "\x00" . "\x01" . "\x03\x00" . pack('V', 41);
mysql_send_seq($sock, $execute_after_reset, 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($after_reset_row_seq, $after_reset_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($after_reset_row_seq, 3, 'COM_STMT_RESET preserves normal execute sequencing');
is(unpack('V', substr($after_reset_row, 2, 4)), 42,
   'COM_STMT_RESET leaves a statement executable with new bindings');

# A prepared-statement protocol error is recoverable.  Make this request an
# exact 0xffffff-byte logical message, which produces a full packet followed
# by its required empty terminator.  The ERR must continue at sequence 2,
# proving the server reassembled all input fragments before dispatch.
my $unknown_execute = "\x17" . pack('V', 0x7fffffff) .
    ("\x00" x ($max_mysql_payload - 5));
is(length($unknown_execute), $max_mysql_payload,
   'fragmented COM_STMT_EXECUTE has an exact maximum MySQL payload length');
mysql_send_seq($sock, $unknown_execute, 0);
my ($unknown_seq, $unknown_err) = mysql_recv_packet($sock);
is($unknown_seq, 2, 'fragmented unknown COM_STMT_EXECUTE error uses sequence 2');
is(mysql_error_code($unknown_err), 1243,
   'unknown COM_STMT_EXECUTE is ER_UNKNOWN_STMT_HANDLER');
mysql_send_seq($sock, "\x0E", 0);
my ($after_unknown_seq, $after_unknown_ping) = mysql_recv_packet($sock);
is($after_unknown_seq, 1, 'packet sequence resets after prepared-statement error');
is(ord(substr($after_unknown_ping, 0, 1)), 0x00,
   'COM_PING succeeds after unknown prepared statement error');

# COM_STMT_FETCH without a cursor is a recoverable MySQL-specific error.
mysql_send_seq($sock, "\x1C" . pack('V', $prepared_id) . pack('V', 1), 0);
my ($fetch_seq, $fetch_err) = mysql_recv_packet($sock);
is($fetch_seq, 1, 'cursorless COM_STMT_FETCH uses error sequence 1');
is(mysql_error_code($fetch_err), 1325,
   'cursorless COM_STMT_FETCH is ER_STMT_HAS_NO_OPEN_CURSOR');
mysql_send_seq($sock, "\x0E", 0);
my ($after_fetch_seq, $after_fetch_ping) = mysql_recv_packet($sock);
is($after_fetch_seq, 1, 'packet sequence resets after COM_STMT_FETCH error');
is(ord(substr($after_fetch_ping, 0, 1)), 0x00,
   'COM_PING succeeds after cursorless COM_STMT_FETCH');

# Server-side cursors are materialized by COM_STMT_EXECUTE.  The execute
# response contains only metadata and a CURSOR_EXISTS terminator; FETCH then
# returns binary rows without repeating the result metadata.
mysql_send_seq($sock, "\x16SELECT 1 AS cursor_v UNION ALL SELECT 2 UNION ALL SELECT 3", 0);
my ($cursor_prepare_seq, $cursor_prepare_ok) = mysql_recv_packet($sock);
is($cursor_prepare_seq, 1, 'cursor COM_STMT_PREPARE OK uses sequence 1');
my $cursor_stmt_id = unpack('V', substr($cursor_prepare_ok, 1, 4));
is(unpack('v', substr($cursor_prepare_ok, 5, 2)), 1,
   'cursor statement exposes one result column');
is(unpack('v', substr($cursor_prepare_ok, 7, 2)), 0,
   'cursor statement has no parameters');
my ($cursor_prepare_col_seq, $cursor_prepare_col) = mysql_recv_packet($sock);
is($cursor_prepare_col_seq, 2,
   'cursor prepare result metadata follows its prepare OK');
is(mysql_column_name($cursor_prepare_col), 'cursor_v',
   'cursor prepare preserves result-column metadata');

mysql_send_seq($sock, "\x17" . pack('V', $cursor_stmt_id) . "\x01" . pack('V', 1), 0);
my ($cursor_execute_hdr_seq, $cursor_execute_hdr) = mysql_recv_packet($sock);
my ($cursor_execute_col_seq, $cursor_execute_col) = mysql_recv_packet($sock);
my ($cursor_execute_end_seq, $cursor_execute_end) = mysql_recv_packet($sock);
is_deeply([$cursor_execute_hdr_seq, $cursor_execute_col_seq, $cursor_execute_end_seq],
          [1, 2, 3], 'cursor execute emits metadata and exactly one terminator');
is(ord(substr($cursor_execute_hdr, -1, 1)), 1,
   'cursor execute result header advertises one column');
is(mysql_column_name($cursor_execute_col), 'cursor_v',
   'cursor execute sends ColumnDefinition41 before opening cursor');
is(mysql_result_status($cursor_execute_end), 0x0042,
   'cursor execute terminator advertises AUTOCOMMIT and CURSOR_EXISTS');

mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 2), 0);
my ($cursor_fetch_one_seq, $cursor_fetch_one) = mysql_recv_packet($sock);
my ($cursor_fetch_two_seq, $cursor_fetch_two) = mysql_recv_packet($sock);
my ($cursor_fetch_end_seq, $cursor_fetch_end) = mysql_recv_packet($sock);
is_deeply([$cursor_fetch_one_seq, $cursor_fetch_two_seq, $cursor_fetch_end_seq],
          [1, 2, 3], 'cursor fetch emits two binary rows then one terminator');
is_deeply([unpack('V', substr($cursor_fetch_one, 2, 4)),
           unpack('V', substr($cursor_fetch_two, 2, 4))], [1, 2],
          'cursor fetch advances through binary rows in order');
is(mysql_result_status($cursor_fetch_end), 0x0042,
   'fetch that exactly fills its request keeps CURSOR_EXISTS set');

# A zero-row FETCH must not advance the cursor.
mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 0), 0);
my ($cursor_zero_seq, $cursor_zero_end) = mysql_recv_packet($sock);
is($cursor_zero_seq, 1, 'zero-row cursor fetch has only a terminator');
is(mysql_result_status($cursor_zero_end), 0x0042,
   'zero-row cursor fetch does not advance or close the cursor');

mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 1), 0);
my ($cursor_last_row_seq, $cursor_last_row) = mysql_recv_packet($sock);
my ($cursor_last_end_seq, $cursor_last_end) = mysql_recv_packet($sock);
is_deeply([$cursor_last_row_seq, $cursor_last_end_seq], [1, 2],
          'cursor fetch of final requested row remains an open cursor');
is(unpack('V', substr($cursor_last_row, 2, 4)), 3,
   'zero-row fetch left the third row available');
is(mysql_result_status($cursor_last_end), 0x0042,
   'exact final-row fetch still reports CURSOR_EXISTS per MySQL semantics');

mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 1), 0);
my ($cursor_eof_seq, $cursor_eof_end) = mysql_recv_packet($sock);
is($cursor_eof_seq, 1, 'post-end cursor fetch has only the final terminator');
is(mysql_result_status($cursor_eof_end), 0x0082,
   'post-end cursor fetch reports AUTOCOMMIT and LAST_ROW_SENT');
mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 1), 0);
my ($cursor_closed_seq, $cursor_closed_err) = mysql_recv_packet($sock);
is($cursor_closed_seq, 1, 'closed cursor fetch starts a fresh error sequence');
is(mysql_error_code($cursor_closed_err), 1325,
   'cursor closes after LAST_ROW_SENT');

# An open materialized cursor is deliberately independent of the creating
# explicit transaction.  The execute result carries IN_TRANS; after rollback
# the cursor remains readable and reports the new transaction status.
mysql_send_seq($sock, "\x03BEGIN", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x17" . pack('V', $cursor_stmt_id) . "\x01" . pack('V', 1), 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($txn_cursor_exec_end_seq, $txn_cursor_exec_end) = mysql_recv_packet($sock);
is($txn_cursor_exec_end_seq, 3, 'transactional cursor execute terminates at sequence 3');
is(mysql_result_status($txn_cursor_exec_end), 0x0043,
   'transactional cursor execute adds IN_TRANS to CURSOR_EXISTS');
mysql_send_seq($sock, "\x03ROLLBACK", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 1), 0);
my ($rollback_cursor_row_seq, $rollback_cursor_row) = mysql_recv_packet($sock);
my ($rollback_cursor_end_seq, $rollback_cursor_end) = mysql_recv_packet($sock);
is_deeply([$rollback_cursor_row_seq, $rollback_cursor_end_seq], [1, 2],
          'cursor remains fetchable after rollback');
is(unpack('V', substr($rollback_cursor_row, 2, 4)), 1,
   'rollback preserves the cursor materialized at execute');
is(mysql_result_status($rollback_cursor_end), 0x0042,
   'post-rollback cursor fetch no longer carries IN_TRANS');
mysql_send_seq($sock, "\x1A" . pack('V', $cursor_stmt_id), 0);
my ($cursor_reset_seq, $cursor_reset_ok) = mysql_recv_packet($sock);
is($cursor_reset_seq, 1, 'cursor COM_STMT_RESET starts a new response sequence');
is(ord(substr($cursor_reset_ok, 0, 1)), 0x00,
   'cursor COM_STMT_RESET closes an open cursor and returns OK');
mysql_send_seq($sock, "\x1C" . pack('V', $cursor_stmt_id) . pack('V', 1), 0);
my ($reset_cursor_fetch_seq, $reset_cursor_fetch_err) = mysql_recv_packet($sock);
is($reset_cursor_fetch_seq, 1, 'reset cursor fetch starts a new error sequence');
is(mysql_error_code($reset_cursor_fetch_err), 1325,
   'COM_STMT_RESET closes the cursor');

# MySQL materializes the cursor during EXECUTE, including rows visible only
# inside the creating transaction.  A later ROLLBACK must not discard that
# session-owned cursor snapshot.
mysql_send_seq($sock, "\x03CREATE TABLE mysql_cursor_txn_wire (v INT)", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x03INSERT INTO mysql_cursor_txn_wire VALUES (1), (2)", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x16SELECT v FROM mysql_cursor_txn_wire ORDER BY v", 0);
my ($txn_prepare_seq, $txn_prepare_ok) = mysql_recv_packet($sock);
is($txn_prepare_seq, 1, 'transactional cursor fixture prepares at sequence 1');
my $txn_cursor_stmt_id = unpack('V', substr($txn_prepare_ok, 1, 4));
mysql_recv_packet($sock);       # result ColumnDefinition41
mysql_send_seq($sock, "\x03BEGIN", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x03INSERT INTO mysql_cursor_txn_wire VALUES (99)", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x17" . pack('V', $txn_cursor_stmt_id) . "\x01" . pack('V', 1), 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($txn_snapshot_exec_seq, $txn_snapshot_exec_end) = mysql_recv_packet($sock);
is($txn_snapshot_exec_seq, 3,
   'transactional snapshot cursor execute terminates at sequence 3');
is(mysql_result_status($txn_snapshot_exec_end), 0x0043,
   'transactional snapshot execute advertises IN_TRANS and CURSOR_EXISTS');
mysql_send_seq($sock, "\x03ROLLBACK", 0);
mysql_recv_packet($sock);
mysql_send_seq($sock, "\x1C" . pack('V', $txn_cursor_stmt_id) . pack('V', 3), 0);
my ($txn_snapshot_row_one_seq, $txn_snapshot_row_one) = mysql_recv_packet($sock);
my ($txn_snapshot_row_two_seq, $txn_snapshot_row_two) = mysql_recv_packet($sock);
my ($txn_snapshot_row_three_seq, $txn_snapshot_row_three) = mysql_recv_packet($sock);
my ($txn_snapshot_end_seq, $txn_snapshot_end) = mysql_recv_packet($sock);
is_deeply([$txn_snapshot_row_one_seq, $txn_snapshot_row_two_seq,
           $txn_snapshot_row_three_seq, $txn_snapshot_end_seq], [1, 2, 3, 4],
          'rollback snapshot cursor returns three binary rows then a terminator');
is_deeply([unpack('V', substr($txn_snapshot_row_one, 2, 4)),
           unpack('V', substr($txn_snapshot_row_two, 2, 4)),
           unpack('V', substr($txn_snapshot_row_three, 2, 4))], [1, 2, 99],
          'rollback retains the rows materialized before transaction abort');
is(mysql_result_status($txn_snapshot_end), 0x0042,
   'rollback snapshot cursor is now outside the transaction and remains open');
mysql_send_seq($sock, "\x1C" . pack('V', $txn_cursor_stmt_id) . pack('V', 1), 0);
my ($txn_snapshot_eof_seq, $txn_snapshot_eof) = mysql_recv_packet($sock);
is($txn_snapshot_eof_seq, 1,
   'post-snapshot EOF fetch has only a terminator');
is(mysql_result_status($txn_snapshot_eof), 0x0082,
   'post-snapshot EOF fetch closes the cursor with LAST_ROW_SENT');

# COM_STMT_SEND_LONG_DATA is silent and may arrive in multiple fragments.
# The subsequent execute packet supplies the type pair but omits this
# parameter's inline value entirely.
mysql_send_seq($sock, "\x16SELECT CONCAT(?, '')", 0);
my ($long_prepare_seq, $long_prepare_ok) = mysql_recv_packet($sock);
my $long_stmt_id = unpack('V', substr($long_prepare_ok, 1, 4));
is($long_prepare_seq, 1, 'long-data COM_STMT_PREPARE uses sequence 1');
is(unpack('v', substr($long_prepare_ok, 7, 2)), 1,
   'long-data statement has one parameter');
mysql_recv_packet($sock);       # parameter ColumnDefinition41
mysql_recv_packet($sock);       # result ColumnDefinition41
mysql_send_seq($sock, "\x18" . pack('V', $long_stmt_id) . pack('v', 0) . 'hello ', 0);
mysql_send_seq($sock, "\x18" . pack('V', $long_stmt_id) . pack('v', 0) . 'world', 0);
my $execute_long_data = "\x17" . pack('V', $long_stmt_id) . "\x00" . pack('V', 1)
                      . "\x00" . "\x01" . "\xFD\x00";
mysql_send_seq($sock, $execute_long_data, 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($long_row_seq, $long_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($long_row_seq, 3, 'long-data execute binary row uses sequence 3');
is(substr($long_row, 2), "\x0Bhello world",
   'COM_STMT_SEND_LONG_DATA fragments are consumed as one parameter value');

# Closing a statement is intentionally silent; the next command must start
# its own packet sequence at zero and receive its response at one.
mysql_send_seq($sock, "\x19" . pack('V', $prepared_id), 0);
mysql_send_seq($sock, "\x19" . pack('V', $pair_stmt_id), 0);
mysql_send_seq($sock, "\x19" . pack('V', $long_stmt_id), 0);
mysql_send_seq($sock, "\x19" . pack('V', $cursor_stmt_id), 0);
mysql_send_seq($sock, "\x19" . pack('V', $txn_cursor_stmt_id), 0);
mysql_send_seq($sock, "\x0E", 0);
my ($post_close_ping_seq, $post_close_ping) = mysql_recv_packet($sock);
is($post_close_ping_seq, 1, 'COM_STMT_CLOSE resets packet sequence silently');
is(ord(substr($post_close_ping, 0, 1)), 0x00,
   'COM_PING succeeds after COM_STMT_CLOSE');

# The MySQL utility parser must link to the real PG18 DDL helpers, not the
# former parser-library stubs.  AUTO_INCREMENT exercises sequence discovery
# and generated trigger names across CREATE and INSERT.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_auto_increment_wire (id INT AUTO_INCREMENT PRIMARY KEY, v INT)",
    0);
my ($auto_create_seq, $auto_create_ok) = mysql_recv_packet($sock);
is($auto_create_seq, 1, 'AUTO_INCREMENT CREATE TABLE uses response sequence 1');
is(ord(substr($auto_create_ok, 0, 1)), 0x00,
   'AUTO_INCREMENT CREATE TABLE completes successfully');
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_auto_increment_wire (v) VALUES (10), (20)", 0);
my ($auto_insert_seq, $auto_insert_ok) = mysql_recv_packet($sock);
is($auto_insert_seq, 1, 'AUTO_INCREMENT INSERT uses response sequence 1');
is(ord(substr($auto_insert_ok, 0, 1)), 0x00,
   'AUTO_INCREMENT INSERT completes successfully');
mysql_send_seq($sock,
    "\x03SELECT max(id) AS max_id FROM mysql_auto_increment_wire", 0);
mysql_recv_packet($sock);       # result-set header
my ($auto_column_seq, $auto_column) = mysql_recv_packet($sock);
my ($auto_row_seq, $auto_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($auto_column_seq, 2, 'AUTO_INCREMENT query sends ColumnDefinition41');
is(mysql_column_name($auto_column), 'max_id',
   'AUTO_INCREMENT query preserves an explicit result alias');
is($auto_row_seq, 3, 'AUTO_INCREMENT query row uses sequence 3');
is(substr($auto_row, 1), '2',
   'AUTO_INCREMENT generated consecutive primary keys');

# MySQL ENUM uses a private schema-local backing domain.  The public wire
# contract must nevertheless retain MySQL's ENUM field type.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_enum_wire (e ENUM('small', 'large') NOT NULL)",
    0);
my ($enum_create_seq, $enum_create_ok) = mysql_recv_packet($sock);
is($enum_create_seq, 1, 'ENUM CREATE TABLE uses response sequence 1');
is(ord(substr($enum_create_ok, 0, 1)), 0x00,
   'ENUM CREATE TABLE completes successfully');
mysql_send_seq($sock, "\x03INSERT INTO mysql_enum_wire VALUES ('small')", 0);
my ($enum_insert_seq, $enum_insert_ok) = mysql_recv_packet($sock);
is($enum_insert_seq, 1, 'ENUM INSERT uses response sequence 1');
is(ord(substr($enum_insert_ok, 0, 1)), 0x00,
   'ENUM accepts a declared label');
mysql_send_seq($sock, "\x03SELECT e FROM mysql_enum_wire", 0);
mysql_recv_packet($sock);       # result-set header
my ($enum_column_seq, $enum_column) = mysql_recv_packet($sock);
my ($enum_row_seq, $enum_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($enum_column_seq, 2, 'ENUM query sends ColumnDefinition41');
is(mysql_column_type($enum_column), 247,
   'ENUM query exposes MYSQL_TYPE_ENUM metadata');
is($enum_row_seq, 3, 'ENUM query row uses sequence 3');
is(substr($enum_row, 1), 'small', 'ENUM round-trips its declared label');
mysql_send_seq($sock, "\x04mysql_enum_wire\0", 0);
my ($enum_field_seq, $enum_field) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # COM_FIELD_LIST terminator
is($enum_field_seq, 1, 'ENUM COM_FIELD_LIST starts at sequence 1');
is(mysql_column_type($enum_field), 247,
   'ENUM COM_FIELD_LIST exposes MYSQL_TYPE_ENUM metadata');
mysql_send_seq($sock, "\x03INSERT INTO mysql_enum_wire VALUES ('invalid')", 0);
my ($enum_bad_seq, $enum_bad) = mysql_recv_packet($sock);
is($enum_bad_seq, 1, 'invalid ENUM insert uses error sequence 1');
is(ord(substr($enum_bad, 0, 1)), 0xFF,
   'invalid ENUM insert is rejected by the generated PostgreSQL enum');
is(mysql_error_code($enum_bad), 1265,
   'invalid ENUM insert is ER_DATA_TRUNCATED like strict MySQL 8.4');
mysql_send_seq($sock, "\x0E", 0);
my ($after_enum_error_seq, $after_enum_error_ping) = mysql_recv_packet($sock);
is($after_enum_error_seq, 1, 'packet sequence resets after ENUM error');
is(ord(substr($after_enum_error_ping, 0, 1)), 0x00,
   'COM_PING succeeds after ENUM error');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_enum_duplicate_wire (e ENUM('small', 'small'))", 0);
my ($enum_duplicate_seq, $enum_duplicate) = mysql_recv_packet($sock);
is($enum_duplicate_seq, 1, 'duplicate ENUM DDL uses error sequence 1');
is(ord(substr($enum_duplicate, 0, 1)), 0xFF,
   'duplicate ENUM DDL is rejected before creating a PostgreSQL type');
is(mysql_error_code($enum_duplicate), 1291,
   'duplicate ENUM DDL is ER_DUPLICATED_VALUE_IN_TYPE like MySQL 8.4');

# MySQL MODIFY replaces the allowed ENUM labels while retaining values that
# remain declared.  This passes through the custom ALTER TABLE preparation
# path, including private type lifecycle and an on-disk table rewrite.
mysql_send_seq($sock,
    "\x03ALTER TABLE mysql_enum_wire MODIFY COLUMN e ENUM('small', 'large', 'huge') NOT NULL", 0);
my ($enum_modify_seq, $enum_modify) = mysql_recv_packet($sock);
is($enum_modify_seq, 1, 'ENUM MODIFY uses response sequence 1');
is(ord(substr($enum_modify, 0, 1)), 0x00,
   'ENUM MODIFY replaces the declared labels');
mysql_send_seq($sock, "\x03INSERT INTO mysql_enum_wire VALUES ('huge')", 0);
my ($enum_modified_insert_seq, $enum_modified_insert) = mysql_recv_packet($sock);
is($enum_modified_insert_seq, 1, 'modified ENUM insert uses response sequence 1');
is(ord(substr($enum_modified_insert, 0, 1)), 0x00,
   'ENUM MODIFY accepts the newly declared label');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM mysql_enum_wire WHERE e IN ('small', 'huge')"),
   '2', 'ENUM MODIFY retains existing compatible values and stores new labels');
mysql_send_seq($sock,
    "\x03ALTER TABLE mysql_enum_wire CHANGE COLUMN e grade ENUM('small', 'large', 'huge') NOT NULL", 0);
my ($enum_change_seq, $enum_change) = mysql_recv_packet($sock);
is($enum_change_seq, 1, 'ENUM CHANGE uses response sequence 1');
is(ord(substr($enum_change, 0, 1)), 0x00,
   'ENUM CHANGE renames the column while retaining its labels');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM mysql_enum_wire WHERE grade IN ('small', 'huge')"),
   '2', 'ENUM CHANGE retains compatible stored values under the new column name');
mysql_send_seq($sock,
    "\x03ALTER TABLE mysql_enum_wire MODIFY COLUMN grade ENUM('small', 'large', 'huge', 'giant') NOT NULL", 0);
my ($enum_renamed_modify_seq, $enum_renamed_modify) = mysql_recv_packet($sock);
is($enum_renamed_modify_seq, 1, 'renamed ENUM MODIFY uses response sequence 1');
is(ord(substr($enum_renamed_modify, 0, 1)), 0x00,
   'renamed ENUM MODIFY refreshes its normalizer labels');
mysql_send_seq($sock, "\x03INSERT INTO mysql_enum_wire VALUES ('giant')", 0);
my ($enum_renamed_modify_insert_seq, $enum_renamed_modify_insert) = mysql_recv_packet($sock);
is($enum_renamed_modify_insert_seq, 1,
   'renamed ENUM expanded-label INSERT uses response sequence 1');
is(ord(substr($enum_renamed_modify_insert, 0, 1)), 0x00,
   'renamed ENUM MODIFY accepts newly declared labels');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM mysql_enum_wire WHERE grade = 'giant'"),
   '1', 'renamed ENUM MODIFY keeps its trigger bound to the refreshed function');

# SET is represented by a generated text domain, table check, and normalizing
# trigger.  Verify that it accepts declared members, rejects an undeclared
# member, and leaves the MySQL socket usable.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_set_wire (s SET('red', 'blue') NOT NULL)", 0);
my ($set_create_seq, $set_create_ok) = mysql_recv_packet($sock);
is($set_create_seq, 1, 'SET CREATE TABLE uses response sequence 1');
is(ord(substr($set_create_ok, 0, 1)), 0x00,
   'SET CREATE TABLE completes successfully');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('red,blue')", 0);
my ($set_insert_seq, $set_insert_ok) = mysql_recv_packet($sock);
is($set_insert_seq, 1, 'SET INSERT uses response sequence 1');
is(ord(substr($set_insert_ok, 0, 1)), 0x00,
   'SET accepts declared members');
mysql_send_seq($sock, "\x03SELECT s FROM mysql_set_wire", 0);
mysql_recv_packet($sock);       # result-set header
my ($set_column_seq, $set_column) = mysql_recv_packet($sock);
my ($set_row_seq, $set_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($set_column_seq, 2, 'SET query sends ColumnDefinition41');
is(mysql_column_type($set_column), 248,
   'SET query exposes MYSQL_TYPE_SET metadata');
is($set_row_seq, 3, 'SET query row uses sequence 3');
is(substr($set_row, 1), 'red,blue', 'SET round-trips declared members');
mysql_send_seq($sock, "\x04mysql_set_wire\0", 0);
my ($set_field_seq, $set_field) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # COM_FIELD_LIST terminator
is($set_field_seq, 1, 'SET COM_FIELD_LIST starts at sequence 1');
is(mysql_column_type($set_field), 248,
   'SET COM_FIELD_LIST exposes MYSQL_TYPE_SET metadata');

# Prepared-statement result metadata is emitted by a separate protocol path.
mysql_send_seq($sock, "\x16SELECT grade FROM mysql_enum_wire", 0);
my ($enum_prepare_seq, $enum_prepare_ok) = mysql_recv_packet($sock);
my ($enum_prepare_col_seq, $enum_prepare_col) = mysql_recv_packet($sock);
is($enum_prepare_seq, 1, 'ENUM COM_STMT_PREPARE starts at sequence 1');
is(unpack('v', substr($enum_prepare_ok, 5, 2)), 1,
   'ENUM COM_STMT_PREPARE has one result column');
is($enum_prepare_col_seq, 2,
   'ENUM COM_STMT_PREPARE result metadata follows its OK packet');
is(mysql_column_type($enum_prepare_col), 247,
   'ENUM COM_STMT_PREPARE exposes MYSQL_TYPE_ENUM metadata');
mysql_send_seq($sock, "\x16SELECT s FROM mysql_set_wire", 0);
my ($set_prepare_seq, $set_prepare_ok) = mysql_recv_packet($sock);
my ($set_prepare_col_seq, $set_prepare_col) = mysql_recv_packet($sock);
is($set_prepare_seq, 1, 'SET COM_STMT_PREPARE starts at sequence 1');
is(unpack('v', substr($set_prepare_ok, 5, 2)), 1,
   'SET COM_STMT_PREPARE has one result column');
is($set_prepare_col_seq, 2,
   'SET COM_STMT_PREPARE result metadata follows its OK packet');
is(mysql_column_type($set_prepare_col), 248,
   'SET COM_STMT_PREPARE exposes MYSQL_TYPE_SET metadata');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('blue,red')", 0);
my ($set_reverse_seq, $set_reverse_ok) = mysql_recv_packet($sock);
is($set_reverse_seq, 1, 'out-of-order SET insert uses response sequence 1');
is(ord(substr($set_reverse_ok, 0, 1)), 0x00,
   'out-of-order SET insert is normalized rather than rejected');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('red,red')", 0);
my ($set_duplicate_seq, $set_duplicate_ok) = mysql_recv_packet($sock);
is($set_duplicate_seq, 1, 'duplicate SET insert uses response sequence 1');
is(ord(substr($set_duplicate_ok, 0, 1)), 0x00,
   'duplicate SET member is normalized rather than rejected');
mysql_send_seq($sock,
    "\x03SELECT count(*) FROM mysql_set_wire WHERE s = 'red,blue'", 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($set_normalized_count_seq, $set_normalized_count) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($set_normalized_count_seq, 3, 'normalized SET count row uses sequence 3');
is(substr($set_normalized_count, 1), '2',
   'SET values are stored in declaration order with duplicates removed');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_set_default_wire (s SET('red', 'blue') NOT NULL DEFAULT 'blue,red')", 0);
my ($set_default_create_seq, $set_default_create_ok) = mysql_recv_packet($sock);
is($set_default_create_seq, 1, 'SET default CREATE TABLE uses response sequence 1');
is(ord(substr($set_default_create_ok, 0, 1)), 0x00,
   'SET accepts an out-of-order default expression');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_default_wire VALUES (DEFAULT)", 0);
my ($set_default_insert_seq, $set_default_insert_ok) = mysql_recv_packet($sock);
is($set_default_insert_seq, 1, 'SET default INSERT uses response sequence 1');
is(ord(substr($set_default_insert_ok, 0, 1)), 0x00,
   'SET default INSERT completes successfully');
mysql_send_seq($sock, "\x03SELECT s FROM mysql_set_default_wire", 0);
mysql_recv_packet($sock);       # result-set header
mysql_recv_packet($sock);       # ColumnDefinition41
my ($set_default_row_seq, $set_default_row) = mysql_recv_packet($sock);
mysql_recv_packet($sock);       # final EOF/OK
is($set_default_row_seq, 3, 'SET default query row uses sequence 3');
is(substr($set_default_row, 1), 'red,blue',
   'SET default is normalized to declaration order');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('red,green')", 0);
my ($set_bad_seq, $set_bad) = mysql_recv_packet($sock);
is($set_bad_seq, 1, 'invalid SET insert uses error sequence 1');
is(ord(substr($set_bad, 0, 1)), 0xFF,
   'invalid SET insert is rejected by the generated SET validation');
is(mysql_error_code($set_bad), 1265,
   'invalid SET insert is ER_DATA_TRUNCATED like strict MySQL 8.4');
mysql_send_seq($sock, "\x0E", 0);
my ($after_set_error_seq, $after_set_error_ping) = mysql_recv_packet($sock);
is($after_set_error_seq, 1, 'packet sequence resets after SET error');
is(ord(substr($after_set_error_ping, 0, 1)), 0x00,
   'COM_PING succeeds after SET error');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_set_duplicate_wire (s SET('red', 'red'))", 0);
my ($set_duplicate_ddl_seq, $set_duplicate_ddl) = mysql_recv_packet($sock);
is($set_duplicate_ddl_seq, 1, 'duplicate SET DDL uses error sequence 1');
is(ord(substr($set_duplicate_ddl, 0, 1)), 0xFF,
   'duplicate SET DDL is rejected before creating generated objects');
is(mysql_error_code($set_duplicate_ddl), 1291,
   'duplicate SET DDL is ER_DUPLICATED_VALUE_IN_TYPE like MySQL 8.4');

mysql_send_seq($sock,
    "\x03ALTER TABLE mysql_set_wire MODIFY COLUMN s SET('red', 'blue', 'green') NOT NULL", 0);
my ($set_modify_seq, $set_modify) = mysql_recv_packet($sock);
is($set_modify_seq, 1, 'SET MODIFY uses response sequence 1');
is(ord(substr($set_modify, 0, 1)), 0x00,
   'SET MODIFY replaces the declared members');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('green,red')", 0);
my ($set_modified_insert_seq, $set_modified_insert) = mysql_recv_packet($sock);
is($set_modified_insert_seq, 1, 'modified SET insert uses response sequence 1');
is(ord(substr($set_modified_insert, 0, 1)), 0x00,
   'SET MODIFY accepts and normalizes newly declared members');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM mysql_set_wire WHERE s = 'red,green'"),
   '1', 'SET MODIFY keeps the normalizing trigger after member replacement');
mysql_send_seq($sock,
    "\x03ALTER TABLE mysql_set_wire CHANGE COLUMN s shade SET('red', 'blue', 'green') NOT NULL", 0);
my ($set_change_seq, $set_change) = mysql_recv_packet($sock);
is($set_change_seq, 1, 'SET CHANGE uses response sequence 1');
is(ord(substr($set_change, 0, 1)), 0x00,
   'SET CHANGE renames the column while retaining its members');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('green,red')", 0);
my ($set_changed_insert_seq, $set_changed_insert) = mysql_recv_packet($sock);
is($set_changed_insert_seq, 1, 'renamed SET insert uses response sequence 1');
is(ord(substr($set_changed_insert, 0, 1)), 0x00,
   'SET CHANGE retains a normalizing trigger under the new column name');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM mysql_set_wire WHERE shade = 'red,green'"),
   '2', 'SET CHANGE stores normalized values under the renamed column');
mysql_send_seq($sock,
    "\x03ALTER TABLE mysql_set_wire MODIFY COLUMN shade SET('red', 'blue', 'green', 'yellow') NOT NULL", 0);
my ($set_renamed_modify_seq, $set_renamed_modify) = mysql_recv_packet($sock);
is($set_renamed_modify_seq, 1, 'renamed SET MODIFY uses response sequence 1');
is(ord(substr($set_renamed_modify, 0, 1)), 0x00,
   'renamed SET MODIFY finds and refreshes its renamed internal objects');
mysql_send_seq($sock, "\x03INSERT INTO mysql_set_wire VALUES ('yellow,red')", 0);
my ($set_renamed_modify_insert_seq, $set_renamed_modify_insert) = mysql_recv_packet($sock);
is($set_renamed_modify_insert_seq, 1, 'second SET MODIFY insert uses response sequence 1');
is(ord(substr($set_renamed_modify_insert, 0, 1)), 0x00,
   'renamed SET MODIFY accepts newly declared members after CHANGE');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM mysql_set_wire WHERE shade = 'red,yellow'"),
   '1', 'renamed SET MODIFY retains the normalizing trigger lifecycle');

# MySQL resolves ENUM/SET labels through the column collation, not by exact
# PostgreSQL text equality.  These values are a compact differential matrix
# taken from a MySQL 8.4.10 server: ai_ci folds case/accent and trailing space;
# as_cs preserves case/accent but ignores a trailing space; binary preserves it.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_labels_ai_wire (e ENUM('a','b') NOT NULL, s SET('a','b') NOT NULL) COLLATE utf8mb4_0900_ai_ci", 0);
my ($labels_ai_create_seq, $labels_ai_create) = mysql_recv_packet($sock);
is($labels_ai_create_seq, 1, 'ai_ci ENUM/SET CREATE uses response sequence 1');
is(ord(substr($labels_ai_create, 0, 1)), 0x00,
   'ai_ci ENUM/SET CREATE completes successfully');
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_labels_ai_wire VALUES ('\xC3\x81 ', 'B,\xC3\xA1,A ')", 0);
my ($labels_ai_insert_seq, $labels_ai_insert) = mysql_recv_packet($sock);
is($labels_ai_insert_seq, 1, 'ai_ci folded INSERT uses response sequence 1');
is(ord(substr($labels_ai_insert, 0, 1)), 0x00,
   'ai_ci ENUM/SET accepts case/accent/trailing-space variants');
is(mysql_query_one_text($sock,
                         'SELECT concat(e,\'|\',s) FROM mysql_labels_ai_wire'),
   'a|a,b', 'ai_ci canonicalizes ENUM/SET labels to declaration spelling and order');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_labels_ai_enum_duplicate_wire (e ENUM('a','A')) COLLATE utf8mb4_0900_ai_ci", 0);
my ($labels_ai_enum_duplicate_seq, $labels_ai_enum_duplicate) = mysql_recv_packet($sock);
is($labels_ai_enum_duplicate_seq, 1, 'ai_ci duplicate ENUM DDL uses response sequence 1');
is(mysql_error_code($labels_ai_enum_duplicate), 1291,
   'ai_ci treats case-only ENUM declarations as duplicated values');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_labels_ai_set_duplicate_wire (s SET('a','\xC3\xA1')) COLLATE utf8mb4_0900_ai_ci", 0);
my ($labels_ai_set_duplicate_seq, $labels_ai_set_duplicate) = mysql_recv_packet($sock);
is($labels_ai_set_duplicate_seq, 1, 'ai_ci duplicate SET DDL uses response sequence 1');
is(mysql_error_code($labels_ai_set_duplicate), 1291,
   'ai_ci treats accent-only SET declarations as duplicated values');

mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_labels_as_cs_wire (e ENUM('a','A','\xC3\xA1') NOT NULL, s SET('a','A','\xC3\xA1','b') NOT NULL) COLLATE utf8mb4_0900_as_cs", 0);
my ($labels_as_cs_create_seq, $labels_as_cs_create) = mysql_recv_packet($sock);
is($labels_as_cs_create_seq, 1, 'as_cs ENUM/SET CREATE uses response sequence 1');
is(ord(substr($labels_as_cs_create, 0, 1)), 0x00,
   'as_cs permits case/accent-distinct declarations');
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_labels_as_cs_wire VALUES ('a ', 'b,\xC3\xA1,A,a ')", 0);
my ($labels_as_cs_insert_seq, $labels_as_cs_insert) = mysql_recv_packet($sock);
is($labels_as_cs_insert_seq, 1, 'as_cs trailing-space INSERT uses response sequence 1');
is(ord(substr($labels_as_cs_insert, 0, 1)), 0x00,
   'as_cs accepts trailing-space label lookup');
is(mysql_query_one_text($sock,
                         'SELECT concat(e,\'|\',s) FROM mysql_labels_as_cs_wire'),
   "a|a,A,\xC3\xA1,b",
   'as_cs preserves case/accent declarations while canonicalizing SET order');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_labels_as_cs_duplicate_wire (e ENUM('a','a ')) COLLATE utf8mb4_0900_as_cs", 0);
my ($labels_as_cs_duplicate_seq, $labels_as_cs_duplicate) = mysql_recv_packet($sock);
is($labels_as_cs_duplicate_seq, 1, 'as_cs trailing-space duplicate DDL uses response sequence 1');
is(mysql_error_code($labels_as_cs_duplicate), 1291,
   'as_cs treats trailing-space ENUM declarations as duplicated values');

mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_labels_binary_wire (e ENUM('a','A','\xC3\xA1','a ') NOT NULL, s SET('a','A','\xC3\xA1','a ','b') NOT NULL) COLLATE binary", 0);
my ($labels_binary_create_seq, $labels_binary_create) = mysql_recv_packet($sock);
is($labels_binary_create_seq, 1, 'binary ENUM/SET CREATE uses response sequence 1');
is(ord(substr($labels_binary_create, 0, 1)), 0x00,
   'binary permits trailing-space-distinct declarations');
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_labels_binary_wire VALUES ('a ', 'b,a ,\xC3\xA1,A,a')", 0);
my ($labels_binary_insert_seq, $labels_binary_insert) = mysql_recv_packet($sock);
is($labels_binary_insert_seq, 1, 'binary INSERT uses response sequence 1');
is(ord(substr($labels_binary_insert, 0, 1)), 0x00,
   'binary accepts trailing-space-distinct ENUM/SET members');
is(mysql_query_one_text($sock,
                         'SELECT concat(e,\'|\',s) FROM mysql_labels_binary_wire'),
   "a |a,A,\xC3\xA1,a ,b",
   'binary preserves trailing spaces while SET remains declaration ordered');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_label_column_binary_wire (e ENUM('a','a ') COLLATE binary NOT NULL)", 0);
my ($column_binary_create_seq, $column_binary_create) = mysql_recv_packet($sock);
is($column_binary_create_seq, 1,
   'column-level binary ENUM CREATE uses response sequence 1');
is(ord(substr($column_binary_create, 0, 1)), 0x00,
   'column-level unquoted binary collation is accepted');
mysql_send_seq($sock,
    "\x03INSERT INTO mysql_label_column_binary_wire VALUES ('a ')", 0);
my ($column_binary_insert_seq, $column_binary_insert) = mysql_recv_packet($sock);
is($column_binary_insert_seq, 1,
   'column-level binary trailing-space INSERT uses response sequence 1');
is(ord(substr($column_binary_insert, 0, 1)), 0x00,
   'column-level binary keeps trailing-space ENUM label distinct');
is(mysql_query_one_text($sock,
                         'SELECT e FROM mysql_label_column_binary_wire'),
   'a ', 'column-level binary ENUM preserves its trailing space');

# MySQL enum/set storage is private to its table-column pair.  Dropping that
# column must not leave an unreachable PostgreSQL type/domain behind.
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_enum_drop_wire (keep_col INT, e ENUM('low', 'high'))", 0);
my ($enum_drop_create_seq, $enum_drop_create) = mysql_recv_packet($sock);
is($enum_drop_create_seq, 1, 'ENUM drop fixture CREATE uses response sequence 1');
is(ord(substr($enum_drop_create, 0, 1)), 0x00,
   'ENUM drop fixture CREATE completes successfully');
mysql_send_seq($sock, "\x03ALTER TABLE mysql_enum_drop_wire DROP COLUMN e", 0);
my ($enum_drop_seq, $enum_drop) = mysql_recv_packet($sock);
is($enum_drop_seq, 1, 'ENUM DROP COLUMN uses response sequence 1');
is(ord(substr($enum_drop, 0, 1)), 0x00,
   'ENUM DROP COLUMN completes successfully');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM pg_catalog.pg_type WHERE typname = 'enum_mysql_enum_drop_wire_e'"),
   '0', 'ENUM DROP COLUMN removes its private backing type');
mysql_send_seq($sock,
    "\x03CREATE TABLE mysql_set_drop_wire (keep_col INT, s SET('red', 'blue'))", 0);
my ($set_drop_create_seq, $set_drop_create) = mysql_recv_packet($sock);
is($set_drop_create_seq, 1, 'SET drop fixture CREATE uses response sequence 1');
is(ord(substr($set_drop_create, 0, 1)), 0x00,
   'SET drop fixture CREATE completes successfully');
mysql_send_seq($sock, "\x03ALTER TABLE mysql_set_drop_wire DROP COLUMN s", 0);
my ($set_drop_seq, $set_drop) = mysql_recv_packet($sock);
is($set_drop_seq, 1, 'SET DROP COLUMN uses response sequence 1');
is(ord(substr($set_drop, 0, 1)), 0x00,
   'SET DROP COLUMN completes successfully');
is(mysql_query_one_text($sock,
                         "SELECT count(*) FROM pg_catalog.pg_type WHERE typname = 'set_mysql_set_drop_wire_s'"),
   '0', 'SET DROP COLUMN removes its private backing domain');

# -- COM_PING --
mysql_send_seq($sock, "\x0E", 0);
my $ping_resp = mysql_recv($sock);
is(ord(substr($ping_resp, 0, 1)), 0x00, 'COM_PING returns OK');

# -- COM_QUIT --
mysql_send_seq($sock, "\x01", 0);
$sock->close();

# mysql-connector-python exercises the client library's actual prepared
# cursor path (COM_STMT_PREPARE followed by COM_STMT_EXECUTE).  Keep this
# optional so that the server regression suite does not acquire a Python
# package dependency, while validating it whenever the connector is present.
SKIP:
{
	my $connector_probe = system('python3', '-c', 'import mysql.connector');
	skip 'mysql-connector-python is not installed', 1 if $connector_probe != 0;

	my $connector_script = join("\n",
		"import mysql.connector",
		"conn = mysql.connector.connect(host='127.0.0.1', port=$mysql_port, user='mysql_user', password='test123', database='postgres', ssl_disabled=True)",
		"cur = conn.cursor(prepared=True)",
		"cur.execute('SELECT %s + 1', (41,))",
		"assert cur.fetchone() == (42,)",
		"cur.execute('SELECT %s + %s', (19, 23))",
		"assert cur.fetchone() == (42.0,)",
		"cur.execute(\"SELECT CONCAT(%s, 'x')\", ('hello',))",
		"assert cur.fetchone() == ('hellox',)",
		"cur.execute('SELECT %s + 1.5', (2.25,))",
		"assert str(cur.fetchone()[0]) == '3.75'",
		"cur.execute('SELECT %s + 1 WHERE false', (41,))",
		"assert cur.fetchone() is None",
		"ddl = conn.cursor()",
		"ddl.execute('CREATE TEMPORARY TABLE mysql_stmt_driver (v int)')",
		"cur.execute('INSERT INTO mysql_stmt_driver VALUES (%s)', (7,))",
		"assert cur.rowcount == 1",
		"ddl.execute('SELECT v FROM mysql_stmt_driver')",
		"assert ddl.fetchone() == (7,)",
		"ddl.close()",
		"cur.close()",
		"conn.close()");
	command_ok(['python3', '-c', $connector_script],
		'mysql-connector prepared cursor executes through MySQL binary protocol');
}

# -- Authentication failures carry one correctly sequenced MySQL ERR --
my ($bad_native_sock, $bad_native_challenge) = mysql_open($mysql_port);
mysql_send_seq($bad_native_sock,
    mysql_login($bad_native_challenge, 'mysql_native_password', 'wrong', $caps), 1);
my ($bad_native_seq, $bad_native_err) = mysql_recv_packet($bad_native_sock);
is($bad_native_seq, 2, 'direct native password failure uses sequence 2');
is(mysql_error_code($bad_native_err), 1045,
   'direct native password failure is ER_ACCESS_DENIED_ERROR');
$bad_native_sock->close();

my ($bad_switch_sock, $bad_switch_challenge) = mysql_open($mysql_port);
mysql_send_seq($bad_switch_sock,
    mysql_login($bad_switch_challenge, 'caching_sha2_password', 'test123', $caps), 1);
my ($bad_switch_seq, $bad_switch_request) = mysql_recv_packet($bad_switch_sock);
is($bad_switch_seq, 2, 'wrong-password switch still starts at sequence 2');
my $bad_switch_seed = substr($bad_switch_request,
    1 + length('mysql_native_password') + 1, 20);
my $bad_switch_stage1 = sha1('wrong');
my $bad_switch_token = $bad_switch_stage1 ^
    sha1($bad_switch_seed . sha1($bad_switch_stage1));
mysql_send_seq($bad_switch_sock, $bad_switch_token, 3);
my ($bad_switch_err_seq, $bad_switch_err) = mysql_recv_packet($bad_switch_sock);
is($bad_switch_err_seq, 4, 'switch password failure uses sequence 4');
is(mysql_error_code($bad_switch_err), 1045,
   'switch password failure is ER_ACCESS_DENIED_ERROR');
$bad_switch_sock->close();

for my $bad_length (19, 21) {
    my ($length_sock, $length_challenge) = mysql_open($mysql_port);
    mysql_send_seq($length_sock,
        mysql_login($length_challenge, 'caching_sha2_password', 'test123', $caps), 1);
    my ($length_switch_seq, $length_request) = mysql_recv_packet($length_sock);
    is($length_switch_seq, 2,
       "AuthSwitchRequest before $bad_length-byte response uses sequence 2");
    mysql_send_seq($length_sock, "\x00" x $bad_length, 3);
    my ($length_err_seq, $length_err) = mysql_recv_packet($length_sock);
    is($length_err_seq, 4,
       "$bad_length-byte AuthSwitchResponse error uses sequence 4");
    is(mysql_error_code($length_err), 1043,
       "$bad_length-byte AuthSwitchResponse is ER_HANDSHAKE_ERROR");
    $length_sock->close();
}

# HBA policy is checked before an auth switch, and only md5 is accepted.
set_mysql_hba($node, "host postgres mysql_user 127.0.0.1/32 reject");
my ($reject_sock, $reject_challenge) = mysql_open($mysql_port);
mysql_send_seq($reject_sock,
    mysql_login($reject_challenge, 'caching_sha2_password', 'test123', $caps), 1);
my ($reject_seq, $reject_err) = mysql_recv_packet($reject_sock);
is($reject_seq, 2, 'HBA reject returns sequence 2 rather than AuthSwitchRequest');
is(mysql_error_code($reject_err), 1045, 'HBA reject is ER_ACCESS_DENIED_ERROR');
$reject_sock->close();

set_mysql_hba($node, "host postgres mysql_user 127.0.0.1/32 trust");
my ($trust_sock, $trust_challenge) = mysql_open($mysql_port);
mysql_send_seq($trust_sock,
    mysql_login($trust_challenge, 'mysql_native_password', 'test123', $caps), 1);
my ($trust_seq, $trust_err) = mysql_recv_packet($trust_sock);
is($trust_seq, 2, 'unsupported trust HBA method returns sequence 2');
is(mysql_error_code($trust_err), 1045,
   'unsupported trust HBA method is ER_ACCESS_DENIED_ERROR');
$trust_sock->close();

set_mysql_hba($node);
my ($implicit_sock, $implicit_challenge) = mysql_open($mysql_port);
mysql_send_seq($implicit_sock,
    mysql_login($implicit_challenge, 'mysql_native_password', 'test123', $caps), 1);
my ($implicit_seq, $implicit_err) = mysql_recv_packet($implicit_sock);
is($implicit_seq, 2, 'implicit HBA reject returns sequence 2');
is(mysql_error_code($implicit_err), 1045,
   'implicit HBA reject is ER_ACCESS_DENIED_ERROR');
$implicit_sock->close();

# Restore HBA rules before SQL_CALC_FOUND_ROWS test
set_mysql_hba($node,
    "host postgres mysql_user 127.0.0.1/32 md5",
    "host postgres mysql_admin 127.0.0.1/32 md5",
    "host all all 127.0.0.1/32 trust");

# -- SQL_CALC_FOUND_ROWS / FOUND_ROWS --
{
    my ($calc_sock, $calc_challenge) = mysql_open($mysql_port);
    mysql_send_seq($calc_sock,
        mysql_login($calc_challenge, 'mysql_native_password', 'test123', $caps), 1);
    my ($calc_auth_seq, $calc_auth_ok) = mysql_recv_packet($calc_sock);
    is($calc_auth_seq, 2, 'SQL_CALC_FOUND_ROWS fixture login seq=2');
    is(mysql_result_status($calc_auth_ok) & 0x0002, 0x0002,
       'SQL_CALC_FOUND_ROWS fixture login AUTOCOMMIT');

    # Parse SQL_CALC_FOUND_ROWS without error
    # First test: does a simple COM_PING work?
    mysql_send_seq($calc_sock, "\x0e", 0);  # COM_PING
    my ($ping_seq, $ping_resp) = mysql_recv_packet($calc_sock);
    is($ping_seq, 1, 'SQL_CALC_FOUND_ROWS pre-query COM_PING seq=1');
    is(ord(substr($ping_resp, 0, 1)), 0x00, 'SQL_CALC_FOUND_ROWS pre-query COM_PING OK');

    # Now test: SELECT 42 AS n
    mysql_send_seq($calc_sock, "\x03SELECT 42 AS n", 0);
    eval {
        my ($calc_hdr_seq, $calc_hdr) = mysql_recv_packet($calc_sock);
        is($calc_hdr_seq, 1, 'SQL_CALC_FOUND_ROWS result header seq=1');
        is(ord(substr($calc_hdr, 0, 1)), 1, 'SQL_CALC_FOUND_ROWS 1 column');

        # Consume ColumnDefinition + row + EOF
        mysql_recv_packet($calc_sock);  # ColumnDefinition
        my ($calc_row_seq, $calc_row) = mysql_recv_packet($calc_sock);
        is($calc_row_seq, 3, 'SQL_CALC_FOUND_ROWS row seq=3');
        my $calc_offset = 0;
        is(mysql_lenenc_string($calc_row, \$calc_offset), '42',
           'SQL_CALC_FOUND_ROWS returns 42');
        mysql_recv_packet($calc_sock);  # EOF

        # FOUND_ROWS() returns the row count of the last SELECT
        my $fr_text = mysql_query_one_text($calc_sock, 'SELECT FOUND_ROWS()');
        like($fr_text, qr/^\d+$/, 'FOUND_ROWS() returns a numeric value');
    };
    if ($@) {
        fail("SQL_CALC_FOUND_ROWS query: $@");
    }

    $calc_sock->close();
}

# -- INFORMATION_SCHEMA wire-protocol tests (KF-060) --
# The mys_informa_schema views are created by the aux_mysql extension,
# which requires pgcrypto.
$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS pgcrypto');
$node->safe_psql('postgres', 'CREATE EXTENSION IF NOT EXISTS aux_mysql');

# Verify that queries against information_schema.X are correctly redirected
# to mys_informa_schema.X and return valid MySQL result sets.
{
    my ($is_sock, $is_challenge) = mysql_open($mysql_port);
    mysql_send_seq($is_sock,
        mysql_login($is_challenge, 'mysql_native_password', 'test123', $caps), 1);
    my ($is_auth_seq, $is_auth_ok) = mysql_recv_packet($is_sock);
    is($is_auth_seq, 2, 'INFO_SCHEMA fixture login seq=2');
    is(mysql_result_status($is_auth_ok) & 0x0002, 0x0002,
       'INFO_SCHEMA fixture login AUTOCOMMIT');

    # Smoke: COM_PING works on this connection
    mysql_send_seq($is_sock, "\x0e", 0);
    my ($ping_seq, $ping_resp) = mysql_recv_packet($is_sock);
    is($ping_seq, 1, 'INFO_SCHEMA COM_PING seq=1');
    is(ord(substr($ping_resp, 0, 1)), 0x00, 'INFO_SCHEMA COM_PING OK');

    # Test 1: information_schema.schemata → mys_informa_schema.schemata
    my ($sc_ncol, @sc_rows) = mysql_query_select($is_sock,
        'SELECT * FROM information_schema.schemata');
    cmp_ok($sc_ncol, '==', 5, 'schemata view has 5 columns');
    cmp_ok(scalar(@sc_rows), '>=', 1, 'schemata returns at least 1 row');
    # The view lists MySQL-visible schemata (mirroring SHOW DATABASES) in
    # SCHEMA_NAME order, so information_schema sorts before mysql.
    is($sc_rows[0][1], 'information_schema',
       'schemata first row is information_schema');
    my @sc_names = map { $_->[1] } @sc_rows;
    ok((grep { $_ eq 'mysql' } @sc_names),
       'schemata includes the mysql schema');

    # Quick sanity: second query after schemata still works
    my $sanity = mysql_query_one_text($is_sock, 'SELECT 42');
    is($sanity, '42', 'query after schemata still works');

    # Test 2: Create a table and verify it appears in all four views
    mysql_query_ok($is_sock,
        'CREATE TABLE public._is_test_t (id INT PRIMARY KEY, name VARCHAR(50))');

    # 2a: tables view — verify TABLE_TYPE
    my $tv = mysql_query_one_text($is_sock,
        "SELECT TABLE_TYPE FROM information_schema.tables " .
        "WHERE TABLE_SCHEMA = 'public' AND TABLE_NAME = '_is_test_t'");
    is($tv, 'BASE TABLE', 'information_schema.tables TABLE_TYPE = BASE TABLE');

    # 2b: columns view — verify type mapping and nullability
    my ($tc_ncol, @tc_rows) = mysql_query_select($is_sock,
        'SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE FROM information_schema.columns ' .
        "WHERE TABLE_SCHEMA = 'public' AND TABLE_NAME = '_is_test_t' ORDER BY ORDINAL_POSITION");
    is($tc_ncol, 3, 'information_schema.columns returns 3 requested columns');
    is(scalar(@tc_rows), 2, 'information_schema.columns returns 2 rows for _is_test_t');
    is($tc_rows[0][0], 'id',        'columns col 0 name = id');
    is($tc_rows[0][1], 'int',       'columns maps int4 → int');
    is($tc_rows[0][2], 'NO',        'columns id IS_NULLABLE=NO');
    is($tc_rows[1][0], 'name',      'columns col 1 name = name');
    is($tc_rows[1][1], 'varchar',   'columns maps varchar');
    is($tc_rows[1][2], 'YES',       'columns name IS_NULLABLE=YES');

    # 2c: statistics view — verify live PK index metadata
    my $si = mysql_query_one_text($is_sock,
        "SELECT COLUMN_NAME FROM information_schema.statistics " .
        "WHERE TABLE_SCHEMA = 'public' AND TABLE_NAME = '_is_test_t' ORDER BY SEQ_IN_INDEX LIMIT 1");
    is($si, 'id',
       'information_schema.statistics exposes the live primary-key column');

    # Cleanup
    mysql_query_ok($is_sock, 'DROP TABLE public._is_test_t');

    $is_sock->close();
}

# -- Verify PG side still works --
my $pg_result = $node->safe_psql('postgres', "SELECT 1 AS still_works;");
$pg_result =~ s/\s+$//;
is($pg_result, "1", 'PG standard connection still works after MySQL test');

done_testing();
