#!/usr/bin/env python3
"""The ESP32 player's stream set: streams its HTTP source can be pointed at,
and streams.json, which says what each one is and what a 7.1.4 output should
receive from it.

    python tools/generators/gen_device_streams.py --ac3cli <path to ac3cli>

writes esp-idf/ac3forge/examples/stream_player/www/. The set covers the
output layouts the player renders onto, both codecs, dependent substreams,
two programmes in one stream, dual mono, the Annex E coding tools, short
frames, VBR, DRC metadata, other encoders' streams and object audio. Most of
it is made here, by this repository's encoder, from signals this script
synthesises; the rest is copied from streams already in the tree.

The levels in streams.json are the host's: each stream decoded by ac3cli as
coded - no dialnorm normalisation and no DRC, which is the player's
CONFIG_AC3FORGE_EXAMPLE_DRC_MODE=2 - with each decoded channel's RMS x 1e6
placed on the slot of the same location in the 7.1.4 layout, in the
layout's slot order. A stream with objects is decoded bed-only, because the
CI shape that checks these plays objects as their bed. A slot whose level is
0 must come out exactly silent: nothing in the stream is at that location,
and the player does not upmix.
"""

import argparse
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

REPO = pathlib.Path(__file__).resolve().parents[2]
OUT = REPO / "esp-idf/ac3forge/examples/stream_player/www"
RATE = 48000

# The output layout the levels are for: "7.1.4" in the player's slot order
# (esp-idf/ac3forge/include/ac3forge/layout.hpp - ring, heights, LFE).
LAYOUT_714 = ["L", "C", "R", "Ls", "Rs", "Lrs", "Rrs", "Vhl", "Vhr", "Lts", "Rts", "LFE"]

# One tone per speaker, a third of an octave apart, so that each slot can be
# told from every other by ear or by an FFT. The LFE's is inside the LFE band.
TONE = {
    "L": 250,
    "C": 315,
    "R": 400,
    "Ls": 500,
    "Rs": 630,
    "Lrs": 800,
    "Rrs": 1000,
    "Vhl": 1250,
    "Vhr": 1600,
    "Lts": 2000,
    "Rts": 2500,
    "LFE": 63,
}


def mapping(names):
    """map= for a source whose channel k is names[k]."""
    return "map=" + ",".join(f"0.{k}:{n}" for k, n in enumerate(names))


MAP_714 = mapping(LAYOUT_714)
MAP_51 = mapping(["L", "C", "R", "Ls", "Rs", "LFE"])
MAP_20 = mapping(["L", "R"])


# --- the signals ---------------------------------------------------------------


def write_wav(path, x, rate=RATE):
    """Plain float32 WAV (format tag 3), which is what ac3cli's reader takes."""
    x = np.asarray(x, dtype="<f4")
    _, channels = x.shape
    data = x.tobytes()
    fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
    body = (
        b"WAVE"
        + b"fmt "
        + struct.pack("<I", len(fmt))
        + fmt
        + b"data"
        + struct.pack("<I", len(data))
        + data
    )
    pathlib.Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def tone(freq, seconds, amp, rate=RATE):
    t = np.arange(round(seconds * rate)) / rate
    return amp * np.sin(2 * np.pi * freq * t)


def faded(x, ms=20):
    n = int(RATE * ms / 1000)
    ramp = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))
    x = x.copy()
    x[:n] *= ramp
    x[-n:] *= ramp[::-1]
    return x


