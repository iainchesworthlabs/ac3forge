#!/usr/bin/env bash
#
# Packaging manifest consistency guard.
#
# Two things, at two different severities:
#
# 1. Internal inconsistency within a manifest (hard failure, exit 1): the kind a manual
#    copy-forward-and-edit bump (see docs/releasing.md's per-ecosystem steps) can
#    introduce - a version string updated in one file of a set but not another, a hash
#    of the wrong length, a URL that does not name the version it is filed under.
# 2. A latest-tag advisory (::warning::, never fails the check): does each manifest
#    actually point at the latest release. This was deliberately left out entirely
#    until roadmap DR2 (.github/workflows/manifest-bump.yml) automated the bump - before
#    that, the four packaging bumps were a manual follow-up step done asynchronously
#    after a release went out (sometimes days later, in a separate PR), not atomically
#    with the tag, so a hard "must match the latest tag" gate would have failed by
#    design in the normal gap between tagging and getting around to the bump - noise,
#    not signal. Automating the bump's *start* does not close that gap outright (merging
#    the PR manifest-bump.yml opens is still a separate, reviewed step), which is why
#    this stays advisory rather than becoming the same hard gate.
#
# Usage:  ./tools/checks/check_packaging_versions.sh [-r <repo-root>]
# Exit:   0 = clean (an advisory warning does not affect this), 1 = inconsistency found.

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
formula_version=""
cask_version=""
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

# --- latest-tag advisory: does each manifest actually point at the latest release?
# Advisory only (::warning::, never sets fail=1) - even with manifest-bump.yml (roadmap
# DR2) opening the bump PR automatically right after a release, merging it is still a
# separate, reviewed step, so a real gap between "tagged" and "all four manifests
# updated" is expected and not itself a defect. Before that workflow existed the bump
# was manual with no automation to prompt it at all, which is why this was deliberately
# left out entirely - see this file's own header. Skipped, not failed, when there is no
# `v*` tag reachable from HEAD (a shallow clone with no tags fetched, or a checkout with
# no releases yet). ---
latest_tag="$(git -C "$root" describe --tags --abbrev=0 --match 'v*' 2>/dev/null || true)"
if [ -n "$latest_tag" ]; then
    latest_version="${latest_tag#v}"
    advise() { echo "::warning::$1"; echo "  (advisory) $1"; }

    vcpkg_json="$root/packaging/vcpkg-port/ac3forge/vcpkg.json"
    if [ -f "$vcpkg_json" ]; then
        v="$(grep -m1 '"version-semver"' "$vcpkg_json" | sed -E 's/.*"version-semver":[[:space:]]*"([^"]+)".*/\1/')"
        [ "$v" = "$latest_version" ] ||
            advise "vcpkg port is at $v, latest release is $latest_version - packaging/vcpkg-port/ac3forge/ needs a bump (docs/releasing.md#vcpkg-port)"
    fi

    if [ -f "$formula" ]; then
        [ "$formula_version" = "$latest_version" ] ||
            advise "Homebrew formula is at $formula_version, latest release is $latest_version - packaging/homebrew/Formula/ac3forge.rb needs a bump (docs/releasing.md#homebrew-formula-and-cask)"
    fi
    if [ -f "$cask" ]; then
        [ "$cask_version" = "$latest_version" ] ||
            advise "Homebrew cask is at $cask_version, latest release is $latest_version - packaging/homebrew/Casks/ac3gui.rb needs a bump (docs/releasing.md#homebrew-formula-and-cask)"
    fi

    if [ -f "$conandata" ] && ! grep -q "\"$latest_version\":" "$conandata"; then
        advise "conandata.yml has no entry for $latest_version - packaging/conan/conandata.yml needs a bump (docs/releasing.md#conan-recipe)"
    fi

    winget_dir="$root/packaging/winget/manifests/i/iainchesworthlabs/ac3forge/$latest_version"
    [ -d "$winget_dir" ] ||
        advise "no winget manifest directory for $latest_version - packaging/winget/manifests/ needs a new version directory (docs/releasing.md#winget-manifest)"
else
    echo "(latest-tag advisory skipped: no v* tag reachable from HEAD)"
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Packaging manifest inconsistency found - see docs/releasing.md for the per-ecosystem update steps."
    exit 1
fi

echo "OK: packaging manifests are internally consistent."
