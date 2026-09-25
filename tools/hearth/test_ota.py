"""Unit tests for ota.py, the tool that updates Hearth sink boards over the network.

stdlib unittest, as the tool is stdlib-only: this runs in ci.yml's script-lint
job with the system python3, and uses no network but 127.0.0.1.

A stand-in board, an http.server on 127.0.0.1 in a thread, answers the routes
the tool uses from scripted state: GET /hardware, /status and /firmware,
PUT /firmware, /firmware/mode and /firmware/rollback, and POST /restart. After
an upload, its GET /firmware follows a script, one answer a request, with
DOWN for a request that gets no answer while the board restarts. The cases are
the rules the tool's header and planning/esp32-ota.md set out: an image that
checks out, and a damaged or cut-short one refused before any board is
contacted; each pre-flight refusal; an update that is accepted, one that rolls
back, and one that does not come back; and several boards, where the first to
roll back or go silent stops the rest. Images are built here byte by byte in
the layout ESP-IDF v6.1 writes, padded by esptool's rule, which the tool reads
with ESP-IDF's.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import base64
import dataclasses
import hashlib
import importlib.util
import io
import json
import random
import struct
import subprocess
import sys
import tempfile
import threading
import types
import unittest
from contextlib import redirect_stderr, redirect_stdout
from functools import reduce
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from operator import xor
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ota

HERE = Path(__file__).resolve().parent
EXAMPLE = HERE.parents[1] / "esp-idf" / "ac3forge" / "examples" / "hearth_sink"

OLD_ELF = hashlib.sha256(b"the image the board runs").digest()
NEW_ELF = hashlib.sha256(b"the image pushed").digest()
OLD_IMAGE_SHA = "ab" * 32

# The 16 MB table (planning/esp32-ota.md, "Flash layout").
TABLE = (
    ("nvs", 1, 0x02, 0x9000, 0x6000),
    ("phy_init", 1, 0x01, 0xF000, 0x1000),
    ("otadata", 1, 0x00, 0x10000, 0x2000),
    ("ota_0", 0, 0x10, 0x20000, 0x400000),
    ("ota_1", 0, 0x11, 0x420000, 0x400000),
    ("coredump", 1, 0x03, 0x820000, 0x10000),
    ("audio", 1, 0x40, 0x830000, 0x40000),
    ("storage", 1, 0x81, 0x870000, 0x40000),
    ("reserve", 1, 0x41, 0x8B0000, 0x400000),
)

# A GET /firmware that gets no answer: the board is restarting.
DOWN = object()
# A PUT /firmware the board closes without reading or recording anything, as a
# board that resets part-way through an upload does; and one it reads to the
# end first, then closes with no answer and nothing recorded.
DROP = object()
IGNORE = object()


def make_image(
    *,
    chip_id: int = 9,
    min_rev: int = 0,
    max_rev: int = 99,
    flash_code: int = 4,
    version: str = "v1.1.0",
    project: str = "ac3forge_hearth_sink",
    elf: bytes = NEW_ELF,
    sizes: tuple[int, ...] = (512, 4096, 1024),
    hash_appended: bool = True,
    checksum_error: int = 0,
    trailing: bytes = b"",
) -> bytes:
    """An app image as ESP-IDF v6.1 lays one out, padded by esptool's rule."""
    desc = struct.pack(
        "<II8x32s32s16s16s32s32s",
        0xABCD5432,
        0,
        version.encode(),
        project.encode(),
        b"10:06:38",
        b"Sep 25 2026",
        b"v6.1",
        elf,
    ).ljust(256, b"\0")
    rng = random.Random(sum(sizes) + len(sizes))
    header = struct.pack(
        "<BBBBIB3sHBHH4sB",
        0xE9,
        len(sizes),
        2,
        (flash_code << 4) | 0x0F,
        0x40379714,
        0xEE,
        bytes(3),
        chip_id,
        min_rev // 100,
        min_rev,
        max_rev,
        bytes(4),
        1 if hash_appended else 0,
    )
    out = io.BytesIO()
    out.write(header)
    checksum = 0xEF
    for index, size in enumerate(sizes):
        data = rng.randbytes(size)
        if index == 0:
            data = desc + data[len(desc) :]
        out.write(struct.pack("<II", 0x3C000020 + 0x10000 * index, len(data)))
        out.write(data)
        checksum = reduce(xor, data, checksum)
    # esptool's align_file_position(f, 16), then the checksum byte.
    out.write(bytes(15 - out.tell() % 16))
    out.write(bytes([checksum ^ checksum_error]))
    image = out.getvalue()
    if hash_appended:
        image += hashlib.sha256(image).digest()
    return image + trailing


def make_table(entries: tuple[tuple[str, int, int, int, int], ...] = TABLE) -> bytes:
    """partition-table.bin as gen_esp32part.py writes it: the entries, an MD5 entry, 0xFF."""
    out = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label.encode(), 0)
        for label, kind, subtype, offset, size in entries
    )
    out += b"\xeb\xeb" + b"\xff" * 14 + bytes(16)  # the tool does not read the digest
    return out.ljust(0xC00, b"\xff")


def slot(
    label: str,
    version: str,
    elf: bytes,
    state: str = "valid",
    image_sha: str = "",
    intact: bool | None = None,
) -> dict[str, Any]:
    return {
        "label": label,
        "state": state,
        "version": version,
        "project": "ac3forge_hearth_sink",
        "idf_version": "v6.1",
        "elf_sha256": elf.hex(),
        "image_sha256": image_sha,
        "intact": intact,
    }


EMPTY_SLOT = {**slot("ota_1", "", b""), "state": "empty", "project": "", "idf_version": ""}


def firmware(**changes: Any) -> dict[str, Any]:
    """GET /firmware's body (firmware_status.hpp), for a board running v1.0.0 from ota_0."""
    doc: dict[str, Any] = {
        "mode": "normal",
        "running": slot("ota_0", "v1.0.0", OLD_ELF, image_sha=OLD_IMAGE_SHA, intact=True),
        "other": EMPTY_SLOT,
        "trial": None,
        "upload": None,
        "last_update": None,
        "coredump": None,
        "network": "stored",
        "slot_bytes": 0x400000,
        "flash_bytes": 16 << 20,
        "partitions": [
            {"label": label, "type": kind, "subtype": subtype, "offset": offset, "size": size}
            for label, kind, subtype, offset, size in TABLE
        ],
        "bootloader_version": "v6.1",
    }
    doc.update(changes)
    return doc


def restarting() -> dict[str, Any]:
    """Between the upload's answer and the restart: flash mode, the upload still there."""
    return firmware(
        mode="flash",
        upload={"received": 1, "total": 1, "stage": "checking"},
        last_update={"version": "v0.9.0", "result": "rolled back", "reason": "an old update"},
    )


