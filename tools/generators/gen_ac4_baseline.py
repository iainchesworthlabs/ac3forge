"""Generates the Dolby Encoding Engine's AC-4 streams this project checks itself against.

Two sets, from the same legs and sources:

- The committed set: tests/golden/external-baseline/ac4-*/dee.ac4, ac4-manifest.json beside
  them, and each stream's syntax digest under tests/golden/ac4dec/. Short streams that CI reads.
- The gold set (--gold-set DIR): every stream the phases of planning/ac4.md need, kept on a
  local disk and never committed. Phase G0 of that plan makes it while the local DEE licence
  runs (it ends on 2026-11-06).

What the streams are for. The decoder in src/ac4dec is checked against them: first its syntax,
read by two transcriptions whose traces must agree (tools/references/ac4_syntax.py writes the
committed digests; tests/ac4dec/test_ac4dec_syntax.cpp holds the decoder to them), then, from
phase D2 on, its PCM, scored against each stream's source. The encoder phases race against the
same streams. So every leg except ac4-stereo-64 is made with loudness measured and not
corrected: DEE's default (measure_and_correct) normalises to -24 LKFS and runs a -2 dBFS
true-peak limiter, which changes the audio in a way no gain fit undoes. DRC profiles and the
downmix settings are metadata; DEE applies neither to the audio.

Legs of the committed set (ac4 = dee_ac4_encoder.exe, ims = dee_ac4ims_encoder.exe):

  leg                    enc  source            kbps  codec mode; what else the stream carries
  ac4-stereo-64          ac4  reference_stereo    64  ASPX; DEE's default loudness correction
  ac4-20-music-192       ac4  music_20           192  SIMPLE
  ac4-20-speech-128      ac4  speech_20          128  ASPX, companding off
  ac4-20-tones-192       ac4  tones_20           192  SIMPLE; one tone per channel
  ac4-51-film-96         ac4  film_51             96  ASPX_ACPL_3
  ac4-51-music-128       ac4  music_51           128  ASPX_ACPL_2
  ac4-51-music-192       ac4  music_51           192  ASPX
  ac4-51-music-384       ac4  music_51           384  SIMPLE
  ac4-51-tones-384       ac4  tones_51           384  SIMPLE; one tone per channel
  ac4-51-drc-ltrt-192    ac4  music_51           192  ASPX; explicit DRC curves, Lt/Rt downmix
  ac4-ims-music-64-2997  ims  music_51            64  ASPX; 29.97 fps (1536 samples)
  ac4-ims-film-96-24     ims  film_51             96  ASPX; 24 fps (1920 samples)
  ac4-ims-music-128-25   ims  music_51           128  ASPX; 25 fps (2048 samples)

ac4-stereo-64 is the version 1 leg and keeps its bytes: tests pin its frame count and its
MediaInfo-checked table of contents, so main() refuses to replace it. The codec mode depends
only on the layout and the data rate (a census of 119 DEE 6.5.4 encodes of 20 s material):
stereo is ASPX up to 144 kbps, with companding on at 48-96 and off at 128-144, and SIMPLE from
192; 5.1 is ASPX_ACPL_3 at 96, ASPX_ACPL_2 at 128-144, ASPX at 192-320 and SIMPLE from 384.
dee_ac4_encoder always writes frame_rate_index 13 (2048 samples) and has no frame-rate option,
so the other frame lengths come from dee_ac4ims_encoder's --target-fps.

ac4-51-drc-ltrt-192 carries per-device DRC profiles that differ (film_standard, speech for
headphones, music_light for home theatre) and the Lt/Rt preferred downmix with explicit mix
levels. dee_ac4ims_encoder's default mode is general, which sends advanced dialogue enhancement
data in the I-frames of the IMS legs.

Every IMS stream signals presentation_version 2 with channel_mode code 0b1111000, which TS
103 190-2 Table 56 maps to 7.0, and codes a channel_pair_element: walked as 7.0, 5.0 or 5.1, at
least every I-frame fails its substream size checks; walked as stereo, every frame ends
exactly (src/ac4dec/ERRATA.md, "presentation_version 2 is read as immersive stereo").

Sources, rebuilt on every run from committed material, so any stream can be scored again. Cuts
of the committed programme fixtures (30 s each), SECONDS long (5 s for the committed set, 10 s
for the gold set), written as 24-bit PCM WAVs (format tag 1) into the scratch or gold
directory, never into the tree:

  music_20   programme_music_stereo.flac from 0 s.
  speech_20  programme_speech_stereo.flac from 0 s.
  music_51   L R C LFE Ls Rs. L/R are the music from 0 s; C is its mono mix from T s x 0.6;
             LFE is its mono mix from 0 s, low-passed at 120 Hz, x 0.8; Ls/Rs are the music
             from 2T s x 0.5, with T = max(5, SECONDS). No channel is a copy of another.
  film_51    The speech's mono mix from 0 s in C over music_51's bed, with L/R x 0.45, Ls/Rs x
             0.35 and the same LFE.
  music_514  (gold set only) music_51 and four top channels: Tfl/Tfr the music from SECONDS/2
             x 0.4, Tbl/Tbr the music from 3 SECONDS/2 x 0.3.
  tones_20, tones_51, tones_514
             One sine per channel at -20 dBFS, at frequencies no channel's shares with
             another's harmonics (TONE_HZ), for checking that each channel lands on its own
             speaker. The LFE's is 47 Hz.

The FLACs are decoded with ffmpeg, the route quality_race.py's materialise_fixture() takes.
The LFE is filtered with the linear-phase windowed-sinc low-pass of
tools/listening/gen_listening_stimuli.py. A mix peaking above 0.98 would be scaled down to
0.98; none of these does. Channel order is L R C LFE Ls Rs, and for 5.1.4 L R C LFE Ls Rs Tfl
Tfr Tbl Tbr, the SMPTE order dee_ac4_encoder's cbi_wav input names. The AC-4 encoders read a
single 6-channel WAV the same way as a wav_list of its channels (music_51 encoded both ways
gives byte-identical streams), unlike dee_ddp_encoder, which gen_external_baseline.py records
losing Ls from a single 6-channel WAV.

Manifest. baseline_version 3 (see BASELINE_VERSION). One entry per leg:

  encoder, source, output_channel_layout, bitrate_kbps, options
      What was encoded and how. options are the DEE arguments beyond input, output, layout and
      data rate; [] is DEE's defaults. The IMS encoder takes no layout argument; "IMS" names
      its immersive-stereo output. ac4-stereo-64 also keeps its version 1 source_wav.
  duration_s, source_sha256, size_bytes
      The source WAV's length and SHA-256, and the stream's size. The SHA-256 is of the WAV
      DEE encoded, so a scorer that rebuilds the source can check it has the same one.
  frame_count, frame_rate_index, frame_len_base
      Read from the bytes with tools/references/ac4_parse.py, which also requires every sync
      frame's CRC and table of contents to read. frame_len_base is TS 103 190-1 Table 83's,
      for 48 kHz.
  codec_mode, explicit_drc_curves, custom_downmix_data, de_parameters, advanced_de_data
      What tools/references/ac4_syntax.py's trace of every frame found (walked_by names it):
        codec_mode           the audio substream's *_codec_mode, the same in every frame.
        explicit_drc_curves  drc_compression_curve_flag set for some DRC decoder mode.
        custom_downmix_data  custom_dmx_data() in the presentation substream sets
                             b_stereo_dmx_coeff or b_cdmx_data_present in some frame.
        de_parameters        dialog_enhancement() sends de_par codewords in some frame.
        advanced_de_data     b_advanced_de_data_present set in some frame.
      LEGS states what the layout, rate and options must give; main() stops if a walk
      disagrees, before anything in the tree changes. de_parameters is not stated: it follows
      the dialogue DEE detects in the content.

The gold set (--gold-set DIR). DIR/sources holds the 10 s sources; DIR/streams/<leg> holds each
stream (dee.ac4), DEE's log and output manifest, MediaInfo's frame-by-frame trace
(mediainfo-details.txt, from --Details=1) and summary (mediainfo.json), and leg.json, which
lets a rerun reuse a stream whose command and source have not changed; DIR/gold-manifest.json
describes every leg as ac4-manifest.json does, with the stream's SHA-256 and each walk's
diagnostics. The legs (gold_legs()) are every layout and rate dee_ac4_encoder writes, from 2.0
at 48 kbps to 5.1.4 at 768, for music, speech or film, and tones; dee_ac4ims_encoder at every
rate and frame rate; and metadata legs: each DRC profile, the per-device profiles, each
preferred downmix and some mix levels, I-frame intervals, loudness presets, the height
downmix, and the IMS encoder's DRC settings. 5.1.4's audio is refused by both transcriptions
until phase D9, so its codec mode is recorded as null.

Objects (--adm-master WAV, repeatable, with --gold-set). dee_ac4ajoc_encoder accepts only an
Atmos master, and DEE refused a master this project authored before, on provenance rather
than syntax (docs/verification.md). Each master given is tried once with dee_ac4ajoc_encoder at
levels 3 and 4 and with dee_ac4ims_encoder; a refusal is recorded with DEE's message, not
raised.

Usage (repo root):
  python tools/generators/gen_ac4_baseline.py [--scratch-dir DIR]
  python tools/generators/gen_ac4_baseline.py --gold-set D:/ac3bld/ac4-gold [--adm-master WAV]

Never run in CI - see guard_not_ci(), the same rule gen_external_baseline.py's own copy of
this function states.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import wave
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

# tools/references/: ac4_parse.py, the independent transcription of the AC-4 sync frame and
# table of contents, reads frame counts and frame_rate_index back out of every encode;
# ac4_syntax.py, the independent transcription of the substream syntax, walks every frame.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "references"))
import ac4_parse
import ac4_syntax

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO = REPO / "tests" / "golden" / "audio"
OUT = REPO / "tests" / "golden" / "external-baseline"
DIGESTS = REPO / "tests" / "golden" / "ac4dec"
SCRATCH = REPO / "build" / "ac4_baseline_scratch"

DEE_DIR = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir")
MEDIAINFO = DEE_DIR / "MediaInfo.exe"
AC4 = "dee_ac4_encoder"
IMS = "dee_ac4ims_encoder"
AJOC = "dee_ac4ajoc_encoder"

# Bump by hand whenever this script is rerun to regenerate the baseline against a new DEE
# release, or changes what it asks DEE for - the same discipline gen_external_baseline.py's
# BASELINE_VERSION follows.
#
# 2: ten legs added and the manifest extended to describe each stream. ac4-stereo-64's bytes
#    are the version 1 bytes: main() checks a fresh encode against the committed file instead
#    of overwriting it.
# 3: planning/ac4.md's phase G0. Every leg but ac4-stereo-64 is made again from 5 s sources
#    with loudness measured and not corrected, so a decode can be scored against its source;
#    two one-tone-per-channel legs; source_sha256; walked values computed by ac4_syntax.py;
#    the digests under tests/golden/ac4dec/ written here; the gold set.
BASELINE_VERSION = 3

RATE = 48000
COMMITTED_SECONDS = 5.0
GOLD_SECONDS = 10.0
GOLD_DIR_DEFAULT = Path("D:/ac3bld/ac4-gold")

MEASURE_ONLY = ["--loudness-management", "measure_only"]

# One sine per channel, L R C LFE Ls Rs Tfl Tfr Tbl Tbr: primes, so no tone sits on another's
# harmonic, and the LFE's inside its band.
TONE_HZ = (331, 457, 613, 47, 787, 953, 1117, 1289, 1453, 1621)
TONE_AMPLITUDE = 0.1

# TS 103 190-1 Table 83, 48 kHz: frame_rate_index -> frame_len_base in samples.
FRAME_LEN_BASE = {0: 1920, 1: 1920, 2: 2048, 3: 1536, 4: 1536, 5: 960, 6: 960, 7: 1024,
                  8: 768, 9: 768, 10: 512, 11: 384, 12: 384, 13: 2048}

# The *_codec_mode fields of the Part 1 channel elements, by value.
CODEC_MODES = {
    "mono_codec_mode": ("SIMPLE", "ASPX"),
    "stereo_codec_mode": ("SIMPLE", "ASPX", "ASPX_ACPL_1", "ASPX_ACPL_2"),
    "3_0_codec_mode": ("SIMPLE", "ASPX"),
    "5_X_codec_mode": ("SIMPLE", "ASPX", "ASPX_ACPL_1", "ASPX_ACPL_2", "ASPX_ACPL_3"),
    "7_X_codec_mode": ("SIMPLE", "ASPX", "ASPX_ACPL_1", "ASPX_ACPL_2"),
}

WALKED_KEYS = ("codec_mode", "explicit_drc_curves", "custom_downmix_data", "de_parameters",
               "advanced_de_data")


def source_descriptions(seconds):
    t = max(5.0, seconds)
    return {
        "reference_stereo": "reference_stereo.wav: the committed 3.0 s synthetic stereo reference",
        "music_20": f"music 2.0: programme_music_stereo.flac 0-{seconds:g} s",
        "speech_20": f"speech 2.0: programme_speech_stereo.flac 0-{seconds:g} s",
        "music_51": (f"music 5.1 bed from programme_music_stereo.flac: L R from 0 s, C mono from "
                     f"{t:g} s x 0.6, LFE mono from 0 s low-passed at 120 Hz x 0.8, Ls Rs from "
                     f"{2 * t:g} s x 0.5; {seconds:g} s"),
        "film_51": (f"film 5.1: programme_speech_stereo.flac mono from 0 s in C over the music_51 "
                    f"bed with L R x 0.45, Ls Rs x 0.35 and the same LFE; {seconds:g} s"),
        "music_514": (f"music 5.1.4: the music_51 bed with Tfl Tfr from {seconds / 2:g} s x 0.4 "
                      f"and Tbl Tbr from {1.5 * seconds:g} s x 0.3; {seconds:g} s"),
        "tones_20": f"one tone per channel, L R: {TONE_HZ[0]} and {TONE_HZ[1]} Hz at -20 dBFS",
        "tones_51": ("one tone per channel, L R C LFE Ls Rs: "
                     + ", ".join(str(f) for f in TONE_HZ[:6]) + " Hz at -20 dBFS"),
        "tones_514": ("one tone per channel, L R C LFE Ls Rs Tfl Tfr Tbl Tbr: "
                      + ", ".join(str(f) for f in TONE_HZ) + " Hz at -20 dBFS"),
    }


_DRC_AND_LTRT = ("drc_profile=film_standard:drc_profile_portable_hp=speech:"
                 "drc_profile_home_theatre=music_light:preferred_downmix_mode=ltrt:"
                 "ltrt_cmix=-6:ltrt_smix=-inf:loro_cmix=0:loro_smix=-6")

# The committed legs. "expect" holds what the layout, the rate and the options must give; a
# walk that disagrees stops main() before anything in the tree changes.
LEGS = [
    # tests/ac4/test_ac4.cpp pins this stream's frame count and its MediaInfo-checked TOC
    # fields; tests/cli and fuzz/generate-seeds.sh read it too. "pinned": main() refuses to
    # replace it with different bytes, so it keeps DEE's defaults.
    {"name": "ac4-stereo-64", "encoder": AC4, "source": "reference_stereo", "layout": "stereo",
     "kbps": 64, "options": [], "pinned": True,
     "expect": {"frame_rate_index": 13, "codec_mode": "ASPX"}},
    {"name": "ac4-20-music-192", "encoder": AC4, "source": "music_20", "layout": "stereo",
     "kbps": 192, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "SIMPLE", "custom_downmix_data": False}},
    {"name": "ac4-20-speech-128", "encoder": AC4, "source": "speech_20", "layout": "stereo",
     "kbps": 128, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "ASPX", "custom_downmix_data": False}},
    {"name": "ac4-20-tones-192", "encoder": AC4, "source": "tones_20", "layout": "stereo",
     "kbps": 192, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "SIMPLE", "custom_downmix_data": False}},
    {"name": "ac4-51-film-96", "encoder": AC4, "source": "film_51", "layout": "5.1",
     "kbps": 96, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "ASPX_ACPL_3", "custom_downmix_data": True,
                "explicit_drc_curves": False}},
    {"name": "ac4-51-music-128", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 128, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "ASPX_ACPL_2", "custom_downmix_data": True,
                "explicit_drc_curves": False}},
    {"name": "ac4-51-music-192", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 192, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "ASPX", "custom_downmix_data": True,
                "explicit_drc_curves": False}},
    {"name": "ac4-51-music-384", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 384, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "SIMPLE", "custom_downmix_data": True,
                "explicit_drc_curves": False}},
    {"name": "ac4-51-tones-384", "encoder": AC4, "source": "tones_51", "layout": "5.1",
     "kbps": 384, "options": MEASURE_ONLY,
     "expect": {"frame_rate_index": 13, "codec_mode": "SIMPLE", "custom_downmix_data": True}},
    {"name": "ac4-51-drc-ltrt-192", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 192, "options": [*MEASURE_ONLY, "--encoder", _DRC_AND_LTRT],
     "expect": {"frame_rate_index": 13, "codec_mode": "ASPX", "custom_downmix_data": True,
                "explicit_drc_curves": True}},
    {"name": "ac4-ims-music-64-2997", "encoder": IMS, "source": "music_51", "layout": "IMS",
     "kbps": 64, "options": [*MEASURE_ONLY, "--target-fps", "29.97"],
     "expect": {"frame_rate_index": 3, "codec_mode": "ASPX", "advanced_de_data": True}},
    {"name": "ac4-ims-film-96-24", "encoder": IMS, "source": "film_51", "layout": "IMS",
     "kbps": 96, "options": [*MEASURE_ONLY, "--target-fps", "24"],
     "expect": {"frame_rate_index": 1, "codec_mode": "ASPX", "advanced_de_data": True}},
    {"name": "ac4-ims-music-128-25", "encoder": IMS, "source": "music_51", "layout": "IMS",
     "kbps": 128, "options": [*MEASURE_ONLY, "--target-fps", "25"],
     "expect": {"frame_rate_index": 2, "codec_mode": "ASPX", "advanced_de_data": True}},
]

STEREO_RATES = (48, 64, 96, 128, 144, 192, 256, 288, 320, 384, 448, 512, 768)
FIVE_ONE_RATES = (96, 128, 144, 192, 256, 288, 320, 384, 448, 512, 768)
FIVE_ONE_FOUR_RATES = (192, 256, 288, 320, 384, 448, 512, 768)
IMS_RATES = (64, 96, 128, 144, 256, 320)
IMS_FPS = ("23.976", "24", "25", "29.97")


def gold_legs():
    """The gold set's legs: every layout and rate, then the metadata legs."""
    legs = []

    def leg(name, encoder, source, layout, kbps, options=(), input_format="wav"):
        legs.append({"name": name, "encoder": encoder, "source": source, "layout": layout,
                     "kbps": kbps, "options": [*MEASURE_ONLY, *options],
                     "input_format": input_format})

    for kbps in STEREO_RATES:
        for source, tag in (("music_20", "music"), ("speech_20", "speech"), ("tones_20", "tones")):
            leg(f"20-{tag}-{kbps}", AC4, source, "stereo", kbps)
    for kbps in FIVE_ONE_RATES:
        for source, tag in (("music_51", "music"), ("film_51", "film"), ("tones_51", "tones")):
            leg(f"51-{tag}-{kbps}", AC4, source, "5.1", kbps)
    for kbps in FIVE_ONE_FOUR_RATES:
        for source, tag in (("music_514", "music"), ("tones_514", "tones")):
            leg(f"514-{tag}-{kbps}", AC4, source, "5.1.4", kbps, input_format="cbi_wav")
    for kbps in IMS_RATES:
        leg(f"ims-music-{kbps}-native", IMS, "music_51", "IMS", kbps)
    for fps in IMS_FPS:
        for kbps in (64, 128):
            for source, tag in (("music_51", "music"), ("film_51", "film")):
                leg(f"ims-{tag}-{kbps}-{fps.replace('.', '')}", IMS, source, "IMS", kbps,
                    ["--target-fps", fps])
    # Music mode refuses to start unless every DRC profile is music_light and dialogue
    # intelligence (a loudness measurement setting) is off.
    legs.append({"name": "ims-music-128-native-musicmode", "encoder": IMS, "source": "music_51",
                 "layout": "IMS", "kbps": 128,
                 "options": ["--loudness-management", "measure_only:dialogue_intelligence=0",
                             "--encoder", "mode=music:drc_profile=music_light"],
                 "input_format": "wav"})
    leg("ims-music-128-native-drcnone", IMS, "music_51", "IMS", 128,
        ["--encoder", "drc_profile=none"])
    leg("ims-music-128-native-drcddp", IMS, "music_51", "IMS", 128,
        ["--encoder", "drc_profile_ddp=music_standard"])

    metadata = [
        *((f"drc-{p}", f"drc_profile={p}") for p in ("film_light", "film_standard",
                                                     "music_light", "music_standard", "speech")),
        ("drc-per-device",
         "drc_profile=film_standard:drc_profile_portable_hp=speech:"
         "drc_profile_home_theatre=music_light:drc_profile_portable_spkr=music_standard:"
         "drc_profile_flat_panel=film_light"),
        *((f"dmx-{m}", f"preferred_downmix_mode={m}")
          for m in ("loro", "ltrt", "ltrt-pl2", "not_indicated")),
        ("mix-loro-cm6-sminf", "loro_cmix=-6:loro_smix=-inf"),
        ("mix-loro-cp3-sm1.5", "loro_cmix=+3:loro_smix=-1.5"),
        ("mix-ltrt-c0-sm4.5", "preferred_downmix_mode=ltrt:ltrt_cmix=0:ltrt_smix=-4.5"),
        *((f"iframe-{n}", f"iframe_interval={n}") for n in (11, 48, 1000)),
    ]
    for tag, setting in metadata:
        leg(f"51-music-192-{tag}", AC4, "music_51", "5.1", 192, ["--encoder", setting])
    for preset in ("atsc_a85", "ebu_r128"):
        legs.append({"name": f"51-music-192-loudness-{preset}", "encoder": AC4,
                     "source": "music_51", "layout": "5.1", "kbps": 192,
                     "options": ["--loudness-management", f"measure_only:preset={preset}"],
                     "input_format": "wav"})
    for mode in ("front", "front_and_surround", "surround"):
        for gain in ("-inf", "0"):
            leg(f"514-music-256-height-{mode}-{gain.replace('-', 'm')}", AC4, "music_514",
                "5.1.4", 256, ["--encoder", f"height_dmx_mode={mode}:height_dmx_gain={gain}"],
                input_format="cbi_wav")
    return legs