def make_sources(work):
    rng = np.random.default_rng(20260911)
    src = {}

    # Every speaker at once, each its own tone.
    src["tones714"] = work / "tones714.wav"
    write_wav(src["tones714"], np.stack([tone(TONE[n], 2.0, 0.25) for n in LAYOUT_714], axis=1))

    # One speaker at a time, in slot order.
    step = 0.4
    walk = np.zeros((round(step * RATE) * len(LAYOUT_714), len(LAYOUT_714)))
    for k, n in enumerate(LAYOUT_714):
        seg = faded(tone(TONE[n], step, 0.25))
        walk[k * len(seg) : (k + 1) * len(seg), k] = seg
    src["walk714"] = work / "walk714.wav"
    write_wav(src["walk714"], walk)

    # For the coding tools: each speaker's tone, a second tone above where
    # coupling and spectral extension start, a little noise across the band,
    # and a click every quarter second so that block switching has transients
    # to switch on.
    seconds = 0.5
    clicks = np.zeros(round(seconds * RATE))
    clicks[:: RATE // 4] = 0.5
    cols = []
    for k, n in enumerate(LAYOUT_714):
        x = tone(TONE[n], seconds, 0.2)
        if n != "LFE":
            x = x + tone(5000 + 600 * k, seconds, 0.08) + rng.normal(0, 0.01, x.shape) + clicks
        cols.append(x)
    by = dict(zip(LAYOUT_714, cols, strict=True))
    src["tools714"] = work / "tools714.wav"
    write_wav(src["tools714"], np.stack(cols, axis=1))
    src["tools51"] = work / "tools51.wav"
    write_wav(src["tools51"], np.stack([by[n] for n in ["L", "C", "R", "Ls", "Rs", "LFE"]], axis=1))
    src["tools20"] = work / "tools20.wav"
    write_wav(src["tools20"], np.stack([by["L"], by["R"]], axis=1))
    # The same speakers at 44.1 kHz, for the stream the player refuses.
    src["tones51_44k"] = work / "tones51_44k.wav"
    write_wav(
        src["tones51_44k"],
        np.stack(
            [tone(TONE[n], 0.5, 0.2, 44100) for n in ["L", "C", "R", "Ls", "Rs", "LFE"]], axis=1
        ),
        44100,
    )
    src["mono1"] = work / "mono1.wav"
    write_wav(src["mono1"], tone(TONE["C"], 1.0, 0.25)[:, None])
    src["mono2"] = work / "mono2.wav"
    write_wav(src["mono2"], tone(TONE["Rs"], 1.0, 0.25)[:, None])
    return src


# --- the set ---------------------------------------------------------------------
#
# Each entry: the file, one line on what it is, and either the ac3cli command
# that makes it ({name} is a source above, {out} the output) or the file in
# the tree it is a copy of. "psram" marks a stream measured to need more
# internal RAM than the http shape has without PSRAM (planning/esp32-device-ui.md,
# "The stream set"): CI's shape plays everything else.

SET = [
    # 7.1.4, for listening and for a level on every slot.
    {
        "file": "714-walk.ec3",
        "what": "7.1.4, one speaker at a time in slot order, 0.4 s each",
        "make": ["eac3-encode", "{walk714}", "{out}", "192", "cpl+spx", "714", MAP_714],
    },
    {
        "file": "714-tones.ec3",
        "what": "7.1.4, every speaker at once, each its own tone",
        "make": ["eac3-encode", "{tones714}", "{out}", "192", "cpl+spx", "714", MAP_714],
    },
    {
        "file": "objects-mdct.ec3",
        "what": "four objects orbiting at different heights over a 5.1 bed, "
        "JOC in the MDCT-band domain",
        "make": ["atmos", "{out}", "4", "384", "4", "3", "objects", "joc-domain=mdct"],
    },
    {
        "file": "demo.ec3",
        "what": "the WASM page's demo: 5.1 with objects, JOC in the QMF domain",
        "copy": "apps/wasm/assets/demo.ec3",
    },
    {
        "file": "514-joc-dee.ec3",
        "what": "Dolby Encoding Engine: 5.1.4 carried as objects, JOC in the QMF domain",
        "copy": "tests/golden/object-fixture/dee_joc_514.ec3",
    },
    {
        "file": "height.ec3",
        "what": "the footprint probe's height fixture: five objects, three on the ceiling, "
        "MDCT-band domain",
        "copy": "esp-idf/ac3forge/examples/stream_player/stream/height.ec3",
    },
    # Every coded layout the encoder names, a tone per speaker.
    {
        "file": "layout-10.ec3",
        "what": "E-AC-3 1/0",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-mono.ec3",
    },
    {
        "file": "layout-20.ec3",
        "what": "E-AC-3 2/0",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-stereo.ec3",
    },
    {
        "file": "layout-51.ec3",
        "what": "E-AC-3 5.1",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-51.ec3",
    },
    {
        "file": "layout-71.ec3",
        "what": "E-AC-3 7.1: 5.1 and a dependent substream",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-71.ec3",
    },
    {
        "file": "layout-512.ec3",
        "what": "E-AC-3 5.1.2: 5.1 and a dependent substream",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-512.ec3",
    },
    {
        "file": "layout-514.ec3",
        "what": "E-AC-3 5.1.4: 5.1 and a dependent substream",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-514.ec3",
    },
    {
        "file": "layout-714.ec3",
        "what": "E-AC-3 7.1.4: 5.1 and two dependent substreams",
        "copy": "fuzz/seeds/fuzz_eac3_decode/eac3-sine-714.ec3",
    },
    # The Annex E coding tools, at 7.1.4 so every substream carries them.
    {
        "file": "714-none.ec3",
        "what": "7.1.4, no coding tools",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "none", "714", MAP_714],
    },
    {
        "file": "714-cpl.ec3",
        "what": "7.1.4, coupling",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "cpl", "714", MAP_714],
    },
    {
        "file": "714-ecpl.ec3",
        "what": "7.1.4, enhanced coupling",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "cpl+ecpl", "714", MAP_714],
        "psram": True,
    },
    {
        "file": "714-spx.ec3",
        "what": "7.1.4, spectral extension",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "spx", "714", MAP_714],
    },
    {
        "file": "714-aht.ec3",
        "what": "7.1.4, adaptive hybrid transform",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "aht", "714", MAP_714],
        "psram": True,
    },
    {
        "file": "714-tpn.ec3",
        "what": "7.1.4, transient pre-noise processing",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "tpn", "714", MAP_714],
        "psram": True,
    },
    {
        "file": "714-all.ec3",
        "what": "7.1.4, coupling, spectral extension and AHT together",
        "make": ["eac3-encode", "{tools714}", "{out}", "256", "all", "714", MAP_714],
        "psram": True,
    },
    {
        "file": "714-blocks2.ec3",
        "what": "7.1.4, two-block syncframes",
        "make": ["eac3-encode", "{tools714}", "{out}", "384", "cpl+numblkscod:1", "714", MAP_714],
    },
    {
        "file": "714-blocks3.ec3",
        "what": "7.1.4, three-block syncframes",
        "make": ["eac3-encode", "{tools714}", "{out}", "384", "cpl+numblkscod:2", "714", MAP_714],
    },
    # The three that 7.1.4 cannot carry without PSRAM, at 5.1, which can.
    {
        "file": "51-aht.ec3",
        "what": "5.1, adaptive hybrid transform",
        "make": ["eac3-encode", "{tools51}", "{out}", "384", "aht", "51", "off", MAP_51],
    },
    {
        "file": "51-ecpl.ec3",
        "what": "5.1, enhanced coupling",
        "make": ["eac3-encode", "{tools51}", "{out}", "384", "cpl+ecpl", "51", "off", MAP_51],
    },
    {
        "file": "51-tpn.ec3",
        "what": "5.1, transient pre-noise processing",
        "make": ["eac3-encode", "{tools51}", "{out}", "384", "tpn", "51", "off", MAP_51],
    },
    {
        "file": "51-blocks1.ec3",
        "what": "5.1, one-block syncframes",
        "make": ["eac3-encode", "{tools51}", "{out}", "448", "cpl+numblkscod:0", "51", MAP_51],
    },
    {
        "file": "51-vbr.ec3",
        "what": "5.1, variable bit rate",
        "make": [
            "eac3-encode",
            "{tools51}",
            "{out}",
            "384",
            "cpl+spx",
            "51",
            "q:0.4,max:640",
            MAP_51,
        ],
    },
    {
        "file": "51-drc.ec3",
        "what": "5.1 with DRC words (film standard) and dialnorm -24",
        "make": [
            "eac3-encode",
            "{tools51}",
            "{out}",
            "384",
            "cpl+spx",
            "51",
            "off",
            "drc=film-standard",
            "dialnorm=24",
            MAP_51,
        ],
    },
    # Stream types.
    {
        "file": "ac3-51.ac3",
        "what": "AC-3 5.1",
        "make": ["encode", "{tools51}", "{out}", "448", "51", MAP_51],
    },
    {
        "file": "ac3-51-cpl.ac3",
        "what": "AC-3 5.1 with coupling, a tone per speaker",
        "make": ["sine", "{out}", "1", "448", "1000", "50", "51c"],
    },
    {
        "file": "ac3-51-44k.ac3",
        "what": "AC-3 5.1 at 44.1 kHz, which the player refuses: its sink runs at 48 kHz",
        "make": ["encode", "{tones51_44k}", "{out}", "448", "51", MAP_51],
        "refused": "sample rate",
    },
    {
        "file": "ac3-20.ac3",
        "what": "AC-3 2/0",
        "make": ["encode", "{tools20}", "{out}", "192", "stereo", MAP_20],
    },
    {
        "file": "eac3-dualmono.ec3",
        "what": "E-AC-3 1+1: two mono programmes in one substream",
        "make": ["eac3-encode", "{mono1}", "{out}", "192", "none", "1+1", "off", "{mono2}"],
    },
    {
        "file": "eac3-programmes.ec3",
        "what": "E-AC-3 with two programmes: 5.1, and a mono second programme",
        "make": [
            "eac3-encode",
            "{tools51}",
            "{out}",
            "384",
            "none",
            "51",
            "off",
            "programme2={mono1}",
            "programme2-layout=mono",
            "programme2-bitrate=96",
        ],
    },
    # Other encoders.
    {
        "file": "dee-eac3-51.ec3",
        "what": "Dolby Encoding Engine, E-AC-3 5.1 at 256 kbit/s",
        "copy": "tests/golden/external-baseline/eac3-51-256/dee.ec3",
        "psram": True,
    },
    {
        "file": "ffmpeg-eac3-51.ec3",
        "what": "FFmpeg, E-AC-3 5.1 at 256 kbit/s",
        "copy": "tests/golden/external-baseline/eac3-51-256/ffmpeg.ec3",
    },
    {
        "file": "dee-ac3-51.ac3",
        "what": "Dolby Encoding Engine, AC-3 5.1 at 448 kbit/s",
        "copy": "tests/golden/external-baseline/ac3-51-448/dee.ac3",
    },
    {
        "file": "dee-eac3-music.ec3",
        "what": "Dolby Encoding Engine, E-AC-3 2/0 music at 96 kbit/s",
        "copy": "tests/golden/external-baseline/eac3-music-stereo-96/dee.ec3",
    },
]