def on_trial(healthy_ms: int = 12_000, waiting: tuple[str, ...] = ("the Sendspin player",)) -> dict:
    return firmware(
        running=slot("ota_1", "v1.1.0", NEW_ELF, state="trial"),
        other=slot("ota_0", "v1.0.0", OLD_ELF, image_sha=OLD_IMAGE_SHA, intact=True),
        trial={
            "healthy_for_ms": healthy_ms,
            "hold_ms": 30_000,
            "remaining_ms": 250_000,
            "waiting_for": list(waiting),
        },
        last_update={"version": "v1.1.0", "result": "on trial", "reason": ""},
    )


def accepted(image_sha: str) -> dict[str, Any]:
    """Accepted; `image_sha` is "" until the board's check of the slot has run."""
    intact = True if image_sha else None
    return firmware(
        running=slot("ota_1", "v1.1.0", NEW_ELF, image_sha=image_sha, intact=intact),
        other=slot("ota_0", "v1.0.0", OLD_ELF, image_sha=OLD_IMAGE_SHA, intact=True),
        last_update={"version": "v1.1.0", "result": "accepted", "reason": ""},
    )


def rolled_back(reason: str) -> dict[str, Any]:
    return firmware(
        other=slot("ota_1", "v1.1.0", NEW_ELF, state="aborted"),
        last_update={"version": "v1.1.0", "result": "rolled back", "reason": reason},
    )


class FakeBoard:
    """A hearth_sink board's routes on 127.0.0.1, answering from scripted state."""

    def __init__(self, case: unittest.TestCase) -> None:
        self.hardware: dict[str, Any] = {
            "target": "esp32s3",
            "chip": "ESP32-S3",
            "revision": "0.2",
            "psram_bytes": 8 << 20,
            "project": "ac3forge_hearth_sink",
            "version": "v1.0.0",
            "idf_version": "v6.1",
        }
        self.firmware = firmware()
        self.status: dict[str, Any] = {"state": "stopped", "sendspin": {"playing": "idle"}}
        # GET /firmware's answers once an upload has been taken, in order; the
        # last one stays.
        self.after_upload: list[Any] = []
        # None: 200 and the JSON a board sends. DOWN: the body is read and no
        # answer comes, and the board restarts. DROP, IGNORE: see above.
        # (status, text): a refusal after the whole body.
        self.upload_answer: Any = None
        # Not empty: read only the head, refuse with this text, and close.
        self.refuse_after_head = ""
        self.answers: dict[tuple[str, str], tuple[int, str]] = {}
        self.requests: list[tuple[str, str]] = []
        self.bodies: dict[tuple[str, str], bytes] = {}
        self.upload_headers: dict[str, str] = {}
        self.received = b""
        self.uploaded = False
        # GET /firmware/coredump's bytes, None for a board with no dump; GET
        # /log's whole text, None for a board that keeps no log. The log
        # route sends at most log_reply bytes a request, as the board does.
        self.coredump: bytes | None = None
        self.log: str | None = None
        self.log_reply = 4096
        self.lock = threading.Lock()
        handler = type("Handler", (BoardHandler,), {"board": self})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.server.block_on_close = False
        thread = threading.Thread(target=self.server.serve_forever, args=(0.02,), daemon=True)
        thread.start()
        case.addCleanup(self.close)

    @property
    def host(self) -> str:
        return f"127.0.0.1:{self.server.server_address[1]}"

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def puts(self) -> list[str]:
        return [path for method, path in self.requests if method == "PUT"]

    def refuse_next(self, reason: str) -> None:
        """What a board records when it refuses an upload: flash mode, and why."""
        last = {"version": "", "result": "refused", "reason": reason}
        self.firmware = {**self.firmware, "mode": "flash", "last_update": last}