def guard_not_ci():
    if os.environ.get("GITHUB_ACTIONS"):
        raise SystemExit(
            "gen_ac4_baseline.py invokes licensed, non-CI-safe tooling "
            "(Dolby DEE) and must never run in a CI job - refusing because "
            "GITHUB_ACTIONS is set.")


def dee_exe(encoder):
    return DEE_DIR / f"{encoder}.exe"


def dee_version(encoder):
    # No --version option; -h's help text carries a "belongs to the Dolby
    # Encoding Engine version X" line instead. check=False: some builds exit
    # non-zero even after printing the banner this parses.
    result = subprocess.run([str(dee_exe(encoder)), "-h"], capture_output=True, text=True,
                            check=False)
    for line in result.stdout.splitlines():
        if "Dolby Encoding Engine version" in line:
            return line.strip()
    return "unknown"


def mediainfo_version():
    result = subprocess.run([str(MEDIAINFO), "--Version"], capture_output=True, text=True,
                            check=False)
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return lines[-1] if lines else "unknown"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_fixture(flac, scratch_dir):
    """A committed 48 kHz 16-bit stereo FLAC as float64 samples, shape (n, 2)."""
    wav = scratch_dir / f"{flac.stem}.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(flac), "-c:a", "pcm_s16le",
                    str(wav)], check=True)
    with wave.open(str(wav), "rb") as r:
        if (r.getframerate(), r.getnchannels(), r.getsampwidth()) != (RATE, 2, 2):
            raise SystemExit(f"{flac}: expected 48 kHz 16-bit stereo, got {r.getparams()}")
        pcm = np.frombuffer(r.readframes(r.getnframes()), dtype="<i2")
    return pcm.reshape(-1, 2) / 32768.0


