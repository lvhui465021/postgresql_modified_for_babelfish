#!/bin/bash
# ---------------------------------------------------------------------------
# run-baseline.sh -- install-first baseline for the multi-protocol stack.
#
# Prerequisites: kernel installed (ninja -C build install) and ALL extensions
# installed (babelfish_extensions/build-all.sh).  PG-core suites still run
# under meson; MySQL/TDS-dependent TAP suites run via prove against the
# installed tree (the MySQL modules are no longer part of the meson build,
# see docs/MYSQL_PLUGIN_BOUNDARY.md).
# ---------------------------------------------------------------------------
set -euo pipefail

K=$(cd "$(dirname "$0")" && pwd)
cd "$K"
PREFIX="${PREFIX:-$K/inst}"
PERLLIBDIR="$PREFIX/lib/x86_64-linux-gnu/pgxs/src/test/perl"
export PGBIN="$PREFIX/bin"
export PG_REGRESS="$K/build/src/test/regress/pg_regress"
export PATH="$PGBIN:$PATH"
# Keep the local PGXS TAP modules first; allow callers to prepend their own
# local Perl libraries through the standard PERL5LIB variable instead of
# hard-coding a machine-specific path.
PERL5LIB="$PERLLIBDIR${PERL5LIB:+:$PERL5LIB}"
export PERL5LIB

failures=0

echo '=== 1. meson: PG-core suites (setup/postmaster/regress/authentication) ==='
meson test -C build \
  --suite setup --suite postmaster --suite regress --suite authentication || failures=$((failures+1))

# MySQL extension tests live in the standalone mysql_extensions repository
# (Phase 2 of the pluginization); the kernel keeps the postmaster TAP tests
# that exercise the installed modules end to end.
MYSQLEXT_DIR="${MYSQLEXT_DIR:-$K/../mysql_extensions}"
BABELFISH_EXT_DIR="${BABELFISH_EXT_DIR:-$K/../babelfish_extensions}"
SQLCMD_BIN_DIR="${SQLCMD_BIN_DIR:-$K/../sqlcmd-bin}"
SQLCMD_OPTIONS="${SQLCMD_OPTIONS:-}"

echo '=== 2. prove: MySQL TAP suites (against installed extensions) ==='
cd "$MYSQLEXT_DIR/contrib/aux_mysql/t"
prove -v \
  005_mysql_compat.pl 006_pg_dump_restore.pl 007_mysql_parallel.pl || failures=$((failures+1))
cd "$K/src/test/postmaster/t"
prove -v \
  004_mysql_protocol.pl 005_mysql_listener_stability.pl || failures=$((failures+1))

echo '=== 3. prove: TDS TAP suites (requires sqlcmd on PATH) ==='
if [ -d "$SQLCMD_BIN_DIR" ]; then
  export PATH="$SQLCMD_BIN_DIR:$PATH"
  cd "$BABELFISH_EXT_DIR/contrib/babelfishpg_tds/test"
  BABELFISH_SQLCMD_OPTIONS="$SQLCMD_OPTIONS" \
    prove -v -I . \
    t/001_tdspasswd.pl t/003_bbfextnotloaded.pl || failures=$((failures+1))
else
  echo "ERROR: sqlcmd directory not found at $SQLCMD_BIN_DIR" >&2
  failures=$((failures+1))
fi

echo
if [ "$failures" -ne 0 ]; then
  echo "baseline: $failures group(s) FAILED" >&2
  exit 1
fi
echo 'baseline: ALL PASS'
