#!/usr/bin/env python3
"""Provision hearth_sink over Improv Wi-Fi under QEMU, and hold it to its answers.

planning/hearth-reference-player.md, B2. A board with no network listens for
Improv Wi-Fi (https://www.improv-wifi.com/serial/) on its console, and a
client there gives it an SSID and a passphrase:
esp-idf/ac3forge/examples/hearth_sink/main/provision.cpp answers, in the
packet format of esp-idf/ac3forge/include/ac3forge/improv.hpp. QEMU has no
radio, so tools/checks/run_improv_qemu.sh boots two images, one for each of
these scenarios:

  late-network   The Sendspin player on QEMU's Ethernet (sdkconfig.ci-sendspin),
                 started with its link down. The boot's network_up() gives up
                 after 30 s and the board listens. This sends it a network and
                 brings the link up while the board waits for an address. The
                 board must answer as a client expects: error none, its page's
                 URL, provisioned. Then it must start what boot would have
                 started: mDNS, the Sendspin player and the player's console
                 commands. The capture goes on while the runner plays to the
                 player, until QEMU exits. Before #741 the board aborted in
                 that second network_up() and restarted.
  unprovisioned  The WiFi build with nothing stored and nothing built in
                 (sdkconfig.ci-wifi). It must boot with its control surface up,
                 listen, and answer current_state (ready) and device_info. It is
                 never sent a network: esp_wifi_start() does not return under
                 QEMU. Before #741 this build restarted in a loop on an lwIP
                 assertion.

The caller starts QEMU with -S, with its monitor and its console on TCP
servers (-monitor tcp:HOST:PORT,server=on,wait=off, and the same for
-serial). This connects to both before the part runs, then starts it, and
writes the console to --capture as it arrives. The capture holds the lines
the part printed, and each Improv packet it sent on a line of its own
("[improv] current_state ready"); tools/checks/check_esp_console.py reads it
afterwards. A monitor command is sent only once the "(qemu)" prompt is in:
QEMU drops one sent before it.

Once the scenario's checks have passed, this creates --ready, if given. It
then goes on capturing until QEMU closes the console or --hold seconds pass.
A check that fails prints one GitHub ::error:: annotation and ends the run.
A reset or panic output ends it too, a few seconds later, so that the
capture has the backtrace. Exit status 0 when every check passed, 1 when one
failed, 2 for a usage error.

  python3 tools/checks/improv_qemu.py late-network --serial 127.0.0.1:15571 \\
      --monitor 127.0.0.1:15572 --capture qemu-improv.txt --ready improv.ready

Standard library only, like the other scripts here. Its tests are
test_improv_qemu.py.
"""

import argparse
import re
import socket
import sys
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

# ---------------------------------------------------------------------------
# The packet format: ac3forge/improv.hpp, byte for byte.
# ---------------------------------------------------------------------------

HEADER = b"IMPROV"
VERSION = 1
# The header, version, type and length before the data, and the checksum after it.
OVERHEAD = len(HEADER) + 4
MAX_DATA = 255
# Where the length byte is, from the start of the header.
LENGTH_AT = len(HEADER) + 2

# Packet types.
CURRENT_STATE = 0x01
ERROR_STATE = 0x02
RPC = 0x03
RPC_RESULT = 0x04
KINDS = {
    CURRENT_STATE: "current_state",
    ERROR_STATE: "error_state",
    RPC: "rpc",
    RPC_RESULT: "rpc_result",
}

STATES = {0x00: "stopped", 0x02: "ready", 0x03: "provisioning", 0x04: "provisioned"}
ERRORS = {
    0x00: "none",
    0x01: "invalid_packet",
    0x02: "unknown_command",
    0x03: "cannot_connect",
    0x05: "bad_hostname",
    0xFF: "unknown",
}

# RPC commands.
WIFI_SETTINGS = 0x01
GET_CURRENT_STATE = 0x02
GET_DEVICE_INFO = 0x03
COMMANDS = {
    WIFI_SETTINGS: "wifi_settings",
    GET_CURRENT_STATE: "current_state",
    GET_DEVICE_INFO: "device_info",
    0x04: "scan",
    0x05: "hostname",
    0x06: "device_name",
    0x07: "network_state",
}


def checksum(data: bytes) -> int:
    """Every byte from the header through the last data byte, modulo 256. The
    specification does not say; improv.hpp computes this, as ESPHome's
    improv_serial and the improv-wifi library do."""
    return sum(data) & 0xFF


