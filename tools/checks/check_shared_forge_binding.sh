#!/usr/bin/env bash
# Prove that an executable built with BUILD_SHARED_LIBS=ON runs its ac3::forge code from
# libac3forge.so, not from a second copy of the codec that reached it another way.
#
# The shared-libs pass (config-linux-llvm-shared, .github/workflows/_ci-linux.yml) exists to show
# that every in-tree consumer links and runs against the real .so. For ac3tests it did not: the
# C API library embeds the codec as a static archive, exported its C++ symbols, and put the archive
# on the link line of whatever linked it, all of them ahead of libac3forge.so. Every test still
# passed, on code that was never in the .so. A passing test suite cannot show this, which is why
# this looks at the binding itself.
#
# Two questions, both about the ac3:: C++ symbols libac3forge.so exports:
#
#   1. Where does the dynamic linker bind the ones the executable imports? Read from
#      LD_DEBUG=bindings with LD_BIND_NOW=1, so every import is resolved at load whether or not a
#      test would have called it. All of them must bind to libac3forge.so.
#   2. Does the executable define any of them itself? A definition linked in from a static archive
#      is used ahead of the shared library and binds nothing, so the first question cannot see it.
#      None may be.
#
# It also fails if nothing binds to libac3forge.so at all, since an executable that links no
# forge symbols would pass both questions for the wrong reason.
#
# libstdc++ template instantiations that libac3forge.so exports are left out: every C++ binary
# carries its own copy of those, and the executable's is meant to win.
#
# Usage: check_shared_forge_binding.sh <executable> <libac3forge.so>

set -euo pipefail
export LC_ALL=C

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <executable> <libac3forge.so>" >&2
    exit 2
fi
exe=$1
forge=$2

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# An Itanium-mangled name in namespace ac3: _ZN3ac3... for a function, _ZNK3ac3... for a const
# member, and the vtable, typeinfo, guard and static-local forms of the same.
ac3_symbol='^_Z(N|NK|TVN|TIN|TSN|GVN|ZN|ZNK)3ac3'

names() { awk '{print $NF}' | { grep -E "$ac3_symbol" || true; } | sort -u; }

nm -D --defined-only "$forge" | names > "$work/exports"
if [ ! -s "$work/exports" ]; then
    echo "error: $forge exports no ac3:: symbols; is it libac3forge.so?" >&2
    exit 2
fi

# 2. What the executable defines: its dynamic table (a definition the process can bind to) and, if
# it has not been stripped, its full symbol table.
{ nm -D --defined-only "$exe"; nm --defined-only "$exe" 2>/dev/null || true; } | names > "$work/defined"
comm -12 "$work/exports" "$work/defined" > "$work/linked_in"

# 1. Where the executable's imports bind. The tag matches no test, so the binary loads, registers
# its tests and exits without running any. That is Catch2's exit code 2 (0 in some versions); with
# LD_BIND_NOW an import that resolves nowhere stops the process before main, and then there is
# nothing to read.
rc=0
LD_DEBUG=bindings LD_BIND_NOW=1 "$exe" '[no-such-tag-for-the-binding-check]' \
    > /dev/null 2> "$work/ld_debug" || rc=$?
if [ "$rc" -ne 0 ] && [ "$rc" -ne 2 ]; then
    echo "error: $exe exited with status $rc before it could be checked:" >&2
    grep -v 'binding file' "$work/ld_debug" | tail -n 5 >&2 || true
    exit 2
fi

# "binding file <user> [0] to <provider> [0]: normal symbol `<name>' [<version>]"
sed -nE "s/.*binding file ([^ ]+) \[[0-9]+\] to ([^ ]+) \[[0-9]+\]: (normal|weak) symbol .([^ ']+)'.*/\1 \2 \4/p" \
    "$work/ld_debug" > "$work/all_bindings"

awk -v exe="$(basename "$exe")" '
    function base(path,  parts, n) { n = split(path, parts, "/"); return parts[n] }
    NR == FNR { exported[$1] = 1; next }
    base($1) == exe && ($3 in exported) { print base($2), $3 }
' "$work/exports" "$work/all_bindings" > "$work/forge_bindings"

bound_to_forge=$(awk '$1 ~ /^libac3forge\.so/' "$work/forge_bindings" | wc -l)
awk '$1 !~ /^libac3forge\.so/' "$work/forge_bindings" > "$work/elsewhere"

status=0
report() {
    local heading=$1 file=$2
    echo "FAIL: $heading" >&2
    awk '{print $NF}' "$file" | head -n 15 | c++filt | sed 's/^/    /' >&2
    if [ "$(wc -l < "$file")" -gt 15 ]; then
        echo "    ... and $(( $(wc -l < "$file") - 15 )) more" >&2
    fi
    status=1
}

if [ -s "$work/elsewhere" ]; then
    report "$(wc -l < "$work/elsewhere") ac3:: symbol(s) $(basename "$exe") imports from libac3forge.so bind to another library:" \
        "$work/elsewhere"
    awk '{print "    bound to " $1}' "$work/elsewhere" | sort | uniq -c >&2
fi
if [ -s "$work/linked_in" ]; then
    report "$(wc -l < "$work/linked_in") ac3:: symbol(s) libac3forge.so exports are defined inside $(basename "$exe") itself:" \
        "$work/linked_in"
fi
if [ "$bound_to_forge" -eq 0 ]; then
    echo "FAIL: nothing in $(basename "$exe") binds to $(basename "$forge"); the check has nothing to say." >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "OK: $bound_to_forge ac3:: symbols bind to $(basename "$forge") from $(basename "$exe"); none are linked in."
fi
exit "$status"