class BoardHandler(BaseHTTPRequestHandler):
    board: FakeBoard
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: Any) -> None:
        pass

    def _send(self, status: int, body: bytes, kind: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def _text(self, status: int, text: str) -> None:
        self._send(status, (text + "\n").encode(), "text/plain")

    def _body(self) -> bytes:
        return self.rfile.read(int(self.headers.get("Content-Length") or 0))

    def do_GET(self) -> None:
        board = self.board
        if self.path == "/firmware/coredump" or self.path.startswith("/log"):
            self._diagnostics()
            return
        with board.lock:
            board.requests.append(("GET", self.path))
            if self.path == "/firmware" and board.uploaded and board.after_upload:
                script = board.after_upload
                answer = script.pop(0) if len(script) > 1 else script[0]
            else:
                routes = {"/hardware": board.hardware, "/status": board.status}
                answer = routes.get(self.path, board.firmware if self.path == "/firmware" else None)
        if answer is DOWN:
            self.close_connection = True
        elif answer is None:
            self._text(404, "not found")
        else:
            self._send(200, (json.dumps(answer) + "\n").encode(), "application/json")

    def do_PUT(self) -> None:
        board = self.board
        with board.lock:
            board.requests.append(("PUT", self.path))
        if self.path != "/firmware":
            body = self._body()
            with board.lock:
                board.bodies[("PUT", self.path)] = body
            self._text(*board.answers.get(("PUT", self.path), (200, "ok")))
            return
        with board.lock:
            board.upload_headers = dict(self.headers.items())
        if board.upload_answer is DROP or board.upload_answer is IGNORE:
            if board.upload_answer is IGNORE:
                self._body()
            self.close_connection = True
            return
        if board.refuse_after_head:
            self.rfile.read(288)
            with board.lock:
                board.refuse_next(board.refuse_after_head)
            self._text(400, board.refuse_after_head)
            return
        body = self._body()
        with board.lock:
            board.received = body
            if board.upload_answer is None or board.upload_answer is DOWN:
                board.uploaded = True
            else:
                board.refuse_next(board.upload_answer[1])
        if board.upload_answer is DOWN:
            self.close_connection = True
        elif board.upload_answer is not None:
            self._text(*board.upload_answer)
        else:
            reply = {
                "version": "v1.1.0",
                "slot": "ota_1",
                "sha256": hashlib.sha256(body).hexdigest(),
                "restarting": True,
            }
            self._send(200, (json.dumps(reply) + "\n").encode(), "application/json")

    def do_POST(self) -> None:
        board = self.board
        body = self._body()
        with board.lock:
            board.requests.append(("POST", self.path))
            board.bodies[("POST", self.path)] = body
        self._text(*board.answers.get(("POST", self.path), (200, "restarting")))

    def do_DELETE(self) -> None:
        board = self.board
        with board.lock:
            board.requests.append(("DELETE", self.path))
            if self.path == "/firmware/coredump":
                board.coredump = None
        self._text(*board.answers.get(("DELETE", self.path), (200, "erased")))

    def _diagnostics(self) -> None:
        """GET /firmware/coredump and GET /log?from=N, as firmware.cpp and control.cpp answer."""
        board = self.board
        with board.lock:
            board.requests.append(("GET", self.path))
            dump = board.coredump
            log = board.log
        if self.path == "/firmware/coredump":
            if dump is None:
                self._text(
                    404, "there is no core dump: nothing has crashed since the last was erased"
                )
            else:
                self._send(200, dump, "application/octet-stream")
            return
        if log is None:
            self._text(404, "this board keeps no log")
            return
        data = log.encode()
        start = min(int(self.path.partition("from=")[2] or 0), len(data))
        chunk = data[start : start + board.log_reply]
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(chunk)))
        self.send_header("X-Log-From", str(start))
        self.send_header("X-Log-Next", str(start + len(chunk)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(chunk)
        self.close_connection = True


def run_ota(*argv: str) -> tuple[int, str]:
    """ota.py's main() with this command line: its exit status, and stdout and stderr."""
    out = io.StringIO()
    with redirect_stdout(out), redirect_stderr(out):
        try:
            code = ota.main(list(argv))
        except SystemExit as stop:
            code = stop.code if isinstance(stop.code, int) else 1
    return code, out.getvalue()


class Case(unittest.TestCase):
    def setUp(self) -> None:
        fast = mock.patch.multiple(
            ota, POLL_SECONDS=0.01, SHA_WAIT_SECONDS=0.3, REFUSAL_WAIT_SECONDS=3.0
        )
        fast.start()
        self.addCleanup(fast.stop)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)

    def write_image(self, data: bytes) -> Path:
        path = self.tmp / "ac3forge_hearth_sink.bin"
        path.write_bytes(data)
        return path

    def build_dir(
        self,
        data: bytes,
        *,
        target: str = "esp32s3",
        ssid: str | None = "",
        table: tuple[tuple[str, int, int, int, int], ...] = TABLE,
        version: str = "v1.1.0",
        name: str = "build",
    ) -> Path:
        """A build directory as idf.py leaves one, with what the tool reads from it."""
        build = self.tmp / name
        (build / "partition_table").mkdir(parents=True)
        (build / "ac3forge_hearth_sink.bin").write_bytes(data)
        (build / "partition_table" / "partition-table.bin").write_bytes(make_table(table))
        description = {
            "project_name": "ac3forge_hearth_sink",
            "project_version": version,
            "build_dir": build.as_posix(),
            "app_bin": "ac3forge_hearth_sink.bin",
            "target": target,
        }
        (build / "project_description.json").write_text(json.dumps(description), encoding="utf-8")
        if ssid is not None:
            (build / "sdkconfig").write_text(
                f'CONFIG_IDF_TARGET="{target}"\n{ota.WIFI_SSID_OPTION}="{ssid}"\n', encoding="utf-8"
            )
        return build


class ImageChecks(Case):
    def test_reads_the_head_and_checks_the_image(self) -> None:
        data = make_image()
        image = ota.parse_image(data, Path("a.bin"))
        self.assertEqual((image.chip_id, image.target, image.chip), (9, "esp32s3", "ESP32-S3"))
        self.assertEqual((image.min_rev_full, image.max_rev_full), (0, 99))
        self.assertEqual((image.flash_code, image.flash_bytes), (4, 16 << 20))
        self.assertEqual(image.version, "v1.1.0")
        self.assertEqual(image.project, "ac3forge_hearth_sink")
        self.assertEqual(image.idf_version, "v6.1")
        self.assertEqual(image.built, "Sep 25 2026 10:06:38")
        self.assertEqual(image.elf_sha256, NEW_ELF.hex())
        self.assertEqual(image.image_bytes, len(data))
        self.assertEqual(image.image_sha256, data[-32:].hex())
        digest = base64.b64encode(hashlib.sha256(data).digest()).decode()
        self.assertEqual(image.content_digest, f"sha-256=:{digest}:")

    def test_every_padding_the_build_writes(self) -> None:
        # Segments are whole words, so they end at one of the four words of a
        # 16-byte block, and 15, 11, 7 or 3 zeros come before the checksum.
        for extra in range(0, 16, 4):
            with self.subTest(extra=extra):
                data = make_image(sizes=(256 + extra, 4096))
                self.assertEqual(ota.parse_image(data, Path("a.bin")).image_bytes, len(data))

    def test_a_changed_byte_is_damage(self) -> None:
        data = bytearray(make_image())
        data[1000] ^= 0x01
        with self.assertRaisesRegex(ota.ImageError, "does not match .* damaged"):
            ota.parse_image(bytes(data), Path("a.bin"))

    def test_a_file_cut_short(self) -> None:
        data = make_image()
        cuts = (
            (len(data) - 1, "ends before the SHA-256 the build appended: it is cut short"),
            (len(data) - 40, "ends before its checksum: it is cut short"),
            (700, "ends inside segment 1: it is cut short or damaged"),
        )
        for length, why in cuts:
            with self.subTest(length=length), self.assertRaisesRegex(ota.ImageError, why):
                ota.parse_image(data[:length], Path("a.bin"))

    def test_an_image_with_no_sha256_of_its_own(self) -> None:
        with self.assertRaisesRegex(ota.ImageError, "carries no SHA-256"):
            ota.parse_image(make_image(hash_appended=False), Path("a.bin"))

    def test_what_is_not_an_app_image(self) -> None:
        data = make_image()
        with self.assertRaisesRegex(ota.ImageError, "not an ESP-IDF application image"):
            ota.parse_image(b"\0" + data[1:], Path("a.bin"))
        with self.assertRaisesRegex(ota.ImageError, "no application description"):
            ota.parse_image(data[:32] + b"\0\0\0\0" + data[36:], Path("a.bin"))
        with self.assertRaisesRegex(ota.ImageError, "17 segments"):
            ota.parse_image(data[:1] + b"\x11" + data[2:], Path("a.bin"))
        with self.assertRaisesRegex(ota.ImageError, "too short"):
            ota.parse_image(data[:100], Path("a.bin"))

    def test_a_checksum_that_does_not_add_up(self) -> None:
        with self.assertRaisesRegex(ota.ImageError, "checksum"):
            ota.parse_image(make_image(checksum_error=0x01), Path("a.bin"))

    def test_bytes_after_the_image(self) -> None:
        data = make_image(trailing=b"\xff" * 4096)
        image = ota.parse_image(data, Path("a.bin"))
        self.assertEqual(image.image_bytes, len(data) - 4096)
        self.assertEqual(image.file_sha256, hashlib.sha256(data).digest())

    def test_the_folded_xor_is_the_xor_of_every_byte(self) -> None:
        rng = random.Random(7)
        for length in [*range(40), 1023, 4099]:
            data = rng.randbytes(length)
            self.assertEqual(ota.xor_of(data), reduce(xor, data, 0), length)


class BuildDirectories(Case):
    def test_a_build_directory(self) -> None:
        image = ota.read_build_dir(self.build_dir(make_image(), ssid="home"))
        self.assertEqual(image.partitions, tuple(ota.Partition(*entry) for entry in TABLE))
        self.assertTrue(image.wifi_built_in)
        self.assertEqual(image.build_version, "v1.1.0")

    def test_whether_a_network_is_built_in(self) -> None:
        self.assertFalse(ota.read_build_dir(self.build_dir(make_image(), ssid="")).wifi_built_in)

    def test_no_sdkconfig_is_not_knowing(self) -> None:
        self.assertIsNone(ota.read_build_dir(self.build_dir(make_image(), ssid=None)).wifi_built_in)

    def test_a_build_without_wifi_has_no_network_built_in(self) -> None:
        build = self.build_dir(make_image(), ssid=None)
        (build / "sdkconfig").write_text('CONFIG_IDF_TARGET="esp32s3"\n', encoding="utf-8")
        self.assertIs(ota.read_build_dir(build).wifi_built_in, False)

    def test_a_target_the_image_is_not_for(self) -> None:
        with self.assertRaisesRegex(ota.ImageError, "built for esp32c6"):
            ota.read_build_dir(self.build_dir(make_image(), target="esp32c6"))

    def test_not_a_build_directory(self) -> None:
        with self.assertRaisesRegex(ota.ImageError, "no project_description.json"):
            ota.read_build_dir(self.tmp)

    def test_a_version_the_build_cut_to_31_characters(self) -> None:
        long = "v0.10.0-beta.1-1877-g6a9bdf999-dirty"
        data = make_image(version=long[:31])
        image = ota.read_build_dir(self.build_dir(data, version=long))
        self.assertNotIn("earlier build", "\n".join(ota.describe_image(image)))
        stale = dataclasses.replace(image, build_version="v0.11.0")
        self.assertIn("earlier build", "\n".join(ota.describe_image(stale)))


class Refusals(Case):
    """ota.refusal(): every reason a board is refused before anything is sent."""

    def setUp(self) -> None:
        super().setUp()
        self.image = ota.parse_image(make_image(), Path("a.bin"))
        self.hardware = {
            "target": "esp32s3",
            "chip": "ESP32-S3",
            "revision": "0.2",
            "project": "ac3forge_hearth_sink",
        }

    def refused(
        self,
        image: ota.Image | None = None,
        hardware: dict[str, Any] | None = None,
        force: bool = False,
        **firmware_changes: Any,
    ) -> str:
        """The refusal, or "" when there is none."""
        board = {**self.hardware, **(hardware or {})}
        return ota.refusal(image or self.image, board, firmware(**firmware_changes), force) or ""

    def test_a_board_that_fits(self) -> None:
        self.assertEqual(self.refused(), "")

    def test_another_chip(self) -> None:
        why = self.refused(hardware={"target": "esp32c6", "chip": "ESP32-C6"})
        self.assertEqual(why, "this image is for an ESP32-S3, and this board is an ESP32-C6")

    def test_a_chip_revision_below_the_image_minimum(self) -> None:
        # A default P4 image needs v3.1 or newer; the FireBeetle 2 is v1.3.
        image = ota.parse_image(make_image(chip_id=18, min_rev=301, max_rev=0), Path("p4.bin"))
        p4 = {"target": "esp32p4", "chip": "ESP32-P4", "revision": "1.3"}
        why = self.refused(image, p4)
        self.assertEqual(why, "this image needs chip revision v3.1 or newer, and this chip is v1.3")

    def test_a_chip_revision_above_the_image_maximum(self) -> None:
        image = ota.parse_image(make_image(chip_id=18, min_rev=100, max_rev=199), Path("p4.bin"))
        p4 = {"target": "esp32p4", "chip": "ESP32-P4", "revision": "3.1"}
        self.assertIn("up to v1.99, and this chip is v3.1", self.refused(image, p4))
        self.assertEqual(self.refused(image, {**p4, "revision": "1.3"}), "")

    def test_a_maximum_of_0_or_65535_is_no_maximum(self) -> None:
        for maximum in (0, 65535):
            image = ota.parse_image(make_image(max_rev=maximum), Path("a.bin"))
            self.assertEqual(self.refused(image, {"revision": "9.9"}), "", maximum)

    def test_a_revision_that_is_not_major_dot_minor(self) -> None:
        self.assertIn("not as M.m", self.refused(hardware={"revision": "rev2"}))

    def test_another_project(self) -> None:
        why = self.refused(hardware={"project": "stream_player"})
        self.assertEqual(
            why, "this image is ac3forge_hearth_sink, and the board runs stream_player"
        )

    def test_a_partition_table_the_board_does_not_have(self) -> None:
        smaller = tuple(
            (label, kind, subtype, offset, 0x1C0000 if label == "ota_0" else size)
            for label, kind, subtype, offset, size in TABLE
        )
        image = ota.read_build_dir(self.build_dir(make_image(), table=smaller))
        why = self.refused(image)
        self.assertIn("the build's ota_0 is app 0x10 at 0x20000, 0x1c0000 bytes", why)
        self.assertIn("An update cannot change the partition table: this needs one USB flash", why)
        fewer = ota.read_build_dir(self.build_dir(make_image(), table=TABLE[:-1], name="fewer"))
        self.assertIn("the board has reserve", self.refused(fewer))
        # A bare .bin carries no table, so there is nothing to compare.
        self.assertEqual(self.refused(partitions=[]), "")

    def test_an_image_larger_than_the_slot(self) -> None:
        size = len(self.image.data)
        why = self.refused(slot_bytes=size - 1)
        self.assertEqual(
            why, f"the image is {size:,} bytes, and the board's slot holds {size - 1:,}"
        )

    def test_another_flash_size(self) -> None:
        why = self.refused(flash_bytes=4 << 20)
        self.assertIn("built for 16 MB of flash, and the board's is set for 4 MB", why)

    def test_a_running_image_on_trial(self) -> None:
        on_trial_running = slot("ota_0", "v1.0.0", OLD_ELF, state="trial")
        self.assertIn("still on trial", self.refused(running=on_trial_running))
        self.assertIn("still on trial", self.refused(trial=on_trial()["trial"]))

    def test_an_update_under_way(self) -> None:
        upload = {"received": 4096, "total": 9000, "stage": "writing"}
        self.assertEqual(
            self.refused(upload=upload), "an update is already under way on this board"
        )

    def test_one_app_slot(self) -> None:
        self.assertIn("one app slot", self.refused(other=None))

    def test_a_network_built_into_the_running_image_only(self) -> None:
        none = ota.read_build_dir(self.build_dir(make_image(), ssid=""))
        self.assertIn("the new image has none", self.refused(none, network="built-in"))
        self.assertIn("the new image has none", self.refused(none, network="built-in", force=True))
        self.assertEqual(self.refused(none, network="stored"), "")
        built_in = dataclasses.replace(none, wifi_built_in=True)
        self.assertEqual(self.refused(built_in, network="built-in"), "")
        # A bare .bin says nothing about its network: refused unless --force.
        self.assertIn("does not say whether", self.refused(network="built-in"))
        self.assertEqual(self.refused(network="built-in", force=True), "")

    def test_what_counts_as_playing(self) -> None:
        self.assertFalse(ota.is_playing({"state": "stopped", "sendspin": {"playing": "idle"}}))
        self.assertFalse(ota.is_playing({"state": "flash", "sendspin": None}))
        self.assertTrue(ota.is_playing({"state": "playing"}))
        self.assertTrue(ota.is_playing({"state": "opening"}))
        self.assertTrue(ota.is_playing({"state": "stopped", "sendspin": {"playing": "bursts"}}))
        self.assertTrue(ota.is_playing({"state": "finished", "sendspin": {"playing": "pcm"}}))


class Push(Case):
    def push(self, board: FakeBoard, data: bytes, *extra: str) -> tuple[int, str]:
        return run_ota("push", str(self.write_image(data)), "--host", board.host, *extra)

    def test_an_update_that_is_accepted(self) -> None:
        data = make_image()
        board = FakeBoard(self)
        # A rollback recorded before this update, which the board reports
        # until it restarts, is not this update's outcome.
        board.firmware = firmware(
            last_update={"version": "v0.9.0", "result": "rolled back", "reason": "an old update"}
        )
        board.after_upload = [restarting(), DOWN, DOWN, on_trial(), accepted(data[-32:].hex())]
        code, out = self.push(board, data)
        self.assertEqual(code, ota.UPDATED, out)
        self.assertEqual(board.received, data)
        headers = {name.lower(): value for name, value in board.upload_headers.items()}
        digest = base64.b64encode(hashlib.sha256(data).digest()).decode()
        self.assertEqual(headers["content-digest"], f"sha-256=:{digest}:")
        self.assertEqual(headers["content-type"], "application/octet-stream")
        self.assertEqual(headers["content-length"], str(len(data)))
        self.assertIn("on trial: healthy for 12s of 30s, waiting for the Sendspin player", out)
        self.assertIn("updated: runs v1.1.0 from ota_1, accepted", out)
        self.assertIn("the image's SHA-256 on the board matches the file's", out)

    def test_the_image_sha256_once_the_board_has_checked(self) -> None:
        data = make_image()
        board = FakeBoard(self)
        board.after_upload = [DOWN, accepted(""), accepted(""), accepted(data[-32:].hex())]
        code, out = self.push(board, data)
        self.assertEqual(code, ota.UPDATED, out)
        self.assertIn("matches the file's", out)

    def test_an_image_sha256_the_board_never_reports(self) -> None:
        board = FakeBoard(self)
        board.after_upload = [accepted("")]
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.UPDATED, out)
        self.assertIn("has not reported the image's SHA-256 yet", out)

    def test_an_update_that_rolls_back(self) -> None:
        board = FakeBoard(self)
        why = "not healthy within 300 s; still waiting for the Sendspin player"
        board.after_upload = [DOWN, on_trial(), DOWN, rolled_back(why)]
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.ROLLED_BACK, out)
        self.assertIn("rolled back: v1.1.0 did not last, and the board runs v1.0.0 from ota_0", out)
        self.assertIn(why, out)

    def test_a_board_that_does_not_come_back(self) -> None:
        board = FakeBoard(self)
        board.after_upload = [DOWN]
        code, out = self.push(board, make_image(), "--timeout", "0.3")
        self.assertEqual(code, ota.SILENT, out)
        self.assertIn("did not come back within 0.3 s", out)
        self.assertIn("Cycle the board's power while the new image is on trial", out)
        self.assertIn("esptool write-flash @flash_args", out)

    def test_a_board_still_on_trial_at_the_timeout(self) -> None:
        board = FakeBoard(self)
        board.after_upload = [on_trial()]
        code, out = self.push(board, make_image(), "--timeout", "0.3")
        self.assertEqual(code, ota.SILENT, out)
        self.assertIn("still runs the new image on trial", out)

    def test_a_damaged_file_reaches_no_board(self) -> None:
        data = bytearray(make_image())
        data[3000] ^= 0x80
        board = FakeBoard(self)
        code, out = self.push(board, bytes(data))
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("damaged", out)
        self.assertEqual(board.requests, [])

    def test_a_refused_board_is_sent_nothing(self) -> None:
        board = FakeBoard(self)
        board.hardware.update(target="esp32c6", chip="ESP32-C6")
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("refused: this image is for an ESP32-S3, and this board is an ESP32-C6", out)
        self.assertEqual(board.puts(), [])

    def test_a_board_already_running_the_image(self) -> None:
        board = FakeBoard(self)
        board.firmware = firmware(running=slot("ota_0", "v1.1.0", NEW_ELF))
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.UPDATED, out)
        self.assertIn("already runs this image", out)
        self.assertEqual(board.puts(), [])
        # --force sends it anyway, and the copy in the other slot is the one
        # that has to be accepted.
        board.after_upload = [on_trial(), accepted("")]
        code, out = self.push(board, make_image(), "--force")
        self.assertEqual(code, ota.UPDATED, out)
        self.assertEqual(board.puts(), ["/firmware"])

    def test_a_playing_board_with_no_terminal(self) -> None:
        board = FakeBoard(self)
        board.status = {"state": "stopped", "sendspin": {"playing": "bursts"}}
        board.after_upload = [accepted("")]
        with mock.patch.object(sys, "stdin", io.StringIO()):
            code, out = self.push(board, make_image())
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("no terminal to ask on", out)
        self.assertEqual(board.puts(), [])
        code, out = self.push(board, make_image(), "--yes")
        self.assertEqual(code, ota.UPDATED, out)

    def test_a_playing_board_on_a_terminal(self) -> None:
        class Terminal(io.StringIO):
            def isatty(self) -> bool:
                return True

        board = FakeBoard(self)
        board.status = {"state": "playing"}
        board.after_upload = [accepted("")]
        for answer, expected in (("n", ota.REFUSED), ("y", ota.UPDATED)):
            with (
                mock.patch.object(sys, "stdin", Terminal()),
                mock.patch("builtins.input", return_value=answer) as asked,
            ):
                code, out = self.push(board, make_image())
            self.assertEqual(code, expected, out)
            self.assertIn("is playing; stop it to update? [y/N]", asked.call_args.args[0])

    def test_the_board_refuses_the_upload(self) -> None:
        board = FakeBoard(self)
        board.upload_answer = (400, "this image is for an ESP32-C6, and this board is an ESP32-S3")
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("refused (400): this image is for an ESP32-C6", out)

    def test_a_host_the_board_does_not_answer_to(self) -> None:
        board = FakeBoard(self)
        board.upload_answer = (403, "firmware changes are taken only on the board's own address")
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("by its IP address or its .local name", out)

    def test_a_refusal_while_the_image_is_on_its_way(self) -> None:
        # The board refuses after the head and closes: the tool finds out why
        # from the answer, or from GET /firmware when the answer is lost.
        board = FakeBoard(self)
        board.refuse_after_head = (
            "this image needs chip revision v3.1 or newer, and this chip is v1.3"
        )
        code, out = self.push(board, make_image(sizes=(512, 2_000_000)))
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("needs chip revision v3.1 or newer", out)

    def test_an_answer_lost_after_the_whole_image(self) -> None:
        board = FakeBoard(self)
        board.upload_answer = DOWN
        board.after_upload = [DOWN, on_trial(), accepted("")]
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.UPDATED, out)
        self.assertIn("no answer to the upload", out)

    def test_an_answer_lost_and_the_image_refused(self) -> None:
        board = FakeBoard(self)
        board.upload_answer = DOWN
        refused = firmware(
            mode="flash",
            last_update={"version": "v1.1.0", "result": "refused", "reason": "the upload stopped"},
        )
        board.after_upload = [refused]
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("refused: the upload stopped", out)

    def test_an_upload_the_board_never_took(self) -> None:
        # A rollback from an earlier update is still what the board reports:
        # it is not this update's outcome, and the push failed. Broken off
        # part-way, and sent in full with no answer.
        for answer, sizes, said in (
            (DROP, (512, 2_000_000), "the board gave no reason"),
            (IGNORE, (512, 4096), "did not take it"),
        ):
            with self.subTest(said=said):
                board = FakeBoard(self)
                old = {"version": "v0.9.0", "result": "rolled back", "reason": "it panicked"}
                board.firmware = firmware(last_update=old)
                board.upload_answer = answer
                code, out = self.push(board, make_image(sizes=sizes))
                self.assertEqual(code, ota.REFUSED, out)
                self.assertIn(said, out)
                self.assertNotIn("it panicked", out)

    def test_a_bare_image_to_a_board_whose_network_is_built_in(self) -> None:
        board = FakeBoard(self)
        board.firmware = firmware(network="built-in")
        code, out = self.push(board, make_image())
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("does not say whether it has one", out)
        board.after_upload = [accepted("")]
        code, out = self.push(board, make_image(), "--force")
        self.assertEqual(code, ota.UPDATED, out)

    def test_a_build_directory(self) -> None:
        board = FakeBoard(self)
        board.after_upload = [accepted("")]
        code, out = run_ota(
            "push", "--build-dir", str(self.build_dir(make_image())), "--host", board.host
        )
        self.assertEqual(code, ota.UPDATED, out)
        self.assertIn("network built in: none", out)


