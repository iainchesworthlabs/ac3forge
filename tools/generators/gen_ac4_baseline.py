"""Generates the Dolby Encoding Engine's AC-4 streams this project checks itself against.

Two sets, from the same legs and sources:

- The committed set: tests/golden/external-baseline/ac4-*/dee.ac4, ac4-manifest.json beside
  them, and each stream's syntax digest under tests/golden/ac4dec/. Short streams that CI reads.
- The gold set (--gold-set DIR): every stream the phases of planning/ac4.md need, kept on a
  local disk and never committed. Phase G0 of that plan made it and phase G1 added to it, both
  while the local DEE licence ran (it ends on 2026-11-06 and is not renewed, so no DEE stream
  can be made after that date).

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
  ac4-514-tones-256      ac4  tones_514          256  immersive ASPX_ACPL_2; one tone per channel
  ac4-514-tones-512      ac4  tones_514          512  immersive ASPX_SCPL; one tone per channel
  ac4-514-tones-768      ac4  tones_514          768  immersive SCPL; one tone per channel

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

The three 5.1.4 legs (phase G1) are one tone per channel in each immersive codec mode DEE
writes, for the phases from D9 on. Both transcriptions read immersive_channel_element() since
phase D9, and these legs' digests are written with the others'.

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

Manifest. baseline_version 5 (see BASELINE_VERSION). One entry per leg:

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
  immersive_codec_mode, input_format (the 5.1.4 legs)
      immersive_codec_mode is TS 103 190-2 Table 73's immersive_codec_mode_code, as the trace
      records it: one bit for ASPX_AJCC, three for the others. input_format is the cbi_wav
      dee_ac4_encoder took the ten channels as.

The gold set (--gold-set DIR). DIR/sources holds the sources; DIR/streams/<leg> holds each
stream (dee.ac4, or dee.ec3 and dee.ac3 for G1's E-AC-3 and AC-3 legs), DEE's log and output
manifest, MediaInfo's frame-by-frame trace (mediainfo-details.txt, from --Details=1) and summary
(mediainfo.json), and leg.json, which lets a rerun reuse a stream whose command and source have
not changed;
DIR/gold-manifest.json describes every leg as ac4-manifest.json does, with the stream's SHA-256
and each walk's diagnostics. G0's legs (gold_legs(), the manifest's "legs") are every layout
and rate dee_ac4_encoder writes, from 2.0 at 48 kbps to 5.1.4 at 768, for music, speech or
film, and tones; dee_ac4ims_encoder at every rate and frame rate; and metadata legs: each DRC
profile, the per-device profiles, each preferred downmix and some mix levels, I-frame
intervals, loudness presets, the height downmix, and the IMS encoder's DRC settings. Both
transcriptions refused 5.1.4's audio until phase D9, so the gold manifests G0 and G1 wrote
record its codec mode as null, and G1's its immersive_codec_mode beside it.

Objects (--adm-master WAV, repeatable, with --gold-set). dee_ac4ajoc_encoder accepts only an
Atmos master. Each master given is tried once with dee_ac4ajoc_encoder at levels 3 and 4 and
with dee_ac4ims_encoder; a refusal is recorded with DEE's message, not raised.

Phase G1 (gold_version 4, g1_legs()). A run over an existing gold set copies G0's legs, sources
and entries as they are, never rewrites a file of theirs, and writes the manifest by replacing
it whole. G1's legs go under the manifest's "g1_legs", beside "legs": the scorers that read
"legs" fail any leg they have not pinned, so a phase reads "g1_legs" when it pins what it needs
from them. Each G1 leg names its group and the phases it serves (G1_GROUPS):

  group               for          what
  synthetic           D2-D5 E1-E4  sweeps, pink noise and silence-then-transient at every 2.0
                                   and 5.1 rate
  immersive           D9 E8        film, speech, sweeps, noise and transients at every 5.1.4
                                   rate, each with its immersive codec mode
  immersive-metadata  D9 E8        5.1.4: stepped tones under each DRC profile, the preferred
                                   downmixes, every mix level and every height downmix mode and
                                   gain on one tone per channel, loudness presets, I-frames
  seven-one           D4 E3 I1     7.1 input, which dee_ac4_encoder writes as 5.1
  encode-downmix      I1 E3        5.1 and 7.1 input written as 2.0: DEE's own downmix at
                                   encoding
  frame-rates         D6 E5 D11    immersive stereo at every rate and frame rate, with tones,
                                   sweeps, noise and transients, and the I-frame intervals each
                                   frame rate allows
  metadata-20         D6 E5        2.0: each DRC profile on music and on stepped tones, the
                                   loudness presets, corrected loudness targets from -31 to -10,
                                   dialogue intelligence, I-frame intervals and forced I-frames
  metadata-51         D6 E5        5.1: every mix level on one tone per channel, stepped tones
                                   under each DRC profile, the other loudness presets and
                                   targets, dialogue intelligence, forced I-frames
  metadata-ims        D6 E5 D7     immersive stereo: each DRC profile, loudness presets, seven
                                   language tags on six sources, music mode at the video frame
                                   rates (refused: DEE's message is the record), and a leveled leg
  presentations       D7 E6        substreams for the test multiplexer: a dialogue tone, an
                                   associated tone, dialogue and associated speech in 2.0, 10 s
                                   on the same I-frame grid as G0's 2.0 and 5.1 legs
  programme           D12 D13 I6   60 s programmes at the common layouts and rates
  gapless             D6 E5 I1     one 60 s programme in three parts through the immersive
                                   stereo encoder's gapless encoding (several inputs in one run,
                                   dee.ac4, dee-2.ac4, dee-3.ac4), at three frame rates: streams
                                   that meet at splices DEE made
  joc                 I1 I5        E-AC-3 JOC from dee_ddpjoc_encoder's channel-based immersive
                                   input: 5.1.4, 7.1.4 and 9.1.6
  eac3                I1           E-AC-3 and AC-3 from dee_ddp_encoder, from the same sources
  objects             D10 E9 I5    A-JOC, immersive stereo and E-AC-3 JOC from ADM BWF masters
                                   this project writes (--cli): refused, see below

G1's sources (g1_source_specs() describes each) are the same fixtures cut further: 7.1, 7.1.4
and 9.1.6 beds and tones (the back surrounds, wides and top middles take tones and music cuts of
their own), film and speech at 5.1.4, the synthetic signals, the presentation substreams,
stepped tones, and 60 s programmes, with -6 dB copies of the loud 5.1 sources for the immersive
stereo encoder, which levels input it measures above about -16 LKFS.

Besides what G0 keeps, each G1 leg keeps MediaInfo's trace with --ParseSpeed=1 (a table of
contents for every frame) and DEE's MP4 muxer's file (dee.mp4, with its log and MediaInfo's
summary), and its entry records the command, DEE's warnings and loudness measurement, every
SHA-256, the table of contents' channel modes, language tags and I-frames, and, with --cli,
what that ac3cli's probe and decode made of the stream. G0's streams get their MP4 under
DIR/mp4/<leg>/. With --cli, the objects group builds its ADM BWF masters with that ac3cli
(DIR/masters/<name>/, atmos-encode then decode's adm_out) and checks them with atmos_info.
Every AC-4 leg's syntax digest goes to <scratch>/census/, for the census comparison:
AC4DEC_GOLDEN_DIR=<scratch>/census AC4DEC_STREAM_DIR=DIR/streams ac3tests "[ac4dec][syntax]".

What DEE 6.5.4 could not be made to write (G1's audit, recorded in the manifest's g1_dee_cannot):
7.1 AC-4 (eight channels in come out as 5.1), 7.1.4 or 9.1.6 AC-4, any frame rate but index 13
from dee_ac4_encoder, several presentations, music and effects with dialogue, associated audio,
dialogue enhancement from a separate dialogue input, and any AC-4 from objects:
dee_ac4ajoc_encoder and dee_ac4ims_encoder take objects only from an Atmos master, and refuse
every master this project writes with "Content was not authored with Dolby tools", a provenance
check this project does not work around.

Usage (repo root):
  python tools/generators/gen_ac4_baseline.py [--scratch-dir DIR]
  python tools/generators/gen_ac4_baseline.py --gold-set D:/ac3bld/ac4-gold [--cli AC3CLI]
      [--jobs N] [--only NAME_PART ...] [--adm-master WAV]

Never run in CI - see guard_not_ci(), the same rule gen_external_baseline.py's own copy of
this function states.
"""

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import wave
from concurrent.futures import ProcessPoolExecutor, as_completed
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
MP4MUXER = DEE_DIR / "dee_mp4muxer.exe"
ATMOS_INFO = DEE_DIR / "atmos_info.exe"
AC4 = "dee_ac4_encoder"
IMS = "dee_ac4ims_encoder"
AJOC = "dee_ac4ajoc_encoder"
DDP = "dee_ddp_encoder"
DDPJOC = "dee_ddpjoc_encoder"

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
# 4: phase G1. Three committed 5.1.4 legs, one tone per channel in each immersive codec mode,
#    without digests until D9; the gold set's g1_legs (the module docstring's "Phase G1"),
#    with G0's legs, sources and files left as they were.
# 5: phase D9. The 5.1.4 legs' digests, now that both transcriptions read
#    immersive_channel_element(), and their immersive_codec_mode from its trace; the streams
#    are G1's bytes.
BASELINE_VERSION = 5

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

# TS 103 190-2 Table 73: immersive_codec_mode_code's two-bit values after a leading 0; a
# leading 1 alone is ASPX_AJCC.
IMMERSIVE_CODEC_MODES = ("SCPL", "ASPX_SCPL", "ASPX_ACPL_1", "ASPX_ACPL_2")

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
    # Phase G1: 5.1.4 in each immersive codec mode DEE writes, digested since phase D9.
    *({"name": f"ac4-514-tones-{kbps}", "encoder": AC4, "source": "tones_514", "layout": "5.1.4",
       "kbps": kbps, "options": MEASURE_ONLY, "input_format": "cbi_wav",
       "expect": {"frame_rate_index": 13, "codec_mode": None, "immersive_codec_mode": mode,
                  "custom_downmix_data": True}}
      for kbps, mode in ((256, "ASPX_ACPL_2"), (512, "ASPX_SCPL"), (768, "SCPL"))),
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


# ------------------------------------------------------------------------------------------
# Phase G1's sources and legs
# ------------------------------------------------------------------------------------------

# The phases each group of G1 legs is made for (planning/ac4.md).
G1_GROUPS = {
    "synthetic": ("D2", "D3", "D4", "D5", "E1", "E2", "E3", "E4"),
    "immersive": ("D9", "E8"),
    "immersive-metadata": ("D9", "E8"),
    "seven-one": ("D4", "E3", "I1"),
    "encode-downmix": ("I1", "E3"),
    "frame-rates": ("D6", "E5", "D11"),
    "metadata-20": ("D6", "E5"),
    "metadata-51": ("D6", "E5"),
    "metadata-ims": ("D6", "E5", "D7"),
    "presentations": ("D7", "E6"),
    "programme": ("D12", "D13", "I6"),
    "gapless": ("D6", "E5", "I1"),
    "joc": ("I1", "I5"),
    "eac3": ("I1",),
    "objects": ("D10", "E9", "I5"),
}

