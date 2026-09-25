"""Score ac3cli's AC-4 decoding of DEE's streams against the sources they were encoded from.

For each leg the decoder turns into PCM - SIMPLE and ASPX mono, stereo and 5.1, 5.1 in ASPX_ACPL_2
and ASPX_ACPL_3, and DEE's immersive stereo (IMS), at frame_rate_index 13, and IMS at 23.976, 24,
25 and 29.97 fps through the sample rate converter (see src/ac4dec/include/ac4dec/decoder.hpp) -
this decodes the stream with `ac3cli decode`, aligns the output with its reference by
cross-correlation, fits a least-squares gain per channel, and checks (planning/ac4.md, the
decoder's ladder, item 3):

  lag      the output lags the source by the leg's LAG: DEE's encoder delay plus this decoder's,
           1 313 samples at index 13 (Part 1 Table 188's d_pcm, the QMF banks' 577 samples and six
           QMF slots of history, for every codec mode: src/ac4dec/ERRATA.md, "Every codec mode
           passes through the QMF banks"). DEE's IMS encoder runs a frame shorter than its AC-4
           encoder. At the other frame rates, the lag first measured (LAG_AT_RATE); the A-SPX
           subbands there are the internal rate's.
  gain     every channel of a mono, 2.0 or 5.1 leg within 0.2 dB of unity, fitted below the
           crossover in ASPX. The streams were made with loudness measured only, and they are
           decoded with no output level, which leaves them at their coded level and uncompressed,
           so the prediction is the source's own level (gain_ac4_decode.py checks the output
           level and the downmixes). A 5.1
           leg's LFE, which DEE low-passes before coding it (LFE_CHANNEL's comment), within 0.5 dB,
           from 20 to 100 Hz. An IMS leg, made from 5.1, is compared with the source's Lo/Ro
           downmix (L + C/sqrt 2 + Ls/sqrt 2 and its mirror), which its channels must correlate with
           at 0.95 or better; its render is DEE's, so its level is only reported.
  SNR      every channel's signal-to-noise ratio against the gain-scaled reference at or above its
           floor, the first measurement less 1 dB: over the whole band for SIMPLE and for the LFE,
           which A-SPX leaves out, and below the A-SPX crossover for the other channels in ASPX,
           from 2 048-point STFT frames.
  tiles    for ASPX, above each channel's crossover, each 2 048-sample frame's energy in each
           low-resolution A-SPX subband group against the reference's, in dB, where the
           reference's is above -95 dB per subband: the mean of their absolute differences at or
           below its ceiling, the first measurement plus 0.5 dB. DEE quantises these envelopes in
           1.5 or 3 dB steps.
  LSD      tools/ci/quality_race.py's log-spectral distance at or below its ceiling, the first
           measurement plus 0.5 dB.
  MOS      ViSQOL's MOS-LQO (quality_race.perceptual_score) at or above its floor, the first
           measurement less 0.1, where visqol-python is installed.
  routing  on a tone leg, each channel's own tone at least 40 dB above every other channel's tone
           in it; in the A-CPL legs, at least the margin first measured less 3 dB, since the
           parameters are per band and a tone near a band's edge reaches its neighbour.

The A-CPL legs code a pair of downmixes and make the surrounds of them (and in ASPX_ACPL_3 the
centre), so their channels are scored as A-CPL rebuilds them:

  downmix  the coded downmixes, recovered from the output, against the source's as waveforms, SNR
           below the crossover at or above its floor, the first measurement less 1 dB: in
           ASPX_ACPL_2 (L + Ls / sqrt 2) / 2 and its mirror, which the upmix keeps exactly
           (Part 1 Pseudocode 117), with C and the LFE as they are; in ASPX_ACPL_3 the Lo/Ro
           downmix over 1 + sqrt 2, which it keeps as closely as gamma's quantisation allows
           (Pseudocode 118), with the LFE.
  bands    per A-CPL parameter band (Part 1 Table 197, 15 bands) over 2 048-sample frames, for the
           pairs A-CPL rebuilds, (L, Ls) and (R, Rs): the mean distance of the output's level
           difference from the source's, and of its correlation, the larger of the two pairs', at
           or below ceilings of the first measurement plus 0.5 dB and 0.05. Music and film legs
           only; a tone leg's bands hold little but its tones.

score_ac4_encode.py scores the encoder's A-CPL legs with the same checks, which also take
ASPX_ACPL_1, whose downmixes are ASPX_ACPL_2's, and the channel pair's A-CPL, whose downmix is
(L + R) / 2 and whose one pair is (L, R).

The crossovers and the subband groups come from the leg's first aspx_config() and the
aspx_xover_subband_offset of each aspx_data element in that frame, a channel's being the element
Part 1 Table 213 gives it, read from `ac3cli decode ... syntax-trace=`, through Part 1
Pseudocodes 67 to 69.

The committed legs (tests/golden/external-baseline/) made with loudness measured only are scored
by default; their sources are rebuilt by tools/generators/gen_ac4_baseline.py from the committed
FLAC fixtures, which needs ffmpeg on PATH. --gold DIR scores phase G0's local gold set in DIR
instead (DIR/streams/<leg>/dee.ac4, DIR/sources/<source>.wav, DIR/gold-manifest.json), which never
runs in CI.

--measure prints what every leg measures, as the PINS lines to pin it with, and checks nothing.

Usage:
    python tools/checks/score_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/score_ac4_decode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
"""

import argparse
import itertools
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "generators"))
sys.path.insert(0, str(REPO / "tools" / "ci"))
import gen_ac4_baseline as baseline  # noqa: E402
import quality_race  # noqa: E402

BASELINE_DIR = REPO / "tests" / "golden" / "external-baseline"
RATE = 48000

# DEE's encoder and this decoder together, at frame_rate_index 13, by DEE encoder: 3 072 samples
# (a frame and a half) plus the decoder's 1 313; the IMS encoder a frame less.
DECODER_DELAY = 352 + 577 + 6 * 64
LAG = {baseline.AC4: 3072 + DECODER_DELAY, baseline.IMS: 1024 + DECODER_DELAY}
# Part 1 Table 83's decoder resampling ratio by frame_rate_index, (up, down): a frame is coded at
# RATE * down / up, and the decoder's sample rate converter makes RATE of it.
RESAMPLING = {0: (1001, 960), 1: (25, 24), 2: (15, 16), 3: (1001, 960), 4: (25, 24),
              5: (1001, 960), 6: (25, 24), 7: (15, 16), 8: (1001, 960), 9: (25, 24),
              10: (15, 16), 11: (1001, 960), 12: (25, 24), 13: (1, 1)}
