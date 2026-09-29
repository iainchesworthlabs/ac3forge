"""Readers outside this project against ac3cli's AC-4 encoder: MediaInfo and DEE's muxer.

planning/ac4.md, the encoder's ladder, item 3, as phases E1 to E6 need it. For each configuration
below, mono to 5.1, the A-CPL modes, the experimental options of 5.X and 7.X and of A-CPL, and
phase E5's frame rates, rate modes, I-frames and metadata, `ac3cli ac4-encode` writes a raw stream
and an MP4 file, and:

  MediaInfo  its frame-by-frame trace (`--Details=1`) of the raw stream holds the values the encoder
             was configured with in every frame it details: the sync word and frame_size, the table
             of contents (bitstream_version, sequence_counter, wait_frames and br_code as the rate
             mode has them, fs_index, frame_rate_index, b_iframe_global where the I-frames fall),
             the presentation and its substream group's channel mode, the presentation substream's
             dialnorm_bits, and each frame's crc_word, computed again here. Every field of the
             substreams it shows holds the value the encoder's syntax trace (`syntax-trace=`) wrote
             for that element: the loudness values, DRC's modes, curves and gains, the downmix
             values and dialogue enhancement's configuration and parameters among them, each of
             which MediaInfo must show at least once where the configuration sends it. MediaInfo
             details the substreams of the first frame, an I-frame, and the table of contents of
             every frame (with --ParseSpeed=1; by default it stops at 128). Two of its readings
             are its own: it reads a DRC gainset no further than drc_gain_val, not decoding the
             gains' Huffman codes, so the comparison stops there; and with de_ms_proc_flag it
             reads a parameter set for each of de_nr_channels where Table 78 reads de_nr_channels
             - de_ms_proc_flag, twice the codes the encoder and the decoder read for L and R's Mid;
  DEE's muxer  dee_mp4muxer takes the raw stream and writes an MP4 file whose 'dac4' box is the one
             the encoder's MP4 file carries; for the 3/2/2 layout, but for channel group 4, which
             the muxer leaves out (src/ac4enc/ERRATA.md, "The 3/2/2 layout's top front pair").

Phase E6's presentations: the encoder's streams of several substreams and presentations under
tests/golden/ac4dec/presentations/ (encoder-*.ac4, which tests/ac4enc/test_ac4enc_presentations.cpp
writes with AC4ENC_WRITE_PRESENTATIONS, beside the configuration each was made from as JSON), whose
MediaInfo reading (`--Output=JSON`) lists every presentation with the configured
presentation_config, presentation_id, md_compat (MediaInfo's "PresentationLevel"), groups,
dialnorm and language (its dialogue substream's, else its main substream's; MediaInfo names a Part 1
Table 92 code rather than printing it), and every group with its content classifier and language;
whose trace (`--Details=1`) frames each alternative presentation's name as written, name_len its
bytes and the 0 after them, and holds at MediaInfo's offset the name's bytes; and whose every
substream field MediaInfo details in the first frame holds the value the decoder's syntax trace
reads there, which tests/ac4enc/test_ac4enc_presentations.cpp holds equal to the encoder's own.
MediaInfo reads no substream after a presentation_config 6 (EMDF-only) presentation, so the
encoder's streams list that presentation last.

MediaInfo and DEE's muxer come from DEE's install, so this runs locally, never in CI
(tools/generators/gen_ac4_baseline.py's DEE_DIR).

Usage:
    python tools/checks/check_ac4_encode_readers.py --cli ac3cli.exe [--dee-dir DIR] [--work DIR]
        [--only presentations]
"""

import argparse
import json
import random
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "ci"))
import fuzz_ac4_encoder_space as space  # noqa: E402  (sync_frames, crc16)
import fuzz_encoder_space as ac3space  # noqa: E402  (write_wav)

DEE_DIR = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir")
IFRAME_INTERVAL = 24  # the encoder's default, which ac4-encode keeps


@dataclass
class Configuration:
    channels: int
    rate: int
    kbps: int
    dialnorm: float
    options: tuple = ()
    stem: bool = False  # a dialogue stem beside the programme: its C, or L and R, at half level
    seconds: float = 1.28

    def option(self, key):
        """The value of ac4-encode's key= option, or None."""
        for o in self.options:
            if o.startswith(f"{key}="):
                return o.split("=", 1)[1]
        return None


# The frame-rate= spellings, by frame_rate_index.
FRAME_RATES = ("23.976", "24", "25", "29.97", "30", "47.95", "48", "50", "59.94", "60", "100",
               "119.88", "120")

