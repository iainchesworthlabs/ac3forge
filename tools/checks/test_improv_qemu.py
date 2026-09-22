"""Unit tests for improv_qemu.py, the Improv check hearth_sink runs under QEMU.

stdlib `unittest`, as the other suites here are: this runs in ci.yml's
script-lint job. The packets are written out byte for byte, with checksums
worked by hand from ac3forge/improv.hpp's rule, so a test cannot pass by
agreeing with the code under test. The scenarios run against a board played
by a thread on two local sockets, one for the console and one for QEMU's
monitor.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import argparse
import contextlib
import http.server
import io
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import improv_qemu as improv

# 'IMPROV' sums to 477: 73 + 77 + 80 + 82 + 79 + 86.
STATE_READY = b"IMPROV\x01\x01\x01\x02\xe2"  # 477 + 1 + 1 + 1 + 2 = 482 = 0x1e2
STATE_PROVISIONING = b"IMPROV\x01\x01\x01\x03\xe3"
STATE_PROVISIONED = b"IMPROV\x01\x01\x01\x04\xe4"
ERROR_NONE = b"IMPROV\x01\x02\x01\x00\xe1"  # 477 + 1 + 2 + 1 + 0 = 481 = 0x1e1
# 477 + 1 + 4 + 20, then 1 + 18 + 17 and the URL's 1,082: 1,620 = 0x654.
RESULT_URL = b"IMPROV\x01\x04\x14\x01\x12\x11http://10.0.2.15/\x54"


def result(command: int, *strings: str) -> bytes:
    """A board's rpc_result, as improv.hpp's write_result lays it out."""
    body = b"".join(bytes([len(s)]) + s.encode() for s in strings)
    return improv.packet(improv.RPC_RESULT, bytes([command, len(body)]) + body)


class Requests(unittest.TestCase):
    def test_current_state(self):
        # 477 + 1 + 3 + 2, then command 2 and length 0: 485 = 0x1e5.
        self.assertEqual(improv.rpc(improv.GET_CURRENT_STATE), b"IMPROV\x01\x03\x02\x02\x00\xe5")

    def test_device_info(self):
        self.assertEqual(improv.rpc(improv.GET_DEVICE_INFO), b"IMPROV\x01\x03\x02\x03\x00\xe6")

    def test_wifi_settings_lays_out_each_string_after_its_length(self):
        # 477 + 1 + 3 + 7, then 1, 5, 2, 'a' 97, 'b' 98, 1, 'c' 99: 791 = 0x317.
        self.assertEqual(
            improv.wifi_settings("ab", "c"), b"IMPROV\x01\x03\x07\x01\x05\x02ab\x01c\x17"
        )

    def test_an_empty_password_still_has_its_length(self):
        # improv.hpp's Reader drops a wifi_settings with no password length.
        sent = improv.wifi_settings("net", "")
        self.assertEqual(sent[6:-1], b"\x01\x03\x07\x01\x05\x03net\x00")
        self.assertEqual(sent[-1], sum(sent[:-1]) % 256)

    def test_the_checks_own_network_carries_a_cr_byte(self):
        # 13 bytes on purpose: the length byte in front of the SSID is then CR,
        # which a console that converts line endings on the way in turns into
        # LF. The check exists to fail when that happens.
        sent = improv.wifi_settings(improv.SSID, improv.PASSPHRASE)
        self.assertEqual(len(improv.SSID), 13)
        self.assertEqual(sent[8], 2 + 1 + len(improv.SSID) + 1 + len(improv.PASSPHRASE))
        self.assertIn(b"\r", sent)

    def test_the_checks_own_name_carries_an_lf_byte(self):
        # And 10 characters the other way: the length byte in front of the name
        # in a device_info answer is LF, which a console that sends CR before
        # every LF corrupts.
        self.assertEqual(len(improv.NEW_NAME), 10)
        answer = result(3, "AC3Forge Hearth sink", "1", "ESP32-S3", improv.NEW_NAME)
        self.assertIn(b"\n", answer)

    def test_refuses_what_a_packet_cannot_carry(self):
        with self.assertRaises(ValueError):
            improv.wifi_settings("", "password")
        with self.assertRaises(ValueError):
            improv.packet(improv.RPC, bytes(256))


class Decode(unittest.TestCase):
    def test_states_and_errors(self):
        self.assertEqual(improv.decode(STATE_READY), improv.Answer("current_state", "ready"))
        self.assertEqual(
            improv.decode(STATE_PROVISIONED), improv.Answer("current_state", "provisioned")
        )
        self.assertEqual(improv.decode(ERROR_NONE), improv.Answer("error_state", "none"))
        cannot = b"IMPROV\x01\x02\x01\x03\xe4"
        self.assertEqual(improv.decode(cannot), improv.Answer("error_state", "cannot_connect"))

    def test_a_result_and_its_strings(self):
        self.assertEqual(
            improv.decode(RESULT_URL),
            improv.Answer("rpc_result", "wifi_settings", ("http://10.0.2.15/",)),
        )
        info = improv.decode(result(3, "AC3Forge Hearth sink", "0.10.0", "ESP32-S3", "hearth-1"))
        self.assertEqual(info.value, "device_info")
        self.assertEqual(info.strings, ("AC3Forge Hearth sink", "0.10.0", "ESP32-S3", "hearth-1"))
        self.assertEqual(improv.decode(result(4)), improv.Answer("rpc_result", "scan"))

    def test_an_unknown_value_is_named_by_its_number(self):
        self.assertEqual(improv.decode(improv.packet(1, b"\x09")).value, "0x09")

    def test_what_a_client_could_not_use(self):
        bad = {
            "checksum": STATE_READY[:-1] + b"\xe3",
            "version": b"IMPROV\x02\x01\x01\x02\xe3",
            "bytes for a length": STATE_READY[:-1],
            "not 1": improv.packet(improv.CURRENT_STATE, b"\x02\x02"),
            "does not send": improv.rpc(improv.GET_CURRENT_STATE),
            "own length": improv.packet(improv.RPC_RESULT, b"\x01\x05\x01x"),
            "runs past": improv.packet(improv.RPC_RESULT, b"\x01\x02\x05x"),
            "whole packet": b"IMPROV\x01",
        }
        for why, raw in bad.items():
            with self.subTest(why=why):
                answer = improv.decode(raw)
                self.assertIn(why, answer.problem)
                self.assertTrue(str(answer).startswith("[improv] bad packet: "))

    def test_reads_as_a_line_of_the_capture(self):
        self.assertEqual(str(improv.decode(STATE_READY)), "[improv] current_state ready")
        self.assertEqual(
            str(improv.decode(RESULT_URL)),
            "[improv] rpc_result wifi_settings 'http://10.0.2.15/'",
        )


# A console as a board sends it: lines ending in CR LF, packets between and
# inside them.
STREAM = (
    b"ESP-ROM:esp32s3-20210327\r\n"
    + STATE_READY
    + b"improv: listening on the console for Wi-Fi credentials\r\n"
    + b"sendspin: "
    + ERROR_NONE
    + b"half a line\r\n"
    + RESULT_URL
)
EVENTS = [
    "ESP-ROM:esp32s3-20210327",
    improv.Answer("current_state", "ready"),
    "improv: listening on the console for Wi-Fi credentials",
    improv.Answer("error_state", "none"),
    "sendspin: half a line",
    improv.Answer("rpc_result", "wifi_settings", ("http://10.0.2.15/",)),
]


def parse(chunks: list[bytes]) -> list:
    parser = improv.ConsoleParser()
    events = []
    for chunk in chunks:
        events += parser.feed(chunk)
    return events + parser.flush()


class ConsoleParser(unittest.TestCase):
    def test_lines_and_packets_in_order(self):
        self.assertEqual(parse([STREAM]), EVENTS)

    def test_the_same_whatever_the_pieces(self):
        for size in range(1, 24):
            with self.subTest(size=size):
                pieces = [STREAM[i : i + size] for i in range(0, len(STREAM), size)]
                self.assertEqual(parse(pieces), EVENTS)

    def test_a_newline_inside_a_packet_does_not_end_a_line(self):
        # A ten-character name: its length byte is LF.
        named = result(6, "kitchen-01")
        self.assertIn(b"\n", named)
        events = parse([b"before ", named, b"after\r\n"])
        self.assertEqual(
            events, [improv.Answer("rpc_result", "device_name", ("kitchen-01",)), "before after"]
        )

    def test_a_packet_the_console_rewrote_is_bad(self):
        # With CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF the console sends CR before
        # every LF, a packet's included, and a client reads a broken packet.
        named = result(6, "kitchen-01")
        events = parse([named.replace(b"\n", b"\r\n") + b"next\r\n"])
        self.assertIn("checksum", events[0].problem)
        self.assertTrue(events[1].endswith("next"))

    def test_a_checksum_the_console_sent_as_cr_lf_is_named(self):
        # A device_name result whose checksum is LF and whose other bytes are
        # not, as the board would send it: CR, then the checksum.
        named = next(
            raw
            for raw in (result(6, f"v{n}") for n in range(1000))
            if raw[-1] == ord("\n") and b"\n" not in raw[:-1]
        )
        events = parse([named[:-1] + b"\r\n" + b"next\r\n"])
        self.assertIn("the checksum is LF, and the console sent CR before it", events[0].problem)
        self.assertEqual(events[1:], ["", "next"])

    def test_an_unfinished_line_is_passed_on_at_the_end(self):
        self.assertEqual(parse([b"one\r\ntwo"]), ["one", "two"])

    def test_an_endless_line_is_passed_on_before_it_grows_without_bound(self):
        parser = improv.ConsoleParser()
        events = parser.feed(b"x" * (improv.ConsoleParser.MAX_LINE + 100) + b"IMPR")
        self.assertEqual(len(events), 1)
        self.assertTrue(set(events[0]) <= {"x"})
        # The start of a header is kept for the bytes that complete it.
        self.assertEqual(parser.feed(STATE_READY[4:])[-1], improv.Answer("current_state", "ready"))

    def test_bytes_that_are_not_text(self):
        self.assertEqual(parse([b"\xff\xfeok\r\n"]), ["\N{REPLACEMENT CHARACTER}" * 2 + "ok"])


class Monitor(unittest.TestCase):
    # What QEMU 9.2.2's monitor sent for two commands in a local run: the typed
    # command redrawn a character at a time, then its output and the prompt.
    CONT = b"c\x1b[K\x1b[Dco\x1b[K\x1b[D\x1b[Dcon\x1b[K\x1b[D\x1b[D\x1b[Dcont\x1b[K\r\n(qemu) "
    REFUSED = (
        b"set_link\x1b[K\r\nset_link: string expected\r\n"
        b'Try "help set_link" for more information\r\n(qemu) '
    )

    def test_a_command_that_prints_nothing(self):
        self.assertEqual(improv.monitor_output(self.CONT), "")

    def test_a_command_that_was_refused(self):
        self.assertEqual(
            improv.monitor_output(self.REFUSED),
            'set_link: string expected\nTry "help set_link" for more information',
        )

    def test_addresses(self):
        self.assertEqual(improv.parse_address("127.0.0.1:15571"), ("127.0.0.1", 15571))
        for text in ("15571", "host:", "host:port"):
            with self.subTest(text=text), self.assertRaises(argparse.ArgumentTypeError):
                improv.parse_address(text)


class Annotation(unittest.TestCase):
    def test_escapes_what_the_runner_would_misread(self):
        self.assertEqual(
            improv.annotation("S3: a, b", "50% of\nit"), "::error title=S3%3A a%2C b::50%25 of%0Ait"
        )


# ---------------------------------------------------------------------------
# The scenarios, against a board played by a thread.
# ---------------------------------------------------------------------------

BOOT = b"ESP-ROM:esp32s3-20210327\r\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"


class FakeQemu:
    """QEMU's monitor and a hearth_sink console, on two local ports.

    `board` runs once the monitor has been told `cont`, with this object. It
    reads requests with `request()` and answers with `send()`; `link_up` is set
    when the monitor is told `set_link n0 on`. With `refusal`, the monitor
    prints it for every command, as it does for one it cannot carry out.
    """

    def __init__(self, board, refusal: bytes = b"") -> None:
        self.board = board
        self.refusal = refusal
        # Called with this object and a console command someone typed.
        self.on_typed = None
        self.commands: list[str] = []
        self.started = threading.Event()
        self.link_up = threading.Event()
        self._console = socket.create_server(("127.0.0.1", 0))
        self._monitor = socket.create_server(("127.0.0.1", 0))
        self.serial = self._console.getsockname()
        self.monitor = self._monitor.getsockname()
        self._threads = [
            threading.Thread(target=self._run_monitor, daemon=True),
            threading.Thread(target=self._run_console, daemon=True),
        ]
        for thread in self._threads:
            thread.start()

    def _run_monitor(self) -> None:
        conn, _ = self._monitor.accept()
        with conn:
            conn.sendall(b"QEMU 9.2.2 monitor - type 'help' for more information\r\n(qemu) ")
            pending = b""
            while True:
                data = conn.recv(1024)
                if not data:
                    return
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    command = line.decode()
                    self.commands.append(command)
                    # The echo, redrawn a character at a time as QEMU's is.
                    echo = b"".join(
                        command[: i + 1].encode() + b"\x1b[K" for i in range(len(command))
                    )
                    conn.sendall(echo + b"\r\n" + self.refusal + b"(qemu) ")
                    if self.refusal:
                        continue
                    if command == "cont":
                        self.started.set()
                    if command == "set_link n0 on":
                        self.link_up.set()

    def _run_console(self) -> None:
        conn, _ = self._console.accept()
        self.conn = conn
        self._pending = b""
        with conn:
            if self.started.wait(10):
                with contextlib.suppress(OSError):
                    self.board(self)

    def send(self, data: bytes) -> None:
        self.conn.sendall(data)

    def typed_lines(self) -> list[str]:
        """Console commands typed since the last call: whole lines of text
        before any packet, as a terminal's Enter key ends them."""
        lines = []
        while True:
            start = self._pending.find(improv.HEADER)
            text = self._pending if start == -1 else self._pending[:start]
            end = min((at for at in (text.find(b"\r"), text.find(b"\n")) if at != -1), default=-1)
            if end == -1:
                return lines
            lines.append(text[:end].decode())
            self._pending = self._pending[end + 1 :]

    def request(self) -> tuple[int, bytes]:
        """The next RPC the client sent: its command and payload. A console
        command typed in between is answered by `on_typed`, if it is set."""
        while True:
            for line in self.typed_lines():
                if self.on_typed is not None:
                    self.on_typed(self, line)
            start = self._pending.find(improv.HEADER)
            if start != -1 and len(self._pending) > start + improv.LENGTH_AT:
                end = start + improv.OVERHEAD + self._pending[start + improv.LENGTH_AT]
                if len(self._pending) >= end:
                    raw = self._pending[start:end]
                    self._pending = self._pending[end:]
                    assert raw[-1] == sum(raw[:-1]) % 256, raw
                    data = raw[improv.LENGTH_AT + 1 : -1]
                    return data[0], data[2:]
            data = self.conn.recv(1024)
            if not data:
                raise OSError("client gone")
            self._pending += data

    def close(self) -> None:
        self._console.close()
        self._monitor.close()


