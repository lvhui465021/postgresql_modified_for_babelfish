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
PERLLIBDIR="$K/inst/lib/x86_64-linux-gnu/pgxs/src/test/perl"
export PGBIN="$K/inst/bin"
export PG_REGRESS="$K/build/src/test/regress/pg_regress"
export PATH="$PGBIN:$PATH"

failures=0

echo '=== 1. meson: PG-core suites (setup/postmaster/regress/authentication) ==='
PERL5LIB=/home/hlv/perl5/lib/perl5 meson test -C build \
  --suite setup --suite postmaster --suite regress --suite authentication || failures=$((failures+1))

echo '=== 2. prove: MySQL TAP suites (against installed extensions) ==='
cd "$K/contrib/aux_mysql/t"
PERL5LIB=/home/hlv/perl5/lib/perl5:$PERLLIBDIR prove -v \
  005_mysql_compat.pl 006_pg_dump_restore.pl 007_mysql_parallel.pl || failures=$((failures+1))
cd "$K/src/test/postmaster/t"
PERL5LIB=/home/hlv/perl5/lib/perl5:$PERLLIBDIR prove -v \
  004_mysql_protocol.pl 005_mysql_listener_stability.pl || failures=$((failures+1))

echo '=== 3. prove: TDS TAP suites (needs sqlcmd->tsql shim on PATH) ==='
if [ -d /home/hlv/openhalo-update/sqlcmd-bin ]; then
  export PATH=/home/hlv/openhalo-update/sqlcmd-bin:$PATH
  cd "$K/../babelfish_extensions/contrib/babelfishpg_tds/test"
  PERL5LIB=/home/hlv/perl5/lib/perl5:$PERLLIBDIR prove -v -I . \
    t/001_tdspasswd.pl t/003_bbfextnotloaded.pl || failures=$((failures+1))
else
  echo 'WARNING: sqlcmd shim not found; skipping TDS TAP suite'
fi

echo
if [ "$failures" -ne 0 ]; then
  echo "baseline: $failures group(s) FAILED" >&2
  exit 1
fi
echo 'baseline: ALL PASS'