#!/usr/bin/env bash

set -uo pipefail

suite_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mysql_bin=${MYSQL_BIN:-mysql}
mysql_host=${MYSQL_HOST:-127.0.0.1}
mysql_port=${MYSQL_PORT:-3306}
mysql_user=${MYSQL_USER:-test}
mysql_database=${MYSQL_DATABASE:-unvdb_mysqldb}
test_timeout=${MYSQL_TEST_TIMEOUT:-30}
actual_dir=${MYSQL_COMPAT_ACTUAL_DIR:-"$suite_dir/actual"}
expected_dir="$suite_dir/expected"
update_expected=${MYSQL_COMPAT_UPDATE_EXPECTED:-0}

mysql_args=(
  --no-defaults
  --protocol=TCP
  --host="$mysql_host"
  --port="$mysql_port"
  --user="$mysql_user"
  --database="$mysql_database"
  --batch
  --raw
  # Keep actual/*.out in the same useful form as a PostgreSQL regression
  # transcript: every SQL statement followed by the result returned by mysql.
  # Two verbosity levels print Query OK and affected-row counts while keeping
  # batch result sets in the tab-separated form used by assertion extraction.
  --verbose
  --verbose
  --connect-timeout=5
)

positive_sql=(
  00_session.sql
  10_ddl_types.sql
  15_ddl_advanced.sql
  20_dml_query.sql
  25_query_advanced.sql
  30_functions.sql
  35_routines_triggers.sql
  40_metadata_admin.sql
  50_ctas_on_update.sql
  60_extension_catalog.sql
)

# --- Setup: ensure the test database exists ---------------------------
mkdir -p "$actual_dir"
scratch_dir=$(mktemp -d "${TMPDIR:-/tmp}/mysql-compat.XXXXXX")
trap 'rm -rf -- "$scratch_dir"' EXIT

setup_db_args=(
  --no-defaults --protocol=TCP --host="$mysql_host" --port="$mysql_port"
  --user="$mysql_user" --batch --raw --skip-column-names --connect-timeout=5
)

if ! timeout 10 stdbuf -oL -eL "$mysql_bin" "${setup_db_args[@]}" \
    --execute "CREATE DATABASE IF NOT EXISTS $mysql_database" > /dev/null 2>&1; then
  echo "FAIL: cannot create database $mysql_database" >&2
  exit 1
fi

failures=0

extract_assertions()
{
  awk '
    tolower($0) == "test_name\tpassed" { next_is_assertion = 1; next }
    next_is_assertion { print; next_is_assertion = 0 }
  ' "$1"
}

compare_output()
{
  test_name=$1
  transcript_file="$actual_dir/$test_name.out"
  normalized_file="$scratch_dir/$test_name.normalized"
  expected_file="$expected_dir/$test_name.out"

  sed -E \
    -e 's/ \([0-9]+\.[0-9]+ sec\)/ (<TIME> sec)/g' \
    -e 's/_[0-9]+_mysql/_<OID>_mysql/g' \
    "$transcript_file" > "$normalized_file"

  if [[ "$update_expected" == 1 ]]; then
    {
      printf '%s\n' '# mysql-compat full transcript'
      cat "$normalized_file"
    } > "$expected_file"
    return
  fi

  if [[ "$(head -n 1 "$expected_file")" == '# mysql-compat full transcript' ]]; then
    {
      printf '%s\n' '# mysql-compat full transcript'
      cat "$normalized_file"
    } > "$scratch_dir/$test_name.full"
    if ! diff -u "$expected_file" "$scratch_dir/$test_name.full"; then
      failures=$((failures + 1))
    fi
  elif ! diff -u "$expected_file" "$scratch_dir/$test_name.check"; then
    failures=$((failures + 1))
  fi
}

for sql_file in "${positive_sql[@]}"; do
  test_name=${sql_file%.sql}
  transcript_file="$actual_dir/$test_name.out"
  check_file="$scratch_dir/$test_name.check"

  # Strip SQL comments and blank lines to avoid MySQL 8.x protocol
  # sequence-number desync that occurs when the CLI pipelines init
  # probes (select @@version_comment, select $$) between the
  # connection handshake and the first stdin query.  Comment lines
  # are innocuous for test logic; their removal is transparent.
  filtered_sql="$scratch_dir/$test_name.filtered.sql"
  sed '/^--/d; /^[[:space:]]*$/d' "$suite_dir/$sql_file" > "$filtered_sql"

  if ! timeout "$test_timeout" stdbuf -oL -eL "$mysql_bin" "${mysql_args[@]}" \
      < "$filtered_sql" > "$transcript_file" 2>&1; then
    echo "FAIL $sql_file: mysql returned an error or timed out" >&2
    sed -n '1,120p' "$transcript_file" >&2
    failures=$((failures + 1))
    continue
  fi

  # The displayed .out is deliberately not filtered. Keep a separate,
  # machine-stable assertion file solely for expected-result comparison.
  extract_assertions "$transcript_file" > "$check_file"
  if awk -F '\t' 'NF != 2 || $2 != "1" { bad = 1 } END { exit bad ? 0 : 1 }' \
      "$check_file"; then
    echo "FAIL $sql_file: an assertion did not return 1" >&2
    awk -F '\t' 'NF != 2 || $2 != "1"' "$check_file" >&2
    failures=$((failures + 1))
  fi
  compare_output "$test_name"
done

run_forced_suite()
{
  sql_file=$1
  test_name=${sql_file%.sql}
  transcript_file="$actual_dir/$test_name.out"
  check_file="$scratch_dir/$test_name.check"

  # Same comment/blank-line filtering as the positive suite (see above).
  filtered_sql="$scratch_dir/$test_name.filtered.sql"
  sed '/^--/d; /^[[:space:]]*$/d' "$suite_dir/$sql_file" > "$filtered_sql"

  if ! timeout "$test_timeout" stdbuf -oL -eL "$mysql_bin" "${mysql_args[@]}" --force \
      < "$filtered_sql" > "$transcript_file" 2>&1; then
    echo "FAIL $sql_file: mysql returned an error or timed out" >&2
    sed -n '1,120p' "$transcript_file" >&2
    failures=$((failures + 1))
    return
  fi

  {
    sed -n -E 's/^ERROR ([0-9]+).*$/ERROR \1/p' "$transcript_file"
    extract_assertions "$transcript_file"
  } > "$check_file"
  compare_output "$test_name"
}

run_forced_suite 80_expected_errors.sql
run_forced_suite 90_known_failures.sql

# --- Teardown: drop the test database ---------------------------------
if timeout 10 stdbuf -oL -eL "$mysql_bin" "${setup_db_args[@]}" \
    --execute "DROP DATABASE IF EXISTS $mysql_database" > /dev/null 2>&1; then
  :  # database dropped successfully
else
  echo "WARNING: could not drop database $mysql_database" >&2
fi

if (( failures != 0 )); then
  echo "mysql compatibility suite: $failures failure(s)" >&2
  exit 1
fi

echo "mysql compatibility suite: PASS"
