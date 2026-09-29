"""Local-only generator for the committed transient pre-noise fixture.

`tests/golden/external-baseline/eac3-transient-stereo-128/` holds the first five
seconds of one leg of the Dolby Encoding Engine golden-master set (the `dee-gold`
set, D:/ac3bld/dee-gold by default, made by PR #1054's generator): `dee.ec3`, the
leg's first 156 syncframes, and `source.wav`, the first five seconds of the WAV
DEE was given, in 16-bit PCM. `leg.json` beside them records where both came
from.

Why this leg. DEE turns on transient pre-noise processing (A/52 Annex E §3.7) for
this material at the lower stereo and 5.1 rates, and places its transients past
the end of the frame that signals them - up to 1,260 samples into the next frame
- which the decoder refused outright until the correction learned to wait for
the frame its transient falls in. Every stereo leg from 96 to 144 kbit/s signals
the same 18 corrections, and of them 128 kbit/s decodes closest to the source,
which is what tools/checks/verify_gold_reference.sh and the decoder test score
it against. The first five seconds carry nine of those corrections: both
lengths DEE uses (0 and 255), and eight whose transient lies in the frame after
the one that signals it against one whose transient does not. The last of them
lies past the final syncframe, where the decoder's flush() applies it.

The source is a burst of decaying noise every second on each channel, the right
channel half a second after the left, after a second of silence. Silence before
each burst is what makes the correction visible: the pre-noise it removes stands
out against zero.

Nothing here runs DEE; the set it cuts from is DEE's output, which is licensed
and made locally, so this is local-only too, and refuses to run under
GITHUB_ACTIONS like gen_external_baseline.py.

Usage (repo root):  python tools/generators/gen_dee_tpn_fixture.py [--gold DIR]
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "tests" / "golden" / "external-baseline" / "eac3-transient-stereo-128"
LEG = "ddp-20-transient-128"
SOURCE = "transient_20.wav"
SYNCFRAMES = 156  # 156 x 1536 samples, 4.99 s
SOURCE_SAMPLES = 240000  # 5.00 s at 48 kHz


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def first_syncframes(stream: bytes, count: int) -> bytes:
    """The first `count` E-AC-3 syncframes, by the frmsiz each one states."""
    offset = 0
    for _ in range(count):
        if stream[offset:offset + 2] != b"\x0b\x77":
            raise SystemExit(f"no sync word at byte {offset}")
        frmsiz = ((stream[offset + 2] & 0x07) << 8) | stream[offset + 3]
        offset += (frmsiz + 1) * 2
    return stream[:offset]


def source_excerpt(path: Path) -> bytes:
    """The WAV's first SOURCE_SAMPLES frames as 16-bit PCM, rounded."""
    with wave.open(str(path), "rb") as r:
        channels, width, rate = r.getnchannels(), r.getsampwidth(), r.getframerate()
        if rate != 48000 or width not in (3, 4):
            raise SystemExit(f"{path}: expected 24- or 32-bit PCM at 48 kHz")
        raw = r.readframes(SOURCE_SAMPLES)
    samples = []
    for i in range(0, len(raw), width):
        word = raw[i:i + width]
        value = int.from_bytes(word, "little", signed=True)
        shift = 8 * width - 16
        # Round half away from zero onto 16 bits, then clamp.
        half = 1 << (shift - 1)
        value = (value + half) >> shift if value >= 0 else -((-value + half) >> shift)
        samples.append(max(-32768, min(32767, value)))
    header_free = struct.pack(f"<{len(samples)}h", *samples)
    out = bytearray()
    out += b"RIFF" + struct.pack("<I", 36 + len(header_free)) + b"WAVE"
    out += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, 48000, 48000 * channels * 2,
                                 channels * 2, 16)
    out += b"data" + struct.pack("<I", len(header_free)) + header_free
    return bytes(out)


def main() -> int:
    if os.environ.get("GITHUB_ACTIONS"):
        print("gen_dee_tpn_fixture.py is local-only: it reads a set made with licensed software")
        return 1
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gold", type=Path, default=Path("D:/ac3bld/dee-gold"))
    args = parser.parse_args()

    manifest = json.loads((args.gold / "dee-gold-manifest.json").read_text())
    leg = manifest["legs"][LEG]
    stream = (args.gold / "streams" / LEG / leg["stream"]).read_bytes()
    if sha256(stream) != leg["sha256"]:
        raise SystemExit(f"{LEG}/{leg['stream']} does not match the set's manifest")
    source_path = args.gold / "sources" / SOURCE
    source = source_path.read_bytes()
    if sha256(source) != manifest["sources"][SOURCE.removesuffix(".wav")]["sha256"]:
        raise SystemExit(f"{SOURCE} does not match the set's manifest")

    cut = first_syncframes(stream, SYNCFRAMES)
    excerpt = source_excerpt(source_path)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "dee.ec3").write_bytes(cut)
    (OUT / "source.wav").write_bytes(excerpt)
    record = {
        "made_by": "tools/generators/gen_dee_tpn_fixture.py",
        "from_set": {
            "gold_version": manifest["gold_version"],
            "made": manifest["made"],
            "leg": LEG,
            "dee_version": manifest["dee_version"],
            "command": leg["command"],
            "options": leg["options"],
            "stream_sha256": leg["sha256"],
            "source": SOURCE,
            "source_sha256": manifest["sources"][SOURCE.removesuffix(".wav")]["sha256"],
            "source_description": manifest["sources"][SOURCE.removesuffix(".wav")]["description"],
        },
        "dee.ec3": {
            "syncframes": SYNCFRAMES,
            "samples": SYNCFRAMES * 1536,
            "bytes": len(cut),
            "sha256": sha256(cut),
        },
        "source.wav": {
            "samples": SOURCE_SAMPLES,
            "bits": 16,
            "note": "the source's first samples, rounded to 16-bit PCM",
            "sha256": sha256(excerpt),
        },
    }
    (OUT / "leg.json").write_text(json.dumps(record, indent=2) + "\n", newline="\n")
    print(f"wrote {OUT}: {len(cut)} bytes of stream, {len(excerpt)} bytes of source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