# One tone per speaker, for every layout G1 gives DEE: TONE_HZ's ten, then the back surrounds
# of 7.1, 7.1.4 and 9.1.6, and 9.1.6's wides and top middles. Primes, as TONE_HZ's are.
SPEAKER_HZ = {**dict(zip(("L", "R", "C", "LFE", "Ls", "Rs", "Tfl", "Tfr", "Tbl", "Tbr"),
                         TONE_HZ, strict=True)),
              "Lrs": 1777, "Rrs": 1933, "Lw": 2089, "Rw": 2243, "Ltm": 2399, "Rtm": 2551}

# Each layout's speakers in the order DEE's inputs take them: wav_list's L:R:C:LFE:LS:RS:LRS:RRS
# (--morehelp input-format), and the cbi_wav orders of Dolby Media Encoder's user guide, where
# 5.1.4's Lfh Rfh Lrh Rrh are Tfl Tfr Tbl Tbr here and 7.1.4 and 9.1.6 add Lrs Rrs, then 9.1.6
# Lw Rw before the heights and Ltm Rtm between them.
SPEAKERS = {
    "20": ("L", "R"),
    "51": ("L", "R", "C", "LFE", "Ls", "Rs"),
    "71": ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs"),
    "514": ("L", "R", "C", "LFE", "Ls", "Rs", "Tfl", "Tfr", "Tbl", "Tbr"),
    "714": ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs", "Tfl", "Tfr", "Tbl", "Tbr"),
    "916": ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs", "Lw", "Rw", "Tfl", "Tfr", "Ltm",
            "Rtm", "Tbl", "Tbr"),
}
ALL_SPEAKERS = SPEAKERS["916"]

DIALOGUE_TONE_HZ = 2711
ASSOCIATED_TONE_HZ = 2903
NOISE_RMS = 0.05  # -26 dBFS
# The immersive stereo encoder levels input it measures above about -16 LKFS (DEE's warning
# "Loud input PCM ... is being leveled"): one tone per channel at -20 dBFS measures -12.2 and is
# leveled, at -26 dBFS -18.2 and is not; noise_51 measures -15.7 and is leveled. Its copies of
# the loud 5.1 sources are 6 dB down.
IMS_SCALE = 0.5
# Stepped tones for the DRC profiles' static curves: a 997 Hz tone in every channel but the
# LFE, from -70 to -1 dBFS in 3 dB steps held 1.5 s each, stepping up, so each step settles
# through the compressor's attack rather than its slower release.
STEP_HZ = 997
STEP_LEVELS_DB = tuple(range(-70, 0, 3))
STEP_SECONDS = 1.5
# The 60 s programmes: music throughout, with the speech fixture's 0-20 s at 20 s and its
# 20-30 s at 45 s (at, from, seconds), the music ducked under it through 0.5 s ramps.
PROGRAMME_SECONDS = 60.0
PROGRAMME_DIALOGUE = ((20.0, 0.0, 20.0), (45.0, 20.0, 10.0))
# dee_ac4ims_encoder encodes several inputs in one run with seamless transitions (its
# --morehelp examples; the user guide's gapless encoding), so consecutive parts of one
# programme give streams that meet at a splice DEE made: programme_51_ims in three.
GAPLESS_PARTS = 3


def g1_source_specs():
    """{name: (seconds, description)} for every source G1's legs encode that G0 does not."""
    s = GOLD_SECONDS

    def tones_text(layout, scale=1.0):
        level = 20.0 * np.log10(TONE_AMPLITUDE * scale)
        return (f"one tone per channel, {' '.join(SPEAKERS[layout])}: "
                + ", ".join(str(SPEAKER_HZ[k]) for k in SPEAKERS[layout])
                + f" Hz at {level:.0f} dBFS; {s:g} s")

    bed = ("L R from 0 s, C mono from 10 s x 0.6, LFE mono from 0 s low-passed at 120 Hz x 0.8, "
           "Ls Rs from 20 s x 0.5 (music_51's)")
    backs = "Lrs Rrs from 7.5 s x 0.4"
    tops = "Tfl Tfr from 5 s x 0.4, Tbl Tbr from 15 s x 0.3 (music_514's)"
    sweep = ("a logarithmic sine sweep, 20 Hz to 20 kHz over the whole source at -20 dBFS with "
             "10 ms fades, in every channel but the LFE (20 to 120 Hz), started a 1/n of the "
             "length later in each of the n channels, circularly")
    noise = ("independent pink noise per channel at -26 dBFS RMS, from a splitmix64 counter "
             "seeded by speaker (so a speaker's noise is the same in every layout), shaped "
             "1/sqrt(f) above 20 Hz, 10 ms fades; the LFE's low-passed at 120 Hz")
    transient = ("1 s of digital silence, then a burst every second in each channel, the n "
                 "channels a 1/n of a second apart: 80 ms of noise decaying with a 10 ms time "
                 "constant from a -6 dBFS peak (the LFE's a 55 Hz tone decaying over 50 ms)")
    steps = (f"a {STEP_HZ} Hz tone in every channel but the LFE, stepping up from "
             f"{STEP_LEVELS_DB[0]} to {STEP_LEVELS_DB[-1]} dBFS in 3 dB steps of "
             f"{STEP_SECONDS:g} s with 5 ms ramps")
    programme = ("programme_music_stereo.flac repeated with 50 ms crossfades at each join, "
                 "with programme_speech_stereo.flac's 0-20 s at 20 s and its 20-30 s at 45 s, "
                 "the music ducked under the speech through 0.5 s ramps")
    ims = " x 0.5, for the immersive stereo encoder, which levels louder input"
    specs = {
        "tones_71": (s, tones_text("71")),
        "tones_714": (s, tones_text("714")),
        "tones_916": (s, tones_text("916")),
        "tones_51_ims": (s, tones_text("51", IMS_SCALE)),
        "music_71": (s, f"music 7.1 from programme_music_stereo.flac: {bed}, {backs}; {s:g} s"),
        "film_71": (s, "film 7.1: film_51 with Lrs Rrs the music from 7.5 s x 0.3; 10 s"),
        "music_714": (s, f"music 7.1.4: {bed}, {backs}, {tops}; {s:g} s"),
        "music_916": (s, f"music 9.1.6: {bed}, {backs}, Lw Rw from 12.5 s x 0.35, {tops}, "
                         f"Ltm Rtm from 17.5 s x 0.3; {s:g} s"),
        "film_514": (s, "film 5.1.4: film_51 with Tfl Tfr the music from 5 s x 0.3 and Tbl "
                        "Tbr from 15 s x 0.25; 10 s"),
        "speech_514": (s, "speech 5.1.4: programme_speech_stereo.flac mono from 0 s in C and its "
                          "stereo x 0.35 in L R, the music from 20 s x 0.15 in Ls Rs, from 5 s x "
                          "0.1 in Tfl Tfr and from 15 s x 0.08 in Tbl Tbr, no LFE; 10 s"),
        "dialogue_tone_20": (s, f"dialogue for the test multiplexer: a {DIALOGUE_TONE_HZ} Hz "
                                f"tone in L and R at -20 dBFS; {s:g} s"),
        "associated_tone_20": (s, f"associated audio for the test multiplexer: a "
                                  f"{ASSOCIATED_TONE_HZ} Hz tone in L and R at -20 dBFS; {s:g} s"),
        "dialogue_20": (s, "dialogue for the test multiplexer: programme_speech_stereo.flac's "
                           "mono mix from 0 s in L and R; 10 s"),
        "associated_20": (s, "associated audio for the test multiplexer: "
                             "programme_speech_stereo.flac's mono mix from 15 s x 0.7 in L and "
                             "R; 10 s"),
        "steps_20": (len(STEP_LEVELS_DB) * STEP_SECONDS, f"stepped tones, L R: {steps}"),
        "steps_51": (len(STEP_LEVELS_DB) * STEP_SECONDS, f"stepped tones, 5.1: {steps}"),
        "steps_514": (len(STEP_LEVELS_DB) * STEP_SECONDS, f"stepped tones, 5.1.4: {steps}"),
        "programme_20": (PROGRAMME_SECONDS, f"60 s programme, 2.0: {programme}; the speech's "
                                            "stereo over the music x (1 - 0.8 d), d the ducking "
                                            "envelope"),
        "programme_51": (PROGRAMME_SECONDS, f"60 s programme, 5.1: {programme}; the bed as "
                                            "music_51's, from 0, 10 and 20 s, with the speech's "
                                            "mono mix replacing C's music and L R x (1 - 0.55 d),"
                                            " Ls Rs x (1 - 0.3 d)"),
        "programme_514": (PROGRAMME_SECONDS, "60 s programme, 5.1.4: programme_51 with Tfl Tfr "
                                             "the music from 5 s x 0.4 and Tbl Tbr from 15 s x "
                                             "0.3, both x (1 - 0.5 d)"),
        "programme_51_ims": (PROGRAMME_SECONDS, f"programme_51{ims}"),
    }
    for part in range(GAPLESS_PARTS):
        span = PROGRAMME_SECONDS / GAPLESS_PARTS
        specs[f"programme_51_ims_{part + 1}"] = (
            span, f"programme_51_ims from {part * span:g} to {(part + 1) * span:g} s, part "
                  f"{part + 1} of {GAPLESS_PARTS} for the immersive stereo encoder's gapless "
                  "encoding")
    for layout in ("20", "51", "514"):
        specs[f"sweep_{layout}"] = (s, f"{sweep}; {s:g} s")
        specs[f"noise_{layout}"] = (s, f"{noise}; {s:g} s")
        specs[f"transient_{layout}"] = (s, f"{transient}; {s:g} s")
    specs["sweep_51_ims"] = (s, f"sweep_51{ims}")
    specs["noise_51_ims"] = (s, f"noise_51{ims}")
    specs["transient_51_ims"] = (s, f"transient_51{ims}")
    return specs


def fade(x, seconds=0.01):
    """x with raised-cosine fades of `seconds` at both ends."""
    n = round(seconds * RATE)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    y = np.array(x, dtype=np.float64)
    y[:n] *= ramp
    y[len(y) - n:] *= ramp[::-1]
    return y


def log_sweep(count, f0, f1, amplitude):
    """A logarithmic sine sweep from f0 to f1 Hz over count samples."""
    t = np.arange(count) / RATE
    length = count / RATE
    k = np.log(f1 / f0)
    return amplitude * np.sin(2.0 * np.pi * f0 * length / k * (np.exp(t / length * k) - 1.0))


def uniform_noise(count, seed):
    """count values uniform in [-1, 1): splitmix64 over a counter, so the same seed gives the
    same samples on any machine and any numpy, whose own generators may change their streams
    between releases."""
    z = (np.arange(1, count + 1, dtype=np.uint64)
         + np.uint64(seed) * np.uint64(1 << 32)) * np.uint64(0x9E3779B97F4A7C15)
    z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    z = z ^ (z >> np.uint64(31))
    return (z >> np.uint64(11)).astype(np.float64) * 2.0 ** -52 - 1.0