class SeveralBoards(Case):
    def test_the_first_rollback_stops_the_rest(self) -> None:
        first, second = FakeBoard(self), FakeBoard(self)
        first.after_upload = [DOWN, rolled_back("it panicked")]
        path = self.write_image(make_image())
        code, out = run_ota("push", str(path), "--host", first.host, "--host", second.host)
        self.assertEqual(code, ota.ROLLED_BACK, out)
        self.assertEqual(second.requests, [])
        self.assertIn(f"stopped at {first.host}; not touched: {second.host}", out)

    def test_a_refusal_does_not_stop_the_rest(self) -> None:
        first, second = FakeBoard(self), FakeBoard(self)
        first.hardware["revision"] = "not a revision"
        second.after_upload = [accepted("")]
        path = self.write_image(make_image())
        code, out = run_ota("push", str(path), "--host", first.host, "--host", second.host)
        self.assertEqual(code, ota.REFUSED, out)
        self.assertEqual(first.puts(), [])
        self.assertEqual(second.puts(), ["/firmware"])

    def test_all_stops_at_the_first_board_that_does_not_come_back(self) -> None:
        first, second = FakeBoard(self), FakeBoard(self)
        first.after_upload = [DOWN]
        found = [ota.Board(first.host, "hearth-a.local"), ota.Board(second.host, "hearth-b.local")]
        path = self.write_image(make_image())
        with mock.patch.object(ota, "discover", return_value=found):
            code, out = run_ota("push", str(path), "--all", "--timeout", "0.3")
        self.assertEqual(code, ota.SILENT, out)
        self.assertIn("found 2 board(s): hearth-a.local, hearth-b.local", out)
        self.assertIn("stopped at hearth-a.local; not touched: hearth-b.local", out)
        self.assertEqual(second.requests, [])

    def test_all_without_zeroconf(self) -> None:
        path = self.write_image(make_image())
        with mock.patch.dict(sys.modules, {"zeroconf": None}):
            code, out = run_ota("push", str(path), "--all")
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("--all finds boards with the zeroconf package", out)
        self.assertNotIn("Traceback", out)

    def test_discovery_takes_one_board_for_each_address(self) -> None:
        services = {
            "b._sendspin._tcp.local.": ("hearth-b.local.", ["192.168.1.20"]),
            "a._sendspin._tcp.local.": ("hearth-a.local.", ["192.168.1.10"]),
            "a2._sendspin._tcp.local.": ("hearth-a.local.", ["192.168.1.10"]),
            "c._sendspin._tcp.local.": ("hearth-c.local.", []),
        }
        with mock.patch.dict(sys.modules, {"zeroconf": stand_in_zeroconf(services)}):
            boards = ota.discover(seconds=0)
        self.assertEqual(
            [(board.host, board.label) for board in boards],
            [
                ("192.168.1.10", "hearth-a.local (192.168.1.10)"),
                ("192.168.1.20", "hearth-b.local (192.168.1.20)"),
            ],
        )


