#!/usr/bin/env python3
"""Update Hearth sink boards over the network, and report how each update ended.

A hearth_sink board on the two-slot flash layout takes a new application image
over its network: PUT /firmware writes the slot that is not running, the board
restarts into it, and the new image stays on trial until it has run well for a
while. A reset during the trial, or a trial that fails, boots the image before
it again (planning/esp32-ota.md). This is the host side of that. Standard
library only; the zeroconf package is used for --all when it is installed.

    python tools/hearth/ota.py push (--build-dir DIR | IMAGE | --release TAG | --run RUN_ID)
                                    (--host H ... | --all)
                                    [--yes] [--force] [--timeout SECONDS] [--download-dir DIR]
    python tools/hearth/ota.py status [--host H ... | --all]
    python tools/hearth/ota.py restart --host H
    python tools/hearth/ota.py rollback --host H
    python tools/hearth/ota.py cancel --host H
    python tools/hearth/ota.py coredump --host H [--out FILE] [--elf ELF] [--erase]
    python tools/hearth/ota.py log --host H [--follow]

H is a board's IP address or its mDNS name (hearth-eb2c64.local), followed by
:PORT when the port is not 80. The firmware routes answer only those two kinds
of name, not a name a router hands out. --all finds boards by their
_sendspin._tcp service and uses each one's IPv4 address; a build without the
Sendspin player advertises no service, so its boards have to be named. status
with no board named looks for boards the same way.

push reads the image file once, and sends the bytes it checked:

1. The image. With --build-dir, project_description.json names the app binary
   and the target, partition_table/partition-table.bin is the layout, and
   sdkconfig says whether a Wi-Fi network is built into the image. The tool
   walks the image's segments as the bootloader does and checks its checksum
   and the SHA-256 the build appended. If either does not match, it stops
   before it contacts any board. The SHA-256 of the whole file goes with the
   upload as its Content-Digest (RFC 9530), which the board checks as well.
2. The pre-flight, from GET /hardware, /firmware and /status. A board is
   refused, with the reason, when the image is for another chip or for chip
   revisions that exclude this one; it is another project; the build's
   partition table is not the board's (changing it takes one USB flash); the
   image is larger than the slot or built for another flash size; the running
   image is on trial or an update is already under way; the board has one app
   slot; or the board's only network is built into its image and the new
   image has none. An image whose network is not known, such as a bare .bin,
   is taken to have none unless --force is given. A board that already runs
   the image (the same ELF SHA-256) is skipped unless --force is given. A
   board that is playing is asked about on a terminal and refused without
   one; --yes answers yes.
3. The upload: PUT /firmware, with a progress line.
4. The wait: GET /firmware every 1.5 s through the restart, for up to
   --timeout seconds (360 by default), until the board runs the new image
   and has accepted it (updated, with the image SHA-256 the board reports
   compared to the file's), runs the previous image with its last update
   rolled back (rolled back, and why), or has not answered (did not come
   back, and what to try).

Boards are updated one at a time. push stops at the first board that rolls
back or does not come back, and the boards after it are not touched.

push --release TAG and --run RUN_ID take published images instead of a build
(planning/esp32-ota.md, O8). A release's hearth-sink-manifest.json lists each
board's image with its chip, flash size, PSRAM, revision range and partition
table; each board gets the one that matches its /hardware and /firmware, and a
board no image fits is named and skipped. The image is downloaded then, from
the GitHub API ("latest" is the newest release that publishes sink firmware),
and checked against the manifest's SHA-256 and the release's SHA512SUMS before
it is sent. --run downloads a CI run's esp32-firmware artifact whole with the
GitHub CLI (gh), so a pull request's firmware can go onto a board with no
local build.

status prints each board's running and other slot, mode, trial, last update,
core dump and network. restart, rollback and cancel send POST /restart,
PUT /firmware/rollback and PUT /firmware/mode "normal" (which leaves flash mode
by restarting into the running image), and print the board's answer.

coredump saves the core dump the board's last crash left (GET
/firmware/coredump) to FILE, coredump-<board>.bin by default. With --elf, the
ELF of the image that wrote it, it checks that the ELF is that image by the
ELF SHA-256 the dump names, then runs ESP-IDF's esp_coredump on the two, which
prints every task's backtrace; run it in the ESP-IDF environment. With
--erase, it erases the dump once saved (DELETE /firmware/coredump), so the
next crash is not taken for this one.

log prints the board's recent console output (GET /log). With --follow it
goes on printing what is new, asking every second, until interrupted.

Exit status: 0 when every board was updated or already ran the image, and for
the other commands when every board answered 200; 1 when a board was refused,
an upload failed, the image did not check out, or the command line was wrong;
2 when a board rolled back; 3 when a board did not come back.
"""

from __future__ import annotations

import argparse
import base64
import dataclasses
import hashlib
import http.client
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, NoReturn
from urllib.parse import urlsplit

# Exit statuses.
UPDATED = 0
REFUSED = 1
ROLLED_BACK = 2
SILENT = 3

# Timing. The wait polls GET /firmware every POLL_SECONDS. The upload's socket
# timeout is long because the board erases what the image needs before it
# reads on, and reads the whole slot back before it answers.
POLL_SECONDS = 1.5
DEFAULT_TIMEOUT = 360.0
GET_TIMEOUT = 10.0
POLL_TIMEOUT = 5.0
UPLOAD_TIMEOUT = 120.0
UPLOAD_CHUNK = 16 * 1024
# After an update is accepted, how long to wait for the board's background
# check to report the running image's SHA-256.
SHA_WAIT_SECONDS = 20.0
# After an upload broke off part-way, how long to look for the board's reason.
# The board gives up on a stalled upload after 30 s.
REFUSAL_WAIT_SECONDS = 40.0
# log --follow asks the board for what is new this often.
LOG_POLL_SECONDS = 1.0

# --all: the service a Sendspin player advertises, and how long to listen.
SERVICE = "_sendspin._tcp.local."
BROWSE_SECONDS = 3.0

# esp_image_header_t (24 bytes), esp_image_segment_header_t (8) and
# esp_app_desc_t (256), as ESP-IDF v6.1 lays them out (esp_app_format.h,
# esp_app_desc.h). The build puts esp_app_desc_t at the start of the first
# segment's data, at byte 32. The board reads these 288 bytes before it erases
# anything.
HEADER_BYTES = 24
SEGMENT_HEADER_BYTES = 8
APP_DESC_AT = HEADER_BYTES + SEGMENT_HEADER_BYTES
HEAD_BYTES = APP_DESC_AT + 256
IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
MAX_SEGMENTS = 16
# After the last segment, zeros up to a one-byte checksum that ends a 16-byte
# block. The checksum is 0xEF XORed with every byte of every segment's data
# (esp_image_format.c, process_checksum; esptool's append_checksum). When the
# header's hash_appended byte is 1, the SHA-256 of everything before it follows.
CHECKSUM_SEED = 0xEF
DIGEST_BYTES = 32
# The build cuts PROJECT_VER to 31 characters for esp_app_desc_t's version
# (esp_app_format's CMakeLists.txt), so a long `git describe --dirty` loses its
# end in the image and in what the board reports.
VERSION_CHARS = 31

# A partition table entry (esp_partition_info_t, 32 bytes). The entries end at
# the first other magic: 0xEBEB for the MD5 entry, 0xFFFF for erased flash.
PARTITION_ENTRY = struct.Struct("<HBBII16sI")
PARTITION_MAGIC = 0x50AA

# esp_chip_id_t: the target ESP-IDF builds for, and the chip as GET /hardware
# names it.
CHIPS: dict[int, tuple[str, str]] = {
    0x0000: ("esp32", "ESP32"),
    0x0002: ("esp32s2", "ESP32-S2"),
    0x0005: ("esp32c3", "ESP32-C3"),
    0x0009: ("esp32s3", "ESP32-S3"),
    0x000C: ("esp32c2", "ESP32-C2"),
    0x000D: ("esp32c6", "ESP32-C6"),
    0x0010: ("esp32h2", "ESP32-H2"),
    0x0012: ("esp32p4", "ESP32-P4"),
    0x0014: ("esp32c61", "ESP32-C61"),
    0x0017: ("esp32c5", "ESP32-C5"),
    0x0019: ("esp32h21", "ESP32-H21"),
    0x001C: ("esp32h4", "ESP32-H4"),
    0x0020: ("esp32s31", "ESP32-S31"),
}

