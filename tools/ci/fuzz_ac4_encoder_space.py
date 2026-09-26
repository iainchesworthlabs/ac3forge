"""Property/fuzz harness over the AC-4 encoder's input space.

The question tools/ci/fuzz_encoder_space.py asks of AC-3's `encode` and
tools/ci/fuzz_eac3_encoder_space.py of `eac3-encode`, asked of `ac4-encode`:
driven across its configuration space by adversarial but valid audio, does the
encoder ever write a stream a reader disagrees with, or refuse, crash or
misframe something it should have encoded? planning/ac4.md, the encoder's
ladder, item 7.

The PCM comes from the AC-3 harness's generator, imported rather than copied:
its per-block plan, the `cliff` profile and the correlation modes serve every
codec. The configuration space is this encoder's: mono, stereo, 5.0 and 5.1,
7.0 and 7.1 in the three 7.X layouts, and in a case in eight the immersive
layouts, 5.0.4 and 5.1.4, and 7.0.4 and 7.1.4 with experimental=back-pair, in
the codec mode the rate picks or SCPL, ASPX_SCPL, ASPX_ACPL_2, ASPX_ACPL_1
(with experimental=acpl) or ASPX_AJCC (with experimental=ajcc) forced, with the
height downmix now and then;
48 kHz at every frame rate of Part 1
Table 83 or 44.1 kHz at the native one, a rate from 8 kbps up (20 in 5.X and
7.X), constant, average or variable, the codec mode the rate picks, SIMPLE or
ASPX forced, or an A-CPL mode forced (ASPX_ACPL_2 and ASPX_ACPL_3 in 5.X, and
with experimental=acpl ASPX_ACPL_1 there and both in stereo), the experimental
tools, I-frames at an interval, at named frames and at fragment starts, the
metadata (dialnorm, the further loudness values, DRC's modes with their
profiles and with experimental gains, the downmix values, and dialogue
enhancement from marked channels or a stem by each method), and raw or MP4
output. A case in five also has several substreams and the presentations of
Part 2 Table 53 that play them (substreamN= and presentationN=): music and
effects with dialogue, main audio with associated audio, main audio whose
hybrid dialogue enhancement's waveform has a substream of its own, the three
together, roles by content classifier, one presentation per substream, and
now and then one of EMDF payloads alone; with languages, classifiers, rate
shares, dialogue mixing values, group gains, the associated audio's mixing
values, ids, levels, names and payloads drawn for them.

Each case is held to:

  traces   the encoder's own trace of what it wrote (`ac4-encode ...
           syntax-trace=`), the decoder's trace of what it read (`decode ...
           syntax-trace=`) and tools/references/ac4_syntax.py's trace of the
           stream are the same, record for record, and the Python parser reads
           every substream to its end with its invariants holding - the
           ladder's item 1;
  framing  the stream's sync frames, walked here from the sync word and
           frame_size alone, tile the file exactly, each with its CRC (Part 2
           Annex G, computed here), and FFmpeg's raw AC-4 demuxer finds as many
           packets as the encoder wrote frames, each the size of its raw frame;
           the MP4 output's track is one AC-4 stream FFmpeg's mov demuxer reads
           with that many samples - item 3's FFmpeg half;
  decode   `ac3cli decode` reads it to PCM: every frame, at the input's
           channel count and sample rate, the frames' lengths adding up to
           what the frame rate gives that many frames (Part 2 clause 5.11),
           and the output covering the input delayed by the lag ac4-encode
           reports, with no more than a frame to spare. With several
           substreams it decodes the first presentation, which plays the
           input's substream with others that add no channel to it.

A configuration outside the encoder's range - a rate below 8 or above 3000
kbps, or at 44.1 kHz a frame rate other than the native one - must be refused
with the encoder's own message, and is drawn on purpose now and then.
--check-envelope re-measures that range.

Every case is a pure function of one 64-bit case seed, printed with any
failure; --replay reruns it, and REGRESSION_SEEDS holds the seeds that ever
failed, replayed by --regressions.

Usage (repo root, after building):
  python tools/ci/fuzz_ac4_encoder_space.py --cli build/dev/bin/ac3cli.exe --cases 50
  python tools/ci/fuzz_ac4_encoder_space.py --seconds 120        # bounded, for CI
  python tools/ci/fuzz_ac4_encoder_space.py --check-envelope     # the accepted rates
  python tools/ci/fuzz_ac4_encoder_space.py --replay 1234567890  # one exact case
"""

import argparse
import concurrent.futures
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(REPO / "tools" / "references"))

import ac4_syntax  # noqa: E402  (the paths above have to come first)
import fuzz_encoder_space as ac3space  # noqa: E402

FRAME = 2048  # samples per frame at frame_rate_index 13
# Part 1 Table 83 at 48 kHz: frame-rate='s spelling of each frame_rate_index, and the samples a
# frame decodes to, a fraction where the frames alternate in length (Part 2 clause 5.11).
FRAME_RATES = {
    0: ("23.976", Fraction(2002)),
    1: ("24", Fraction(2000)),
    2: ("25", Fraction(1920)),
    3: ("29.97", Fraction(8008, 5)),
    4: ("30", Fraction(1600)),
    5: ("47.95", Fraction(1001)),
    6: ("48", Fraction(1000)),
    7: ("50", Fraction(960)),
    8: ("59.94", Fraction(4004, 5)),
    9: ("60", Fraction(800)),
    10: ("100", Fraction(480)),
    11: ("119.88", Fraction(2002, 5)),
    12: ("120", Fraction(400)),
    13: ("native", Fraction(FRAME)),
}
# The metadata options' values, as ac4-encode spells them.
PRACTICES = ["atsc-a85", "ebu-r128", "arib-tr-b32", "freetv-op59", "manual", "consumer-leveller",
             "not-indicated"]
DRC_PROFILES = ["film-standard", "film-light", "music-standard", "music-light", "speech", "none"]
DRC_MODES = ["drc-home-theatre", "drc-flat-panel-tv", "drc-portable-speakers",
             "drc-portable-headphones"]
