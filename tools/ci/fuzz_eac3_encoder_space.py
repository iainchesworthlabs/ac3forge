"""Property/fuzz harness over the E-AC-3 ENCODER's own input space.

The AC-3 half of this question is tools/ci/fuzz_encoder_space.py, whose own
header named where it stopped and why - "E-AC-3's own space - the Annex E tool
tokens, VBR, the wider layouts - is a real remaining gap, and deliberately not
smuggled in half-covered here".

This file is that gap (roadmap VX1); that note now points here instead. It
asks the same question of
`eac3-encode` and `atmos-encode` that the AC-3 harness asks of `encode`: does
the encoder, driven across its own legal configuration space by adversarial
but perfectly valid audio, ever emit a stream a decoder refuses - or refuse,
crash or mis-frame something it should have encoded?

It shares that file's PCM generator by importing it rather than copying it, so
the adversarial material (the per-256-sample-BLOCK plan, the `cliff` profile
the deltbaie defect needed, the correlation modes) stays one implementation
serving both codecs.

What is different here, and why this is a separate file rather than a --codec
switch on the other one:

  * The configuration space barely overlaps. Annex E tool tokens (cpl, ecpl,
    spx, aht, tpn, their band-edge pins, `auto`), VBR, `fscod2` half sample
    rates, eight layouts including three that need dependent substreams, and
    a second command (`atmos-encode`) with an object count of its own.
  * The ACCEPTANCE envelope is a different shape. AC-3's frmsizcod indexes
    Table 5.18, so its frame size is fixed by the rate pair. E-AC-3 signals
    the size directly in frmsiz, which is 11 bits - so the ceiling is a WORD
    COUNT, and at the half rates an ordinary Table 5.18 bitrate runs past it.
    See CEILING_WORDS.
  * The ORACLE is a different shape, and this is the big one. FFmpeg reads
    AC-3 whole. It does not read E-AC-3 whole, and the parts it cannot read
    are exactly the parts this harness exists to cover.

--- the oracle classes ------------------------------------------------------

docs/verification.md's "Where the oracles don't reach" records what FFmpeg
does and does not read; tools/ci/run_codec_matrix.sh acts on it by SKIPPING
the FFmpeg check for those cells rather than tolerating a failure, which is
the right call for a hand-enumerated matrix. A generator drawing thousands of
configurations needs the distinction to be a first-class part of the verdict
instead, because "clean" has to mean something different in each class. So
every case is classified before it runs, and every stream is then checked as
hard as something external still can:

  full    FFmpeg's strict decode, byte-for-byte the invocation
          run_ffmpeg_check() in run_codec_matrix.sh uses. Available when the
          access unit has at most one dependent substream, no ecpl and no tpn,
          and a normal sample rate.

  header  FFmpeg cannot decode the audio, but the FRAMING is still checked -
          by two independent means, neither of which needs a decode:

            * syncframe_walk(), this file's own walk over the four fields
              that decide E-AC-3's framing (syncword, strmtyp, substreamid,
              frmsiz). No tables, no coding tools, nothing shared with the
              encoder, and it works at EVERY layout - including the ones
              FFmpeg cannot parse at all. This is what makes the class
              genuinely non-empty rather than a polite name for "skipped".
            * ffprobe's own syncframe walk, where FFmpeg can be trusted to do
              one: access-unit count, exact byte tiling, sample rate. Not
              asked for a layout needing two dependent substreams - see
              header_check() for the stream that proved why.

  none    Nothing external and nothing independent says anything at all. This
          class is empty: syncframe_walk() reaches every stream this harness
          can produce. It exists so a future cell that escapes even that is
          reported as such rather than quietly counted as a pass.

Which class a case lands in, and why, is measured against FFmpeg 8.0 rather
than assumed - see ORACLE_GAPS and --check-oracles. Every class still runs the
in-repo `ac3cli decode` round trip; the class only decides what ELSE runs.

The weaker guarantee in the `header` class is stated plainly because it is
real: a misreading of the spec shared by this project's encoder AND its
decoder would survive it, and so would one shared by the encoder and the field
layout syncframe_walk() reads. docs/verification.md says the same thing about
the CI gate that covers ecpl/tpn today. What framing does prove is where a
bit-offset defect - the shape of the deltbaie bug that motivated the AC-3
harness - shows up first: a syncframe whose contents are written at the wrong
offset is a syncframe whose frmsiz no longer lands the next syncword where it
promised.

--- determinism -------------------------------------------------------------

Every case is a pure function of a single 64-bit case seed, so a failure is
reproducible from the one number printed with it - see --replay. The master
seed only decides which case seeds get drawn; it is printed at the start of
every run, failure or not.

Usage (repo root, after building):
  python tools/ci/fuzz_eac3_encoder_space.py --cli build/dev/bin/ac3cli.exe --cases 200
  python tools/ci/fuzz_eac3_encoder_space.py --seconds 120       # bounded, for CI
  python tools/ci/fuzz_eac3_encoder_space.py --check-envelope    # re-measure the tables below
  python tools/ci/fuzz_eac3_encoder_space.py --check-oracles     # re-measure what FFmpeg reads
  python tools/ci/fuzz_eac3_encoder_space.py --replay 1234567890 # one exact case
"""

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

# The AC-3 harness's adversarial PCM generator, imported rather than copied:
# one `cliff`, one block plan, one set of correlation modes for both codecs.
# Only the generator half is reused - the configuration space, the acceptance
# envelope and the oracle model below are this file's own.
import fuzz_encoder_space as ac3space  # noqa: E402  (sys.path set up just above)

BLOCKS_PER_FRAME = ac3space.BLOCKS_PER_FRAME    # §5.3.1
FRAME = ac3space.FRAME
AUDIO_PROFILES = ac3space.AUDIO_PROFILES
DRC_PROFILES = ac3space.DRC_PROFILES
CMIXLEV = ac3space.CMIXLEV
SURMIXLEV = ac3space.SURMIXLEV

# Table 5.18's nominal rates. E-AC-3's frmsiz is not an index into that table -
# §E2.3.1.3 makes it an arbitrary word count, so any rate landing on a legal
# word count is expressible - but the CLI takes a kbps number and this is the
# set every other gate in this repo uses, so it is what gets drawn.
LEGAL_RATES = ac3space.LEGAL_RATES

# §5.4.1.3's three, plus Annex E's `fscod2` half rates (§E2.3.1.5). The half
# rates are most of the reason this harness exists as far as sample rate goes:
# nothing else in the repo encodes real audio at them. run_codec_matrix.sh's
# only WAV source is 48 kHz; eac3-sine and eac3-silence synthesize at 48 kHz
# and have no rate argument; the existing fscod2 coverage is header-level.
SAMPLE_RATES = [48000, 44100, 32000, 24000, 22050, 16000]
FSCOD2_RATES = [24000, 22050, 16000]

# §E2.3.1.3: frmsiz is 11 bits and holds (words - 1), so 2048 words is the
# largest syncframe the format can signal at any rate - ac3::eac3::
# kMaxFrameWords, and the arithmetic below is ac3::eac3::frame_words().
#
# This is not a detail. At 48 kHz the ceiling binds nowhere near the 640 kbit/s
# top of LEGAL_RATES (it would take 1024); at the half rates it binds INSIDE
# the table: the highest expressible rate is 320 kbit/s at 16 kHz, 448 at
# 22.05 kHz and 512 at 24 kHz. Both halves of such a pair are legal on their
# own and nothing in the CLI's grammar marks the combination, so a generator
# drawing rates and sample rates independently walks straight into it - which
# is how this harness's first sweep found that `eac3-encode` met the resulting
# refusal with an assert() rather than a message: a clean but causeless
# refusal in a release build, an abort in any build with assertions live, at
# every layout. Fixed in the change that added this file; REFUSALS' "frmsiz
# ceiling" entry is what that fix now reports, --check-envelope gates it, and
# tests/cli/test_cli.cpp's "[frmsiz]" case pins the message.
#
# Rates above the ceiling are still DRAWN, deliberately: they are legal input
# to a CLI that has to refuse them cleanly, and that refusal is exactly what
# was wrong once already.
CEILING_WORDS = 2048