def packet(kind: int, data: bytes) -> bytes:
    """One whole packet of `kind` carrying `data`."""
    if len(data) > MAX_DATA:
        raise ValueError(f"an Improv packet carries at most {MAX_DATA} bytes, not {len(data)}")
    body = HEADER + bytes([VERSION, kind, len(data)]) + data
    return body + bytes([checksum(body)])


def rpc(command: int, payload: bytes = b"") -> bytes:
    """A client's request: the command, the length of its own data, the data."""
    return packet(RPC, bytes([command, len(payload)]) + payload)


def wifi_settings(ssid: str, password: str) -> bytes:
    """The request that hands a board a network: each string after its length."""
    name = ssid.encode("utf-8")
    secret = password.encode("utf-8")
    if not name:
        raise ValueError("an SSID cannot be empty")
    return rpc(WIFI_SETTINGS, bytes([len(name)]) + name + bytes([len(secret)]) + secret)


@dataclass(frozen=True)
class Answer:
    """One packet a board sent.

    `kind` is current_state, error_state or rpc_result. `value` is the state,
    the error, or the command a result answers. `strings` are a result's
    strings. `problem` says why a client could not use the packet, and is empty
    when it could.
    """

    kind: str
    value: str = ""
    strings: tuple[str, ...] = ()
    problem: str = ""

    def __str__(self) -> str:
        if self.problem:
            return f"[improv] bad packet: {self.problem}"
        text = f"[improv] {self.kind} {self.value}"
        if self.strings:
            text += " " + " ".join(repr(s) for s in self.strings)
        return text


def _name(table: dict[int, str], value: int) -> str:
    return table.get(value, f"0x{value:02x}")


def decode(raw: bytes) -> Answer:
    """What one packet from a board says. `raw` runs from the header through the
    checksum."""
    if len(raw) < OVERHEAD or not raw.startswith(HEADER):
        return Answer("unknown", problem=f"not a whole packet: {raw.hex()}")
    kind = _name(KINDS, raw[len(HEADER) + 1])
    length = raw[LENGTH_AT]
    if len(raw) != OVERHEAD + length:
        return Answer(kind, problem=f"{len(raw)} bytes for a length of {length}: {raw.hex()}")
    if checksum(raw[:-1]) != raw[-1]:
        why = f"checksum 0x{raw[-1]:02x}, not 0x{checksum(raw[:-1]):02x}"
        if raw[-1] == ord("\r") and checksum(raw[:-1]) == ord("\n"):
            # ESP-IDF's console sends CR before every LF by default
            # (CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF), a packet's included. A
            # packet whose one varying byte is its checksum - device_info's,
            # with the version in it - meets this once in 256 versions.
            why += "; the checksum is LF, and the console sent CR before it"
        return Answer(kind, problem=f"{why}: {raw.hex()}")
    if raw[len(HEADER)] != VERSION:
        return Answer(kind, problem=f"version {raw[len(HEADER)]}, not {VERSION}: {raw.hex()}")
    data = raw[LENGTH_AT + 1 : -1]
    if kind in ("current_state", "error_state"):
        if len(data) != 1:
            return Answer(kind, problem=f"{len(data)} data bytes, not 1: {raw.hex()}")
        return Answer(kind, _name(STATES if kind == "current_state" else ERRORS, data[0]))
    if kind != "rpc_result":
        return Answer(kind, problem=f"a {kind} packet, which a board does not send: {raw.hex()}")
    if len(data) < 2 or data[1] != len(data) - 2:
        return Answer(kind, problem=f"a result whose own length does not fit it: {raw.hex()}")
    strings = []
    at = 2
    while at < len(data):
        size = data[at]
        if at + 1 + size > len(data):
            return Answer(kind, problem=f"a string that runs past the result: {raw.hex()}")
        strings.append(data[at + 1 : at + 1 + size].decode("utf-8", errors="replace"))
        at += 1 + size
    return Answer(kind, _name(COMMANDS, data[0]), tuple(strings))


