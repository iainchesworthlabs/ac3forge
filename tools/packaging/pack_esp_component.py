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

# Dropped from the staged copy of src/forge. Every one of these is excluded from
# the minimum-footprint profile already (src/forge/minimal.cmake), so removing
# them changes nothing that builds - they are here because an archive that
# carries the AVX2 kernels and the Tracy shims for a part that has neither is
# just bigger.
PRUNE_FROM_FORGE = (
    "src/internal/avx2/mdct_avx2.cpp",
    "src/internal/avx2/avx2_probe.cpp",
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

    for relative in PRUNE_FROM_FORGE:
        (library / "src" / "forge" / relative).unlink(missing_ok=True)

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
        (root / "sdkconfig.defaults").write_text(
            'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_COMPILER_OPTIMIZATION_SIZE=y\n', encoding="utf-8"
        )

        # Through the interpreter rather than as `idf.py`: it is a Python
        # script, and on Windows subprocess cannot execute one directly
        # (WinError 193). This spelling works on both.
        idf_py = pathlib.Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"
        for command in (["set-target", "esp32s3"], ["build"]):
            subprocess.run([sys.executable, str(idf_py), *command], cwd=root, check=True)


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