def frame_words(sample_rate, bitrate_kbps):
    """Words per syncframe - ac3::eac3::frame_words, in Python."""
    return bitrate_kbps * 1000 * FRAME // sample_rate // 16


def over_ceiling(sample_rate, bitrate_kbps):
    return frame_words(sample_rate, bitrate_kbps) > CEILING_WORDS


def highest_expressible(sample_rate):
    """The largest LEGAL_RATES entry frmsiz can signal at this sample rate."""
    fits = [r for r in LEGAL_RATES if not over_ceiling(sample_rate, r)]
    return fits[-1] if fits else None


# The smallest elementary stream this harness will hand FFmpeg, in bytes, and
# the reason is the AC-3 harness's own - see its long note above
# MIN_STREAM_BYTES. FFmpeg auto-detects the container of a raw stream by
# probing, and on a few kilobytes that probe is unreliable in a way that has
# nothing to do with whether the bitstream is valid. The floor keeps this
# harness out of the SIZE regime where the probe is unreliable; run_case()'s
# forced-format arbitration handles the regime where it is OUTRUN.
MIN_STREAM_BYTES = ac3space.MIN_STREAM_BYTES

# Where each layout's usable rate range starts, measured against this build at
# 48 kHz rather than derived from the spec, with exactly the two columns the
# AC-3 harness's LAYOUTS table has and for the same reason: the refusal is
# ac3::eac3::FrameEncoder's own budget check, and its cost moves with how many
# substreams the layout codes, with the Annex E tools, and - through the
# exponent strategy and the delta bit allocation - with the AUDIO. So there is
# no one floor; there are two, and conflating them would make this table
# either a lie or useless.
#
#   robust  every rate from here up encodes whatever you feed it, under every
#           tool set. A refusal at or above this rate is a regression, and
#           --check-envelope asserts it across a spread of deliberately
#           different probe material AND a spread of tool sets.
#   min     the lowest rate this generator DRAWS. Between min and robust the
#           answer genuinely depends on the input and on the tools, so drawing
#           there is real coverage of the most bit-starved corner of the space
#           - it just means some of those cases come back refused rather than
#           encoded. --check-envelope asserts something still encodes there,
#           so a min set below anything reachable shows up instead of silently
#           burning budget on cases that can never run.
#
# 48 kHz is the tightest sample rate and so the one worth tabulating: frame
# bytes are bitrate x 1536 / sample_rate / 8, so a LOWER sample rate buys MORE
# room at the same kbps and a floor measured at 48 kHz is conservative
# everywhere below it. Measured on this build: 5.1's floor is 112 kbit/s at
# 48 kHz but only 56 at 24 kHz and 40 at 16 kHz.
#
# `sources` is the source channel counts each layout is offered. Everything but
# 1+1 folds a wider source down (§7.8) and leaves a narrower one's missing
# channels silent, so any width is legal there; 1+1 routes exactly two channels
# as two independent programmes - a strict identity, never a fold-down - so it
# refuses any other width outright.
LAYOUTS = {
    #             min  robust  source channel counts this layout is offered
    "mono":   {"min": 32,  "robust": 32,  "sources": [1, 2, 6]},
    "stereo": {"min": 32,  "robust": 48,  "sources": [1, 2, 6]},
    "1+1":    {"min": 32,  "robust": 48,  "sources": [2]},
    "51":     {"min": 80,  "robust": 128, "sources": [1, 2, 6, 8]},
    "71":     {"min": 112, "robust": 224, "sources": [6, 8]},
    "512":    {"min": 80,  "robust": 160, "sources": [6, 8]},
    "514":    {"min": 112, "robust": 224, "sources": [6, 8, 10]},
    "714":    {"min": 112, "robust": 256, "sources": [6, 8, 12]},
}

# Layouts by how many dependent substreams they need - the property FFmpeg's
# own limit keys on, not a cosmetic grouping. ff_ac3_parse_header rejects
# substreamid != 0, so a layout needing two dependents has no FFmpeg decode at
# all (docs/verification.md, and run_codec_matrix.sh's own 714 skip).
DEPENDENTS = {"mono": 0, "stereo": 0, "1+1": 0, "51": 0,
              "71": 1, "512": 1, "514": 1, "714": 2}

# Annex E coding tools, as the `[tools]` positional's own grammar spells them
# (plan::kToolsSyntax, docs/cli/metadata-options.md).
#
# `ecpl` is never drawn alone: it "only takes effect alongside cpl", so a lone
# `ecpl` would silently be a `none` case wearing an interesting name, and the
# run summary would then overstate how much enhanced coupling was covered.
TOOL_ATOMS = ["cpl", "spx", "aht", "tpn"]
DMIXMOD = ["ltrt", "loro", "none"]

# Every way a generated configuration can be legitimately refused by
# `eac3-encode` or `atmos-encode`, keyed by the CLI's own exact words on exit
# code 1. Each is correct behaviour meeting a real limit, not a defect, and
# none can be excluded up front - see the per-entry notes.
#
# An allow-list, not a catch-all: any OTHER non-zero exit is a failure. That
# matters more here than in the AC-3 harness, because the first thing this
# file found was an ABORT that a blanket "non-zero means skip" would have
# swallowed whole - see classify().
REFUSALS = {
    # ac3::eac3's own budget check, reported by the CLI whenever an access
    # unit cannot be built at the chosen settings. Two distinct causes reach
    # this one message, both measured against the CLI rather than assumed:
    #
    #   * CBR side information that will not fit. Reachable between a layout's
    #     `min` and `robust` rates because that cost moves with the tools, the
    #     substream count and the audio - see LAYOUTS.
    #   * a VBR quality the content cannot be coded at. The CLI's own vbr help
    #     warns that "bit cost rises steeply above roughly half the range, so
    #     a high quality with no max bound will often refuse real programme
    #     material outright"; measured, q:0.95 over a 5.1 bed with `all` does
    #     exactly that - and reports it in these words, having no message of
    #     its own. So VBR needs no separate entry here, and adding one that
    #     never matches would be worse than none.
    "header room": "the encoder cannot express this configuration",
    # §E2.3.1.3's 11-bit frmsiz word count - see CEILING_WORDS. A legal Table
    # 5.18 rate above what frmsiz can signal at the chosen sample rate, which
    # only happens at the Annex E half rates.
    "frmsiz ceiling": "words per syncframe, past the",
    # atmos-encode's own budget check: the object metadata and the mantissas
    # share one frame. Matched on the ASCII tail of that message, since its
    # head contains an em dash that is not worth making a key depend on.
    "object room": "mantissas share one frame",
    # dialnorm=auto measures BS.1770-4 loudness, whose -70 LKFS absolute gate
    # discards every block below it; material entirely below the gate leaves
    # nothing to measure, and the CLI says so and names the fix rather than
    # inventing a number. This generator deliberately produces near-silent and
    # mostly-silent material (the `transient` profile, and the silent half of
    # every `cliff`), so it meets that limit honestly.
    "sub-gate loudness": "no audio above the -70 LKFS absolute gate",
    # The same limit reached through atmos-encode, which has no one fixed
    # layout to measure a fold-down against for every source width and says so
    # in its own words instead.
    "unmeasurable loudness": "cannot measure loudness for this file",
    # atmos-encode's object-count limit: TS 103 420 §8.3.2.2 caps a programme
    # at 16 including the bed's LFE, so 15 objects is the CLI's own ceiling.
    # Reached by the 16-channel source width drawn below, whose default
    # "one object per source channel" asks for exactly one too many - a legal
    # WAV that has to be refused rather than silently truncated. Naming an
    # explicit count of 15 or fewer for the same file still encodes.
    "object count": "objects (the bed's LFE is the 16th",
}