# The Kconfig option that builds a Wi-Fi network into the image (net/wifi).
WIFI_SSID_OPTION = "CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID"


class ImageError(Exception):
    """The image or the build directory cannot be pushed; the text says why."""


class BoardError(Exception):
    """A board did not answer, or answered something this tool cannot use."""


class UsageError(Exception):
    """The command line cannot be carried out; the text says why."""


# --- the image ---------------------------------------------------------------


@dataclass(frozen=True)
class Partition:
    label: str
    type: int
    subtype: int
    offset: int
    size: int

    def describe(self) -> str:
        kind = {0: "app", 1: "data"}.get(self.type, f"type {self.type:#04x}")
        return f"{kind} {self.subtype:#04x} at {self.offset:#x}, {self.size:#x} bytes"


@dataclass(frozen=True)
class Image:
    path: Path
    data: bytes = field(repr=False)
    chip_id: int
    min_rev_full: int
    max_rev_full: int
    flash_code: int
    version: str
    project: str
    idf_version: str
    built: str
    elf_sha256: str  # hex, esp_app_desc_t's app_elf_sha256
    image_sha256: str  # hex, the SHA-256 the build appended
    file_sha256: bytes  # of the whole file
    image_bytes: int  # the image's own length, the appended SHA-256 included
    # What a build directory adds; None for a bare .bin.
    partitions: tuple[Partition, ...] | None = None
    wifi_built_in: bool | None = None
    build_version: str | None = None

    @property
    def target(self) -> str | None:
        return CHIPS[self.chip_id][0] if self.chip_id in CHIPS else None

    @property
    def chip(self) -> str:
        return chip_name(self.chip_id)

    @property
    def flash_bytes(self) -> int | None:
        return (1 << self.flash_code) << 20 if self.flash_code <= 7 else None

    @property
    def content_digest(self) -> str:
        return "sha-256=:" + base64.b64encode(self.file_sha256).decode("ascii") + ":"


def chip_name(chip_id: int) -> str:
    return CHIPS[chip_id][1] if chip_id in CHIPS else f"chip ID {chip_id}"


def with_article(name: str) -> str:
    """The name after "a" or "an": an ESP32-S3, a chip ID 99."""
    return ("an " if name.startswith("ESP") or name[:1].upper() in "AEIOU" else "a ") + name


def revision_text(full: int) -> str:
    return f"v{full // 100}.{full % 100}"


def revision_set(full: int) -> bool:
    """ESP-IDF's IS_FIELD_SET: 0 and 65535 both mean the build set no maximum."""
    return full not in (0, 0xFFFF)


def revision_range(image: Image) -> str:
    low = revision_text(image.min_rev_full)
    if revision_set(image.max_rev_full):
        return f"chip revision {low} to {revision_text(image.max_rev_full)}"
    return f"chip revision {low} or newer"


def flash_text(image: Image) -> str:
    return f"{1 << image.flash_code} MB" if image.flash_bytes else "an unknown size"


def c_string(data: bytes, at: int, size: int) -> str:
    """A fixed-size text field of esp_app_desc_t, up to its terminator."""
    return data[at : at + size].split(b"\0", 1)[0].decode("utf-8", "replace")


def xor_of(data: bytes | memoryview) -> int:
    """The XOR of every byte of `data`, folded in halves rather than a byte at a time."""
    value = int.from_bytes(data, "little")
    width = len(data)
    while width > 1:
        half = (width + 1) // 2
        value = (value & ((1 << (8 * half)) - 1)) ^ (value >> (8 * half))
        width = half
    return value & 0xFF


def parse_image(data: bytes, path: Path) -> Image:
    """The image in `data`, checked as the bootloader checks it, or ImageError."""
    if len(data) < HEAD_BYTES:
        raise ImageError(f"it is {len(data)} bytes, too short to be an application image")
    if data[0] != IMAGE_MAGIC:
        raise ImageError(
            "it is not an ESP-IDF application image (its first byte is not 0xE9); push "
            "ac3forge_hearth_sink.bin, not the merged factory image or the ELF"
        )
    segments = data[1]
    if not 1 <= segments <= MAX_SEGMENTS:
        raise ImageError(f"its header names {segments} segments, which no ESP-IDF application has")
    if struct.unpack_from("<I", data, APP_DESC_AT)[0] != APP_DESC_MAGIC:
        raise ImageError(
            "it has no application description where ESP-IDF puts one, so it is not an "
            "application image"
        )

    view = memoryview(data)
    at = HEADER_BYTES
    checksum = CHECKSUM_SEED
    for index in range(segments):
        if at + SEGMENT_HEADER_BYTES > len(data):
            raise ImageError(f"it ends before segment {index}'s header: it is cut short or damaged")
        length = struct.unpack_from("<I", data, at + 4)[0]
        at += SEGMENT_HEADER_BYTES
        if length % 4:
            raise ImageError(f"segment {index} is {length} bytes, not a whole number of words")
        if at + length > len(data):
            raise ImageError(f"it ends inside segment {index}: it is cut short or damaged")
        checksum ^= xor_of(view[at : at + length])
        at += length
    padded = (at + 1 + 15) & ~15  # the checksum byte ends a 16-byte block
    if padded > len(data):
        raise ImageError("it ends before its checksum: it is cut short")
    if data[23] != 1:
        raise ImageError(
            "it carries no SHA-256 of itself (hash_appended is not set), so damage to it could "
            "not be found; the board refuses such an image too"
        )
    end = padded + DIGEST_BYTES
    if end > len(data):
        raise ImageError("it ends before the SHA-256 the build appended: it is cut short")
    appended = data[padded:end]
    if hashlib.sha256(view[:padded]).digest() != appended:
        raise ImageError(
            "its SHA-256 does not match the one the build appended to it: the file is damaged"
        )
    if data[padded - 1] != checksum:
        raise ImageError(
            f"its checksum byte is {data[padded - 1]:#04x}, and its segments add up to "
            f"{checksum:#04x}: the image is damaged"
        )

    min_rev, max_rev = struct.unpack_from("<HH", data, 15)
    return Image(
        path=path,
        data=data,
        chip_id=struct.unpack_from("<H", data, 12)[0],
        min_rev_full=min_rev,
        max_rev_full=max_rev,
        flash_code=data[3] >> 4,
        version=c_string(data, APP_DESC_AT + 16, 32),
        project=c_string(data, APP_DESC_AT + 48, 32),
        idf_version=c_string(data, APP_DESC_AT + 112, 32),
        built=f"{c_string(data, APP_DESC_AT + 96, 16)} {c_string(data, APP_DESC_AT + 80, 16)}",
        elf_sha256=data[APP_DESC_AT + 144 : APP_DESC_AT + 176].hex(),
        image_sha256=appended.hex(),
        file_sha256=hashlib.sha256(data).digest(),
        image_bytes=end,
    )


def read_image_file(path: Path) -> Image:
    if path.is_dir():
        raise ImageError(f"{path} is a directory; name a build directory with --build-dir")
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ImageError(f"cannot read {path}: {error.strerror or error}") from None
    try:
        return parse_image(data, path)
    except ImageError as error:
        raise ImageError(f"{path}: {error}") from None


def read_partition_table(path: Path) -> tuple[Partition, ...]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ImageError(f"cannot read the build's partition table {path}: {error}") from None
    entries: list[Partition] = []
    for at in range(0, len(data) - PARTITION_ENTRY.size + 1, PARTITION_ENTRY.size):
        magic, kind, subtype, offset, size, label, _flags = PARTITION_ENTRY.unpack_from(data, at)
        if magic != PARTITION_MAGIC:
            break
        name = label.split(b"\0", 1)[0].decode("utf-8", "replace")
        entries.append(Partition(name, kind, subtype, offset, size))
    if not entries:
        raise ImageError(f"{path} holds no partition entries")
    return tuple(entries)