def cut(x, start_s, seconds):
    """seconds of x, starting start_s seconds in."""
    first = round(start_s * RATE)
    count = round(seconds * RATE)
    if first + count > len(x):
        raise SystemExit(f"a {seconds} s cut at {start_s} s runs past the end of the fixture")
    return x[first:first + count]


def lowpass(x, cutoff_hz=120.0, taps=2047):
    """Linear-phase windowed-sinc low-pass; mode="same" removes its group delay."""
    n = np.arange(taps) - (taps - 1) / 2.0
    kernel = 2.0 * (cutoff_hz / RATE) * np.sinc(2.0 * (cutoff_hz / RATE) * n) * np.blackman(taps)
    return np.convolve(x, kernel / kernel.sum(), mode="same")


def tones(count, seconds):
    t = np.arange(round(seconds * RATE)) / RATE
    return [TONE_AMPLITUDE * np.sin(2.0 * np.pi * hz * t) for hz in TONE_HZ[:count]]


def write_wav24(path, columns):
    pcm = np.stack(columns, axis=1)
    peak = float(np.max(np.abs(pcm)))
    if peak > 0.98:
        pcm *= 0.98 / peak
    samples = np.round(pcm * 8388607.0).astype("<i4")
    frames = samples.view(np.uint8).reshape(len(samples), -1, 4)[:, :, :3]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(3)
        w.setframerate(RATE)
        w.writeframes(frames.tobytes())


