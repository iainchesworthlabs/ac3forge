#!/usr/bin/env python3
"""Refuse ESP-IDF settings that would stop a board being recovered over USB.

A board flashed from this tree has to stay recoverable over USB: whatever goes
wrong with an image, `esptool write-flash` through the chip's ROM download mode
puts a working one back (planning/esp32-ota.md, "What stays USB-only, and how a
board is recovered"). Some ESP-IDF options take that away by burning eFuses,
which cannot be undone, and two more stop the bootloader checking an image
before it boots it, which the plan's integrity checks rely on. Any of them can
arrive as one line in an sdkconfig fragment, and nothing else in CI would
notice. This makes adding one a red job. Stdlib-only, run from ci.yml's
script-lint job and runnable the same way locally:

    python3 tools/checks/check_esp_efuse_free.py [--root <repo>]

It reads every sdkconfig fragment under the ESP-IDF projects the repository
holds (esp-idf/ and apps/baremetal/): files named sdkconfig.defaults or
sdkconfig.<anything>, but not sdkconfig itself or sdkconfig.old, which are a
build's own output. Directories a build or the component manager writes
(build*, managed_components) are skipped, so a local tree with builds in it
checks the same as CI's clean checkout.

A setting counts only when a fragment turns it on (`CONFIG_X=y`). The form
menuconfig writes for off (`# CONFIG_X is not set`) and `=n` are both fine.

Exit 1 with one ::error:: line per refused setting, and also when no fragment
is found at all, so a moved tree cannot pass by checking nothing.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Where the repository's ESP-IDF projects live.
PROJECT_ROOTS = ("esp-idf", "apps/baremetal")

# Directory names whose contents are a build's or the component manager's,
# not the repository's: `build`, the `build-<shape>` and `build_<shape>` forms
# a second shape gets, and the component manager's downloads.
SKIPPED_DIRS = ("build", "managed_components")
SKIPPED_DIR_PREFIXES = ("build-", "build_")

# Each refused option, with what turning it on would cost.
REFUSED: dict[str, str] = {
    "CONFIG_SECURE_BOOT": (
        "hardware secure boot burns the signing key's digest into eFuses, and from then "
        "on the ROM boots only a bootloader signed with that key"
    ),
    "CONFIG_SECURE_FLASH_ENC_ENABLED": (
        "flash encryption burns its key and counter into eFuses, and a board in release "
        "mode refuses a plaintext image written over USB"
    ),
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK": (
        "anti-rollback burns the image's security version into eFuses, and an older "
        "image never boots again, whatever is flashed"
    ),
    "CONFIG_SECURE_DISABLE_ROM_DL_MODE": (
        "burns the eFuse that turns the ROM download mode off, the way every board is "
        "recovered over USB"
    ),
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE": (
        "burns the eFuse for the secure download mode, which refuses most of what "
        "esptool does to recover a board"
    ),
    "CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON": (
        "the bootloader stops checking an image's SHA-256 before it boots it, and so no "
        "longer falls back to the other slot when an image is damaged"
    ),
    "CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS": (
        "the bootloader stops checking an image's SHA-256 before it boots it, and so no "
        "longer falls back to the other slot when an image is damaged"
    ),
}

# `CONFIG_NAME=y`, with the whitespace menuconfig never writes but a person might.
ON_RE = re.compile(r"^\s*(CONFIG_[A-Z0-9_]+)\s*=\s*y\s*$")


def is_fragment(path: Path) -> bool:
    """Whether `path` is an sdkconfig fragment rather than a build's own sdkconfig."""
    name = path.name
    if name in ("sdkconfig", "sdkconfig.old"):
        return False
    return name == "sdkconfig.defaults" or name.startswith("sdkconfig.")


def is_skipped(relative: Path) -> bool:
    """Whether a path lies under a directory a build or the component manager writes."""
    return any(
        part in SKIPPED_DIRS or part.startswith(SKIPPED_DIR_PREFIXES)
        for part in relative.parts[:-1]
    )


def fragments(root: Path) -> list[Path]:
    """Every sdkconfig fragment in the repository's ESP-IDF projects, sorted."""
    found: list[Path] = []
    for project_root in PROJECT_ROOTS:
        base = root / project_root
        if not base.is_dir():
            continue
        for path in base.rglob("sdkconfig*"):
            if not path.is_file() or not is_fragment(path):
                continue
            if is_skipped(path.relative_to(root)):
                continue
            found.append(path)
    return sorted(found)


def refused_in(text: str) -> list[tuple[int, str]]:
    """The (line number, option) of every refused option `text` turns on."""
    hits: list[tuple[int, str]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        match = ON_RE.match(line)
        if match and match.group(1) in REFUSED:
            hits.append((number, match.group(1)))
    return hits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root: Path = args.root.resolve()

    found = fragments(root)
    if not found:
        print(
            "::error::no sdkconfig fragments found under "
            + " or ".join(PROJECT_ROOTS)
            + " - the check would pass by reading nothing"
        )
        return 1

    refused = 0
    for path in found:
        relative = path.relative_to(root).as_posix()
        for number, option in refused_in(path.read_text(encoding="utf-8")):
            refused += 1
            print(
                f"::error file={relative},line={number}::{option}=y would stop a board being "
                f"recovered over USB or checking its image at boot: {REFUSED[option]}. "
                f"See planning/esp32-ota.md"
            )

    print(f"esp recovery check: {len(found)} fragment(s) read, {refused} refused setting(s)")
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main())