def read_wifi_built_in(sdkconfig: Path) -> bool | None:
    """Whether the build's sdkconfig builds a Wi-Fi network in; None when there is none to read."""
    try:
        text = sdkconfig.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith(WIFI_SSID_OPTION + "="):
            return line.split("=", 1)[1].strip() not in ("", '""')
    return False  # no Wi-Fi in this build at all


def read_build_dir(build_dir: Path) -> Image:
    description_path = build_dir / "project_description.json"
    try:
        description = json.loads(description_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ImageError(
            f"{build_dir} has no project_description.json: name the directory idf.py built in"
        ) from None
    except (OSError, ValueError) as error:
        raise ImageError(f"cannot read {description_path}: {error}") from None
    if not isinstance(description, dict) or not description.get("app_bin"):
        raise ImageError(f"{description_path} names no app_bin: the build made no app image")
    app_bin = Path(str(description["app_bin"]))
    image = read_image_file(app_bin if app_bin.is_absolute() else build_dir / app_bin)
    target = description.get("target")
    if target and image.target != target:
        raise ImageError(
            f"{build_dir} was built for {target}, and its image {image.path.name} is for "
            f"{with_article(image.chip)}"
        )
    version = description.get("project_version")
    return dataclasses.replace(
        image,
        partitions=read_partition_table(build_dir / "partition_table" / "partition-table.bin"),
        wifi_built_in=read_wifi_built_in(build_dir / "sdkconfig"),
        build_version=version if isinstance(version, str) else None,
    )


def describe_image(image: Image) -> list[str]:
    lines = [
        f"image: {image.path}",
        f"  {image.project} {image.version}, built {image.built} with ESP-IDF {image.idf_version}",
        f"  for {with_article(image.chip)} of {revision_range(image)}, with {flash_text(image)} "
        "of flash",
        f"  {len(image.data):,} bytes; the SHA-256 the build appended checks out "
        f"({image.image_sha256[:16]}...)",
        f"  ELF SHA-256 {image.elf_sha256[:16]}...",
    ]
    if image.image_bytes != len(image.data):
        lines.append(f"  {len(image.data) - image.image_bytes:,} bytes follow the image's end")
    if image.partitions is None:
        lines.append("  network built in: not known from a bare .bin")
    elif image.wifi_built_in is None:
        lines.append("  network built in: not known (no sdkconfig in the build directory)")
    else:
        lines.append(f"  network built in: {'yes' if image.wifi_built_in else 'none'}")
    if image.build_version and image.build_version[:VERSION_CHARS] != image.version:
        lines.append(
            f"  the build directory says {image.build_version}: the image may be from an earlier "
            "build"
        )
    return lines


# --- boards ------------------------------------------------------------------------


def parse_host(text: str) -> tuple[str, int]:
    """A --host value as (name or address, port)."""
    value = text.strip()
    if value.lower().startswith("http://"):
        value = value[len("http://") :]
    value = value.rstrip("/")
    try:
        parts = urlsplit("//" + value)
        port = parts.port or 80
    except ValueError:
        raise UsageError(f"--host {text}: the port is not a number from 1 to 65535") from None
    if not parts.hostname or parts.path or parts.query or parts.username:
        raise UsageError(
            f"--host {text}: name a board by its IP address or its mDNS name, with :PORT when "
            "the port is not 80"
        )
    return parts.hostname, port


class Board:
    """One board's HTTP routes."""

    def __init__(self, host: str, label: str | None = None) -> None:
        self.host = host
        self.hostname, self.port = parse_host(host)
        self.label = label or host

    def __str__(self) -> str:
        return self.label

    def request(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
        timeout: float = GET_TIMEOUT,
    ) -> tuple[int, str]:
        """(status, body text); OSError or http.client.HTTPException when no answer comes."""
        connection = http.client.HTTPConnection(self.hostname, self.port, timeout=timeout)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read().decode("utf-8", "replace").strip()
        finally:
            connection.close()

    def fetch(self, path: str, timeout: float = GET_TIMEOUT) -> tuple[int, bytes, dict[str, str]]:
        """GET as bytes, with the reply's headers; errors as request() raises them."""
        connection = http.client.HTTPConnection(self.hostname, self.port, timeout=timeout)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            return response.status, response.read(), dict(response.getheaders())
        finally:
            connection.close()

    def get_json(self, path: str, timeout: float = GET_TIMEOUT) -> dict[str, Any]:
        try:
            status, text = self.request("GET", path, timeout=timeout)
        except (OSError, http.client.HTTPException) as error:
            raise BoardError(f"GET {path} got no answer: {error_text(error)}") from None
        if status != 200:
            raise BoardError(f"GET {path} answered {status}: {text}{http_hint(status)}")
        try:
            value = json.loads(text)
        except ValueError:
            raise BoardError(f"GET {path} answered something that is not JSON") from None
        if not isinstance(value, dict):
            raise BoardError(f"GET {path} answered JSON that is not an object")
        return value


def error_text(error: BaseException) -> str:
    text = str(error)
    return f"{type(error).__name__}: {text}" if text else type(error).__name__


def http_hint(status: int) -> str:
    if status == 403:
        return " (name the board by its IP address or its .local name)"
    if status == 404:
        return " (the image it runs takes no updates over the network)"
    return ""


def part(value: Any, key: str) -> dict[str, Any]:
    """value[key] when it is a JSON object; {} for null, absent or anything else."""
    found = value.get(key) if isinstance(value, dict) else None
    return found if isinstance(found, dict) else {}


def text_of(value: Any, key: str) -> str:
    found = value.get(key) if isinstance(value, dict) else None
    return found if isinstance(found, str) else ""


def number_of(value: Any, key: str) -> int:
    found = value.get(key) if isinstance(value, dict) else None
    return found if isinstance(found, int) and not isinstance(found, bool) else 0


def parse_revision(text: str) -> int | None:
    """GET /hardware's "revision", "M.m", as M * 100 + m."""
    major, dot, minor = text.strip().lstrip("v").partition(".")
    if not dot or not major.isdigit() or not minor.isdigit():
        return None
    return int(major) * 100 + int(minor)


def board_partitions(firmware: dict[str, Any]) -> tuple[Partition, ...] | None:
    entries = firmware.get("partitions")
    if not isinstance(entries, list):
        return None
    return tuple(
        Partition(
            text_of(entry, "label"),
            number_of(entry, "type"),
            number_of(entry, "subtype"),
            number_of(entry, "offset"),
            number_of(entry, "size"),
        )
        for entry in entries
    )


def table_difference(build: tuple[Partition, ...], board: tuple[Partition, ...]) -> str | None:
    """How the build's partition table differs from the board's, or None when it does not."""
    theirs = {entry.label: entry for entry in board}
    ours = {entry.label: entry for entry in build}
    for entry in build:
        other = theirs.get(entry.label)
        if other is None:
            return f"the build has {entry.label}, {entry.describe()}, and the board has none"
        if other != entry:
            return (
                f"the build's {entry.label} is {entry.describe()}, and the board's is "
                f"{other.describe()}"
            )
    for entry in board:
        if entry.label not in ours:
            return f"the board has {entry.label}, {entry.describe()}, and the build has none"
    return None


def refusal(
    image: Image, hardware: dict[str, Any], firmware: dict[str, Any], force: bool
) -> str | None:
    """Why this board must not take `image`, or None when it may."""
    board_chip = text_of(hardware, "chip") or text_of(hardware, "target") or "unknown chip"
    if image.target != text_of(hardware, "target") or (
        text_of(hardware, "chip") and image.chip != board_chip
    ):
        return (
            f"this image is for {with_article(image.chip)}, and this board is "
            f"{with_article(board_chip)}"
        )

    revision = parse_revision(text_of(hardware, "revision"))
    if revision is None:
        return f"the board reports its chip revision as {hardware.get('revision')!r}, not as M.m"
    if revision < image.min_rev_full:
        return (
            f"this image needs chip revision {revision_text(image.min_rev_full)} or newer, and "
            f"this chip is {revision_text(revision)}"
        )
    if revision_set(image.max_rev_full) and revision > image.max_rev_full:
        return (
            f"this image runs on chip revisions up to {revision_text(image.max_rev_full)}, and "
            f"this chip is {revision_text(revision)}"
        )

    project = text_of(hardware, "project")
    if project and image.project != project:
        return f"this image is {image.project or 'unnamed'}, and the board runs {project}"

    if image.partitions is not None:
        theirs = board_partitions(firmware)
        if theirs is None:
            difference: str | None = "the board does not report its partition table"
        else:
            difference = table_difference(image.partitions, theirs)
        if difference:
            return (
                f"the build's partition table is not the board's: {difference}. An update cannot "
                "change the partition table: this needs one USB flash"
            )

    slot_bytes = number_of(firmware, "slot_bytes")
    if slot_bytes and len(image.data) > slot_bytes:
        return f"the image is {len(image.data):,} bytes, and the board's slot holds {slot_bytes:,}"

    flash_bytes = number_of(firmware, "flash_bytes")
    if flash_bytes and image.flash_bytes != flash_bytes:
        return (
            f"this image was built for {flash_text(image)} of flash, and the board's is set for "
            f"{flash_bytes / (1 << 20):g} MB"
        )

    if text_of(part(firmware, "running"), "state") == "trial" or part(firmware, "trial"):
        return (
            "the running image is still on trial; wait until the board has accepted it "
            "(ota.py status), then push again"
        )
    if part(firmware, "upload"):
        return "an update is already under way on this board"
    if not part(firmware, "other"):
        return (
            "this board's partition table has one app slot: move it to the two-slot table with "
            "one USB flash first"
        )

    if text_of(firmware, "network") == "built-in" and not image.wifi_built_in:
        if image.wifi_built_in is False:
            return (
                "the board's only network is built into the image it runs, and the new image has "
                "none: it would not rejoin the network, and would roll back. Store the network "
                "on the board (Improv, or PUT /network), or build the network into the image"
            )
        if not force:
            return (
                "the board's only network is built into the image it runs, and this image does "
                "not say whether it has one: push with --build-dir, or give --force if the "
                "image has the network built in"
            )
    return None


def is_playing(status: dict[str, Any]) -> bool:
    if text_of(status, "state") in ("playing", "opening"):
        return True
    return text_of(part(status, "sendspin"), "playing") not in ("", "idle")


def slot_text(slot: dict[str, Any]) -> str:
    label = text_of(slot, "label") or "?"
    state = text_of(slot, "state") or "unknown"
    if state == "empty":
        return f"{label}  empty"
    intact = slot.get("intact")
    checked = "not checked yet" if intact is None else "intact" if intact else "does not check out"
    return f"{label}  {text_of(slot, 'version') or '(no version)'}  {state}, {checked}"


def waiting_for(trial: dict[str, Any]) -> list[str]:
    items = trial.get("waiting_for")
    return [item for item in items if isinstance(item, str)] if isinstance(items, list) else []


def trial_text(trial: dict[str, Any]) -> str:
    healthy = number_of(trial, "healthy_for_ms") // 1000
    hold = number_of(trial, "hold_ms") // 1000
    text = f"healthy for {healthy}s of {hold}s"
    if waiting_for(trial):
        text += f", waiting for {', '.join(waiting_for(trial))}"
    return text + f" ({number_of(trial, 'remaining_ms') // 1000}s left)"


def last_update_text(last: dict[str, Any]) -> str:
    text = f"{text_of(last, 'version') or '(no version)'}: {text_of(last, 'result') or 'unknown'}"
    reason = text_of(last, "reason")
    return f"{text} ({reason})" if reason else text


# --- output ----------------------------------------------------------------------------


class Console:
    """Lines for a person: a status line a terminal rewrites in place, and lines that stay.

    Off a terminal, a status line is printed only when its key changes, so a log
    gets a line for each step rather than one for each poll.
    """

    def __init__(self) -> None:
        self.width = 0  # the status line's length on the terminal, while one is open
        self.key: object = None

    def say(self, text: str) -> None:
        self.close()
        print(text, flush=True)

    def status(self, text: str, key: object) -> None:
        if sys.stdout.isatty():
            # One row: \r goes back only to the start of the row a long line
            # wrapped onto.
            text = text[: max(20, shutil.get_terminal_size().columns - 1)]
            sys.stdout.write("\r" + text + " " * max(0, self.width - len(text)))
            sys.stdout.flush()
            self.width = len(text)
        elif key != self.key:
            print(text, flush=True)
        self.key = key

    def close(self) -> None:
        if self.width:
            sys.stdout.write("\n")
            sys.stdout.flush()
        self.width = 0
        self.key = None


console = Console()


def say(board: Board, text: str) -> None:
    console.say(f"{board}: {text}")


# --- push --------------------------------------------------------------------------------


@dataclass
class PushOptions:
    yes: bool = False
    force: bool = False
    timeout: float = DEFAULT_TIMEOUT


@dataclass
class Sent:
    status: int | None  # None when no answer came
    text: str
    sent: int


def upload(board: Board, image: Image) -> Sent:
    """PUT /firmware with the image, a chunk at a time, and the board's answer."""
    data = image.data
    total = len(data)
    sent = 0
    connection = http.client.HTTPConnection(board.hostname, board.port, timeout=UPLOAD_TIMEOUT)
    try:
        connection.putrequest("PUT", "/firmware")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(total))
        connection.putheader("Content-Digest", image.content_digest)
        connection.endheaders()
        view = memoryview(data)
        while sent < total:
            chunk = view[sent : sent + UPLOAD_CHUNK]
            connection.send(chunk)
            sent += len(chunk)
            console.status(
                f"{board}: sent {sent:,} of {total:,} bytes ({100 * sent // total}%)",
                4 * sent // total,
            )
        console.say(f"{board}: sent; the board reads the slot back and checks it before it answers")
        response = connection.getresponse()
        return Sent(response.status, response.read().decode("utf-8", "replace").strip(), sent)
    except (OSError, http.client.HTTPException) as error:
        console.close()
        return Sent(None, error_text(error), sent)
    finally:
        connection.close()