def build_sources(source_dir, seconds, names):
    """Writes the named sources' WAVs (see the module docstring's Sources); returns their
    paths."""
    source_dir.mkdir(parents=True, exist_ok=True)
    music = read_fixture(AUDIO / "programme_music_stereo.flac", source_dir)
    speech = read_fixture(AUDIO / "programme_speech_stereo.flac", source_dir)
    music_mono = music.mean(axis=1)
    t = max(5.0, seconds)
    left, right = cut(music, 0.0, seconds).T
    surround_left, surround_right = cut(music, 2 * t, seconds).T
    centre = 0.6 * cut(music_mono, t, seconds)
    lfe = 0.8 * lowpass(cut(music_mono, 0.0, seconds))
    bed = [left, right, centre, lfe, 0.5 * surround_left, 0.5 * surround_right]
    mixes = {
        "music_20": [left, right],
        "speech_20": list(cut(speech, 0.0, seconds).T),
        "music_51": bed,
        "film_51": [0.45 * left, 0.45 * right, cut(speech.mean(axis=1), 0.0, seconds), lfe,
                    0.35 * surround_left, 0.35 * surround_right],
        "tones_20": tones(2, seconds),
        "tones_51": tones(6, seconds),
        "tones_514": tones(10, seconds),
    }
    if "music_514" in names:
        top_front_left, top_front_right = cut(music, seconds / 2, seconds).T
        top_back_left, top_back_right = cut(music, 1.5 * seconds, seconds).T
        mixes["music_514"] = [*bed, 0.4 * top_front_left, 0.4 * top_front_right,
                              0.3 * top_back_left, 0.3 * top_back_right]
    paths = {}
    for name in names:
        paths[name] = source_dir / f"{name}.wav"
        write_wav24(paths[name], mixes[name])
    return paths