# Case seeds that once produced a wrong verdict from this harness or from the
# encoder, kept replayable so neither failure mode comes back silently.
# --regressions runs every entry and fails on any case whose status is
# "fail"; ci.yml runs that before the unseeded search. This is the
# encoder-space counterpart of fuzz/regressions/ (which holds DECODER inputs)
# - a case seed regenerates its whole input from one number, so the number is
# the artifact.
# Mostly PINNED rather than harvested, unlike the AC-3 harness's list: the
# defects below were found by unseeded sweeps, which by construction do not
# reuse a seed, so most entries are the smallest case seed that redraws the
# same shape. That is the useful property either way - what a regression seed
# has to do is reproduce, not be the literal number that failed first. (The
# 7.1.4 entry is the exception: that one IS the seed that failed.)
#
# MAINTENANCE: a case is a pure function of its seed AND of draw_case, so any
# change to the generator - a new option, a reweighting, one more entry in a
# choice list - silently redraws every seed here into a different case. Two of
# these entries stopped testing what their description claimed exactly that
# way while this file was being written. Re-run --regressions after touching
# draw_case and check each case still MATCHES its description, not merely that
# the run exits 0: a seed that has drifted into an unrelated configuration
# passes quietly.
REGRESSION_SEEDS = {
    25: "16 kHz at 384 kbit/s (mono, cpl): a legal Table 5.18 rate at a legal "
        "Annex E half rate that no syncframe can signal, because frmsiz is 11 "
        "bits. Must classify as refused ('frmsiz ceiling') - it was an abort "
        "before the fix, and an abort is not a refusal",
    24: "the same ceiling at 22.05 kHz (1+1 at 640 kbit/s), where the cut "
        "falls two Table 5.18 steps higher than at 16 kHz",
    412: "and at 24 kHz (mono at 640 kbit/s), the highest of the three cuts. "
         "All three are pinned because the ceiling is derived per sample "
         "rate, not tabulated, so one of them passing proves little about "
         "the others",
    4765573204069690189:
        "7.1.4 under VBR at 32 kHz: a stream this harness first reported as a "
        "framing failure and which turned out to be correct - 54 syncframes, "
        "18 access units of 1536 bytes, tiling the file exactly. FFmpeg's "
        "demuxer lost sync inside the second dependent substream it cannot "
        "parse and resynced mid-frame, reporting 19 packets split 1329/207 at "
        "an offset that is not a syncframe boundary. Must come back clean: it "
        "pins that ffprobe is no longer asked about a two-dependent layout, "
        "and that the independent walk still is",
    59: "a 16-channel source handed to atmos-encode with no explicit object "
        "count, so its default one-object-per-channel asks for one more than "
        "TS 103 420 8.3.2.2 allows. A legal WAV, so the refusal is the "
        "behaviour under test - must classify as refused ('object count')",
}


# --- configuration ----------------------------------------------------------

@dataclass
class Case:
    seed: int
    command: str            # "eac3-encode" | "atmos-encode"
    layout: str             # "" for atmos-encode, which has no layout argument
    bitrate: int
    sample_rate: int
    source_channels: int
    frames: int
    pcm16: bool
    audio_profile: str
    correlation: str
    tools: str = "none"
    vbr: str = "off"
    objects: int = 0        # atmos-encode only; 0 means "one per source channel"
    options: list = field(default_factory=list)

    def cli_args(self, wav, out):
        if self.command == "atmos-encode":
            # <in.wav> <out.ec3> [bitrate_kbps] [objects]
            return ["atmos-encode", str(wav), str(out), str(self.bitrate),
                    str(self.objects), *self.options]
        # <in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr]
        return ["eac3-encode", str(wav), str(out), str(self.bitrate), self.tools,
                self.layout, self.vbr, *self.options]


def draw_tools(rng):
    """A legal `[tools]` token, drawn to cover the Annex E space evenly.

    The whole-set tokens are drawn as often as hand-built combinations because
    they are what real callers type and what run_codec_matrix.sh reports on.
    `auto` in particular is the rate policy choosing for itself, so what it
    selects moves with the bitrate and the layout this case already drew,
    which no fixed combination reaches."""
    roll = rng.random()
    if roll < 0.12:
        return "none"
    if roll < 0.24:
        return "all"
    if roll < 0.36:
        # A caller pinning one band edge while leaving the on/off choice to
        # the rate policy is the other half of `auto`'s decision.
        if rng.random() < 0.3:
            return f"auto+spx:{rng.randint(0, 7)}"
        return "auto"

    # tpn is drawn less often than the other three: it is the one atom that
    # costs the case FFmpeg's decode outright, so at an equal rate it would
    # quietly halve how much of this space gets the stronger oracle.
    picked = [atom for atom in TOOL_ATOMS
              if rng.random() < (0.2 if atom == "tpn" else 0.45)]
    if not picked:
        picked = [rng.choice(TOOL_ATOMS)]
    tokens = []
    for atom in picked:
        if atom == "cpl":
            # §E3.4's cplbegf, 0..15. Pinned half the time; `cpl` alone lets
            # the encoder's own frequency policy choose.
            tokens.append(f"cpl:{rng.randint(0, 15)}" if rng.random() < 0.5 else "cpl")
            # Enhanced coupling (§E3.5) rides on standard coupling: a
            # different way of coding the same band, not a different band.
            # Kept below the other sub-draws because ecpl, like tpn just
            # below, costs the case FFmpeg's decode entirely - see the sample
            # rate weights in draw_case for the same trade.
            if rng.random() < 0.25:
                tokens.append("ecpl")
        elif atom == "spx":
            # §E3.6's spxbegf, 0..7.
            tokens.append(f"spx:{rng.randint(0, 7)}" if rng.random() < 0.5 else "spx")
            # The SPX notch depth, which tunes spx rather than turning it on.
            if rng.random() < 0.3:
                tokens.append(f"atten:{rng.randint(0, 3)}" if rng.random() < 0.5 else "noatten")
        elif atom == "aht":
            # aht:N pins the GAQ mode; aht:0 is AHT with GAQ switched OFF,
            # which is a genuinely different mantissa path, not a disabled
            # tool.
            tokens.append(f"aht:{rng.randint(0, 2)}" if rng.random() < 0.5 else "aht")
        else:
            tokens.append(atom)
    # Not a coding tool - nothing in the bitstream's syntax changes - but it
    # is the direct §8.2.3.2 forward MDCT rather than the default §7.9.4 fast
    # path, so every downstream decision sees a different set of coefficient
    # roundings. `none` and `all` deliberately leave it alone, so this is the
    # only place it can be drawn.
    if rng.random() < 0.12:
        tokens.append("nofastmdct")
    return "+".join(tokens)


def draw_vbr(rng):
    """`off`, or a q: setting with optional min:/max: bounds."""
    if rng.random() < 0.72:
        return "off"
    quality = round(rng.uniform(0.05, 0.95), 3)
    spec = f"q:{quality}"
    # Bounds are drawn so that max-alone, min-alone, both and neither are all
    # reachable, and always ordered so the pair itself is legal - an inverted
    # pair is a CLI-parse question, covered by unit tests, not an
    # encoder-input-space one.
    roll = rng.random()
    if roll < 0.30:
        spec += f",max:{rng.choice([128, 192, 256, 320, 448, 640])}"
    elif roll < 0.45:
        spec += f",min:{rng.choice([32, 64, 96])}"
    elif roll < 0.60:
        low = rng.choice([32, 64, 96])
        spec += f",min:{low},max:{rng.choice([r for r in [128, 192, 256, 320, 448] if r > low])}"
    return spec