class ConsoleParser:
    """Splits what a console sends into its lines and the Improv packets among them.

    The specification's serial transport is the console itself: a board prints
    its lines and sends its packets on the same port, and a client picks the
    packets out. `feed` takes bytes as they arrive, in pieces of any size, and
    returns what they completed, in order: a line as a str, without its line
    ending, and a packet as an Answer. A packet can arrive part-way through a
    line, and the text on either side of it is then one line. A packet's bytes
    can include a newline, so a packet is read to its length and never split
    at one.
    """

    # A line this long with no end is passed on as it stands rather than held
    # without bound.
    MAX_LINE = 8192

    def __init__(self) -> None:
        self._pending = bytearray()
        self._line = bytearray()

    def feed(self, data: bytes) -> list[str | Answer]:
        self._pending += data
        events: list[str | Answer] = []
        while True:
            start = self._pending.find(HEADER)
            newline = self._pending.find(b"\n")
            if start != -1 and (newline == -1 or start < newline):
                if len(self._pending) <= start + LENGTH_AT:
                    break  # the length has not arrived
                end = start + OVERHEAD + self._pending[start + LENGTH_AT]
                if len(self._pending) < end:
                    break
                self._line += self._pending[:start]
                events.append(decode(bytes(self._pending[start:end])))
                del self._pending[:end]
                continue
            if newline == -1:
                break
            self._line += self._pending[:newline]
            del self._pending[: newline + 1]
            events.append(self._take_line())
        held = len(self._line) + len(self._pending)
        if held > self.MAX_LINE and self._pending.find(HEADER) == -1:
            # All but what could be the start of a header.
            keep = len(HEADER) - 1
            self._line += self._pending[:-keep]
            del self._pending[:-keep]
            events.append(self._take_line())
        return events

    def flush(self) -> list[str | Answer]:
        """Whatever is left when the console closes, as a last line."""
        self._line += self._pending
        self._pending.clear()
        return [self._take_line()] if self._line else []

    def _take_line(self) -> str:
        text = self._line.decode("utf-8", errors="replace").rstrip("\r")
        self._line.clear()
        return text


# ---------------------------------------------------------------------------
# QEMU's human monitor.
# ---------------------------------------------------------------------------

PROMPT = "(qemu)"
ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def monitor_output(reply: bytes) -> str:
    """What a monitor command printed, given everything the monitor sent from
    the command up to its next prompt.

    The monitor echoes a command as it is typed, redrawing the line with
    escape sequences, and answers the newline that ends it with CR LF. A
    command's own output follows, then the prompt. set_link and cont print
    nothing when they work.
    """
    text = reply.decode("utf-8", errors="replace")
    _, found, rest = text.partition("\r\n")
    if not found:
        return ""
    rest = rest.rsplit(PROMPT, 1)[0]
    return ESCAPE.sub("", rest).replace("\r\n", "\n").strip()


def parse_address(text: str) -> tuple[str, int]:
    host, _, port = text.rpartition(":")
    if not host or not port.isdigit():
        raise argparse.ArgumentTypeError(f"{text!r} is not HOST:PORT")
    return host, int(port)


def connect(address: tuple[str, int], seconds: float) -> socket.socket:
    """A connection to one of QEMU's TCP servers, which may not be listening yet."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            return socket.create_connection(address, timeout=5)
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.2)


class Monitor:
    """A connection to QEMU's human monitor, one command at a time."""

    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._sock.settimeout(0.5)
        self._read_to_prompt(b"", 30)

    def command(self, text: str, seconds: float = 30) -> str:
        """Runs `text`, and returns what it printed."""
        self._sock.sendall(text.encode("ascii") + b"\n")
        return monitor_output(self._read_to_prompt(b"\r\n", seconds))

    def _read_to_prompt(self, after: bytes, seconds: float) -> bytes:
        received = b""
        deadline = time.monotonic() + seconds
        while True:
            at = received.find(after)
            if at != -1 and received.find(PROMPT.encode(), at + len(after)) != -1:
                # The prompt's trailing space may still be on its way. The next
                # command's echo takes it in.
                return received
            if time.monotonic() > deadline:
                raise TimeoutError(f"no monitor prompt in {seconds:g} s: {received[-200:]!r}")
            try:
                chunk = self._sock.recv(4096)
            except TimeoutError:
                continue
            if not chunk:
                raise ConnectionError("QEMU closed its monitor")
            received += chunk

    def close(self) -> None:
        self._sock.close()


# ---------------------------------------------------------------------------
# The board's console.
# ---------------------------------------------------------------------------

# What the ROM prints first on every boot, and what ESP-IDF prints as the
# application dies: check_esp_console.py's lists, with the first line of a
# failed ESP_ERROR_CHECK, which names the error. Looked for here only to stop
# waiting for answers that will not come; check_esp_console.py gives the
# verdict on the capture.
BANNER = "ESP-ROM:"
PANIC = (
    "ESP_ERROR_CHECK failed",
    "Guru Meditation",
    "assert failed",
    "abort() was called",
    "Backtrace:",
    "Rebooting",
)