def crlf(data: bytes) -> bytes:
    """What a console with ESP-IDF's default output line endings sends for
    `data`: CR before every LF, a packet's own bytes included."""
    return data.replace(b"\n", b"\r\n")


class FakeControl:
    """The board's REST surface, as far as this needs one: PUT /name changes
    the name device_info answers with."""

    def __init__(self, name: str) -> None:
        self.name = name
        control = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_PUT(self) -> None:  # http.server's own spelling
                size = int(self.headers.get("Content-Length", 0))
                control.name = self.rfile.read(size).decode()
                self.send_response(200)
                self.send_header("Content-Length", "3")
                self.end_headers()
                self.wfile.write(b"ok\n")

            def log_message(self, *args) -> None:
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()


def late_board(
    qemu: FakeQemu,
    abort_on_join: bool = False,
    control: FakeControl | None = None,
    rewrite: bool = False,
) -> None:
    """hearth_sink's openeth image with its link down, as #741 left it, or as it
    was before (`abort_on_join`). With `rewrite`, its console converts line
    endings as ESP-IDF's defaults do, which is what corrupts its answers."""
    send = (lambda data: qemu.send(crlf(data))) if rewrite else qemu.send

    def answer_typed(board: FakeQemu, line: str) -> None:
        if line == "sendspin":
            board.send(b"sendspin: server '' (, , ), role , 0 connection(s), playing idle\n")

    qemu.on_typed = answer_typed
    qemu.send(BOOT + b"error: openeth got no address in 30 s\r\nmdns: no network\r\n")
    qemu.send(STATE_READY + b"improv: listening on the console for Wi-Fi credentials\r\n")
    provisioned = False
    while True:
        command, payload = qemu.request()
        if command == improv.GET_CURRENT_STATE and not provisioned:
            qemu.send(ERROR_NONE + STATE_READY)
        elif command == improv.GET_CURRENT_STATE:
            qemu.send(ERROR_NONE + STATE_PROVISIONED + result(2, "http://10.0.2.15/"))
        elif command == improv.GET_DEVICE_INFO:
            name = control.name if control is not None else "hearth-000000"
            send(ERROR_NONE + result(3, "AC3Forge Hearth sink", "1", "ESP32-S3", name))
        elif command == improv.WIFI_SETTINGS:
            # What the check sends, with the 13-byte SSID whose length byte is
            # CR. A console that converted it would not get this far: the
            # board's own reader would drop the packet.
            assert payload == b"\x0dqemu-net-1234\x0fqemu-passphrase", payload
            qemu.send(STATE_PROVISIONING)
            if abort_on_join:
                qemu.send(
                    b"ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE) at 0x42\r\n"
                    b"file: \"main/net/openeth/network.cpp\" line 53\r\n\r\n"
                    b"abort() was called at PC 0x40 on core 0\r\n\r\nBacktrace: 0x1 0x2\r\n\r\n"
                    b"Rebooting...\r\n" + BOOT + b"network: openeth 10.0.2.15 via 10.0.2.2\r\n"
                    + STATE_PROVISIONED
                )
                continue
            assert qemu.link_up.wait(10)
            # app_main's loop may print before Improv's task answers.
            qemu.send(b"network: openeth 10.0.2.15 via 10.0.2.2\r\n")
            qemu.send(b"mdns: hearth-000000.local, _sendspin._tcp on 8928, path /sendspin\r\n")
            qemu.send(ERROR_NONE + RESULT_URL + STATE_PROVISIONED)
            qemu.send(b"sendspin: pairing token SP:0ABC\r\nconsole: listening for commands\r\n")
            provisioned = True


