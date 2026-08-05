#!/bin/sh
#
# check_mysql_kernel_exports.sh
#
# Verify the "kernel -> compatibility library" symbol contract (G3):
# every undefined (imported) symbol of the MySQL-compat shared libraries
# must resolve against the postgres binary.  The backend is linked with
# --export-dynamic, so a symbol not exported by postgres here means the
# module would fail to load at runtime (or, worse, silently bind to the
# wrong definition).
#
# Usage:
#   check_mysql_kernel_exports.sh <postgres-binary> <compat.so...>
#
# System-library symbols (GLIBC, libgcc) are ignored; they are not part
# of the kernel contract.

set -u

postgres_bin=${1:?usage: check_mysql_kernel_exports.sh <postgres> <so...>}
shift

# Export table of the postgres binary, one symbol per line.
exported=$(nm -D --defined-only "$postgres_bin" 2>/dev/null | awk '{print $3}')

# System libraries that a module legitimately links on its own; symbols
# resolved there are not part of the kernel contract.
sys_lib_defined() {
    for lib in /lib/*/libm.so.* /usr/lib/*/libm.so.* \
               /lib/*/libgcc_s.so.* /usr/lib/*/libgcc_s.so.*; do
        [ -e "$lib" ] || continue
        # Symbol names may carry version suffixes (e.g. ceil@@GLIBC_2.2.5).
        if nm -D --defined-only "$lib" 2>/dev/null |
            awk '{print $3}' | sed 's/@.*//' | grep -x "$1" >/dev/null; then
            return 0
        fi
    done
    return 1
}

fail=0

for so in "$@"; do
    if [ ! -f "$so" ]; then
        echo "check_mysql_kernel_exports: missing module $so"
        fail=1
        continue
    fi
    for sym in $(nm -D --undefined-only "$so" 2>/dev/null | awk '{print $2}'); do
        # Skip compiler runtime / transactional-memory symbols.
        case "$sym" in
            __* | _fini | _init | _ITM_*) continue ;;
            *@GLIBC* | *@GCC* | *@GLIBCXX* | *@CXXABI*) continue ;;
        esac
        # Do not use grep -q here: the producer is a shell builtin and a
        # short-circuiting grep would make it report a misleading SIGPIPE.
        if ! printf '%s\n' "$exported" | grep -x "$sym" >/dev/null && ! sys_lib_defined "$sym"; then
            echo "ERROR: $so imports '$sym' which postgres does not export"
            fail=1
        fi
    done
done

exit $fail