def command(leg, wav, out, work):
    """DEE's command line for one leg."""
    exe = str(dee_exe(leg["encoder"]))
    if leg["encoder"] == AJOC:
        return [exe, "-i", str(wav), "-o", str(out), "--overwrite", "1",
                "--output-manifest", str(work / "dee_manifest.json"), *leg["options"]]
    cmd = [exe, "--input-format", leg.get("input_format", "wav"), "-i", str(wav), "-o",
           str(out), "--overwrite", "1"]
    if leg["encoder"] == AC4:
        cmd += ["--output-channel-layout", leg["layout"], "--temp-dir", str(work),
                "--output-manifest", str(work / "dee_manifest.json")]
    return [*cmd, "--data-rate", str(leg["kbps"]), *leg["options"]]


def encode(leg, wav, work, refusal_ok=False):
    """One DEE encode into work/dee.ac4; returns the stream's bytes, or None when DEE refuses
    and refusal_ok is set (DEE's message is then in work/dee_output.txt).

    Each leg runs from its own directory, which also keeps each leg's DEE output apart:
    DEE keeps temporary files in the current directory unless --temp-dir says otherwise,
    and the installation directory itself is not writable. dee_ac4_encoder and
    dee_ac4ajoc_encoder refuse to start without --output-manifest; dee_ac4ims_encoder has no
    such option, and no layout option either.
    """
    work.mkdir(parents=True, exist_ok=True)
    out = work / "dee.ac4"
    out.unlink(missing_ok=True)
    cmd = command(leg, wav, out, work)
    result = subprocess.run(cmd, cwd=work, capture_output=True, text=True, check=False)
    log = work / "dee_output.txt"
    log.write_text(subprocess.list2cmdline(cmd) + "\n\n" + result.stdout + result.stderr)
    if result.returncode != 0 or not out.is_file():
        if refusal_ok:
            return None
        raise SystemExit(f"{leg['name']}: DEE exited {result.returncode} - see {log}\n"
                         + (result.stdout + result.stderr)[-2000:])
    return out.read_bytes()