def draw_case(seed):
    """A case is a pure function of its seed - the whole point of --replay."""
    rng = random.Random(seed)

    # atmos-encode is a quarter of the draw rather than half: it has a much
    # smaller configuration space of its own (an object count and the shared
    # metadata options - no layout, no tools, no VBR), so an even split would
    # spend half the budget redrawing the same few shapes.
    command = "atmos-encode" if rng.random() < 0.25 else "eac3-encode"

    sample_rate = rng.choices(
        SAMPLE_RATES,
        # These weights are the harness's one real budget decision, because
        # sample rate alone decides whether a case gets FFmpeg's strict decode
        # at all: `fscod2` audio is refused outright, so every half-rate case
        # falls back to the framing oracle.
        #
        # The half rates are still weighted up against a per-entry even split
        # - they are the part of the space nothing else in the repo encodes
        # real audio at, and where frmsiz's ceiling bites - but not so far up
        # that the strongest check available stops running. At 11:5 they take
        # about 31% of the draw, which lands the run near an even split
        # between the two oracle classes once ecpl, tpn and 7.1.4 have taken
        # their own share. Measured before this was tuned, an even per-entry
        # split put HALF the budget beyond FFmpeg's decode on sample rate
        # alone, and a smoke run of six cases got no full-oracle case at all.
        weights=[6.0, 2.5, 2.5, 1.5, 1.5, 1.5], k=1)[0]

    if command == "atmos-encode":
        layout = ""
        # The bed is 5.1 whatever the source is; the object count is what
        # varies. TS 103 420 §8.3.2.2 caps a programme at 16 including the
        # bed's LFE, so the CLI's own limit is 15. 0 means "one per source
        # channel", which is the default a caller gets by omitting it.
        # 16 is deliberately past the cap: TS 103 420 §8.3.2.2 allows 16
        # including the bed's LFE, so a 16-channel source asking for its
        # default one-object-per-channel asks for one too many. It is a legal
        # WAV, so that refusal is behaviour under test - see REFUSALS'
        # "object count".
        source_channels = rng.choice([1, 2, 6, 8, 10, 12, 15, 16])
        objects = rng.choice([0, rng.randint(1, 15)])
        floor = 96
    else:
        layout = rng.choice(list(LAYOUTS))
        source_channels = rng.choice(LAYOUTS[layout]["sources"])
        objects = 0
        floor = LAYOUTS[layout]["min"]

    rates = [r for r in LEGAL_RATES if r >= floor]
    # Weighted low, exactly as the AC-3 harness weights and for the same
    # reason: the side-information budget, the exponent strategy and the delta
    # bit allocation are all under most pressure at the bottom of the range.
    # The top still gets drawn, just less often - and at a half sample rate
    # the top is where frmsiz's ceiling is, so it has to keep getting drawn.
    weights = [1.0 / (1.0 + i) for i in range(len(rates))]
    bitrate = rng.choices(rates, weights=weights, k=1)[0]

    options = []
    if rng.random() < 0.5:
        options.append(f"drc={rng.choice(DRC_PROFILES)}")
    if rng.random() < 0.25:
        options.append("heavy")
        if rng.random() < 0.5:
            options.append(f"ceiling={rng.choice(['-0.1', '-0.5', '-1.0', '-3.0'])}")
        if rng.random() < 0.5:
            options.append(f"dialogue={rng.choice(['-31', '-24', '-20', '-14'])}")
    roll = rng.random()
    if roll < 0.2:
        options.append("dialnorm=auto")
    elif roll < 0.4:
        options.append(f"dialnorm={rng.randint(1, 31)}")
    if rng.random() < 0.25:
        options.append(f"cmixlev={rng.choice(CMIXLEV)}")
    if rng.random() < 0.25:
        options.append(f"surmixlev={rng.choice(SURMIXLEV)}")

    tools = "none"
    vbr = "off"
    if command == "eac3-encode":
        tools = draw_tools(rng)
        vbr = draw_vbr(rng)
        # Annex E's own metadata group (Table E1.2) and the E-AC-3-only mix
        # fields: side information competing for the same frame the mantissas
        # are in, so it belongs in a rate-pressure search.
        if rng.random() < 0.3:
            options.append("mixmeta")
        if rng.random() < 0.2:
            options.append(f"lfemix={rng.choice([str(rng.randint(0, 31)), 'off'])}")
        if rng.random() < 0.2:
            options.append(f"dmixmod={rng.choice(DMIXMOD)}")
        if layout == "1+1":
            # The second programme's own metadata, which is NOT inherited from
            # the first: an encoder that quietly applied one programme's
            # settings to both would still pass every check above.
            if rng.random() < 0.5:
                options.append(f"drc2={rng.choice(DRC_PROFILES)}")
            if rng.random() < 0.3:
                options.append("heavy2")
            if rng.random() < 0.3:
                options.append(f"dialnorm2={rng.choice(['auto', str(rng.randint(1, 31))])}")
    else:
        # atmos-encode honours fast-mdct=off but has no [tools] positional to
        # reach the same field through, so this is its only spelling.
        if rng.random() < 0.15:
            options.append("fast-mdct=off")

    frames = frames_for(bitrate, sample_rate, rng.randint(6, 24))

    return Case(
        seed=seed,
        command=command,
        layout=layout,
        bitrate=bitrate,
        sample_rate=sample_rate,
        source_channels=source_channels,
        frames=frames,
        pcm16=rng.random() < 0.7,
        audio_profile=rng.choices(
            AUDIO_PROFILES + ["mixed"],
            # `cliff` is the shape the AC-3 defect needed and the one that
            # moves an exponent strategy part-way through a frame, so it is
            # drawn more often than an even split would give it - without
            # crowding out the profiles that would find a DIFFERENT shape's
            # bug.
            weights=[1.0, 1.0, 2.5, 1.5, 1.0, 0.5, 1.0], k=1)[0],
        correlation=rng.choice(["independent", "identical", "pairs", "inverted"]),
        tools=tools,
        vbr=vbr,
        objects=objects,
        options=options,
    )


def frames_for(bitrate, sample_rate, wanted):
    """`wanted` frames, or however many it takes to clear MIN_STREAM_BYTES.

    CBR frame size is fixed by the rate pair: bitrate x 1536 samples / sample
    rate, in bytes. Under VBR the content decides and the result is smaller
    than the nominal rate would give, so this over-counts there rather than
    under - the safe direction for a size floor."""
    frame_bytes = bitrate * 1000 * FRAME / sample_rate / 8
    return max(wanted, math.ceil(MIN_STREAM_BYTES / frame_bytes))


def case_seed(master, index):
    digest = hashlib.sha256(f"eac3:{master}:{index}".encode()).hexdigest()
    return int(digest[:16], 16)


# --- the oracle model -------------------------------------------------------
#
# Every entry names something FFmpeg 8.0 cannot decode, with the reason it
# cannot. Measured against this build's own streams rather than assumed - the
# measurements are in the notes, and --check-oracles re-runs them.

ORACLE_GAPS = {
    # ff_ac3_parse_header rejects substreamid != 0, which is exactly what a
    # SECOND dependent substream is. Measured: `ffmpeg -xerror` refuses a
    # 7.1.4 stream even with `-f eac3` forced, at every tool set and both
    # sample-rate families - while ffprobe still walks all 63 access units of
    # the same file and its packet sizes still tile it exactly. The decode is
    # gone; the framing is not.
    "two dependent substreams": lambda case: DEPENDENTS.get(case.layout, 0) >= 2,
    # FFmpeg's Annex E parser was never written to read enhanced coupling or
    # transient pre-noise processing. It therefore does NOT refuse these
    # streams - measured: `-xerror` exits 0 on both - which makes a clean
    # decode meaningless rather than merely unavailable, and is why this is a
    # skip rather than a tolerated failure. docs/verification.md says the same.
    "enhanced coupling (ecpl)": lambda case: "ecpl" in case.tools.split("+"),
    "transient pre-noise (tpn)": lambda case: "tpn" in case.tools.split("+"),
    # "Not yet implemented in FFmpeg, patches welcome" - and not in Dolby's
    # own Reference Player either (docs/verification.md). Measured: the decode
    # is refused at 24 kHz and 22.05 kHz, while ffprobe reports the exact
    # right sample_rate and access-unit count for both.
    "fscod2 half rate": lambda case: case.sample_rate in FSCOD2_RATES,
}


def oracle_for(case):
    """(class, reasons) - see this file's header for what each class means."""
    reasons = [name for name, applies in ORACLE_GAPS.items() if applies(case)]
    if not reasons:
        return "full", []
    return "header", reasons


# --- running one case -------------------------------------------------------

@dataclass
class Result:
    case: Case
    status: str          # "ok" | "refused" | "misprobed" | "no-oracle" | "fail"
    detail: str = ""
    stage: str = ""
    reason: str = ""     # which REFUSALS entry matched, when status == "refused"
    oracle: str = ""     # which oracle class this case fell into
    gaps: list = field(default_factory=list)


def _run(argv):
    return subprocess.run(argv, capture_output=True, text=True)