CONFIGURATIONS = [
    Configuration(2, 48000, 192, 24),
    Configuration(2, 48000, 64, 31),
    Configuration(1, 48000, 96, 18),
    Configuration(2, 44100, 256, 27),
    Configuration(1, 44100, 48, 1),
    Configuration(6, 48000, 384, 27),
    Configuration(6, 48000, 192, 20),
    Configuration(5, 44100, 320, 31),
    Configuration(6, 48000, 256, 24, ("experimental=coding-configs",)),
    Configuration(8, 48000, 640, 24, ("experimental=7x-back",)),
    Configuration(8, 48000, 448, 24, ("experimental=7x-wide",)),
    Configuration(7, 48000, 448, 24, ("experimental=7x-top-front",)),
    Configuration(8, 48000, 512, 24, ("experimental=7x-top-front",)),
    # A-CPL: ASPX_ACPL_2 and ASPX_ACPL_3 by the rate, and the experimental modes by name.
    Configuration(6, 48000, 128, 24),
    Configuration(6, 48000, 96, 27),
    Configuration(5, 48000, 112, 31),
    Configuration(6, 48000, 160, 24, ("codec-mode=aspx-acpl-1", "experimental=acpl")),
    Configuration(2, 48000, 48, 24, ("codec-mode=aspx-acpl-2", "experimental=acpl")),
    Configuration(2, 44100, 64, 27, ("codec-mode=aspx-acpl-1", "experimental=acpl")),
    # Phase E5: frame rates, rate modes and I-frames.
    Configuration(2, 48000, 128, 24.25, ("frame-rate=29.97", "rate-mode=average",
                                         "iframe-interval=10")),
    Configuration(2, 48000, 192, 24.5, ("frame-rate=120", "rate-mode=variable", "iframes=3,7")),
    Configuration(6, 48000, 256, 20.75, ("frame-rate=25", "iframe-interval=5")),
    Configuration(1, 48000, 64, 17, ("frame-rate=59.94", "fragment=0.5")),
    # The loudness values, measured over four seconds, with each practice among them.
    Configuration(2, 48000, 192, 24, ("loudness=ebu-r128",), seconds=4.0),
    Configuration(6, 48000, 384, 27, ("loudness=atsc-a85", "frame-rate=50"), seconds=4.0),
    Configuration(1, 44100, 96, 31, ("loudness=consumer-leveller",), seconds=4.0),
    # DRC: the four modes on one profile, modes on their own and a repeat, and the gains.
    Configuration(2, 48000, 192, 24, ("drc=film-light",)),
    Configuration(6, 48000, 384, 27, ("drc=film-standard", "drc-portable-speakers=speech",
                                      "drc-portable-headphones=speech", "drc-home-theatre=none")),
    Configuration(6, 48000, 256, 24, ("drc=music-light", "experimental=drc-gains-1")),
    Configuration(2, 48000, 192, 24, ("drc=speech", "experimental=drc-gains-3",
                                      "drc-flat-panel-tv=music-standard")),
    Configuration(1, 48000, 128, 24, ("drc=none", "experimental=drc-gains-0")),
    Configuration(5, 48000, 320, 24, ("drc=music-standard", "experimental=drc-gains-2")),
    # The downmix values: Lo/Ro's, Lt/Rt's beside them, the LFE's, each preferred method and
    # the loudness corrections.
    Configuration(6, 48000, 384, 24, ("lorocmixlev=-1.5", "lorosurmixlev=-4.5", "lfemix=-4.5",
                                      "dmixmod=loro", "loro-correction=-2")),
    Configuration(5, 48000, 320, 24, ("ltrtcmixlev=-3", "ltrtsurmixlev=-6", "dmixmod=pl2",
                                      "ltrt-correction=1.5", "cmixlev=+3", "surmixlev=0")),
    Configuration(6, 48000, 256, 24, ("dmixmod=ltrt", "lfemix=+5.5", "lorocmixlev=off")),
    Configuration(6, 48000, 128, 24, ("dmixmod=none", "surmixlev=off")),
    # Dialogue enhancement: marked channels by each method, and a stem.
    Configuration(1, 48000, 96, 24, ("dialogue-channels=c", "dialogue-max-gain=12")),
    Configuration(2, 48000, 192, 24, ("dialogue-channels=l,r", "dialogue-method=mid",
                                      "dialogue-max-gain=6")),
    Configuration(6, 48000, 384, 24, ("dialogue-channels=l,r,c", "dialogue-max-gain=9",
                                      "frame-rate=50")),
    Configuration(2, 48000, 192, 24, ("dialogue-channels=l,r",), stem=True),
    Configuration(6, 48000, 384, 24, ("dialogue-channels=l,r,c", "dialogue-method=cross",
                                      "dialogue-max-gain=3"), stem=True),
    Configuration(6, 48000, 256, 24, ("dialogue-channels=l,r", "dialogue-method=cross"),
                  stem=True),
]

