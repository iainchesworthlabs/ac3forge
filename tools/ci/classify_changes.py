#!/usr/bin/env python3
"""Classify a change's file list into CI lanes (roadmap: CI lane partitions).

`ci.yml`'s `changes` job already tells a docs-only PR from a code one so the
split platform builds can be skipped (`docs_re`, right above the step this
script is called from). This is the next cut of that same idea: which
*lanes* - core library, each platform, each satellite toolchain - actually
need to run, so a Windows-only change stops paying for ESP-IDF and Android.

    gh api --paginate "repos/$REPO/pulls/$PR/files" --jq '.[].filename' \\
      | python3 tools/ci/classify_changes.py >> "$GITHUB_OUTPUT"

    python3 tools/ci/classify_changes.py --force-all >> "$GITHUB_OUTPUT"

    ... | python3 tools/ci/classify_changes.py --satellites-direct >> "$GITHUB_OUTPUT"

Reads one path per line from stdin (repo-relative, the same shape `gh api
... --jq '.[].filename'` already produces) and prints `<lane>=true` or
`<lane>=false` for every lane, one per line - ready to append straight to
`$GITHUB_OUTPUT`, the same convention the `code` output next to it uses.
`--force-all` skips stdin entirely and marks every lane true, for `push` to
`main` and `merge_group`: a queued or direct-to-main run has no single PR
diff to read against, and the merge queue's whole point is catching what an
individual PR's own lane subset could not see.

`ci.yml` consumes these outputs to gate the core workflow, split platform
workflows, satellites and package workflows. See docs/ci-lanes.md for the
mapping, the fan-out rule, and why an unrecognised path lights every lane
rather than none of them.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable

# Directory prefixes that put a changed path in a lane. A path can land in
# more than one lane - apps/cli/ is windows AND linux AND macos, because it
# is one desktop CLI built and tested on all three, not three separate
# programs. Every prefix ends in "/" so a differently-named sibling directory
# that merely starts with the same characters cannot accidentally match.
LANE_PREFIXES: dict[str, tuple[str, ...]] = {
    "core": (
        "src/", "tests/", "fuzz/", "cmake/", "tools/checks/", "tools/ci/", "requirements/",
    ),
    "windows": (
        "apps/windows/", "apps/notices/platform/windows/", "packaging/winget/",
        "packaging/conan/", "packaging/vcpkg-port/",
        "apps/cli/", "apps/gui/", "apps/common/", "apps/crucible/", "apps/hearth/",
    ),
    "linux": (
        "apps/linux/", "apps/notices/platform/linux/",
        "packaging/conan/", "packaging/vcpkg-port/",
        "apps/cli/", "apps/gui/", "apps/common/", "apps/crucible/", "apps/hearth/",
    ),
    "macos": (
        "apps/notices/platform/macos/", "packaging/homebrew/",
        "packaging/conan/", "packaging/vcpkg-port/",
        "apps/cli/", "apps/gui/", "apps/common/", "apps/crucible/", "apps/hearth/",
    ),
    "android": ("apps/android/",),
    "wasm": ("apps/wasm/", "js/"),
    # tools/packaging/ holds only pack_esp_component.py (the ESP-IDF
    # component/ESPHome workflow's own packaging step) - see docs/ci-lanes.md.
    # src/ac3/, src/base/, src/dsp/, src/objects/, src/render/, src/iec61937/,
    # src/arithmetic/ and cmake/ are the trees that script stages
    # into the component (its STAGED_TREES, and the root CMakeLists.txt in
    # ESP_ROOT_FILES below): the run after a merge leaves a core change to the
    # nightly run for every other satellite, and these are the exception,
    # because a change there is what breaks the package and the QEMU images
    # (a new tree missing from the pack list stopped the ESP-IDF configure on
    # 2026-09-29). The AC-4 trees are staged only for `--with-ac4` and stay
    # with the nightly run.
    "esp": (
        "esp-idf/", "esphome/", "apps/baremetal/", "tools/packaging/",
        "src/ac3/", "src/base/", "src/dsp/", "src/objects/", "src/render/",
        "src/iec61937/", "src/arithmetic/", "cmake/",
    ),
    "rust": ("rust/",),
    # examples/python/ alongside python/ itself - the rest of examples/ is
    # plain C++, already core's concern via its own build, not this lane's.
    "python": ("python/", "examples/python/"),
    # js/ package unit tests, not the wasm E2E demo - see wasm above. Left out
    # of CORE_FANOUT below on purpose: a core-only change does not need the
    # npm package's own tests run, only the platforms that embed core.
    "npm": ("js/",),
    "ci_self": (".github/workflows/", ".github/actions/", ".github/toolchain/"),
    "docs": ("docs/",),
}

# Root-level files matched by exact name, not a directory prefix - a nested
# apps/*/CMakeLists.txt must light only its own app's lane (already covered
# by that app's prefix above), never core.
CORE_ROOT_FILES = ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json")
# The root files the ESP-IDF component ships that a build reads (STAGED_FILES in
# tools/packaging/pack_esp_component.py; its LICENSE and README.md are documentation).
ESP_ROOT_FILES = ("CMakeLists.txt",)
DOCS_ROOT_FILES = ("LICENSE", "mkdocs.yml")
DOCS_SUFFIX = ".md"

# core fans out to every platform and language lane: a library change has to
# be validated everywhere it is built. npm is deliberately absent - see its
# comment in LANE_PREFIXES above.
CORE_FANOUT = ("windows", "linux", "macos", "android", "wasm", "esp", "rust", "python")

# The lanes for a toolchain other than the desktop platforms' own: each has its own
# build, its own runners and, mostly, its own slow tests. The run after a merge runs
# one of these only when a path in its own tree changed (`satellites_direct` below);
# a change to the core library reaches them in the nightly run, which lights every lane.
SATELLITES = ("android", "wasm", "esp", "rust", "python", "npm")

LANES = tuple(LANE_PREFIXES)


def classify(
    paths: Iterable[str], *, force_all: bool = False, satellites_direct: bool = False
) -> dict[str, bool]:
    """Return `{lane: bool}` for every lane in LANES.

    Two things make every lane true regardless of what actually matched: an
    empty path list (a manual dispatch, an API hiccup - the same "no list
    means build" rule `ci.yml`'s `code` output already applies) and any path
    this function does not recognise. A false skip is silent and wrong; a
    false build just costs a few extra minutes - see docs/ci-lanes.md.

    `satellites_direct` keeps the core fan-out away from SATELLITES: a lane
    among them is true only if a path in its own tree changed. The two cases
    above still light everything, so a workflow edit or an unknown path is
    proven everywhere.
    """
    if force_all:
        return dict.fromkeys(LANES, True)

    hits = dict.fromkeys(LANES, False)
    saw_path = False
    saw_unmatched = False
    for raw in paths:
        path = raw.strip()
        if not path:
            continue
        saw_path = True
        matched = False
        for lane, prefixes in LANE_PREFIXES.items():
            if path.startswith(prefixes):
                hits[lane] = True
                matched = True
        if "/" not in path and path in CORE_ROOT_FILES:
            hits["core"] = True
            matched = True
        if "/" not in path and path in ESP_ROOT_FILES:
            hits["esp"] = True
        if path.endswith(DOCS_SUFFIX) or ("/" not in path and path in DOCS_ROOT_FILES):
            hits["docs"] = True
            matched = True
        if not matched:
            saw_unmatched = True

    if not saw_path or saw_unmatched or hits["ci_self"]:
        return dict.fromkeys(LANES, True)

    if hits["core"]:
        for lane in CORE_FANOUT:
            if not (satellites_direct and lane in SATELLITES):
                hits[lane] = True

    return hits


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--force-all", action="store_true",
        help="Mark every lane true without reading stdin (push to main, merge_group).",
    )
    parser.add_argument(
        "--satellites-direct", action="store_true",
        help="A satellite lane (android, wasm, esp, rust, python, npm) is true only when a "
        "path in its own tree changed, not through the core fan-out (the run after a merge).",
    )
    args = parser.parse_args(argv[1:])

    paths = [] if args.force_all else sys.stdin.read().splitlines()
    hits = classify(paths, force_all=args.force_all, satellites_direct=args.satellites_direct)

    for lane in sorted(hits):
        value = "true" if hits[lane] else "false"
        print(f"{lane}={value}")
        print(f"  {lane}: {value}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