def ffmpeg_check(ffmpeg, path, forced=False):
    """FFmpeg's strict decode, byte-for-byte the invocation
    run_ffmpeg_check() in tools/ci/run_codec_matrix.sh uses.

    -xerror is not belt-and-braces: -err_detect alone only changes what the
    decoder treats as an error internally, concealing a bad frame and moving
    on with exit code 0. -xerror is what turns a detected error into a failing
    process.

    forced=True adds `-f eac3`, taking container auto-detection out of the
    invocation while keeping every error check in it. It is the arbiter run
    when the default invocation refuses a stream: a stream that then decodes
    cleanly was turned away by the PROBE, not the decoder."""
    argv = [ffmpeg, "-v", "error", "-xerror", "-err_detect",
            "crccheck+bitstream+buffer+explode"]
    if forced:
        argv += ["-f", "eac3"]
    return _run(argv + ["-i", str(path), "-f", "null", "-"])


def syncframe_walk(path, expected_units):
    """The framing oracle that does not depend on FFmpeg at all.

    Four fields decide E-AC-3's framing, and all four sit at fixed bit offsets
    immediately after the syncword, before anything a coding tool can change:

        syncword   16 bits, 0x0B77   (§E2.3.1.1)
        strmtyp     2 bits           (§E2.3.1.2: 0 independent, 1 dependent)
        substreamid 3 bits           (§E2.3.1.2)
        frmsiz     11 bits           (§E2.3.1.3: words - 1)

    So a walk over them needs no bit allocation, no exponent strategy, no
    tables - and shares nothing with the encoder that produced the stream. If
    a frmsiz does not describe its own syncframe, the very next read lands on
    something that is not a syncword and this says so. That is exactly the
    failure a bit-offset defect produces, and it is checked here at EVERY
    layout, including the ones FFmpeg cannot read at all.

    Written after ffprobe was caught disagreeing about a stream that turned
    out to be correct - see ORACLE_GAPS' "two dependent substreams" entry.

    Returns None when everything holds, or a string naming what did not."""
    data = path.read_bytes()
    offset = 0
    units = 0
    frames = 0
    previous_dependent = -1
    while offset < len(data):
        if offset + 5 > len(data):
            return (f"{len(data) - offset} trailing bytes at offset {offset} are too short to "
                    f"be a syncframe")
        if data[offset] != 0x0B or data[offset + 1] != 0x77:
            return (f"no syncword at offset {offset}, where the previous frmsiz said the next "
                    f"syncframe starts ({frames} syncframes in, {units} access units)")
        word = (data[offset + 2] << 8) | data[offset + 3]
        strmtyp = (word >> 14) & 0x3
        substreamid = (word >> 11) & 0x7
        size = ((word & 0x7FF) + 1) * 2
        if strmtyp == 0:
            units += 1
            previous_dependent = -1
        elif strmtyp == 1:
            if units == 0:
                return f"a dependent substream at offset {offset} precedes any independent one"
            # §E2.3.1.2 numbers an independent substream's dependents from 0
            # upwards in transmission order; a gap or a repeat would leave a
            # decoder unable to tell which bed channels a dependent replaces.
            if substreamid != previous_dependent + 1:
                return (f"dependent substreamid {substreamid} at offset {offset} does not "
                        f"follow {previous_dependent}")
            previous_dependent = substreamid
        else:
            return (f"strmtyp {strmtyp} at offset {offset} is neither independent nor "
                    f"dependent (0x2 needs a branch this encoder does not write, 0x3 is "
                    f"reserved)")
        frames += 1
        offset += size
    if offset != len(data):
        return f"the last frmsiz runs {offset - len(data)} bytes past the end of the stream"
    if units == 0:
        return "no independent substream anywhere in the stream"
    if expected_units is not None and units != expected_units:
        return (f"the syncframes make {units} access units; the encoder reported writing "
                f"{expected_units}")
    return None


def header_check(ffprobe, path, case, expected_units):
    """Third-party agreement on the framing, from FFmpeg's own demuxer.

    This is the weaker of the two framing checks and the one with a limit, so
    syncframe_walk() above runs first and unconditionally; this one only adds
    what an outside implementation can confirm.

    `-f eac3` is forced rather than arbitrated the way the full decode
    arbitrates. The point here is the syncframe walk, and letting container
    auto-detection have an opinion would only add a second, unrelated way for
    it to fail - the misprobe question belongs to the decode path, which keeps
    run_codec_matrix.sh's exact invocation precisely so that it still means
    what that script means.

    Three assertions:

      1. ffprobe reads the stream at all.
      2. It finds exactly the access units the encoder says it wrote, and
         their packet sizes tile the file EXACTLY - sum == file size, no slack.
      3. The sample rate reads back as encoded.

    NOT RUN at all for a layout needing two or more dependent substreams. That
    is not caution, it is measurement: `ff_ac3_parse_header` rejects
    `substreamid != 0`, so inside a 7.1.4 access unit FFmpeg's demuxer has a
    substream it cannot parse, and it can lose sync there and resync on an
    ordinary byte pattern. Caught doing exactly that on case seed
    4765573204069690189 - an 18-access-unit stream reported as 19 packets,
    split 1329/207 at an offset that is not a syncframe boundary at all, while
    the independent walk above found all 54 syncframes forming 18 units of
    1536 bytes each, tiling the file exactly. Its sample_rate is unusable
    there for the same reason (it reports 0, having stopped at the substream
    it will not parse). Asserting any of it would be asserting FFmpeg's
    limitation rather than the stream."""
    if DEPENDENTS.get(case.layout, 0) >= 2:
        return None
    result = _run([ffprobe, "-v", "error", "-f", "eac3", "-select_streams", "a:0",
                   "-show_entries", "stream=sample_rate", "-show_packets",
                   "-of", "json", str(path)])
    if result.returncode != 0:
        return (f"ffprobe exited {result.returncode} on a stream it should still be able to "
                f"walk\n{result.stderr.strip()}")
    try:
        parsed = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        return f"ffprobe wrote JSON that will not parse: {exc}"

    packets = parsed.get("packets", [])
    if not packets:
        return "ffprobe found no packets at all in a stream that has access units"

    total = 0
    for packet in packets:
        try:
            total += int(packet["size"])
        except (KeyError, TypeError, ValueError):
            return f"ffprobe reported a packet with no usable size: {packet!r}"
    size = path.stat().st_size
    if total != size:
        return (f"ffprobe's packets do not tile the stream: {len(packets)} packets summing to "
                f"{total} bytes over a {size}-byte file")
    if expected_units is not None and len(packets) != expected_units:
        return (f"ffprobe found {len(packets)} access units; the encoder reported writing "
                f"{expected_units}")

    streams = parsed.get("streams", [])
    reported = streams[0].get("sample_rate") if streams else None
    if str(reported) != str(case.sample_rate):
        return (f"ffprobe reports sample_rate {reported}; the stream was encoded at "
                f"{case.sample_rate}")
    return None


def units_written(output):
    """The access-unit count the CLI reports, for the framing check to hold
    FFmpeg against. Parsed rather than computed so it is the ENCODER's own
    claim being checked against an external walk, not this file's arithmetic
    agreeing with itself."""
    marker = " E-AC-3 access units"
    for line in output.splitlines():
        if line.startswith("encoded ") and marker in line:
            try:
                return int(line[len("encoded "):line.index(marker)])
            except ValueError:
                return None
    return None


def classify(case, encode, out_path):
    """Why a non-zero encode is not automatically a failure - and why almost
    every non-zero encode still is. See REFUSALS for the recognised refusals
    and why none can be excluded up front; everything else, including a zero
    exit that wrote nothing and any exit code that is not 0 or 1, is a
    failure.

    The exit-code check is not pedantry. The first defect this harness found
    was an assert() firing on a legal-looking configuration - an abort, which
    subprocess reports as 3 or 0x80000003 on Windows and as -6 (SIGABRT) on
    Linux, never as 1. A plain `!= 0` test with a message allow-list behind it
    would have reported that as a tolerated refusal the moment the abort
    happened to print a matching line first."""
    if encode.returncode != 0:
        if encode.returncode == 1:
            for reason, message in REFUSALS.items():
                if message in encode.stderr:
                    return Result(case, "refused", encode.stderr.strip(), "encode", reason)
            return Result(case, "fail",
                          f"encode exited 1 with an unrecognised error\n{encode.stderr.strip()}",
                          "encode")
        return Result(case, "fail",
                      f"encode exited {encode.returncode} - not a refusal (exit 1) but a "
                      f"crash or abort\n{encode.stderr.strip()}", "encode")
    if not out_path.exists() or out_path.stat().st_size == 0:
        return Result(case, "fail", "encode exited 0 but wrote no bitstream", "encode")
    return Result(case, "ok")