def pink_noise(count, seed, rms):
    """Noise shaped 1/sqrt(f) above 20 Hz (so its power falls 3 dB an octave), at `rms`."""
    spectrum = np.fft.rfft(uniform_noise(count, seed))
    bins = np.arange(len(spectrum), dtype=np.float64)
    low = bins < 20.0 * count / RATE
    spectrum[low] = 0.0
    spectrum[~low] /= np.sqrt(bins[~low])
    x = np.fft.irfft(spectrum, count)
    return x * (rms / np.sqrt(np.mean(x * x)))


def loop_cut(x, start_s, seconds, crossfade_s=0.05):
    """seconds of x from start_s, x repeated with a raised-cosine crossfade at each join."""
    f = round(crossfade_s * RATE)
    ramp = (0.5 - 0.5 * np.cos(np.pi * np.arange(f) / f)).reshape(-1, *([1] * (x.ndim - 1)))
    body = np.array(x[:len(x) - f], dtype=np.float64)
    body[:f] = x[:f] * ramp + x[len(x) - f:] * (1.0 - ramp)
    first = round(start_s * RATE) % len(body)
    count = round(seconds * RATE)
    return np.concatenate([body] * ((first + count) // len(body) + 1))[first:first + count]


def speaker_tones(speakers, seconds, scale=1.0):
    t = np.arange(round(seconds * RATE)) / RATE
    return [scale * TONE_AMPLITUDE * np.sin(2.0 * np.pi * SPEAKER_HZ[s] * t) for s in speakers]


def music_speakers(music, seconds):
    """Every speaker's music: music_51's and music_514's cuts (build_sources()), and cuts of
    their own for the back surrounds, wides and top middles."""
    t = max(5.0, seconds)
    mono = music.mean(axis=1)

    def pair(start, gain):
        return tuple(gain * c for c in cut(music, start, seconds).T)

    out = {}
    out["L"], out["R"] = cut(music, 0.0, seconds).T
    out["C"] = 0.6 * cut(mono, t, seconds)
    out["LFE"] = 0.8 * lowpass(cut(mono, 0.0, seconds))
    out["Ls"], out["Rs"] = pair(2 * t, 0.5)
    out["Tfl"], out["Tfr"] = pair(seconds / 2, 0.4)
    out["Tbl"], out["Tbr"] = pair(1.5 * seconds, 0.3)
    out["Lrs"], out["Rrs"] = pair(0.75 * seconds, 0.4)
    out["Lw"], out["Rw"] = pair(1.25 * seconds, 0.35)
    out["Ltm"], out["Rtm"] = pair(1.75 * seconds, 0.3)
    return out


def sweeps(speakers, seconds):
    count = round(seconds * RATE)
    full = fade(log_sweep(count, 20.0, 20000.0, TONE_AMPLITUDE))
    lfe = fade(log_sweep(count, 20.0, 120.0, TONE_AMPLITUDE))
    mains = [s for s in speakers if s != "LFE"]
    return [lfe if s == "LFE" else np.roll(full, round(mains.index(s) * count / len(mains)))
            for s in speakers]


def noises(speakers, seconds):
    count = round(seconds * RATE)
    cols = []
    for s in speakers:
        x = pink_noise(count, ALL_SPEAKERS.index(s) + 1, NOISE_RMS)
        if s == "LFE":
            x = lowpass(x)
            x *= NOISE_RMS / np.sqrt(np.mean(x * x))
        cols.append(fade(x))
    return cols


def transients(speakers, seconds):
    count = round(seconds * RATE)
    t = np.arange(round(0.08 * RATE)) / RATE
    click = uniform_noise(len(t), 99) * np.exp(-t / 0.010)
    click *= 0.5 / np.max(np.abs(click))
    t_lfe = np.arange(round(0.3 * RATE)) / RATE
    thump = np.sin(2.0 * np.pi * 55.0 * t_lfe) * np.exp(-t_lfe / 0.05)
    thump *= 0.5 / np.max(np.abs(thump))
    cols = []
    for i, s in enumerate(speakers):
        shape = thump if s == "LFE" else click
        x = np.zeros(count)
        start = 1.0 + i / len(speakers)
        while start + len(shape) / RATE <= seconds:
            first = round(start * RATE)
            x[first:first + len(shape)] += shape
            start += 1.0
        cols.append(x)
    return cols


def stepped_tones(speakers):
    n = round(STEP_SECONDS * RATE)
    gain = np.repeat(10.0 ** (np.array(STEP_LEVELS_DB, dtype=np.float64) / 20.0), n)
    kernel = np.hanning(round(0.005 * RATE))
    gain = np.convolve(gain, kernel / kernel.sum(), mode="same")
    x = fade(gain * np.sin(2.0 * np.pi * STEP_HZ * np.arange(len(gain)) / RATE))
    return [np.zeros_like(x) if s == "LFE" else x for s in speakers]


def programme_parts(speech):
    """The 60 s programmes' pieces: the ducking envelope d, 1 under the speech and 0 elsewhere
    with 0.5 s raised-cosine ramps centred on each edge, and the speech placed in time."""
    count = round(PROGRAMME_SECONDS * RATE)
    t = np.arange(count) / RATE
    duck = np.zeros(count)
    placed = np.zeros((count, 2))

    def rise(u):
        return 0.5 - 0.5 * np.cos(np.pi * np.clip(u, 0.0, 1.0))

    for at, start, length in PROGRAMME_DIALOGUE:
        first = round(at * RATE)
        piece = cut(speech, start, length)
        placed[first:first + len(piece)] = piece
        duck += rise((t - at + 0.25) / 0.5) * rise((at + length + 0.25 - t) / 0.5)
    return duck, placed


def g1_mixes(music, speech, names):
    """{name: columns} for the named G1 sources (g1_source_specs())."""
    s = GOLD_SECONDS
    bed = music_speakers(music, s)
    speech_mono = speech.mean(axis=1)
    film = {"L": 0.45 * bed["L"], "R": 0.45 * bed["R"], "C": cut(speech_mono, 0.0, s),
            "LFE": bed["LFE"], "Ls": 0.7 * bed["Ls"], "Rs": 0.7 * bed["Rs"]}
    builders = {
        "tones_71": lambda: speaker_tones(SPEAKERS["71"], s),
        "tones_714": lambda: speaker_tones(SPEAKERS["714"], s),
        "tones_916": lambda: speaker_tones(SPEAKERS["916"], s),
        "tones_51_ims": lambda: speaker_tones(SPEAKERS["51"], s, IMS_SCALE),
        "music_71": lambda: [bed[k] for k in SPEAKERS["71"]],
        "music_714": lambda: [bed[k] for k in SPEAKERS["714"]],
        "music_916": lambda: [bed[k] for k in SPEAKERS["916"]],
        "film_71": lambda: [*(film[k] for k in SPEAKERS["51"]), 0.75 * bed["Lrs"],
                            0.75 * bed["Rrs"]],
        "film_514": lambda: [*(film[k] for k in SPEAKERS["51"]), 0.75 * bed["Tfl"],
                             0.75 * bed["Tfr"], bed["Tbl"] * (0.25 / 0.3),
                             bed["Tbr"] * (0.25 / 0.3)],
        "speech_514": lambda: [*(0.35 * c for c in cut(speech, 0.0, s).T),
                               cut(speech_mono, 0.0, s), np.zeros(round(s * RATE)),
                               0.3 * bed["Ls"], 0.3 * bed["Rs"], 0.25 * bed["Tfl"],
                               0.25 * bed["Tfr"], bed["Tbl"] * (0.08 / 0.3),
                               bed["Tbr"] * (0.08 / 0.3)],
        "dialogue_tone_20": lambda: 2 * [TONE_AMPLITUDE * np.sin(
            2.0 * np.pi * DIALOGUE_TONE_HZ * np.arange(round(s * RATE)) / RATE)],
        "associated_tone_20": lambda: 2 * [TONE_AMPLITUDE * np.sin(
            2.0 * np.pi * ASSOCIATED_TONE_HZ * np.arange(round(s * RATE)) / RATE)],
        "dialogue_20": lambda: 2 * [cut(speech_mono, 0.0, s)],
        "associated_20": lambda: 2 * [0.7 * cut(speech_mono, 15.0, s)],
        "steps_20": lambda: stepped_tones(SPEAKERS["20"]),
        "steps_51": lambda: stepped_tones(SPEAKERS["51"]),
        "steps_514": lambda: stepped_tones(SPEAKERS["514"]),
        "programme_20": lambda: programme(music, speech, "20"),
        "programme_51": lambda: programme(music, speech, "51"),
        "programme_514": lambda: programme(music, speech, "514"),
        "sweep_51_ims": lambda: [IMS_SCALE * c for c in sweeps(SPEAKERS["51"], s)],
        "noise_51_ims": lambda: [IMS_SCALE * c for c in noises(SPEAKERS["51"], s)],
        "transient_51_ims": lambda: [IMS_SCALE * c for c in transients(SPEAKERS["51"], s)],
        "programme_51_ims": lambda: [IMS_SCALE * c for c in programme(music, speech, "51")],
    }
    for layout in ("20", "51", "514"):
        builders[f"sweep_{layout}"] = lambda k=layout: sweeps(SPEAKERS[k], s)
        builders[f"noise_{layout}"] = lambda k=layout: noises(SPEAKERS[k], s)
        builders[f"transient_{layout}"] = lambda k=layout: transients(SPEAKERS[k], s)
    span = round(PROGRAMME_SECONDS / GAPLESS_PARTS * RATE)
    for part in range(GAPLESS_PARTS):
        builders[f"programme_51_ims_{part + 1}"] = lambda first=part * span: [
            IMS_SCALE * c[first:first + span] for c in programme(music, speech, "51")]
    return {name: builders[name]() for name in names}


def programme(music, speech, layout):
    """programme_20, programme_51 or programme_514 (g1_source_specs())."""
    p = PROGRAMME_SECONDS
    duck, placed = programme_parts(speech)
    if layout == "20":
        return list(((1.0 - 0.8 * duck)[:, None] * loop_cut(music, 0.0, p)
                     + duck[:, None] * placed).T)
    mono = music.mean(axis=1)
    front = loop_cut(music, 0.0, p)
    back = loop_cut(music, 20.0, p)
    cols = [(1.0 - 0.55 * duck) * front[:, 0], (1.0 - 0.55 * duck) * front[:, 1],
            0.6 * (1.0 - duck) * loop_cut(mono, 10.0, p) + duck * placed.mean(axis=1),
            0.8 * lowpass(loop_cut(mono, 0.0, p)),
            0.5 * (1.0 - 0.3 * duck) * back[:, 0], 0.5 * (1.0 - 0.3 * duck) * back[:, 1]]
    if layout == "514":
        top_front = loop_cut(music, 5.0, p)
        top_back = loop_cut(music, 15.0, p)
        cols += [0.4 * (1.0 - 0.5 * duck) * top_front[:, 0],
                 0.4 * (1.0 - 0.5 * duck) * top_front[:, 1],
                 0.3 * (1.0 - 0.5 * duck) * top_back[:, 0],
                 0.3 * (1.0 - 0.5 * duck) * top_back[:, 1]]
    return cols


DRC_PROFILES = ("film_light", "film_standard", "music_light", "music_standard", "speech")
PER_DEVICE = ("drc_profile=film_standard:drc_profile_portable_hp=speech:"
              "drc_profile_home_theatre=music_light:drc_profile_portable_spkr=music_standard:"
              "drc_profile_flat_panel=film_light")
LOUDNESS_PRESETS = ("atsc_a85", "ebu_r128", "freetv_op59", "arib_b32")
# measure_and_correct's loudness_target range is [-31, -10]; dialnorm follows the target, so
# these legs give phase D6's output-level check dialnorms from DEE across that range. The audio
# is changed (a gain and DEE's -2 dBTP limiter), which each entry's audio_altered says.
LOUDNESS_TARGETS = (-31, -27, -24, -20, -17, -14, -10)
# Every value dee_ac4_encoder's --morehelp encoder lists for each mix level, less the default,
# -3, which the plain legs carry.
MIX_LEVELS = {"loro_cmix": ("-6", "-4.5", "-1.5", "0", "+1.5", "+3"),
              "loro_smix": ("-inf", "-6", "-4.5", "-1.5"),
              "ltrt_cmix": ("-6", "-4.5", "-1.5", "0", "+1.5", "+3"),
              "ltrt_smix": ("-inf", "-6", "-4.5", "-1.5")}
HEIGHT_MODES = ("front", "front_and_surround", "surround")
HEIGHT_GAINS = ("-inf", "-12", "-9", "-6", "-4.5", "-3", "-1.5", "0")
DMX_MODES = ("loro", "ltrt", "ltrt-pl2", "not_indicated")
# Forced I-frames: frame indices off DEE's one-second grid (frames 0, 1, then every 23 or 24).
FORCED_IFRAMES = (0, 5, 17, 40, 41, 100, 150, 151, 200)
# The immersive stereo encoder's I-frame interval ranges by frame rate (--morehelp encoder).
IMS_IFRAMES = (("23.976", 12), ("24", 12), ("25", 12), ("25", 50), ("25", 1000),
               ("29.97", 15), ("29.97", 60), ("native", 11), ("native", 48))
# Language tags for the immersive stereo encoder's language_tag (IETF BCP 47), each on content
# of its own, so a presentation chosen by language can be told by its audio.
IMS_LANGUAGES = (("en", "film_51"), ("eng", "film_51"), ("fr-CA", "music_51"),
                 ("de", "tones_51_ims"), ("es", "sweep_51_ims"), ("pt-BR", "noise_51_ims"),
                 ("ja", "transient_51_ims"))
# dee_ddp_encoder's production defaults off, as gen_external_baseline.py's _DEE_ENCODER_OPTS.
DDP_OFF = "surround_90deg_phase_shift=0:lfe_filter=0"


def fps_tag(fps):
    return fps.replace(".", "")


def level_tag(value):
    return value.replace("+", "p").replace("-", "m")


def g1_legs():
    """G1's legs, by group (G1_GROUPS); the module docstring's "Phase G1" says what each is."""
    legs = []

    def add(group, name, encoder, source, layout, kbps, options=(), input_format="wav",
            **extra):
        legs.append({"name": name, "group": group, "phases": list(G1_GROUPS[group]),
                     "encoder": encoder, "source": source, "layout": layout, "kbps": kbps,
                     "options": list(options), "input_format": input_format, **extra})

    mo = MEASURE_ONLY

    def encoder_opt(setting):
        return [*mo, "--encoder", setting]

    for tag in ("sweep", "noise", "transient"):
        for kbps in STEREO_RATES:
            add("synthetic", f"20-{tag}-{kbps}", AC4, f"{tag}_20", "stereo", kbps, mo)
        for kbps in FIVE_ONE_RATES:
            add("synthetic", f"51-{tag}-{kbps}", AC4, f"{tag}_51", "5.1", kbps, mo)

    for tag in ("film", "speech", "sweep", "noise", "transient"):
        for kbps in FIVE_ONE_FOUR_RATES:
            add("immersive", f"514-{tag}-{kbps}", AC4, f"{tag}_514", "5.1.4", kbps, mo,
                "cbi_wav")

    def imm(name, source, setting=None, options=None):
        opts = options if options is not None else (encoder_opt(setting) if setting else mo)
        add("immersive-metadata", name, AC4, source, "5.1.4", 256, opts, "cbi_wav")

    for p in DRC_PROFILES:
        imm(f"514-steps-256-drc-{p}", "steps_514", f"drc_profile={p}")
    imm("514-music-256-drc-per-device", "music_514", PER_DEVICE)
    for m in DMX_MODES:
        imm(f"514-tones-256-dmx-{m}", "tones_514", f"preferred_downmix_mode={m}")
    for key, values in MIX_LEVELS.items():
        for value in values:
            imm(f"514-tones-256-mix-{key}-{level_tag(value)}", "tones_514", f"{key}={value}")
    for mode in HEIGHT_MODES:
        for gain in HEIGHT_GAINS:
            if (mode, gain) != ("surround", "-3"):
                imm(f"514-tones-256-height-{mode}-{level_tag(gain)}", "tones_514",
                    f"height_dmx_mode={mode}:height_dmx_gain={gain}")
    for preset in LOUDNESS_PRESETS:
        imm(f"514-music-256-loudness-{preset}", "music_514",
            options=["--loudness-management", f"measure_only:preset={preset}"])
    for n in (11, 48, 1000):
        imm(f"514-music-256-iframe-{n}", "music_514", f"iframe_interval={n}")
    add("immersive-metadata", "514-music-256-forced-iframes", AC4, "music_514", "5.1.4", 256,
        mo, "cbi_wav", forced_iframes=list(FORCED_IFRAMES))

    for kbps in (96, 128, 192, 384, 768):
        add("seven-one", f"71-tones-{kbps}", AC4, "tones_71", "auto", kbps, mo,
            input_layout="7.1")
    for tag in ("music", "film"):
        for kbps in (128, 192, 384):
            add("seven-one", f"71-{tag}-{kbps}", AC4, f"{tag}_71", "auto", kbps, mo,
                input_layout="7.1")
    add("seven-one", "71-tones-384-wavlist", AC4, "tones_71", "auto", 384, mo, "wav_list",
        input_layout="7.1")

    # DEE's own downmix when asked for fewer channels than it is given, a transcoder's reference.
    for tag, source, layout in (("51", "tones_51", "5.1"), ("51", "music_51", "5.1"),
                                ("71", "tones_71", "7.1")):
        kind = source.split("_")[0]
        add("encode-downmix", f"{tag}to20-{kind}-192", AC4, source, "stereo", 192, mo,
            input_layout=layout)

    for fps in IMS_FPS:
        for kbps in (96, 144, 256, 320):
            for tag in ("music", "film"):
                add("frame-rates", f"ims-{tag}-{kbps}-{fps_tag(fps)}", IMS, f"{tag}_51", "IMS",
                    kbps, [*mo, "--target-fps", fps])
    for kbps in IMS_RATES:
        add("frame-rates", f"ims-film-{kbps}-native", IMS, "film_51", "IMS", kbps, mo)
    for fps in (*IMS_FPS, "native"):
        fps_opt = [] if fps == "native" else ["--target-fps", fps]
        for kbps in (64, 128, 320):
            add("frame-rates", f"ims-tones-{kbps}-{fps_tag(fps)}", IMS, "tones_51_ims", "IMS",
                kbps, [*mo, *fps_opt])
        for tag, source in (("sweep", "sweep_51_ims"), ("noise", "noise_51_ims"),
                            ("transient", "transient_51_ims")):
            add("frame-rates", f"ims-{tag}-128-{fps_tag(fps)}", IMS, source, "IMS", 128,
                [*mo, *fps_opt])
    for fps, n in IMS_IFRAMES:
        fps_opt = [] if fps == "native" else ["--target-fps", fps]
        add("frame-rates", f"ims-music-128-{fps_tag(fps)}-iframe-{n}", IMS, "music_51", "IMS",
            128, [*mo, *fps_opt, "--encoder", f"iframe_interval={n}"])

    def meta20(name, source, options, **extra):
        add("metadata-20", name, AC4, source, "stereo", 192, options, **extra)

    for p in DRC_PROFILES:
        meta20(f"20-music-192-drc-{p}", "music_20", encoder_opt(f"drc_profile={p}"))
        meta20(f"20-steps-192-drc-{p}", "steps_20", encoder_opt(f"drc_profile={p}"))
    meta20("20-music-192-drc-per-device", "music_20", encoder_opt(PER_DEVICE))
    for preset in LOUDNESS_PRESETS:
        meta20(f"20-music-192-loudness-{preset}", "music_20",
               ["--loudness-management", f"measure_only:preset={preset}"])
    for target in LOUDNESS_TARGETS:
        meta20(f"20-music-192-target-{-target}", "music_20",
               ["--loudness-management", f"measure_and_correct:loudness_target={target}"],
               audio_altered=f"loudness corrected to {target} LKFS, -2 dBTP true-peak limiter")
    meta20("20-speech-192-di-0", "speech_20",
           ["--loudness-management", "measure_only:dialogue_intelligence=0"])
    for threshold in (0, 50, 100):
        meta20(f"20-speech-192-speech-threshold-{threshold}", "speech_20",
               ["--loudness-management", f"measure_only:speech_threshold={threshold}"])
    for n in (11, 48, 1000):
        meta20(f"20-music-192-iframe-{n}", "music_20", encoder_opt(f"iframe_interval={n}"))
    meta20("20-music-192-forced-iframes", "music_20", mo, forced_iframes=list(FORCED_IFRAMES))

    def meta51(name, source, options, **extra):
        add("metadata-51", name, AC4, source, "5.1", 192, options, **extra)

    for key, values in MIX_LEVELS.items():
        for value in values:
            meta51(f"51-tones-192-mix-{key}-{level_tag(value)}", "tones_51",
                   encoder_opt(f"{key}={value}"))
    for p in DRC_PROFILES:
        meta51(f"51-steps-192-drc-{p}", "steps_51", encoder_opt(f"drc_profile={p}"))
    for preset in ("freetv_op59", "arib_b32"):
        meta51(f"51-music-192-loudness-{preset}", "music_51",
               ["--loudness-management", f"measure_only:preset={preset}"])
    for target in LOUDNESS_TARGETS:
        meta51(f"51-music-192-target-{-target}", "music_51",
               ["--loudness-management", f"measure_and_correct:loudness_target={target}"],
               audio_altered=f"loudness corrected to {target} LKFS, -2 dBTP true-peak limiter")
    meta51("51-film-192-di-0", "film_51",
           ["--loudness-management", "measure_only:dialogue_intelligence=0"])
    for threshold in (0, 50, 100):
        meta51(f"51-film-192-speech-threshold-{threshold}", "film_51",
               ["--loudness-management", f"measure_only:speech_threshold={threshold}"])
    meta51("51-music-192-forced-iframes", "music_51", mo, forced_iframes=list(FORCED_IFRAMES))

    def meta_ims(name, source, options, fps=None, **extra):
        fps_opt = [] if fps is None else ["--target-fps", fps]
        add("metadata-ims", name, IMS, source, "IMS", 128, [*options, *fps_opt], **extra)

    for p in DRC_PROFILES:
        meta_ims(f"ims-music-128-native-drc-{p}", "music_51", encoder_opt(f"drc_profile={p}"))
    meta_ims("ims-music-128-native-drc-per-device", "music_51", encoder_opt(PER_DEVICE))
    for p in ("film_standard", "none"):
        meta_ims(f"ims-music-128-native-drcddp-{p}", "music_51",
                 encoder_opt(f"drc_profile_ddp={p}"))
    # Music mode at the video frame rates: DEE refuses each ("Music mode requires native frame
    # rate"), and the refusals stay in the manifest as the record of it.
    for fps in IMS_FPS:
        meta_ims(f"ims-music-128-{fps_tag(fps)}-musicmode", "music_51",
                 ["--loudness-management", "measure_only:dialogue_intelligence=0",
                  "--encoder", "mode=music:drc_profile=music_light"], fps, refusal_ok=True)
    # One tone per channel at -20 dBFS, which the encoder measures at -12.2 LKFS and levels:
    # what DEE does to loud input, kept apart from every other leg, which it leaves alone.
    add("metadata-ims", "ims-tones-128-native-leveled", IMS, "tones_51", "IMS", 128, mo,
        leveling_expected=True)
    for language, source in IMS_LANGUAGES:
        meta_ims(f"ims-{source.split('_')[0]}-128-native-lang-{language}", source,
                 encoder_opt(f"language_tag={language}"))
    for preset in LOUDNESS_PRESETS:
        meta_ims(f"ims-music-128-native-loudness-{preset}", "music_51",
                 ["--loudness-management", f"measure_only:preset={preset}"])
    meta_ims("ims-film-128-native-di-0", "film_51",
             ["--loudness-management", "measure_only:dialogue_intelligence=0"])

    for tag, source in (("dlgtone", "dialogue_tone_20"), ("assoctone", "associated_tone_20")):
        for kbps in (64, 128, 192):
            add("presentations", f"20-{tag}-{kbps}", AC4, source, "stereo", kbps, mo)
    for tag, source in (("dialogue", "dialogue_20"), ("associated", "associated_20")):
        for kbps in (64, 128):
            add("presentations", f"20-{tag}-{kbps}", AC4, source, "stereo", kbps, mo)

    for kbps in (48, 64, 96, 128, 192, 256):
        add("programme", f"20-programme-{kbps}", AC4, "programme_20", "stereo", kbps, mo)
    for kbps in (96, 128, 192, 256, 384, 448):
        add("programme", f"51-programme-{kbps}", AC4, "programme_51", "5.1", kbps, mo)
    for kbps in (192, 256, 512, 768):
        add("programme", f"514-programme-{kbps}", AC4, "programme_514", "5.1.4", kbps, mo,
            "cbi_wav")
    for kbps, fps in ((64, "native"), (128, "native"), (64, "25"), (128, "29.97"), (96, "24"),
                      (64, "23.976")):
        fps_opt = [] if fps == "native" else ["--target-fps", fps]
        add("programme", f"ims-programme-{kbps}-{fps_tag(fps)}", IMS, "programme_51_ims", "IMS",
            kbps, [*mo, *fps_opt])

    parts = [f"programme_51_ims_{part + 1}" for part in range(GAPLESS_PARTS)]
    for fps in ("native", "25", "29.97"):
        fps_opt = [] if fps == "native" else ["--target-fps", fps]
        add("gapless", f"ims-programme-128-{fps_tag(fps)}-gapless", IMS, parts[0], "IMS", 128,
            [*mo, *fps_opt], parts=parts)

    def joc(name, source, layout, kbps, input_format="cbi_wav", options=()):
        add("joc", name, DDPJOC, source, layout, kbps, [*mo, *options], input_format)

    for kbps in (384, 448, 576, 640, 768, 1024):
        joc(f"ddpjoc-514-tones-{kbps}", "tones_514", "5.1.4", kbps)
    for kbps in (448, 768):
        joc(f"ddpjoc-514-music-{kbps}", "music_514", "5.1.4", kbps)
    joc("ddpjoc-514-film-768", "film_514", "5.1.4", 768)
    for kbps in (448, 768, 1024):
        joc(f"ddpjoc-714-tones-{kbps}", "tones_714", "7.1.4", kbps)
    joc("ddpjoc-714-music-768", "music_714", "7.1.4", 768)
    for kbps in (768, 1024):
        joc(f"ddpjoc-916-tones-{kbps}", "tones_916", "9.1.6", kbps)
    joc("ddpjoc-916-music-1024", "music_916", "9.1.6", 1024)
    joc("ddpjoc-514-tones-768-trim-h6-s3", "tones_514", "5.1.4", 768,
        "cbi_wav:height_trim_5_1=-6:surround_trim_5_1=-3")
    joc("ddpjoc-514-tones-768-trim-h12-s9", "tones_514", "5.1.4", 768,
        "cbi_wav:height_trim_5_1=-12:surround_trim_5_1=-9")
    joc("ddpjoc-514-tones-768-ltrt", "tones_514", "5.1.4", 768,
        options=["--encoder", "preferred_downmix_mode=ltrt:ltrt_cmix=-1.5:ltrt_smix=-4.5"])
    joc("ddpjoc-514-programme-768", "programme_514", "5.1.4", 768)

    def eac3(name, codec, source, layout, kbps, setting=""):
        add("eac3", name, DDP, source, layout, kbps,
            ["--encoder", f"{codec}:{DDP_OFF}{':' + setting if setting else ''}", *mo],
            "wav_list")

    for tag in ("music", "speech", "tones"):
        for kbps in (128, 256):
            eac3(f"ddp-20-{tag}-{kbps}", "ddp", f"{tag}_20", "stereo", kbps)
    for tag in ("music", "film", "tones"):
        for kbps in (256, 448):
            eac3(f"ddp-51-{tag}-{kbps}", "ddp", f"{tag}_51", "5.1", kbps)
    for tag in ("tones", "music"):
        eac3(f"ddp-71-{tag}-448", "ddp", f"{tag}_71", "auto", 448)
    eac3("dd-20-music-192", "dd", "music_20", "stereo", 192)
    eac3("dd-51-film-448", "dd", "film_51", "5.1", 448)
    eac3("ddp-51-film-448-drc-ltrt", "ddp", "film_51", "5.1", 448,
         "drc_profile_line=film_standard:drc_profile_rf=speech:preferred_downmix_mode=ltrt:"
         "ltrt_cmix=-6:ltrt_smix=-inf:loro_cmix=0:loro_smix=-6")
    return legs


def master_legs(masters):
    """The objects group: each master tried at A-JOC levels 3 and 4, as immersive stereo, and
    as E-AC-3 JOC, once each (a refusal is recorded, not raised)."""
    legs = []
    for name, path in masters.items():
        common = {"group": "objects", "phases": list(G1_GROUPS["objects"]), "source": path,
                  "refusal_ok": True}
        legs += [
            {**common, "name": f"ajoc-l3-448-{name}", "encoder": AJOC, "layout": "A-JOC",
             "kbps": 448, "options": ["--level", "3", "--data-rate", "448"],
             "input_format": None},
            {**common, "name": f"ajoc-l4-256-{name}", "encoder": AJOC, "layout": "A-JOC",
             "kbps": 256, "options": ["--level", "4", "--data-rate", "256"],
             "input_format": None},
            {**common, "name": f"ims-atmos-128-{name}", "encoder": IMS, "layout": "IMS",
             "kbps": 128, "options": [], "input_format": "atmos_mezz"},
            {**common, "name": f"ddpjoc-atmos-768-{name}", "encoder": DDPJOC, "layout": "JOC",
             "kbps": 768, "options": [], "input_format": "atmos_mezz"},
        ]
    return legs


# ADM BWF masters for the objects group, made with ac3cli: `atmos-encode <in> objects.ec3 kbps
# objects [paths]` codes each input channel as an object, and `decode objects.ec3 bed.wav
# objects adm-master.wav` writes the Dolby Atmos Master ADM Profile BW64 its adm_out argument
# writes (planning/ac4.md, phase G0's objects). music51 is music_51's six channels (G0's master);
# refobjects the five committed reference objects at their authored positions.
MASTERS = {
    "music51": {"input": "sources/music_51.wav", "kbps": 768, "objects": 6, "paths": None},
    "refobjects": {"input": AUDIO / "reference_objects.wav", "kbps": 448, "objects": 5,
                   "paths": AUDIO / "reference_objects.paths"},
}

# What the audit found DEE 6.5.4 cannot be made to write; the manifest carries it as
# g1_dee_cannot, so a phase reading the set knows what to build elsewhere.
DEE_CANNOT = [
    "7.1 AC-4: dee_ac4_encoder takes 7.1 input (a WAV or wav_list of 8 channels) and writes 5.1; "
    "--output-channel-layout offers auto, stereo, 5.1 and 5.1.4 (the seven-one legs record "
    "what it does with the back surrounds)",
    "7.1.4 and 9.1.6 AC-4: dee_ac4_encoder refuses cbi_wav of 12 and 16 channels ('Invalid "
    "input. Unsupported channel count'); 5.1.4 is its only immersive layout, and a 5.1.4 "
    "input cannot be written as 5.1 or stereo",
    "frame rates other than frame_rate_index 13 from dee_ac4_encoder (--timecode-frame-rate "
    "does not change it); dee_ac4ims_encoder writes indices 0 to 3 and 13 only, and its music "
    "mode index 13 only ('Music mode requires native frame rate')",
    "several presentations, substream groups or audio substreams in one stream, music and "
    "effects with dialogue, associated audio, a dialogue enhancement substream, or dialogue "
    "enhancement from a separate dialogue input: no encoder, option or template reaches them",
    "A-JOC, direct-coded objects, OAMD, or immersive stereo from objects: dee_ac4ajoc_encoder "
    "and dee_ac4ims_encoder take objects only as an Atmos master (ADM BWF, MXF IAB or a Dolby "
    "Atmos master file set), and refuse every ADM BWF master ac3cli writes with 'Content was "
    "not authored with Dolby tools' (dlb::isAtmosMezzFile), as atmos_info and "
    "dee_ddpjoc_encoder do; a bare IAB is 'ATMOS_STORAGE_RES_UNSUPPORTED_MASTER_TYPE'",
    "mono, 3.0 or 5.0 AC-4: dee_ac4_encoder refuses input of 1, 3, 4 or 5 channels ('Invalid "
    "input. Unsupported channel count'), and 5.1 with a silent LFE is written as 5.1",
    "22.2, the speech frontend where it can be seen, A-JCC, ASPX_ACPL_1, 44.1 kHz (48 kHz input "
    "only), transmitted DRC gains, the CRC or the bitstream version, as the census of 2026-09-15 "
    "found (planning/ac4.md, What DEE writes)",
]


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


def read_fixtures(scratch_dir):
    """The music and speech fixtures, (music, speech)."""
    return (read_fixture(AUDIO / "programme_music_stereo.flac", scratch_dir),
            read_fixture(AUDIO / "programme_speech_stereo.flac", scratch_dir))


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


def wav24_bytes(columns):
    """A 24-bit PCM WAV of the columns, as write_wav24() writes it."""
    pcm = np.stack(columns, axis=1)
    peak = float(np.max(np.abs(pcm)))
    if peak > 0.98:
        pcm *= 0.98 / peak
    samples = np.round(pcm * 8388607.0).astype("<i4")
    frames = samples.view(np.uint8).reshape(len(samples), -1, 4)[:, :, :3]
    with tempfile.SpooledTemporaryFile() as f:
        with wave.open(f, "wb") as w:
            w.setnchannels(pcm.shape[1])
            w.setsampwidth(3)
            w.setframerate(RATE)
            w.writeframes(frames.tobytes())
        f.seek(0)
        return f.read()


def write_wav24(path, columns):
    Path(path).write_bytes(wav24_bytes(columns))


def replace_file(path, data):
    """Writes data to path by writing a temporary file beside it and renaming it over path, so a
    reader sees the old file or the new one, never a partial one. Windows refuses the rename
    while another process holds path open, so it is retried for a while."""
    path = Path(path)
    temporary = path.with_name(path.name + f".tmp{os.getpid()}")
    temporary.write_bytes(data)
    for attempt in range(60):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if attempt == 59:
                raise
            time.sleep(1.0)


def g0_mixes(music, speech, seconds, names):
    """{name: columns} for the named G0 sources (the module docstring's Sources)."""
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
    return {name: mixes[name] for name in names}


def build_sources(source_dir, seconds, names):
    """Writes the named sources' WAVs (see the module docstring's Sources); returns their
    paths."""
    source_dir.mkdir(parents=True, exist_ok=True)
    music, speech = read_fixtures(source_dir)
    mixes = g0_mixes(music, speech, seconds, names)
    paths = {}
    for name in names:
        paths[name] = source_dir / f"{name}.wav"
        write_wav24(paths[name], mixes[name])
    return paths


def command(leg, wav, out, work):
    """DEE's command line for one of G0's legs, and the committed set's."""
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


def immersive_codec_mode(width, value):
    """TS 103 190-2 Table 73's name for an immersive_codec_mode_code record: one bit is
    ASPX_AJCC, and three bits, a 0 and two more, the others."""
    if width == 1:
        return "ASPX_AJCC"
    return IMMERSIVE_CODEC_MODES[value & 3]


def walk_all(data, label=None):
    """What every frame carries, from tools/references/ac4_syntax.py's trace (the module
    docstring's Manifest section defines each value): (values, diagnostics, the syntax
    digest's lines when label names the stream, the immersive codec modes found)."""
    diagnostics = []
    modes = set()
    immersive = set()
    lines = [f"# ac4-syntax-digest/1 {label}", ac4_syntax.DIGEST_COLUMNS] if label else None
    values = dict.fromkeys(WALKED_KEYS[1:], False)
    for frame, substream, kind, records in ac4_syntax.walk_stream(data, True, diagnostics):
        if lines is not None:
            lines.append(ac4_syntax.digest_line(frame, substream, kind, records))
        for _offset, width, value, element in records:
            if element in CODEC_MODES:
                names = CODEC_MODES[element]
                modes.add(names[value] if value < len(names) else f"{element} {value}")
            elif element == "immersive_codec_mode_code":
                immersive.add(immersive_codec_mode(width, value))
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
    return {key: values[key] for key in WALKED_KEYS}, diagnostics, lines, sorted(immersive)


def walk(data):
    """(values, diagnostics): walk_all()'s first two."""
    values, diagnostics, _, _ = walk_all(data)
    return values, diagnostics


def check_expected(leg, frame_rate_index, walked, immersive=None):
    expected = leg.get("expect") or {}
    found = {"frame_rate_index": frame_rate_index, **walked,
             "immersive_codec_mode": immersive[0] if immersive and len(immersive) == 1
             else immersive or None}
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
        walked, diagnostics, _, immersive = walk_all(data)
        if diagnostics:
            raise SystemExit(f"{leg['name']}: ac4_syntax.py did not read every substream to "
                             f"its end: {diagnostics[:5]}")
        check_expected(leg, frame_rate_index, walked, immersive)
        dest = OUT / leg["name"] / "dee.ac4"
        if leg.get("pinned") and dest.is_file() and dest.read_bytes() != data:
            raise SystemExit(f"{leg['name']}: this encode differs from the committed "
                             f"{dest.relative_to(REPO)}, which tests pin. Replace it by hand, "
                             "together with the tests that read it, if that is intended.")
        encoded.append((leg, data, frame_count, frame_rate_index, walked, immersive))

    manifest = {"baseline_version": BASELINE_VERSION, "dee_version": version, "legs": {}}
    digests = 0
    for leg, data, frame_count, frame_rate_index, walked, immersive in encoded:
        dest = OUT / leg["name"] / "dee.ac4"
        if dest.is_file() and dest.read_bytes() == data:
            print(f"unchanged {dest} ({len(data)} bytes)")
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            print(f"wrote {dest} ({len(data)} bytes)")
        entry = manifest_entry(leg, wavs[leg["source"]], descriptions[leg["source"]], data,
                               frame_count, frame_rate_index, walked, leg["source"] in built)
        if "input_format" in leg:
            entry["input_format"] = leg["input_format"]
        if immersive:
            entry["immersive_codec_mode"] = immersive[0] if len(immersive) == 1 else immersive
        label = f"{leg['name']}/dee.ac4"
        digest = DIGESTS / f"{leg['name']}.tsv"
        digest.write_text("\n".join(ac4_syntax.digest_lines(data, label)) + "\n",
                          encoding="utf-8", newline="\n")
        digests += 1
        manifest["legs"][leg["name"]] = entry

    manifest_path = OUT / "ac4-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8",
                             newline="\n")
    print(f"wrote {manifest_path} and {digests} digests under {DIGESTS.relative_to(REPO)}")


def mediainfo(stream, work):
    """MediaInfo's frame-by-frame trace and its summary, beside the stream (G0's form)."""
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


def g0_leg(leg, wav, work):
    """One of G0's legs made and described as G0's run makes it: the manifest entry."""
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
        return entry
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
    return entry


# ------------------------------------------------------------------------------------------
# One G1 leg, end to end (run in worker processes)
# ------------------------------------------------------------------------------------------

def run_logged(cmd, cwd, log, timeout=1800):
    """Runs cmd in cwd and writes the command and its output to log; (exit code, output)."""
    try:
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, check=False, timeout=timeout)
        output = (result.stdout + result.stderr).decode("utf-8", "replace")
        code = result.returncode
    except subprocess.TimeoutExpired as exc:
        output = ((exc.stdout or b"") + (exc.stderr or b"")).decode("utf-8", "replace")
        output += f"\n(timed out after {timeout} s)"
        code = -1
    Path(log).write_text(subprocess.list2cmdline(cmd) + "\n\n" + output, encoding="utf-8")
    return code, output


