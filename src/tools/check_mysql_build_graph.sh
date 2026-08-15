#!/bin/sh
#
# Verify the Phase 2 kernel/extension build boundary for MySQL compatibility.
#
# mysql_parser, mysm, and aux_mysql are standalone PGXS modules in the sibling
# mysql_extensions repository.  The kernel retains only the executor and
# command forks, the session-state bridge, and the compatibility ABI.  A
# lingering Meson edge to an in-tree module would create a second, divergent
# product; losing an edge to a retained core fork would silently drop MySQL
# semantics from postgres itself.
#

set -eu

root=${1:?usage: $0 POSTGRES_SOURCE_ROOT}
status=0

fail()
{
	printf '%s\n' "mysql build graph: $*" >&2
	status=1
}

require_text()
{
	file=$1
	pattern=$2
	if ! grep -Fq -- "$pattern" "$root/$file"; then
		fail "$file is missing Meson graph entry: $pattern"
	fi
}

require_text src/backend/commands/meson.build "subdir('mysql')"
require_text src/backend/adapter/meson.build "subdir('mysql')"
require_text src/backend/executor/meson.build "'mys_execMain.c'"
require_text src/backend/executor/meson.build "'mys_nodeModifyTable.c'"

# Loadable modules must not reappear in the kernel Meson graph after Phase 2.
for edge in "subdir('mysql_parser')" "subdir('mysm')" "subdir('aux_mysql')"; do
	if grep -Fq -- "$edge" "$root/contrib/meson.build"; then
		fail "contrib/meson.build retains external-module edge: $edge"
	fi
done

# A clean kernel checkout contains no build rules for the extracted modules.
for orphan in \
	contrib/mysql_parser/Makefile \
	contrib/mysql_parser/meson.build \
	contrib/mysm/Makefile \
	contrib/mysm/meson.build \
	contrib/aux_mysql/Makefile \
	contrib/aux_mysql/meson.build
do
	if test -e "$root/$orphan"; then
		fail "extracted module retains an in-kernel build rule: $orphan"
	fi
done

if test "$status" -ne 0; then
	exit 1
fi

printf '%s\n' 'MySQL compatibility build graph: Phase 2 boundary intact'