def describe(name, data):
    """(frame_count, frame_rate_index), read with tools/references/ac4_parse.py.

    Every sync frame has to pass its CRC and have a TOC and substream index table that read,
    and frame_rate_index has to stay the same throughout.
    """
    frame_rate_indices = set()
    frame_count = 0
    for offset, _, raw, crc_ok in ac4_parse.iter_sync_frames(data):
        if crc_ok is False:
            raise SystemExit(f"{name}: CRC mismatch in the sync frame at byte {offset}")
        toc, _ = ac4_parse.parse_raw_frame(raw)
        frame_rate_indices.add(toc["frame_rate_index"])
        frame_count += 1
    if len(frame_rate_indices) != 1:
        raise SystemExit(f"{name}: frame_rate_index varies: {sorted(frame_rate_indices)}")
    return frame_count, frame_rate_indices.pop()


def walk(data):
    """What every frame carries, from tools/references/ac4_syntax.py's trace (the module
    docstring's Manifest section defines each value); returns (values, diagnostics)."""
    diagnostics = []
    modes = set()
    values = dict.fromkeys(WALKED_KEYS[1:], False)
    for _frame, _substream, kind, records in ac4_syntax.walk_stream(data, True, diagnostics):
        for _offset, _width, value, element in records:
            if element in CODEC_MODES:
                names = CODEC_MODES[element]
                modes.add(names[value] if value < len(names) else f"{element} {value}")
            elif element == "drc_compression_curve_flag" and value:
                values["explicit_drc_curves"] = True
            elif (element in ("b_stereo_dmx_coeff", "b_cdmx_data_present") and value
                  and kind == "presentation"):
                values["custom_downmix_data"] = True
            elif element == "de_par_code":
                values["de_parameters"] = True
            elif element == "b_advanced_de_data_present" and value:
                values["advanced_de_data"] = True
    if len(modes) > 1:
        values["codec_mode"] = sorted(modes)
    else:
        values["codec_mode"] = modes.pop() if modes else None
    return {key: values[key] for key in WALKED_KEYS}, diagnostics


