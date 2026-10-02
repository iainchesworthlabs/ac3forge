#!/usr/bin/env python3
"""Stage and pack iclforge as a self-contained ESP-IDF component archive.

WHY THIS EXISTS. `compote component pack` roots its archive at the component
directory and cannot reach above it. iclforge's component at esp-idf/iclforge/
is a thin wrapper that add_subdirectory()s the repo root, so packing it directly
produces an archive of three files - CMakeLists.txt, idf_component.yml and the
directory entry - which installs happily and then fails to configure, because
ICLFORGE_ROOT points outside the installed tree. That was the state of the
manifest until this script existed, and nothing said so: the pack SUCCEEDS.

So the sources are staged INTO a copy of the component first. The staged tree is
generated, never committed: a second copy of src/ac3/ in the repository is
exactly the drift this project avoids everywhere else.

WHAT GOES IN is the minimum the minimum-footprint profile compiles, worked out
from the same lists CMake uses rather than from a parallel one here - see
STAGED_TREES below for what that means and where it stops.

    python tools/packaging/pack_esp_component.py --version 0.10.0-beta.1
    python tools/packaging/pack_esp_component.py --version 0.10.0-beta.1 --verify
    python tools/packaging/pack_esp_component.py --version 0.10.0-beta.1 --with-ac4 --verify \
        --verify-targets esp32p4

--verify configures and builds a throwaway ESP-IDF project against the packed
archive, which is the only check that actually establishes the thing this script
is for. Needs an exported IDF environment; without one it says so and stops.

--with-ac4 also stages the AC-4 inspector, core and decoder, for a project that
turns on CONFIG_ICLFORGE_AC4 (planning/ac4.md, D14b). Without it the archive is
what it was before that option existed: the option is off by default, and a
project that turns it on against an archive packed without the AC-4 sources is
told so at configure.
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
COMPONENT = REPO / "esp-idf" / "iclforge"

# Whole directories copied verbatim. Directories rather than a file list on
# purpose: src/ac3/minimal.cmake names its own sources and changes without
# telling this script, so anything narrower would need keeping in step with it -
# which is the failure this repo has hit before (the bare-metal fixture, 131
# encoder commits stale). Copying the tree costs archive size and cannot go
# stale.
#
# What is NOT here is as deliberate: no apps/, no tests/, no tools/, no other
# language binding, and none of the container muxers. A component archive should
# carry the part that builds for this chip.
STAGED_TREES = (
    # The AC-3 codec and the five libraries it is built from: the minimum-footprint profile is
    # one archive of files from all six (src/ac3/minimal.cmake).
    "src/ac3",
    "src/base",
    "src/dsp",
    "src/objects",
    "src/render",
    "src/iec61937",
    # The header-only Fixed32 / scalar-function target src/ac3 and src/ac4core
    # both link (planning/ac4.md decision 31); the root CMakeLists.txt adds it
    # with add_subdirectory before it reaches src/ac3, so a staged tree
    # without it stops the configure with "source src/arithmetic ... is not an
    # existing directory" - the failure this list produced when D14a added it.
    "src/arithmetic",
    "cmake",
)

# What --with-ac4 adds: the AC-4 inspector, the core both AC-4 libraries link and
# the decoder, whole, as the trees above are. Not src/ac4enc, which no ESP32 part
# builds (planning/ac4.md, decision 34); the root adds it only under
# ICLFORGE_BUILD_AC4, which the component keeps off. Off by default, so an
# archive packed without the flag holds exactly the trees it always did.
STAGED_AC4_TREES = (
    "src/ac4",
    "src/ac4core",
    "src/ac4dec",
)

# Individual files the root build needs before it reaches src/ac3.
STAGED_FILES = (
    "CMakeLists.txt",
    "LICENSE",
    "README.md",
)

# Dropped from the staged copy. Both are excluded from the minimum-footprint
# profile already (src/ac3/minimal.cmake), so removing them changes nothing
# that builds - they are here because an archive carrying AVX2 kernels for a
# part with no AVX2 is just bigger.
#
# Repo-relative, and applied against the staged tree unchanged, because the
# staging preserves the layout. Written the other way - relative to src/ac3 -
# they still worked, and tools/checks/check_doc_paths.py rightly called them
# paths that do not exist: a reader cannot tell a wrong path from one that is
# merely relative to something else.
PRUNE = (
    "src/ac3/src/internal/avx2/mdct_avx2.cpp",
    "src/ac3/src/internal/avx2/avx2_probe.cpp",
)


def stage(destination: pathlib.Path, with_ac4: bool = False) -> None:
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
    shutil.rmtree(destination / "examples" / "hearth_sink" / "www", ignore_errors=True)

    library = destination / "lib"
    for tree in STAGED_TREES + (STAGED_AC4_TREES if with_ac4 else ()):
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
        ["compote", "component", "pack", "--name", "iclforge", "--version", version],
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
    sources = [n for n in names if "/lib/src/ac3/src/" in n and n.endswith(".cpp")]
    return len(names), len(sources)


def verify(archive: pathlib.Path, with_ac4: bool = False, targets: list[str] | None = None) -> None:
    """Build a throwaway project against the archive.

    The only check that establishes self-containment. Everything else - entry
    counts, file lists - can pass on an archive that does not configure.

    With `with_ac4` every part turns the AC-4 decoder on, and the throwaway
    application constructs one and calls it, for the reason the
    AC-3 decoder is called: an archive whose AC-4 headers or archives were left
    out would still link an application that never named them.

    `targets` narrows the parts built to some of the manifest's, for a run that
    only asks whether one option builds and does not need the rest.
    """
    if "IDF_PATH" not in os.environ:
        raise SystemExit("--verify needs an exported ESP-IDF environment (IDF_PATH is unset)")

    with tempfile.TemporaryDirectory(prefix="iclforge-verify-") as tmp:
        root = pathlib.Path(tmp)
        components = root / "components"
        unpacked = components / "iclforge"
        unpacked.mkdir(parents=True)
        with tarfile.open(archive) as tar:
            tar.extractall(unpacked, filter="data")

        (root / "main").mkdir()
        (root / "main" / "CMakeLists.txt").write_text(
            'idf_component_register(SRCS "main.cpp" REQUIRES iclforge)\n', encoding="utf-8"
        )
        (root / "CMakeLists.txt").write_text(
            "\n".join(
                [
                    "cmake_minimum_required(VERSION 3.28)",
                    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)",
                    "project(iclforge_component_verify LANGUAGES C CXX)",
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
        for target in targets or manifest_targets():
            # AC-4 on every part: single precision where there is a
            # floating-point unit, the fixed-point tier where there is not
            # (planning/ac4.md, D14d), as the AC-3 decoder above it.
            defaults = f'CONFIG_IDF_TARGET="{target}"\nCONFIG_COMPILER_OPTIMIZATION_SIZE=y\n'
            if with_ac4:
                # IDF's 1.5 MB factory partition: with AC-4 the esp32c3 image is
                # 1,155,232 bytes, past the default table's 1 MB.
                defaults += "CONFIG_ICLFORGE_AC4=y\nCONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y\n"
            (root / "sdkconfig.defaults").write_text(defaults, encoding="utf-8")
            (root / "main" / "main.cpp").write_text(main_source(with_ac4), encoding="utf-8")
            for command in (["set-target", target], ["build"]):
                subprocess.run([sys.executable, str(idf_py), *command], cwd=root, check=True)


def main_source(with_ac4: bool) -> str:
    """The throwaway project's application.

    Calls into the library rather than merely linking it: a component that
    unpacked but whose headers do not resolve would still LINK an empty main, and
    prove nothing.
    """
    lines = [
        '#include "iclforge/ac3/decoder/decoder.hpp"',
        '#include "iclforge/ac3/decoder/output.hpp"',
    ]
    if with_ac4:
        lines.append('#include "iclforge/ac4dec/decoder.hpp"')
    lines += [
        "#include <array>",
        "#include <span>",
        "",
        'extern "C" void app_main() {',
        "    // Instantiated and CALLED, not merely linked: a component",
        "    // that unpacked but whose headers did not resolve would",
        "    // still link an empty app_main and prove nothing.",
        "    static iclforge::ac3::FrameDecoder decoder{",
        "        {.output = {.target = iclforge::ac3::DownmixTarget::kLoRo}}};",
        "    static std::array<float, iclforge::ac3::kSamplesPerFrame> pcm{};",
        "    static std::array<std::span<float>, 1> spans{std::span<float>(pcm)};",
        "    (void)decoder.decode_frame_into({}, spans);",
    ]
    if with_ac4:
        lines += [
            "    // The AC-4 decoder the same way: an empty frame is a table of",
            "    // contents that does not read, which it refuses.",
            "    static iclforge::ac4::Decoder ac4_decoder;",
            "    (void)ac4_decoder.decode({});",
        ]
    lines.append("}")
    return "\n".join(lines) + "\n"


def manifest_targets() -> list[str]:
    """The `targets:` list from esp-idf/iclforge/idf_component.yml.

    Read rather than restated, and parsed by hand rather than with PyYAML: this
    script has no third-party dependency and the block it needs is a flat list
    of scalars under one key. A malformed or missing block is an error, not a
    default - silently verifying nothing is how a target ends up claimed and
    unbuilt.
    """
    manifest = (REPO / "esp-idf" / "iclforge" / "idf_component.yml").read_text(encoding="utf-8")
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
        raise SystemExit("no targets: block in esp-idf/iclforge/idf_component.yml")
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
    parser.add_argument(
        "--verify-targets",
        default="",
        help="with --verify: build only these of the manifest's targets, comma-separated "
        "(default: every one)",
    )
    parser.add_argument(
        "--with-ac4",
        action="store_true",
        help="also stage the AC-4 inspector, core and decoder, for CONFIG_ICLFORGE_AC4 "
        "(off by default: the archive is then what it was before that option)",
    )
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="iclforge-stage-") as tmp:
        staged = pathlib.Path(tmp) / "iclforge"
        stage(staged, args.with_ac4)
        archive = pack(staged, args.version)
        entries, sources = describe(archive)
        final = args.output / archive.name
        shutil.copy2(archive, final)

    print(f"packed {final}")
    print(f"  entries: {entries}")
    print(f"  src/ac3 sources: {sources}")
    if args.with_ac4:
        print("  with the AC-4 inspector, core and decoder")
    # The number that would have caught the original three-file archive. A
    # threshold rather than an exact count, because minimal.cmake's source list
    # is meant to change.
    if sources < 20:
        raise SystemExit(
            f"only {sources} library sources in the archive - it is not self-contained. "
            "See this script's own docstring."
        )
    if args.verify:
        wanted = [t for t in args.verify_targets.split(",") if t]
        unknown = [t for t in wanted if t not in manifest_targets()]
        if unknown:
            raise SystemExit(
                f"--verify-targets names {unknown}, not in the manifest's {manifest_targets()}"
            )
        verify(final, args.with_ac4, wanted or None)
        print("  verified: a throwaway IDF project builds against it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