def run_case(cli, ffmpeg, ffprobe, case, workdir, artifacts):
    tmp = Path(tempfile.mkdtemp(prefix=f"eac3space_{case.seed:016x}_", dir=workdir))
    oracle, gaps = oracle_for(case)
    try:
        wav = tmp / "in.wav"
        out = tmp / "out.ec3"
        decoded = tmp / "out.wav"

        data = ac3space.generate_pcm(random.Random(case.seed ^ 0x5EAC3), case.source_channels,
                                     case.frames * BLOCKS_PER_FRAME, case.sample_rate,
                                     case.audio_profile, case.correlation)
        ac3space.write_wav(wav, data, case.sample_rate, case.pcm16)

        encode = _run([cli, *case.cli_args(wav, out)])
        result = classify(case, encode, out)
        result.oracle = oracle
        result.gaps = gaps
        if result.status != "ok":
            if result.status == "fail":
                save_artifacts(artifacts, case, result, tmp)
            return result

        # The in-repo decoder runs at every oracle class: it is the one that
        # reads every Annex E tool at every layout, and the only check the
        # `header` class has on the samples themselves.
        decode = _run([cli, "decode", str(out), str(decoded)])
        if decode.returncode != 0:
            result = Result(case, "fail",
                            f"ac3cli decode exited {decode.returncode}\n"
                            f"{decode.stderr.strip()}", "decode", oracle=oracle, gaps=gaps)
            save_artifacts(artifacts, case, result, tmp)
            return result
        if not decoded.exists() or decoded.stat().st_size == 0:
            result = Result(case, "fail", "decode exited 0 but wrote no PCM", "decode",
                            oracle=oracle, gaps=gaps)
            save_artifacts(artifacts, case, result, tmp)
            return result

        # The framing oracles, at every class including `full` - both are
        # cheap, and a stream whose audio decodes can still carry a frmsiz
        # that does not describe it.
        #
        # The independent walk runs first and always, FFmpeg or not: it is the
        # one that works at every layout, and the only framing check 7.1.4
        # gets at all.
        claimed = units_written(encode.stdout + encode.stderr)
        problem = syncframe_walk(out, claimed)
        if problem is None and ffprobe is not None:
            problem = header_check(ffprobe, out, case, claimed)
        if problem is not None:
            result = Result(case, "fail", problem, "framing", oracle=oracle, gaps=gaps)
            save_artifacts(artifacts, case, result, tmp)
            return result

        if oracle != "full":
            return Result(case, "no-oracle", ", ".join(gaps), "ffmpeg",
                          oracle=oracle, gaps=gaps)

        if ffmpeg is not None:
            check = ffmpeg_check(ffmpeg, out)
            if check.returncode != 0:
                # Not a verdict yet: auto-detection can hand a valid stream to
                # the wrong demuxer. `-f eac3` with the same error checks is
                # what actually asks the decoder.
                forced = ffmpeg_check(ffmpeg, out, forced=True)
                if forced.returncode == 0:
                    first = check.stderr.strip().splitlines()
                    return Result(case, "misprobed", first[0] if first else "", "ffmpeg",
                                  oracle=oracle, gaps=gaps)
                result = Result(case, "fail",
                                f"ffmpeg strict decode exited {forced.returncode} "
                                f"even with -f eac3 forced\n{forced.stderr.strip()}",
                                "ffmpeg", oracle=oracle, gaps=gaps)
                save_artifacts(artifacts, case, result, tmp)
                return result

        return Result(case, "ok", oracle=oracle, gaps=gaps)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def save_artifacts(artifacts, case, result, tmp):
    if artifacts is None:
        return
    dest = artifacts / f"{case.seed:016x}"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("in.wav", "out.ec3", "out.wav"):
        source = tmp / name
        if source.exists():
            shutil.copy2(source, dest / name)
    (dest / "case.json").write_text(json.dumps(asdict(case), indent=2) + "\n")
    (dest / "failure.txt").write_text(
        f"{describe(case)}\n\nstage: {result.stage}\noracle: {result.oracle}"
        + (f" (gaps: {', '.join(result.gaps)})" if result.gaps else "")
        + f"\n\n{result.detail}\n\n{repro(case)}\n")


def describe(case):
    head = (f"seed={case.seed} {case.command} "
            + (f"layout={case.layout} " if case.layout else f"objects={case.objects} "))
    return (head + f"bitrate={case.bitrate} rate={case.sample_rate} "
            f"src_ch={case.source_channels} frames={case.frames} "
            f"fmt={'pcm16' if case.pcm16 else 'float32'} "
            f"audio={case.audio_profile}/{case.correlation} "
            + (f"tools={case.tools} vbr={case.vbr} " if case.command == "eac3-encode" else "")
            + f"options=[{' '.join(case.options)}]")


def repro(case):
    return (f"regenerate:  python tools/ci/fuzz_eac3_encoder_space.py --replay {case.seed}\n"
            f"the encode:  ac3cli {' '.join(case.cli_args('in.wav', 'out.ec3'))}")


# --- the acceptance envelope this generator draws from ----------------------

# Deliberately different shapes rather than N draws of one - the whole reason
# `robust` and `min` differ is that the floor moves with the material, so a
# probe set that agreed with itself would measure nothing.
ENVELOPE_PROBES = ["steady", "chaotic", "cliff", "spectral_jump", "transient", "runs"]

# ...and deliberately different TOOL SETS, which the AC-3 harness's envelope
# has no equivalent of. The Annex E tools are side information competing for
# the same frame the mantissas are in, so a floor measured with tools off is
# not the floor: `all` over a 5.1 bed at 64 kbit/s cannot hold the side
# information at all, and tests/cli/test_cli.cpp already relies on that.
ENVELOPE_TOOLS = ["none", "all", "auto", "cpl+ecpl", "tpn"]


def _envelope_probes(tmp, channels, tag, offset):
    probes = []
    for i, profile in enumerate(ENVELOPE_PROBES):
        wav = tmp / f"env_{tag}_{profile}.wav"
        data = ac3space.generate_pcm(random.Random(offset + i), channels,
                                     BLOCKS_PER_FRAME * 4, 48000, profile, "independent")
        ac3space.write_wav(wav, data, 48000, True)
        probes.append(wav)
    return probes