def refusal_after_break(board: Board) -> str | None:
    """After an upload broke off, the board's reason once it has given the upload up.

    An upload that reached the board put it in flash mode, so a board in
    normal mode never had this one, and its last update is an older one.
    """
    deadline = time.monotonic() + REFUSAL_WAIT_SECONDS
    while True:
        try:
            firmware: dict[str, Any] | None = board.get_json("/firmware", timeout=POLL_TIMEOUT)
        except BoardError:
            firmware = None
        if firmware is not None and not part(firmware, "upload"):
            last = part(firmware, "last_update")
            in_flash_mode = text_of(firmware, "mode") == "flash"
            if in_flash_mode and text_of(last, "result") in ("refused", "failed"):
                return text_of(last, "reason") or text_of(last, "result")
            return None
        if time.monotonic() >= deadline:
            return None
        time.sleep(POLL_SECONDS)


@dataclass
class Outcome:
    code: int
    text: str


def is_new_image(running: dict[str, Any], image: Image, slot: str | None) -> bool:
    """Whether `running` is the image this push wrote: its ELF, from the slot it went to."""
    same_elf = text_of(running, "elf_sha256").lower() == image.elf_sha256
    return same_elf and (slot is None or text_of(running, "label") == slot)


def wait_for(
    board: Board,
    image: Image,
    slot: str | None,
    timeout: float,
    reply_lost: bool = False,
    last_before: dict[str, Any] | None = None,
) -> Outcome:
    """Poll GET /firmware until the board has decided about the image written to `slot`.

    Until the board restarts it answers in flash mode, and what it says then
    about the last update is from before this one, so nothing is judged from
    it. When the upload's answer was lost, a board in flash mode with no upload
    running has refused the image, and a board in normal mode that still
    reports `last_before`, the last update from the pre-flight, never took it.
    """
    deadline = time.monotonic() + timeout
    last: dict[str, Any] | None = None
    sha_deadline: float | None = None
    while True:
        try:
            firmware: dict[str, Any] | None = board.get_json("/firmware", timeout=POLL_TIMEOUT)
        except BoardError:
            firmware = None
        now = time.monotonic()
        running = part(firmware, "running")
        last_update = part(firmware, "last_update")
        if firmware is None:
            console.status(f"{board}: no answer yet", "no answer")
        elif text_of(firmware, "mode") == "flash":
            last = firmware
            if reply_lost and not part(firmware, "upload"):
                reason = text_of(last_update, "reason") or "the board did not say why"
                return Outcome(REFUSED, f"refused: {reason}")
            console.status(f"{board}: still in flash mode; it restarts next", "flash mode")
        elif is_new_image(running, image, slot):
            last = firmware
            trial = part(firmware, "trial")
            state = text_of(running, "state")
            if state == "trial" or trial:
                console.status(
                    f"{board}: on trial: {trial_text(trial)}",
                    (tuple(waiting_for(trial)), number_of(trial, "healthy_for_ms") // 10_000),
                )
            elif state == "valid":
                if text_of(running, "image_sha256") or running.get("intact") is False:
                    return Outcome(UPDATED, updated_text(running, image))
                if sha_deadline is None:
                    sha_deadline = now + SHA_WAIT_SECONDS
                if now >= sha_deadline or now >= deadline:
                    return Outcome(UPDATED, updated_text(running, image))
        else:
            last = firmware
            if reply_lost and not part(firmware, "upload") and last_update == (last_before or {}):
                return Outcome(
                    REFUSED,
                    "failed: the board runs the image it ran before and says nothing of this "
                    "upload, so it did not take it",
                )
            if text_of(last_update, "result") == "rolled back":
                return Outcome(
                    ROLLED_BACK,
                    f"rolled back: {text_of(last_update, 'version') or image.version} did not "
                    f"last, and the board runs {text_of(running, 'version')} from "
                    f"{text_of(running, 'label')} again. The board says: "
                    f"{text_of(last_update, 'reason') or 'no reason given'}",
                )
        if now >= deadline:
            return Outcome(SILENT, silent_text(board, image, slot, timeout, last))
        time.sleep(POLL_SECONDS)


def updated_text(running: dict[str, Any], image: Image) -> str:
    text = f"updated: runs {text_of(running, 'version')} from {text_of(running, 'label')}, accepted"
    reported = text_of(running, "image_sha256").lower()
    if running.get("intact") is False:
        return text + "; but the board's own check of the running slot does not check out"
    if not reported:
        return text + "; the board has not reported the image's SHA-256 yet (ota.py status)"
    if reported == image.image_sha256:
        return text + "; the image's SHA-256 on the board matches the file's"
    return (
        text + f"; but the board reports the image's SHA-256 as {reported}, and the file's is "
        f"{image.image_sha256}"
    )


def silent_text(
    board: Board, image: Image, slot: str | None, timeout: float, last: dict[str, Any] | None
) -> str:
    running = part(last, "running")
    if last is None:
        head = f"did not come back within {timeout:g} s."
    elif text_of(last, "mode") == "flash":
        head = f"still in flash mode after {timeout:g} s: it has not restarted into the new image."
    elif is_new_image(running, image, slot):
        state = text_of(running, "state")
        head = (
            f"still runs the new image on trial after {timeout:g} s."
            if state == "trial" or part(last, "trial")
            else f"runs the new image after {timeout:g} s ({state}), and has not accepted it."
        )
    else:
        head = (
            f"runs {text_of(running, 'version')} from {text_of(running, 'label')} after "
            f"{timeout:g} s, and has not said how the update to {slot or 'the other slot'} ended."
        )
    return "\n".join(
        [
            head,
            "  What to try:",
            "  - Cycle the board's power while the new image is on trial: a reset before the",
            "    image is accepted boots the previous one. Then look again:",
            f"    python tools/hearth/ota.py status --host {board.host}",
            "  - Failing that, USB: esptool write-flash @flash_args from the build directory,",
            "    holding BOOT while pressing RESET if the board does not enter download mode.",
        ]
    )


def confirm_stop(board: Board, yes: bool) -> str | None:
    """None when a playing board may be stopped for the update; otherwise why not."""
    if yes:
        say(board, "is playing; --yes stops it for the update")
        return None
    if sys.stdin is None or not sys.stdin.isatty():
        return "it is playing, and there is no terminal to ask on: --yes stops it and updates"
    console.close()
    try:
        answer = input(f"{board} is playing; stop it to update? [y/N] ")
    except EOFError:
        answer = ""
    if answer.strip().lower() in ("y", "yes"):
        return None
    return "it is playing, and the answer was no"


def written_text(answer: Any, reply: str, image: Image) -> str:
    if not isinstance(answer, dict):
        return f"written and checked: {reply}"
    text = f"written to {text_of(answer, 'slot') or 'the other slot'} and checked"
    reported = text_of(answer, "sha256").lower()
    if reported and reported != image.file_sha256.hex():
        text += f"; the board's SHA-256 of what it received is {reported}, not the file's"
    return text + "; the board restarts into it, on trial"


def push_one(board: Board, image: Image, options: PushOptions) -> int:
    try:
        hardware = board.get_json("/hardware")
        firmware = board.get_json("/firmware")
    except BoardError as error:
        say(board, f"refused: {error}")
        return REFUSED
    running = part(firmware, "running")
    say(
        board,
        f"{text_of(hardware, 'chip') or '?'} v{text_of(hardware, 'revision') or '?'}, runs "
        f"{text_of(running, 'version') or '?'} from {text_of(running, 'label') or '?'} "
        f"({text_of(running, 'state') or '?'})",
    )
    reason = refusal(image, hardware, firmware, options.force)
    if reason:
        say(board, f"refused: {reason}")
        return REFUSED
    if text_of(running, "elf_sha256").lower() == image.elf_sha256 and not options.force:
        say(board, "already runs this image (the same ELF SHA-256); skipped. --force sends it")
        return UPDATED
    try:
        status = board.get_json("/status")
    except BoardError as error:
        say(board, f"refused: {error}")
        return REFUSED
    if is_playing(status):
        reason = confirm_stop(board, options.yes)
        if reason:
            say(board, f"refused: {reason}")
            return REFUSED

    slot = text_of(part(firmware, "other"), "label") or None
    say(
        board,
        f"sending {len(image.data):,} bytes to {slot or 'the other slot'}; the board erases what "
        "the image needs first",
    )
    sent = upload(board, image)
    if sent.status is None and sent.sent < len(image.data):
        say(board, f"the upload broke off after {sent.sent:,} bytes: {sent.text}")
        reason = refusal_after_break(board)
        if reason:
            say(board, f"refused: {reason}")
        else:
            say(
                board,
                "failed: the board gave no reason. If it is in flash mode (ota.py status), a new "
                "push, ota.py cancel, or ten minutes restarts it into the image it runs",
            )
        return REFUSED
    if sent.status is None:
        say(board, f"no answer to the upload ({sent.text}); looking for the board")
    elif sent.status != 200:
        say(board, f"refused ({sent.status}): {sent.text}{http_hint(sent.status)}")
        return REFUSED
    else:
        try:
            answer = json.loads(sent.text)
        except ValueError:
            answer = None
        say(board, written_text(answer, sent.text, image))
        slot = text_of(answer, "slot") or slot

    say(board, f"waiting for it to restart and decide (up to {options.timeout:g} s)")
    outcome = wait_for(
        board,
        image,
        slot,
        options.timeout,
        reply_lost=sent.status is None,
        last_before=part(firmware, "last_update"),
    )
    say(board, outcome.text)
    return outcome.code


def push(image: Image, boards: list[Board], options: PushOptions) -> int:
    worst = UPDATED
    for index, board in enumerate(boards):
        code = push_one(board, image, options)
        if code in (ROLLED_BACK, SILENT):
            rest = boards[index + 1 :]
            if rest:
                console.say(
                    f"stopped at {board}; not touched: {', '.join(str(each) for each in rest)}"
                )
            return code
        worst = max(worst, code)
    return worst


# --- the other commands ---------------------------------------------------------------------


def show_status(board: Board) -> int:
    try:
        firmware = board.get_json("/firmware")
    except BoardError as error:
        say(board, str(error))
        return REFUSED
    trial = part(firmware, "trial")
    upload_state = part(firmware, "upload")
    last = part(firmware, "last_update")
    other = part(firmware, "other")
    if upload_state:
        upload_line = (
            f"{text_of(upload_state, 'stage')}, {number_of(upload_state, 'received'):,} of "
            f"{number_of(upload_state, 'total'):,} bytes"
        )
    else:
        upload_line = "none"
    rows = [
        ("mode", text_of(firmware, "mode") or "?"),
        ("running", slot_text(part(firmware, "running"))),
        ("other", slot_text(other) if other else "none: one app slot"),
        ("trial", trial_text(trial) if trial else "none"),
        ("upload", upload_line),
        ("last update", last_update_text(last) if last else "none"),
        ("core dump", coredump_text(firmware)),
        ("network", text_of(firmware, "network") or "?"),
    ]
    console.say(str(board))
    for name, value in rows:
        console.say(f"  {name:<12} {value}")
    return UPDATED


def coredump_source(dump: dict[str, Any], firmware: dict[str, Any]) -> str:
    """Which slot's image wrote a core dump, by the start of the ELF SHA-256 the dump keeps."""
    prefix = text_of(dump, "elf_sha256")
    if not prefix:
        return "an image the dump does not name"
    for name in ("running", "other"):
        held = part(firmware, name)
        if held and text_of(held, "elf_sha256").startswith(prefix):
            return f"{text_of(held, 'version')} in {text_of(held, 'label')}"
    return f"an image neither slot holds now (ELF SHA-256 {prefix}...)"


def coredump_text(firmware: dict[str, Any]) -> str:
    # A board keeps the key, null when there is no dump; firmware from before O4
    # has no key at all.
    if "coredump" not in firmware:
        return "not reported by this firmware"
    dump = part(firmware, "coredump")
    if not dump:
        return "none"
    words = [f"{number_of(dump, 'bytes'):,} bytes"]
    if dump.get("intact") is not True:
        words.append("which do not check out")
    if text_of(dump, "task"):
        words.append(f"{text_of(dump, 'task')} at {text_of(dump, 'pc')}")
    if text_of(dump, "reason"):
        words.append(text_of(dump, "reason"))
    words.append(f"written by {coredump_source(dump, firmware)}")
    return ", ".join(words)


def send(board: Board, method: str, path: str, body: bytes, what: str) -> int:
    headers = {"Content-Type": "text/plain"} if body else {}
    try:
        status, text = board.request(method, path, body=body, headers=headers)
    except (OSError, http.client.HTTPException) as error:
        say(board, f"{what}: no answer ({error_text(error)})")
        return REFUSED
    say(board, f"{what}: {status} {text}{http_hint(status)}")
    return UPDATED if status == 200 else REFUSED


def fetch_coredump(board: Board, out: Path | None, elf: Path | None, erase: bool) -> int:
    """Saves the board's core dump, reads it with esp_coredump when given the ELF, erases it."""
    try:
        firmware = board.get_json("/firmware")
        status, data, _ = board.fetch("/firmware/coredump", timeout=UPLOAD_TIMEOUT)
    except BoardError as error:
        say(board, str(error))
        return REFUSED
    except (OSError, http.client.HTTPException) as error:
        say(board, f"GET /firmware/coredump got no answer: {error_text(error)}")
        return REFUSED
    if status != 200:
        say(
            board,
            f"GET /firmware/coredump answered {status}: {data.decode('utf-8', 'replace').strip()}",
        )
        return REFUSED
    dump = part(firmware, "coredump")
    path = out or Path(f"coredump-{board.hostname}.bin")
    path.write_bytes(data)
    say(board, f"saved {len(data):,} bytes to {path}: {coredump_text(firmware)}")
    code = UPDATED if elf is None else decode_coredump(board, path, elf, dump)
    if erase:
        code = max(code, send(board, "DELETE", "/firmware/coredump", b"", "erase"))
    return code


def decode_coredump(board: Board, path: Path, elf: Path, dump: dict[str, Any]) -> int:
    """esp_coredump's reading of a saved dump, once the ELF is shown to be its image's."""
    try:
        digest = hashlib.sha256(elf.read_bytes()).hexdigest()
    except OSError as error:
        raise UsageError(f"--elf {elf}: {error_text(error)}") from None
    prefix = text_of(dump, "elf_sha256")
    if prefix and not digest.startswith(prefix):
        say(
            board,
            f"{elf} is not the image that wrote the dump: its ELF SHA-256 starts "
            f"{digest[: len(prefix)]}, and the dump names {prefix}",
        )
        return REFUSED
    command = [sys.executable, "-m", "esp_coredump", "info_corefile"]
    command += ["--core", str(path), "--core-format", "raw", str(elf)]
    say(board, "running " + " ".join(command[1:]))
    try:
        done = subprocess.run(command, check=False)
    except OSError as error:
        say(board, f"esp_coredump did not run: {error_text(error)}")
        return REFUSED
    if done.returncode != 0:
        say(board, f"esp_coredump exited {done.returncode}; run it in the ESP-IDF environment")
        return REFUSED
    return UPDATED


def show_log(board: Board, follow: bool) -> int:
    """GET /log from the start of what the board holds, and with --follow what comes after."""
    start = 0
    first = True
    try:
        while True:
            try:
                status, data, headers = board.fetch(f"/log?from={start}")
            except (OSError, http.client.HTTPException) as error:
                say(board, f"GET /log got no answer: {error_text(error)}")
                if not follow:
                    return REFUSED
                time.sleep(LOG_POLL_SECONDS)
                continue
            if status != 200:
                say(board, f"GET /log answered {status}: {data.decode('utf-8', 'replace').strip()}")
                return REFUSED
            by_name = {name.lower(): value for name, value in headers.items()}
            began = int(by_name.get("x-log-from", start))
            if began > start and not first:
                sys.stdout.write(f"[{began - start:,} bytes came and went before they were read]\n")
            first = False
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
            start = int(by_name.get("x-log-next", began + len(data)))
            if data:
                continue  # more may be waiting: a reply carries at most a few KB
            if not follow:
                return UPDATED
            time.sleep(LOG_POLL_SECONDS)
    except KeyboardInterrupt:
        return UPDATED


# --- finding boards ----------------------------------------------------------------------


def discover(seconds: float = BROWSE_SECONDS) -> list[Board]:
    """Every board that advertises SERVICE, by its IPv4 address."""
    try:
        from zeroconf import (  # noqa: PLC0415 - optional, and only --all needs it
            IPVersion,
            ServiceBrowser,
            ServiceStateChange,
            Zeroconf,
        )
    except ImportError:
        raise UsageError(
            "--all finds boards with the zeroconf package, which this Python does not have: "
            "python -m pip install zeroconf, or name each board with --host"
        ) from None

    names: list[str] = []

    def on_change(zeroconf: Any, service_type: str, name: str, state_change: Any) -> None:
        if state_change is ServiceStateChange.Added and name not in names:
            names.append(name)

    found: dict[str, Board] = {}
    try:
        zc = Zeroconf(ip_version=IPVersion.V4Only)
    except OSError as error:
        raise UsageError(f"--all could not listen for mDNS: {error}") from None
    try:
        browser = ServiceBrowser(zc, SERVICE, handlers=[on_change])
        time.sleep(seconds)
        browser.cancel()
        for name in list(names):
            info = zc.get_service_info(SERVICE, name, timeout=2000)
            addresses = info.parsed_addresses(IPVersion.V4Only) if info is not None else []
            if addresses:
                server = (info.server or name).rstrip(".")
                found.setdefault(addresses[0], Board(addresses[0], f"{server} ({addresses[0]})"))
    finally:
        zc.close()
    return sorted(found.values(), key=lambda board: board.label)


def boards_from(args: argparse.Namespace) -> list[Board]:
    if args.host:
        return [Board(host) for host in args.host]
    boards = discover()
    if not boards:
        raise UsageError(f"no boards found: nothing answered for {SERVICE} in {BROWSE_SECONDS:g} s")
    console.say(f"found {len(boards)} board(s): {', '.join(str(board) for board in boards)}")
    return boards


# --- published images (planning/esp32-ota.md, O8) ---------------------------------------------

REPOSITORY = "iainchesworthlabs/ac3forge"
MANIFEST_NAME = "hearth-sink-manifest.json"
FIRMWARE_ARTIFACT = "esp32-firmware"
GITHUB_API = "https://api.github.com"
DOWNLOAD_TIMEOUT = 120.0


@dataclass
class Published:
    """A published set of images: its manifest, where its files are, and SHA512SUMS's lines."""

    manifest: dict[str, Any]
    directory: Path
    source: str  # "release v0.11.0", "CI run 1234"
    sums: dict[str, str] = field(default_factory=dict)
    # A file that is not in `directory` yet, and where to fetch it.
    urls: dict[str, str] = field(default_factory=dict)


def github_request(url: str) -> urllib.request.Request:
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "ac3forge-ota.py"}
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return urllib.request.Request(url, headers=headers)


