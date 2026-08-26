#!/usr/bin/env bash
#
# Packaging manifest consistency guard.
#
# This does NOT check "does packaging/ point at the latest release tag" -
# docs/releasing.md's post-release checklist documents the four packaging
# bumps as their own follow-up step, done asynchronously after a release
# goes out (sometimes days later, in a separate PR), not atomically with the
# tag. A "must match the latest tag" gate would therefore fail by design in
# the normal gap between tagging and getting around to the bump PRs - noise,
# not signal.
#
# What this DOES catch: internal inconsistency within a manifest, the kind a
# manual copy-forward-and-edit bump (see docs/releasing.md's per-ecosystem
# steps) can introduce - a version string updated in one file of a set but
# not another, a hash of the wrong length, a URL that does not name the
# version it is filed under.
#
# Usage:  ./tools/checks/check_packaging_versions.sh [-r <repo-root>]
# Exit:   0 = clean, 1 = inconsistency found.

set -euo pipefail

root="."
while getopts "r:" opt; do
    case "$opt" in
        r) root="$OPTARG" ;;
        *) echo "Usage: $0 [-r <repo-root>]" >&2; exit 2 ;;
    esac
done

fail=0
note() { echo "::error::$1"; echo "  $1"; fail=1; }

# --- winget: every version directory's three files must agree with each
# other and with the directory name, and the installer's hash/URL must be
# well-formed. ---
winget_root="$root/packaging/winget/manifests/i/iainchesworthlabs/ac3forge"
if [ -d "$winget_root" ]; then
    for dir in "$winget_root"/*/; do
        version="$(basename "$dir")"
        installer="$dir/iainchesworthlabs.ac3forge.installer.yaml"
        locale="$dir/iainchesworthlabs.ac3forge.locale.en-US.yaml"
        manifest="$dir/iainchesworthlabs.ac3forge.yaml"

        for f in "$installer" "$locale" "$manifest"; do
            [ -f "$f" ] || note "winget $version: missing $(basename "$f")"
        done
        [ -f "$installer" ] && [ -f "$locale" ] && [ -f "$manifest" ] || continue

        for f in "$installer" "$locale" "$manifest"; do
            pv="$(grep -m1 '^PackageVersion:' "$f" | sed 's/^PackageVersion:[[:space:]]*//')"
            if [ "$pv" != "$version" ]; then
                note "winget $version: $(basename "$f") has PackageVersion: $pv, expected $version"
            fi
        done

        url="$(grep -m1 'InstallerUrl:' "$installer" | sed 's/^[[:space:]]*InstallerUrl:[[:space:]]*//')"
        case "$url" in
            *"/v$version/"*) ;;
            *) note "winget $version: InstallerUrl does not reference v$version ($url)" ;;
        esac

        sha="$(grep -m1 'InstallerSha256:' "$installer" | sed 's/^[[:space:]]*InstallerSha256:[[:space:]]*//')"
        if ! echo "$sha" | grep -qE '^[0-9A-Fa-f]{64}$'; then
            note "winget $version: InstallerSha256 is not 64 hex characters ($sha)"
        fi
    done
else
    note "winget manifest root not found: $winget_root"
fi