def check_envelope(cli, jobs):
    """Re-measure LAYOUTS, and frmsiz's ceiling, against this binary.

    A hand-maintained table is exactly the kind of second copy this project's
    own tools/checks/check_matrix_coverage.py refuses to trust, so it gets
    checked rather than believed. Three assertions:

      robust  EVERY probe encodes at EVERY rate from here up, under EVERY tool
              set. This is the one that catches a regression - an encoder that
              started refusing configurations it used to fit.
      min     at least one probe/tool pair encodes at each rate from min up to
              robust. This catches a min set below anything reachable, which
              would silently spend the run's budget on cases that can never
              encode.
      ceiling at each Annex E half rate, the first legal Table 5.18 rate above
              what frmsiz can signal must be refused CLEANLY - exit 1 with a
              message naming the limit - and the highest one that fits must
              still encode. This is the deterministic gate on the defect that
              motivated CEILING_WORDS: before the fix that refusal was an
              abort, and an abort passes any "did it refuse" test that only
              looks for a non-zero exit.

    Run in parallel, unlike the AC-3 harness's equivalent, because this sweep
    is an order of magnitude larger: eight layouts rather than four, and a
    tool-set axis the AC-3 envelope has no equivalent of (six probes x five
    tool sets per rate, not six probes). Each encode is an independent process
    writing its own output file, so the only shared state is the read-only
    probe WAVs. It stays a seconds-cheap gate that way, which is what lets
    ci.yml run it on every pull request.
    """
    failures = []
    with tempfile.TemporaryDirectory(prefix="eac3space_env_") as tmp_str:
        tmp = Path(tmp_str)
        wide = _envelope_probes(tmp, 6, "wide", 200)
        # 1+1 is a strict identity on exactly two channels, never a fold-down,
        # so the 6-channel probes cannot drive it.
        narrow = _envelope_probes(tmp, 2, "narrow", 300)
        attempts = len(ENVELOPE_PROBES) * len(ENVELOPE_TOOLS)

        def attempt(index, wav, rate, tools, layout):
            out = tmp / f"env_{index}.ec3"
            return _run([cli, "eac3-encode", str(wav), str(out), str(rate),
                         tools, layout]).returncode == 0

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for layout, info in LAYOUTS.items():
                probes = narrow if layout == "1+1" else wide
                rates = [r for r in LEGAL_RATES if r >= info["min"]]
                work = [(wav, rate, tools)
                        for rate in rates for wav in probes for tools in ENVELOPE_TOOLS]
                # `layout` bound as a default rather than captured: this
                # lambda is consumed before the loop advances, so capturing
                # would be correct today and a late-binding bug the moment
                # anything here stops being eager.
                results = list(pool.map(
                    lambda item, layout=layout: attempt(item[0], *item[1], layout),
                    enumerate(work)))
                counts = {rate: sum(results[i * attempts:(i + 1) * attempts])
                          for i, rate in enumerate(rates)}

                not_robust = [r for r, n in counts.items()
                              if r >= info["robust"] and n < attempts]
                unreachable = [r for r, n in counts.items()
                               if r < info["robust"] and n == 0]
                ok = not not_robust and not unreachable
                summary = " ".join(f"{r}:{n}/{attempts}" for r, n in counts.items())
                print(f"  {'PASS' if ok else 'FAIL'}  {layout} (min {info['min']}, "
                      f"robust {info['robust']}): {summary}")
                if not_robust:
                    failures.append(f"{layout}: refused at or above robust={info['robust']} "
                                    f"for rates {not_robust}")
                if unreachable:
                    failures.append(f"{layout}: no probe encodes at all at rates "
                                    f"{unreachable}, which min={info['min']} still draws")

        print()
        # Section signs stay in comments here, never in printed output: this
        # would otherwise be the only line in tools/ that puts a non-ASCII
        # character on stdout, and a Windows console codepage is a needless
        # way for a gate to fail.
        print("  frmsiz's 11-bit word ceiling (E2.3.1.3), per Annex E half rate")
        ceiling_out = tmp / "env_ceiling.ec3"
        for rate in FSCOD2_RATES:
            wav = tmp / f"env_ceiling_{rate}.wav"
            data = ac3space.generate_pcm(random.Random(400), 2, BLOCKS_PER_FRAME * 4,
                                         rate, "runs", "independent")
            ac3space.write_wav(wav, data, rate, True)

            fits = highest_expressible(rate)
            over = next((r for r in LEGAL_RATES if over_ceiling(rate, r)), None)
            if fits is None or over is None:
                failures.append(f"{rate} Hz: no rate pair straddles the ceiling to test")
                continue

            below = _run([cli, "eac3-encode", str(wav), str(ceiling_out), str(fits),
                          "none", "stereo"])
            above = _run([cli, "eac3-encode", str(wav), str(ceiling_out), str(over),
                          "none", "stereo"])
            clean = above.returncode == 1 and REFUSALS["frmsiz ceiling"] in above.stderr
            ok = below.returncode == 0 and clean
            print(f"  {'PASS' if ok else 'FAIL'}  {rate} Hz: {fits} kbit/s encodes "
                  f"(exit {below.returncode}), {over} kbit/s refused with exit "
                  f"{above.returncode}")
            if below.returncode != 0:
                failures.append(f"{rate} Hz: {fits} kbit/s is inside frmsiz's ceiling "
                                f"({frame_words(rate, fits)} words) but was refused")
            if not clean:
                failures.append(
                    f"{rate} Hz: {over} kbit/s needs {frame_words(rate, over)} words, past "
                    f"frmsiz's {CEILING_WORDS} - it must be refused with exit 1 and a message "
                    f"naming the limit, not exit {above.returncode}")

    if failures:
        print("\nacceptance envelope has drifted from the tables in this file:")
        for f in failures:
            print(f"  {f}")
        print("A 'refused at or above robust' line is an encoder regression, not a table "
              "problem - fix that before touching this file. A 'no probe encodes' line means "
              "min is set below anything reachable; raise it, and say why in the commit. A "
              "ceiling line is a refusal that stopped being a clean one.")
        return 1
    print("\nacceptance envelope matches the tables in this file")
    return 0


