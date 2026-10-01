#!/usr/bin/env bash
# Prove that an executable built with BUILD_SHARED_LIBS=ON runs its iclforge:: code from the
# shared libraries of the codec, not from a second copy of it that reached the executable
# another way.
#
# The shared-libs pass (config-linux-llvm-shared, .github/workflows/_ci-linux.yml) exists to show
# that every in-tree consumer links and runs against the real .so. For iclforge-tests it did not: the
# C API library embeds the codec as a static archive, exported its C++ symbols, and put the archive
# on the link line of whatever linked it, all of them ahead of the codec's .so. Every test still
# passed, on code that was never in the .so. A passing test suite cannot show this, which is why
# this looks at the binding itself.
#
# The codec is six libraries (iclforge_ac3, base, dsp, objects, render and iec61937), and all of
# them export iclforge:: symbols, so they are all given. Two questions, both about the
# iclforge:: C++ symbols the libraries export:
#
#   1. Where does the dynamic linker bind the ones the executable imports? Read from
#      LD_DEBUG=bindings with LD_BIND_NOW=1, so every import is resolved at load whether or not a
#      test would have called it. Each must bind to the library that exports it.
#   2. Does the executable define any of them itself? A definition linked in from a static archive
#      is used ahead of the shared library and binds nothing, so the first question cannot see it.
#      None may be.
#
# It also fails if nothing binds to the libraries at all, since an executable that links no
# codec symbols would pass both questions for the wrong reason.
#
# libstdc++ template instantiations that the libraries export are left out: every C++ binary
# carries its own copy of those, and the executable's is meant to win.
#
# Usage: check_shared_forge_binding.sh <executable> <library.so>...

set -euo pipefail
export LC_ALL=C

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <executable> <library.so>..." >&2
    exit 2
fi
exe=$1
shift

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# An Itanium-mangled name in namespace iclforge: _ZN8iclforge... for a function, _ZNK8iclforge...
# for a const member, and the vtable, typeinfo, guard and static-local forms of the same. The
# libraries that are not given (ac4, mp4 ...) export iclforge:: names too, and they are not looked
# at: only what the given libraries export is asked about.
iclforge_symbol='^_Z(N|NK|TVN|TIN|TSN|GVN|ZN|ZNK)8iclforge'

names() { awk '{print $NF}' | { grep -E "$iclforge_symbol" || true; } | sort -u; }

# One line per exported symbol: the library's name without its version, and the symbol.
: > "$work/exports"
for lib in "$@"; do
    stem="$(basename "$lib")"
    stem="${stem%%.so*}"
    nm -D --defined-only "$lib" | names | sed "s/^/$stem /" >> "$work/exports"
done
sort -u -o "$work/exports" "$work/exports"
if [ ! -s "$work/exports" ]; then
    echo "error: none of the libraries exports iclforge:: symbols; are they the codec's?" >&2
    exit 2
fi

# 2. What the executable defines: its dynamic table (a definition the process can bind to) and, if
# it has not been stripped, its full symbol table.
{ nm -D --defined-only "$exe"; nm --defined-only "$exe" 2>/dev/null || true; } | names > "$work/defined"
awk 'NR == FNR { defined[$1] = 1; next } ($2 in defined) { print $2 }' \
    "$work/defined" "$work/exports" | sort -u > "$work/linked_in"
# One definition is meant to be there twice. iclforge-tests compiles src/base/src/cpu_features.cpp a
# second time on purpose (tests/CMakeLists.txt) to test the dispatch against the probe directory the
# build chose, and iclforge_base exports has_avx2() since the libraries were split.
{ grep -v -E '^_ZN8iclforge8internal3cpu8has_avx2Ev$' "$work/linked_in" || true; } > "$work/kept"
mv "$work/kept" "$work/linked_in"

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

# The executable's bindings of a symbol some library exports: the provider it got, and whether that
# is one of the libraries that export it.
awk -v exe="$(basename "$exe")" '
    function base(path,  parts, n) { n = split(path, parts, "/"); return parts[n] }
    function stem(path,  s) { s = base(path); sub(/\.so.*$/, "", s); return s }
    NR == FNR { owners[$2] = owners[$2] " " $1 " "; next }
    base($1) == exe && ($3 in owners) {
        print stem($2), $3, (index(owners[$3], " " stem($2) " ") ? "ok" : "elsewhere")
    }
' "$work/exports" "$work/all_bindings" > "$work/forge_bindings"

bound=$(awk '$3 == "ok"' "$work/forge_bindings" | wc -l)
awk '$3 == "elsewhere" { print $1, $2 }' "$work/forge_bindings" > "$work/elsewhere"

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
    report "$(wc -l < "$work/elsewhere") iclforge:: symbol(s) $(basename "$exe") imports from the codec's libraries bind to another library:" \
        "$work/elsewhere"
    awk '{print "    bound to " $1}' "$work/elsewhere" | sort | uniq -c >&2
fi
if [ -s "$work/linked_in" ]; then
    report "$(wc -l < "$work/linked_in") iclforge:: symbol(s) the libraries export are defined inside $(basename "$exe") itself:" \
        "$work/linked_in"
fi
if [ "$bound" -eq 0 ]; then
    echo "FAIL: nothing in $(basename "$exe") binds to the libraries; the check has nothing to say." >&2
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "OK: $bound iclforge:: symbols bind from $(basename "$exe") to the $# libraries that export them; none are linked in."
fi
exit "$status"