# At the other frame rates DEE's IMS encoder writes, the lag by frame_rate_index, as first
# measured. DEE's IMS encoder delays by half a frame at 48 kHz, as at index 13 (1 024); this
# decoder by its d_pcm, the QMF banks' 577 samples and 384 of history at the internal rate, and
# its converter's delay() (src/ac4core/src/dsp/resampler.hpp), taken to 48 kHz. Their sum comes
# within 1.3 samples of each lag: 1 000 + 1 301.0 + 49.0 at 24 fps, 1 001 + 1 302.4 + 49.1 at
# 23.976, 960 + 1 230.9 + 46.8 at 25, 800.8 + 1 102.1 + 49.1 at 29.97.
LAG_AT_RATE = {(baseline.IMS, 0): 2353, (baseline.IMS, 1): 2351, (baseline.IMS, 2): 2239,
               (baseline.IMS, 3): 1952}
GAIN_TOLERANCE_DB = 0.2
ROUTING_MARGIN_DB = 40.0
IMS_CORRELATION = 0.95
# Samples left out at each end of the aligned overlap: the decoder's first frames start from
# silence, and the encoder's last frames are padding.
EDGE = 4096
FRAME = 2048
TILE_FLOOR_DB = -95.0
SNR_MARGIN_DB = 1.0
LSD_MARGIN_DB = 0.5
TILE_MARGIN_DB = 0.5
MOS_MARGIN = 0.1

# Part 1 5.7.6.3.1.1's template subband group tables.
SBG_TEMPLATE_LOWRES = [10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 24, 26, 28, 30, 32, 35, 38,
                       42, 46]
SBG_TEMPLATE_HIGHRES = [18, 19, 20, 21, 22, 23, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 47,
                        50, 53, 56, 59, 62]
ASPX_CONFIG_FIELDS = ("aspx_master_freq_scale", "aspx_start_freq", "aspx_stop_freq")
# Part 1 Table 213 by the decoder's channel order (L R C Ls Rs for 5.0, L R C LFE Ls Rs for 5.1):
# the aspx_data element, in syntax order, that carries each channel, and None for the LFE, which
# A-SPX leaves out and which is scored over its whole band.
ASPX_UNIT = {1: (0,), 2: (0, 0), 5: (0, 0, 2, 1, 1), 6: (0, 0, 2, None, 1, 1)}
# The A-CPL legs' aspx_data elements (Part 1 Table 213, codec modes 3 and 4 of the 5.X element),
# by the decoder's channel order for 5.1: ASPX_ACPL_2 carries L and R in element 0 and C in element
# 1; ASPX_ACPL_3 carries L and R alone. The downmixes are scored below element 0's crossover.
ACPL_UNIT = {"ASPX_ACPL_1": (0, 0, 1, None, None, None),
             "ASPX_ACPL_2": (0, 0, 1, None, None, None),
             "ASPX_ACPL_3": (0, 0, None, None, None, None)}
# Table 197's first QMF subband of each of the 15 parameter bands, and the end.
ACPL_BAND_SUBBANDS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 18, 23, 35, 64]
# The pairs A-CPL rebuilds from one downmix each, by the decoder's 5.1 channel order.
ACPL_PAIRS = ((0, 4), (1, 5))
ACPL_ILD_MARGIN_DB = 0.5
ACPL_RHO_MARGIN = 0.05
ACPL_ROUTING_MARGIN_DB = 3.0
# The LFE of a 5.1 leg, by the decoder's channel order. DEE low-passes the LFE before it codes it:
# from the source to the decoded LFE the level runs 0.2 to 0.4 dB under unity up to 100 Hz and
# falls 12 dB by 120 to 160 Hz, with the phase of a filter near 120 Hz (-54 degrees at 110 Hz),
# which librempeg's decode shows as well, agreeing with this decoder's LFE to 83 dB. So the LFE's
# level is taken from 20 to 100 Hz, and its SNR against the source, low for that phase, is pinned.
LFE_CHANNEL = {6: 3}
LFE_BAND_HZ = (20.0, 100.0)
LFE_GAIN_TOLERANCE_DB = 0.5