def check_expected(leg, frame_rate_index, walked):
    expected = leg.get("expect") or {}
    found = {"frame_rate_index": frame_rate_index, **walked}
    wrong = {key: (want, found[key]) for key, want in expected.items() if found[key] != want}
    if wrong:
        raise SystemExit(f"{leg['name']}: expected (LEGS) and found (the stream) differ: {wrong}")


def manifest_entry(leg, wav, description, data, frame_count, frame_rate_index, walked, built):
    with wave.open(str(wav), "rb") as r:
        duration_s = r.getnframes() / r.getframerate()
    entry = {"encoder": leg["encoder"], "source": description}
    if not built:
        entry["source_wav"] = (wav.relative_to(REPO).as_posix() if wav.is_relative_to(REPO)
                               else wav.as_posix())
    entry.update({
        "output_channel_layout": leg["layout"],
        "bitrate_kbps": leg["kbps"],
        "options": leg["options"],
        "duration_s": duration_s,
        "source_sha256": sha256(wav),
        "size_bytes": len(data),
        "frame_count": frame_count,
        "frame_rate_index": frame_rate_index,
        "frame_len_base": FRAME_LEN_BASE[frame_rate_index],
        "walked_by": "tools/references/ac4_syntax.py",
    })
    entry.update(walked)
    return entry


def committed_run(args, version):
    """The committed set: every leg encoded and checked, then written with its digest."""
    scratch = args.scratch_dir.resolve()
    names = sorted({leg["source"] for leg in LEGS} - {"reference_stereo"})
    wavs = build_sources(scratch / "sources", COMMITTED_SECONDS, names)
    built = set(wavs)
    wavs["reference_stereo"] = AUDIO / "reference_stereo.wav"
    descriptions = source_descriptions(COMMITTED_SECONDS)
    descriptions["reference_stereo"] = source_descriptions(3.0)["reference_stereo"]

    # Every leg is encoded and checked before anything in the tree changes, so a refusal or a
    # failed check part-way through leaves the committed streams, the manifest and the digests
    # as they were.
    encoded = []
    for leg in LEGS:
        data = encode(leg, wavs[leg["source"]], scratch / leg["name"])
        frame_count, frame_rate_index = describe(leg["name"], data)
        walked, diagnostics = walk(data)
        if diagnostics:
            raise SystemExit(f"{leg['name']}: ac4_syntax.py did not read every substream to "
                             f"its end: {diagnostics[:5]}")
        check_expected(leg, frame_rate_index, walked)
        dest = OUT / leg["name"] / "dee.ac4"
        if leg.get("pinned") and dest.is_file() and dest.read_bytes() != data:
            raise SystemExit(f"{leg['name']}: this encode differs from the committed "
                             f"{dest.relative_to(REPO)}, which tests pin. Replace it by hand, "
                             "together with the tests that read it, if that is intended.")
        encoded.append((leg, data, frame_count, frame_rate_index, walked))

    manifest = {"baseline_version": BASELINE_VERSION, "dee_version": version, "legs": {}}
    for leg, data, frame_count, frame_rate_index, walked in encoded:
        dest = OUT / leg["name"] / "dee.ac4"
        if dest.is_file() and dest.read_bytes() == data:
            print(f"unchanged {dest} ({len(data)} bytes)")
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            print(f"wrote {dest} ({len(data)} bytes)")
        label = f"{leg['name']}/dee.ac4"
        digest = DIGESTS / f"{leg['name']}.tsv"
        digest.write_text("\n".join(ac4_syntax.digest_lines(data, label)) + "\n",
                          encoding="utf-8", newline="\n")
        manifest["legs"][leg["name"]] = manifest_entry(
            leg, wavs[leg["source"]], descriptions[leg["source"]], data, frame_count,
            frame_rate_index, walked, leg["source"] in built)

    manifest_path = OUT / "ac4-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8",
                             newline="\n")
    print(f"wrote {manifest_path} and {len(encoded)} digests under {DIGESTS.relative_to(REPO)}")


def mediainfo(stream, work):
    """MediaInfo's frame-by-frame trace and its summary, beside the stream."""
    for args, name in ((["--Details=1"], "mediainfo-details.txt"),
                       (["--Output=JSON"], "mediainfo.json")):
        result = subprocess.run([str(MEDIAINFO), *args, str(stream)], capture_output=True,
                                text=True, check=False)
        (work / name).write_text(result.stdout, encoding="utf-8")


def object_legs(masters):
    legs = []
    for master in masters:
        stem = master.stem
        legs += [
            {"name": f"ajoc-l3-448-{stem}", "encoder": AJOC, "source": master, "layout": "A-JOC",
             "kbps": 448, "options": ["--level", "3", "--data-rate", "448"]},
            {"name": f"ajoc-l4-256-{stem}", "encoder": AJOC, "source": master, "layout": "A-JOC",
             "kbps": 256, "options": ["--level", "4", "--data-rate", "256"]},
            {"name": f"ims-atmos-128-{stem}", "encoder": IMS, "source": master, "layout": "IMS",
             "kbps": 128, "options": [], "input_format": "atmos_mezz"},
        ]
    return legs