Event = str | Answer
Match = Callable[[Event], bool]


class Failed(Exception):
    """A check that did not pass, with the sentence that says so."""


class Console:
    """The board's console: captured to a file, and watched for lines and answers.

    A thread reads the socket and writes each line and packet to the capture
    as it completes, then records it. A scenario waits on the records.
    """

    def __init__(self, sock: socket.socket, capture: Path, log: Callable[[str], None]) -> None:
        self._sock = sock
        self._sock.settimeout(0.5)
        self._file = capture.open("w", encoding="utf-8", newline="\n")
        self._log = log
        self._parser = ConsoleParser()
        self._events: list[Event] = []
        self._changed = threading.Condition()
        self._closed = False
        self._stop = False
        self._boots = 0
        # The record that shows the part reset or died, and its text.
        self._stopped_at: int | None = None
        self._stopped = ""
        self._thread = threading.Thread(target=self._run, name="console", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop:
            try:
                data = self._sock.recv(4096)
            except TimeoutError:
                continue
            except OSError:
                data = b""
            for event in self._parser.feed(data) if data else self._parser.flush():
                self._record(event)
            if not data:
                break
        with self._changed:
            self._closed = True
            self._changed.notify_all()

    def _record(self, event: Event) -> None:
        self._file.write(f"{event}\n")
        self._file.flush()
        if isinstance(event, Answer):
            self._log(str(event))
        with self._changed:
            if isinstance(event, str) and self._stopped_at is None:
                if BANNER in event:
                    self._boots += 1
                if self._boots > 1:
                    self._stopped = f"the part booted again: {event.strip()}"
                elif any(marker in event for marker in PANIC):
                    self._stopped = event.strip()
                if self._stopped:
                    self._stopped_at = len(self._events)
            self._events.append(event)
            self._changed.notify_all()

    def mark(self) -> int:
        """The number of records so far, for a wait that looks only at later ones."""
        with self._changed:
            return len(self._events)

    def send(self, data: bytes) -> None:
        self._sock.sendall(data)

    def wait(self, since: int, match: Match, seconds: float, waiting: str) -> tuple[Event, int]:
        """The first record from `since` on that `match` accepts, and the index
        after it. Fails when none comes in `seconds`, when the console closes
        first, or when the part resets or panics before one; `waiting` says
        what for, as "waiting for ..."."""
        deadline = time.monotonic() + seconds
        with self._changed:
            while True:
                end = len(self._events) if self._stopped_at is None else self._stopped_at
                for index in range(since, end):
                    if match(self._events[index]):
                        return self._events[index], index + 1
                if self._stopped_at is not None:
                    raise Failed(f"{waiting}: the part stopped first: {self._stopped}")
                if self._closed:
                    raise Failed(f"{waiting}: the console closed first")
                left = deadline - time.monotonic()
                if left <= 0:
                    raise Failed(f"{waiting}: nothing in {seconds:g} s")
                self._changed.wait(left)

    def bad_packets(self) -> list[Answer]:
        with self._changed:
            return [e for e in self._events if isinstance(e, Answer) and e.problem]

    @property
    def stopped(self) -> bool:
        with self._changed:
            return self._stopped_at is not None

    def hold(self, seconds: float) -> None:
        """Captures on until the console closes or `seconds` pass."""
        deadline = time.monotonic() + seconds
        with self._changed:
            while not self._closed:
                left = deadline - time.monotonic()
                if left <= 0:
                    return
                self._changed.wait(left)

    def close(self) -> None:
        self._stop = True
        self._thread.join(timeout=5)
        self._sock.close()
        self._file.close()


class Run:
    """A scenario's steps. Each waits from where the one before it left off,
    and says what it found and when."""

    def __init__(self, console: Console, monitor: Monitor, started: float) -> None:
        self.console = console
        self.monitor = monitor
        self.started = started
        # The record the next wait starts from.
        self.at = 0

    def say(self, text: str) -> None:
        say(text, self.started)

    def qemu(self, command: str) -> None:
        printed = self.monitor.command(command)
        if printed:
            raise Failed(f"QEMU's monitor refused '{command}': {printed}")
        self.say(f"qemu: {command}")

    def line(self, text: str, seconds: float) -> None:
        """Waits for the next console line holding `text`."""
        event, self.at = self.console.wait(
            self.at,
            lambda event: isinstance(event, str) and text in event,
            seconds,
            f"waiting for a console line with '{text}'",
        )
        self.say(f"console: {str(event).strip()}")

    def request(self, data: bytes, name: str) -> None:
        """Sends a request; the next wait looks only at what came after it."""
        self.at = self.console.mark()
        self.console.send(data)
        self.say(f"sent {name}")

    def answers(self, expected: list[tuple[str, str]], seconds: float, what: str) -> list[Answer]:
        """The board's next packets, which must be `expected`, (kind, value)
        pairs in order. A packet other than the next one expected fails the
        step, as a client would stop there."""
        got: list[Answer] = []
        deadline = time.monotonic() + seconds
        for kind, value in expected:
            after = f" after {', '.join(a.kind + ' ' + a.value for a in got)}" if got else ""
            event, self.at = self.console.wait(
                self.at,
                lambda event: isinstance(event, Answer),
                max(deadline - time.monotonic(), 0.0),
                f"{what}, waiting for {kind} {value}{after}",
            )
            assert isinstance(event, Answer)
            if event.problem or (event.kind, event.value) != (kind, value):
                raise Failed(f"{what}: {event} where {kind} {value} was due")
            got.append(event)
        return got

    def no_bad_packets(self) -> None:
        bad = self.console.bad_packets()
        if bad:
            raise Failed(f"the board sent {len(bad)} packet(s) a client cannot read: {bad[0]}")


def say(text: str, started: float) -> None:
    print(f"{time.monotonic() - started:7.1f}s {text}", flush=True)


# The network the board is given. The openeth seam ignores the name and joins
# QEMU's Ethernet, but the board still stores what it was given. No byte of
# either request is a CR or an LF: the console's line-ending conversion
# rewrites those on their way in (CONFIG_LIBC_STDIN_LINE_ENDING_CR), and that
# is not what this check measures.
SSID = "qemu-net"
PASSPHRASE = "qemu-passphrase"
# Where QEMU's user-mode network puts the guest, so the page a client is sent to.
URL = "http://10.0.2.15/"
# The name a board with nothing stored takes from its MAC address, all zeros
# under QEMU.
NAME = "hearth-000000"

# How long each wait may take. QEMU runs slower than a board, and a CI runner
# slower than a desktop: on a desktop, boot and the 30 s address wait took
# 31 s, and the join after the link came up 3 s.
BOOT_SECONDS = 120
ANSWER_SECONDS = 15
JOIN_SECONDS = 60
# How long after the board says it is provisioning the link comes up: inside
# network_up()'s 30 s wait, and well after its start.
LINK_DELAY_SECONDS = 5


def late_network(run: Run) -> None:
    run.qemu("set_link n0 off")
    run.qemu("cont")
    run.line(BANNER, BOOT_SECONDS)
    run.line("openeth got no address", BOOT_SECONDS)
    announced = run.at
    run.line("improv: listening on the console for Wi-Fi credentials", ANSWER_SECONDS)
    # Improv's task says where it stands as it starts, before anyone asks, and
    # its line may come before or after that.
    run.at = announced
    run.answers([("current_state", "ready")], ANSWER_SECONDS, "Improv's first state")

    run.request(rpc(GET_CURRENT_STATE), "current_state")
    run.answers(
        [("error_state", "none"), ("current_state", "ready")],
        ANSWER_SECONDS,
        "current_state with no network",
    )

    run.request(wifi_settings(SSID, PASSPHRASE), f"wifi_settings for '{SSID}'")
    sent = run.at
    run.answers([("current_state", "provisioning")], ANSWER_SECONDS, "wifi_settings")
    # The board is in network_up() now, waiting for an address.
    time.sleep(LINK_DELAY_SECONDS)
    run.qemu("set_link n0 on")
    result = run.answers(
        [
            ("error_state", "none"),
            ("rpc_result", "wifi_settings"),
            ("current_state", "provisioned"),
        ],
        JOIN_SECONDS,
        "wifi_settings once the link is up",
    )[1]
    if result.strings[:1] != (URL,):
        raise Failed(f"wifi_settings answered {result.strings}, not the page at {URL}")
    run.say(f"provisioned, with the page at {URL}")

    # What app_main starts once a network comes up after boot. Its task may
    # have printed the first of these before Improv's answers went out.
    run.at = sent
    run.line(f"mdns: {NAME}.local, _sendspin._tcp", ANSWER_SECONDS)
    run.line("sendspin: pairing token", ANSWER_SECONDS)
    run.line("console: listening for commands", ANSWER_SECONDS)

    # A provisioned board gives its page with its state.
    run.request(rpc(GET_CURRENT_STATE), "current_state")
    result = run.answers(
        [
            ("error_state", "none"),
            ("current_state", "provisioned"),
            ("rpc_result", "current_state"),
        ],
        ANSWER_SECONDS,
        "current_state once provisioned",
    )[2]
    if result.strings[:1] != (URL,):
        raise Failed(f"current_state answered {result.strings}, not the page at {URL}")
    run.no_bad_packets()


def unprovisioned(run: Run) -> None:
    run.qemu("cont")
    run.line(BANNER, BOOT_SECONDS)
    announced = run.at
    run.line("control: http on port 80", BOOT_SECONDS)
    run.line("improv: listening on the console for Wi-Fi credentials", ANSWER_SECONDS)
    run.at = announced
    run.answers([("current_state", "ready")], ANSWER_SECONDS, "Improv's first state")

    run.request(rpc(GET_CURRENT_STATE), "current_state")
    run.answers(
        [("error_state", "none"), ("current_state", "ready")],
        ANSWER_SECONDS,
        "current_state with nothing stored",
    )

    run.request(rpc(GET_DEVICE_INFO), "device_info")
    info = run.answers(
        [("error_state", "none"), ("rpc_result", "device_info")],
        ANSWER_SECONDS,
        "device_info",
    )[1]
    # The firmware, its version, the chip and the board's name.
    wanted = ("AC3Forge Hearth sink", "ESP32-S3", NAME)
    if len(info.strings) != 4 or (info.strings[0], *info.strings[2:]) != wanted:
        raise Failed(f"device_info answered {info.strings}")
    run.say(f"device_info: {', '.join(info.strings)}")
    run.no_bad_packets()


SCENARIOS: dict[str, Callable[[Run], None]] = {
    "late-network": late_network,
    "unprovisioned": unprovisioned,
}

# How long a scenario that passed goes on capturing unless QEMU exits first,
# and how long the capture runs on after a reset or a panic, for the
# backtrace.
HOLD_SECONDS = 600
AFTER_STOP_SECONDS = 3


def annotation(title: str, message: str) -> str:
    """A GitHub Actions ::error:: command, escaped the way the runner reads one."""

    def escape(text: str) -> str:
        return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")

    title = escape(title).replace(":", "%3A").replace(",", "%2C")
    return f"::error title={title}::{escape(message)}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Provision hearth_sink over Improv Wi-Fi under QEMU, "
        "and hold it to its answers."
    )
    parser.add_argument("scenario", choices=sorted(SCENARIOS))
    parser.add_argument(
        "--serial", type=parse_address, required=True, help="QEMU's console, as HOST:PORT"
    )
    parser.add_argument(
        "--monitor", type=parse_address, required=True, help="QEMU's monitor, as HOST:PORT"
    )
    parser.add_argument("--capture", type=Path, required=True, help="where the console goes")
    parser.add_argument("--ready", type=Path, help="a file to create once the checks have passed")
    parser.add_argument(
        "--hold",
        type=float,
        default=HOLD_SECONDS,
        metavar="SECONDS",
        help="how long to go on capturing after the checks, unless QEMU exits first",
    )
    parser.add_argument(
        "--title", default="hearth_sink Improv", help="the title of the ::error:: annotations"
    )
    args = parser.parse_args(argv)

    started = time.monotonic()
    try:
        serial = connect(args.serial, 30)
        monitor = Monitor(connect(args.monitor, 30))
    except OSError as error:
        print(annotation(args.title, f"{args.scenario}: cannot reach QEMU: {error}"))
        return 1
    console = Console(serial, args.capture, lambda text: say(text, started))
    try:
        try:
            SCENARIOS[args.scenario](Run(console, monitor, started))
        except (Failed, OSError) as error:
            if console.stopped:
                console.hold(AFTER_STOP_SECONDS)
            print(annotation(args.title, f"{args.scenario}: {error}"))
            return 1
        finally:
            monitor.close()
        say(f"{args.scenario}: every check passed", started)
        if args.ready is not None:
            args.ready.touch()
        console.hold(args.hold)
        return 0
    finally:
        console.close()


if __name__ == "__main__":
    sys.exit(main())