def check_oracles(cli, ffmpeg, ffprobe):
    """Re-measure ORACLE_GAPS against this FFmpeg.

    The gap table decides what gets checked and what does not, which makes it
    the one place where being wrong is silent: a cell wrongly listed as a gap
    stops being tested and nothing says so. So it is measured rather than
    trusted - one representative stream per entry, plus a control that must
    still decode. An entry FFmpeg has since learned to read is reported as
    something to DELETE, which is the direction this table should move."""
    probes = [
        # (gap name, layout, tools, sample rate, must FFmpeg decode it?)
        ("(control: no gap)", "51", "none", 48000, True),
        ("two dependent substreams", "714", "none", 48000, False),
        # ecpl/tpn: FFmpeg exits 0 without having read the tool's syntax at
        # all, so neither outcome is informative - which is the whole reason
        # they are skipped rather than tolerated. Nothing is asserted about
        # the decode for these two; the framing check below still is.
        ("enhanced coupling (ecpl)", "51", "cpl+ecpl", 48000, None),
        ("transient pre-noise (tpn)", "51", "tpn", 48000, None),
        ("fscod2 half rate", "51", "none", 24000, False),
    ]
    failures = []
    with tempfile.TemporaryDirectory(prefix="eac3space_oracle_") as tmp_str:
        tmp = Path(tmp_str)
        for name, layout, tools, rate, expect_decode in probes:
            wav = tmp / f"oracle_{layout}_{rate}.wav"
            data = ac3space.generate_pcm(random.Random(500), 6, BLOCKS_PER_FRAME * 8,
                                         rate, "runs", "independent")
            ac3space.write_wav(wav, data, rate, True)
            out = tmp / "oracle.ec3"
            encode = _run([cli, "eac3-encode", str(wav), str(out), "384", tools, layout])
            if encode.returncode != 0:
                failures.append(f"{name}: the probe stream would not encode - "
                                f"{encode.stderr.strip().splitlines()[0]}")
                continue
            decoded = ffmpeg_check(ffmpeg, out, forced=True).returncode == 0
            case = Case(seed=0, command="eac3-encode", layout=layout, bitrate=384,
                        sample_rate=rate, source_channels=6, frames=8, pcm16=True,
                        audio_profile="runs", correlation="independent", tools=tools)
            claimed = units_written(encode.stdout + encode.stderr)
            walk = syncframe_walk(out, claimed)
            framing = header_check(ffprobe, out, case, claimed) if walk is None else None
            probe_note = ("not asked (FFmpeg cannot parse this layout)"
                          if DEPENDENTS.get(layout, 0) >= 2
                          else ("walks it" if framing is None
                                else "FAILED: " + framing.splitlines()[0]))
            print(f"  {name}: ffmpeg {'decodes' if decoded else 'refused'}, "
                  f"syncframes {'walk' if walk is None else 'FAILED: ' + walk.splitlines()[0]}"
                  f", ffprobe {probe_note}")
            if walk is not None:
                failures.append(f"{name}: the independent syncframe walk failed - {walk}")
            if framing is not None:
                failures.append(f"{name}: the framing oracle no longer holds - {framing}")
            if expect_decode is True and not decoded:
                failures.append(f"{name}: this is the control and it must decode")
            if expect_decode is False and decoded:
                failures.append(f"{name}: FFmpeg now DECODES this - delete the ORACLE_GAPS "
                                f"entry so these streams are checked properly")
    if failures:
        print("\nthe oracle model has drifted from what FFmpeg actually does:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("\noracle model matches this FFmpeg")
    return 0


# --- driver -----------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default=os.environ.get("AC3CLI", "build/dev/bin/ac3cli.exe"),
                        help="path to ac3cli (or set AC3CLI)")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="path to ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe", help="path to ffprobe")
    parser.add_argument("--no-ffmpeg", action="store_true",
                        help="skip the external oracles - the in-repo decoder only")
    parser.add_argument("--seed", type=int, default=None,
                        help="master seed; a random one is drawn and printed if omitted")
    parser.add_argument("--cases", type=int, default=None, help="run exactly this many cases")
    parser.add_argument("--seconds", type=float, default=None,
                        help="run until this many seconds have elapsed")
    parser.add_argument("--replay", type=int, default=None,
                        help="run the single case with this exact case seed")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--artifacts", default="fuzz-eac3-encoder-artifacts",
                        help="where failing cases are written")
    parser.add_argument("--check-envelope", action="store_true",
                        help="re-measure the per-layout rate floors and frmsiz's ceiling, "
                             "then exit")
    parser.add_argument("--check-oracles", action="store_true",
                        help="re-measure ORACLE_GAPS against this FFmpeg, then exit")
    parser.add_argument("--regressions", action="store_true",
                        help="replay every REGRESSION_SEEDS case and exit non-zero "
                             "if any of them fails")
    parser.add_argument("--max-failures", type=int, default=10,
                        help="stop after this many failing cases")
    args = parser.parse_args()

    cli = args.cli if Path(args.cli).is_absolute() else str((REPO / args.cli).resolve())
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")

    if args.check_envelope:
        print("acceptance envelope - the rate floors and the frmsiz ceiling this generator "
              "draws around")
        sys.exit(check_envelope(cli, args.jobs))

    ffmpeg = None
    ffprobe = None
    if not args.no_ffmpeg:
        ffmpeg = shutil.which(args.ffmpeg)
        ffprobe = shutil.which(args.ffprobe)
        if ffmpeg is None or ffprobe is None:
            missing = args.ffmpeg if ffmpeg is None else args.ffprobe
            raise SystemExit(
                f"{missing} not found - FFmpeg is the external oracle half of this harness, "
                "and ffprobe specifically is the ONLY external check on the cells FFmpeg "
                "cannot decode (see the header). Pass --no-ffmpeg to run against the in-repo "
                "decoder alone, knowing that a stream which only round-trips against its own "
                "encoder proves much less.")

    if args.check_oracles:
        if ffmpeg is None:
            raise SystemExit("--check-oracles measures FFmpeg; it cannot run with --no-ffmpeg")
        print("oracle model - what this FFmpeg can and cannot read")
        sys.exit(check_oracles(cli, ffmpeg, ffprobe))

    artifacts = Path(args.artifacts)
    if not artifacts.is_absolute():
        artifacts = REPO / artifacts

    if args.regressions:
        if not REGRESSION_SEEDS:
            print("no recorded regressions yet")
            sys.exit(0)
        failed = 0
        for seed, why in REGRESSION_SEEDS.items():
            case = draw_case(seed)
            print(f"regression {seed}: {why}")
            print(f"  {describe(case)}")
            with tempfile.TemporaryDirectory(prefix="eac3space_") as workdir:
                result = run_case(cli, ffmpeg, ffprobe, case, workdir, artifacts)
            print(f"  {result.status}"
                  + (f" ({result.stage}): {result.detail}" if result.detail else ""))
            failed += result.status == "fail"
        sys.exit(1 if failed else 0)

    if args.replay is not None:
        case = draw_case(args.replay)
        print(describe(case))
        oracle, gaps = oracle_for(case)
        print(f"  oracle: {oracle}" + (f" (gaps: {', '.join(gaps)})" if gaps else ""))
        with tempfile.TemporaryDirectory(prefix="eac3space_") as workdir:
            result = run_case(cli, ffmpeg, ffprobe, case, workdir, artifacts)
        print(f"  {result.status}"
              + (f" ({result.stage}): {result.detail}" if result.detail else ""))
        sys.exit(1 if result.status == "fail" else 0)

    if args.cases is None and args.seconds is None:
        args.cases = 100
    master = args.seed if args.seed is not None else random.SystemRandom().randrange(2 ** 63)

    print(f"E-AC-3 encoder-space fuzz: cli={cli} ffmpeg={'off' if ffmpeg is None else ffmpeg}")
    print(f"master seed {master}"
          + (f", {args.cases} cases" if args.cases else f", {args.seconds:g}s budget")
          + f", {args.jobs} jobs")
    print("(every failure below prints its own case seed; --replay <seed> reruns just it)")
    print()

    started = time.monotonic()
    counts = {"ok": 0, "refused": 0, "misprobed": 0, "no-oracle": 0, "fail": 0}
    refusals = {reason: 0 for reason in REFUSALS}
    gaps_seen = {name: 0 for name in ORACLE_GAPS}
    failures = []
    index = 0

    def budget_left():
        if args.cases is not None:
            return index < args.cases
        return (time.monotonic() - started) < args.seconds

    with tempfile.TemporaryDirectory(prefix="eac3space_") as workdir:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = set()
            while (budget_left() or pending) and len(failures) < args.max_failures:
                while budget_left() and len(pending) < args.jobs * 2:
                    case = draw_case(case_seed(master, index))
                    index += 1
                    pending.add(pool.submit(run_case, cli, ffmpeg, ffprobe, case, workdir,
                                            artifacts))
                if not pending:
                    break
                done, pending = concurrent.futures.wait(
                    pending, return_when=concurrent.futures.FIRST_COMPLETED)
                for future in done:
                    result = future.result()
                    counts[result.status] += 1
                    if result.status == "refused":
                        refusals[result.reason] += 1
                    for gap in result.gaps:
                        gaps_seen[gap] += 1
                    if result.status == "fail":
                        failures.append(result)
                        print(f"FAIL [{result.stage}] {describe(result.case)}")
                        print(f"  {result.detail.splitlines()[0] if result.detail else ''}")
                        print(f"  {repro(result.case)}".replace("\n", "\n  "))
                        print()

    elapsed = time.monotonic() - started
    total = sum(counts.values())
    breakdown = ", ".join(f"{n} {reason}" for reason, n in refusals.items() if n)
    print(f"{total} cases in {elapsed:.1f}s: {counts['ok']} encoded and decoded cleanly against "
          f"both decoders, {counts['no-oracle']} against the in-repo decoder and the framing "
          f"oracle only, {counts['refused']} refused"
          + (f" ({breakdown})" if breakdown else "")
          + (f", {counts['misprobed']} misprobed (valid stream, ffmpeg auto-detection chose "
             "another container)" if counts["misprobed"] else "")
          + f", {counts['fail']} failed")
    if any(gaps_seen.values()):
        print("  cells with no FFmpeg decode (framing checked, samples not): "
              + ", ".join(f"{n} {name}" for name, n in gaps_seen.items() if n))

    if counts["fail"]:
        print(f"\nfailing inputs kept in {artifacts}")
        print("Each directory holds the exact in.wav/out.ec3 plus the config that produced it.")
        sys.exit(1)

    # A run where the encoder refused nearly everything is not a pass: it
    # would mean the generator has drifted out of the accepted space, or the
    # encoder has regressed into refusing it, and every case above would have
    # been "clean" without a single stream being checked. Misprobed and
    # no-oracle cases count as checked: their streams were encoded, decoded by
    # ac3cli, and held against ffprobe's own syncframe walk.
    checked = counts["ok"] + counts["misprobed"] + counts["no-oracle"]
    if total >= 20 and checked < total * 0.5:
        print(f"\nonly {checked} of {total} configurations encoded at all - too few to call "
              "this a pass. Either the generator is drawing outside the accepted space (re-run "
              "with --check-envelope) or the encoder has regressed into refusing it.")
        sys.exit(1)


if __name__ == "__main__":
    main()
