#!/bin/sh
#
# Verify the deliberate Meson-only build boundary for MySQL compatibility.
#
# The MySQL sources are tree-internal, but their parser, type, protocol, and
# DDL components are loadable modules or Meson backend sources.  An orphaned
# parser Makefile/meson.build or a parent Makefile reference would create a
# second, incomplete build graph and silently ship a different product.
#

set -eu

root=${1:?usage: $0 POSTGRES_SOURCE_ROOT}
status=0

fail()
{
	printf '%s\n' "mysql build graph: $*" >&2
	status=1
}

for orphan in \
	src/backend/parser/mysql/Makefile \
	src/backend/parser/mysql/meson.build
do
	if test -e "$root/$orphan"; then
		fail "orphan build rule exists: $orphan"
	fi
done

require_text()
{
	file=$1
	pattern=$2
	if ! grep -Fq -- "$pattern" "$root/$file"; then
		fail "$file is missing Meson graph entry: $pattern"
	fi
}

require_text src/backend/parser/meson.build "mysql_parser = shared_module('mysql_parser'"
require_text src/backend/parser/meson.build "mys_kwlist_d = custom_target('mys_kwlist_d'"
require_text src/backend/commands/meson.build "subdir('mysql')"
require_text src/backend/adapter/meson.build "subdir('mysql')"
require_text src/backend/utils/adt/meson.build "subdir('mysql')"
require_text src/backend/utils/ddsm/meson.build "subdir('mysm')"
require_text contrib/meson.build "subdir('aux_mysql')"
require_text contrib/aux_mysql/meson.build "aux_mysql = shared_module('aux_mysql'"

# The autoconf/make graph must not pretend to recurse into a MySQL tree.
for makefile in \
	src/backend/Makefile \
	src/backend/parser/Makefile \
	src/backend/commands/Makefile \
	src/backend/adapter/Makefile \
	src/backend/utils/adt/Makefile
do
	if grep -Eq '(^|[[:space:]/])(mysql|mysm)(/|[[:space:]\\.$])' "$root/$makefile"; then
		fail "$makefile contains an unsupported MySQL/mysm make edge"
	fi
done

if test "$status" -ne 0; then
	exit 1
fi

printf '%s\n' 'MySQL compatibility build graph: Meson-only, no orphan parser rules'