CENTRE_LEVELS = ["+3", "+1.5", "0", "-1.5", "-3", "-4.5", "-6", "off"]
SURROUND_LEVELS = ["0", "-1.5", "-3", "-4.5", "-6", "off"]
DOWNMIX_METHODS = ["loro", "ltrt", "pl2", "none"]
BLOCK = ac3space.BLOCK  # the generator's block, 256 samples
SAMPLE_RATES = [48000, 44100]
# The rates drawn, weighted low, where the frame's side information is under
# the most pressure; any whole rate in between is drawn too.
RATES = [8, 12, 16, 24, 32, 48, 64, 96, 128, 144, 192, 256, 320, 384, 448, 512, 640, 768, 1024]
LOWEST_KBPS = 8
HIGHEST_KBPS = 3000
# Stereo at 48 kHz in the ASPX mode, whose 8 kbps frames of 42 bytes hold no I-frame whose A-SPX
# interval starts a slot in (a VARFIX interval's left border, which a frame after one whose interval
# ran on takes).
STEREO_48K_LOWEST_KBPS = 9
# The 5.X and 7.X elements' least rate: from 14 to 19 kbps, by layout and sample rate, the
# smallest frame holds one with no bands and every aspx_data element's least; 20 holds all.
MULTICHANNEL_LOWEST_KBPS = 20
# The A-CPL modes a case may force, by stereo or 5.X, and the least rate each holds in 5.X, a
# frame with no bands, A-SPX's least and the A-CPL parameters of an I-frame (13 to 25 kbps at 48
# kHz; --check-envelope measures them). In stereo both hold from LOWEST_KBPS.
ACPL_MODES = {False: ["aspx-acpl-1", "aspx-acpl-2"],
              True: ["aspx-acpl-1", "aspx-acpl-2", "aspx-acpl-3"]}
ACPL_LOWEST_KBPS = {"aspx-acpl-1": 16, "aspx-acpl-2": 16, "aspx-acpl-3": 26}
# The channel counts drawn, stereo and 5.1 twice as often; seven and eight channels take one of the
# 7.X element's pairs, an experimental option.
CHANNELS = [1, 2, 2, 5, 6, 6, 7, 8]
SEVEN_X = ["7x-back", "7x-wide", "7x-top-front"]
# The immersive layouts (phase E8), a case in eight, drawn from a generator of their own so that
# the other cases, the regression seeds' among them, draw as they did: 5.0.4 and 5.1.4, and 7.0.4
# and 7.1.4 with experimental=back-pair; the codec modes a case may force besides the one the rate
# picks, ASPX_ACPL_1 with experimental=acpl and ASPX_AJCC with experimental=ajcc; and the height
# downmix's routes and gains. The least rate each codec mode holds, from silence at 48 kHz
# (--check-envelope measures them), "auto" being ASPX_ACPL_2's, which the lowest rates take.
IMMERSIVE_SHARE = 0.125
IMMERSIVE_SALT = 0xE8E8E8E8E8E8E8E8
IMMERSIVE_CHANNELS = [9, 10, 10, 11, 12]
IMMERSIVE_MODES = ["scpl", "aspx-scpl", "aspx-acpl-2", "aspx-acpl-1", "aspx-ajcc"]
IMMERSIVE_LOWEST_KBPS = {"auto": 27, "scpl": 12, "aspx-scpl": 33, "aspx-acpl-2": 27,
                         "aspx-acpl-1": 28, "aspx-ajcc": 24}
# The experimental tool each immersive codec mode needs, where it needs one.
IMMERSIVE_TOOLS = {"aspx-acpl-1": "acpl", "aspx-ajcc": "ajcc"}
HEIGHT_DOWNMIXES = ["front", "surround", "front-and-surround"]
HEIGHT_GAINS = ["0", "-1.5", "-3", "-4.5", "-6", "-9", "-12", "off"]
# The encoder's delay (a frame and a half) and the decoder's at frame_rate_index 13, which the
# encoder's last frame covers: d_pcm (Part 1 Table 188), the QMF banks' 577 samples and six QMF
# slots. At the other frame rates ac4-encode reports the lag, to the nearest sample.
LAG = 3072 + 352 + 577 + 6 * 64
# The cases of several substreams: the presentations' shapes, Part 2 Table 53's configurations
# over substreams in its positions' order ("singles" is one presentation per substream); the
# classifiers of associated audio, those Part 2 Table 54 gives that role; the language tags drawn;
# and the alternative presentations' names.
TEMPLATES = ["singles", "config0", "config1", "config2", "config3", "config4", "config5"]
ASSOCIATED = ["visually-impaired", "hearing-impaired", "commentary"]
LANGUAGES = ["en", "de", "fr", "es", "ja", "qad", "en-GB", "pt-BR", "zh-Hant"]
NAMES = ["Deutsch", "Commentary", "Director", "Stadium", "Home team"]

# A frame of this many bytes holds the least frame of any configuration drawn: a silent I-frame
# with every metadata element, DRC's gains and dialogue enhancement's parameters in 7.1 among them.
# A rate that gives smaller frames may be refused as holding no least frame, if the same case at
# this size encodes.
FRAME_BYTES_CAP = 400

# Refusals a case may end in, by the text ac3cli prints for each: the encoder's reasons
# (ac4::Encoder::refusal_reason()) and ac3cli's own.
REFUSALS = {
    "rate out of range": "a rate outside 8 to 3 000 kbps",
    # A rate whose frames cannot hold the least frame of a substream, or the presentation and EMDF
    # payload substreams.
    "least frame": "a rate that cannot hold ",
    # dialnorm=auto on input BS.1770's gates leave nothing of, which the
    # generator's sparse profiles draw: the encoder commands all refuse it so.
    "nothing to measure": "no audio above the -70 LKFS absolute gate",
    "frame rate at 44.1 kHz": "at 44.1 kHz AC-4 has the native frame rate alone",
}

# Case seeds that ever failed, with why; --regressions replays them.
REGRESSION_SEEDS = {
    5756050987806798014: "an out-of-range rate at 44.1 kHz with another frame rate: ac3cli names "
    "the frame rate first, and the harness took only the rate's refusal",
    10507227253340255992: "8 kbps stereo with dialnorm=auto over input BS.1770's gates leave "
    "nothing of: refused for the rate, then at the retry's rate for the loudness, which the "
    "harness took as a failure",
    11618868545183904483: "mono and two stereo substreams at an average rate with A-SPX's balance "
    "coding: the frame gave the substream that takes what the others leave less than its least "
    "frame, and the encoder threw bad_optional_access (fixed in phase E7)",
    6658067609366379392: "7.1 music and effects, 3.0 dialogue and stereo associated audio, twelve "
    "tracks, at md-compat=3, which holds eleven: the harness drew a level the encoder refuses",
    8555004500497304315: "mono at 59.94 fps and 12 kbps with a dialogue stem: a frame between "
    "I-frames was sized for the stem's parameters coded against the last frame's, which it could "
    "not hold, where it falls back to the last frame's kept, and the encoder threw "
    "bad_optional_access (phase E6's, fixed in E7)",
}