# Per leg: (SNR floor per channel in dB, LSD ceiling in dB, tile ceiling in dB or None for
# SIMPLE, MOS floor or None where ViSQOL was not installed), the first measurement less (plus) the
# margins above. The committed legs by their directory under tests/golden/external-baseline/, the
# gold legs by their name in gold-manifest.json. Measured 2026-09-25: the SIMPLE legs with the
# decoder of phase D3, the ASPX and 5.1 legs with phase D4's, whose reading of pre-flattening
# (src/ac4dec/ERRATA.md, "Pre-flattening's direction") moved the ASPX legs' tiles, LSD and MOS.
PINS = {
    "ac4-20-music-192": ((33.9, 34.5), 1.82, None, 4.62),
    "ac4-20-speech-128": ((37.1, 37.1), 0.81, 3.40, 4.29),
    "ac4-20-tones-192": ((48.9, 50.6), 10.87, None, 4.63),
    "ac4-51-drc-ltrt-192": ((21.4, 22.1, 23.7, -3.3, 22.2, 22.5), 2.65, 3.56, 4.49),
    "ac4-51-music-192": ((21.4, 22.1, 23.6, -3.3, 22.2, 22.5), 2.64, 3.71, 4.49),
    "ac4-51-music-384": ((30.9, 31.7, 32.8, -3.3, 31.2, 31.5), 2.77, None, 4.62),
    "ac4-51-tones-384": ((48.9, 50.6, 52.8, 18.7, 55.3, 52.5), 11.76, None, 4.63),
    # The IMS legs at 24, 25 and 29.97 fps, through the sample rate converter (phase D6).
    "ac4-ims-film-96-24": ((19.8, 19.9), 1.20, 2.17, 4.34),
    "ac4-ims-music-128-25": ((18.4, 19.2), 1.52, None, 4.59),
    "ac4-ims-music-64-2997": ((15.7, 16.1), 1.65, 2.07, 4.54),
    "20-music-48": ((15.7, 15.8), 1.82, 2.03, 4.50),
    "20-music-64": ((18.3, 18.3), 1.41, 6.18, 4.50),
    "20-music-96": ((24.3, 24.3), 1.45, 0.61, 4.57),
    "20-music-128": ((28.8, 29.0), 1.24, 4.17, 4.60),
    "20-music-144": ((30.2, 30.3), 1.18, 2.13, 4.61),
    "20-music-192": ((33.6, 33.6), 1.67, None, 4.63),
    "20-speech-48": ((24.2, 24.2), 1.30, 1.82, 4.45),
    "20-speech-64": ((27.3, 27.3), 1.10, 2.10, 4.40),
    "20-speech-96": ((32.0, 32.0), 0.92, 3.55, 4.47),
    "20-speech-128": ((37.0, 37.0), 0.81, 3.56, 4.51),
    "20-speech-144": ((38.2, 38.2), 0.79, 3.58, 4.52),
    "20-speech-192": ((38.4, 38.4), 0.83, None, 4.60),
    "20-tones-48": ((48.6, 49.7), 11.18, None, 4.63),
    "20-tones-64": ((48.5, 49.7), 10.94, None, 4.63),
    "20-tones-96": ((48.7, 49.4), 10.74, None, 4.63),
    "20-tones-128": ((48.8, 50.3), 10.74, None, 4.63),
    "20-tones-144": ((48.8, 50.3), 10.74, None, 4.63),
    "20-tones-192": ((49.0, 50.3), 10.44, None, 4.63),
    # The 5.1 legs, measured with the decoder of phase D4. The LFE's floor is low: DEE low-passes
    # the LFE (LFE_CHANNEL's comment).
    "51-film-192": ((19.1, 19.0, 30.4, -3.2, 17.7, 17.8), 3.20, 2.35, 4.44),
    "51-film-256": ((23.4, 23.4, 34.7, -3.2, 22.4, 22.4), 2.53, 2.28, 4.52),
    "51-film-288": ((25.2, 25.2, 36.4, -3.2, 24.1, 24.2), 2.47, 2.23, 4.55),
    "51-film-320": ((26.6, 26.7, 37.6, -3.2, 25.6, 25.7), 2.42, 2.23, 4.56),
    "51-film-384": ((29.0, 29.1, 39.3, -3.2, 27.9, 27.9), 3.19, None, 4.62),
    "51-film-448": ((30.7, 30.8, 40.4, -3.2, 29.5, 29.6), 3.10, None, 4.62),
    "51-film-512": ((32.3, 32.3, 40.8, -3.2, 31.0, 31.1), 2.96, None, 4.63),
    "51-film-768": ((34.3, 34.3, 40.9, -3.2, 33.4, 33.5), 2.72, None, 4.63),
    "51-music-192": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-dmx-loro": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-dmx-ltrt": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-dmx-ltrt-pl2": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-dmx-not_indicated": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-drc-film_light": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-drc-film_standard": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-drc-music_light": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-drc-music_standard": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-drc-per-device": ((22.2, 22.2, 25.8, -3.2, 18.9, 18.9), 3.04, 4.21, 4.55),
    "51-music-192-drc-speech": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-iframe-1000": ((22.2, 22.3, 25.8, -3.2, 19.0, 19.0), 3.07, 3.99, 4.55),
    "51-music-192-iframe-11": ((22.2, 22.2, 25.7, -3.2, 18.9, 19.0), 3.04, 4.13, 4.55),
    "51-music-192-iframe-48": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.06, 4.13, 4.55),
    "51-music-192-loudness-atsc_a85": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-loudness-ebu_r128": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-mix-loro-cm6-sminf": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.13, 4.56),
    "51-music-192-mix-loro-cp3-sm1.5": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.05, 4.56),
    "51-music-192-mix-ltrt-c0-sm4.5": ((22.2, 22.2, 25.8, -3.2, 18.9, 19.0), 3.04, 4.05, 4.56),
    "51-music-256": ((26.3, 26.2, 30.0, -3.2, 23.4, 23.4), 2.53, 4.70, 4.56),
    "51-music-288": ((27.8, 27.8, 31.5, -3.2, 25.0, 25.1), 2.47, 4.99, 4.60),
    "51-music-320": ((29.2, 29.2, 32.6, -3.2, 26.3, 26.5), 2.41, 5.51, 4.60),
    "51-music-384": ((31.7, 31.8, 34.5, -3.2, 28.8, 28.8), 3.21, None, 4.62),
    "51-music-448": ((33.3, 33.4, 36.0, -3.2, 30.2, 30.3), 3.06, None, 4.63),
    "51-music-512": ((34.6, 34.6, 36.6, -3.2, 31.6, 31.7), 2.90, None, 4.63),
    "51-music-768": ((35.7, 35.8, 36.9, -3.2, 33.8, 33.8), 2.72, None, 4.63),
    "51-tones-192": ((48.8, 50.3, 52.5, 18.7, 55.1, 52.2), 11.72, None, 4.63),
    "51-tones-256": ((48.8, 50.3, 52.7, 18.7, 55.1, 52.2), 11.89, None, 4.63),
    "51-tones-288": ((48.8, 50.3, 52.7, 18.7, 55.1, 52.2), 11.89, None, 4.63),
    "51-tones-320": ((48.8, 50.3, 52.7, 18.7, 55.1, 52.2), 11.89, None, 4.63),
    "51-tones-384": ((49.0, 50.3, 52.8, 18.7, 55.3, 52.5), 11.56, None, 4.63),
    "51-tones-448": ((49.0, 50.3, 52.8, 18.7, 55.2, 52.5), 11.56, None, 4.63),
    "51-tones-512": ((49.0, 50.3, 52.8, 18.7, 54.8, 52.5), 11.56, None, 4.63),
    "51-tones-768": ((49.0, 50.3, 52.8, 18.7, 54.8, 52.5), 11.56, None, 4.63),
    # The immersive stereo legs against the source's Lo/Ro downmix, which DEE's render is not.
    "ims-music-64-native": ((12.3, 12.4), 1.57, 2.17, 4.48),
    "ims-music-96-native": ((13.3, 13.5), 1.22, 5.22, 4.50),
    "ims-music-128-native": ((13.6, 13.7), 1.48, None, 4.53),
    "ims-music-128-native-drcddp": ((13.6, 13.7), 1.48, None, 4.53),
    "ims-music-128-native-drcnone": ((13.6, 13.7), 1.48, None, 4.53),
    "ims-music-128-native-musicmode": ((13.3, 13.4), 1.43, None, 4.54),
    "ims-music-144-native": ((13.6, 13.7), 1.43, None, 4.53),
    "ims-music-256-native": ((13.7, 13.9), 0.97, None, 4.55),
    "ims-music-320-native": ((13.7, 13.9), 0.97, None, 4.55),
    # At 23.976, 24, 25 and 29.97 fps, through the sample rate converter (phase D6).
    "ims-film-128-23976": ((12.2, 12.3), 1.19, 3.18, 4.15),
    "ims-film-128-24": ((12.1, 12.2), 1.20, 3.17, 4.10),
    "ims-film-128-25": ((11.7, 11.7), 1.21, 3.27, 4.22),
    "ims-film-128-2997": ((12.2, 12.2), 1.20, 3.28, 4.11),
    "ims-film-64-23976": ((11.7, 11.8), 1.52, 2.37, 4.16),
    "ims-film-64-24": ((11.7, 11.7), 1.52, 2.35, 4.15),
    "ims-film-64-25": ((11.4, 11.5), 1.54, 2.33, 4.20),
    "ims-film-64-2997": ((11.4, 11.5), 1.64, 2.13, 4.24),
    "ims-music-128-23976": ((13.4, 13.5), 1.35, 4.17, 4.54),
    "ims-music-128-24": ((13.4, 13.6), 1.34, 3.13, 4.53),
    "ims-music-128-25": ((13.0, 13.2), 1.44, 3.62, 4.53),
    "ims-music-128-2997": ((13.5, 13.6), 1.39, 3.28, 4.54),
    "ims-music-64-23976": ((12.2, 12.4), 1.53, 2.05, 4.50),
    "ims-music-64-24": ((12.3, 12.4), 1.53, 1.98, 4.49),
    "ims-music-64-25": ((12.0, 12.1), 1.53, 2.25, 4.51),
    "ims-music-64-2997": ((11.9, 11.9), 1.65, 2.18, 4.49),
}
# The A-CPL legs: (floors of the downmixes' SNRs, in ASPX_ACPL_2 (L + Ls / sqrt 2) / 2, its mirror,
# C and the LFE, in ASPX_ACPL_3 Lo and Ro over 1 + sqrt 2 and the LFE; ceilings of the level
# difference's distance in dB and of the correlation's, per parameter band, or None for a tone
# leg; the routing floor in dB for a tone leg, else None; the LSD ceiling; the MOS floor or None).
# Measured 2026-09-25 with the decoder of phase D5: the committed legs, then the gold set's.
ACPL_PINS = {
    "ac4-51-film-96": (
        (23.7, 23.9, -3.3),
        (3.95, 3.80, 3.93, 4.19, 3.88, 3.49, 3.67, 3.75, 4.10, 3.45, 3.11, 3.66, 4.50, 2.66, None),
        (0.368, 0.402, 0.328, 0.373, 0.346, 0.331, 0.362, 0.415, 0.438, 0.365, 0.408, 0.413, 0.471,
         0.334, None),
        None, 5.17, 4.42),
    "ac4-51-music-128": (
        (20.4, 20.9, 23.5, -3.3),
        (3.14, 3.80, 4.23, 4.45, 3.85, 3.57, 3.51, 3.50, 3.29, 2.93, 2.41, 2.32, 2.29, 2.40, None),
        (0.404, 0.336, 0.322, 0.392, 0.330, 0.309, 0.318, 0.323, 0.322, 0.304, 0.270, 0.243, 0.226,
         0.244, None),
        None, 3.87, 4.48),
    "51-film-96": (
        (22.8, 22.8, -3.2),
        (4.07, 3.75, 3.69, 3.97, 4.04, 3.64, 3.54, 3.76, 3.54, 3.20, 3.15, 3.39, 3.67, 2.92, None),
        (0.351, 0.378, 0.343, 0.429, 0.352, 0.329, 0.347, 0.407, 0.406, 0.408, 0.454, 0.456, 0.516,
         0.418, None),
        None, 5.46, 4.37),
    "51-film-128": (
        (16.0, 15.9, 28.6, -3.2),
        (3.40, 3.47, 3.70, 4.20, 4.04, 3.69, 3.68, 3.36, 3.22, 2.79, 2.47, 2.44, 2.43, 2.42, None),
        (0.340, 0.316, 0.318, 0.425, 0.335, 0.301, 0.316, 0.320, 0.311, 0.290, 0.249, 0.246, 0.241,
         0.249, None),
        None, 4.10, 4.50),
    "51-film-144": (
        (18.6, 18.6, 30.7, -3.2),
        (3.41, 3.47, 3.68, 4.13, 4.01, 3.67, 3.69, 3.44, 3.21, 2.76, 2.49, 2.43, 2.39, 2.44, None),
        (0.337, 0.316, 0.317, 0.424, 0.335, 0.305, 0.318, 0.310, 0.306, 0.284, 0.255, 0.239, 0.239,
         0.247, None),
        None, 4.07, 4.50),
    "51-music-96": (
        (21.3, 21.4, -3.2),
        (3.54, 3.50, 3.66, 3.95, 3.62, 3.60, 3.59, 3.56, 3.34, 2.89, 2.73, 2.88, 2.59, 2.91, None),
        (0.339, 0.413, 0.355, 0.421, 0.347, 0.355, 0.328, 0.324, 0.327, 0.316, 0.296, 0.334, 0.338,
         0.346, None),
        None, 4.93, 4.52),
    "51-music-128": (
        (20.2, 20.3, 24.8, -3.2),
        (3.43, 3.23, 3.52, 3.94, 3.90, 3.71, 3.68, 3.40, 3.21, 2.72, 2.36, 2.30, 2.37, 2.45, None),
        (0.330, 0.352, 0.366, 0.456, 0.344, 0.329, 0.311, 0.323, 0.302, 0.278, 0.255, 0.246, 0.243,
         0.248, None),
        None, 4.07, 4.52),
    "51-music-144": (
        (22.3, 22.3, 26.7, -3.2),
        (3.42, 3.24, 3.47, 3.97, 3.91, 3.76, 3.68, 3.41, 3.21, 2.66, 2.44, 2.32, 2.30, 2.42, None),
        (0.329, 0.350, 0.368, 0.458, 0.348, 0.328, 0.305, 0.331, 0.302, 0.278, 0.245, 0.243, 0.242,
         0.244, None),
        None, 4.04, 4.53),
    "51-tones-96": (
        (44.7, 43.3, 18.7),
        None,
        None,
        5.4, 18.19, 4.63),
    "51-tones-128": (
        (46.9, 47.2, 52.5, 18.7),
        None,
        None,
        12.4, 15.10, 4.63),
    "51-tones-144": (
        (46.9, 47.2, 52.5, 18.7),
        None,
        None,
        12.4, 15.10, 4.63),
}
# DEE's 2.0 streams carry the same audio from 256 kbps up, the rest of each frame being fill,
# so every rate from 256 to 768 decodes to the same samples and takes the same pins.
for _rate in (256, 288, 320, 384, 448, 512, 768):
    PINS[f"20-music-{_rate}"] = ((35.7, 35.8), 1.54, None, 4.63)
    PINS[f"20-speech-{_rate}"] = ((38.4, 38.4), 0.83, None, 4.60)
    PINS[f"20-tones-{_rate}"] = ((49.0, 50.3), 10.44, None, 4.63)