def unprovisioned_board(qemu: FakeQemu, name: str = "hearth-000000") -> None:
    qemu.send(BOOT + b"error: no network stored and none built in; provision the board first\r\n")
    qemu.send(b"control: http on port 80 - a web page at /, the REST routes listed at /api\r\n")
    qemu.send(b"improv: listening on the console for Wi-Fi credentials\r\n" + STATE_READY)
    while True:
        command, _ = qemu.request()
        if command == improv.GET_CURRENT_STATE:
            qemu.send(ERROR_NONE + STATE_READY)
        elif command == improv.GET_DEVICE_INFO:
            qemu.send(ERROR_NONE + result(3, "AC3Forge Hearth sink", "0.10.0", "ESP32-S3", name))


class Scenarios(unittest.TestCase):
    def setUp(self):
        self.patches = {"LINK_DELAY_SECONDS": 0, "AFTER_STOP_SECONDS": 0, "JOIN_SECONDS": 10}
        self.saved = {key: getattr(improv, key) for key in self.patches}
        for key, value in self.patches.items():
            setattr(improv, key, value)
        self.dir = tempfile.TemporaryDirectory()
        self.capture = Path(self.dir.name) / "console.txt"
        self.ready = Path(self.dir.name) / "ready"

    def tearDown(self):
        for key, value in self.saved.items():
            setattr(improv, key, value)
        self.dir.cleanup()

    def run_scenario(
        self, scenario: str, board, refusal: bytes = b"", control: str | None = None
    ) -> tuple[int, str]:
        qemu = FakeQemu(board, refusal)
        arguments = [
            scenario,
            "--serial",
            f"{qemu.serial[0]}:{qemu.serial[1]}",
            "--monitor",
            f"{qemu.monitor[0]}:{qemu.monitor[1]}",
            "--capture",
            str(self.capture),
            "--ready",
            str(self.ready),
            "--hold",
            "0",
        ]
        if control is not None:
            arguments += ["--control", control]
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                status = improv.main(arguments)
        finally:
            qemu.close()
        self.commands = qemu.commands
        return status, out.getvalue()

    def test_late_network_passes_as_the_board_now_answers(self):
        status, out = self.run_scenario("late-network", late_board)
        self.assertEqual(status, 0, out)
        self.assertTrue(self.ready.exists())
        self.assertEqual(self.commands, ["set_link n0 off", "cont", "set_link n0 on"])
        capture = self.capture.read_text(encoding="utf-8").splitlines()
        self.assertIn("[improv] rpc_result wifi_settings 'http://10.0.2.15/'", capture)
        self.assertIn("sendspin: pairing token SP:0ABC", capture)

    def test_late_network_holds_the_name_the_board_answers_with(self):
        control = FakeControl("hearth-000000")
        self.addCleanup(control.close)
        status, out = self.run_scenario(
            "late-network", lambda qemu: late_board(qemu, control=control), control=control.url
        )
        self.assertEqual(status, 0, out)
        self.assertEqual(control.name, improv.NEW_NAME)
        self.assertIn(f"device_info: AC3Forge Hearth sink, 1, ESP32-S3, {improv.NEW_NAME}", out)

    def test_late_network_fails_when_the_console_rewrites_the_answer(self):
        # The bug this covers: with ESP-IDF's default line endings the board
        # sends CR before the LF that is the name's length byte, and the packet
        # a client reads is broken.
        control = FakeControl("hearth-000000")
        self.addCleanup(control.close)
        status, out = self.run_scenario(
            "late-network",
            lambda qemu: late_board(qemu, control=control, rewrite=True),
            control=control.url,
        )
        self.assertEqual(status, 1, out)
        self.assertIn("device_info", out)
        self.assertIn("bad packet", out)

    def test_late_network_fails_when_the_board_keeps_its_old_name(self):
        control = FakeControl("hearth-000000")
        self.addCleanup(control.close)
        status, out = self.run_scenario(
            "late-network",
            lambda qemu: late_board(qemu, control=FakeControl("hearth-000000")),
            control=control.url,
        )
        self.assertEqual(status, 1, out)
        self.assertIn(f"and the board is named {improv.NEW_NAME}", out)

    def test_late_network_fails_at_the_abort_the_board_had_before_741(self):
        status, out = self.run_scenario(
            "late-network", lambda qemu: late_board(qemu, abort_on_join=True)
        )
        self.assertEqual(status, 1, out)
        self.assertFalse(self.ready.exists())
        self.assertIn(
            "::error title=hearth_sink Improv::late-network: wifi_settings once the link is up, "
            "waiting for error_state none: the part stopped first: ESP_ERROR_CHECK failed: "
            "esp_err_t 0x103 (ESP_ERR_INVALID_STATE)",
            out,
        )

    def test_late_network_fails_when_the_board_cannot_connect(self):
        def board(qemu):
            qemu.send(BOOT + b"error: openeth got no address in 30 s\r\n" + STATE_READY)
            qemu.send(b"improv: listening on the console for Wi-Fi credentials\r\n")
            while True:
                command, _ = qemu.request()
                if command == improv.WIFI_SETTINGS:
                    qemu.send(STATE_PROVISIONING)
                    qemu.link_up.wait(10)
                    qemu.send(b"IMPROV\x01\x02\x01\x03\xe4" + STATE_READY)
                else:
                    qemu.send(ERROR_NONE + STATE_READY)

        status, out = self.run_scenario("late-network", board)
        self.assertEqual(status, 1, out)
        self.assertIn("error_state cannot_connect where error_state none was due", out)

    def test_unprovisioned_passes_as_the_board_now_answers(self):
        status, out = self.run_scenario("unprovisioned", unprovisioned_board)
        self.assertEqual(status, 0, out)
        self.assertEqual(self.commands, ["cont"])
        self.assertIn("device_info: AC3Forge Hearth sink, 0.10.0, ESP32-S3, hearth-000000", out)

    def test_unprovisioned_fails_at_a_boot_loop(self):
        def board(qemu):
            for _ in range(2):
                qemu.send(BOOT + b"assert failed: tcpip_send_msg_wait_sem tcpip.c:454 ")
                qemu.send(b"(Invalid mbox)\r\nBacktrace: 0x1 0x2\r\n\r\nRebooting...\r\n")

        status, out = self.run_scenario("unprovisioned", board)
        self.assertEqual(status, 1, out)
        self.assertIn(
            "unprovisioned: waiting for a console line with 'control: http on port 80': "
            "the part stopped first: assert failed: tcpip_send_msg_wait_sem",
            out,
        )

    def test_unprovisioned_fails_on_the_wrong_name(self):
        status, out = self.run_scenario(
            "unprovisioned", lambda qemu: unprovisioned_board(qemu, name="kitchen")
        )
        self.assertEqual(status, 1, out)
        self.assertIn("device_info answered", out)

    def test_a_command_the_monitor_refuses_fails_the_run(self):
        # What QEMU says when its network has no id n0, as a wrong -nic gives.
        status, out = self.run_scenario(
            "late-network", late_board, refusal=b"Error: Device 'n0' not found\r\n"
        )
        self.assertEqual(status, 1, out)
        self.assertEqual(self.commands, ["set_link n0 off"])
        self.assertIn(
            "QEMU's monitor refused 'set_link n0 off': Error: Device 'n0' not found", out
        )


if __name__ == "__main__":
    unittest.main()