def stand_in_zeroconf(services: dict[str, tuple[str, list[str]]]) -> types.ModuleType:
    """The parts of the zeroconf package that ota.discover() uses, answering from `services`."""
    module = types.ModuleType("zeroconf")

    class IPVersion:
        V4Only = "v4"

    class ServiceStateChange:
        Added = "added"

    class ServiceInfo:
        def __init__(self, server: str, addresses: list[str]) -> None:
            self.server = server
            self.addresses = addresses

        def parsed_addresses(self, version: str) -> list[str]:
            return list(self.addresses)

    class Zeroconf:
        def __init__(self, ip_version: str) -> None:
            self.ip_version = ip_version

        def get_service_info(self, kind: str, name: str, timeout: int) -> ServiceInfo:
            return ServiceInfo(*services[name])

        def close(self) -> None:
            pass

    class ServiceBrowser:
        def __init__(self, zc: Zeroconf, kind: str, handlers: list[Any]) -> None:
            for name in services:
                for handler in handlers:
                    handler(
                        zeroconf=zc,
                        service_type=kind,
                        name=name,
                        state_change=ServiceStateChange.Added,
                    )

        def cancel(self) -> None:
            pass

    module.IPVersion = IPVersion  # type: ignore[attr-defined]
    module.ServiceStateChange = ServiceStateChange  # type: ignore[attr-defined]
    module.Zeroconf = Zeroconf  # type: ignore[attr-defined]
    module.ServiceBrowser = ServiceBrowser  # type: ignore[attr-defined]
    return module