@dataclass
class Case:
    seed: int
    channels: int
    sample_rate: int
    bitrate: int
    blocks: int
    pcm16: bool
    audio_profile: str
    correlation: str
    mp4: bool
    options: list = field(default_factory=list)
    frame_rate_index: int = 13
    # A dialogue stem beside the programme: the programme's channels, each scaled.
    stem: bool = False
    # The substreams after the input's, substream 2 on: each one's channel count, 0 for a dialogue
    # enhancement substream, which takes no input. Their options and the presentations' are in
    # `options`; ac3cli is given each input as substreamN=.
    substreams: list = field(default_factory=list)

    @property
    def in_range(self):
        return LOWEST_KBPS <= self.bitrate <= HIGHEST_KBPS

    @property
    def frame_rate_valid(self):
        return self.sample_rate == 48000 or self.frame_rate_index == 13

    @property
    def measures(self):
        return any(o == "dialnorm=auto" or o.startswith("loudness=") for o in self.options)


@dataclass
class Result:
    case: Case
    status: str  # "ok" | "refused" | "fail"
    stage: str = ""
    detail: str = ""


def draw_case(seed):
    rng = random.Random(seed)
    channels = rng.choice(CHANNELS)
    immersive_rng = random.Random(seed ^ IMMERSIVE_SALT)
    immersive = immersive_rng.random() < IMMERSIVE_SHARE
    if immersive:
        channels = immersive_rng.choice(IMMERSIVE_CHANNELS)
    roll = rng.random()
    if roll < 0.04:
        bitrate = rng.choice([1, 4, 7, 3001, 4000])  # outside the range: must be refused
    elif roll < 0.25:
        bitrate = rng.randint(LOWEST_KBPS, 1536)
    else:
        weights = [1.0 / (1.0 + 0.3 * i) for i in range(len(RATES))]
        bitrate = rng.choices(RATES, weights=weights, k=1)[0]
    if channels > 2 and LOWEST_KBPS <= bitrate < MULTICHANNEL_LOWEST_KBPS:
        bitrate = MULTICHANNEL_LOWEST_KBPS
    sample_rate = rng.choice(SAMPLE_RATES)
    # Half the cases at 48 kHz at another frame rate than the native one; now and then one at
    # 44.1 kHz, which must be refused.
    roll = rng.random()
    frame_rate_index = 13
    if (sample_rate == 48000 and roll < 0.5) or roll < 0.03:
        frame_rate_index = rng.randrange(13)
    # A frame rate above the native one shares the rate among more frames, and most draws scale
    # the rate with it, so that its frames hold what the native frame rate's would.
    if frame_rate_index != 13 and LOWEST_KBPS <= bitrate <= HIGHEST_KBPS and rng.random() < 0.7:
        fps = Fraction(48000) / FRAME_RATES[frame_rate_index][1]
        bitrate = min(HIGHEST_KBPS, round(bitrate * fps / Fraction(48000, FRAME)))
    options = []
    if frame_rate_index != 13 or rng.random() < 0.1:
        options.append(f"frame-rate={FRAME_RATES[frame_rate_index][0]}")
    roll = rng.random()
    if roll < 0.15:
        options.append("rate-mode=average")
    elif roll < 0.3:
        options.append("rate-mode=variable")
    # I-frames at an interval, at named frames and where fragments start.
    if rng.random() < 0.2:
        options.append(f"iframe-interval={rng.randint(1, 12)}")
    if rng.random() < 0.1:
        named = sorted({rng.randrange(20) for _ in range(rng.randint(1, 3))})
        options.append("iframes=" + ",".join(str(f) for f in named))
    if rng.random() < 0.1:
        options.append(f"fragment={rng.choice(['0.1', '0.25', '0.5', '1.001'])}")
    roll = rng.random()
    if roll < 0.2:
        options.append("dialnorm=auto")
    elif roll < 0.5:
        options.append(f"dialnorm={rng.randint(0, 127) / 4:g}")
    if rng.random() < 0.15:
        options.append(f"loudness={rng.choice(PRACTICES)}")
    tools = []
    # DRC's modes on a profile, some on one of their own, and now and then their gains.
    if rng.random() < 0.25:
        options.append(f"drc={rng.choice(DRC_PROFILES)}")
        options.extend(f"{mode}={rng.choice(DRC_PROFILES)}" for mode in DRC_MODES
                       if rng.random() < 0.25)
        # The immersive element refuses DRC's gains: they name channel groups it has not taken.
        if rng.random() < 0.3 and not immersive:
            tools.append(f"drc-gains-{rng.randrange(4)}")
    # The downmix values, in 5.X and 7.X, the LFE's where there is one.
    if channels >= 5 and rng.random() < 0.3:
        draws = [
            ("lorocmixlev", CENTRE_LEVELS),
            ("lorosurmixlev", SURROUND_LEVELS),
            ("ltrtcmixlev", CENTRE_LEVELS),
            ("ltrtsurmixlev", SURROUND_LEVELS),
            ("dmixmod", DOWNMIX_METHODS),
            ("loro-correction", [f"{x / 2:g}" for x in range(-15, 16)]),
            ("ltrt-correction", [f"{x / 2:g}" for x in range(-15, 16)]),
        ]
        if channels % 2 == 0:
            draws.append(("lfemix", [f"{5.5 - x:g}" for x in range(32)]))
        chosen = [(key, values) for key, values in draws if rng.random() < 0.5]
        if not chosen:
            chosen = [rng.choice(draws)]
        options.extend(f"{key}={rng.choice(values)}" for key, values in chosen)
    # Dialogue enhancement: marked channels, or a stem; the Mid of L and R, and a stem over two
    # or three channels cross-channel.
    stem = False
    if rng.random() < 0.2:
        available = {1: ["c"], 2: ["l", "r"]}.get(channels, ["l", "r", "c"])
        marked = [c for c in available if rng.random() < 0.6] or [rng.choice(available)]
        stem = rng.random() < 0.5
        method = "independent"
        roll = rng.random()
        if roll < 0.25 and marked == ["l", "r"]:
            method = "mid"
        elif roll < 0.5 and stem and len(marked) >= 2:
            method = "cross"
        options.append(f"dialogue-channels={','.join(marked)}")
        options.append(f"dialogue-method={method}")
        options.append(f"dialogue-max-gain={rng.choice([3, 6, 9, 12])}")
    # The rate picks the codec mode (in 5.X ASPX_ACPL_3 and ASPX_ACPL_2 at
    # the lowest rates a channel, then ASPX below 96 kbps a channel); now and
    # then SIMPLE or ASPX is forced, or an A-CPL mode, and the experimental
    # A-SPX tools asked for.
    roll = rng.random()
    acpl = None
    if immersive:
        # The immersive codec modes, and each one's least rate.
        mode = "auto"
        if immersive_rng.random() < 0.4:
            mode = immersive_rng.choice(IMMERSIVE_MODES)
            options.append(f"codec-mode={mode}")
            if mode in IMMERSIVE_TOOLS:
                tools.append(IMMERSIVE_TOOLS[mode])
        if LOWEST_KBPS <= bitrate < IMMERSIVE_LOWEST_KBPS[mode]:
            bitrate = IMMERSIVE_LOWEST_KBPS[mode]
        if immersive_rng.random() < 0.3:
            options.append(f"height-downmix={immersive_rng.choice(HEIGHT_DOWNMIXES)}")
            if immersive_rng.random() < 0.7:
                options.append(f"height-gain={immersive_rng.choice(HEIGHT_GAINS)}")
    elif roll < 0.15:
        options.append("codec-mode=simple")
    elif roll < 0.3:
        options.append("codec-mode=aspx")
    elif roll < 0.42 and channels in (2, 5, 6):
        acpl = rng.choice(ACPL_MODES[channels > 2])
        options.append(f"codec-mode={acpl}")
        if channels > 2 and LOWEST_KBPS <= bitrate < ACPL_LOWEST_KBPS[acpl]:
            bitrate = ACPL_LOWEST_KBPS[acpl]
    if rng.random() < 0.25:
        tools.append(
            rng.choice(
                [
                    "aspx-balance",
                    "aspx-varvar",
                    "aspx-interleave",
                    "aspx-balance,aspx-varvar,aspx-interleave",
                ]
            )
        )
    if acpl is not None and (channels == 2 or acpl == "aspx-acpl-1"):
        tools.append("acpl")
    if channels > 2 and acpl is None and rng.random() < 0.3 and not immersive:
        tools.append("coding-configs")
    if immersive:
        if channels > 10:
            tools.append("back-pair")
    elif channels > 6:
        tools.append(rng.choice(SEVEN_X))
    # A case in five has several substreams and presentations, drawn from a generator of its own so
    # that the other cases, the regression seeds' among them, draw as they did. Its rate holds half
    # of FRAME_BYTES_CAP a substream in each frame, where the rate is in range.
    substreams = []
    presentation_rng = random.Random(seed ^ 0x9E3779B97F4A7C15)
    if presentation_rng.random() < 0.2:
        substreams, stem = draw_presentations(presentation_rng, channels, options, tools, stem)
        if LOWEST_KBPS <= bitrate <= HIGHEST_KBPS:
            per_frame = FRAME_RATES[frame_rate_index][1] if sample_rate == 48000 else FRAME
            least = math.ceil(
                Fraction(FRAME_BYTES_CAP // 2 * (1 + len(substreams)) * 8 * sample_rate)
                / (1000 * per_frame)
            )
            bitrate = min(HIGHEST_KBPS, max(bitrate, least))
    if tools:
        options.append(f"experimental={','.join(tools)}")
    # Two to ten frames of input: several frames, never one, so that block
    # switching and the stereo choice change between frames. A measurement
    # needs more: BS.1770's gating blocks are 400 ms long.
    frame = math.ceil(FRAME_RATES[frame_rate_index][1]) if sample_rate == 48000 else FRAME
    shortest = -(-2 * frame // BLOCK)
    if any(o == "dialnorm=auto" or o.startswith("loudness=") for o in options):
        shortest = max(shortest, -(-sample_rate * 3 // 5 // BLOCK))
    return Case(
        seed=seed,
        channels=channels,
        sample_rate=sample_rate,
        bitrate=bitrate,
        blocks=rng.randint(shortest, max(shortest, -(-10 * frame // BLOCK))),
        pcm16=rng.random() < 0.5,
        audio_profile=rng.choice([*ac3space.AUDIO_PROFILES, "mixed"]),
        correlation=rng.choice(["independent", "identical", "pairs", "inverted"]),
        mp4=rng.random() < 0.3,
        options=options,
        frame_rate_index=frame_rate_index,
        stem=stem,
        substreams=substreams,
    )


def _emdf(rng):
    """An EMDF payload as emdf= takes it: an id from 1, past emdf_payload_id's five bits and their
    escape now and then, and up to eight bytes."""
    payload_id = rng.randint(1, 30) if rng.random() < 0.7 else rng.randint(31, 400)
    return f"{payload_id}:" + "".join(f"{rng.randrange(256):02x}" for _ in range(rng.randrange(9)))


def _dialogue(rng, channels, tools):
    """Hybrid dialogue enhancement's options for the input, substream 1, of `channels`: marked
    channels, or a stem, by each method, with the waveform's share, which a dialogue enhancement
    substream carries; whether a stem gives the dialogue; and the waveform's channels. The
    channel-independent method's waveform has a channel for each marked channel, three being the
    3.0 element's, and the others' one."""
    available = {1: ["c"], 2: ["l", "r"]}.get(channels, ["l", "r", "c"])
    marked = [c for c in available if rng.random() < 0.6] or [rng.choice(available)]
    stem = rng.random() < 0.3
    method = "independent"
    roll = rng.random()
    if roll < 0.25 and marked == ["l", "r"]:
        method = "mid"
    elif roll < 0.5 and stem and len(marked) >= 2:
        method = "cross"
    if method == "independent" and len(marked) == 3:
        tools.append("three-zero")
    options = [
        f"dialogue-channels={','.join(marked)}",
        f"dialogue-method={method}",
        f"dialogue-max-gain={rng.choice([3, 6, 9, 12])}",
        f"dialogue-hybrid={rng.randrange(32) / 31:.4f}",
    ]
    return options, stem, len(marked) if method == "independent" else 1


def draw_presentations(rng, channels, options, tools, stem):
    """Several substreams and the presentations that play them, for a case whose input, substream
    1, has `channels`: the other substreams' channel counts, 0 for a dialogue enhancement
    substream (the waveform of substream 1's hybrid dialogue enhancement, which takes no input),
    with their options and the presentations' added to `options` and the experimental tools they
    need to `tools`; and whether substream 1's dialogue enhancement reads a stem. The first
    presentation plays substream 1 as its main or music and effects audio, and the others' channels
    are ones it has, or a mono one."""
    template = rng.choice(TEMPLATES)
    # dialnorm=auto and loudness= measure one programme, and ac3cli refuses them with several.
    measured = [o for o in options if o == "dialnorm=auto" or o.startswith("loudness=")]
    for option in measured:
        options.remove(option)
    if measured and rng.random() < 0.7:
        options.append(f"dialnorm={rng.randint(0, 127) / 4:g}")

    def companion():
        # Dialogue or associated audio: mono, or channels substream 1 has.
        return rng.choice([1, 1, 2]) if channels >= 2 else 1

    def dialogue_channels():
        # A music and effects presentation's dialogue may also be 3.0, the 3.0 element's L R C.
        if channels >= 5 and rng.random() < 0.15:
            tools.append("three-zero")
            return 3
        return companion()

    music_and_effects = template in ("config0", "config3", "config5")
    subs = []  # (channels, content classifier, or "enhancement")
    presentations = []  # (substreams from 1, configuration or None)
    waveform = 0  # the dialogue enhancement substream's channels
    if template in ("config1", "config4"):
        options[:] = [o for o in options if not o.startswith("dialogue-")]
        dialogue, stem, waveform = _dialogue(rng, channels, tools)
        options.extend(dialogue)
        subs.append((0, "enhancement"))
    if template == "singles":
        for _ in range(rng.randint(1, 2)):
            layout = rng.choice([1, 2])
            subs.append((layout, rng.choice(["main", "main", "emergency", "voice-over"])))
        presentations = [([n], None) for n in range(1, len(subs) + 2)]
        if rng.random() < 0.3:
            presentations.append(([1], None))  # another presentation of the input, below named
    elif template == "config0":
        subs.append((dialogue_channels(), "dialogue"))
        presentations = [([1, 2], 0)]
    elif template == "config1":
        presentations = [([1, 2], 1)]
    elif template == "config2":
        subs.append((companion(), rng.choice(ASSOCIATED)))
        presentations = [([1, 2], 2)]
    elif template == "config3":
        subs.append((dialogue_channels(), "dialogue"))
        subs.append((companion(), rng.choice(ASSOCIATED)))
        presentations = [([1, 2, 3], 3), ([1, 2], 0)]
    elif template == "config4":
        subs.append((companion(), rng.choice(ASSOCIATED)))
        presentations = [([1, 2, 3], 4), ([1, 2], 1)]
    else:
        subs.append((dialogue_channels(), "dialogue"))
        if rng.random() < 0.5:
            subs.append((companion(), rng.choice(ASSOCIATED)))
        members = list(range(1, len(subs) + 2))
        rng.shuffle(members)
        presentations = [(members, 5)]
    if template != "singles" and rng.random() < 0.5:
        presentations.append(([1], None))  # the input's substream alone
    if rng.random() < 0.1:
        presentations.append(([], 6))  # EMDF payloads alone

    # Substream 1: its classifier, which configuration 5 needs, and a language.
    if music_and_effects or rng.random() < 0.7:
        options.append(f"substream1-content={'music-and-effects' if music_and_effects else 'main'}")
        if rng.random() < 0.3:
            options.append(f"substream1-language={rng.choice(LANGUAGES)}")
    if rng.random() < 0.1:
        options.append(f"substream1-emdf={_emdf(rng)}")
    for number, (sub_channels, content) in enumerate(subs, start=2):
        key = f"substream{number}"
        if content == "enhancement":
            options.append(f"{key}-enhances=1")
        else:
            options.append(f"{key}-content={content}")
            if rng.random() < 0.7:
                options.append(f"{key}-language={rng.choice(LANGUAGES)}")
            if rng.random() < 0.2:
                options.append(f"{key}-codec-mode={rng.choice(['auto', 'simple', 'aspx'])}")
        # A share of the rate, which the retry at a higher rate scales with it.
        if rng.random() < 0.15:
            options.append(f"{key}-bitrate-share={rng.uniform(0.2, 0.35):.3f}")
        if content == "dialogue" and rng.random() < 0.4:
            options.append(f"{key}-max-dialogue-gain={rng.choice([3, 6, 9, 12])}")
            if sub_channels <= 2 and rng.random() < 0.5:
                pans = ",".join(f"{rng.randrange(240) * 1.5:g}" for _ in range(sub_channels))
                options.append(f"{key}-pan={pans}")
        if rng.random() < 0.1:
            options.append(f"{key}-emdf={_emdf(rng)}")

    # The presentations: ids for all or none, levels, filters, names, dialnorms, group gains
    # where the syntax sends them (not configuration 1's, or 4's enhancement's, or one group's),
    # the associated audio's mixing values, and payloads.
    audio = [p for p in presentations if p[1] != 6]
    ids = rng.sample(range(512), len(audio)) if rng.random() < 0.5 else None
    associated_mono = any(c in ASSOCIATED and n == 1 for n, c in subs)
    # Each substream's tracks, its channels but an LFE (Part 2 Table 55): md_compat 3 holds 11.
    tracks = {1: channels - (1 if channels in (6, 8, 10, 12) else 0)}
    for number, (sub_channels, content) in enumerate(subs, start=2):
        tracks[number] = waveform if content == "enhancement" else sub_channels
    for number, (members, config) in enumerate(presentations, start=1):
        key = f"presentation{number}"
        if config == 6:
            options.append(f"{key}-config=6")
            options.extend(f"{key}-emdf={_emdf(rng)}" for _ in range(rng.randint(1, 2)))
            continue
        options.append(f"{key}={','.join(str(m) for m in members)}")
        if config is not None:
            options.append(f"{key}-config={config}")
        if ids is not None:
            options.append(f"{key}-id={ids[number - 1]}")
        # The first presentation is decoded at the decoder's default level, enabled.
        roll = rng.random()
        if number > 1 and roll < 0.1:
            options.append(f"{key}-md-compat=7")
        elif roll < 0.25 and sum(tracks[m] for m in members) <= 11:
            options.append(f"{key}-md-compat=3")
        if number > 1 and rng.random() < 0.1:
            options.append(f"{key}-enabled=off")
        if number > 1 and rng.random() < 0.1:
            options.append(f"{key}-pre-virtualized=on")
        if rng.random() < (0.8 if number > 1 and members == [1] else 0.05):
            options.append(f"{key}-name={rng.choice(NAMES)}")
        if rng.random() < 0.2:
            options.append(f"{key}-dialnorm={rng.randint(0, 127) / 4:g}")
        if config in (0, 2, 3, 5) and rng.random() < 0.3:
            gains = [rng.choice(["0", "-0.25", "-3", "-15.5", "off", f"{-rng.randrange(63) / 4:g}"])
                     for _ in members]
            options.append(f"{key}-gains={','.join(gains)}")
        if config in (2, 3, 4) and rng.random() < 0.4:
            for which in ("main-gain", "main-centre-gain", "main-front-gain"):
                if rng.random() < 0.5:
                    scale = rng.choice(["0", "-76.2", "off", f"{-rng.randrange(255) * 0.3:.1f}"])
                    options.append(f"{key}-{which}={scale}")
            if associated_mono and rng.random() < 0.5:
                options.append(f"{key}-associated-pan={rng.randrange(240) * 1.5:g}")
        if rng.random() < 0.1:
            options.append(f"{key}-emdf={_emdf(rng)}")
    return [n for n, _ in subs], stem


def describe(case):
    return (
        f"seed {case.seed}: {case.channels} ch, {case.sample_rate} Hz, {case.bitrate} kbps, "
        f"{case.blocks * BLOCK} samples ({case.audio_profile}, {case.correlation}, "
        f"{'pcm16' if case.pcm16 else 'float'})"
        f"{' ' + ' '.join(case.options) if case.options else ''}"
        f"{', a dialogue stem' if case.stem else ''}"
        f"{', and substreams of ' + str(case.substreams) + ' channels' if case.substreams else ''}"
        f"{', MP4 too' if case.mp4 else ''}"
    )


def _run(argv):
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, check=False)


# --- independent checks ------------------------------------------------------------------------


def crc16(data):
    """Part 2 Annex G.4.2's crc_word: CRC-16, polynomial 0x8005, initial 0, MSB first."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def sync_frames(data):
    """The raw frames of a stream of ac4_syncframe()s, or an error naming where the walk stopped.

    Only the sync word and frame_size are read, and each 0xAC41 frame's crc_word is checked over
    frame_size and the raw frame (Part 2 Annex G.3.1 and G.4.2)."""
    frames = []
    pos = 0
    while pos < len(data):
        if pos + 4 > len(data):
            return None, f"{len(data) - pos} bytes left at {pos}, too few for a sync frame header"
        sync = int.from_bytes(data[pos : pos + 2], "big")
        if sync not in (0xAC40, 0xAC41):
            return None, f"sync word 0x{sync:04X} at byte {pos}"
        size = int.from_bytes(data[pos + 2 : pos + 4], "big")
        header = 4
        if size == 0xFFFF:
            size = int.from_bytes(data[pos + 4 : pos + 7], "big")
            header = 7
        end = pos + header + size + (2 if sync == 0xAC41 else 0)
        if end > len(data):
            return None, f"the frame at byte {pos} runs {end - len(data)} bytes past the end"
        raw = data[pos + header : pos + header + size]
        if sync == 0xAC41:
            stored = int.from_bytes(data[end - 2 : end], "big")
            computed = crc16(data[pos + 2 : pos + header] + raw)
            if stored != computed:
                return (
                    None,
                    f"crc_word 0x{stored:04X} at frame {len(frames)}, computed 0x{computed:04X}",
                )
        frames.append(raw)
        pos = end
    return frames, ""


def read_trace(path):
    """(frame, substream, offset, width, value) per line of a syntax-trace= file."""
    records = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        records.append(tuple(int(x) for x in parts[:5]))
    return records


def python_trace(data):
    diagnostics = []
    records = []
    for frame, substream, _kind, recs in ac4_syntax.walk_stream(data, True, diagnostics):
        records.extend((frame, substream, pos, width, value) for pos, width, value, _ in recs)
    return records, diagnostics


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a, b, strict=False)):
        if x != y:
            return f"record {i}: {x} against {y}"
    if len(a) != len(b):
        return f"{len(a)} records against {len(b)}"
    return ""


def ffprobe_json(ffprobe, args, path):
    result = _run([ffprobe, "-v", "error", *args, "-of", "json", path])
    if result.returncode != 0:
        return None, result.stderr.strip()
    return json.loads(result.stdout), ""


# --- one case ----------------------------------------------------------------------------------


def run_case(cli, ffprobe, case, workdir):
    tmp = Path(tempfile.mkdtemp(prefix=f"case{case.seed}_", dir=workdir))
    try:
        return _run_case(cli, ffprobe, case, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def at_rate(options, kbps):
    """The options as ac3cli takes them at a rate of `kbps`: each substreamN-bitrate-share=, the
    harness's own spelling of a substream's share of the rate, as substreamN-bitrate= in kbps."""
    out = []
    for option in options:
        key, _, value = option.partition("=")
        if key.endswith("-bitrate-share"):
            out.append(f"{key.removesuffix('-share')}={max(1, round(kbps * float(value)))}")
        else:
            out.append(option)
    return out


def _run_case(cli, ffprobe, case, tmp):
    rng = random.Random(case.seed ^ 0x5EED)
    pcm = ac3space.generate_pcm(
        rng, case.channels, case.blocks, case.sample_rate, case.audio_profile, case.correlation
    )
    wav = tmp / "in.wav"
    ac3space.write_wav(wav, pcm, case.sample_rate, case.pcm16)
    options = list(case.options)
    if case.stem:
        # The dialogue in the programme's channels: each channel scaled, some to nothing.
        gains = [rng.choice([0.0, 0.3, 0.7, 1.0]) for _ in range(case.channels)]
        stem = [[x * g for x in channel] for channel, g in zip(pcm, gains, strict=True)]
        ac3space.write_wav(tmp / "stem.wav", stem, case.sample_rate, False)
        options.append(f"dialogue-stem={tmp / 'stem.wav'}")
    # The other substreams' inputs, of the input's rate and length.
    for number, channels in enumerate(case.substreams, start=2):
        if channels:
            other = ac3space.generate_pcm(
                rng, channels, case.blocks, case.sample_rate, case.audio_profile, case.correlation
            )
            ac3space.write_wav(tmp / f"sub{number}.wav", other, case.sample_rate, case.pcm16)
            options.append(f"substream{number}={tmp / f'sub{number}.wav'}")
    stream = tmp / "out.ac4"
    encoded = _run(
        [
            cli,
            "ac4-encode",
            wav,
            stream,
            case.bitrate,
            *at_rate(options, case.bitrate),
            f"syntax-trace={tmp / 'enc.tsv'}",
        ]
    )
    if not case.in_range or not case.frame_rate_valid:
        # A case can be wrong on both counts; either refusal is right for it.
        whys = [
            why
            for why, wrong in (
                ("rate out of range", not case.in_range),
                ("frame rate at 44.1 kHz", not case.frame_rate_valid),
            )
            if wrong
        ]
        refused = [why for why in whys if REFUSALS[why] in encoded.stderr]
        if encoded.returncode != 0 and refused:
            return Result(case, "refused", "encode", refused[0])
        return Result(
            case,
            "fail",
            "encode",
            f"a {' and a '.join(whys)} was not refused (exit {encoded.returncode}): "
            f"{encoded.stderr.strip()}",
        )
    per_frame = Fraction(FRAME)
    if case.sample_rate == 48000:
        per_frame = FRAME_RATES[case.frame_rate_index][1]
    if encoded.returncode != 0:
        if case.measures and REFUSALS["nothing to measure"] in encoded.stderr:
            return Result(case, "refused", "encode", "nothing to measure")
        frame_bytes = Fraction(case.bitrate * 1000 * per_frame, 8 * case.sample_rate)
        cap = FRAME_BYTES_CAP * (1 + len(case.substreams))
        if REFUSALS["least frame"] in encoded.stderr and frame_bytes < cap:
            # A rate whose frames hold no least frame, if frames of FRAME_BYTES_CAP bytes a
            # substream do.
            higher = math.ceil(Fraction(cap * 8 * case.sample_rate, 1000 * per_frame))
            retried = at_rate(options, higher)
            retry = _run([cli, "ac4-encode", wav, tmp / "retry.ac4", higher, "quiet", *retried])
            if retry.returncode == 0:
                return Result(case, "refused", "encode", "frames too small for the least frame")
            # The rate is checked before the loudness is measured, so the retry can meet the
            # input's own refusal.
            if case.measures and REFUSALS["nothing to measure"] in retry.stderr:
                return Result(case, "refused", "encode", "nothing to measure")
            return Result(
                case,
                "fail",
                "encode",
                f"refused at {case.bitrate} kbps and at {higher}: {retry.stderr.strip()}",
            )
        return Result(
            case, "fail", "encode", f"exit {encoded.returncode}: {encoded.stderr.strip()}"
        )
    match = re.search(r"encoded (\d+) AC-4 frames", encoded.stdout)
    lag_match = re.search(r"lags the input by (\d+) samples", encoded.stdout)
    if match is None or lag_match is None:
        return Result(case, "fail", "encode", f"no frame count or lag in: {encoded.stdout.strip()}")
    count = int(match.group(1))
    lag = int(lag_match.group(1))
    # What the frames decode to, frame by frame from sequence_counter 0: floor((f + 1) R) -
    # floor(f R), which add up to floor(count R).
    decoded_length = math.floor(count * per_frame)
    length = case.blocks * BLOCK
    if case.frame_rate_index == 13:
        expected_frames = -(-(length + LAG) // FRAME)
        if lag != LAG or count != expected_frames:
            return Result(
                case,
                "fail",
                "encode",
                f"{count} frames and a lag of {lag}, expected {expected_frames} and {LAG}",
            )
    elif decoded_length < length + lag - 1 or math.floor((count - 2) * per_frame) >= length + lag:
        # The output covers the input at its lag, the converter's delay rounded, with no more
        # than a frame to spare: flush() also codes out what the converters hold.
        return Result(
            case,
            "fail",
            "encode",
            f"{count} frames decode to {decoded_length} samples, for {length} samples at a lag "
            f"of {lag}",
        )

    data = stream.read_bytes()
    raw_frames, why = sync_frames(data)
    if raw_frames is None:
        return Result(case, "fail", "framing", why)
    if len(raw_frames) != count:
        return Result(
            case, "fail", "framing", f"{len(raw_frames)} sync frames, the encoder said {count}"
        )

    # The three traces. With several substreams the decoder reads every one, and decodes the first
    # presentation, whose channels are the input's, at level 7, which takes any number of tracks.
    chosen = ["presentation=0", "md-compat=7"] if case.substreams else []
    decoded = _run(
        [cli, "decode", stream, tmp / "out.wav", f"syntax-trace={tmp / 'dec.tsv'}", *chosen]
    )
    if decoded.returncode != 0:
        return Result(
            case, "fail", "decode", f"exit {decoded.returncode}: {decoded.stderr.strip()}"
        )
    written = read_trace(tmp / "enc.tsv")
    read = read_trace(tmp / "dec.tsv")
    parsed, diagnostics = python_trace(data)
    if diagnostics:
        return Result(case, "fail", "traces", f"ac4_syntax.py: {diagnostics[0]}")
    for label, other in (("the decoder's", read), ("ac4_syntax.py's", parsed)):
        difference = first_difference(written, other)
        if difference:
            return Result(
                case, "fail", "traces", f"the encoder's trace and {label} differ at {difference}"
            )

    # The decode.
    samples, rate = read_wav_shape(tmp / "out.wav")
    if rate != case.sample_rate or samples[0] != case.channels or samples[1] != decoded_length:
        return Result(
            case,
            "fail",
            "decode",
            f"decoded {samples[0]} channels of {samples[1]} samples at {rate} Hz, expected "
            f"{case.channels} of {decoded_length} at {case.sample_rate}",
        )

    # FFmpeg's framing.
    if ffprobe is not None:
        probed, why = ffprobe_json(ffprobe, ["-f", "ac4", "-show_entries", "packet=size"], stream)
        if probed is None:
            return Result(case, "fail", "ffprobe", why)
        sizes = [int(p["size"]) for p in probed.get("packets", [])]
        if sizes != [len(f) for f in raw_frames]:
            return Result(
                case,
                "fail",
                "ffprobe",
                f"{len(sizes)} packets, the first sizes {sizes[:4]}, against {count} frames of "
                f"{[len(f) for f in raw_frames[:4]]}",
            )
        if case.mp4:
            mp4 = tmp / "out.mp4"
            muxed = _run(
                [cli, "ac4-encode", wav, mp4, case.bitrate, *at_rate(options, case.bitrate)]
            )
            if muxed.returncode != 0:
                return Result(
                    case, "fail", "mp4", f"exit {muxed.returncode}: {muxed.stderr.strip()}"
                )
            probed, why = ffprobe_json(
                ffprobe,
                [
                    "-count_packets",
                    "-show_entries",
                    "stream=codec_name,codec_tag_string,sample_rate,nb_read_packets",
                ],
                mp4,
            )
            if probed is None:
                return Result(case, "fail", "mp4", why)
            streams = probed.get("streams", [])
            wanted = {
                "codec_name": "ac4",
                "codec_tag_string": "ac-4",
                "sample_rate": str(case.sample_rate),
                "nb_read_packets": str(count),
            }
            if len(streams) != 1 or any(streams[0].get(k) != v for k, v in wanted.items()):
                return Result(
                    case,
                    "fail",
                    "mp4",
                    f"ffprobe read {streams}, expected one stream with {wanted}",
                )
    return Result(case, "ok")


def read_wav_shape(path):
    """((channels, frames), sample rate) of a WAV, from its fmt and data chunks."""
    blob = Path(path).read_bytes()
    pos = 12
    channels = rate = block = 0
    frames = 0
    while pos + 8 <= len(blob):
        chunk, size = blob[pos : pos + 4], int.from_bytes(blob[pos + 4 : pos + 8], "little")
        if chunk == b"fmt ":
            channels = int.from_bytes(blob[pos + 10 : pos + 12], "little")
            rate = int.from_bytes(blob[pos + 12 : pos + 16], "little")
            block = int.from_bytes(blob[pos + 20 : pos + 22], "little")
        elif chunk == b"data" and block:
            frames = size // block
        pos += 8 + size + (size & 1)
    return (channels, frames), rate


# --- the envelope ------------------------------------------------------------------------------


def check_envelope(cli):
    """The rates the encoder takes, at both sample rates and every layout: in mono and stereo the
    lowest accepted and the one below it refused, in 5.X and 7.X MULTICHANNEL_LOWEST_KBPS accepted,
    and everywhere the highest accepted and the one above it refused; each A-CPL mode likewise,
    from ACPL_LOWEST_KBPS in 5.X; and each immersive layout in each of its codec modes, from
    IMMERSIVE_LOWEST_KBPS."""
    failures = 0
    layouts = [(1, []), (2, []), (5, []), (6, []), *((8, [f"experimental={p}"]) for p in SEVEN_X)]
    for channels in (2, 5, 6):
        for mode in ACPL_MODES[channels > 2]:
            layouts.append((channels, [f"codec-mode={mode}", "experimental=acpl"]))
    for channels in (9, 10, 11, 12):
        for mode in IMMERSIVE_LOWEST_KBPS:
            tools = ([IMMERSIVE_TOOLS[mode]] if mode in IMMERSIVE_TOOLS else []) + (
                ["back-pair"] if channels > 10 else [])
            layouts.append((channels, ([] if mode == "auto" else [f"codec-mode={mode}"])
                            + ([f"experimental={','.join(tools)}"] if tools else [])))
    with tempfile.TemporaryDirectory(prefix="ac4envelope_") as tmp:
        for channels, options in layouts:
            for rate in SAMPLE_RATES:
                wav = Path(tmp) / f"{channels}-{rate}.wav"
                ac3space.write_wav(wav, [[0.0] * (4 * FRAME) for _ in range(channels)], rate, False)
                least = LOWEST_KBPS
                if channels == 2 and rate == 48000 and not options:
                    least = STEREO_48K_LOWEST_KBPS
                lowest = [(least - 1, False), (least, True)]
                if channels > 2:
                    forced = [o.split("=", 1)[1] for o in options if o.startswith("codec-mode=")]
                    least = ACPL_LOWEST_KBPS[forced[0]] if forced else MULTICHANNEL_LOWEST_KBPS
                    if channels > 8:
                        least = IMMERSIVE_LOWEST_KBPS[forced[0] if forced else "auto"]
                    lowest = [(least, True)]
                for kbps, accepted in (*lowest, (HIGHEST_KBPS, True), (HIGHEST_KBPS + 1, False)):
                    result = _run(
                        [cli, "ac4-encode", wav, Path(tmp) / "out.ac4", kbps, "quiet", *options]
                    )
                    ok = (result.returncode == 0) == accepted
                    expected = "accepted" if accepted else "refused"
                    if not accepted:
                        ok = ok and (
                            REFUSALS["rate out of range"] in result.stderr
                            or REFUSALS["least frame"] in result.stderr
                        )
                    print(
                        f"  {channels} ch {' '.join(options)} {rate} Hz {kbps:5d} kbps: "
                        f"{'accepted' if result.returncode == 0 else 'refused'}"
                        f"{'' if ok else '  <- expected ' + expected}"
                    )
                    failures += not ok
    return 1 if failures else 0


# --- main --------------------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--cli",
        default=os.environ.get("AC3CLI", "build/dev/bin/ac3cli.exe"),
        help="path to ac3cli (or set AC3CLI)",
    )
    parser.add_argument("--ffprobe", default="ffprobe", help="path to ffprobe")
    parser.add_argument("--no-ffmpeg", action="store_true", help="skip FFmpeg's framing checks")
    parser.add_argument(
        "--seed", type=int, default=None, help="master seed; drawn and printed if omitted"
    )
    parser.add_argument("--cases", type=int, default=None, help="run exactly this many cases")
    parser.add_argument(
        "--seconds", type=float, default=None, help="run until this many seconds have passed"
    )
    parser.add_argument(
        "--replay", type=int, default=None, help="run the one case with this case seed"
    )
    parser.add_argument(
        "--regressions", action="store_true", help="replay every REGRESSION_SEEDS case"
    )
    parser.add_argument(
        "--check-envelope", action="store_true", help="re-measure the accepted rates"
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--max-failures", type=int, default=10)
    args = parser.parse_args()

    cli = args.cli if Path(args.cli).is_absolute() else str((REPO / args.cli).resolve())
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")
    if args.check_envelope:
        print("acceptance envelope - the rates ac4-encode takes")
        sys.exit(check_envelope(cli))

    ffprobe = None
    if not args.no_ffmpeg:
        ffprobe = shutil.which(args.ffprobe)
        if ffprobe is None:
            raise SystemExit(
                f"ffprobe not found ('{args.ffprobe}'); pass --no-ffmpeg to skip FFmpeg's checks"
            )

    def one(seed):
        case = draw_case(seed)
        print(describe(case))
        with tempfile.TemporaryDirectory(prefix="ac4space_") as workdir:
            result = run_case(cli, ffprobe, case, workdir)
        print(
            f"  {result.status}" + (f" ({result.stage}): {result.detail}" if result.detail else "")
        )
        return result.status != "fail"

    if args.regressions:
        if not REGRESSION_SEEDS:
            print("no regression seeds recorded")
        failed = sum(not one(seed) for seed in REGRESSION_SEEDS)
        sys.exit(1 if failed else 0)
    if args.replay is not None:
        sys.exit(0 if one(args.replay) else 1)

    if args.cases is None and args.seconds is None:
        args.cases = 50
    master = args.seed if args.seed is not None else random.SystemRandom().randrange(2**63)
    print(f"AC-4 encoder-space fuzz: cli={cli} ffprobe={ffprobe or 'off'}")
    print(
        f"master seed {master}"
        + (f", {args.cases} cases" if args.cases is not None else f", {args.seconds:g}s budget")
        + f", {args.jobs} jobs"
    )
    print("(every failure prints its case seed; --replay <seed> reruns it)\n")

    started = time.monotonic()
    counts = {"ok": 0, "refused": 0, "fail": 0}
    failures = []
    index = 0

    def budget_left():
        if args.cases is not None:
            return index < args.cases
        return (time.monotonic() - started) < args.seconds

    with (
        tempfile.TemporaryDirectory(prefix="ac4space_") as workdir,
        concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool,
    ):
        pending = set()
        while (budget_left() or pending) and len(failures) < args.max_failures:
            while budget_left() and len(pending) < args.jobs * 2:
                case = draw_case(ac3space.case_seed(master, index))
                index += 1
                pending.add(pool.submit(run_case, cli, ffprobe, case, workdir))
            if not pending:
                break
            done, pending = concurrent.futures.wait(
                pending, return_when=concurrent.futures.FIRST_COMPLETED
            )
            for future in done:
                result = future.result()
                counts[result.status] += 1
                if result.status == "fail":
                    failures.append(result)
                    print(f"FAIL [{result.stage}] {describe(result.case)}")
                    print(f"  {result.detail}")
                    print(
                        "  replay: python tools/ci/fuzz_ac4_encoder_space.py "
                        f"--replay {result.case.seed}\n"
                    )

    total = sum(counts.values())
    print(
        f"{total} cases in {time.monotonic() - started:.1f}s: {counts['ok']} encoded, read "
        f"and decoded cleanly, {counts['refused']} refused (a rate out of range or too low for "
        f"the frame rate and metadata, a frame rate 44.1 kHz does not have, or no loudness to "
        f"measure), {counts['fail']} failed"
    )
    if counts["fail"]:
        sys.exit(1)
    if total == 0 or counts["ok"] < total * 0.5:
        print("too few cases encoded to call this a pass")
        sys.exit(1)


if __name__ == "__main__":
    main()