# --- conan: every sources: entry's key, url and sha256 must agree with each
# other. ---
conandata="$root/packaging/conan/conandata.yml"
if [ -f "$conandata" ]; then
    # Each version block is "  \"X.Y.Z...\":" followed by indented url:/sha256: lines.
    version=""
    while IFS= read -r line; do
        case "$line" in
            '  "'*'":')
                version="$(echo "$line" | sed -E 's/^  "([^"]+)":.*/\1/')"
                ;;
            *url:*)
                url="$(echo "$line" | sed -E 's/^[[:space:]]*url:[[:space:]]*"?([^"]*)"?[[:space:]]*$/\1/')"
                case "$url" in
                    *"/v$version.tar.gz") ;;
                    *) note "conan $version: url does not reference v$version.tar.gz ($url)" ;;
                esac
                ;;
            *sha256:*)
                sha="$(echo "$line" | sed -E 's/^[[:space:]]*sha256:[[:space:]]*"?([^"]*)"?[[:space:]]*$/\1/')"
                if ! echo "$sha" | grep -qE '^[0-9A-Fa-f]{64}$'; then
                    note "conan $version: sha256 is not 64 hex characters ($sha)"
                fi
                ;;
        esac
    done < "$conandata"
else
    note "conan manifest not found: $conandata"
fi

# --- homebrew: the Formula and Cask should agree on which release they pin,
# even though they version independently (Formula from source, Cask from a
# prebuilt .dmg) - see docs/releasing.md#homebrew-formula-and-cask. ---
formula="$root/packaging/homebrew/Formula/ac3forge.rb"
cask="$root/packaging/homebrew/Casks/ac3gui.rb"
if [ -f "$formula" ] && [ -f "$cask" ]; then
    formula_version="$(grep -m1 -oE 'archive/refs/tags/v[0-9][^"'"'"']*' "$formula" | sed -E 's#archive/refs/tags/v##; s/\.tar\.gz$//')"
    cask_version="$(grep -m1 -oE '^\s*version\s+"[^"]+"' "$cask" | sed -E 's/^\s*version\s+"([^"]+)"/\1/')"
    if [ -n "$formula_version" ] && [ -n "$cask_version" ] && [ "$formula_version" != "$cask_version" ]; then
        note "homebrew: Formula pins v$formula_version but Cask pins v$cask_version"
    fi
else
    note "homebrew formula/cask not found under $root/packaging/homebrew"
fi

# --- vcpkg port: portfile's SHA512 must be well-formed. ---
portfile="$root/packaging/vcpkg-port/ac3forge/portfile.cmake"
if [ -f "$portfile" ]; then
    sha="$(grep -m1 -oE 'SHA512 [0-9A-Fa-f]+' "$portfile" | awk '{print $2}')"
    if ! echo "$sha" | grep -qE '^[0-9A-Fa-f]{128}$'; then
        note "vcpkg port: SHA512 is not 128 hex characters ($sha)"
    fi
else
    note "vcpkg portfile not found: $portfile"
fi

# --- licence identifier: vcpkg.json, conanfile.py, the Homebrew formula and pyproject.toml must
# all agree on the SPDX identifier - a drift here is real legal-metadata inconsistency, not
# cosmetic (roadmap AP7: pyproject.toml drifted to GPL-3.0-only while everything else already
# said GPL-3.0-or-later, unnoticed until it was checked by hand). ---
vcpkg_json="$root/packaging/vcpkg-port/ac3forge/vcpkg.json"
conanfile="$root/packaging/conan/conanfile.py"
pyproject="$root/python/pyproject.toml"
if [ -f "$vcpkg_json" ] && [ -f "$conanfile" ] && [ -f "$formula" ] && [ -f "$pyproject" ]; then
    vcpkg_license="$(grep -m1 '"license"' "$vcpkg_json" | sed -E 's/.*"license":[[:space:]]*"([^"]*)".*/\1/')"
    conan_license="$(grep -m1 '^[[:space:]]*license = ' "$conanfile" | sed -E 's/^[[:space:]]*license = "([^"]*)".*/\1/')"
    formula_license="$(grep -m1 '^[[:space:]]*license ' "$formula" | sed -E 's/^[[:space:]]*license[[:space:]]+"([^"]*)".*/\1/')"
    pyproject_license="$(grep -m1 '^license = ' "$pyproject" | sed -E 's/^license = "([^"]*)".*/\1/')"

    if [ -n "$vcpkg_license" ]; then
        [ "$conan_license" = "$vcpkg_license" ] || note "licence drift: packaging/conan/conanfile.py says '$conan_license', vcpkg.json says '$vcpkg_license'"
        [ "$formula_license" = "$vcpkg_license" ] || note "licence drift: packaging/homebrew/Formula/ac3forge.rb says '$formula_license', vcpkg.json says '$vcpkg_license'"
        [ "$pyproject_license" = "$vcpkg_license" ] || note "licence drift: python/pyproject.toml says '$pyproject_license', vcpkg.json says '$vcpkg_license'"
    else
        note "licence check: could not extract vcpkg.json's \"license\" field"
    fi
else
    note "licence check: one or more of vcpkg.json/conanfile.py/Formula/pyproject.toml not found"
fi

# --- vcpkg feature <-> portfile.cmake parity: every feature vcpkg.json declares must be wired
# into portfile.cmake's vcpkg_check_features() call, and vice versa - the exact class of gap
# roadmap AP7's "capi" feature closed (a CMake option existed, a vcpkg feature didn't). Catches
# it for any future feature too, not just this one. ---
if [ -f "$vcpkg_json" ] && [ -f "$portfile" ]; then
    vcpkg_features="$(awk '/"features":/{found=1; next} found' "$vcpkg_json" \
        | grep -oE '"[a-zA-Z0-9_-]+":[[:space:]]*\{' \
        | sed -E 's/^"([^"]+)".*/\1/' | sort -u)"
    portfile_features="$(awk '/FEATURES/{found=1; next} found && /\)/{exit} found' "$portfile" \
        | awk '{print $1}' | sort -u)"

    missing_in_portfile="$(comm -23 <(printf '%s\n' "$vcpkg_features") <(printf '%s\n' "$portfile_features") | grep -v '^$' || true)"
    missing_in_vcpkg_json="$(comm -13 <(printf '%s\n' "$vcpkg_features") <(printf '%s\n' "$portfile_features") | grep -v '^$' || true)"
    [ -z "$missing_in_portfile" ] || note "vcpkg.json feature(s) not wired into portfile.cmake's vcpkg_check_features(): $(echo "$missing_in_portfile" | tr '\n' ' ')"
    [ -z "$missing_in_vcpkg_json" ] || note "portfile.cmake's vcpkg_check_features() names feature(s) missing from vcpkg.json's \"features\": $(echo "$missing_in_vcpkg_json" | tr '\n' ' ')"
else
    note "vcpkg feature-parity check: vcpkg.json or portfile.cmake not found"
fi

# --- Conan option <-> generate() parity: every AC3FORGE_BUILD_<NAME>-shaped Conan option
# (excluding shared/fPIC, which aren't component switches) must actually be wired into
# generate()'s tc.variables[...] - same gap class as the vcpkg check above. ---
if [ -f "$conanfile" ]; then
    conan_options="$(awk '/^[[:space:]]*options = \{/{found=1; next} found && /^[[:space:]]*\}/{exit} found' "$conanfile" \
        | grep -oE '"[a-zA-Z0-9_]+"' | tr -d '"' | grep -vE '^(shared|fPIC)$' | sort -u)"
    while IFS= read -r opt; do
        [ -n "$opt" ] || continue
        grep -q "self\.options\.$opt" "$conanfile" \
            || note "conanfile.py: option '$opt' has no matching AC3FORGE_BUILD_<NAME> wiring (no self.options.$opt reference found)"
    done <<< "$conan_options"
else
    note "conan option-parity check: conanfile.py not found"
fi

# --- pkg-config completeness: cmake/InstallLibrary.cmake's install(TARGETS ... EXPORT ...)
# component blocks and its ac3forge_install_pkgconfig() calls must be in 1:1 count - a future
# component that adds one but forgets the other (roadmap AP7's original gap: no pkg-config files
# existed for any component) fails here immediately. ---
install_lib="$root/cmake/InstallLibrary.cmake"
if [ -f "$install_lib" ]; then
    export_count="$(grep -cE '^[[:space:]]*EXPORT [A-Za-z0-9]+Targets$' "$install_lib" || true)"
    pkgconfig_count="$(grep -cE '^[[:space:]]*ac3forge_install_pkgconfig\($' "$install_lib" || true)"
    if [ "$export_count" -ne "$pkgconfig_count" ]; then
        note "cmake/InstallLibrary.cmake: $export_count install(TARGETS ... EXPORT ...) component block(s) but $pkgconfig_count ac3forge_install_pkgconfig() call(s) - every installed/exported component needs a matching pkg-config file"
    fi
else
    note "pkg-config completeness check: cmake/InstallLibrary.cmake not found"
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Packaging manifest inconsistency found - see docs/releasing.md for the per-ecosystem update steps."
    exit 1
fi

echo "OK: packaging manifests are internally consistent."