def github_get(url: str) -> bytes:
    try:
        with urllib.request.urlopen(github_request(url), timeout=DOWNLOAD_TIMEOUT) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise UsageError(f"{url} answered {error.code} {error.reason}") from None
    except (OSError, http.client.HTTPException) as error:
        raise UsageError(f"{url} got no answer: {error_text(error)}") from None


def read_sums(text: str) -> dict[str, str]:
    """SHA512SUMS's lines as sha512sum writes them: '<hex>  <name>'."""
    sums = {}
    for line in text.splitlines():
        digest, _, name = line.strip().partition(" ")
        if digest and name:
            sums[name.strip().lstrip("*")] = digest.lower()
    return sums


def fetch_release(tag: str, into: Path) -> Published:
    """The manifest and SHA512SUMS of release `tag`, or of the newest release that has a manifest.

    The images themselves are fetched when a board is matched to one (image_for_board), so
    that a push to one board downloads one image. "latest" takes prereleases too: every
    release so far is one.
    """
    if tag == "latest":
        releases = json.loads(github_get(f"{GITHUB_API}/repos/{REPOSITORY}/releases?per_page=30"))
        release = next(
            (
                entry
                for entry in releases
                if any(asset.get("name") == MANIFEST_NAME for asset in entry.get("assets", []))
            ),
            None,
        )
        if release is None:
            raise UsageError(f"no release of {REPOSITORY} publishes {MANIFEST_NAME} yet")
    else:
        release = json.loads(github_get(f"{GITHUB_API}/repos/{REPOSITORY}/releases/tags/{tag}"))
    urls = {
        str(asset.get("name")): str(asset.get("browser_download_url"))
        for asset in release.get("assets", [])
    }
    name = str(release.get("tag_name") or tag)
    if MANIFEST_NAME not in urls:
        raise UsageError(f"release {name} publishes no {MANIFEST_NAME}: it has no sink firmware")
    into.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(github_get(urls[MANIFEST_NAME]))
    sums = read_sums(github_get(urls["SHA512SUMS"]).decode("utf-8")) if "SHA512SUMS" in urls else {}
    return Published(manifest, into, f"release {name}", sums, urls)