# --- what each stream is, and what 7.1.4 should get from it ------------------------


def read_wav(path):
    data = pathlib.Path(path).read_bytes()
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8 : pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, _, _, _, bits = struct.unpack_from("<HHIIHH", body, 0)
            if tag == 0xFFFE:
                tag = struct.unpack_from("<H", body, 24)[0]
            fmt = (tag, channels, bits)
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    tag, channels, bits = fmt
    if not (tag == 3 and bits == 32):
        raise SystemExit(f"{path}: expected float32 WAV, got format {tag} at {bits} bits")
    return np.frombuffer(samples, dtype="<f4").astype(np.float64).reshape(-1, channels)


# ac3cli writes its WAV in WAV channel order and names it on the summary line
# for E-AC-3 ("12 channels, 48000 Hz: L R C LFE Lrs Rrs Ls Rs Vhl Vhr Lts
# Rts"). Its AC-3 summary names no order; AC-3's is the same WAV order over
# Table 5.8's channels. Dual mono's two programmes are Ch1 and Ch2, which the
# player places as L and R (Table E2.5 has no layout for 1+1, and the
# player's peek gives it L and R).
WAV_ORDER = ["L", "R", "C", "LFE", "Lrs", "Rrs", "Ls", "Rs"]
AS_PLACED = {"Ch1": "L", "Ch2": "R"}