# The substream fields each metadata option sends, one of which at least MediaInfo must show,
# with the value the encoder wrote, where the configuration sends them.
REQUIRED = {
    "loudness": ("loud_prac_type", "loudrelgat", "max_loudstrm3s", "max_truepk", "lra",
                 "max_loudmntry"),
    "drc": ("drc_decoder_nr_modes", "drc_decoder_mode_id", "drc_default_profile_flag",
            "drc_eac3_profile"),
    "drc-mode": ("drc_compression_curve_flag", "drc_lev_nullband_low", "drc_gain_max_cut",
                 "drc_tc_attack"),
    "drc-repeat": ("drc_repeat_profile_flag", "drc_repeat_id"),
    "drc-gains": ("drc_gains_config", "drc_gainset_size_value"),
    "downmix": ("loro_centre_mixgain", "loro_surround_mixgain", "preferred_dmx_method"),
    "ltrt": ("ltrt_centre_mixgain", "ltrt_surround_mixgain"),
    "lfemix": ("lfe_mixgain",),
    "loro-correction": ("loro_dmx_loud_corr",),
    "ltrt-correction": ("ltrt_dmx_loud_corr",),
    "dialogue": ("de_method", "de_max_gain", "de_channel_config", "de_par_code"),
    "cross": ("de_mix_coef1_idx",),
}


def required_fields(config):
    """The substream fields MediaInfo must show for `config`."""
    wanted = []
    keys = {o.split("=", 1)[0] for o in config.options}
    if "loudness" in keys:
        wanted += [f for f in REQUIRED["loudness"]
                   if config.seconds >= 3.0 or f not in ("max_loudstrm3s", "lra")]
    if "drc" in keys:
        wanted += REQUIRED["drc"]
        modes = [o.split("=", 1)[1] for o in config.options if o.startswith("drc-") and "=" in o]
        profile = config.option("drc")
        gains = any(o.startswith("experimental=drc-gains-") for o in config.options)
        if gains:
            # Every mode sends gains, a mode's own profile among them, and no curve.
            wanted += REQUIRED["drc-gains"]
        elif any(m != profile for m in modes):
            wanted += REQUIRED["drc-mode"]
        if len(modes) != len(set(modes)):
            wanted += REQUIRED["drc-repeat"]
    if keys & {"lorocmixlev", "lorosurmixlev", "ltrtcmixlev", "ltrtsurmixlev", "cmixlev",
               "surmixlev", "lfemix", "dmixmod", "loro-correction", "ltrt-correction"}:
        wanted += REQUIRED["downmix"]
        if keys & {"ltrtcmixlev", "ltrtsurmixlev"}:
            wanted += REQUIRED["ltrt"]
        for key in ("lfemix", "loro-correction", "ltrt-correction"):
            if key in keys and config.option(key) != "off":
                wanted += REQUIRED[key]
    if "dialogue-channels" in keys:
        wanted += REQUIRED["dialogue"]
        if config.option("dialogue-method") == "cross":
            wanted += REQUIRED["cross"]
    return wanted

# The channel mode MediaInfo names for each configuration: Part 1 Table 88's, as its trace
# prints it.
MODES = {1: "Mono", 2: "Stereo", 5: "5.0", 6: "5.1"}
SEVEN_X_MODES = {"7x-back": "3/4/0", "7x-wide": "5/2/0", "7x-top-front": "3/2/2"}


def channel_mode(channels, options):
    for option in options:
        for tool, layout in SEVEN_X_MODES.items():
            if tool in option:
                return f"7.{channels - 7} {layout}" + (".1" if channels == 8 else "")
    return MODES[channels]

# The fields check_frame() holds to the configuration: the sync frame's, the table of contents'
# and dialnorm_bits. against_trace() holds the substreams' others to the encoder's syntax trace.
FIELDS = ("sync_word", "frame_size", "bitstream_version", "sequence_counter", "b_wait_frames",
          "wait_frames", "fs_index", "frame_rate_index", "b_iframe_global", "presentation_version",
          "channel_mode", "dialnorm_bits", "crc_word")