def dee_log_facts(text):
    """(warnings, loudness): DEE's WARNING lines, and the key=value pairs of its
    "[... loudness]" lines, by tag."""
    warnings = sorted({line.split("WARNING: ", 1)[1].strip() for line in text.splitlines()
                       if "WARNING: " in line and "Summary" not in line})
    loudness = {}
    for match in re.finditer(r"\[([A-Za-z ]*loudness)\] ([A-Za-z_]+)=(\S+)", text):
        loudness.setdefault(match.group(1), {})[match.group(2)] = match.group(3)
    return warnings, loudness


def dee_errors(text):
    return list(dict.fromkeys(line.split("ERROR: ", 1)[1].strip()
                              for line in text.splitlines() if "ERROR: " in line))


def g1_inputs(leg, source, work):
    """(the -i argument, files to write before the encode as {path: bytes}). wav_list inputs
    are the source's channels as mono WAVs named by speaker in the leg's directory, given to
    DEE by name from that directory: a drive letter's colon would split its list."""
    if leg["input_format"] != "wav_list":
        return str(source), {}
    with wave.open(str(source), "rb") as r:
        channels, width = r.getnchannels(), r.getsampwidth()
        raw = np.frombuffer(r.readframes(r.getnframes()), dtype=np.uint8)
    frames = raw.reshape(-1, channels, width)
    speakers = {1: ("C",), 2: SPEAKERS["20"], 6: SPEAKERS["51"], 8: SPEAKERS["71"]}[channels]
    files = {}
    for i, speaker in enumerate(speakers):
        with tempfile.SpooledTemporaryFile() as f:
            with wave.open(f, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(width)
                w.setframerate(RATE)
                w.writeframes(np.ascontiguousarray(frames[:, i, :]).tobytes())
            f.seek(0)
            files[work / f"in_{speaker}.wav"] = f.read()
    return ":".join(p.name for p in files), files


def g1_command(leg, inputs, outs, work):
    """DEE's command line for one G1 leg (paths relative to work where DEE splits on colons);
    inputs and outs pair up, more than one of each for the gapless legs."""
    exe = str(dee_exe(leg["encoder"]))
    encoder = leg["encoder"]
    pairs = [*(a for i in inputs for a in ("-i", i)), *(a for o in outs for a in ("-o", str(o)))]
    if encoder == AJOC:
        return [exe, *pairs, "--overwrite", "1",
                "--output-manifest", str(work / "dee_manifest.json"), *leg["options"]]
    cmd = [exe, "--input-format", leg["input_format"], *pairs, "--overwrite", "1"]
    if encoder == AC4:
        cmd += ["--output-channel-layout", leg["layout"], "--temp-dir", str(work),
                "--output-manifest", str(work / "dee_manifest.json")]
    elif encoder == DDP:
        cmd += ["--output-channel-layout", leg["layout"], "--temp-dir", str(work)]
    if leg.get("forced_iframes"):
        cmd += ["--forced-iframes", "csvfile:path=iframes.csv"]
    return [*cmd, "--data-rate", str(leg["kbps"]), *leg["options"]]


def toc_facts(data):
    """What the tables of contents say across the stream: channel modes, presentations,
    content classifiers, language tags, and the frames flagged b_iframe_global."""
    modes, presentations, classifiers, languages, iframes = set(), set(), set(), set(), []
    for index, (_, _, raw, _) in enumerate(ac4_parse.iter_sync_frames(data)):
        toc, _ = ac4_parse.parse_raw_frame(raw)
        if toc["b_iframe_global"]:
            iframes.append(index)
        presentations.add(toc["n_presentations"])
        for group in toc["substream_groups"]:
            content = group.get("content_type") or {}
            if content.get("content_classifier") is not None:
                classifiers.add(content["content_classifier"])
            if content.get("language_tag"):
                languages.add(content["language_tag"].decode("ascii", "replace"))
            for substream in group["substreams"]:
                info = substream.get("info") or {}
                modes.add(info.get("channel_mode_name", substream.get("kind")))
    return {"channel_modes": sorted(map(str, modes)), "n_presentations": sorted(presentations),
            "content_classifiers": sorted(classifiers), "language_tags": sorted(languages),
            "iframes": iframes}


def wav_shape(path):
    """(channels, sample frames) from a WAV's header, or None."""
    with open(path, "rb") as f:
        head = f.read(65536)
    if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
        return None
    pos, channels, block = 12, None, None
    while pos + 8 <= len(head):
        chunk, size = head[pos:pos + 4], struct.unpack_from("<I", head, pos + 4)[0]
        if chunk == b"fmt ":
            channels = struct.unpack_from("<H", head, pos + 10)[0]
            block = struct.unpack_from("<H", head, pos + 20)[0]
        elif chunk == b"data" and channels and block:
            return channels, size // block
        pos += 8 + size + (size & 1)
    return None


def decoder_check(cli, stream, scratch):
    """What cli's probe and decode make of the stream: "ok" and what was decoded, or the exit
    code and the message."""
    out = {}

    def message(result):
        text = (result.stderr + result.stdout).decode("utf-8", "replace").strip().splitlines()
        picked = [line for line in text if "error" in line.lower() or "refus" in line.lower()
                  or "unsupported" in line.lower()]
        return (picked or text or [""])[0].strip()[:400]

    result = subprocess.run([str(cli), "probe", str(stream)], capture_output=True, check=False,
                            timeout=600)
    out["probe"] = "ok" if result.returncode == 0 else f"exit {result.returncode}: " \
                                                       f"{message(result)}"
    scratch.mkdir(parents=True, exist_ok=True)
    wav = scratch / f"{stream.parent.name}-{stream.stem}.wav"
    wav.unlink(missing_ok=True)
    result = subprocess.run([str(cli), "decode", str(stream), str(wav)], capture_output=True,
                            check=False, timeout=1800)
    shape = wav_shape(wav) if wav.is_file() else None
    if result.returncode == 0 and shape:
        text = (result.stdout + result.stderr).decode("utf-8", "replace")
        objects = [line.strip() for line in text.splitlines() if "objects" in line]
        out["decode"] = f"ok: {shape[0]} channels, {shape[1]} samples" + (
            f"; {objects[0]}" if objects else "")
    else:
        out["decode"] = f"exit {result.returncode}: {message(result)}"
    wav.unlink(missing_ok=True)
    return out


def mediainfo_g1(stream, work, suffix=""):
    """MediaInfo's trace with a table of contents for every frame, and its summary."""
    files = {}
    for args, name in ((["--Details=1", "--ParseSpeed=1"], f"mediainfo-details{suffix}.txt"),
                       (["--Output=JSON", "--ParseSpeed=1"], f"mediainfo{suffix}.json")):
        result = subprocess.run([str(MEDIAINFO), *args, str(stream)], capture_output=True,
                                check=False, timeout=1800)
        (work / name).write_bytes(result.stdout)
        files[name] = hashlib.sha256(result.stdout).hexdigest()
    return files


def mux_mp4(stream, out_dir, fmt, manifest_json, suffix=""):
    """DEE's MP4 muxer's file of the stream in out_dir (dee<suffix>.mp4), with DEE's own offset
    and duration when its output manifest gives them; the entry for the manifest."""
    out_dir.mkdir(parents=True, exist_ok=True)
    mp4 = out_dir / f"dee{suffix}.mp4"
    mp4.unlink(missing_ok=True)
    options = f"input_format={fmt}"
    cmd = [str(MP4MUXER), "--overwrite", "1", "--temp-dir", str(out_dir)]
    timing = None
    if manifest_json is not None and Path(manifest_json).is_file():
        timing = json.loads(Path(manifest_json).read_text(encoding="utf-8"))["output_files"][0]
        cmd += ["--time-scale", str(RATE)]
        options += f":offset={timing['offset']}:duration={timing['duration']}"
    cmd += ["--track", str(stream), "--track-options", options, "--output", str(mp4)]
    code, output = run_logged(cmd, out_dir, out_dir / f"dee{suffix}_mp4muxer_output.txt")
    if code != 0 or not mp4.is_file():
        return {"refused": dee_errors(output) or output.strip().splitlines()[-3:]}
    result = subprocess.run([str(MEDIAINFO), "--Output=JSON", str(mp4)], capture_output=True,
                            check=False, timeout=600)
    (out_dir / f"mediainfo-mp4{suffix}.json").write_bytes(result.stdout)
    entry = {"file": mp4.name, "sha256": sha256(mp4), "size_bytes": mp4.stat().st_size,
             "muxer_warnings": [line.strip() for line in output.splitlines()
                                if line.strip().startswith("Warning")]}
    if timing:
        entry.update({"time_scale": RATE, "offset": timing["offset"],
                      "duration": timing["duration"]})
    return entry


def describe_g1_stream(name, stream, suffix, fresh, old, task):
    """The manifest's description of one stream a G1 leg made (its MediaInfo files and MP4 are
    named with suffix, "" for a leg's first stream), and its syntax digest's lines."""
    work = stream.parent
    fmt = {".ac4": "ac4", ".ec3": "eac3", ".ac3": "ac3"}[stream.suffix]
    data = stream.read_bytes()
    out = {"size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    details, summary = work / f"mediainfo-details{suffix}.txt", work / f"mediainfo{suffix}.json"
    if fresh or not details.is_file() or not summary.is_file():
        out["mediainfo"] = mediainfo_g1(stream, work, suffix)
    else:
        out["mediainfo"] = {path.name: sha256(path) for path in (details, summary)}
    out["mediainfo"]["parse_speed"] = 1
    manifest_json = work / "dee_manifest.json"
    if not fresh and (work / f"dee{suffix}.mp4").is_file() and old.get("mp4"):
        out["mp4"] = old["mp4"]
    else:
        out["mp4"] = mux_mp4(stream, work, fmt,
                             manifest_json if manifest_json.is_file() else None, suffix)
    lines = None
    if fmt == "ac4":
        frame_count, frame_rate_index = describe(name, data)
        walked, diagnostics, lines, immersive = walk_all(data, f"{name}/{stream.name}")
        out.update({"frame_count": frame_count, "frame_rate_index": frame_rate_index,
                    "frame_len_base": FRAME_LEN_BASE[frame_rate_index],
                    "walked_by": "tools/references/ac4_syntax.py", **walked,
                    "walk_failures": [d for d in diagnostics if "FAIL" in d][:10],
                    "walk_refusals": sorted({d.split(": ", 1)[-1] for d in diagnostics
                                             if "refused" in d})})
        if immersive:
            out["immersive_codec_mode"] = immersive[0] if len(immersive) == 1 else immersive
        out.update(toc_facts(data))
    if task["cli"]:
        out["main_decoder"] = decoder_check(task["cli"], stream, Path(task["scratch"]) / "decode")
    elif "main_decoder" in old:
        out["main_decoder"] = old["main_decoder"]
    return out, lines


def run_g1_leg(task):
    """Makes one G1 leg in DIR/streams/<name> (reusing streams whose command and sources are
    unchanged) and describes it: (name, manifest entry, {census name: digest lines}). A gapless
    leg's further streams are dee-2.ac4 and on, described under the entry's part_streams."""
    leg, root, old = task["leg"], Path(task["root"]), task["old"] or {}
    sources = [(Path(path), sha) for path, sha in task["sources"]]
    name = leg["name"]
    work = root / "streams" / name
    work.mkdir(parents=True, exist_ok=True)
    if leg["encoder"] == DDP and any(o.startswith("dd:") for o in leg["options"]):
        ext = "ac3"
    else:
        ext = "ec3" if leg["encoder"] in (DDP, DDPJOC) else "ac4"
    streams = [work / (f"dee.{ext}" if i == 0 else f"dee-{i + 1}.{ext}")
               for i in range(len(sources))]
    inputs, files = [], {}
    for source, _ in sources:
        argument, more = g1_inputs(leg, source, work)
        inputs.append(argument)
        files.update(more)
    cmd = g1_command(leg, inputs, streams, work)
    shas = [sha for _, sha in sources]
    stamp = {"command": subprocess.list2cmdline(cmd),
             "source_sha256": shas[0] if len(shas) == 1 else shas}
    leg_json = work / "leg.json"
    fresh = not (all(s.is_file() for s in streams) and leg_json.is_file()
                 and json.loads(leg_json.read_text(encoding="utf-8")) == stamp)
    entry = {"group": leg["group"], "phases": leg["phases"], "encoder": leg["encoder"],
             "source": leg["source"] if isinstance(leg["source"], str)
             else Path(leg["source"]).relative_to(root).as_posix(),
             "source_sha256": shas[0], "input_format": leg["input_format"],
             "output_channel_layout": leg["layout"], "bitrate_kbps": leg["kbps"],
             "options": leg["options"], "command": stamp["command"]}
    for key in ("input_layout", "audio_altered", "forced_iframes", "parts", "leveling_expected"):
        if key in leg:
            entry[key] = leg[key]
    if fresh:
        for stream in streams:
            stream.unlink(missing_ok=True)
        for path, data in files.items():
            path.write_bytes(data)
        if leg.get("forced_iframes"):
            (work / "iframes.csv").write_text("\n".join(map(str, leg["forced_iframes"])))
        code, output = run_logged(cmd, work, work / "dee_output.txt")
        for path in files:
            path.unlink(missing_ok=True)
        if code != 0 or not all(s.is_file() for s in streams):
            key = "refused" if leg.get("refusal_ok") else "failed"
            entry[key] = dee_errors(output) or output.strip().splitlines()[-5:]
            return name, entry, {}
        leg_json.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    output = (work / "dee_output.txt").read_text(encoding="utf-8", errors="replace")
    entry["dee_warnings"], entry["dee_loudness"] = dee_log_facts(output)
    if leg["encoder"] == IMS and any("leveled" in w for w in entry["dee_warnings"]):
        entry["leveled"] = True
    digests = {}
    old_parts = old.get("part_streams", [])
    for i, stream in enumerate(streams):
        suffix = "" if i == 0 else f"-{i + 1}"
        described, lines = describe_g1_stream(
            name, stream, suffix, fresh, old if i == 0 else
            (old_parts[i - 1] if i - 1 < len(old_parts) else {}), task)
        if lines:
            digests[f"{name}{suffix}"] = lines
        if i == 0:
            entry.update(described)
        else:
            entry.setdefault("part_streams", []).append(
                {"stream": stream.name, "source": leg["parts"][i], "source_sha256": shas[i],
                 **described})
    return name, entry, digests


# ------------------------------------------------------------------------------------------
# The gold set
# ------------------------------------------------------------------------------------------

def write_source(path, data):
    """Writes a source WAV only where it is missing or differs, whole (replace_file())."""
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    replace_file(path, data)
    return True


def cli_version(cli):
    result = subprocess.run([str(cli), "--version"], capture_output=True, text=True,
                            check=False)
    return " ".join(line.strip() for line in result.stdout.splitlines()[:3])


def build_masters(cli, root):
    """The objects group's ADM BWF masters (MASTERS), under DIR/masters/<name>/, each with
    atmos_info's reading; {name: master path} and {name: source entry}."""
    paths, entries = {}, {}
    for name, spec in MASTERS.items():
        work = root / "masters" / name
        work.mkdir(parents=True, exist_ok=True)
        source = spec["input"]
        source = root / source if isinstance(source, str) else source
        ec3 = work / "objects.ec3"
        master = work / "adm-master.wav"
        encode_cmd = [str(cli), "atmos-encode", str(source), str(ec3), str(spec["kbps"]),
                      str(spec["objects"]), *([str(spec["paths"])] if spec["paths"] else [])]
        decode_cmd = [str(cli), "decode", str(ec3), str(work / "bed.wav"),
                      str(work / "objects"), str(master)]
        stamp = {"cli": cli_version(cli), "input_sha256": sha256(source),
                 "commands": [subprocess.list2cmdline(encode_cmd),
                              subprocess.list2cmdline(decode_cmd)]}
        stamp_path = work / "master.json"
        if not (master.is_file() and stamp_path.is_file()
                and json.loads(stamp_path.read_text(encoding="utf-8")) == stamp):
            for step, cmd in (("atmos-encode", encode_cmd), ("decode", decode_cmd)):
                code, output = run_logged(cmd, work, work / f"ac3cli_{step}.txt")
                if code != 0:
                    raise SystemExit(f"master {name}: ac3cli {step} exited {code}:\n"
                                     + output[-2000:])
            stamp_path.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
        code, output = run_logged([str(ATMOS_INFO), "-i", str(master)], work,
                                  work / "atmos_info.txt")
        paths[name] = master
        entries[master.relative_to(root).as_posix()] = {
            "description": (f"ADM BWF master written by ac3cli ({stamp['cli']}): "
                            f"{' then '.join(stamp['commands'])}"),
            "sha256": sha256(master), "phase": "G1",
            "atmos_info": dee_errors(output) or ["read"],
        }
    return paths, entries


def gold_run(args, version):
    """The gold set under args.gold_set: G0's legs (made only where missing), then G1's."""
    root = args.gold_set.resolve()
    streams = root / "streams"
    manifest_path = root / "gold-manifest.json"
    old = (json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file()
           else {})
    scratch = args.scratch_dir.resolve() / "g1"
    with tempfile.TemporaryDirectory() as temporary:
        music, speech = read_fixtures(Path(temporary))

    # G0's sources: written where missing; a file that differs from its rebuild is kept, and
    # reported, since legs were made from it.
    sources = dict(old.get("sources", {}))
    descriptions = source_descriptions(GOLD_SECONDS)
    g0_names = sorted({leg["source"] for leg in gold_legs()})
    wavs = {}
    for name, columns in g0_mixes(music, speech, GOLD_SECONDS, g0_names).items():
        path = root / "sources" / f"{name}.wav"
        data = wav24_bytes(columns)
        if path.is_file() and path.read_bytes() != data:
            print(f"note: {path} differs from its rebuild from committed material; kept")
        elif not path.is_file():
            replace_file(path, data)
        wavs[name] = path
        sources.setdefault(name, {"description": descriptions[name], "sha256": sha256(path),
                                  "seconds": GOLD_SECONDS})

    # G0's legs: every entry of the manifest as it is, its stream's SHA-256 checked; a leg of
    # gold_legs() the manifest lacks (a new gold set) made as G0 makes it.
    legs = {}
    for name, entry in old.get("legs", {}).items():
        stream = streams / name / "dee.ac4"
        if "sha256" in entry and (not stream.is_file() or sha256(stream) != entry["sha256"]):
            raise SystemExit(f"{stream} is missing or no longer matches the manifest; the gold "
                             "set's G0 streams are never remade over a live set")
        legs[name] = entry
    masters_g0 = [m.resolve() for m in args.adm_master or []]
    for master in masters_g0:
        wavs[master] = master
        sources.setdefault(master.as_posix(), {"description": f"Atmos master {master.as_posix()}",
                                               "sha256": sha256(master)})
    for leg in gold_legs() + object_legs(masters_g0):
        if leg["name"] not in legs:
            legs[leg["name"]] = g0_leg(leg, wavs[leg["source"]], streams / leg["name"])
            print(f"G0 {leg['name']}: made")

    # DEE's MP4 of each of G0's streams, beside the set rather than in G0's directories.
    g0_mp4 = dict(old.get("g0_mp4", {}))
    for name, entry in sorted(legs.items()):
        stream = streams / name / "dee.ac4"
        out_dir = root / "mp4" / name
        if "sha256" not in entry or (name in g0_mp4 and (out_dir / "dee.mp4").is_file()):
            continue
        manifest_json = streams / name / "dee_manifest.json"
        g0_mp4[name] = mux_mp4(stream, out_dir, "ac4",
                               manifest_json if manifest_json.is_file() else None)
    print(f"G0: {len(legs)} legs as they were; {len(g0_mp4)} MP4s under {root / 'mp4'}")

    # G1's sources, legs and masters.
    specs = g1_source_specs()
    g1 = g1_legs()
    masters, master_sources = {}, {}
    if args.cli:
        masters, master_sources = build_masters(args.cli.resolve(), root)
        g1 += master_legs(masters)
    elif "g1_legs" in old:
        g1 += master_legs({name: root / "masters" / name / "adm-master.wav"
                           for name in MASTERS
                           if (root / "masters" / name / "adm-master.wav").is_file()})
    names = [leg["name"] for leg in g1]
    clash = sorted(set(names) & set(legs)) + sorted({n for n in names if names.count(n) > 1})
    if clash:
        raise SystemExit(f"G1 leg names clash with G0's or with each other: {clash}")
    needed = sorted({part for leg in g1 if isinstance(leg["source"], str)
                     for part in leg.get("parts", [leg["source"]])} - set(g0_names))
    for name, columns in g1_mixes(music, speech, needed).items():
        path = root / "sources" / f"{name}.wav"
        if write_source(path, wav24_bytes(columns)):
            print(f"G1 source {name}: written")
        wavs[name] = path
        sources[name] = {"description": specs[name][1], "sha256": sha256(path),
                         "seconds": specs[name][0], "phase": "G1"}
    sources.update(master_sources)

    old_g1 = old.get("g1_legs", {})
    chosen = [leg for leg in g1
              if not args.only or any(part in leg["name"] for part in args.only)]
    source_sha = {}
    tasks = []
    for leg in chosen:
        paths = ([leg["source"]] if isinstance(leg["source"], Path)
                 else [wavs[part] for part in leg.get("parts", [leg["source"]])])
        for path in paths:
            if path not in source_sha:
                source_sha[path] = sha256(path)
        tasks.append({"leg": leg, "root": str(root),
                      "sources": [(str(path), source_sha[path]) for path in paths],
                      "cli": str(args.cli.resolve()) if args.cli else None,
                      "scratch": str(scratch), "old": old_g1.get(leg["name"])})
    census = scratch / "census"
    census.mkdir(parents=True, exist_ok=True)
    made = {}
    started = time.monotonic()
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_g1_leg, task) for task in tasks]
        for number, future in enumerate(as_completed(futures), 1):
            name, entry, digests = future.result()
            made[name] = entry
            for census_name, lines in digests.items():
                (census / f"{census_name}.tsv").write_text("\n".join(lines) + "\n",
                                                            encoding="utf-8", newline="\n")
            state = ("refused" if "refused" in entry else "FAILED" if "failed" in entry
                     else entry.get("codec_mode") or entry.get("immersive_codec_mode") or "ok")
            decoded = entry.get("main_decoder", {}).get("decode", "")
            print(f"[{number}/{len(tasks)} {time.monotonic() - started:6.0f} s] {name}: "
                  f"{state}; {decoded[:70]}", flush=True)
    g1_entries = {}
    for leg in g1:
        if leg["name"] in made:
            g1_entries[leg["name"]] = made[leg["name"]]
        elif leg["name"] in old_g1:
            g1_entries[leg["name"]] = old_g1[leg["name"]]

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    history = old.get("history") or [{
        "gold_version": old.get("gold_version", 3), "phase": "G0", "made": old.get("made"),
        "legs_key": "legs", "legs": len(old.get("legs", {})),
        "added": ("every layout and rate dee_ac4_encoder writes, 2.0 at 48 kbps to 5.1.4 at "
                  "768, music, speech, film and tones; dee_ac4ims_encoder at every rate and "
                  "frame rate; 5.1 DRC, downmix, mix-level, loudness and I-frame legs; 5.1.4 "
                  "height downmix legs; three object attempts, refused")}]
    history = [h for h in history if h.get("phase") != "G1"] + [{
        "gold_version": BASELINE_VERSION, "phase": "G1", "made": now, "legs_key": "g1_legs",
        "legs": len(g1_entries),
        "added": ("golden masters for every phase still to come: " + "; ".join(
            f"{group} ({', '.join(phases)})" for group, phases in G1_GROUPS.items()))}]
    manifest = {
        "gold_version": BASELINE_VERSION, "dee_version": old.get("dee_version", version),
        "mediainfo_version": old.get("mediainfo_version", mediainfo_version()),
        "made": old.get("made", now), "seconds": old.get("seconds", GOLD_SECONDS),
        "sources": sources, "legs": legs, "g0_mp4": g0_mp4,
        "g1_made": now, "g1_dee_version": version, "g1_mediainfo_version": mediainfo_version(),
        "g1_cli": cli_version(args.cli.resolve()) if args.cli else old.get("g1_cli"),
        "g1_dee_cannot": DEE_CANNOT, "g1_legs": g1_entries, "history": history,
    }
    replace_file(manifest_path, (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    failed = sorted(n for n, e in g1_entries.items() if "failed" in e)
    refused = sorted(n for n, e in g1_entries.items() if "refused" in e)
    leveled = sorted(n for n, e in g1_entries.items()
                     if e.get("leveled") and not e.get("leveling_expected"))
    walk_failed = sorted(n for n, e in g1_entries.items() if e.get("walk_failures"))
    print(f"wrote {manifest_path}: {len(legs)} G0 legs, {len(g1_entries)} G1 legs "
          f"({len(refused)} refused by DEE as expected, {len(failed)} failed, {len(leveled)} "
          f"leveled, {len(walk_failed)} with walk failures)")
    for label, names in (("failed", failed), ("leveled", leveled), ("walk failures", walk_failed)):
        if names:
            print(f"  {label}: {names}")
    print(f"census digests in {census}: AC4DEC_GOLDEN_DIR={census} "
          f"AC4DEC_STREAM_DIR={streams} ac3tests \"[ac4dec][syntax]\"")
    if failed:
        raise SystemExit(1)


def main():
    guard_not_ci()
    parser = argparse.ArgumentParser(description="Generate the AC-4 DEE streams.")
    parser.add_argument("--scratch-dir", type=Path, default=SCRATCH,
                        help="where the committed set's sources, encodes and DEE logs go, and "
                             "the gold run's decodes and census digests (default: "
                             "build/ac4_baseline_scratch)")
    parser.add_argument("--gold-set", type=Path, metavar="DIR",
                        help=f"make the local gold set in DIR (e.g. {GOLD_DIR_DEFAULT.as_posix()}) "
                             "instead of the committed set")
    parser.add_argument("--adm-master", type=Path, action="append", metavar="WAV",
                        help="with --gold-set: an Atmos ADM BWF master to try the A-JOC and "
                             "immersive stereo encoders on, as G0 did; repeatable")
    parser.add_argument("--cli", type=Path, metavar="AC3CLI",
                        help="with --gold-set: an ac3cli built with -DAC3FORGE_BUILD_ADM=ON, which "
                             "writes the objects group's masters and whose probe and decode are "
                             "recorded for every G1 leg")
    parser.add_argument("--jobs", type=int, default=4,
                        help="with --gold-set: G1 legs made at once (default 4)")
    parser.add_argument("--only", nargs="+", metavar="NAME_PART",
                        help="with --gold-set: make only the G1 legs whose names contain one of "
                             "these; the others keep their manifest entries")
    args = parser.parse_args()
    if (args.adm_master or args.cli or args.only) and not args.gold_set:
        parser.error("--adm-master, --cli and --only need --gold-set")

    encoders = (AC4, IMS, AJOC, DDP, DDPJOC) if args.gold_set else (AC4, IMS)
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
