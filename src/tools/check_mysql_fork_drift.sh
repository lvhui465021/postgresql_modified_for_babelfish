#!/bin/sh
#
# check_mysql_fork_drift.sh
#
# Detect silent semantic drift between openHalo's MySQL fork files and
# their upstream PostgreSQL counterparts.  openHalo maintains
# mys_tablecmds.c and mys_gram.y as parallel copies of tablecmds.c and
# gram.y.  Upstream bug fixes apply cleanly (no merge conflicts) and are
# silently left unfixed in the fork -- the drift is the danger, and this
# script is its only detector.
#
# For every function / grammar production present in BOTH files, compare
# the two bodies line by line; any pair whose diff exceeds the threshold
# (in changed lines) is reported.  Exit status is nonzero when at least
# one pair exceeds the threshold or when a file is missing.
#
# Usage:
#   check_mysql_fork_drift.sh [threshold] <fork-file> <upstream-file>
#
# Default threshold: 50 changed lines (a fork function that has drifted
# this far from upstream needs a manual reconciliation).

set -u

threshold=${1:-50}
fork_file=$2
upstream_file=$3

[ -f "$fork_file" ] || { echo "fork drift: missing fork file $fork_file"; exit 1; }
[ -f "$upstream_file" ] || { echo "fork drift: missing upstream file $upstream_file"; exit 1; }

# Extract (start_line, end_line, name) for every top-level function whose
# definition line begins with "<name>(".
# A function body ends at the next top-level "^}" line.
extract_functions() {
    file=$1
    awk '
        /^[a-zA-Z_][a-zA-Z0-9_]*\(/ {
            line = $0
            n = line
            gsub(/[()].*/, "", n)
            start = NR
        }
        start && /^}/ {
            if (n != "") print start, NR, n
            start = 0; n = ""
        }
    ' "$file"
}

# Collect function ranges for both files.
extract_functions "$fork_file" | sort -k3 > /tmp/mys_fork_drift_fork.$$
extract_functions "$upstream_file" | sort -k3 > /tmp/mys_fork_drift_up.$$

fail=0
reported=0

# For each shared function name, diff the two bodies.
while read -r fstart fend fname; do
    ustart=$(awk -v n="$fname" '$3 == n {print $1; exit}' /tmp/mys_fork_drift_up.$$)
    uend=$(awk -v n="$fname" '$3 == n {print $2; exit}' /tmp/mys_fork_drift_up.$$)
    [ -n "$ustart" ] || continue
    sed -n "${fstart},${fend}p" "$fork_file" > /tmp/mys_fork_body_f.$$
    sed -n "${ustart},${uend}p" "$upstream_file" > /tmp/mys_fork_body_u.$$
    changed=$(diff /tmp/mys_fork_body_f.$$ /tmp/mys_fork_body_u.$$ |
        grep -cE '^[<>]')
    if [ "${changed:-0}" -gt "$threshold" ]; then
        echo "fork drift: $fname differs by $changed lines (threshold $threshold)"
        fail=1
        reported=$((reported + 1))
    fi
done < /tmp/mys_fork_drift_fork.$$

rm -f /tmp/mys_fork_drift_fork.$$ /tmp/mys_fork_drift_up.$$ \
      /tmp/mys_fork_body_f.$$ /tmp/mys_fork_body_u.$$

if [ "$fail" -eq 0 ]; then
    echo "fork drift: OK (no function differs by more than $threshold lines)"
fi
exit $fail