def decode_names(cli, stream, wav, extra, probe):
    run = subprocess.run(
        [cli, "decode", str(stream), str(wav), *extra], capture_output=True, text=True, check=False
    )
    if run.returncode != 0:
        raise SystemExit(
            f"{stream.name}: ac3cli decode exited {run.returncode}: {run.stderr.strip()[-400:]}"
        )
    said = run.stdout + run.stderr
    line = re.search(r"\d+ channels, \d+ Hz: ([A-Za-z0-9 ]+?)\s*(?:\(|$)", said, re.M)
    if line:
        return line.group(1).split()
    coded = probe["stream"]["layout"]
    return [n for n in WAV_ORDER if n in coded] + [n for n in coded if n not in WAV_ORDER]


def describe(cli, stream, work):
    probe = json.loads(
        subprocess.run(
            [cli, "probe", str(stream), "json=1"], capture_output=True, text=True, check=True
        ).stdout
    )
    s = probe["stream"]
    joc = bool(s["objects"].get("joc"))
    wav = work / (stream.stem + ".wav")
    names = decode_names(cli, stream, wav, ["bed-only"] if joc else [], probe)
    x = read_wav(wav)
    if x.shape[1] != len(names):
        raise SystemExit(f"{stream.name}: {x.shape[1]} channels decoded, {len(names)} named")
    placed = [AS_PLACED.get(n, n) for n in names]
    rms = {n: round(float(np.sqrt(np.mean(x[:, c] ** 2))) * 1e6) for c, n in enumerate(placed)}
    exact = all(n in LAYOUT_714 for n in placed)
    t = s["tools"]
    tools = [
        name
        for key, name in [
            ("coupling", "cpl"),
            ("enhanced_coupling", "ecpl"),
            ("spectral_extension", "spx"),
            ("aht_syncframes", "aht"),
            ("transient_prenoise_syncframes", "tpn"),
            ("block_switch", "blksw"),
            ("rematrixing", "remat"),
        ]
        if t.get(key)
    ]
    independents = {
        sub["substream_id"] for sub in s["substreams"] if sub["stream_type"] == "independent"
    }
    return {
        "codec": "E-AC-3" if s["codec"] == "eac3" else "AC-3",
        "channels": s["layout"] if s["layout"] else names,
        "substreams": s["substreams_per_access_unit"],
        "programmes": len(independents) if s["codec"] == "eac3" else 1,
        "objects": (s["objects"].get("object_count") or True) if joc else False,
        "blocks": s["blocks_per_syncframe"],
        "tools": tools,
        "kbps": round(s["bitrate_kbps"]),
        "vbr": s["variable_bitrate"],
        "seconds": round(s["duration_seconds"], 3),
        # What a player of the first programme decodes: the host decode's
        # own length, since a second programme's units are in the file too.
        "units": x.shape[0] // (256 * s["blocks_per_syncframe"]),
        "bytes": s["bytes"],
        # None where a channel has no slot of its own on 7.1.4 and would be
        # panned: the level is then the renderer's, not the decoder's.
        "levels_714": [rms.get(n, 0) for n in LAYOUT_714] if exact else None,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ac3cli", required=True)
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    ap.add_argument(
        "--repo", type=pathlib.Path, default=REPO, help="the checkout the copies come from"
    )
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {"layout": "7.1.4", "slots": LAYOUT_714, "streams": []}
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        src = make_sources(work)
        for entry in SET:
            out = args.out / entry["file"]
            if "copy" in entry:
                shutil.copyfile(args.repo / entry["copy"], out)
            else:
                cmd = [args.ac3cli, *(a.format(out=out, **src) for a in entry["make"])]
                run = subprocess.run(cmd, capture_output=True, text=True, check=False)
                if run.returncode != 0:
                    raise SystemExit(
                        f"{entry['file']}: {' '.join(cmd[1:])}\n{run.stdout}{run.stderr}"
                    )
            facts = describe(args.ac3cli, out, work)
            if entry.get("refused"):
                facts.update(levels_714=None, units=None)
            manifest["streams"].append(
                {
                    "file": entry["file"],
                    "what": entry["what"],
                    "source": entry.get("copy", "made by this script"),
                    "psram": bool(entry.get("psram")),
                    "refused": entry.get("refused"),
                    **facts,
                }
            )
            tools = "+".join(facts["tools"]) or "-"
            print(
                f"{entry['file']:22} {facts['codec']:6} {len(facts['channels']):2}ch "
                f"sub={facts['substreams']} prog={facts['programmes']} obj={facts['objects']} "
                f"tools={tools} blocks={facts['blocks']} {facts['kbps']}k {facts['seconds']}s "
                f"{facts['bytes']}B"
            )
    (args.out / "streams.json").write_text(
        json.dumps(manifest, indent=1) + "\n", encoding="utf-8", newline="\n"
    )


if __name__ == "__main__":
    sys.exit(main())