def read_wav(path):
    """(samples of shape (n, channels) as float64, rate) from a PCM or IEEE float WAV."""
    blob = Path(path).read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    fmt = data = None
    pos = 12
    while pos + 8 <= len(blob):
        chunk, size = blob[pos:pos + 4], int.from_bytes(blob[pos + 4:pos + 8], "little")
        body = blob[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            fmt = body
        elif chunk == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        raise SystemExit(f"{path}: no fmt or data chunk")
    tag = int.from_bytes(fmt[0:2], "little")
    channels = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: the subformat's first two bytes
        tag = int.from_bytes(fmt[24:26], "little")
    if tag == 3 and bits == 32:
        samples = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 24:
        raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        value = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        samples = np.where(value >= 1 << 23, value - (1 << 24), value) / float(1 << 23)
    elif tag == 1 and bits == 16:
        samples = np.frombuffer(data, dtype="<i2") / 32768.0
    else:
        raise SystemExit(f"{path}: WAV format {tag} at {bits} bits is not read here")
    return samples.reshape(-1, channels), rate


def best_lag(reference, decoded, max_lag):
    """The shift d that best matches decoded[n + d] to reference[n], within +-max_lag."""
    size = 1 << int(np.ceil(np.log2(len(reference) + len(decoded))))
    spectrum = np.conj(np.fft.rfft(reference, size)) * np.fft.rfft(decoded, size)
    correlation = np.fft.irfft(spectrum, size)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    values = np.concatenate([correlation[:max_lag + 1], correlation[-max_lag:]])
    return int(lags[np.argmax(values)])


def tone_power(x, hz):
    """The power of x at hz, normalised so a sine of amplitude A reads A^2 / 4."""
    n = np.arange(len(x))
    projection = np.dot(x, np.exp(-2j * np.pi * hz * n / RATE)) / len(x)
    return float(np.abs(projection) ** 2)


def align(reference, decoded):
    """lag, and the reference and output over their overlap less EDGE at each end."""
    lag = best_lag(reference.sum(axis=1), decoded.sum(axis=1), 16384)
    if lag >= 0:
        count = min(len(reference), len(decoded) - lag)
        ref, out = reference[:count], decoded[lag:lag + count]
    else:
        count = min(len(reference) + lag, len(decoded))
        ref, out = reference[-lag:-lag + count], decoded[:count]
    return lag, ref[EDGE:-EDGE], out[EDGE:-EDGE]


def score(reference, decoded):
    """lag, per channel (gain in dB, SNR in dB over the whole band), and the aligned reference
    and output. score_ac4_encode.py scores the encoder's streams with it."""
    lag, ref, out = align(reference, decoded)
    channels = []
    for c in range(reference.shape[1]):
        gain = float(np.dot(ref[:, c], out[:, c]) / np.dot(ref[:, c], ref[:, c]))
        error = out[:, c] - gain * ref[:, c]
        snr = 10.0 * np.log10(np.dot(gain * ref[:, c], gain * ref[:, c]) / np.dot(error, error))
        channels.append((20.0 * np.log10(abs(gain)), float(snr)))
    return lag, channels, ref, out


def internal_rate(frame_rate_index):
    """The rate a frame at frame_rate_index is coded at (Part 1 Table 83)."""
    up, down = RESAMPLING[frame_rate_index]
    return RATE * down / up


def subband_hz(rate=None):
    """The width of a QMF subband at `rate`, the internal rate the QMF banks run at, RATE where
    none is given: half the sampling rate over 64. RATE is read at the call, since
    score_ac4_encode.py sets it for its 44.1 kHz legs."""
    return (RATE if rate is None else rate) / 128.0


def band_gain(ref, out, top_hz):
    """The least-squares gain of out against ref below top_hz, over band_snr's STFT frames: in
    ASPX, where above the crossover only the energy is the source's, the level below it."""
    window = np.hanning(FRAME)
    top = int(top_hz / (RATE / FRAME))
    cross = power = 0.0
    for start in range(0, len(ref) - FRAME, FRAME // 2):
        r = np.fft.rfft(window * ref[start:start + FRAME])[:top]
        o = np.fft.rfft(window * out[start:start + FRAME])[:top]
        cross += float(np.sum((np.conj(r) * o).real))
        power += float(np.sum(np.abs(r) ** 2))
    return cross / power


def lfe_gain(ref, out):
    """out's level against ref's over LFE_BAND_HZ, as an amplitude ratio: the square root of
    their energies there, from one transform of the whole aligned overlap. Energies, so that the
    phase of DEE's low-pass does not count as lost level."""
    r, o = np.fft.rfft(ref), np.fft.rfft(out)
    hz = np.fft.rfftfreq(len(ref), 1.0 / RATE)
    band = (hz >= LFE_BAND_HZ[0]) & (hz < LFE_BAND_HZ[1])
    return float(np.sqrt(np.sum(np.abs(o[band]) ** 2) / np.sum(np.abs(r[band]) ** 2)))


def band_snr(ref, out, gain, top_hz):
    """SNR in dB of out against gain * ref below top_hz, over half-overlapped Hann STFT frames."""
    window = np.hanning(FRAME)
    top = int(top_hz / (RATE / FRAME))
    signal = error = 0.0
    for start in range(0, len(ref) - FRAME, FRAME // 2):
        r = np.fft.rfft(window * ref[start:start + FRAME])[:top]
        o = np.fft.rfft(window * out[start:start + FRAME])[:top]
        signal += float(np.sum(np.abs(gain * r) ** 2))
        error += float(np.sum(np.abs(o - gain * r) ** 2))
    return 10.0 * np.log10(signal / error)


def aspx_groups(values, offset):
    """The low-resolution signal subband groups (Pseudocodes 67 to 69) from a leg's
    aspx_config() and an aspx_data element's crossover offset; their first border is the
    crossover, sbx."""
    scale, start, stop = (values[name] for name in ASPX_CONFIG_FIELDS)
    template = SBG_TEMPLATE_HIGHRES if scale else SBG_TEMPLATE_LOWRES
    num_master = (22 if scale else 20) - 2 * start - 2 * stop
    master = template[2 * start:2 * start + num_master + 1]
    high = master[offset:]
    num_high = len(high) - 1
    num_low = num_high - num_high // 2
    return [high[0]] + [high[2 * g] if num_high % 2 == 0 else high[2 * g - 1]
                        for g in range(1, num_low + 1)]


def trace_values(trace):
    """The first aspx_config()'s ASPX_CONFIG_FIELDS in a syntax trace, and the
    aspx_xover_subband_offset of each aspx_data element of the frame that sent it, in syntax
    order; None when the trace has none."""
    values = {}
    offsets = []
    frame = None
    for line in Path(trace).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 6:
            continue
        if fields[5] in ASPX_CONFIG_FIELDS and (frame is None or fields[0] == frame):
            frame = fields[0]
            values.setdefault(fields[5], int(fields[4]))
        elif fields[5] == "aspx_xover_subband_offset" and fields[0] == frame:
            offsets.append(int(fields[4]))
        elif frame is not None and fields[0] != frame:
            break
    if len(values) != len(ASPX_CONFIG_FIELDS) or not offsets:
        return None
    return values, offsets


def tile_error(ref, out, groups, rate=None):
    """The mean absolute dB difference of out's tile energies from ref's, per frame and
    low-resolution group above the crossover, over the tiles where ref's is above the floor; the
    groups' subbands are the QMF banks' at `rate`, the internal rate (subband_hz's default)."""
    window = np.hanning(FRAME)
    bin_hz = RATE / FRAME
    # A full-scale sine's energy in one frame through the window: (FRAME / 4)^2.
    full_scale = (FRAME / 4.0) ** 2
    differences = []
    for start in range(0, len(ref) - FRAME, FRAME):
        r = np.abs(np.fft.rfft(window * ref[start:start + FRAME])) ** 2
        o = np.abs(np.fft.rfft(window * out[start:start + FRAME])) ** 2
        for low, high in itertools.pairwise(groups):
            first = int(low * subband_hz(rate) / bin_hz)
            last = int(high * subband_hz(rate) / bin_hz)
            er, eo = float(r[first:last].sum()), float(o[first:last].sum())
            per_subband = er / (high - low) / full_scale
            if per_subband > 10.0 ** (TILE_FLOOR_DB / 10.0):
                differences.append(10.0 * np.log10(max(eo, 1e-30) / er))
    return differences


def decode(cli, stream, out_wav, trace=None):
    """ac3cli's decode of `stream`, with its syntax trace written to `trace` when one is given."""
    command = [str(cli), "decode", str(stream), str(out_wav)]
    if trace is not None:
        command.append(f"syntax-trace={trace}")
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream}: ac3cli decode failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
    return read_wav(out_wav)


def lo_ro(five_one):
    """The Lo/Ro downmix of an L R C LFE Ls Rs source, centre and surrounds at -3 dB."""
    k = 1.0 / np.sqrt(2.0)
    left = five_one[:, 0] + k * five_one[:, 2] + k * five_one[:, 4]
    right = five_one[:, 1] + k * five_one[:, 2] + k * five_one[:, 5]
    return np.stack([left, right], axis=1)


def chosen(leg):
    return (leg.get("codec_mode") in ("SIMPLE", "ASPX", "ASPX_ACPL_2", "ASPX_ACPL_3")
            and leg.get("frame_rate_index") in RESAMPLING
            and leg.get("output_channel_layout") in ("stereo", "mono", "IMS", "5.1")
            and any(option.startswith("measure_only") for option in leg.get("options", [])))


def legs_committed(work):
    """(name, stream, source WAV path, source name, encoder, codec mode, frame_rate_index) for
    every committed leg decode reads.

    The manifest records what each stream turned out to be and a digest of the source it was
    made from; gen_ac4_baseline.py's LEGS names the source, which is rebuilt here and must
    hash to that digest."""
    manifest = json.loads((BASELINE_DIR / "ac4-manifest.json").read_text(encoding="utf-8"))
    source_of = {leg["name"]: leg["source"] for leg in baseline.LEGS}
    picked = {name: leg for name, leg in manifest["legs"].items() if chosen(leg)}
    sources = sorted({source_of[name] for name in picked})
    paths = baseline.build_sources(work / "sources", baseline.COMMITTED_SECONDS, sources)
    legs = []
    for name, leg in sorted(picked.items()):
        path = paths[source_of[name]]
        if baseline.sha256(path) != leg["source_sha256"]:
            raise SystemExit(f"{name}: the rebuilt source {path.name} does not hash to the "
                             "manifest's source_sha256")
        legs.append((name, BASELINE_DIR / name / "dee.ac4", path, source_of[name], leg["encoder"],
                     leg["codec_mode"], leg["frame_rate_index"]))
    return legs


def legs_gold(gold):
    manifest = json.loads((gold / "gold-manifest.json").read_text(encoding="utf-8"))
    return [(name, gold / "streams" / name / "dee.ac4", gold / "sources" / f"{leg['source']}.wav",
             leg["source"], leg["encoder"], leg["codec_mode"], leg["frame_rate_index"])
            for name, leg in sorted(manifest["legs"].items()) if chosen(leg)]


def acpl_units(codec_mode, channels):
    """ACPL_UNIT's entry for a leg: in the channel pair, one aspx_data element carries both."""
    return (0, 0) if channels == 2 else ACPL_UNIT[codec_mode]


def acpl_pairs(channels):
    """The pairs A-CPL rebuilds from one downmix each, by the decoder's channel order: the channel
    pair's, 5.0's (L, Ls) and (R, Rs), or 5.1's (ACPL_PAIRS)."""
    return {2: ((0, 1),), 5: ((0, 3), (1, 4))}.get(channels, ACPL_PAIRS)


def acpl_downmixes(signal, codec_mode):
    """The signals the A-CPL leg's waveform codes, as the output or the source carries them, by the
    decoder's channel order: the channel pair's (L + R) / 2; or the 5.X element's downmixes, then
    5.1's LFE."""
    k = 1.0 / np.sqrt(2.0)
    if signal.shape[1] == 2:
        return [(signal[:, 0] + signal[:, 1]) / 2.0]
    ls, rs = (3, 4) if signal.shape[1] == 5 else (4, 5)
    lfe = [] if signal.shape[1] == 5 else [signal[:, 3]]
    if codec_mode == "ASPX_ACPL_3":
        norm = 1.0 + np.sqrt(2.0)
        lo = (signal[:, 0] + k * signal[:, 2] + k * signal[:, ls]) / norm
        ro = (signal[:, 1] + k * signal[:, 2] + k * signal[:, rs]) / norm
        return [lo, ro, *lfe]
    return [(signal[:, 0] + k * signal[:, ls]) / 2.0, (signal[:, 1] + k * signal[:, rs]) / 2.0,
            signal[:, 2], *lfe]


def acpl_band_scores(ref, out):
    """Per parameter band, the larger over ACPL_PAIRS of the mean |ILD_out - ILD_ref| in dB and of
    the mean |rho_out - rho_ref|, over half-overlapped Hann frames where both channels of the
    source's pair are above TILE_FLOOR_DB per subband; None for a band where no frame is (the
    sources hold nothing above 13 kHz)."""
    window = np.hanning(FRAME)
    bin_hz = RATE / FRAME
    full_scale = (FRAME / 4.0) ** 2
    starts = range(0, len(ref) - FRAME, FRAME // 2)
    spec_r = np.array([np.fft.rfft(window[:, None] * ref[s:s + FRAME], axis=0) for s in starts])
    spec_o = np.array([np.fft.rfft(window[:, None] * out[s:s + FRAME], axis=0) for s in starts])
    ild, rho = [], []
    for low, high in itertools.pairwise(ACPL_BAND_SUBBANDS):
        first = round(low * subband_hz() / bin_hz)
        last = round(high * subband_hz() / bin_hz)
        floor = full_scale * (high - low) * 10.0 ** (TILE_FLOOR_DB / 10.0)
        band_ild, band_rho = None, None
        for a, b in acpl_pairs(ref.shape[1]):
            measures = []
            for spec in (spec_r, spec_o):
                xa, xb = spec[:, first:last, a], spec[:, first:last, b]
                ea, eb = np.sum(np.abs(xa) ** 2, axis=1), np.sum(np.abs(xb) ** 2, axis=1)
                cross = np.sum((xa * np.conj(xb)).real, axis=1)
                measures.append((ea, eb, cross))
            (ea_r, eb_r, c_r), (ea_o, eb_o, c_o) = measures
            keep = (ea_r > floor) & (eb_r > floor) & (ea_o > 0.0) & (eb_o > 0.0)
            if not keep.any():
                continue
            ild_r = 10.0 * np.log10(ea_r[keep] / eb_r[keep])
            ild_o = 10.0 * np.log10(ea_o[keep] / eb_o[keep])
            rho_r = c_r[keep] / np.sqrt(ea_r[keep] * eb_r[keep])
            rho_o = c_o[keep] / np.sqrt(ea_o[keep] * eb_o[keep])
            band_ild = max(band_ild or 0.0, float(np.mean(np.abs(ild_o - ild_r))))
            band_rho = max(band_rho or 0.0, float(np.mean(np.abs(rho_o - rho_r))))
        ild.append(band_ild)
        rho.append(band_rho)
    return ild, rho


def routing_margins(out):
    """Per channel of a tone leg, its own tone's power over the loudest other tone in it, in dB."""
    margins = []
    for c in range(out.shape[1]):
        own = tone_power(out[:, c], baseline.TONE_HZ[c])
        leak = max(tone_power(out[:, c], baseline.TONE_HZ[other])
                   for other in range(out.shape[1]) if other != c)
        margins.append(10.0 * np.log10(own / max(leak, 1e-30)))
    return margins


def acpl_pin_text(name, floors, ild, rho, rest):
    """An ACPL_PINS entry as --measure prints it, each field on its own lines within 100
    columns."""
    indent = " " * 8

    def field(values, last):
        if values is None:
            return [f"{indent}None{last}"]
        lines, line = [], indent + "("
        for i, value in enumerate(values):
            closing = "," * (len(values) == 1) + ")" if i == len(values) - 1 else ", "
            piece = value + closing
            if len(line) + len(piece.rstrip()) > 99:
                lines.append(line.rstrip())
                line = indent + " "
            line += piece
        lines.append(line + last)
        return lines

    text = [f'    "{name}": (']
    text += field(floors, ",") + field(ild, ",") + field(rho, ",")
    text.append(f"{indent}{', '.join(rest)}),")
    return "\n".join(text)


def score_acpl(name, codec_mode, source_name, ref, out, offsets, config, args, failures, pins,
               table=None):
    """An A-CPL leg's downmix, band and routing checks (the docstring's A-CPL paragraph), against
    `table`'s pins (ACPL_PINS by default); what it measured, as a dict."""
    units = acpl_units(codec_mode, ref.shape[1])
    downmix_units = [u for u in units if u is not None]
    if len(offsets) <= max(downmix_units):
        failures.append(f"{name}: no aspx_data elements for its channels in its syntax trace")
        return
    groups = aspx_groups(config, offsets[0])
    refs, outs = acpl_downmixes(ref, codec_mode), acpl_downmixes(out, codec_mode)
    # Which of the downmixes an aspx_data element carries, and the LFE (last) over the whole band:
    # the pair's one, ASPX_ACPL_3's two, or ASPX_ACPL_1's and 2's two and C.
    five_x = [0, 0] if codec_mode == "ASPX_ACPL_3" else [0, 0, 1]
    carried = [0] if ref.shape[1] == 2 else five_x
    snrs, cells = [], []
    for i, (r, o) in enumerate(zip(refs, outs, strict=True)):
        if i < len(carried):
            top_hz = (aspx_groups(config, offsets[carried[i]])[0] - 1) * subband_hz()
            snr = band_snr(r, o, band_gain(r, o, top_hz), top_hz)
        else:
            gain = float(np.dot(r, o) / np.dot(r, r))
            error = o - gain * r
            snr = 10.0 * np.log10(np.dot(gain * r, gain * r) / np.dot(error, error))
        snrs.append(float(snr))
        cells.append(f"dmx{i} {snr:.2f} dB")
    tones = source_name.startswith("tones")
    ild, rho = (None, None) if tones else acpl_band_scores(ref, out)
    routing = min(routing_margins(out)) if tones else None
    lsd, _ = quality_race.spectral_scores(ref, out)
    mos = quality_race.perceptual_score(ref, out, RATE)
    bands_text = ("" if ild is None else
                  f"  ILD {max(v for v in ild if v is not None):.2f} dB, "
                  f"rho {max(v for v in rho if v is not None):.3f} (worst band)")
    routing_text = "" if routing is None else f"  routing {routing:.1f} dB"
    mos_text = "-" if mos is None else f"{mos:.2f}"
    print(f"{name:<32} {codec_mode}  {'  '.join(cells)}{bands_text}{routing_text}  LSD {lsd:.2f} dB"
          f"  MOS {mos_text} (xover {groups[0] * subband_hz() / 1000:.2f} kHz)", flush=True)
    floors = [f"{s - SNR_MARGIN_DB:.1f}" for s in snrs]
    ild_pin = None if ild is None else ["None" if v is None else f"{v + ACPL_ILD_MARGIN_DB:.2f}"
                                        for v in ild]
    rho_pin = None if rho is None else ["None" if v is None else f"{v + ACPL_RHO_MARGIN:.3f}"
                                        for v in rho]
    routing_pin = "None" if routing is None else f"{routing - ACPL_ROUTING_MARGIN_DB:.1f}"
    mos_pin = "None" if mos is None else f"{mos - MOS_MARGIN:.2f}"
    pins.append(acpl_pin_text(name, floors, ild_pin, rho_pin,
                              [routing_pin, f"{lsd + LSD_MARGIN_DB:.2f}", mos_pin]))
    measured = {"snrs": snrs, "ild": ild, "rho": rho, "routing": routing, "lsd": float(lsd),
                "mos": mos}
    if args.measure:
        return measured
    pin = (ACPL_PINS if table is None else table).get(name)
    if pin is None:
        failures.append(f"{name}: nothing pinned for its A-CPL checks")
        return measured
    snr_floors, ild_ceilings, rho_ceilings, routing_floor, lsd_ceiling, mos_floor = pin
    for i, snr in enumerate(snrs):
        if snr < snr_floors[i]:
            failures.append(f"{name} downmix {i}: SNR {snr:.2f} dB below its floor {snr_floors[i]}")
    if ild_ceilings is not None:
        for band, (value, ceiling) in enumerate(zip(ild, ild_ceilings, strict=True)):
            if ceiling is None or value is None:
                if (ceiling is None) != (value is None):
                    failures.append(f"{name} band {band}: frames to score where none were pinned, "
                                    "or none where some were")
                continue
            if value > ceiling:
                failures.append(f"{name} band {band}: level difference {value:.2f} dB from the "
                                f"source's, above its ceiling {ceiling}")
        for band, (value, ceiling) in enumerate(zip(rho, rho_ceilings, strict=True)):
            if ceiling is None or value is None:
                continue
            if value > ceiling:
                failures.append(f"{name} band {band}: correlation {value:.3f} from the source's, "
                                f"above its ceiling {ceiling}")
    if routing_floor is not None and routing < routing_floor:
        failures.append(f"{name}: routing margin {routing:.1f} dB below its floor {routing_floor}")
    if lsd > lsd_ceiling:
        failures.append(f"{name}: LSD {lsd:.2f} dB above its ceiling {lsd_ceiling}")
    if mos is not None and mos_floor is not None and mos < mos_floor:
        failures.append(f"{name}: MOS {mos:.2f} below its floor {mos_floor}")
    return measured


def pin_text(name, snrs, lsd, tiles, mos):
    floors = ", ".join(f"{snr - SNR_MARGIN_DB:.1f}" for snr in snrs)
    tile = "None" if tiles is None else f"{tiles + TILE_MARGIN_DB:.2f}"
    mos_text = "None" if mos is None else f"{mos - MOS_MARGIN:.2f}"
    return f'    "{name}": (({floors},), {lsd + LSD_MARGIN_DB:.2f}, {tile}, {mos_text}),'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="the ac3cli to decode with")
    parser.add_argument("--gold", type=Path, help="score G0's local gold set in this directory")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument("--measure", action="store_true",
                        help="print every leg's measurements and check nothing")
    parser.add_argument("--only", nargs="+", metavar="LEG", help="score only these legs")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        legs = legs_gold(args.gold) if args.gold else legs_committed(work)
        if args.only:
            legs = [leg for leg in legs if leg[0] in args.only]
        if not legs:
            raise SystemExit("no leg to score")
        failures = []
        pins = []
        for name, stream, source_path, source_name, encoder, codec_mode, rate_index in legs:
            source, source_rate = read_wav(source_path)
            decoded, rate = decode(args.cli, stream, work / f"{name}.wav", work / f"{name}.trace")
            if rate != RATE or source_rate != RATE:
                failures.append(f"{name}: decoded at {rate} Hz, source at {source_rate} Hz")
                continue
            ims = encoder == baseline.IMS
            reference = lo_ro(source) if ims else source
            if decoded.shape[1] != reference.shape[1]:
                failures.append(f"{name}: {decoded.shape[1]} channels decoded, the reference has "
                                f"{reference.shape[1]}")
                continue
            internal = internal_rate(rate_index)
            lag, ref, out = align(reference, decoded)
            expected_lag = (LAG[encoder] if rate_index == 13
                            else LAG_AT_RATE.get((encoder, rate_index)))
            if not args.measure and lag != expected_lag:
                failures.append(f"{name}: lag {lag}, expected {expected_lag}")
            if codec_mode in ACPL_UNIT:
                found = trace_values(work / f"{name}.trace")
                if found is None:
                    failures.append(f"{name}: no aspx_config() in its syntax trace")
                    continue
                config, offsets = found
                score_acpl(name, codec_mode, source_name, ref, out, offsets, config, args, failures,
                           pins)
                continue
            channel_groups = [None] * reference.shape[1]
            if codec_mode == "ASPX":
                found = trace_values(work / f"{name}.trace")
                units = ASPX_UNIT.get(reference.shape[1])
                if found is None or units is None or len(found[1]) <= max(u or 0 for u in units):
                    failures.append(f"{name}: no aspx_config() and aspx_data elements for its "
                                    "channels in its syntax trace")
                    continue
                config, offsets = found
                channel_groups = [None if u is None else aspx_groups(config, offsets[u])
                                  for u in units]
            groups = next((g for g in channel_groups if g is not None), None)
            cells, snrs, tiles = [], [], []
            for c in range(reference.shape[1]):
                r, o = ref[:, c], out[:, c]
                gain = float(np.dot(r, o) / np.dot(r, r))
                if channel_groups[c] is None:
                    error = o - gain * r
                    snr = 10.0 * np.log10(np.dot(gain * r, gain * r) / np.dot(error, error))
                else:
                    top_hz = (channel_groups[c][0] - 1) * subband_hz(internal)
                    gain = band_gain(r, o, top_hz)
                    snr = band_snr(r, o, gain, top_hz)
                    tiles += tile_error(r, o, channel_groups[c], internal)
                snrs.append(float(snr))
                correlation = float(np.dot(r, o) / np.sqrt(np.dot(r, r) * np.dot(o, o)))
                lfe = LFE_CHANNEL.get(reference.shape[1]) == c
                if lfe:
                    gain = lfe_gain(r, o)
                gain_db = 20.0 * np.log10(abs(gain))
                cells.append(f"ch{c} {gain_db:+.3f} dB {snr:.2f} dB"
                             + (f" r {correlation:.3f}" if ims else ""))
                if args.measure:
                    continue
                if ims and correlation < IMS_CORRELATION:
                    failures.append(f"{name} ch{c}: correlation with the Lo/Ro downmix "
                                    f"{correlation:.3f}, under {IMS_CORRELATION}")
                tolerance = LFE_GAIN_TOLERANCE_DB if lfe else GAIN_TOLERANCE_DB
                if not ims and abs(gain_db) > tolerance:
                    failures.append(f"{name} ch{c}: gain {gain_db:+.3f} dB, beyond "
                                    f"+-{tolerance} dB of unity")
            tile_mean = float(np.mean(np.abs(tiles))) if tiles else None
            lsd, _ = quality_race.spectral_scores(ref, out)
            mos = quality_race.perceptual_score(ref, out, RATE)
            tile_text = "" if tile_mean is None else f"  tiles {tile_mean:.2f} dB ({len(tiles)})"
            mos_text = "-" if mos is None else f"{mos:.2f}"
            where = f" (xover {groups[0] * subband_hz(internal) / 1000:.2f} kHz)" if groups else ""
            print(f"{name:<32} lag {lag:5d}  {'  '.join(cells)}{where}  LSD {lsd:.2f} dB"
                  f"{tile_text}  MOS {mos_text}", flush=True)
            pins.append(pin_text(name, snrs, float(lsd), tile_mean, mos))
            if args.measure:
                continue
            pin = PINS.get(name)
            if pin is None:
                failures.append(f"{name}: nothing pinned in PINS")
            else:
                snr_floors, lsd_ceiling, tile_ceiling, mos_floor = pin
                for c, snr in enumerate(snrs):
                    if snr < snr_floors[c]:
                        failures.append(f"{name} ch{c}: SNR {snr:.2f} dB below its floor "
                                        f"{snr_floors[c]}")
                if lsd > lsd_ceiling:
                    failures.append(f"{name}: LSD {lsd:.2f} dB above its ceiling {lsd_ceiling}")
                if tile_ceiling is not None and (tile_mean is None or tile_mean > tile_ceiling):
                    failures.append(f"{name}: A-SPX tiles {tile_mean} dB from the reference's, "
                                    f"above the ceiling {tile_ceiling}")
                if mos is not None and mos_floor is not None and mos < mos_floor:
                    failures.append(f"{name}: MOS {mos:.2f} below its floor {mos_floor}")
            if source_name.startswith("tones"):
                for c in range(out.shape[1]):
                    own = tone_power(out[:, c], baseline.TONE_HZ[c])
                    for other in range(out.shape[1]):
                        if other == c:
                            continue
                        leak = tone_power(out[:, c], baseline.TONE_HZ[other])
                        margin = 10.0 * np.log10(own / max(leak, 1e-30))
                        if margin < ROUTING_MARGIN_DB:
                            failures.append(f"{name} ch{c}: its tone only {margin:.1f} dB above "
                                            f"ch{other}'s")
        if args.measure:
            print("\nPINS and ACPL_PINS lines:")
            print("\n".join(pins))
            return 0
        if failures:
            print("\nFAILED:")
            for failure in failures:
                print(f"  {failure}")
            return 1
        print(f"\n{len(legs)} legs: lag, level, SNR, tiles, A-CPL's downmixes and bands, LSD, MOS "
              "and routing all hold")
        return 0


if __name__ == "__main__":
    sys.exit(main())