class OtherCommands(Case):
    def test_status(self) -> None:
        board = FakeBoard(self)
        board.firmware = on_trial()
        code, out = run_ota("status", "--host", board.host)
        self.assertEqual(code, 0, out)
        self.assertIn("  mode         normal", out)
        self.assertIn("  running      ota_1  v1.1.0  trial, not checked yet", out)
        self.assertIn("  other        ota_0  v1.0.0  valid, intact", out)
        self.assertIn(
            "  trial        healthy for 12s of 30s, waiting for the Sendspin player (250s left)",
            out,
        )
        self.assertIn("  last update  v1.1.0: on trial", out)
        self.assertIn("  network      stored", out)

    def test_status_of_a_board_that_does_not_answer(self) -> None:
        board = FakeBoard(self)
        host = board.host
        board.close()
        code, out = run_ota("status", "--host", host)
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("GET /firmware got no answer", out)

    def test_restart_rollback_and_cancel(self) -> None:
        board = FakeBoard(self)
        for command, method, path, body in (
            ("restart", "POST", "/restart", b""),
            ("rollback", "PUT", "/firmware/rollback", b""),
            ("cancel", "PUT", "/firmware/mode", b"normal"),
        ):
            with self.subTest(command=command):
                code, out = run_ota(command, "--host", board.host)
                self.assertEqual(code, 0, out)
                self.assertEqual(board.requests[-1], (method, path))
                self.assertEqual(board.bodies[(method, path)], body)
        board.answers[("POST", "/restart")] = (409, "the running image is still on trial")
        code, out = run_ota("restart", "--host", board.host)
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("restart: 409 the running image is still on trial", out)


