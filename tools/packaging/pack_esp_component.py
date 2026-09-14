#!/usr/bin/env python3
"""Stage and pack ac3forge as a self-contained ESP-IDF component archive.

WHY THIS EXISTS. `compote component pack` roots its archive at the component
directory and cannot reach above it. ac3forge's component at esp-idf/ac3forge/
is a thin wrapper that add_subdirectory()s the repo root, so packing it directly
produces an archive of three files - CMakeLists.txt, idf_component.yml and the
directory entry - which installs happily and then fails to configure, because
AC3FORGE_ROOT points outside the installed tree. That was the state of the
manifest until this script existed, and nothing said so: the pack SUCCEEDS.

So the sources are staged INTO a copy of the component first. The staged tree is
generated, never committed: a second copy of src/forge/ in the repository is
exactly the drift this project avoids everywhere else.

WHAT GOES IN is the minimum the minimum-footprint profile compiles, worked out
from the same lists CMake uses rather than from a parallel one here - see
STAGED_TREES below for what that means and where it stops.

    python tools/packaging/pack_esp_component.py --version 0.10.0-beta.1
    python tools/packaging/pack_esp_component.py --version 0.10.0-beta.1 --verify

--verify configures and builds a throwaway ESP-IDF project against the packed
archive, which is the only check that actually establishes the thing this script
is for. Needs an exported IDF environment; without one it says so and stops.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
COMPONENT = REPO / "esp-idf" / "ac3forge"

# Whole directories copied verbatim. Directories rather than a file list on
# purpose: src/forge/minimal.cmake names its own sources and changes without
# telling this script, so anything narrower would need keeping in step with it -
# which is the failure this repo has hit before (the bare-metal fixture, 131
# encoder commits stale). Copying the tree costs archive size and cannot go
# stale.
#
# What is NOT here is as deliberate: no apps/, no tests/, no tools/, no other
# language binding, and none of the container muxers. A component archive should
# carry the part that builds for this chip.
STAGED_TREES = (
    "src/forge",
    "cmake",
)

# Individual files the root build needs before it reaches src/forge.
STAGED_FILES = (
    "CMakeLists.txt",
    "LICENSE",
    "README.md",
)

# Dropped from the staged copy. Both are excluded from the minimum-footprint
# profile already (src/forge/minimal.cmake), so removing them changes nothing
# that builds - they are here because an archive carrying AVX2 kernels for a
# part with no AVX2 is just bigger.
#
# Repo-relative, and applied against the staged tree unchanged, because the
# staging preserves the layout. Written the other way - relative to src/forge -
# they still worked, and tools/checks/check_doc_paths.py rightly called them
# paths that do not exist: a reader cannot tell a wrong path from one that is
# merely relative to something else.
PRUNE = (
    "src/forge/src/internal/avx2/mdct_avx2.cpp",
    "src/forge/src/internal/avx2/avx2_probe.cpp",
)


def stage(destination: pathlib.Path) -> None:
    """Copy the component plus the sources it needs into `destination`.

    The library lands under lib/, NOT beside the component's own files. Both
    trees have a CMakeLists.txt at their root - the component's wrapper and the
    library's project - and staging them into one directory silently replaces
    the first with the second, which is a component that is no longer a
    component. That is how the first attempt at this failed.
    """
    shutil.copytree(COMPONENT, destination, dirs_exist_ok=True)
    # A previous run's output, if the component directory was packed in place.
    shutil.rmtree(destination / "dist", ignore_errors=True)
    # The streaming example's stream set - the repository's streams for a
    # device to fetch, some 2.7 MB (planning/esp32-stream-set.md) - is not part
    # of the component. The example's own stream/ stays.
    shutil.rmtree(destination / "examples" / "stream_player" / "www", ignore_errors=True)

    library = destination / "lib"
    for tree in STAGED_TREES:
        src = REPO / tree
        if not src.is_dir():
            raise SystemExit(f"missing staged tree: {src}")
        shutil.copytree(src, library / tree, dirs_exist_ok=True)

    for name in STAGED_FILES:
        src = REPO / name
        if not src.is_file():
            raise SystemExit(f"missing staged file: {src}")
        (library).mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, library / name)

    for relative in PRUNE:
        target = library / relative
        if not target.is_file():
            # Not missing_ok: a path that stopped resolving is how a prune list
            # goes quietly stale, and the whole point of these entries is that
            # they are NOT in the archive.
            raise SystemExit(f"prune list is stale, no such file: {relative}")
        target.unlink()

    # Generated build output that copytree would otherwise carry along.
    for junk in ("build", "dist", "managed_components"):
        shutil.rmtree(destination / junk, ignore_errors=True)


def pack(staged: pathlib.Path, version: str) -> pathlib.Path:
    subprocess.run(
        ["compote", "component", "pack", "--name", "ac3forge", "--version", version],
        cwd=staged,
        check=True,
    )
    archives = sorted((staged / "dist").glob("*.tgz"))
    if not archives:
        raise SystemExit("compote produced no archive")
    return archives[-1]


def describe(archive: pathlib.Path) -> tuple[int, int]:
    """Returns (entries, forge source files) - the second is what matters."""
    with tarfile.open(archive) as tar:
        names = tar.getnames()
    sources = [n for n in names if "/lib/src/forge/src/" in n and n.endswith(".cpp")]
    return len(names), len(sources)


def verify(archive: pathlib.Path) -> None:
    """Build a throwaway project against the archive.

    The only check that establishes self-containment. Everything else - entry
    counts, file lists - can pass on an archive that does not configure.
    """
    if "IDF_PATH" not in os.environ:
        raise SystemExit("--verify needs an exported ESP-IDF environment (IDF_PATH is unset)")

    with tempfile.TemporaryDirectory(prefix="ac3forge-verify-") as tmp:
        root = pathlib.Path(tmp)
        components = root / "components"
        unpacked = components / "ac3forge"
        unpacked.mkdir(parents=True)
        with tarfile.open(archive) as tar:
            tar.extractall(unpacked, filter="data")

        (root / "main").mkdir()
        (root / "main" / "CMakeLists.txt").write_text(
            'idf_component_register(SRCS "main.cpp" REQUIRES ac3forge)\n', encoding="utf-8"
        )
        # Calls into the library rather than merely linking it: a component that
        # unpacked but whose headers do not resolve would still LINK an empty
        # main, and prove nothing.
        (root / "main" / "main.cpp").write_text(
            "\n".join(
                [
                    '#include "ac3/decoder/decoder.hpp"',
                    '#include "ac3/decoder/output.hpp"',
                    "#include <array>",
                    "#include <span>",
                    "",
                    "extern \"C\" void app_main() {",
                    "    // Instantiated and CALLED, not merely linked: a component",
                    "    // that unpacked but whose headers did not resolve would",
                    "    // still link an empty app_main and prove nothing.",
                    "    static ac3::FrameDecoder decoder{",
                    "        {.output = {.target = ac3::DownmixTarget::kLoRo}}};",
                    "    static std::array<float, ac3::kSamplesPerFrame> pcm{};",
                    "    static std::array<std::span<float>, 1> spans{std::span<float>(pcm)};",
                    "    (void)decoder.decode_frame_into({}, spans);",
                    "}",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        (root / "CMakeLists.txt").write_text(
            "\n".join(
                [
                    "cmake_minimum_required(VERSION 3.28)",
                    'include($ENV{IDF_PATH}/tools/cmake/project.cmake)',
                    "project(ac3forge_component_verify LANGUAGES C CXX)",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        # Through the interpreter rather than as `idf.py`: it is a Python
        # script, and on Windows subprocess cannot execute one directly
        # (WinError 193). This spelling works on both.
        idf_py = pathlib.Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"

        # Every target the manifest claims, not the first one. The two differ
        # in the thing the archive is most likely to get wrong: the S3 has a
        # single-precision FPU and builds the float32 decode path, the C3 has
        # no FPU at all and builds the fixed-point one
        # (planning/arithmetic-tiers.md), so a package that links for one can
        # still fail to configure for the other. The manifest's own list is
        # the source - adding a target there is what adds it here.
        for target in manifest_targets():
            (root / "sdkconfig.defaults").write_text(
                f'CONFIG_IDF_TARGET="{target}"\nCONFIG_COMPILER_OPTIMIZATION_SIZE=y\n',
                encoding="utf-8",
            )
            for command in (["set-target", target], ["build"]):
                subprocess.run([sys.executable, str(idf_py), *command], cwd=root, check=True)


def manifest_targets() -> list[str]:
    """The `targets:` list from esp-idf/ac3forge/idf_component.yml.

    Read rather than restated, and parsed by hand rather than with PyYAML: this
    script has no third-party dependency and the block it needs is a flat list
    of scalars under one key. A malformed or missing block is an error, not a
    default - silently verifying nothing is how a target ends up claimed and
    unbuilt.
    """
    manifest = (REPO / "esp-idf" / "ac3forge" / "idf_component.yml").read_text(encoding="utf-8")
    targets: list[str] = []
    inside = False
    for line in manifest.splitlines():
        if line.startswith("targets:"):
            inside = True
            continue
        if inside:
            stripped = line.strip()
            if stripped.startswith("- "):
                targets.append(stripped[2:].strip())
            elif stripped and not stripped.startswith("#"):
                break
    if not targets:
        raise SystemExit("no targets: block in esp-idf/ac3forge/idf_component.yml")
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="component version, e.g. 0.10.0-beta.1")
    parser.add_argument("--output", type=pathlib.Path, default=REPO / "dist" / "esp-component")
    parser.add_argument(
        "--verify",
        action="store_true",
        help="build a throwaway IDF project against the packed archive",
    )
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ac3forge-stage-") as tmp:
        staged = pathlib.Path(tmp) / "ac3forge"
        stage(staged)
        archive = pack(staged, args.version)
        entries, sources = describe(archive)
        final = args.output / archive.name
        shutil.copy2(archive, final)

    print(f"packed {final}")
    print(f"  entries: {entries}")
    print(f"  src/forge sources: {sources}")
    # The number that would have caught the original three-file archive. A
    # threshold rather than an exact count, because minimal.cmake's source list
    # is meant to change.
    if sources < 20:
        raise SystemExit(
            f"only {sources} library sources in the archive - it is not self-contained. "
            "See this script's own docstring."
        )
    if args.verify:
        verify(final)
        print("  verified: a throwaway IDF project builds against it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
