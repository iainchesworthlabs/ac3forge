"""Look at, and type into, the test guest's console before VMware Tools runs.

vmrun's captureScreen and key injection need Tools in the guest, which does
not exist during Setup and OOBE. Workstation can expose the console over VNC
(RemoteDisplay.vnc.* in the VMX, no password, loopback use only); this is
the small part of RFB 3.8 needed to grab the framebuffer as a PNG and to
press keys, with no third-party VNC client.

    python guest_console.py shot out.png          # screenshot
    python guest_console.py key space             # press one key
    python guest_console.py key Return
    python guest_console.py type "hello"          # type printable text

Options: --host (default 127.0.0.1), --port (default 5951).
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

from PIL import Image

KEYSYMS = {
    "space": 0x0020,
    "Return": 0xFF0D,
    "Enter": 0xFF0D,
    "Escape": 0xFF1B,
    "Tab": 0xFF09,
    "BackSpace": 0xFF08,
    "Delete": 0xFFFF,
    "Up": 0xFF52,
    "Down": 0xFF54,
    "Left": 0xFF51,
    "Right": 0xFF53,
    "F1": 0xFFBE,
    "F2": 0xFFBF,
    "F8": 0xFFC5,
    "F10": 0xFFC7,
    "F12": 0xFFC9,
    "Shift_L": 0xFFE1,
    "Control_L": 0xFFE3,
    "Alt_L": 0xFFE9,
    "Super_L": 0xFFEB,
}

# Shifted symbol -> the unshifted key under it, US layout.
US_SHIFTED = {
    "~": "`", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
    "(": "9", ")": "0", "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";", '"': "'",
    "<": ",", ">": ".", "?": "/",
}


class Rfb:
    def __init__(self, host: str, port: int, timeout: float = 10.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.width = 0
        self.height = 0
        self.bpp = 32
        self.handshake()

    def read(self, n: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < n:
            piece = self.sock.recv(n - len(chunks))
            if not piece:
                raise ConnectionError("VNC server closed the connection")
            chunks.extend(piece)
        return bytes(chunks)

    def handshake(self) -> None:
        version = self.read(12)
        if not version.startswith(b"RFB "):
            raise ConnectionError(f"not an RFB server: {version!r}")
        self.sock.sendall(b"RFB 003.008\n")
        count = self.read(1)[0]
        if count == 0:
            length = struct.unpack(">I", self.read(4))[0]
            raise ConnectionError("server refused: " + self.read(length).decode(errors="replace"))
        types = list(self.read(count))
        if 1 not in types:
            raise ConnectionError(f"server offers security types {types}; only None (1) is supported, "
                                  "clear RemoteDisplay.vnc.password in the VMX")
        self.sock.sendall(bytes([1]))
        result = struct.unpack(">I", self.read(4))[0]
        if result != 0:
            length = struct.unpack(">I", self.read(4))[0]
            raise ConnectionError("security failed: " + self.read(length).decode(errors="replace"))
        self.sock.sendall(bytes([1]))  # ClientInit: shared
        self.width, self.height = struct.unpack(">HH", self.read(4))
        self.read(16)  # server pixel format, replaced below
        name_len = struct.unpack(">I", self.read(4))[0]
        self.name = self.read(name_len).decode(errors="replace")
        # Ask for 32 bpp true colour, little-endian BGRX, so Raw rectangles map
        # straight onto a PIL image.
        pixel_format = struct.pack(">BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        self.sock.sendall(struct.pack(">Bxxx", 0) + pixel_format)
        self.sock.sendall(struct.pack(">BxH", 2, 1) + struct.pack(">i", 0))  # SetEncodings: Raw only

    def key(self, keysym: int, down: bool) -> None:
        self.sock.sendall(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    def press(self, keysym: int) -> None:
        self.key(keysym, True)
        time.sleep(0.05)
        self.key(keysym, False)
        time.sleep(0.05)

    # Workstation's VNC server turns a keysym into a scancode by the US
    # layout and does not add Shift on its own, so a capital or a shifted
    # symbol is sent as Shift plus the unshifted key. The guest is installed
    # with a US keyboard (autounattend.xml) for the same reason.
    def type_char(self, ch: str) -> None:
        if ch == "\n":
            self.press(KEYSYMS["Return"])
            return
        if ch == "\t":
            self.press(KEYSYMS["Tab"])
            return
        base = ch
        shifted = ch.isupper() or ch in US_SHIFTED
        if ch in US_SHIFTED:
            base = US_SHIFTED[ch]
        elif ch.isupper():
            base = ch.lower()
        if shifted:
            self.key(KEYSYMS["Shift_L"], True)
            time.sleep(0.02)
        self.press(ord(base))
        if shifted:
            self.key(KEYSYMS["Shift_L"], False)
            time.sleep(0.02)

    def screenshot(self, deadline: float = 5.0) -> Image.Image:
        image = Image.new("RGB", (self.width, self.height), (0, 0, 0))
        self.sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, self.width, self.height))
        covered = 0
        end = time.monotonic() + deadline
        while covered < self.width * self.height and time.monotonic() < end:
            kind = self.read(1)[0]
            if kind == 0:  # FramebufferUpdate
                self.read(1)
                rects = struct.unpack(">H", self.read(2))[0]
                for _ in range(rects):
                    x, y, w, h, encoding = struct.unpack(">HHHHi", self.read(12))
                    if encoding != 0:
                        raise NotImplementedError(f"encoding {encoding} not handled")
                    raw = self.read(w * h * 4)
                    tile = Image.frombytes("RGB", (w, h), raw, "raw", "BGRX")
                    image.paste(tile, (x, y))
                    covered += w * h
            elif kind == 1:  # SetColourMapEntries
                self.read(1)
                _, count = struct.unpack(">HH", self.read(4))
                self.read(6 * count)
            elif kind == 2:  # Bell
                pass
            elif kind == 3:  # ServerCutText
                self.read(3)
                length = struct.unpack(">I", self.read(4))[0]
                self.read(length)
            else:
                raise NotImplementedError(f"server message {kind} not handled")
        return image

    def close(self) -> None:
        self.sock.close()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5951)
    sub = parser.add_subparsers(dest="command", required=True)
    shot = sub.add_parser("shot", help="save the console as a PNG")
    shot.add_argument("out")
    key = sub.add_parser("key", help="press a key (space, Return, Escape, F8, a single character, or a hex keysym)")
    key.add_argument("name")
    key.add_argument("--repeat", type=int, default=1)
    key.add_argument("--interval", type=float, default=0.5)
    text = sub.add_parser("type", help="type printable text")
    text.add_argument("text")
    combo = sub.add_parser("combo", help="hold modifiers while pressing a key, e.g. Super_L+r or Control_L+Shift_L+Escape")
    combo.add_argument("keys")
    args = parser.parse_args(argv)

    rfb = Rfb(args.host, args.port)
    try:
        if args.command == "shot":
            rfb.screenshot().save(args.out)
            print(f"{rfb.width}x{rfb.height} console of '{rfb.name}' -> {args.out}")
        elif args.command == "key":
            if args.name in KEYSYMS:
                keysym = KEYSYMS[args.name]
            elif len(args.name) == 1:
                keysym = ord(args.name)
            else:
                keysym = int(args.name, 16)
            for i in range(args.repeat):
                rfb.press(keysym)
                if i + 1 < args.repeat:
                    time.sleep(args.interval)
        elif args.command == "type":
            for ch in args.text:
                rfb.type_char(ch)
        elif args.command == "combo":
            names = args.keys.split("+")
            syms = [KEYSYMS[n] if n in KEYSYMS else (ord(n) if len(n) == 1 else int(n, 16)) for n in names]
            for sym in syms[:-1]:
                rfb.key(sym, True)
            rfb.press(syms[-1])
            for sym in reversed(syms[:-1]):
                rfb.key(sym, False)
    finally:
        rfb.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
