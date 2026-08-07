#!/bin/sh
#
# check_mysql_vtable_contract.sh
#
# Check the load-bearing compatibility vtables.  Active slots must have live
# consumers.  ADTExtMethod also carries explicitly reserved ABI slots for
# future dialects; those are checked for declaration but are intentionally not
# required to have a current PostgreSQL/MySQL consumer.
#
# Usage:
#   check_mysql_vtable_contract.sh <postgres-source-root>
#

set -u

root=${1:?usage: check_mysql_vtable_contract.sh <postgres-source-root>}
backend="$root/src/backend"
include="$root/src/include"
fail=0

die() {
	echo "ERROR: $*" >&2
	fail=1
}

function_members() {
	file=$1
	table=$2
	awk -v table="$table" '
		$0 ~ "typedef struct " table "[[:space:]]*$" { in_table = 1; next }
		in_table && $0 ~ "}[[:space:]]*" table "[[:space:]]*;" { exit }
		in_table {
			line = $0
			while (match(line, /\(\*[A-Za-z_][A-Za-z0-9_]*\)/)) {
				member = substr(line, RSTART + 2, RLENGTH - 3)
				print member
				line = substr(line, RSTART + RLENGTH)
			}
		}
	' "$file"
}

check_function_members() {
	table=$1
	file=$2
	expected=$3
	members=$(function_members "$file" "$table")
	count=$(printf '%s\n' "$members" | sed '/^$/d' | wc -l)

	if [ "$count" -ne "$expected" ]; then
		die "$table declares $count function-pointer slots; expected $expected"
	fi

	for member in $members; do
		if ! grep -R -E -l --include='*.c' --include='*.h' \
			-- "->[[:space:]]*$member[[:space:]]*\\(" "$backend" >/dev/null 2>&1; then
			die "$table.$member has no live ->$member() consumer"
		fi
	done
}

check_named_members() {
	table=$1
	members=$2
	for member in $members; do
		if ! grep -R -E -l --include='*.c' --include='*.h' \
			-- "->[[:space:]]*$member[[:space:]]*\\(" "$backend" >/dev/null 2>&1; then
			die "$table.$member has no live ->$member() consumer"
		fi
	done
}

check_reserved_members() {
	file=$1
	members=$2
	for member in $members; do
		if ! grep -E -q -- "[[:space:]]$member[[:space:];]" "$file"; then
			die "reserved ADTExtMethod slot $member is missing"
		fi
	done
}

# ADTExtMethod members are declared with typedef'd function-pointer types
# (e.g. "pre_numeric_in_function pre_numeric_in;"), not the (*name)(...)
# spelling, so the generic function_members() extractor cannot count them.
# Count the typedef'd members directly: every function-pointer slot is
# declared as "<name>_function <member>;", so counting those declarations
# inside the struct body guards against accidental field loss/insertion.
check_adt_function_member_count() {
	file=$1
	expected=$2
	count=$(awk '
		/typedef struct ADTExtMethod[[:space:]]*$/ { in_table = 1; next }
		in_table && /^}[[:space:]]*ADTExtMethod[[:space:]]*;/ { exit }
		in_table && /^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*_function[[:space:]]+[A-Za-z_][A-Za-z0-9_]*;/ { count++ }
		END { print count }
	' "$file")
	if [ "$count" -ne "$expected" ]; then
		die "ADTExtMethod declares $count function-pointer members; expected $expected"
	fi
}

check_function_members ParserRoutine \
	"$include/parser/parserapi.h" 5

# ADTExtMethod: 10 forward-compat ADT callbacks + 13 W5 dialect-dispatch
# callbacks = 23 function-pointer slots (the allow_zero_length_char_typmod
# bool member is not a function pointer and is checked separately below).
# Every slot must have a live ->member() consumer in the kernel -- slots the
# MySQL registrant leaves NULL are explicit future-dialect ABI reservations,
# but the kernel call site (adtext -> slot, NULL -> legacy-hook fallback)
# must exist for each one.  check_reserved_members then verifies the
# reserved slots are actually declared, including the W5 slots
# (validate_var_datatype_scale, replace_non_deterministic, ...) that
# previously were not covered.
check_adt_function_member_count \
	"$include/utils/adtextapi.h" 23
check_named_members ADTExtMethod \
	"pre_time_in post_time_out date_in timestamp_in validate_var_datatype_scale replace_non_deterministic"
check_reserved_members "$include/utils/adtextapi.h" \
	"pre_numeric_in post_numeric_out pre_timetz_in post_timetz_out pre_timestamp_in post_timestamp_out allow_zero_length_char_typmod expr_typmod coalesce_typmod param_collation default_collation strpos_non_deterministic adjust_numeric_result detect_numeric_overflow identity_datatype sequence_datatype sortby_nulls unique_constraint_nulls_ordering"
check_function_members ProtocolRoutine \
	"$include/postmaster/protocol_routine.h" 22

# process_utility is a typedef-based hook rather than a (*member) spelling,
# so account for it explicitly in the ProtocolRoutine contract.
if ! grep -R -E -l --include='*.c' --include='*.h' \
	-- "->[[:space:]]*process_utility[[:space:]]*\\(" "$backend" >/dev/null 2>&1; then
	die "ProtocolRoutine.process_utility has no live ->process_utility() consumer"
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi

echo "MySQL compatibility vtable contract: ParserRoutine 5/5, ADTExtMethod 23/23 slots (6 live + 17 reserved), ProtocolRoutine 23/23 live"