# The samples a frame decodes to at 48 kHz, num / den, by frame_rate_index (Part 2 clause 5.11).
PER_FRAME = ((2002, 1), (2000, 1), (1920, 1), (8008, 5), (1600, 1), (1001, 1), (1000, 1), (960, 1),
             (4004, 5), (800, 1), (480, 1), (2002, 5), (400, 1), (2048, 1))


def rate_index(config):
    frame_rate = config.option("frame-rate")
    return 13 if frame_rate in (None, "native") else FRAME_RATES.index(frame_rate)


def iframe_schedule(config, count):
    """The frames of `count` the encoder makes I-frames: every iframe-interval= frames (24 by
    default), those iframes= names, and for each fragment= start the first frame whose output
    starts there or after."""
    interval = int(config.option("iframe-interval") or IFRAME_INTERVAL)
    out = {f for f in range(count) if f % interval == 0}
    if config.option("iframes"):
        out |= {int(f) for f in config.option("iframes").split(",")}
    if config.option("fragment"):
        num, den = PER_FRAME[rate_index(config)]
        step = float(config.option("fragment")) * config.rate
        at = step
        while at < count * num / den:
            start = round(at)
            out.add(next(f for f in range(count + 1) if f * num // den >= start))
            at += step
    return out

LINE = re.compile(r"^([0-9A-F]{4,})\s+(.*?):\s+(.*)$")
# A Huffman code MediaInfo reads without showing its value: a line of its own, "de_par_code (1
# bytes)".
CODE = re.compile(r"^[0-9A-F]{4,}\s+(de_par_code) \(\d+ bytes\)$")
FRAME = re.compile(r"^([0-9A-F]{4,}) ac4_syncframe - (\d+) ")


def run(command, timeout=600):
    # A timeout for every tool: DEE's muxer can hang on a stream (src/ac4enc/ERRATA.md, "An
    # alternative presentation's dac4").
    try:
        result = subprocess.run(
            [str(c) for c in command], capture_output=True, text=True, check=False, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        raise SystemExit(
            f"{' '.join(str(c) for c in command)} did not finish in {timeout} s"
        ) from None
    if result.returncode != 0:
        raise SystemExit(f"{' '.join(str(c) for c in command)} failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    return result.stdout


def mediainfo_frames(mediainfo, stream):
    """Per sync frame MediaInfo details, a list of (name, value) pairs in order."""
    frames = []
    for line in run([mediainfo, "--Details=1", "--ParseSpeed=1", stream]).splitlines():
        if FRAME.match(line):
            frames.append([])
            continue
        match = LINE.match(line)
        if match and frames:
            frames[-1].append((match.group(2).strip(), match.group(3).strip()))
        code = CODE.match(line)
        if code and frames:
            frames[-1].append((code.group(1), ""))
    return frames


def number(value):
    return int(value.split()[0])


def expected_counter(frame):
    return 0 if frame == 0 else (frame - 1) % 1020 + 1


def check_frame(index, fields, raw, crc, mode, rate, config, iframe):
    """What is wrong with one frame's MediaInfo fields, as a list of messages."""
    wrong = []
    seen = {}
    for name, value in fields:
        seen.setdefault(name, value)

    def want(name, test, description):
        if name in seen and not test(seen[name]):
            wrong.append(f"frame {index}: {name} is {seen[name]!r}, expected {description}")

    want("sync_word", lambda v: number(v) == 0xAC41, "0xAC41")
    want("frame_size", lambda v: number(v) == len(raw), str(len(raw)))
    want("bitstream_version", lambda v: number(v) == 2, "2")
    counter = expected_counter(index)
    want("sequence_counter", lambda v: number(v) == counter, str(counter))
    # b_wait_frames set, and wait_frames 0 at a constant rate (decode at once), 1 to 6 at an
    # average one and 7 at a variable one (Part 1 Table 81).
    want("b_wait_frames", lambda v: v == "Yes", "Yes")
    waits = {"average": range(1, 7), "variable": (7,)}.get(config.option("rate-mode"), (0,))
    want("wait_frames", lambda v: number(v) in waits, f"one of {list(waits)}")
    want("frame_rate_index", lambda v: number(v) == rate_index(config), str(rate_index(config)))
    want("fs_index", lambda v: v.endswith(f"{rate} Hz"), f"{rate} Hz")
    want("b_iframe_global", lambda v: v == ("Yes" if iframe else "No"), "Yes" if iframe else "No")
    want("presentation_version", lambda v: number(v) == 1, "1")
    want("channel_mode", lambda v: v.endswith(mode), mode)
    bits = round(4 * config.dialnorm)
    want("dialnorm_bits", lambda v: number(v) == bits, str(bits))
    want("crc_word", lambda v: number(v) == crc, f"0x{crc:04X}")
    return wrong, set(seen)


def field_value(text):
    """A MediaInfo field's value as a number: its first token, or 1 and 0 for Yes and No; None
    where it is neither."""
    first = text.split()[0] if text.split() else ""
    if first in ("Yes", "No"):
        return 1 if first == "Yes" else 0
    try:
        return int(first, 0)
    except ValueError:
        return None


# MediaInfo's names where the syntax's differ: the 6-bit read drc_gainset_size_value, which it
# names after the variable it goes into (Part 1 clause 4.2.14.10).
ALIASES = {"drc_gainset_size": "drc_gainset_size_value"}


def substream_fields(fields):
    """MediaInfo's fields of a frame, their names without the syntax's indices and as the syntax
    names them, leaving out variable_bits()' b_more_bits and the extension each set flag brings,
    which MediaInfo names after the field extended and the encoder's trace otherwise."""
    out = []
    extension = False
    for shown, value in fields:
        name = re.sub(r"\[.*$", "", shown)
        if name == "b_more_bits":
            extension = value.startswith("Yes")
            continue
        if extension:
            extension = False
            continue
        out.append((ALIASES.get(name, name), field_value(value)))
    return out


def against_trace(fields, records):
    """MediaInfo's fields against the encoder's syntax trace of the same frame, in order: each
    field whose name the trace has takes the next record of that name. The fields that differ, and
    the names checked. MediaInfo reads a DRC gainset no further than drc_gain_val: it does not
    decode the gains' Huffman codes, and what it shows after that is not where the encoder's
    fields are, so the comparison stops there."""
    names = {name for name, _ in records}
    at = 0
    wrong = []
    checked = set()
    for name, value in fields:
        if name not in names or value is None:
            continue
        for k in range(at, len(records)):
            if records[k][0] == name:
                at = k + 1
                if records[k][1] != value:
                    wrong.append(f"{name} is {value}, the encoder wrote {records[k][1]}")
                checked.add(name)
                break
        if name == "drc_gain_val":
            break
    return wrong, checked


def trace_frames(path):
    """The encoder's syntax trace, per frame a list of (name, value) in order."""
    frames = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) == 6:
            frames.setdefault(int(parts[0]), []).append((parts[5], int(parts[4])))
    return frames


# The channel groups (Part 2 Table A.27) of the 7.X 3/2/2 layouts, 7.0 and 7.1, which DEE's muxer
# writes without group 4, their top front pair.
TOP_FRONT_GROUPS = {7: 0x17, 8: 0x57}


def without_group_4(box, groups):
    """`box` with each 24-bit channel group mask `groups` in it written without group 4."""
    bits = "".join(f"{b:08b}" for b in box)
    bits = bits.replace(f"{groups:024b}", f"{groups & ~(1 << 4):024b}")
    return bytes(int(bits[i : i + 8], 2) for i in range(0, len(bits), 8))


def dac4(path):
    """The payload of the first 'dac4' box in an MP4 file."""
    data = Path(path).read_bytes()
    at = data.find(b"dac4")
    if at < 4:
        raise SystemExit(f"{path}: no dac4 box")
    size = struct.unpack(">I", data[at - 4:at])[0]
    return data[at + 4:at - 4 + size]


# --- Phase E6: the encoder's presentations ------------------------------------------------

PRESENTATION_STREAMS = REPO / "tests" / "golden" / "ac4dec" / "presentations"
# MediaInfo's names for Part 2 Table 53's presentation_config and Part 1 Table 91's
# content_classifier.
CONFIG_NAMES = {0: "Music and Effects + Dialogue", 1: "Main + Dialogue Enhancement",
                2: "Main + Associate", 3: "Music and Effects + Dialogue + Associate",
                4: "Main + Dialogue Enhancement + Associate", 5: "Arbitrary Substream Groups",
                6: "EMDF Only"}
CLASSIFIERS = ("Main", "Music and Effects", "Visually Impaired", "Hearing Impaired", "Dialogue",
               "Commentary", "Emergency", "Voice Over")
# Table 54's content classifiers of associated audio.
ASSOCIATED = (2, 3, 5)
# Part 1 Table 92's codes, which MediaInfo names instead of printing them as a language.
TABLE_92 = ("qad", "qax", "qas", "qtx", "qss", "qsx", "qei", "qex")
# Table 53's roles by position, for the presentation's language.
DIALOGUE_POSITION = {0: 1, 3: 1}


def presentation_language(presentation, substreams):
    """The language MediaInfo gives a presentation (Part 1 clause 4.3.3.8.8's NOTE): its dialogue
    substream's, else its main or music and effects substream's; '' for none. MediaInfo gives
    none to a presentation of one group classified as associated audio, a commentary alone, which
    Part 2 clause 6.3.2.2.1 makes a main substream and the decoder takes the language of."""
    members = presentation["substreams"]
    config = presentation["presentation_config"]
    if config is None and substreams[members[0]]["content_classifier"] in ASSOCIATED:
        return ""
    dialogue = None
    if config in DIALOGUE_POSITION:
        dialogue = members[DIALOGUE_POSITION[config]]
    elif config == 5:
        dialogue = next((m for m in members if substreams[m]["content_classifier"] == 4), None)
    for index in ([dialogue] if dialogue is not None else []) + [members[0]]:
        language = substreams[index]["language"]
        if language and language.lower() not in TABLE_92:
            return language
    return ""


def name_bytes(path, offset, length):
    """The bytes of the file at the name MediaInfo frames, from `offset`, where one alternative
    presentation's b_name_present is, the first bit of its presentation substream, and `length`
    bytes of it, name_len; the name starts 7 bits on (b_name_present, b_length, name_len)."""
    data = Path(path).read_bytes()
    bits = "".join(f"{b:08b}" for b in data[offset:offset + length + 2])
    return bytes(int(bits[7 + 8 * i:15 + 8 * i], 2) for i in range(length))


def mediainfo_names(mediainfo, stream):
    """(offset of b_name_present, name_len) for each alternative presentation MediaInfo details
    in the first frame, in order."""
    out = []
    lines = run([mediainfo, "--Details=1", "--ParseSpeed=1", stream]).splitlines()
    frames = 0
    present = None
    for line in lines:
        if FRAME.match(line):
            frames += 1
            if frames > 1:
                break
            continue
        match = LINE.match(line)
        if not match:
            continue
        name, value = match.group(2).strip(), match.group(3).strip()
        if name == "b_name_present" and value.startswith("Yes"):
            present = int(match.group(1), 16)
        elif name == "name_len" and present is not None:
            out.append((present, number(value)))
            present = None
    return out


def check_presentation_stream(mediainfo, cli, stream, work):
    """What MediaInfo reads of one of the encoder's presentation streams against the
    configuration it was made from, as a list of messages; and a summary."""
    config = json.loads(stream.with_suffix(".json").read_text(encoding="utf-8"))
    substreams = config["substreams"]
    presentations = config["presentations"]
    wrong = []
    info = json.loads(run([mediainfo, "--Output=JSON", stream]))
    audio = next(t for t in info["media"]["track"] if t["@type"] == "Audio")
    extra = audio.get("extra", {})
    shown = extra.get("Presentation", [])
    if len(shown) != len(presentations):
        wrong.append(f"MediaInfo lists {len(shown)} presentations, the stream has "
                     f"{len(presentations)}")
    checked = 0
    for pos, (want, got) in enumerate(zip(presentations, shown, strict=False)):
        def expect(field, value, got=got, pos=pos):
            if got.get(field) != value:
                wrong.append(f"presentation {pos}: MediaInfo's {field} is {got.get(field)!r}, "
                             f"the configuration's {value!r}")
        config_value = want["presentation_config"]
        if config_value == 6:
            expect("PresentationConfig", CONFIG_NAMES[6])
            continue
        expect("PresentationID", str(want["presentation_id"]))
        expect("PresentationLevel", str(want["md_compat"]))
        expect("LinkedTo_Group_Pos", " + ".join(str(s) for s in want["substreams"]))
        if config_value is not None:
            expect("PresentationConfig", CONFIG_NAMES[config_value])
        if "DialogueNormalization" in got:
            expect("DialogueNormalization", f"{want['dialnorm_db']:.2f}")
        else:
            wrong.append(f"presentation {pos}: MediaInfo shows no dialnorm")
        language = presentation_language(want, substreams)
        if language:
            expect("Language", language)
        elif "Language" in got:
            wrong.append(f"presentation {pos}: MediaInfo's Language is {got['Language']!r}, the "
                         "configuration has none")
        checked += 1
    groups = extra.get("Group", [])
    if len(groups) != len(substreams):
        wrong.append(f"MediaInfo lists {len(groups)} groups, the stream has {len(substreams)}")
    for pos, (want, got) in enumerate(zip(substreams, groups, strict=False)):
        classifier = want["content_classifier"]
        if classifier is not None and got.get("Classifier") != CLASSIFIERS[classifier]:
            wrong.append(f"group {pos}: MediaInfo's classifier is {got.get('Classifier')!r}, the "
                         f"configuration's {CLASSIFIERS[classifier]!r}")
        language = want["language"]
        if language and language.lower() not in TABLE_92 and got.get("Language") != language:
            wrong.append(f"group {pos}: MediaInfo's language is {got.get('Language')!r}, the "
                         f"configuration's {language!r}")
    # The alternative presentations' names, as MediaInfo frames them.
    named = [p["name"] for p in presentations if p["name"]]
    framed = mediainfo_names(mediainfo, stream)
    if len(framed) != len(named):
        wrong.append(f"MediaInfo frames {len(framed)} names, the configuration has {len(named)}")
    for name, (offset, length) in zip(named, framed, strict=False):
        text = name.encode("utf-8")
        if length != len(text) + 1:
            wrong.append(f"MediaInfo's name_len for {name!r} is {length}, the name's bytes and "
                         f"its 0 are {len(text) + 1}")
        elif name_bytes(stream, offset, length) != text + b"\0":
            wrong.append(f"the bytes where MediaInfo reads {name!r}'s name are "
                         f"{name_bytes(stream, offset, length)!r}")
    # Every substream field MediaInfo details, against the decoder's trace of the same frame.
    trace = work / f"{stream.stem}.trace.tsv"
    run([cli, "decode", stream, work / f"{stream.stem}.wav", f"syntax-trace={trace}"])
    frames = mediainfo_frames(mediainfo, stream)
    records = trace_frames(trace)
    differ, names = against_trace(substream_fields(frames[0]), records.get(0, []))
    wrong += [f"frame 0: {d}" for d in differ]
    raw_frames, _ = space.sync_frames(stream.read_bytes())
    if len(frames) != len(raw_frames):
        wrong.append(f"MediaInfo found {len(frames)} sync frames, the stream has {len(raw_frames)}")
    summary = (f"{stream.name}: MediaInfo lists {len(shown)} presentations and {len(groups)} "
               f"groups as configured ({checked} with audio), frames {len(framed)} names, and "
               f"reads {len(names)} substream fields as the decoder's trace does")
    return wrong, summary


def check_presentations(mediainfo, cli, work):
    failures = []
    streams = sorted(PRESENTATION_STREAMS.glob("encoder-*.ac4"))
    if not streams:
        failures.append(f"no encoder presentation streams under {PRESENTATION_STREAMS}")
    for stream in streams:
        wrong, summary = check_presentation_stream(mediainfo, cli, stream, work)
        print(summary)
        failures += [f"{stream.name}: {w}" for w in wrong]
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--dee-dir", type=Path, default=DEE_DIR)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--only", choices=("presentations",),
                        help="run phase E6's presentation checks alone")
    args = parser.parse_args()
    mediainfo = args.dee_dir / "MediaInfo.exe"
    muxer = args.dee_dir / "dee_mp4muxer.exe"
    for tool in (mediainfo, muxer):
        if not tool.exists():
            raise SystemExit(f"{tool} not found; this check needs DEE's install")

    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        failures += check_presentations(mediainfo, args.cli, work)
        for config in CONFIGURATIONS if args.only is None else ():
            channels, rate, kbps = config.channels, config.rate, config.kbps
            options = config.options
            name = f"{channels}ch-{rate}-{kbps}-dn{config.dialnorm:g}" + "".join(
                f"-{o.split('=')[1]}" for o in options
            ) + ("-stem" if config.stem else "")
            rng = random.Random(kbps * 7 + channels)
            blocks = int(config.seconds * rate) // 256
            pcm = ac3space.generate_pcm(rng, channels, blocks, rate, "chaotic", "pairs")
            wav = work / f"{name}.wav"
            ac3space.write_wav(wav, pcm, rate, False)
            extra = [f"dialnorm={config.dialnorm:g}", "quiet"]
            if config.stem:
                # The dialogue: C at half level, or L and R where there is no C.
                keep = (2,) if channels > 2 else (0, 1)
                stem = [[0.5 * x for x in channel] if c in keep else [0.0] * len(channel)
                        for c, channel in enumerate(pcm)]
                ac3space.write_wav(work / f"{name}.stem.wav", stem, rate, False)
                extra.append(f"dialogue-stem={work / f'{name}.stem.wav'}")
            stream = work / f"{name}.ac4"
            ours_mp4 = work / f"{name}.mp4"
            trace = work / f"{name}.tsv"
            run([args.cli, "ac4-encode", wav, stream, kbps, *extra, *options,
                 f"syntax-trace={trace}"])
            run([args.cli, "ac4-encode", wav, ours_mp4, kbps, *extra, *options])

            data = stream.read_bytes()
            raw_frames, why = space.sync_frames(data)
            if raw_frames is None:
                failures.append(f"{name}: {why}")
                continue
            written = trace_frames(trace)
            iframes = iframe_schedule(config, len(raw_frames))
            frames = mediainfo_frames(mediainfo, stream)
            covered = set()
            checked = set()
            detailed = 0
            compared = 0
            for index, fields in enumerate(frames[:len(raw_frames)]):
                raw = raw_frames[index]
                crc = space.crc16(len(raw).to_bytes(2, "big") + raw)
                wrong, seen = check_frame(index, fields, raw, crc, channel_mode(channels, options),
                                          rate, config, index in iframes)
                failures += [f"{name}: {w}" for w in wrong]
                covered |= seen
                detailed += len(seen) > 3
                ours = written.get(index, [])
                differ, names = against_trace(substream_fields(fields), ours)
                failures += [f"{name}: frame {index}: {d}" for d in differ]
                checked |= names
                # Dialogue enhancement's parameters, where MediaInfo details the frame's
                # metadata: as many codes as the encoder wrote. With de_ms_proc_flag MediaInfo
                # reads a set for each of de_nr_channels where Table 78 reads de_nr_channels -
                # de_ms_proc_flag, and so twice what the encoder wrote for L and R's Mid.
                if any(n == "b_de_data_present" for n, _ in fields):
                    codes = sum(1 for n, _ in fields if n == "de_par_code")
                    wrote = sum(1 for n, _ in ours if n == "de_par_code")
                    mid = any(n == "de_ms_proc_flag" and v.startswith("Yes") for n, v in fields)
                    if codes != (2 * wrote if mid else wrote):
                        failures.append(f"{name}: frame {index}: MediaInfo read {codes} "
                                        f"de_par_code, the encoder wrote {wrote}")
                    elif wrote:
                        checked.add("de_par_code")
                compared += len(names) > 0
            missing = sorted(set(FIELDS) - covered)
            unseen = [f for f in required_fields(config) if f not in checked]
            print(f"{name}: MediaInfo found {len(frames)} sync frames and detailed {detailed}; "
                  f"{len(FIELDS) - len(missing)} of {len(FIELDS)} table of contents fields and "
                  f"{len(checked)} substream fields, in {compared} frames, checked"
                  + (f", never shown: {', '.join(missing)}" if missing else ""))
            if unseen:
                failures.append(f"{name}: MediaInfo never showed {', '.join(unseen)}")
            if len(frames) != len(raw_frames):
                failures.append(f"{name}: MediaInfo found {len(frames)} sync frames, "
                                f"the stream has {len(raw_frames)}")
            theirs = work / f"{name}.dee.mp4"
            run([muxer, "--track", stream, "-o", theirs, "--overwrite", "1"])
            ours_box, their_box = dac4(ours_mp4), dac4(theirs)
            same = ours_box == their_box
            note = ""
            if not same and any("7x-top-front" in o for o in options):
                same = without_group_4(ours_box, TOP_FRONT_GROUPS[channels]) == their_box
                note = ", but for group 4, which DEE's muxer leaves out of 3/2/2" if same else ""
            print(f"{name}: dac4 {'equal' if same else 'DIFFERS'} ({len(ours_box)} bytes){note}"
                  + ("" if same else f": ours {ours_box.hex()}, DEE's muxer {their_box.hex()}"))
            if not same:
                failures.append(f"{name}: the dac4 boxes differ")
    if failures:
        print("\nFAILED:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print(f"\n{len(CONFIGURATIONS) if args.only is None else 0} configurations: MediaInfo reads "
          "each as configured and as the encoder wrote it, and DEE's muxer writes the encoder's "
          "dac4; and the encoder's presentation streams as configured")
    return 0


if __name__ == "__main__":
    sys.exit(main())