DUMP = {
    "bytes": 23_456,
    "intact": True,
    "task": "fw_trial",
    "pc": "0x4037a1b2",
    "reason": "abort() was called at PC 0x4200abcd on core 0",
    "elf_sha256": NEW_ELF.hex()[:9],
}


class Diagnostics(Case):
    """The last crash's core dump and the console's recent output (O4)."""

    def test_status_names_the_image_that_wrote_a_core_dump(self) -> None:
        board = FakeBoard(self)
        board.firmware = rolled_back("it panicked")
        board.firmware["coredump"] = DUMP
        code, out = run_ota("status", "--host", board.host)
        self.assertEqual(code, 0, out)
        self.assertIn(
            "  core dump    23,456 bytes, fw_trial at 0x4037a1b2, abort() was called at PC "
            "0x4200abcd on core 0, written by v1.1.0 in ota_1",
            out,
        )
        # An image neither slot holds, and a dump that does not check out.
        board.firmware["coredump"] = {
            **DUMP,
            "intact": False,
            "task": "",
            "reason": "",
            "elf_sha256": "0123abcd9",
        }
        code, out = run_ota("status", "--host", board.host)
        self.assertIn(
            "  core dump    23,456 bytes, which do not check out, written by an image neither slot "
            "holds now (ELF SHA-256 0123abcd9...)",
            out,
        )
        board.firmware["coredump"] = {**DUMP, "elf_sha256": ""}
        code, out = run_ota("status", "--host", board.host)
        self.assertIn("written by an image the dump does not name", out)
        board.firmware["coredump"] = None
        code, out = run_ota("status", "--host", board.host)
        self.assertIn("  core dump    none", out)

    def test_coredump_saves_the_dump_and_erases_it_when_asked(self) -> None:
        board = FakeBoard(self)
        board.firmware = {**rolled_back("it panicked"), "coredump": DUMP}
        board.coredump = bytes(range(256)) * 10
        out_file = self.tmp / "dump.bin"
        code, out = run_ota("coredump", "--host", board.host, "--out", str(out_file))
        self.assertEqual(code, 0, out)
        self.assertEqual(out_file.read_bytes(), bytes(range(256)) * 10)
        self.assertIn(f"saved 2,560 bytes to {out_file}: 23,456 bytes, fw_trial", out)
        self.assertNotIn(("DELETE", "/firmware/coredump"), board.requests)
        code, out = run_ota("coredump", "--host", board.host, "--out", str(out_file), "--erase")
        self.assertEqual(code, 0, out)
        self.assertIn(("DELETE", "/firmware/coredump"), board.requests)
        self.assertIn("erase: 200 erased", out)
        # Nothing left: the board's own words.
        code, out = run_ota("coredump", "--host", board.host, "--out", str(out_file))
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("GET /firmware/coredump answered 404: there is no core dump", out)

    def test_coredump_reads_the_dump_with_the_elf_that_wrote_it(self) -> None:
        board = FakeBoard(self)
        board.firmware = {
            **rolled_back("it panicked"),
            "coredump": {**DUMP, "elf_sha256": hashlib.sha256(b"elf").hexdigest()[:9]},
        }
        board.coredump = b"a core dump"
        elf = self.tmp / "panic.elf"
        elf.write_bytes(b"elf")
        out_file = self.tmp / "dump.bin"
        ran = mock.Mock(return_value=subprocess.CompletedProcess([], 0))
        with mock.patch.object(ota.subprocess, "run", ran):
            code, out = run_ota(
                "coredump", "--host", board.host, "--out", str(out_file), "--elf", str(elf)
            )
        self.assertEqual(code, 0, out)
        command = ran.call_args.args[0]
        self.assertEqual(command[1:4], ["-m", "esp_coredump", "info_corefile"])
        self.assertEqual(command[4:], ["--core", str(out_file), "--core-format", "raw", str(elf)])
        # esp_coredump failing, or not there at all.
        with mock.patch.object(
            ota.subprocess, "run", mock.Mock(return_value=subprocess.CompletedProcess([], 2))
        ):
            code, out = run_ota(
                "coredump", "--host", board.host, "--out", str(out_file), "--elf", str(elf)
            )
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("esp_coredump exited 2; run it in the ESP-IDF environment", out)
        with mock.patch.object(ota.subprocess, "run", mock.Mock(side_effect=OSError("no python"))):
            code, out = run_ota(
                "coredump", "--host", board.host, "--out", str(out_file), "--elf", str(elf)
            )
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("esp_coredump did not run: OSError: no python", out)

    def test_coredump_refuses_an_elf_that_did_not_write_the_dump(self) -> None:
        board = FakeBoard(self)
        board.firmware = {**rolled_back("it panicked"), "coredump": DUMP}
        board.coredump = b"a core dump"
        elf = self.tmp / "other.elf"
        elf.write_bytes(b"another image")
        ran = mock.Mock()
        with mock.patch.object(ota.subprocess, "run", ran):
            code, out = run_ota(
                "coredump",
                "--host",
                board.host,
                "--out",
                str(self.tmp / "d.bin"),
                "--elf",
                str(elf),
            )
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn(f"{elf} is not the image that wrote the dump", out)
        ran.assert_not_called()
        code, out = run_ota(
            "coredump",
            "--host",
            board.host,
            "--out",
            str(self.tmp / "d.bin"),
            "--elf",
            str(self.tmp / "missing.elf"),
        )
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("--elf", out)

    def test_coredump_from_a_board_that_does_not_answer(self) -> None:
        board = FakeBoard(self)
        host = board.host
        board.close()
        code, out = run_ota("coredump", "--host", host)
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("GET /firmware got no answer", out)

    def test_log_prints_what_the_board_holds_a_reply_at_a_time(self) -> None:
        board = FakeBoard(self)
        board.log = "firmware: running v1.0.0 from ota_0\n" * 20
        board.log_reply = 64
        code, out = run_ota("log", "--host", board.host)
        self.assertEqual(code, 0, out)
        self.assertEqual(out, board.log)
        froms = [path for method, path in board.requests if path.startswith("/log")]
        self.assertEqual(froms[:3], ["/log?from=0", "/log?from=64", "/log?from=128"])
        board.log = None
        code, out = run_ota("log", "--host", board.host)
        self.assertEqual(code, ota.REFUSED, out)
        self.assertIn("GET /log answered 404: this board keeps no log", out)

    def test_log_follow_keeps_asking_and_says_what_it_missed(self) -> None:
        board = FakeBoard(self)
        board.log = "one\n"
        rounds = []

        def sleep(_: float) -> None:
            rounds.append(1)
            if len(rounds) == 1:
                # The board wrote on, and moved past what was asked for next.
                board.log = "one\ntwo\n"
                board.log_reply = 4096
            elif len(rounds) == 2:
                raise KeyboardInterrupt

        with mock.patch.object(ota.time, "sleep", sleep):
            code, out = run_ota("log", "--host", board.host, "--follow")
        self.assertEqual(code, 0, out)
        self.assertEqual(out, "one\ntwo\n")

    def test_log_follow_waits_through_a_board_that_does_not_answer(self) -> None:
        board = FakeBoard(self)
        host = board.host
        board.close()
        calls = []

        def sleep(_: float) -> None:
            calls.append(1)
            raise KeyboardInterrupt

        with mock.patch.object(ota.time, "sleep", sleep):
            code, out = run_ota("log", "--host", host, "--follow")
        self.assertEqual(code, 0, out)
        self.assertIn("GET /log got no answer", out)
        code, out = run_ota("log", "--host", host)
        self.assertEqual(code, ota.REFUSED, out)


