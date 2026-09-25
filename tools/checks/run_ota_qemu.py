#!/usr/bin/env python3
"""Update hearth_sink over the network under QEMU, and hold every step to the plan.

planning/esp32-ota.md, "Tests": the ESP32-S3 is the only part with a QEMU
machine, so this is where an update, a refusal and each kind of rollback run
end to end, over QEMU's emulated Ethernet and a port forward, against images
built from one tree:

  a.bin          the image the board boots (sdkconfig.ci-ota, over the
                 Sendspin player's CI shape), and which qemu_flash.bin holds
  b.bin          the same with another PROJECT_VER
  unhealthy.bin  built with AC3FORGE_FIRMWARE_TEST=unhealthy: never healthy,
                 so it goes back at its deadline
  panic.bin      built with AC3FORGE_FIRMWARE_TEST=panic: panics as its trial
                 starts, so the bootloader goes back

In order, each step failing the run with an ::error:: annotation that says what
the board answered:

   1. the board boots a.bin from ota_0, valid, with the other slot empty;
   2. tools/hearth/ota.py pushes b.bin: the board restarts into ota_1, the
      trial accepts it, and a.bin is the image to go back to;
   3. refusals, each leaving b.bin running: a byte of the image changed (400,
      the image does not check out), a Content-Digest that does not match
      (400), another chip's image (400, before anything is written), an upload
      cut short (400), a Host that is not the board's (403), and a play while
      in flash mode (409);
   4. ota.py pushes a.bin back: accepted;
   5. ota.py pushes unhealthy.bin: rolled back at the deadline, and the board
      says why;
   6. ota.py pushes panic.bin: rolled back by the bootloader, and the board
      says it panicked;
   7. PUT /firmware/rollback with nothing to go back to: 409; then b.bin is
      pushed, and PUT /firmware/rollback takes the board back to a.bin;
   8. POST /restart: the board restarts into the image it ran;
   9. the running slot damaged in the flash image between two boots: the
      bootloader boots the other slot;
  10. PUT /firmware/mode flash and then normal: the board restarts.

Standard library only, as the other scripts here are.

  python3 tools/checks/run_ota_qemu.py --qemu QEMU --images DIR [--out DIR]

--images holds a.bin, b.bin, unhealthy.bin, panic.bin, qemu_flash.bin and
qemu_efuse.bin. Exit status 0 when every step passes, 1 otherwise.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import json
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OTA = HERE.parent / "hearth" / "ota.py"
TITLE = "ESP32-S3 update over the network"
PORT = 18080
HOST = f"127.0.0.1:{PORT}"
# Where ota_0 starts in partitions.csv; a byte a page into it lies in the
# image's first segment.
OTA_0_OFFSET = 0x20000
# What the board prints once an update is written and checked, before it
# restarts into it (see Board).
RESTART_INTO_NEW_IMAGE = "firmware: restarting into the new image"


class Failure(Exception):
    pass


class Board:
    """QEMU running the flash image, and HTTP to it through the port forward.

    Every image an update writes boots in a QEMU of its own. QEMU keeps the code
    it has translated from flash, and a write through the emulated SPI flash
    does not discard it: an image written over one that already ran in this
    QEMU runs the old image's code with the new image's data. On 2026-09-25
    the never-healthy image, written over b.bin, passed its trial as b.bin
    would have, while reporting its own version. So a watcher reads the
    console, and when the board announces the restart into a new image, half a
    second before it (firmware.cpp's restart_now), it stops QEMU and starts
    another on the same flash image, which boots the new image as a power
    cycle would. The other restarts stay in the one QEMU, which keeps the
    reset reason a panic leaves: they boot code this QEMU has not run from
    flash yet, or has run unchanged.
    """

    def __init__(self, qemu: str, out: Path) -> None:
        self.qemu = qemu
        self.out = out
        self.console = out / "qemu-ota.txt"
        self.process: subprocess.Popen[bytes] | None = None
        self.boots = 0
        # The process and the console, between the watcher and the steps.
        self.lock = threading.Lock()
        self.closing = threading.Event()
        self.watcher = threading.Thread(target=self._watch, daemon=True)

    def start(self) -> None:
        with self.lock:
            self._start()
        if not self.watcher.is_alive():
            self.watcher.start()

    def stop(self) -> None:
        with self.lock:
            self._stop()

    def close(self) -> None:
        self.closing.set()
        if self.watcher.is_alive():
            self.watcher.join(timeout=10)
        self.stop()

    def exited(self) -> bool:
        with self.lock:
            return self.process is not None and self.process.poll() is not None

    def _start(self) -> None:
        self.boots += 1
        log = self.out / f"qemu-ota-{self.boots}.txt"
        self.console = log
        self.process = subprocess.Popen(
            [
                self.qemu,
                "-M", "esp32s3", "-m", "32M",
                "-drive", f"file={self.out / 'qemu_flash.bin'},if=mtd,format=raw",
                "-drive", f"file={self.out / 'qemu_efuse.bin'},if=none,format=raw,id=efuse",
                "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
                "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
                "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{PORT}-:80",
                "-nographic", "-monitor", "none", "-serial", f"file:{log}",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )  # fmt: skip

    def _stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)
        self.process = None

    def _watch(self) -> None:
        while not self.closing.wait(0.05):
            with self.lock:
                if self.process is None or self.process.poll() is not None:
                    continue
                try:
                    text = self.console.read_text("utf-8", "replace")
                except OSError:
                    continue
                if RESTART_INTO_NEW_IMAGE in text:
                    print(f"(a new image: QEMU starts again, console {self.boots + 1})")
                    self._stop()
                    self._start()

    def request(
        self,
        method: str,
        path: str,
        body: bytes = b"",
        headers: dict[str, str] | None = None,
        timeout: float = 30.0,
    ) -> tuple[int, str]:
        connection = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read().decode("utf-8", "replace")
        finally:
            connection.close()

    def firmware(self) -> dict:
        status, text = self.request("GET", "/firmware")
        if status != 200:
            raise Failure(f"GET /firmware answered {status}: {text.strip()}")
        return json.loads(text)

    def wait_up(self, seconds: float = 240.0) -> dict:
        """The board's GET /firmware once it answers, through restarts."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.exited():
                raise Failure("QEMU exited")
            try:
                return self.firmware()
            except (OSError, http.client.HTTPException, json.JSONDecodeError):
                time.sleep(1.0)
        raise Failure(f"the board did not answer GET /firmware within {seconds:.0f} s")

    def wait_accepted(self, version: str, seconds: float = 240.0) -> dict:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                state = self.firmware()
                running = state.get("running") or {}
                if running.get("version") == version and running.get("state") == "valid":
                    return state
            except (OSError, http.client.HTTPException, json.JSONDecodeError):
                pass
            time.sleep(1.0)
        raise Failure(f"{version} was not running and accepted within {seconds:.0f} s")


def content_digest(data: bytes) -> str:
    return "sha-256=:" + base64.b64encode(hashlib.sha256(data).digest()).decode() + ":"


def ota(*args: str) -> tuple[int, str]:
    """tools/hearth/ota.py, as a person runs it; its exit status and output."""
    result = subprocess.run(
        [sys.executable, str(OTA), *args],
        capture_output=True,
        text=True,
        timeout=900,
        check=False,
    )
    output = result.stdout + result.stderr
    print(output, end="")
    return result.returncode, output


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def expect_running(state: dict, label: str, version: str) -> None:
    running = state.get("running") or {}
    expect(
        running.get("label") == label and running.get("version") == version,
        f"expected {version} running from {label}, the board runs {running.get('version')!r} "
        f"from {running.get('label')!r}",
    )


def cut_short_upload(image: bytes) -> tuple[int, str]:
    """Half an image under a Content-Length for all of it, then the writer's FIN."""
    with socket.create_connection(("127.0.0.1", PORT), timeout=120) as sock:
        head = (
            f"PUT /firmware HTTP/1.1\r\nHost: {HOST}\r\n"
            "Content-Type: application/octet-stream\r\n"
            f"Content-Length: {len(image)}\r\n\r\n"
        )
        sock.sendall(head.encode() + image[: len(image) // 2])
        sock.shutdown(socket.SHUT_WR)
        reply = b""
        while chunk := sock.recv(4096):
            reply += chunk
    first = reply.split(b"\r\n", 1)[0].decode("ascii", "replace")
    status = int(first.split()[1]) if len(first.split()) > 1 else 0
    return status, reply.split(b"\r\n\r\n", 1)[-1].decode("utf-8", "replace")


def run(board: Board, images: Path) -> None:
    a = (images / "a.bin").read_bytes()
    b = (images / "b.bin").read_bytes()
    version_a = image_version(a)
    version_b = image_version(b)

    print("--- 1: the board boots a.bin from ota_0")
    board.start()
    state = board.wait_up()
    expect_running(state, "ota_0", version_a)
    expect(
        (state.get("other") or {}).get("state") == "empty",
        f"ota_1 is not empty: {state.get('other')}",
    )
    expect(state.get("mode") == "normal", f"mode is {state.get('mode')}")

    print("--- 2: ota.py pushes b.bin")
    code, _ = ota("push", str(images / "b.bin"), "--host", HOST, "--yes")
    expect(code == 0, f"ota.py push b.bin exited {code}")
    state = board.wait_accepted(version_b)
    expect_running(state, "ota_1", version_b)
    other = state.get("other") or {}
    expect(
        other.get("version") == version_a and other.get("state") == "valid",
        f"ota_0 after the update: {other}",
    )

    print("--- 3: refusals")
    damaged = bytearray(b)
    damaged[len(damaged) // 2] ^= 0xFF
    status, text = board.request(
        "PUT",
        "/firmware",
        bytes(damaged),
        {"Content-Type": "application/octet-stream"},
        timeout=180,
    )
    expect(
        status == 400 and "does not check out" in text, f"a damaged image: {status} {text.strip()}"
    )
    status, text = board.request(
        "PUT",
        "/firmware",
        a,
        {"Content-Type": "application/octet-stream", "Content-Digest": content_digest(b)},
        timeout=180,
    )
    expect(
        status == 400 and "Content-Digest" in text,
        f"a Content-Digest that does not match: {status} {text.strip()}",
    )
    other_chip = bytearray(a)
    other_chip[12:14] = (0x0D).to_bytes(2, "little")  # ESP32-C6
    status, text = board.request("PUT", "/firmware", bytes(other_chip), {}, timeout=60)
    expect(status == 400 and "ESP32-C6" in text, f"another chip's image: {status} {text.strip()}")
    status, text = cut_short_upload(a)
    expect(
        status == 400 and "stopped after" in text, f"an upload cut short: {status} {text.strip()}"
    )
    status, text = board.request("PUT", "/firmware/mode", b"flash", {"Host": "attacker.example"})
    expect(status == 403, f"a Host that is not the board's: {status} {text.strip()}")
    status, text = board.request("POST", "/play", b"http://10.0.2.2:8000/demo.ec3")
    expect(status == 409 and "flash mode" in text, f"a play in flash mode: {status} {text.strip()}")
    state = board.firmware()
    expect_running(state, "ota_1", version_b)
    expect(state.get("mode") == "flash", f"mode after the refusals: {state.get('mode')}")

    print("--- 4: ota.py pushes a.bin back")
    code, _ = ota("push", str(images / "a.bin"), "--host", HOST, "--yes")
    expect(code == 0, f"ota.py push a.bin exited {code}")
    board.wait_accepted(version_a)

    print("--- 5: an image that never becomes healthy goes back at its deadline")
    code, _ = ota("push", str(images / "unhealthy.bin"), "--host", HOST, "--yes")
    expect(code == 2, f"ota.py push unhealthy.bin exited {code}, not 2 (rolled back)")
    state = board.wait_up()
    expect_running(state, "ota_0", version_a)
    last = state.get("last_update") or {}
    expect(
        last.get("result") == "rolled back" and "not healthy within" in last.get("reason", ""),
        f"last_update after the unhealthy image: {last}",
    )

    print("--- 6: an image that panics on its trial is gone back from by the bootloader")
    code, _ = ota("push", str(images / "panic.bin"), "--host", HOST, "--yes")
    expect(code == 2, f"ota.py push panic.bin exited {code}, not 2 (rolled back)")
    state = board.wait_up()
    expect_running(state, "ota_0", version_a)
    last = state.get("last_update") or {}
    expect(
        last.get("result") == "rolled back" and "panicked" in last.get("reason", ""),
        f"last_update after the panicking image: {last}",
    )

    print("--- 7: PUT /firmware/rollback")
    status, text = board.request("PUT", "/firmware/rollback")
    expect(status == 409, f"a rollback to an image that failed its trial: {status} {text.strip()}")
    code, _ = ota("push", str(images / "b.bin"), "--host", HOST, "--yes")
    expect(code == 0, f"ota.py push b.bin exited {code}")
    board.wait_accepted(version_b)
    status, text = board.request("PUT", "/firmware/rollback")
    expect(status == 200, f"a rollback to a.bin: {status} {text.strip()}")
    state = board.wait_accepted(version_a)
    expect_running(state, "ota_0", version_a)

    print("--- 8: POST /restart")
    status, text = board.request("POST", "/restart")
    expect(status == 200, f"POST /restart: {status} {text.strip()}")
    time.sleep(3)
    state = board.wait_up()
    expect_running(state, "ota_0", version_a)

    print("--- 9: a damaged running slot, between two boots")
    board.stop()
    flash = board.out / "qemu_flash.bin"
    data = bytearray(flash.read_bytes())
    data[OTA_0_OFFSET + 0x1000] ^= 0xFF
    flash.write_bytes(bytes(data))
    board.start()
    state = board.wait_up()
    expect_running(state, "ota_1", version_b)

    print("--- 10: flash mode, and out of it")
    status, text = board.request("PUT", "/firmware/mode", b"flash")
    expect(status == 200, f"PUT /firmware/mode flash: {status} {text.strip()}")
    status, text = board.request("GET", "/status")
    expect(
        status == 200 and json.loads(text).get("state") == "flash",
        f"GET /status in flash mode: {text.strip()}",
    )
    status, text = board.request("PUT", "/firmware/mode", b"normal")
    expect(status == 200, f"PUT /firmware/mode normal: {status} {text.strip()}")
    time.sleep(3)
    state = board.wait_up()
    expect(state.get("mode") == "normal", f"mode after a restart: {state.get('mode')}")


def image_version(image: bytes) -> str:
    """esp_app_desc_t's version, 48 bytes into the image."""
    field = image[48:80]
    return field.split(b"\0", 1)[0].decode("utf-8", "replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--out", default=Path("ota-qemu-run"), type=Path)
    args = parser.parse_args()
    # A CI log shows each step as it happens, not all of them at the end.
    sys.stdout.reconfigure(line_buffering=True)

    shutil.rmtree(args.out, ignore_errors=True)
    args.out.mkdir(parents=True)
    # QEMU writes to its flash image, so the board boots a copy.
    for name in ("qemu_flash.bin", "qemu_efuse.bin"):
        shutil.copy(args.images / name, args.out / name)

    board = Board(args.qemu, args.out)
    try:
        run(board, args.images)
    except Failure as failure:
        print(f"::error title={TITLE}::{failure}", file=sys.stderr)
        for log in sorted(args.out.glob("qemu-ota-*.txt")):
            lines = log.read_text("utf-8", "replace").splitlines()
            keep = [
                line for line in lines if "firmware:" in line or "boot:" in line or "abort" in line
            ]
            print(f"--- {log.name}", file=sys.stderr)
            print("\n".join(keep[-40:]), file=sys.stderr)
        return 1
    finally:
        board.close()
    print(f"{TITLE}: every step passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
