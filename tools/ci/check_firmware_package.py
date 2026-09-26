#!/usr/bin/env python3
"""Open every published hearth_sink image before it is uploaded (planning/esp32-ota.md, O8).

    python tools/ci/check_firmware_package.py DIR [--allow-missing]

DIR is what tools/hearth/package_firmware.py wrote for each board image, with
hearth-sink-manifest.json beside the files. An image published under a board's
name goes onto that board, over the network or through the browser installer,
and the ways it can be wrong all leave a plausible file behind: the S3's image
under the C6's name, a P4 image that claims v3 silicon, a factory image that
flashes something other than the parts beside it, a network someone's build
had built in. So each image is opened and held to its name:

- the chip it is for, the flash size it was built for, whether its board has
  PSRAM, and the chip revisions it accepts are the ones its name promises
  (IMAGES below);
- it is an application image whose segments, checksum and appended SHA-256
  check out, as the bootloader checks them, and it carries the version its
  file names do, one that names its build rather than ESP-IDF's fallback, "1";
- it fits the smallest app slot of the partition table it ships with;
- its parts.zip, laid out as its own flash_args says, is byte for byte its
  factory.bin, and writes the same app image as the .bin beside it;
- no Wi-Fi network is built into it;
- every file is there, with the size and SHA-256 the manifest gives.

Every image IMAGES names must be there, unless --allow-missing (for a local
run over a subset). An image IMAGES does not name is refused: a new board's
image needs its rule first.

The image walk here is written apart from package_firmware.py's on purpose,
so that the one checks the other rather than agreeing with it by
construction. Stdlib only, as the script-lint job's Python has nothing else.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

MANIFEST_NAME = "hearth-sink-manifest.json"


@dataclass(frozen=True)
class Rule:
    target: str
    chip_id: int
    flash_size: str
    psram: bool
    # The image must accept every revision in [lowest, highest] ...
    lowest: int
    highest: int
    # ... and, where set, refuse every revision above this.
    refuse_above: int | None = None


# One rule for each image the releases publish (the plan's "What O8 builds").
IMAGES = {
    "hearth-sink-esp32s3": Rule("esp32s3", 0x0009, "16MB", True, 0, 99),
    "hearth-sink-esp32c6": Rule("esp32c6", 0x000D, "4MB", False, 0, 99),
    "hearth-sink-esp32c6-16mb": Rule("esp32c6", 0x000D, "16MB", False, 0, 99),
    # sdkconfig.p4 builds for v1.x silicon; a v3.x board needs another build.
    "hearth-sink-esp32p4-rev1": Rule("esp32p4", 0x0012, "16MB", True, 100, 199, refuse_above=199),
}

FLASH_CODES = {"1MB": 0, "2MB": 1, "4MB": 2, "8MB": 3, "16MB": 4, "32MB": 5, "64MB": 6, "128MB": 7}


def revision_set(full: int) -> bool:
    """ESP-IDF's IS_FIELD_SET: 0 and 65535 both mean the build set no maximum."""
    return full not in (0, 65535)


def walk_image(data: bytes) -> str | None:
    """Why `data` is not a whole application image, or None: esp_image_format.c's checks."""
    if len(data) < 32 + 256 or data[0] != 0xE9:
        return "it is not an ESP-IDF application image"
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        return "it has no application description"
    segments = data[1]
    if not 1 <= segments <= 16:
        return f"its header names {segments} segments"
    at = 24
    checksum = 0xEF
    for index in range(segments):
        if at + 8 > len(data):
            return f"it ends before segment {index}'s header"
        length = struct.unpack_from("<I", data, at + 4)[0]
        at += 8
        if length % 4 or at + length > len(data):
            return f"segment {index} does not fit the file"
        for byte in data[at : at + length]:
            checksum ^= byte
        at += length
    padded = (at + 1 + 15) & ~15
    if data[23] != 1:
        return "it carries no SHA-256 of itself (hash_appended is not set)"
    if padded + 32 > len(data):
        return "it ends before the SHA-256 the build appended"
    if hashlib.sha256(data[:padded]).digest() != data[padded : padded + 32]:
        return "its SHA-256 does not match the one appended to it"
    if data[padded - 1] != checksum:
        return "its checksum byte does not match its segments"
    return None


def smallest_slot(table: bytes) -> int | None:
    slots = []
    for at in range(0, len(table) - 31, 32):
        magic, ptype, _subtype, _offset, size = struct.unpack_from("<HBBII", table, at)
        if magic != 0x50AA:
            break
        if ptype == 0x00:
            slots.append(size)
    return min(slots) if slots else None


def lay_out(regions: list[tuple[int, bytes]]) -> bytes:
    end = max(offset + len(data) for offset, data in regions)
    flash = bytearray(b"\xff" * end)
    for offset, data in regions:
        flash[offset : offset + len(data)] = data
    return bytes(flash)