def gold_run(args, version):
    """The gold set, under args.gold_set; reuses a stream whose command and source are
    unchanged since the run that made it."""
    root = args.gold_set.resolve()
    streams = root / "streams"
    legs = gold_legs()
    names = sorted({leg["source"] for leg in legs})
    wavs = build_sources(root / "sources", GOLD_SECONDS, names)
    descriptions = source_descriptions(GOLD_SECONDS)
    sources = {name: {"description": descriptions[name], "sha256": sha256(wav),
                      "seconds": GOLD_SECONDS} for name, wav in wavs.items()}
    masters = [m.resolve() for m in args.adm_master or []]
    for master in masters:
        wavs[master] = master
        descriptions[master] = f"Atmos master {master.as_posix()}"
        sources[master.as_posix()] = {"description": descriptions[master],
                                      "sha256": sha256(master)}
    manifest = {"gold_version": BASELINE_VERSION, "dee_version": version,
                "mediainfo_version": mediainfo_version(),
                "made": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "seconds": GOLD_SECONDS, "sources": sources, "legs": {}}
    all_legs = legs + object_legs(masters)
    for number, leg in enumerate(all_legs, 1):
        work = streams / leg["name"]
        wav = wavs[leg["source"]]
        cmd = command(leg, wav, work / "dee.ac4", work)
        stamp = {"command": subprocess.list2cmdline(cmd), "source_sha256": sha256(wav)}
        leg_json = work / "leg.json"
        stream = work / "dee.ac4"
        reuse = (stream.is_file() and leg_json.is_file()
                 and json.loads(leg_json.read_text(encoding="utf-8")) == stamp)
        refusal_ok = leg["encoder"] == AJOC or leg.get("input_format") == "atmos_mezz"
        data = stream.read_bytes() if reuse else encode(leg, wav, work, refusal_ok)
        entry = {"encoder": leg["encoder"], "source": str(leg["source"]),
                 "output_channel_layout": leg["layout"], "bitrate_kbps": leg["kbps"],
                 "options": leg["options"], "input_format": leg.get("input_format", "wav")}
        if data is None:
            log = (work / "dee_output.txt").read_text(errors="replace")
            entry["refused"] = log.strip().splitlines()[-12:]
            manifest["legs"][leg["name"]] = entry
            print(f"[{number}/{len(all_legs)}] {leg['name']}: refused by DEE")
            continue
        if not reuse:
            leg_json.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
            mediainfo(stream, work)
        frame_count, frame_rate_index = describe(leg["name"], data)
        walked, diagnostics = walk(data)
        entry.update({"size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                      "frame_count": frame_count, "frame_rate_index": frame_rate_index,
                      "frame_len_base": FRAME_LEN_BASE[frame_rate_index],
                      "walked_by": "tools/references/ac4_syntax.py", **walked,
                      "walk_failures": [d for d in diagnostics if "FAIL" in d][:10],
                      "walk_refusals": sorted({d.split(": ", 1)[-1] for d in diagnostics
                                               if "refused" in d})})
        manifest["legs"][leg["name"]] = entry
        print(f"[{number}/{len(all_legs)}] {leg['name']}: {'reused' if reuse else 'encoded'}, "
              f"{frame_count} frames, {walked['codec_mode']}")
    path = root / "gold-manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    refused = [name for name, e in manifest["legs"].items() if "refused" in e]
    failed = [name for name, e in manifest["legs"].items() if e.get("walk_failures")]
    print(f"wrote {path}: {len(manifest['legs'])} legs, {len(refused)} refused by DEE, "
          f"{len(failed)} with walk failures")


def main():
    guard_not_ci()
    parser = argparse.ArgumentParser(description="Generate the AC-4 DEE streams.")
    parser.add_argument("--scratch-dir", type=Path, default=SCRATCH,
                        help="where the committed set's sources, encodes and DEE logs go "
                             "(default: build/ac4_baseline_scratch)")
    parser.add_argument("--gold-set", type=Path, metavar="DIR",
                        help=f"make the local gold set in DIR (e.g. {GOLD_DIR_DEFAULT.as_posix()}) "
                             "instead of the committed set")
    parser.add_argument("--adm-master", type=Path, action="append", metavar="WAV",
                        help="with --gold-set: an Atmos ADM BWF master to try the A-JOC and "
                             "immersive stereo encoders on; repeatable")
    args = parser.parse_args()
    if args.adm_master and not args.gold_set:
        parser.error("--adm-master needs --gold-set")

    encoders = (AC4, IMS, AJOC) if args.adm_master else (AC4, IMS)
    for encoder in encoders:
        if not dee_exe(encoder).exists():
            raise SystemExit(f"{dee_exe(encoder)} not found - this generator only runs on a "
                             "machine with Dolby Media Encoder installed.")
    versions = {dee_version(encoder) for encoder in encoders}
    if len(versions) != 1:
        raise SystemExit(f"{encoders} report different DEE versions: {sorted(versions)}")
    if args.gold_set:
        if not MEDIAINFO.exists():
            raise SystemExit(f"{MEDIAINFO} not found; the gold set records MediaInfo's trace.")
        gold_run(args, versions.pop())
    else:
        committed_run(args, versions.pop())


if __name__ == "__main__":
    main()