def fetch_run(run_id: str, into: Path) -> Published:
    """A CI run's esp32-firmware artifact, downloaded whole with the GitHub CLI."""
    if not run_id.isdigit():
        raise UsageError(f"--run takes a workflow run's number, not '{run_id}'")
    command = [
        "gh", "run", "download", run_id, "--repo", REPOSITORY,
        "--name", FIRMWARE_ARTIFACT, "--dir", str(into),
    ]  # fmt: skip
    try:
        done = subprocess.run(command, capture_output=True, text=True, check=False, timeout=600)
    except FileNotFoundError:
        raise UsageError(
            "--run downloads with the GitHub CLI (gh), which is not installed"
        ) from None
    if done.returncode != 0:
        raise UsageError(
            f"gh run download {run_id} failed: {done.stderr.strip() or done.stdout.strip()}"
        )
    try:
        manifest = json.loads((into / MANIFEST_NAME).read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise UsageError(
            f"run {run_id}'s {FIRMWARE_ARTIFACT} has no readable {MANIFEST_NAME}: {error}"
        ) from None
    return Published(manifest, into, f"CI run {run_id}")


def image_problem(
    entry: dict[str, Any], hardware: dict[str, Any], firmware: dict[str, Any]
) -> str | None:
    """Why the published image `entry` is not the one for this board, or None when it is."""
    if entry.get("target") != text_of(hardware, "target"):
        return f"it is for {entry.get('chip') or entry.get('target')}"
    flash_bytes = number_of(firmware, "flash_bytes")
    flash = str(entry.get("flash_size", ""))
    if (
        flash_bytes
        and flash.endswith("MB")
        and flash[:-2].isdigit()
        and int(flash[:-2]) << 20 != flash_bytes
    ):
        return f"it is built for {flash} of flash"
    has_psram = number_of(hardware, "psram_bytes") > 0
    if bool(entry.get("psram")) != has_psram:
        return (
            "it is built for a board with PSRAM"
            if entry.get("psram")
            else "it is built for a board without PSRAM"
        )
    revision = parse_revision(text_of(hardware, "revision"))
    low = number_of(entry, "min_rev_full")
    high = number_of(entry, "max_rev_full")
    if revision is not None and (revision < low or (revision_set(high) and revision > high)):
        return f"it runs on chip revisions {revision_text(low)} to {revision_text(high)}"
    theirs = board_partitions(firmware)
    ours = tuple(
        Partition(
            text_of(part, "label"),
            number_of(part, "type"),
            number_of(part, "subtype"),
            number_of(part, "offset"),
            number_of(part, "size"),
        )
        for part in entry.get("partitions") or []
    )
    if theirs is not None and ours and table_difference(ours, theirs):
        return f"its partition table is not the board's ({table_difference(ours, theirs)})"
    return None


def image_for_board(
    published: Published, hardware: dict[str, Any], firmware: dict[str, Any]
) -> tuple[Image | None, str]:
    """The published image that fits this board, read and checked, or why there is none."""
    reasons = []
    for entry in published.manifest.get("images") or []:
        problem = image_problem(entry, hardware, firmware)
        if problem:
            reasons.append(f"{entry.get('name')}: {problem}")
            continue
        app = (entry.get("files") or {}).get("app") or {}
        name = str(app.get("name", ""))
        path = published.directory / name
        if not path.is_file() and name in published.urls:
            path.write_bytes(github_get(published.urls[name]))
        if not path.is_file():
            raise UsageError(f"{published.source} names {name}, which it does not hold")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != app.get("sha256"):
            raise ImageError(
                f"{name}'s SHA-256 is not the one {MANIFEST_NAME} gives: the download is damaged"
            )
        if published.sums and published.sums.get(name) != hashlib.sha512(data).hexdigest():
            raise ImageError(
                f"{name}'s SHA-512 is not the one SHA512SUMS gives: the download is damaged"
            )
        image = read_image_file(path)
        partitions = tuple(
            Partition(
                text_of(part, "label"),
                number_of(part, "type"),
                number_of(part, "subtype"),
                number_of(part, "offset"),
                number_of(part, "size"),
            )
            for part in entry.get("partitions") or []
        )
        return (
            dataclasses.replace(
                image,
                partitions=partitions or None,
                wifi_built_in=bool(entry.get("network_built_in")),
            ),
            str(entry.get("name")),
        )
    return None, "; ".join(reasons) or f"{published.source} publishes no images"


def push_published(published: Published, boards: list[Board], options: PushOptions) -> int:
    """Each board gets the published image that fits it, one board at a time, as push() goes."""
    worst = UPDATED
    for index, board in enumerate(boards):
        try:
            hardware = board.get_json("/hardware")
            firmware = board.get_json("/firmware")
        except BoardError as error:
            say(board, f"refused: {error}")
            worst = max(worst, REFUSED)
            continue
        image, name = image_for_board(published, hardware, firmware)
        if image is None:
            say(board, f"refused: no image of {published.source} fits this board. {name}")
            worst = max(worst, REFUSED)
            continue
        say(board, f"{published.source}'s {name} fits this board")
        for line in describe_image(image):
            console.say(line)
        code = push_one(board, image, options)
        if code in (ROLLED_BACK, SILENT):
            rest = boards[index + 1 :]
            if rest:
                console.say(
                    f"stopped at {board}; not touched: {', '.join(str(each) for each in rest)}"
                )
            return code
        worst = max(worst, code)
    return worst


# --- the command line -----------------------------------------------------------------------


class Parser(argparse.ArgumentParser):
    """argparse, with exit status 1 for a usage error: 2 means a board rolled back."""

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        self.exit(REFUSED, f"{self.prog}: error: {message}\n")


EPILOG = (
    "H is a board's IP address or its mDNS name (hearth-eb2c64.local), with :PORT when the "
    "port is not 80. --all finds boards by their _sendspin._tcp service and needs the zeroconf "
    "package. Exit status: 0 every board updated or already running the image; 1 refused, "
    "failed, or a usage error; 2 a board rolled back; 3 a board did not come back. "
    "planning/esp32-ota.md has the design."
)


def build_parser() -> Parser:
    parser = Parser(prog="ota.py", description=__doc__.splitlines()[0], epilog=EPILOG)
    commands = parser.add_subparsers(
        title="commands", dest="command", required=True, metavar="COMMAND"
    )

    push_parser = commands.add_parser(
        "push",
        help="send an app image to boards, one at a time, and wait for each to accept it",
        description="Send an app image to boards, one at a time, and wait for each to accept it.",
        epilog=EPILOG,
    )
    source = push_parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--build-dir",
        type=Path,
        metavar="DIR",
        help="an ESP-IDF build directory: its app image, partition table and sdkconfig",
    )
    source.add_argument(
        "image",
        nargs="?",
        type=Path,
        metavar="IMAGE",
        help="an app image (ac3forge_hearth_sink.bin) on its own",
    )
    source.add_argument(
        "--release",
        metavar="TAG",
        help="a release's published images (or 'latest'): each board gets the one that fits it, "
        f"checked against {MANIFEST_NAME} and SHA512SUMS",
    )
    source.add_argument(
        "--run",
        metavar="RUN_ID",
        help=f"a CI run's {FIRMWARE_ARTIFACT} artifact, downloaded with the GitHub CLI (gh): "
        "each board gets the image that fits it",
    )
    push_parser.add_argument(
        "--download-dir",
        type=Path,
        metavar="DIR",
        help="where --release and --run keep what they download (default: a temporary directory)",
    )
    targets = push_parser.add_mutually_exclusive_group(required=True)
    targets.add_argument(
        "--host", action="append", metavar="H", help="a board to update; repeat for several"
    )
    targets.add_argument(
        "--all", action="store_true", help="every board that advertises _sendspin._tcp"
    )
    push_parser.add_argument(
        "--yes", action="store_true", help="stop a board that is playing without asking"
    )
    push_parser.add_argument(
        "--force",
        action="store_true",
        help="push to a board that already runs the image, and push an image whose network is "
        "not known to a board whose only network is built into its image",
    )
    push_parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        metavar="SECONDS",
        help=f"how long to wait for each board to decide (default {DEFAULT_TIMEOUT:g})",
    )

    status_parser = commands.add_parser(
        "status",
        help="what each board runs, its other slot, a trial and the last update",
        description="Print what each board runs, its other slot, a trial and the last update. "
        "With no board named, every board that advertises _sendspin._tcp.",
        epilog=EPILOG,
    )
    status_targets = status_parser.add_mutually_exclusive_group()
    status_targets.add_argument("--host", action="append", metavar="H", help="a board; repeat")
    status_targets.add_argument(
        "--all", action="store_true", help="every board that advertises _sendspin._tcp"
    )

    for name, text in (
        ("restart", "restart a board into the image it runs (POST /restart)"),
        ("rollback", "boot the other slot's image next, on trial (PUT /firmware/rollback)"),
        ("cancel", "leave flash mode, restarting into the running image (PUT /firmware/mode)"),
    ):
        command = commands.add_parser(name, help=text, description=text[0].upper() + text[1:] + ".")
        command.add_argument("--host", required=True, metavar="H", help="the board")

    coredump_parser = commands.add_parser(
        "coredump",
        help="save the core dump the board's last crash left, and read it (GET /firmware/coredump)",
        description="Save the core dump the board's last crash left, and read it with esp_coredump "
        "when given the ELF of the image that wrote it.",
        epilog=EPILOG,
    )
    coredump_parser.add_argument("--host", required=True, metavar="H", help="the board")
    coredump_parser.add_argument(
        "--out", type=Path, metavar="FILE", help="where to save it (default coredump-<board>.bin)"
    )
    coredump_parser.add_argument(
        "--elf",
        type=Path,
        metavar="ELF",
        help="the ELF of the image that wrote it: esp_coredump then prints each task's backtrace",
    )
    coredump_parser.add_argument(
        "--erase", action="store_true", help="erase it on the board once saved"
    )

    log_parser = commands.add_parser(
        "log",
        help="print the board's recent console output (GET /log)",
        description="Print the board's recent console output, and with --follow what comes after.",
        epilog=EPILOG,
    )
    log_parser.add_argument("--host", required=True, metavar="H", help="the board")
    log_parser.add_argument(
        "--follow", action="store_true", help="keep printing what is new until interrupted"
    )
    return parser