def check_image(directory: Path, image: dict[str, Any]) -> list[str]:
    """Every way `image` does not keep its name's promises, each as a sentence."""
    problems: list[str] = []
    name = str(image.get("name", ""))
    rule = IMAGES.get(name)
    if rule is None:
        return [f"no rule for an image named '{name}': add it to IMAGES first"]
    version = str(image.get("version", ""))

    files: dict[str, bytes] = {}
    for kind in ("app", "factory", "parts", "elf"):
        entry = (image.get("files") or {}).get(kind) or {}
        file_name = str(entry.get("name", ""))
        path = directory / file_name
        if not file_name or not path.is_file():
            problems.append(f"its {kind} file '{file_name}' is not there")
            continue
        if version not in file_name:
            problems.append(f"its {kind} file '{file_name}' does not carry its version, {version}")
        data = path.read_bytes()
        if len(data) != entry.get("size") or hashlib.sha256(data).hexdigest() != entry.get(
            "sha256"
        ):
            problems.append(f"{file_name} is not the file the manifest describes")
        files[kind] = data
    if "app" not in files:
        return problems
    app = files["app"]

    if image.get("target") != rule.target or image.get("chip_id") != rule.chip_id:
        problems.append(f"it is for {image.get('target')}, and its name says {rule.target}")
    if len(app) >= 32 and struct.unpack_from("<H", app, 12)[0] != rule.chip_id:
        problems.append(f"its header's chip ID is not {rule.target}'s")
    why = walk_image(app)
    if why:
        problems.append(f"its app image does not check out: {why}")
        return problems
    min_rev, max_rev = struct.unpack_from("<HH", app, 15)
    if min_rev > rule.lowest or (revision_set(max_rev) and max_rev < rule.highest):
        problems.append(
            f"it accepts chip revisions {min_rev} to {max_rev} (major*100+minor), and its name "
            "promises "
            f"{rule.lowest} to {rule.highest}"
        )
    if rule.refuse_above is not None and (not revision_set(max_rev) or max_rev > rule.refuse_above):
        problems.append(
            f"it accepts chip revisions above {rule.refuse_above}, which its name excludes"
        )
    flash_code = app[3] >> 4
    if FLASH_CODES.get(rule.flash_size) != flash_code or image.get("flash_size") != rule.flash_size:
        problems.append(f"it was built for another flash size than {rule.flash_size}")
    if bool(image.get("psram")) != rule.psram:
        problems.append(
            f"its PSRAM setting is not its board's ({'with' if rule.psram else 'without'} PSRAM)"
        )
    embedded = app[48:80].split(b"\0", 1)[0].decode("utf-8", "replace")
    if embedded != version:
        problems.append(
            f"the image carries version '{embedded}', and the manifest says '{version}'"
        )
    if embedded == "1":
        problems.append(
            "its version is '1', which ESP-IDF gives a build with no PROJECT_VER whose "
            "git describe failed: no board, page or tool could tell it from another build"
        )
    if image.get("network_built_in") is not False:
        problems.append("it has a Wi-Fi network built in")

    if "parts" in files:
        problems += check_parts(files["parts"], app, files.get("factory"), image)
    return problems


def check_parts(
    parts: bytes, app: bytes, factory: bytes | None, image: dict[str, Any]
) -> list[str]:
    problems: list[str] = []
    try:
        archive = zipfile.ZipFile(io.BytesIO(parts))
        members = {info.filename: archive.read(info) for info in archive.infolist()}
    except zipfile.BadZipFile as error:
        return [f"its parts.zip cannot be read: {error}"]
    flash_args = members.get("flash_args", b"").decode("utf-8", "replace").splitlines()
    if not flash_args or not flash_args[0].startswith("--"):
        return ["its parts.zip has no flash_args that starts with esptool's options"]
    regions = []
    for line in (line.strip() for line in flash_args[1:]):
        if not line:
            continue
        offset, _, member = line.partition(" ")
        if member not in members:
            problems.append(
                f"its parts.zip's flash_args names {member}, which the archive does not hold"
            )
            continue
        regions.append((int(offset, 16), members[member]))
    if problems or not regions:
        return problems or ["its parts.zip's flash_args writes nothing"]
    if sum(1 for _, data in regions if data == app) != 1:
        problems.append("its parts.zip does not write the app image beside it exactly once")
    tables = [data for offset, data in regions if offset == 0x8000]
    slot = smallest_slot(tables[0]) if tables else None
    if slot is None:
        problems.append("its parts.zip writes no partition table with an app slot at 0x8000")
    else:
        if len(app) > slot:
            problems.append(
                f"its app is {len(app):,} bytes, and the smallest slot of its table holds {slot:,}"
            )
        if image.get("slot_bytes") != slot:
            problems.append(f"the manifest's slot_bytes is not its table's smallest slot, {slot:,}")
    if factory is not None and lay_out(regions) != factory:
        problems.append("its parts.zip, laid out as its flash_args says, is not its factory image")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument(
        "--allow-missing", action="store_true", help="check only the images that are there"
    )
    args = parser.parse_args(argv)
    path = args.directory / MANIFEST_NAME
    try:
        manifest = json.loads(path.read_text("utf-8"))
    except (OSError, ValueError) as error:
        print(f"::error::{path} cannot be read: {error}")
        return 1
    images = manifest.get("images") or []
    failed = False
    names = [str(image.get("name", "")) for image in images]
    if len(set(names)) != len(names):
        print("::error::the manifest names an image more than once")
        failed = True
    for missing in sorted(set(IMAGES) - set(names)):
        if not args.allow_missing:
            print(f"::error::{missing} is not in the manifest: every release publishes it")
            failed = True
    for image in images:
        problems = check_image(args.directory, image)
        if problems:
            failed = True
            for problem in problems:
                print(f"::error::{image.get('name')}: {problem}")
        else:
            print(
                f"OK  {image['name']} {image['version']}: "
                f"{image['files']['app']['size']:,}-byte app, "
                f"{len(image.get('parts') or [])} regions"
            )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