class CommandLine(Case):
    def test_help(self) -> None:
        for argv in (["--help"], ["push", "--help"], ["status", "--help"]):
            result = subprocess.run(
                [sys.executable, str(HERE / "ota.py"), *argv],
                capture_output=True,
                text=True,
                timeout=60,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("usage: ota.py", result.stdout)

    def test_a_usage_error_is_exit_status_1_not_2(self) -> None:
        self.assertEqual(run_ota("push")[0], 1)
        self.assertEqual(run_ota("push", "a.bin", "--host", "h", "--timeout", "0")[0], 1)
        self.assertEqual(run_ota("restart")[0], 1)

    def test_hosts(self) -> None:
        self.assertEqual(ota.parse_host("hearth-eb2c64.local"), ("hearth-eb2c64.local", 80))
        self.assertEqual(ota.parse_host("192.168.1.5:8080"), ("192.168.1.5", 8080))
        self.assertEqual(ota.parse_host("http://10.0.0.2/"), ("10.0.0.2", 80))
        self.assertEqual(ota.parse_host("[fe80::1]:81"), ("fe80::1", 81))
        for bad in ("hearth:99999", "hearth/firmware", ""):
            with self.subTest(bad=bad), self.assertRaises(ota.UsageError):
                ota.parse_host(bad)


class IdfExtension(unittest.TestCase):
    """esp-idf/ac3forge/examples/hearth_sink/idf_ext.py, as idf.py loads it."""

    def load(self) -> types.ModuleType:
        spec = importlib.util.spec_from_file_location("idf_ext_hearth_sink", EXAMPLE / "idf_ext.py")
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        with mock.patch.object(sys, "dont_write_bytecode", True):  # no __pycache__ in the example
            spec.loader.exec_module(module)
        return module

    def test_the_ota_action(self) -> None:
        module = self.load()
        action = module.action_extensions({}, str(EXAMPLE))["actions"]["ota"]
        self.assertEqual(
            [option["names"] for option in action["options"]], [["--host"], ["--yes"], ["--force"]]
        )
        self.assertTrue(action["options"][0]["multiple"])
        self.assertEqual(action["order_dependencies"], ["all", "app"])
        args = types.SimpleNamespace(build_dir="B")
        done = subprocess.CompletedProcess([], 0)
        with mock.patch.object(module.subprocess, "run", return_value=done) as run:
            action["callback"]("ota", None, args, host=("h1", "h2"), yes=True, force=False)
        command = run.call_args.args[0]
        self.assertEqual(command[0], sys.executable)
        self.assertEqual(Path(command[1]).resolve(), (HERE / "ota.py").resolve())
        self.assertEqual(
            command[2:], ["push", "--build-dir", "B", "--host", "h1", "--host", "h2", "--yes"]
        )
        with (
            mock.patch.object(
                module.subprocess, "run", return_value=subprocess.CompletedProcess([], 2)
            ),
            self.assertRaisesRegex(SystemExit, "exited with 2: a board rolled back"),
        ):
            action["callback"]("ota", None, args, host=("h1",), yes=False, force=True)
        with self.assertRaisesRegex(SystemExit, "needs a board"):
            action["callback"]("ota", None, args, host=(), yes=False, force=False)


if __name__ == "__main__":
    unittest.main()