def run(args: argparse.Namespace) -> int:
    if args.command == "push":
        if args.timeout <= 0:
            raise UsageError("--timeout takes a number of seconds above 0")
        options = PushOptions(yes=args.yes, force=args.force, timeout=args.timeout)
        if args.release or args.run:
            with tempfile.TemporaryDirectory(prefix="ota-") as temporary:
                into = args.download_dir or Path(temporary)
                published = (
                    fetch_release(args.release, into) if args.release else fetch_run(args.run, into)
                )
                images = published.manifest.get("images") or []
                console.say(
                    f"{published.source}: {len(images)} image(s), "
                    f"{', '.join(str(image.get('name')) for image in images)}"
                )
                return push_published(published, boards_from(args), options)
        # The image is read and checked before any board is contacted.
        image = read_build_dir(args.build_dir) if args.build_dir else read_image_file(args.image)
        for line in describe_image(image):
            console.say(line)
        return push(image, boards_from(args), options)
    if args.command == "status":
        return max((show_status(board) for board in boards_from(args)), default=UPDATED)
    board = Board(args.host)
    if args.command == "restart":
        return send(board, "POST", "/restart", b"", "restart")
    if args.command == "rollback":
        return send(board, "PUT", "/firmware/rollback", b"", "rollback")
    if args.command == "coredump":
        return fetch_coredump(board, args.out, args.elf, args.erase)
    if args.command == "log":
        return show_log(board, args.follow)
    return send(board, "PUT", "/firmware/mode", b"normal", "cancel")


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(errors="replace")  # a board's text, on a console that cannot show it
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except (ImageError, UsageError) as error:
        console.close()
        print(f"ota.py: {error}", file=sys.stderr)
        return REFUSED
    except KeyboardInterrupt:
        console.close()
        print("ota.py: interrupted; ota.py status shows where each board is", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
