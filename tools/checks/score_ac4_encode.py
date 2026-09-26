"""Score ac3cli's AC-4 encoding: the encoder's streams, decoded, against their sources.

planning/ac4.md, the encoder's ladder, items 4 and 5, as phases E1 to E5 and E8 need them: SIMPLE,
ASPX and A-CPL, mono, stereo, 5.0 and 5.1, at frame_rate_index 13 and, in the frame-rate legs, the
others, and the immersive element's 5.1.4. Each leg encodes a source with `ac3cli
ac4-encode`, which picks the codec mode from the rate, decodes the stream with `ac3cli decode`,
aligns the output with the source by cross-correlation, fits a least-squares gain per channel,
and checks, as score_ac4_decode.py scores DEE's streams:

  lag      the output lags the source by LAG samples: the encoder's delay, a frame and a half,
           and the decoder's, 1 313 at frame_rate_index 13 (score_ac4_decode.py);
  gain     every channel within 0.2 dB of unity, where its SNR is 20 dB or more: below that the
           least-squares gain of a coarse quantiser's output is no measure of level. In ASPX the
           gain is fitted below the crossover, where the output is the source's waveform, and a
           5.1 leg's LFE's is taken from 20 to 100 Hz, as score_ac4_decode.py takes it;
  SNR      every channel's signal-to-noise ratio at or above its floor, the first measurement
           less 1 dB (FLOORS): over the whole band in SIMPLE and for the LFE, and in ASPX below the
           crossover of the aspx_data element that carries the channel, which the stream's own
           aspx_config() and aspx_data elements give (score_ac4_decode.py's ASPX_UNIT);
  tiles    in ASPX, above the crossover, the mean absolute difference between the A-SPX tiles'
           energy and the source's at or below its ceiling, the first measurement plus 0.5 dB;
  LSD      tools/ci/quality_race.py's log-spectral distance at or below its ceiling, the first
           measurement plus 0.5 dB;
  MOS      ViSQOL's MOS-LQO (quality_race.perceptual_score) at or above its floor, the first
           measurement less 0.1, where visqol-python is installed;
  routing  on a tone leg, each channel's tone at least 40 dB above the other channel's in it.

The A-CPL legs, 5.1 at 96 and 128 kbps, where the encoder picks ASPX_ACPL_3 and ASPX_ACPL_2 as
DEE does, 5.0 in ASPX_ACPL_2, and the experimental ASPX_ACPL_1 and A-CPL in stereo, are scored as
score_ac4_decode.py scores DEE's A-CPL legs: the coded downmixes, recovered from the output, as
waveforms below the crossover; per A-CPL parameter band the distance of each rebuilt pair's level
difference and correlation from the source's; on a tone leg the routing margin; LSD and MOS. Their
pins are ACPL_FLOORS, in score_ac4_decode.py's ACPL_PINS form. The codec mode is read from the
decode's syntax trace.

The sources are 5 s long: the committed programme fixtures' 2.0 cuts, their 5.1 music and film
mixes and one tone per channel, rebuilt by tools/generators/gen_ac4_baseline.py (which reads the
FLAC fixtures with ffmpeg, so ffmpeg must be on PATH); the mono downmixes and the 5.1 music
without its LFE; and synthetic signals made here: a sweep, noise, castanet-like bursts after
silence, and a source panned between the channels, which the encoder's stereo prediction codes.
A leg may name ac4-encode's options after its rate: the -configs legs take the experimental coding
configurations.

The immersive legs (phase E8) encode tones and music in 5.1.4, from 192 to 768 kbps, where the
encoder picks ASPX_ACPL_2, ASPX_SCPL and SCPL as DEE does, and in ASPX_ACPL_1 by name
(experimental), and score each in full and in core decoding with score_ac4_decode.py's
score_immersive(): per channel (in full decoding ASPX_ACPL_2's top pairs as their sums, which A-CPL
keeps; in core decoding the 5.1.2 the core renders to) the lag, the level, the SNR below the lowest
crossover (in SCPL below 16 kHz), on a tone leg the routing margin, the LSD and the MOS, pinned in
IMMERSIVE_FLOORS. In full decoding the A-SPX tiles above the lowest crossover, and in the
parametric modes the level difference and correlation per A-CPL parameter band of the pairs the
parameters shape, are held to IMMERSIVE_EXTRA as the tiles and bands above are held. Those pairs
are the top pairs A-CPL rebuilds in ASPX_ACPL_1 and 2, and in ASPX_AJCC (experimental) each
side's front (L, Tfl) and back (Ls, Tbl), whose A-JCC modules rebuild them from one core channel
each; there the waveform SNR of the channels the modules make is pinned as it comes, a
decorrelated part having none.

The frame-rate legs (phase E5) encode music and speech in stereo and music in 5.1 at the other
frame rates of Part 1 Table 83 and at index 13, in the same run, and hold each to the index-13
stream of the same source and rate on the log-spectral distance and MOS alone: at the other frame
rates the delay is a fraction of a sample off the output's grid, which SNR and the lag would read
as error. Their pins, FRAME_RATE_PINS, are how far above index 13's LSD and below its MOS each may
land: the first measurement plus 0.25 dB and less 0.05.

--gold DIR runs the race instead, locally: for each of phase G0's 2.0 legs, ASPX from 48 to 144
kbps and SIMPLE from 192 to 768, and its 5.1 legs from 96 to 768, ASPX_ACPL_3 at 96, ASPX_ACPL_2 at
128 and 144 and ASPX to 320 kbps, DEE's stream
(DIR/streams/<leg>/dee.ac4) and this encoder's stream of the same source (DIR/sources/<source>.wav)
at the same rate are both decoded and scored as above. The encoder's scores are checked against
RACE, and in the A-CPL legs against RACE_ACPL, pinned the same way, and its gaps to DEE's are
printed. The race's 5.1.4 legs are G0's music and tones and G1's film, speech, sweeps and
transients (the manifest's g1_legs) at each of DEE's 5.1.4 rates, 192 to 768 kbps, scored as the
immersive legs are, in full and in core decoding, against RACE_IMMERSIVE and
RACE_IMMERSIVE_EXTRA.

--only TEXT runs only the legs whose names contain TEXT.

--librempeg PATH also decodes every stream with librempeg's ffmpeg, run in WSL (the path is the
WSL one), and reports its scores against the source and how far its output is from the
decoder's; of a 5.1.4 stream, for each source channel the output channel that follows it most
closely, with their correlation and level. Nothing it measures is checked: planning/ac4.md,
decision 13.

--measure prints what every leg measures and checks nothing, for pinning a new leg.

Usage:
    python tools/checks/score_ac4_encode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/score_ac4_encode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
        [--librempeg /mnt/d/ac3bld/librempeg/install/bin/ffmpeg]
"""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "generators"))
sys.path.insert(0, str(REPO / "tools" / "ci"))
sys.path.insert(0, str(REPO / "tools" / "checks"))
import gen_ac4_baseline as baseline  # noqa: E402
import quality_race  # noqa: E402
import score_ac4_decode as decoding  # noqa: E402

SECONDS = 5.0
# The encoder's delay, 3 072 samples, and the decoder's, 1 313, at frame_rate_index 13.
LAG = 3072 + decoding.DECODER_DELAY
GAIN_TOLERANCE_DB = 0.2
GAIN_MIN_SNR_DB = 20.0
ROUTING_MARGIN_DB = 40.0
SNR_MARGIN_DB = 1.0
LSD_MARGIN_DB = 0.5
TILE_MARGIN_DB = 0.5
MOS_MARGIN = 0.1

# name: (source, sample rate, kbps). The source's name ends in its channel count, 20 or 10. The
# encoder picks the codec mode: ASPX below 96 kbps a channel, SIMPLE from there.
LEGS = {
    "20-music-48": ("music_20", 48000, 48),
    "20-music-64": ("music_20", 48000, 64),
    "20-music-96": ("music_20", 48000, 96),
    "20-music-128": ("music_20", 48000, 128),
    "20-music-192": ("music_20", 48000, 192),
    "20-music-256": ("music_20", 48000, 256),
    "20-speech-48": ("speech_20", 48000, 48),
    "20-speech-128": ("speech_20", 48000, 128),
    "20-speech-192": ("speech_20", 48000, 192),
    "20-tones-64": ("tones_20", 48000, 64),
    "20-tones-192": ("tones_20", 48000, 192),
    "20-sweep-96": ("sweep_20", 48000, 96),
    "20-sweep-192": ("sweep_20", 48000, 192),
    "20-noise-192": ("noise_20", 48000, 192),
    "20-castanets-64": ("castanets_20", 48000, 64),
    "20-castanets-192": ("castanets_20", 48000, 192),
    "20-panned-128": ("panned_20", 48000, 128),
    "10-music-64": ("music_10", 48000, 64),
    "10-speech-24": ("speech_10", 48000, 24),
    "10-speech-48": ("speech_10", 48000, 48),
    "20-sweep-64-44k": ("sweep_20", 44100, 64),
    "20-tones-192-44k": ("tones_20", 44100, 192),
    "20-sweep-192-44k": ("sweep_20", 44100, 192),
    "51-music-192": ("music_51", 48000, 192),
    "51-music-384": ("music_51", 48000, 384),
    "51-film-256": ("film_51", 48000, 256),
    "51-tones-192": ("tones_51", 48000, 192),
    "51-tones-384": ("tones_51", 48000, 384),
    "50-music-320": ("music_50", 48000, 320),
    "51-music-192-configs": ("music_51", 48000, 192, "experimental=coding-configs"),
    "51-music-384-configs": ("music_51", 48000, 384, "experimental=coding-configs"),
    # A-CPL: ASPX_ACPL_3 below 22.4 kbps a channel, ASPX_ACPL_2 below 33.6; ASPX_ACPL_1 and the
    # channel pair's modes by name, experimental.
    "51-music-96": ("music_51", 48000, 96),
    "51-music-128": ("music_51", 48000, 128),
    "51-film-128": ("film_51", 48000, 128),
    "51-tones-96": ("tones_51", 48000, 96),
    "51-tones-128": ("tones_51", 48000, 128),
    "50-music-112": ("music_50", 48000, 112),
    "51-music-160-acpl1": ("music_51", 48000, 160, "codec-mode=aspx-acpl-1", "experimental=acpl"),
    "20-music-32-acpl2": ("music_20", 48000, 32, "codec-mode=aspx-acpl-2", "experimental=acpl"),
    "20-music-64-acpl1": ("music_20", 48000, 64, "codec-mode=aspx-acpl-1", "experimental=acpl"),
}

# Per leg: (SNR floor per channel in dB, LSD ceiling in dB, A-SPX tile ceiling in dB or None for
# SIMPLE, MOS floor), the first measurement less 1 dB, plus 0.5 dB, plus 0.5 dB and less 0.1.
# Measured 2026-09-25 with the encoder of phase E2 and the decoder of phase D4, whose QMF banks
# every decode passes through: they reconstruct to about 78 dB, which bounds the sweeps' SNR.
# Phase D4's reading of pre-flattening (src/ac4dec/ERRATA.md, "Pre-flattening's direction") moved
# the ASPX legs' pins from those phase E2 took. The 5.0 and 5.1 legs, measured with the encoder of
# phase E3, are scored per channel below its own crossover, and a 5.1 leg's LFE over its whole band,
# where the 140.6 Hz the LFE is coded to (three scale factor bands, as DEE's) leaves the source's
# content above it, from its 120 Hz low-pass, as noise. The -configs legs take the experimental
# coding configurations.
FLOORS = {
    "20-music-48": ((14.1, 14.9), 2.02, 2.25, 4.40),
    "20-music-64": ((17.5, 18.1), 1.56, 2.40, 4.50),
    "20-music-96": ((22.0, 22.4), 1.22, None, 4.57),
    "20-music-128": ((32.0, 32.7), 0.95, None, 4.60),
    "20-music-192": ((42.5, 43.3), 1.60, None, 4.60),
    "20-music-256": ((49.7, 50.4), 1.53, None, 4.60),
    "20-speech-48": ((22.3, 22.3), 1.42, 2.20, 4.22),
    "20-speech-128": ((43.4, 43.4), 0.79, 3.35, 4.30),
    "20-speech-192": ((53.4, 53.4), 0.58, None, 4.62),
    "20-tones-64": ((54.8, 57.8), 12.83, None, 4.63),
    "20-tones-192": ((81.4, 81.8), 9.98, None, 4.63),
    "20-sweep-96": ((47.3, 47.4), 16.02, 9.25, 4.63),
    "20-sweep-192": ((75.7, 75.6), 12.77, None, 4.63),
    "20-noise-192": ((12.0, 12.0), 1.02, None, 3.72),
    "20-castanets-64": ((13.7, 13.9), 3.01, 2.16, 4.56),
    "20-castanets-192": ((6.8, 6.8), 1.96, None, 4.62),
    "20-panned-128": ((42.3, 38.5), 0.89, None, 4.61),
    "10-music-64": ((26.0,), 1.05, None, 4.59),
    "10-speech-24": ((9.2,), 2.53, 2.24, 4.17),
    "10-speech-48": ((22.5,), 1.46, 3.33, 4.23),
    "20-sweep-64-44k": ((47.1, 47.2), 16.02, 12.82, 4.63),
    "20-tones-192-44k": ((81.4, 79.1), 8.95, None, 4.63),
    "20-sweep-192-44k": ((75.9, 75.9), 13.16, None, 4.63),
    "51-music-192": ((11.1, 12.2, 13.7, 7.8, 12.8, 13.2), 2.35, 4.03, 4.58),
    "51-music-384": ((33.1, 33.8, 36.6, 16.3, 36.3, 36.6), 2.19, None, 4.61),
    "51-film-256": ((18.5, 19.4, 31.3, 14.3, 24.1, 24.3), 1.89, 2.23, 4.59),
    "51-tones-192": ((80.3, 80.4, 79.9, 69.1, 81.1, 78.7), 10.80, None, 4.63),
    "51-tones-384": ((81.4, 81.7, 80.5, 69.1, 82.1, 79.3), 10.52, None, 4.63),
    "50-music-320": ((28.5, 29.2, 31.9, 31.6, 31.9), 0.93, 5.22, 4.62),
    "51-music-192-configs": ((10.7, 11.8, 13.0, 7.9, 13.2, 13.5), 2.37, 4.30, 4.58),
    "51-music-384-configs": ((32.8, 33.5, 35.7, 16.3, 36.5, 36.8), 2.20, None, 4.61),
}

# The A-CPL legs, in score_ac4_decode.py's ACPL_PINS form: the downmixes' SNR floors, the per-band
# level difference and correlation ceilings (None where the source holds nothing to score), the
# routing floor on a tone leg, the LSD ceiling and the MOS floor. Measured 2026-09-25 with the
# encoder of phase E4 and the decoder of phase D5.
ACPL_FLOORS = {
    "51-music-96": (
        (17.5, 17.9, 12.6),
        (3.29, 4.57, 4.42, 4.09, 3.68, 3.71, 3.30, 3.79, 3.30, 3.00, 2.53, 2.76, 2.57, 2.42, None),
        (0.368, 0.368, 0.319, 0.391, 0.334, 0.311, 0.313, 0.347, 0.334, 0.323, 0.266, 0.290, 0.256,
         0.260, None),
        None, 4.16, 4.54),
    "51-music-128": (
        (10.5, 11.0, 14.6, 8.3),
        (3.03, 3.94, 4.07, 4.20, 3.54, 3.57, 3.27, 3.52, 3.41, 2.92, 2.43, 2.21, 2.19, 2.08, None),
        (0.380, 0.342, 0.327, 0.418, 0.343, 0.326, 0.300, 0.322, 0.311, 0.300, 0.260, 0.258, 0.218,
         0.226, None),
        None, 3.59, 4.55),
    "51-film-128": (
        (10.9, 11.5, 20.0, 12.2),
        (2.73, 3.70, 4.13, 4.19, 3.70, 3.46, 3.26, 3.46, 3.53, 2.94, 2.70, 2.45, 2.43, 2.21, None),
        (0.412, 0.400, 0.324, 0.380, 0.349, 0.321, 0.319, 0.331, 0.350, 0.326, 0.276, 0.259, 0.229,
         0.222, None),
        None, 3.57, 4.51),
    "51-tones-96": (
        (76.7, 76.7, 69.1),
        None,
        None,
        6.7, 16.05, 4.62),
    "51-tones-128": (
        (79.6, 78.7, 80.2, 69.1),
        None,
        None,
        15.3, 13.03, 4.63),
    "50-music-112": (
        (15.4, 15.7, 20.0),
        (3.06, 3.87, 4.22, 4.20, 3.63, 3.50, 3.17, 3.64, 3.51, 3.02, 2.72, 2.52, 2.33, 2.19, None),
        (0.379, 0.335, 0.325, 0.403, 0.337, 0.316, 0.315, 0.322, 0.320, 0.297, 0.274, 0.281, 0.231,
         0.243, None),
        None, 3.17, 4.52),
    "51-music-160-acpl1": (
        (11.1, 11.7, 15.7, 9.3),
        (1.37, 1.16, 1.79, 1.56, 2.04, 1.88, 2.25, 2.17, 3.09, 2.87, 2.52, 2.18, 2.19, 2.11, None),
        (0.144, 0.139, 0.188, 0.175, 0.198, 0.199, 0.207, 0.217, 0.307, 0.304, 0.261, 0.253, 0.215,
         0.228, None),
        None, 2.62, 4.58),
    "20-music-32-acpl2": (
        (18.7,),
        (2.82, 3.19, 2.94, 3.96, 3.52, 3.49, 2.89, 2.92, 2.87, 2.43, 2.44, 1.68, 1.82, 1.75, None),
        (0.222, 0.156, 0.194, 0.217, 0.215, 0.274, 0.231, 0.240, 0.236, 0.216, 0.220, 0.167, 0.179,
         0.189, None),
        None, 2.96, 4.38),
    "20-music-64-acpl1": (
        (14.4,),
        (1.14, 0.93, 1.33, 1.19, 1.33, 1.52, 1.30, 1.49, 2.55, 2.27, 2.07, 1.74, 1.57, 1.73, None),
        (0.121, 0.085, 0.127, 0.103, 0.130, 0.153, 0.138, 0.155, 0.215, 0.213, 0.207, 0.164, 0.172,
         0.174, None),
        None, 1.81, 4.58),
}

# The race, per G0 leg: the encoder's scores, pinned as FLOORS are. What it measured against DEE's
# streams of the same legs, both decoded by the decoder of phase D4. In the ASPX legs, ViSQOL is
# within 0.03 of DEE's or above it from 64 kbps up (music +0.05 at 64 and +0.04 at 96), and 0.06
# and 0.09 under it on music and speech at 48 kbps. Below the crossover the encoder's SNR is 1.4 to
# 3.6 dB under DEE's at 48 and 64 kbps and on music at 96, whose bits go where ViSQOL marks the
# noise, and 0.6 to 8.2 dB over it on speech at 96 and from 128. The A-SPX tiles land within 0.35
# dB of DEE's distance from the source's energy, or closer (music at 64 kbps: 2.3 dB against 5.7).
# D4's pre-flattening raised both encoders' speech at 48 kbps, DEE's by 0.32 and this one's by
# 0.21, where under D3's this one led by 0.02. In the SIMPLE legs, from 192 kbps, the encoder is
# E1's: 5.6 dB over DEE's SNR on music at 192 kbps, 14.9 on speech and 32 on the tones, its
# log-spectral distance lower on each, and ViSQOL within 0.01 of DEE's 4.70 to 4.73.
# From 256 kbps DEE's audio stops changing and the encoder's goes on improving, to 74 dB on music
# at 768 kbps, where the QMF banks' reconstruction bounds it.
# The 5.1 legs, measured 2026-09-25 with the encoder of phase E3, ASPX to 320 kbps and SIMPLE
# from 384 as DEE's are. Below the crossover the encoder's SNR is 7.3 to 11.6 dB under DEE's at
# 192 kbps and 1.9 to 3.7 under at 256: DEE keeps 21 to 27 dB below 2 kHz and lets the band from
# 8 kHz fall to 3 dB and under, where this encoder spreads its noise across the band. From 288
# kbps it is within 1.6 dB of DEE's, and from 320 over it: 3.2 to 3.8 dB at 384 and 15 to 19 at
# 768. ViSQOL is at or above DEE's at 192 and 256 kbps; from 288 it is up to 0.05 under on film
# and 0.02 on music, and within 0.01 at 768. The log-spectral distance is lower on every leg, the
# A-SPX tiles land within 0.07 dB of DEE's distance from the source's energy or closer, and the
# LFE, which DEE low-passes, comes back 15 to 29 dB over its noise by the rate.
RACE = {
    "20-music-48": ((12.4, 12.6), 2.05, 2.23, 4.44),
    "20-music-64": ((14.9, 15.1), 1.59, 2.75, 4.55),
    "20-music-96": ((20.7, 20.8), 1.27, 0.87, 4.61),
    "20-music-128": ((29.4, 29.4), 0.99, 2.05, 4.62),
    "20-music-144": ((32.4, 32.4), 0.93, 1.81, 4.62),
    "20-music-192": ((39.2, 39.2), 1.54, None, 4.62),
    "20-music-256": ((46.3, 46.4), 1.44, None, 4.62),
    "20-music-288": ((49.4, 49.5), 1.39, None, 4.62),
    "20-music-320": ((52.3, 52.3), 1.33, None, 4.63),
    "20-music-384": ((57.3, 57.4), 1.01, None, 4.63),
    "20-music-448": ((61.5, 61.5), 0.69, None, 4.63),
    "20-music-512": ((65.0, 65.0), 0.59, None, 4.63),
    "20-music-768": ((73.6, 73.4), 0.59, None, 4.63),
    "20-speech-48": ((22.1, 22.1), 1.44, 2.15, 4.36),
    "20-speech-64": ((25.8, 25.8), 1.19, 2.21, 4.39),
    "20-speech-96": ((34.1, 34.1), 0.95, 3.56, 4.48),
    "20-speech-128": ((43.2, 43.2), 0.81, 3.50, 4.49),
    "20-speech-144": ((46.5, 46.5), 0.78, 3.50, 4.49),
    "20-speech-192": ((53.3, 53.3), 0.59, None, 4.62),
    "20-speech-256": ((61.5, 61.5), 0.54, None, 4.63),
    "20-speech-288": ((65.1, 65.1), 0.53, None, 4.63),
    "20-speech-320": ((68.2, 68.2), 0.52, None, 4.63),
    "20-speech-384": ((72.6, 72.6), 0.51, None, 4.63),
    "20-speech-448": ((74.6, 74.6), 0.51, None, 4.63),
    "20-speech-512": ((75.3, 75.3), 0.51, None, 4.63),
    "20-speech-768": ((75.5, 75.5), 0.51, None, 4.63),
    "20-tones-48": ((54.7, 57.6), 11.34, None, 4.63),
    "20-tones-64": ((54.8, 57.8), 11.48, None, 4.63),
    "20-tones-96": ((54.8, 57.9), 11.94, None, 4.63),
    "20-tones-128": ((81.0, 81.7), 8.98, None, 4.63),
    "20-tones-144": ((81.1, 81.9), 8.87, None, 4.63),
    "20-tones-192": ((81.2, 82.0), 8.80, None, 4.63),
    "20-tones-256": ((81.3, 82.2), 8.60, None, 4.63),
    "20-tones-288": ((81.3, 82.2), 8.53, None, 4.63),
    "20-tones-320": ((81.3, 82.2), 8.49, None, 4.63),
    "20-tones-384": ((81.3, 82.2), 8.48, None, 4.63),
    "20-tones-448": ((81.3, 82.2), 8.48, None, 4.63),
    "20-tones-512": ((81.3, 82.2), 8.48, None, 4.63),
    "20-tones-768": ((81.3, 82.2), 8.48, None, 4.63),
    "51-film-192": ((11.3, 11.0, 18.8, 15.2, 10.3, 10.5), 2.73, 2.31, 4.45),
    "51-film-256": ((21.4, 21.4, 31.0, 24.9, 20.3, 20.4), 2.24, 2.28, 4.53),
    "51-film-288": ((25.0, 25.0, 34.9, 26.3, 24.0, 24.1), 2.13, 2.28, 4.53),
    "51-film-320": ((27.9, 28.0, 37.9, 27.0, 27.0, 27.1), 2.06, 2.30, 4.53),
    "51-film-384": ((32.4, 32.5, 42.5, 27.4, 31.4, 31.6), 2.58, None, 4.57),
    "51-film-448": ((36.2, 36.3, 46.4, 27.5, 35.3, 35.4), 2.53, None, 4.58),
    "51-film-512": ((39.4, 39.5, 49.7, 27.6, 38.6, 38.7), 2.49, None, 4.60),
    "51-film-768": ((49.6, 49.7, 60.0, 27.6, 48.8, 48.9), 2.33, None, 4.62),
    "51-music-192": ((13.1, 13.1, 14.6, 13.9, 10.1, 10.0), 2.73, 3.22, 4.58),
    "51-music-256": ((24.3, 24.4, 26.6, 23.9, 20.9, 21.0), 2.21, 4.10, 4.60),
    "51-music-288": ((27.9, 27.9, 30.3, 25.7, 24.4, 24.6), 2.10, 4.11, 4.61),
    "51-music-320": ((30.8, 30.9, 33.2, 26.6, 27.4, 27.5), 2.04, 4.10, 4.61),
    "51-music-384": ((35.6, 35.6, 38.0, 27.3, 32.0, 32.1), 2.64, None, 4.61),
    "51-music-448": ((39.4, 39.5, 41.9, 27.5, 35.9, 35.9), 2.59, None, 4.60),
    "51-music-512": ((42.7, 42.8, 45.2, 27.6, 39.1, 39.2), 2.55, None, 4.62),
    "51-music-768": ((53.1, 53.2, 55.6, 27.6, 49.6, 49.6), 2.38, None, 4.63),
    "51-tones-192": ((80.2, 80.7, 80.0, 69.0, 81.3, 78.8), 10.67, None, 4.63),
    "51-tones-256": ((80.8, 81.5, 80.4, 69.0, 81.9, 79.2), 10.57, None, 4.63),
    "51-tones-288": ((81.0, 81.7, 80.5, 69.0, 82.0, 79.3), 10.51, None, 4.63),
    "51-tones-320": ((81.1, 81.9, 80.6, 69.0, 82.1, 79.4), 10.44, None, 4.63),
    "51-tones-384": ((81.1, 82.0, 80.6, 69.0, 82.1, 79.3), 10.42, None, 4.63),
    "51-tones-448": ((81.2, 82.1, 80.6, 69.0, 82.2, 79.4), 10.34, None, 4.63),
    "51-tones-512": ((81.2, 82.1, 80.6, 69.0, 82.2, 79.4), 10.30, None, 4.63),
    "51-tones-768": ((81.3, 82.2, 80.7, 69.0, 82.3, 79.5), 10.17, None, 4.63),
}
# The race's A-CPL legs, 5.1 at 96, 128 and 144 kbps, pinned as ACPL_FLOORS are. Measured 2026-09-25
# with the encoder of phase E4, both streams decoded by the decoder of phase D5. Against DEE's, the
# per-band level difference lands 0.02 to 0.13 dB nearer the source's on music and film, the
# correlation within 0.007 of DEE's distance, and the log-spectral distance is lower on every leg;
# ViSQOL is 0.01 to 0.06 above DEE's, except on film at 128 kbps, 0.12 under, where the coded C's
# band below 2 kHz trails DEE's by 9.5 dB of SNR. On the tones the routing margin is 1.2 dB (at 96)
# and 2.9 dB over DEE's. The coded downmixes' SNR trails DEE's by 3.4 to 9.5 dB, the E3 rate loop's
# spread of noise across the band. Phase E6 gave every presentation a presentation_id, as DEE's
# streams and CMAF have it: three bits of the table of contents that take a byte of the audio from
# each frame at 128 kbps, which moved film's coded C from 21.4 to 20.2 dB and its other downmixes by
# 0.1 to 0.4 dB, ViSQOL unchanged; its C floor is pinned again from there.
RACE_ACPL = {
    "51-music-96": (
        (17.7, 17.7, 21.9),
        (3.25, 3.53, 3.64, 3.56, 3.43, 3.46, 3.24, 3.59, 3.42, 3.01, 2.61, 2.80, 2.51, 2.55, None),
        (0.351, 0.421, 0.350, 0.423, 0.354, 0.353, 0.330, 0.341, 0.348, 0.333, 0.285, 0.335, 0.324,
         0.310, None),
        None, 4.50, 4.52),
    "51-music-128": (
        (10.8, 10.8, 15.3, 15.2),
        (3.51, 3.20, 3.63, 3.54, 3.57, 3.58, 3.34, 3.37, 3.29, 2.71, 2.46, 2.37, 2.14, 2.29, None),
        (0.319, 0.355, 0.366, 0.457, 0.347, 0.331, 0.327, 0.320, 0.311, 0.293, 0.241, 0.242, 0.215,
         0.232, None),
        None, 3.85, 4.55),
    "51-music-144": (
        (16.2, 16.3, 22.0, 20.8),
        (3.59, 3.09, 3.61, 3.55, 3.48, 3.49, 3.32, 3.38, 3.27, 2.68, 2.45, 2.38, 2.10, 2.26, None),
        (0.315, 0.360, 0.375, 0.455, 0.347, 0.331, 0.325, 0.322, 0.310, 0.292, 0.238, 0.239, 0.215,
         0.227, None),
        None, 3.71, 4.56),
    "51-film-96": (
        (19.4, 19.4, 23.2),
        (3.54, 3.56, 3.70, 3.78, 3.77, 3.54, 3.32, 3.92, 4.01, 3.50, 3.31, 3.48, 3.71, 2.91, None),
        (0.362, 0.387, 0.352, 0.440, 0.357, 0.331, 0.357, 0.435, 0.440, 0.442, 0.444, 0.450, 0.527,
         0.368, None),
        None, 5.15, 4.43),
    "51-film-128": (
        (11.2, 11.3, 19.2, 17.3),
        (3.60, 3.40, 3.58, 3.73, 3.68, 3.61, 3.32, 3.37, 3.28, 2.91, 2.61, 2.38, 2.23, 2.31, None),
        (0.314, 0.317, 0.328, 0.431, 0.342, 0.307, 0.329, 0.320, 0.305, 0.298, 0.254, 0.247, 0.235,
         0.230, None),
        None, 3.85, 4.38),
    "51-film-144": (
        (12.1, 12.2, 24.2, 20.4),
        (3.57, 3.37, 3.62, 3.70, 3.69, 3.59, 3.31, 3.37, 3.28, 2.82, 2.48, 2.32, 2.05, 2.27, None),
        (0.306, 0.315, 0.321, 0.433, 0.344, 0.310, 0.326, 0.320, 0.302, 0.286, 0.246, 0.239, 0.220,
         0.226, None),
        None, 3.79, 4.52),
    "51-tones-96": (
        (76.9, 76.8, 69.0),
        None,
        None,
        6.7, 15.92, 4.62),
    "51-tones-128": (
        (79.9, 79.1, 80.3, 69.0),
        None,
        None,
        15.3, 12.87, 4.63),
    "51-tones-144": (
        (80.3, 79.6, 80.4, 69.0),
        None,
        None,
        15.3, 12.78, 4.63),
}
# The G0 legs the race runs, by rate: 2.0 in ASPX where DEE writes it and in SIMPLE from 192 kbps,
# and 5.1 from 96 kbps, in A-CPL below 192.
RACE_RATES = (48, 64, 96, 128, 144, 192, 256, 288, 320, 384, 448, 512, 768)
# The codec modes a 5_X_codec_mode or stereo_codec_mode names, where A-CPL is on (Part 1 Tables 95
# and 97).
ACPL_MODES = {2: "ASPX_ACPL_1", 3: "ASPX_ACPL_2", 4: "ASPX_ACPL_3"}

# The immersive legs (phase E8), 5.1.4: name: (source, kbps, *ac4-encode's options). The encoder
# takes ASPX_ACPL_2 below 480 kbps, ASPX_SCPL below 640 and SCPL from there, and ASPX_ACPL_1 and
# ASPX_AJCC by name, experimental. score_ac4_decode.py's score_immersive() scores them, in full
# and in core decoding, against IMMERSIVE_FLOORS, in its IMMERSIVE_PINS form; in full decoding the
# A-SPX tiles of every channel but the LFE above the frame's lowest crossover, and in the
# parametric modes the level difference and correlation per A-CPL parameter band of the pairs
# IMMERSIVE_BAND_PAIRS names, are held to IMMERSIVE_EXTRA as E2 and E4 hold theirs.
IMMERSIVE_LEGS = {
    "514-tones-256": ("tones_514", 256),
    "514-tones-512": ("tones_514", 512),
    "514-tones-768": ("tones_514", 768),
    "514-tones-448-acpl1": ("tones_514", 448, "codec-mode=aspx-acpl-1", "experimental=acpl"),
    "514-music-192": ("music_514", 192),
    "514-music-384": ("music_514", 384),
    "514-music-512": ("music_514", 512),
    "514-music-768": ("music_514", 768),
    "514-music-448-acpl1": ("music_514", 448, "codec-mode=aspx-acpl-1", "experimental=acpl"),
    "514-tones-256-ajcc": ("tones_514", 256, "codec-mode=aspx-ajcc", "experimental=ajcc"),
    "514-music-192-ajcc": ("music_514", 192, "codec-mode=aspx-ajcc", "experimental=ajcc"),
    "514-music-384-ajcc": ("music_514", 384, "codec-mode=aspx-ajcc", "experimental=ajcc"),
}
# The immersive legs' pins, by (leg, "full" or "core"), in score_ac4_decode.py's IMMERSIVE_PINS
# form: (SNR floors per scored signal, the routing floor on a tone leg, the LSD ceiling, the MOS
# floor). Measured 2026-09-26 with the encoder of phase E8 and the decoder of phase D9.
IMMERSIVE_FLOORS = {
    ("514-tones-256", "full"): ((80.3, 80.3, 79.8, 69.1, 80.4, 78.3, 74.3, 75.2), 61.3, 10.48,
                                4.63),
    ("514-tones-256", "core"): ((80.3, 80.3, 79.8, 69.1, 80.4, 78.3, 74.3, 75.2), 61.3, 10.48,
                                4.63),
    ("514-tones-512", "full"): ((81.2, 81.5, 80.4, 69.1, 81.7, 79.1, 79.9, 80.7, 80.6, 78.5),
                                61.3, 10.72, 4.63),
    ("514-tones-512", "core"): ((81.2, 81.5, 80.4, 69.1, 81.6, 79.1, 74.6, 75.7), 61.3, 10.79,
                                4.63),
    ("514-tones-768", "full"): ((81.5, 82.0, 80.5, 69.1, 82.1, 79.4, 80.2, 81.0, 80.9, 78.7),
                                61.3, 9.75, 4.63),
    ("514-tones-768", "core"): ((81.5, 82.0, 80.5, 69.1, 82.1, 79.4, 74.7, 75.7), 61.3, 10.17,
                                4.63),
    ("514-tones-448-acpl1", "full"): ((80.9, 81.1, 80.2, 69.1, 81.2, 78.9, 80.0, 79.9, 80.1,
                                      78.4), 61.3, 10.22, 4.63),
    ("514-tones-448-acpl1", "core"): ((80.9, 81.1, 80.2, 69.1, 81.2, 78.9, 74.5, 75.5), 61.3,
                                      10.34, 4.63),
    ("514-music-192", "full"): ((15.4, 16.2, 18.2, 12.3, 16.0, 16.4, 13.1, 13.5), None, 2.23, 4.41),
    ("514-music-192", "core"): ((15.4, 16.2, 18.2, 12.3, 16.0, 16.4, 13.1, 13.5), None, 2.23, 4.41),
    ("514-music-384", "full"): ((25.5, 26.1, 28.8, 15.9, 26.5, 26.8, 23.1, 23.6), None, 1.59, 4.60),
    ("514-music-384", "core"): ((25.5, 26.1, 28.8, 15.9, 26.5, 26.8, 23.1, 23.6), None, 1.59, 4.60),
    ("514-music-512", "full"): ((27.4, 28.2, 30.6, 16.2, 28.4, 28.8, 23.8, 24.1, 24.8, 25.2),
                                None, 1.52, 4.61),
    ("514-music-512", "core"): ((27.4, 28.2, 30.6, 16.2, 28.4, 28.8, 24.9, 25.3), None, 1.60, 4.60),
    ("514-music-768", "full"): ((38.0, 38.8, 41.5, 16.5, 39.2, 39.5, 34.5, 34.8, 35.3, 35.7),
                                None, 1.96, 4.62),
    ("514-music-768", "core"): ((38.0, 38.8, 41.5, 16.5, 39.2, 39.5, 35.5, 36.0), None, 2.05, 4.61),
    ("514-music-448-acpl1", "full"): ((23.3, 24.0, 26.5, 15.6, 24.3, 24.6, 16.4, 16.9, 20.5,
                                      20.9), None, 1.75, 4.61),
    ("514-music-448-acpl1", "core"): ((23.3, 24.0, 26.5, 15.6, 24.2, 24.5, 20.8, 21.3), None,
                                      1.66, 4.60),
    # ASPX_AJCC, experimental. The channels A-JCC makes are part decorrelated, whose waveform
    # SNR is pinned as it comes.
    ("514-tones-256-ajcc", "full"): ((75.4, 74.1, 80.3, 69.1, 73.2, 44.5, 72.3, 71.3, 73.0, 47.2),
                                     42.5, 10.90, 4.63),
    ("514-tones-256-ajcc", "core"): ((75.4, 74.1, 80.3, 69.1, 73.2, 47.5, 71.0, 50.1), 45.5,
                                     11.08, 4.63),
    ("514-music-192-ajcc", "full"): ((5.7, 5.7, 20.5, 14.3, 2.5, 2.1, -8.7, -9.6, -7.1, -7.2),
                                     None, 3.34, 4.50),
    ("514-music-192-ajcc", "core"): ((5.7, 5.7, 20.5, 14.3, 3.3, 3.0, -8.1, -9.5), None, 3.13,
                                     4.42),
    ("514-music-384-ajcc", "full"): ((5.9, 5.9, 35.7, 16.5, 2.6, 2.2, -8.6, -9.4, -7.1, -7.1),
                                     None, 3.17, 4.61),
    ("514-music-384-ajcc", "core"): ((5.9, 5.9, 35.7, 16.5, 3.4, 3.1, -8.0, -9.4), None, 2.94,
                                     4.58),
}
# By leg: (the A-SPX tile ceiling, or None in SCPL; the ceilings per A-CPL parameter band of the
# pairs' level difference and correlation distances, or None where the leg is not in a parametric
# mode or is a tone leg), the first measurement plus TILE_MARGIN_DB, decoding.ACPL_ILD_MARGIN_DB
# and decoding.ACPL_RHO_MARGIN.
IMMERSIVE_EXTRA = {
    "514-tones-256": (None, None, None),
    "514-tones-512": (None, None, None),
    "514-tones-768": (None, None, None),
    "514-tones-448-acpl1": (None, None, None),
    "514-music-192": (
        2.21,
        (3.09, 3.74, 4.01, 4.22, 3.45, 3.49, 3.26, 3.70, 3.25, 2.92, 2.59, 2.72, 2.52, 2.39, None),
        (0.389, 0.339, 0.312, 0.400, 0.333, 0.326, 0.309, 0.370, 0.333, 0.311, 0.282, 0.264, 0.265,
         0.255, None)),
    "514-music-384": (
        None,
        (3.09, 3.79, 4.07, 4.25, 3.56, 3.40, 3.16, 3.71, 3.18, 2.83, 2.35, 2.60, 2.27, 2.33, None),
        (0.386, 0.337, 0.321, 0.391, 0.334, 0.335, 0.301, 0.358, 0.329, 0.284, 0.273, 0.245, 0.237,
         0.248, None)),
    "514-music-512": (None, None, None),
    "514-music-768": (None, None, None),
    "514-music-448-acpl1": (
        None,
        (0.78, 0.73, 1.09, 0.80, 1.03, 1.13, 1.60, 1.68, 3.04, 2.80, 2.41, 2.60, 2.27, 2.33, None),
        (0.083, 0.074, 0.108, 0.080, 0.110, 0.121, 0.154, 0.169, 0.336, 0.301, 0.271, 0.246, 0.237,
         0.248, None)),
    "514-tones-256-ajcc": (None, None, None),
    "514-music-192-ajcc": (
        2.64,
        (3.40, 3.32, 3.27, 4.14, 3.41, 3.90, 3.12, 3.51, 3.41, 3.19, 2.47, 2.61, 2.22, 2.46, None),
        (0.352, 0.426, 0.336, 0.412, 0.356, 0.383, 0.311, 0.355, 0.335, 0.281, 0.257, 0.246, 0.227,
         0.230, None)),
    "514-music-384-ajcc": (
        None,
        (3.36, 3.36, 3.29, 4.14, 3.40, 3.91, 3.08, 3.46, 3.47, 3.22, 2.44, 2.60, 2.16, 2.41, None),
        (0.347, 0.423, 0.339, 0.409, 0.362, 0.380, 0.307, 0.362, 0.355, 0.285, 0.247, 0.242, 0.223,
         0.229, None)),
}
# The race's 5.1.4 legs: G0's music and tones and G1's film, speech, sweeps and transients (the
# gold manifest's g1_legs), at each of DEE's 5.1.4 rates, in IMMERSIVE_FLOORS' and
# IMMERSIVE_EXTRA's forms. Measured 2026-09-26 with the encoder of phase E8 and the decoder of
# phase D9, both encoders' streams in the codec mode DEE's take at the rate, ASPX_ACPL_2 to 448
# kbps, ASPX_SCPL at 512 and SCPL at 768. On music, film and speech the encoder's SNR below the
# lowest crossover is 0.1 to 10.5 dB under DEE's from 192 to 320 kbps (most at 256, where the
# waveform codes to 10.5 kHz), within 1.4 dB of it or over it at 384 and 512, and 0.6 to 14 dB
# over it at 448 and 768: as E3 found in 5.1, DEE keeps its SNR high at low frequencies and lets
# the top of the band go, where this encoder spreads its noise by its masking thresholds. ViSQOL
# is up to 0.08 over DEE's from 192 to 384 kbps and at most 0.035 under it on any of these legs,
# in core decoding as in full (the two within 0.02 of each other). The log-spectral distance is
# lower on all but six of the 24 legs (0.07 to 0.31 dB higher on those), the A-SPX tiles land
# within 0.2 dB of DEE's distance from the source's energy or closer, and A-CPL's top pairs 0.06
# to 0.16 dB closer to the source's level difference than DEE's and 0.04 to 0.06 closer in
# correlation. The tones come back 25 to 50 dB over DEE's SNR, the transients 8 to 12 dB. The
# sweeps' waveform is 31 to 40 dB over DEE's, but above 16.5 kHz, where A-SPX's patch holds
# nothing to copy while the sweep is up there, this encoder's A-SPX leaves the band some 50 dB
# under the sweep's energy where DEE's comes within about 10 dB of it: the tiles land 4 to 14 dB
# further from the source's energy than DEE's, and ViSQOL is 0.03 to 0.18 under DEE's from 256 to
# 512 kbps (E2's A-SPX encoder, which every layout shares; G1's 5.1 sweeps show the tiles' gap
# too, 27.7 dB against DEE's 16.7 at 192 kbps).
RACE_IMMERSIVE_CONTENT = ("music", "tones", "film", "speech", "sweep", "transient")
RACE_IMMERSIVE = {
    ("514-music-192", "full"): ((17.3, 17.3, 18.9, 21.6, 12.3, 12.4, 13.3, 13.5), None, 2.53, 4.40),
    ("514-music-192", "core"): ((17.3, 17.3, 18.9, 21.6, 12.3, 12.4, 13.3, 13.5), None, 2.53, 4.43),
    ("514-music-256", "full"): ((12.8, 12.7, 14.8, 17.3, 8.9, 8.9, 9.5, 9.8), None, 2.53, 4.59),
    ("514-music-256", "core"): ((12.8, 12.7, 14.8, 17.3, 8.9, 8.9, 9.5, 9.8), None, 2.53, 4.59),
    ("514-music-288", "full"): ((19.0, 19.0, 21.3, 22.9, 13.8, 13.8, 14.9, 15.3), None, 2.23, 4.60),
    ("514-music-288", "core"): ((19.0, 19.0, 21.3, 22.9, 13.8, 13.8, 14.9, 15.3), None, 2.23, 4.61),
    ("514-music-320", "full"): ((21.7, 21.8, 24.0, 24.7, 16.4, 16.4, 17.6, 17.9), None, 2.10, 4.60),
    ("514-music-320", "core"): ((21.7, 21.8, 24.0, 24.7, 16.4, 16.4, 17.6, 17.9), None, 2.10, 4.61),
    ("514-music-384", "full"): ((27.8, 27.8, 30.1, 26.8, 22.3, 22.4, 23.6, 23.8), None, 1.90, 4.60),
    ("514-music-384", "core"): ((27.8, 27.8, 30.1, 26.8, 22.3, 22.4, 23.6, 23.8), None, 1.90, 4.61),
    ("514-music-448", "full"): ((32.1, 32.1, 34.4, 27.3, 26.5, 26.6, 27.8, 28.0), None, 1.81, 4.61),
    ("514-music-448", "core"): ((32.1, 32.1, 34.4, 27.3, 26.5, 26.6, 27.8, 28.0), None, 1.81, 4.61),
    ("514-music-512", "full"): ((29.8, 29.8, 32.0, 27.1, 24.2, 24.3, 25.6, 25.9, 22.6, 23.1),
                                None, 1.77, 4.60),
    ("514-music-512", "core"): ((29.8, 29.8, 32.0, 27.1, 24.2, 24.3, 25.4, 25.6), None, 1.90, 4.61),
    ("514-music-768", "full"): ((40.4, 40.4, 42.7, 27.6, 34.6, 34.7, 36.0, 36.3, 33.1, 33.5),
                                None, 2.25, 4.62),
    ("514-music-768", "core"): ((40.4, 40.4, 42.7, 27.6, 34.6, 34.7, 35.8, 36.1), None, 2.40, 4.62),
    ("514-tones-192", "full"): ((78.7, 78.8, 78.9, 69.0, 78.6, 76.9, 73.9, 74.5), 67.5, 10.49,
                                4.63),
    ("514-tones-192", "core"): ((78.7, 78.8, 78.9, 69.0, 78.6, 76.9, 73.9, 74.5), 67.5, 10.48,
                                4.63),
    ("514-tones-256", "full"): ((80.2, 80.6, 80.0, 69.0, 80.6, 78.4, 74.4, 75.2), 67.5, 10.37,
                                4.63),
    ("514-tones-256", "core"): ((80.2, 80.6, 80.0, 69.0, 80.6, 78.4, 74.4, 75.2), 67.5, 10.37,
                                4.63),
    ("514-tones-288", "full"): ((80.5, 81.1, 80.2, 69.0, 81.1, 78.7, 74.5, 75.4), 67.5, 10.31,
                                4.63),
    ("514-tones-288", "core"): ((80.5, 81.1, 80.2, 69.0, 81.1, 78.7, 74.5, 75.4), 67.5, 10.31,
                                4.63),
    ("514-tones-320", "full"): ((80.7, 81.4, 80.3, 69.0, 81.4, 78.9, 74.6, 75.4), 67.5, 10.24,
                                4.63),
    ("514-tones-320", "core"): ((80.7, 81.4, 80.3, 69.0, 81.4, 78.9, 74.6, 75.4), 67.5, 10.24,
                                4.63),
    ("514-tones-384", "full"): ((81.0, 81.7, 80.5, 69.0, 81.8, 79.2, 74.7, 75.5), 67.5, 10.15,
                                4.63),
    ("514-tones-384", "core"): ((81.0, 81.7, 80.5, 69.0, 81.8, 79.2, 74.7, 75.5), 67.5, 10.15,
                                4.63),
    ("514-tones-448", "full"): ((81.1, 82.0, 80.6, 69.0, 82.0, 79.3, 74.7, 75.6), 67.5, 10.05,
                                4.63),
    ("514-tones-448", "core"): ((81.1, 82.0, 80.6, 69.0, 82.0, 79.3, 74.7, 75.6), 67.5, 10.05,
                                4.63),
    ("514-tones-512", "full"): ((80.9, 81.7, 80.5, 69.0, 81.8, 79.2, 80.0, 80.7, 80.6, 78.6),
                                67.5, 10.63, 4.63),
    ("514-tones-512", "core"): ((80.9, 81.7, 80.5, 69.0, 81.7, 79.1, 74.7, 75.6), 67.5, 10.70,
                                4.63),
    ("514-tones-768", "full"): ((81.2, 82.1, 80.6, 69.0, 82.2, 79.4, 80.2, 80.9, 80.9, 78.8),
                                67.5, 9.67, 4.63),
    ("514-tones-768", "core"): ((81.2, 82.1, 80.6, 69.0, 82.1, 79.4, 74.7, 75.6), 67.5, 10.09,
                                4.63),
    ("514-film-192", "full"): ((14.9, 15.0, 22.5, 23.1, 12.0, 12.1, 13.5, 13.8), None, 2.57, 4.38),
    ("514-film-192", "core"): ((14.9, 15.0, 22.5, 23.1, 12.0, 12.1, 13.5, 13.8), None, 2.57, 4.34),
    ("514-film-256", "full"): ((11.2, 11.1, 18.8, 19.0, 8.9, 8.9, 9.8, 10.1), None, 2.55, 4.54),
    ("514-film-256", "core"): ((11.2, 11.1, 18.8, 19.0, 8.9, 8.9, 9.8, 10.1), None, 2.55, 4.54),
    ("514-film-288", "full"): ((16.8, 16.8, 26.4, 24.5, 13.8, 13.8, 15.6, 15.8), None, 2.24, 4.56),
    ("514-film-288", "core"): ((16.8, 16.8, 26.4, 24.5, 13.8, 13.8, 15.6, 15.8), None, 2.24, 4.55),
    ("514-film-320", "full"): ((18.8, 18.9, 28.6, 25.5, 15.8, 15.9, 17.6, 17.9), None, 2.15, 4.58),
    ("514-film-320", "core"): ((18.8, 18.9, 28.6, 25.5, 15.8, 15.9, 17.6, 17.9), None, 2.15, 4.58),
    ("514-film-384", "full"): ((24.8, 24.8, 34.8, 27.1, 21.8, 21.9, 23.7, 23.9), None, 1.94, 4.58),
    ("514-film-384", "core"): ((24.8, 24.8, 34.8, 27.1, 21.8, 21.9, 23.7, 23.9), None, 1.94, 4.58),
    ("514-film-448", "full"): ((29.2, 29.2, 39.3, 27.4, 26.2, 26.3, 28.0, 28.3), None, 1.83, 4.59),
    ("514-film-448", "core"): ((29.2, 29.2, 39.3, 27.4, 26.2, 26.3, 28.0, 28.3), None, 1.83, 4.58),
    ("514-film-512", "full"): ((26.8, 26.7, 36.8, 27.3, 23.7, 23.8, 25.7, 25.9, 23.2, 23.5), None,
                               1.80, 4.58),
    ("514-film-512", "core"): ((26.8, 26.7, 36.8, 27.3, 23.7, 23.8, 25.4, 25.6), None, 1.94, 4.58),
    ("514-film-768", "full"): ((37.1, 37.1, 47.3, 27.6, 34.0, 34.1, 36.0, 36.2, 33.4, 33.8), None,
                               2.24, 4.61),
    ("514-film-768", "core"): ((37.1, 37.1, 47.3, 27.6, 34.0, 34.1, 35.7, 36.0), None, 2.37, 4.60),
    ("514-speech-192", "full"): ((14.9, 14.9, 19.6, None, 7.7, 7.7, 7.8, 8.2), None, 1.95, 4.24),
    ("514-speech-192", "core"): ((14.9, 14.9, 19.6, None, 7.7, 7.7, 7.8, 8.2), None, 1.95, 4.23),
    ("514-speech-256", "full"): ((21.4, 21.4, 27.2, None, 11.3, 11.3, 11.8, 12.2), None, 1.56,
                                 4.40),
    ("514-speech-256", "core"): ((21.4, 21.4, 27.2, None, 11.3, 11.3, 11.8, 12.2), None, 1.56,
                                 4.40),
    ("514-speech-288", "full"): ((26.4, 26.4, 32.6, None, 15.5, 15.6, 16.1, 16.3), None, 1.32,
                                 4.40),
    ("514-speech-288", "core"): ((26.4, 26.4, 32.6, None, 15.5, 15.6, 16.1, 16.3), None, 1.32,
                                 4.40),
    ("514-speech-320", "full"): ((28.2, 28.2, 34.5, None, 17.1, 17.2, 17.7, 17.9), None, 1.31,
                                 4.51),
    ("514-speech-320", "core"): ((28.2, 28.2, 34.5, None, 17.1, 17.2, 17.7, 17.9), None, 1.32,
                                 4.50),
    ("514-speech-384", "full"): ((33.8, 33.8, 40.4, None, 22.6, 22.7, 23.1, 23.3), None, 1.12,
                                 4.50),
    ("514-speech-384", "core"): ((33.8, 33.8, 40.4, None, 22.6, 22.7, 23.1, 23.3), None, 1.12,
                                 4.50),
    ("514-speech-448", "full"): ((38.2, 38.2, 44.9, None, 26.8, 26.9, 27.2, 27.4), None, 1.02,
                                 4.49),
    ("514-speech-448", "core"): ((38.2, 38.2, 44.9, None, 26.8, 26.9, 27.2, 27.4), None, 1.02,
                                 4.49),
    ("514-speech-512", "full"): ((36.2, 36.2, 42.8, None, 24.7, 24.8, 25.3, 25.5, 22.4, 22.8),
                                 None, 1.27, 4.53),
    ("514-speech-512", "core"): ((36.2, 36.2, 42.8, None, 24.7, 24.8, 24.9, 25.2), None, 1.16,
                                 4.49),
    ("514-speech-768", "full"): ((46.3, 46.3, 53.0, None, 34.6, 34.7, 35.2, 35.3, 32.1, 32.5),
                                 None, 1.62, 4.61),
    ("514-speech-768", "core"): ((46.3, 46.3, 53.0, None, 34.6, 34.7, 34.8, 35.0), None, 1.45,
                                 4.61),
    ("514-sweep-192", "full"): ((74.5, 74.3, 74.6, 39.4, 73.6, 73.7, 73.8, 73.8), None, 10.53,
                                3.41),
    ("514-sweep-192", "core"): ((74.5, 74.3, 74.6, 39.4, 73.6, 73.7, 73.8, 73.8), None, 10.53,
                                3.38),
    ("514-sweep-256", "full"): ((75.6, 75.3, 75.5, 39.4, 75.0, 75.0, 75.0, 75.2), None, 10.27,
                                4.02),
    ("514-sweep-256", "core"): ((75.6, 75.3, 75.5, 39.4, 75.0, 75.0, 75.0, 75.2), None, 10.27,
                                4.01),
    ("514-sweep-288", "full"): ((75.9, 75.6, 75.8, 39.4, 75.4, 75.4, 75.4, 75.6), None, 10.18,
                                4.01),
    ("514-sweep-288", "core"): ((75.9, 75.6, 75.8, 39.4, 75.4, 75.4, 75.4, 75.6), None, 10.18,
                                4.00),
    ("514-sweep-320", "full"): ((75.9, 75.7, 75.9, 39.4, 75.5, 75.5, 75.5, 75.7), None, 10.28,
                                4.33),
    ("514-sweep-320", "core"): ((75.9, 75.7, 75.9, 39.4, 75.5, 75.5, 75.5, 75.7), None, 10.28,
                                4.33),
    ("514-sweep-384", "full"): ((76.2, 75.9, 76.1, 39.4, 75.8, 75.8, 75.8, 76.0), None, 10.14,
                                4.37),
    ("514-sweep-384", "core"): ((76.2, 75.9, 76.1, 39.4, 75.8, 75.8, 75.8, 76.0), None, 10.14,
                                4.36),
    ("514-sweep-448", "full"): ((76.3, 76.0, 76.2, 39.4, 75.9, 76.0, 75.9, 76.2), None, 10.03,
                                4.41),
    ("514-sweep-448", "core"): ((76.3, 76.0, 76.2, 39.4, 75.9, 76.0, 75.9, 76.2), None, 10.03,
                                4.40),
    ("514-sweep-512", "full"): ((76.2, 75.9, 76.0, 39.4, 75.7, 75.8, 75.9, 76.0, 75.7, 76.2),
                                None, 11.80, 4.18),
    ("514-sweep-512", "core"): ((76.2, 75.9, 76.0, 39.4, 75.7, 75.8, 75.8, 76.1), None, 10.53,
                                4.34),
    ("514-sweep-768", "full"): ((76.3, 76.0, 76.2, 39.4, 76.0, 76.1, 76.1, 76.2, 75.9, 76.4),
                                None, 11.45, 4.42),
    ("514-sweep-768", "core"): ((76.3, 76.0, 76.2, 39.4, 76.0, 76.1, 76.0, 76.3), None, 10.38,
                                4.42),
    ("514-transient-192", "full"): ((49.4, 51.6, 50.1, 26.5, 51.2, 52.0, 49.2, 52.9), None, 2.23,
                                    4.56),
    ("514-transient-192", "core"): ((49.4, 51.6, 50.1, 26.5, 51.2, 52.0, 49.2, 52.9), None, 2.23,
                                    4.56),
    ("514-transient-256", "full"): ((41.9, 42.0, 41.5, 26.5, 41.6, 42.2, 41.4, 42.4), None, 2.03,
                                    4.58),
    ("514-transient-256", "core"): ((41.9, 42.0, 41.5, 26.5, 41.6, 42.2, 41.4, 42.4), None, 2.03,
                                    4.58),
    ("514-transient-288", "full"): ((42.2, 42.3, 41.8, 26.5, 41.8, 42.4, 41.7, 42.7), None, 2.01,
                                    4.58),
    ("514-transient-288", "core"): ((42.2, 42.3, 41.8, 26.5, 41.8, 42.4, 41.7, 42.7), None, 2.01,
                                    4.58),
    ("514-transient-320", "full"): ((41.5, 40.6, 41.5, 26.5, 41.7, 40.5, 41.5, 40.5), None, 2.01,
                                    4.59),
    ("514-transient-320", "core"): ((41.5, 40.6, 41.5, 26.5, 41.7, 40.5, 41.5, 40.5), None, 2.01,
                                    4.58),
    ("514-transient-384", "full"): ((41.7, 40.7, 41.8, 26.5, 41.9, 40.6, 41.7, 40.7), None, 1.99,
                                    4.59),
    ("514-transient-384", "core"): ((41.7, 40.7, 41.8, 26.5, 41.9, 40.6, 41.7, 40.7), None, 1.99,
                                    4.58),
    ("514-transient-448", "full"): ((41.8, 40.7, 41.8, 26.5, 41.9, 40.6, 41.8, 40.7), None, 1.98,
                                    4.59),
    ("514-transient-448", "core"): ((41.8, 40.7, 41.8, 26.5, 41.9, 40.6, 41.8, 40.7), None, 1.98,
                                    4.58),
    ("514-transient-512", "full"): ((41.8, 40.7, 41.8, 26.5, 41.9, 40.6, 41.7, 40.7, 41.9, 40.7),
                                    None, 1.71, 4.58),
    ("514-transient-512", "core"): ((41.8, 40.7, 41.8, 26.5, 41.9, 40.6, 41.8, 40.7), None, 2.54,
                                    4.27),
    ("514-transient-768", "full"): ((67.2, 75.0, 72.8, 26.5, 74.9, 74.3, 65.6, 75.6, 72.9, 74.5),
                                    None, 1.73, 4.32),
    ("514-transient-768", "core"): ((67.2, 75.0, 72.8, 26.5, 74.9, 74.3, 67.8, 75.1), None, 2.00,
                                    4.32),
}
RACE_IMMERSIVE_EXTRA = {
    "514-music-192": (
        2.21,
        (3.54, 3.36, 3.31, 3.34, 3.41, 3.23, 3.22, 3.34, 3.32, 3.03, 2.79, 2.53, 2.40, 2.48, None),
        (0.351, 0.343, 0.322, 0.430, 0.365, 0.319, 0.348, 0.332, 0.329, 0.315, 0.280, 0.253, 0.256,
         0.254, None)),
    "514-music-256": (
        2.47,
        (3.59, 3.35, 3.33, 3.35, 3.43, 3.22, 3.22, 3.34, 3.26, 2.97, 2.60, 2.46, 2.28, 2.30, None),
        (0.351, 0.342, 0.327, 0.427, 0.359, 0.325, 0.325, 0.330, 0.328, 0.310, 0.256, 0.246, 0.228,
         0.246, None)),
    "514-music-288": (
        2.48,
        (3.52, 3.33, 3.27, 3.36, 3.45, 3.21, 3.22, 3.33, 3.23, 2.95, 2.59, 2.45, 2.24, 2.27, None),
        (0.353, 0.338, 0.327, 0.428, 0.359, 0.323, 0.334, 0.332, 0.330, 0.308, 0.255, 0.249, 0.228,
         0.239, None)),
    "514-music-320": (
        4.22,
        (3.53, 3.33, 3.25, 3.37, 3.40, 3.21, 3.13, 3.32, 3.17, 2.94, 2.60, 2.44, 2.24, 2.26, None),
        (0.353, 0.336, 0.325, 0.434, 0.363, 0.322, 0.324, 0.334, 0.323, 0.305, 0.256, 0.250, 0.228,
         0.239, None)),
    "514-music-384": (
        4.21,
        (3.53, 3.34, 3.29, 3.36, 3.42, 3.20, 3.17, 3.35, 3.23, 2.88, 2.57, 2.46, 2.24, 2.26, None),
        (0.351, 0.337, 0.322, 0.437, 0.363, 0.315, 0.326, 0.340, 0.327, 0.299, 0.253, 0.247, 0.228,
         0.239, None)),
    "514-music-448": (
        4.14,
        (3.53, 3.34, 3.29, 3.35, 3.42, 3.21, 3.20, 3.30, 3.23, 2.88, 2.54, 2.45, 2.24, 2.26, None),
        (0.351, 0.336, 0.322, 0.435, 0.365, 0.314, 0.327, 0.335, 0.324, 0.302, 0.256, 0.250, 0.228,
         0.239, None)),
    "514-music-512": (4.19, None, None),
    "514-music-768": (None, None, None),
    "514-tones-192": (None, None, None),
    "514-tones-256": (None, None, None),
    "514-tones-288": (None, None, None),
    "514-tones-320": (None, None, None),
    "514-tones-384": (None, None, None),
    "514-tones-448": (None, None, None),
    "514-tones-512": (None, None, None),
    "514-tones-768": (None, None, None),
    "514-film-192": (
        2.28,
        (3.59, 3.37, 3.31, 3.42, 3.46, 3.13, 3.22, 3.33, 3.37, 3.03, 2.75, 2.45, 2.40, 2.45, None),
        (0.350, 0.332, 0.321, 0.424, 0.348, 0.313, 0.329, 0.331, 0.327, 0.315, 0.277, 0.256, 0.262,
         0.248, None)),
    "514-film-256": (
        2.36,
        (3.60, 3.46, 3.38, 3.46, 3.45, 3.25, 3.24, 3.34, 3.18, 2.98, 2.57, 2.38, 2.27, 2.29, None),
        (0.344, 0.326, 0.331, 0.425, 0.346, 0.327, 0.327, 0.322, 0.327, 0.303, 0.256, 0.244, 0.231,
         0.239, None)),
    "514-film-288": (
        2.35,
        (3.57, 3.40, 3.32, 3.46, 3.49, 3.19, 3.16, 3.32, 3.14, 2.96, 2.57, 2.36, 2.24, 2.25, None),
        (0.346, 0.327, 0.318, 0.422, 0.348, 0.307, 0.334, 0.320, 0.332, 0.305, 0.254, 0.245, 0.228,
         0.234, None)),
    "514-film-320": (
        2.42,
        (3.57, 3.38, 3.32, 3.39, 3.46, 3.15, 3.10, 3.32, 3.16, 2.94, 2.56, 2.36, 2.23, 2.24, None),
        (0.346, 0.327, 0.322, 0.425, 0.351, 0.307, 0.327, 0.326, 0.326, 0.304, 0.255, 0.246, 0.227,
         0.233, None)),
    "514-film-384": (
        2.43,
        (3.57, 3.38, 3.33, 3.40, 3.44, 3.20, 3.12, 3.34, 3.30, 2.92, 2.60, 2.39, 2.23, 2.24, None),
        (0.347, 0.326, 0.323, 0.427, 0.356, 0.310, 0.323, 0.334, 0.323, 0.299, 0.251, 0.242, 0.227,
         0.233, None)),
    "514-film-448": (
        2.45,
        (3.56, 3.38, 3.32, 3.39, 3.43, 3.17, 3.12, 3.29, 3.30, 2.95, 2.57, 2.37, 2.23, 2.24, None),
        (0.347, 0.326, 0.320, 0.425, 0.355, 0.309, 0.327, 0.329, 0.322, 0.303, 0.255, 0.246, 0.228,
         0.233, None)),
    "514-film-512": (2.45, None, None),
    "514-film-768": (None, None, None),
    "514-speech-192": (
        2.40,
        (3.55, 3.42, 3.34, 3.49, 3.40, 3.22, 3.29, 3.32, 3.24, 2.97, 2.67, 2.44, 2.38, 2.35, None),
        (0.349, 0.334, 0.327, 0.422, 0.355, 0.318, 0.334, 0.323, 0.329, 0.310, 0.257, 0.246, 0.251,
         0.238, None)),
    "514-speech-256": (
        2.23,
        (3.59, 3.31, 3.36, 3.50, 3.37, 3.29, 3.20, 3.28, 3.24, 2.95, 2.64, 2.42, 2.23, 2.45, None),
        (0.342, 0.330, 0.325, 0.424, 0.352, 0.321, 0.325, 0.325, 0.332, 0.304, 0.255, 0.248, 0.227,
         0.237, None)),
    "514-speech-288": (
        2.23,
        (3.57, 3.38, 3.36, 3.42, 3.42, 3.17, 3.18, 3.31, 3.23, 2.92, 2.65, 2.42, 2.23, 2.45, None),
        (0.347, 0.334, 0.319, 0.425, 0.358, 0.313, 0.326, 0.320, 0.329, 0.304, 0.257, 0.248, 0.227,
         0.237, None)),
    "514-speech-320": (
        2.55,
        (3.60, 3.40, 3.34, 3.46, 3.41, 3.23, 3.14, 3.29, 3.24, 2.91, 2.67, 2.43, 2.23, 2.42, None),
        (0.345, 0.334, 0.321, 0.426, 0.353, 0.314, 0.323, 0.328, 0.332, 0.303, 0.256, 0.247, 0.227,
         0.236, None)),
    "514-speech-384": (
        2.56,
        (3.56, 3.40, 3.33, 3.44, 3.39, 3.23, 3.11, 3.30, 3.26, 2.93, 2.64, 2.43, 2.23, 2.42, None),
        (0.344, 0.330, 0.321, 0.428, 0.356, 0.307, 0.325, 0.335, 0.324, 0.304, 0.253, 0.245, 0.227,
         0.236, None)),
    "514-speech-448": (
        2.57,
        (3.58, 3.38, 3.34, 3.43, 3.40, 3.22, 3.13, 3.35, 3.35, 2.96, 2.61, 2.44, 2.23, 2.42, None),
        (0.343, 0.330, 0.321, 0.429, 0.356, 0.309, 0.326, 0.327, 0.325, 0.306, 0.258, 0.248, 0.226,
         0.236, None)),
    "514-speech-512": (2.58, None, None),
    "514-speech-768": (None, None, None),
    "514-sweep-192": (
        29.46,
        (4.00, 19.96, None, None, None, None, None, None, None, None, None, None, None, None,
         None),
        (0.197, 0.670, None, None, None, None, None, None, None, None, None, None, None, None,
         None)),
    "514-sweep-256": (
        28.54,
        (4.00, 19.97, None, None, None, None, None, None, None, None, None, None, None, None,
         None),
        (0.197, 0.670, None, None, None, None, None, None, None, None, None, None, None, None,
         None)),
    "514-sweep-288": (
        26.14,
        (4.00, 19.96, None, None, None, None, None, None, None, None, None, None, None, None,
         None),
        (0.197, 0.670, None, None, None, None, None, None, None, None, None, None, None, None,
         None)),
    "514-sweep-320": (
        22.13,
        (4.00, 19.96, None, None, None, None, None, None, None, None, None, None, None, None,
         None),
        (0.197, 0.670, None, None, None, None, None, None, None, None, None, None, None, None,
         None)),
    "514-sweep-384": (
        19.57,
        (4.00, 19.97, None, None, None, None, None, None, None, None, None, None, None, None,
         None),
        (0.197, 0.670, None, None, None, None, None, None, None, None, None, None, None, None,
         None)),
    "514-sweep-448": (
        17.73,
        (4.00, 19.96, None, None, None, None, None, None, None, None, None, None, None, None,
         None),
        (0.197, 0.670, None, None, None, None, None, None, None, None, None, None, None, None,
         None)),
    "514-sweep-512": (25.70, None, None),
    "514-sweep-768": (None, None, None),
    "514-transient-192": (1.98, None, None),
    "514-transient-256": (1.67, None, None),
    "514-transient-288": (1.67, None, None),
    "514-transient-320": (1.85, None, None),
    "514-transient-384": (1.84, None, None),
    "514-transient-448": (1.85, None, None),
    "514-transient-512": (1.85, None, None),
    "514-transient-768": (None, None, None),
}

# The frame-rate legs: name: (source, kbps, the frame_rate_index values scored against index 13).
# frame-rate= spells each index as Table 83 prints its rate.
FRAME_RATE_SOURCES = {
    "20-music-128": ("music_20", 128, tuple(range(13))),
    "20-speech-192": ("speech_20", 192, (0, 3, 7, 12)),
    "51-music-256": ("music_51", 256, (2, 5, 10)),
}
FRAME_RATE_NAMES = ("23.976", "24", "25", "29.97", "30", "47.95", "48", "50", "59.94", "60", "100",
                    "119.88", "120")
FRAME_RATE_LSD_MARGIN_DB = 0.25
FRAME_RATE_MOS_MARGIN = 0.05
# Per leg: (how far the LSD may land above index 13's, in dB; how far the MOS may land below index
# 13's, as a negative number). Measured 2026-09-25 with the encoder of phase E5: to 60 fps the LSD
# is within 0.17 dB of index 13's and the MOS within 0.012; at 100 to 120 fps, where a second holds
# five times as many frames' tables of contents, metadata and side information, music at 128 kbps
# is 0.63 to 0.87 dB over and 0.09 to 0.18 under, 5.1 at 256 kbps at 100 fps 1.28 dB and 0.11,
# and speech at 192 kbps 0.09 dB and 0.01 at 120 fps.
FRAME_RATE_PINS = {
    "20-music-128-fr0": (0.26, -0.05),
    "20-music-128-fr1": (0.26, -0.04),
    "20-music-128-fr2": (0.27, -0.05),
    "20-music-128-fr3": (0.29, -0.05),
    "20-music-128-fr4": (0.29, -0.05),
    "20-music-128-fr5": (0.35, -0.05),
    "20-music-128-fr6": (0.36, -0.05),
    "20-music-128-fr7": (0.36, -0.04),
    "20-music-128-fr8": (0.42, -0.04),
    "20-music-128-fr9": (0.42, -0.06),
    "20-music-128-fr10": (0.88, -0.14),
    "20-music-128-fr11": (1.10, -0.23),
    "20-music-128-fr12": (1.12, -0.21),
    "20-speech-192-fr0": (0.25, -0.05),
    "20-speech-192-fr3": (0.25, -0.05),
    "20-speech-192-fr7": (0.26, -0.06),
    "20-speech-192-fr12": (0.34, -0.06),
    "51-music-256-fr2": (0.34, -0.05),
    "51-music-256-fr5": (0.37, -0.06),
    "51-music-256-fr10": (1.53, -0.16),
}


# --- Sources ----------------------------------------------------------------------------------


def write_wav_f32(path, columns, rate):
    """An IEEE float WAV of the given channels (a list of equal-length arrays)."""
    data = np.stack([np.asarray(c, dtype="<f4") for c in columns], axis=1).tobytes()
    channels = len(columns)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack(
        "<IHHIIHH", 16, 3, channels, rate, rate * 4 * channels, 4 * channels, 32
    )
    Path(path).write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


def synthetic(name, rate, programme):
    """The synthetic sources, deterministic; `programme` holds the rebuilt 2.0 fixtures."""
    n = int(SECONDS * rate)
    t = np.arange(n) / rate
    if name == "tones_20":
        return [baseline.TONE_AMPLITUDE * np.sin(2 * np.pi * hz * t) for hz in baseline.TONE_HZ[:2]]
    if name == "sweep_20":
        # Logarithmic sweeps from 40 Hz to 18 kHz, up in L and down in R, at -12 dBFS.
        low, high = 40.0, 18000.0
        k = np.log(high / low) / SECONDS
        up = 0.25 * np.sin(2 * np.pi * low * (np.exp(k * t) - 1) / k)
        down = 0.25 * np.sin(2 * np.pi * high * (1 - np.exp(-k * t)) / k)
        return [up, down]
    rng = np.random.default_rng(20260925)
    if name == "noise_20":
        # Pink noise at -20 dBFS RMS, half of it shared by both channels.
        def pink():
            spectrum = np.fft.rfft(rng.standard_normal(n))
            spectrum /= np.sqrt(np.maximum(np.fft.rfftfreq(n, 1 / rate), 20.0))
            x = np.fft.irfft(spectrum, n)
            return x / np.sqrt(np.mean(x * x))

        shared = pink()
        return [0.1 * (np.sqrt(0.5) * shared + np.sqrt(0.5) * pink()) for _ in range(2)]
    if name == "castanets_20":
        # A second of silence, then a noise burst every 250 ms with a 4 ms decay, peaking near
        # -6 dBFS, louder in L.
        burst = np.zeros(n)
        envelope = np.exp(-np.arange(int(0.05 * rate)) / (0.004 * rate))
        for start in range(int(1.0 * rate), n - len(envelope), int(0.25 * rate)):
            burst[start : start + len(envelope)] += envelope * rng.standard_normal(len(envelope))
        burst *= 0.5 / np.max(np.abs(burst))
        return [burst, 0.6 * burst]
    if name == "panned_20":
        mono = programme["music_20"].mean(axis=1)
        return [0.8 * mono, 0.3 * mono]
    if name in ("music_10", "speech_10"):
        return [programme[f"{name[:-3]}_20"].mean(axis=1)]
    if name == "music_50":
        return [programme["music_51"][:, c] for c in (0, 1, 2, 4, 5)]
    raise SystemExit(f"unknown source {name}")


def build_sources(work):
    """Every leg's source WAV, by (source, rate)."""
    rebuilt = baseline.build_sources(
        work / "fixtures",
        SECONDS,
        ["music_20", "speech_20", "music_51", "film_51", "tones_51", "music_514", "tones_514"],
    )
    programme = {name: decoding.read_wav(path)[0] for name, path in rebuilt.items()}
    paths = {}
    wanted = [(source, rate) for source, rate, *_ in LEGS.values()]
    wanted += [(source, 48000) for source, *_ in IMMERSIVE_LEGS.values()]
    for source, rate in wanted:
        if (source, rate) in paths:
            continue
        path = work / f"{source}-{rate}.wav"
        if source in programme and rate == 48000:
            columns = list(programme[source].T)
        else:
            columns = synthetic(source, rate, programme)
        write_wav_f32(path, columns, rate)
        paths[(source, rate)] = path
    return paths


# --- Encoding, decoding and scoring -----------------------------------------------------------


def run(command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(
            f"{' '.join(str(c) for c in command)} failed ({result.returncode}):\n"
            f"{result.stdout}{result.stderr}"
        )


def wsl_path(path):
    text = str(Path(path).resolve()).replace("\\", "/")
    return f"/mnt/{text[0].lower()}{text[2:]}"


def decode_librempeg(args, stream, out_wav):
    """librempeg's decode of `stream`; on Windows run in WSL, where it is built."""
    if sys.platform == "win32":
        prefix = ["wsl", "-d", args.wsl_distro, "--", args.librempeg]
        stream_arg, out_arg = wsl_path(stream), wsl_path(out_wav)
    else:
        prefix = [args.librempeg]
        stream_arg, out_arg = str(stream), str(out_wav)
    run(
        [
            *prefix,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            stream_arg,
            "-acodec",
            "pcm_f32le",
            out_arg,
        ]
    )
    return decoding.read_wav(out_wav)


def decode_with_groups(cli, stream, out_wav):
    """ac3cli's decode of `stream`, and per channel the low-resolution A-SPX subband groups its
    first aspx_config() gives with the crossover offset of the aspx_data element carrying that
    channel (score_ac4_decode.py's aspx_groups and ASPX_UNIT), or None for a SIMPLE stream and
    for the LFE, which A-SPX leaves out."""
    trace = Path(str(out_wav) + ".trace")
    decoded, rate = decoding.decode(cli, stream, out_wav, trace)
    found = decoding.trace_values(trace)
    # An A-CPL leg's elements are score_acpl_leg()'s to read.
    if found is None or acpl_mode(trace) is not None:
        return decoded, rate, [None] * decoded.shape[1]
    config, offsets = found
    units = decoding.ASPX_UNIT[decoded.shape[1]]
    return (
        decoded,
        rate,
        [None if u is None else decoding.aspx_groups(config, offsets[u]) for u in units],
    )


@dataclass
class Measured:
    lag: int
    channels: list  # per channel (gain dB, SNR dB)
    lsd: float
    tiles: float | None  # ASPX: the tiles' mean absolute difference in dB
    mos: float | None
    aligned: np.ndarray


def measure(source, decoded, rate, groups):
    """The scores above, of `decoded` against `source`: each channel's SNR over the whole band
    where its entry in `groups` is None (SIMPLE, and the LFE), and below the crossover, with the
    A-SPX tiles above it, otherwise. The LFE's level is taken from 20 to 100 Hz, as
    score_ac4_decode.py takes it."""
    decoding.RATE = rate  # tone_power's, band_snr's and tile_error's rate
    lag, reference, aligned = decoding.align(source, decoded)
    channels = []
    tiles = []
    for c in range(reference.shape[1]):
        r, o = reference[:, c], aligned[:, c]
        gain = float(np.dot(r, o) / np.dot(r, r))
        if groups[c] is None:
            error = o - gain * r
            snr = 10.0 * np.log10(np.dot(gain * r, gain * r) / np.dot(error, error))
            if decoding.LFE_CHANNEL.get(reference.shape[1]) == c:
                gain = decoding.lfe_gain(r, o)
        else:
            top_hz = (groups[c][0] - 1) * decoding.subband_hz()
            gain = decoding.band_gain(r, o, top_hz)
            snr = decoding.band_snr(r, o, gain, top_hz)
            tiles += decoding.tile_error(r, o, groups[c])
        channels.append((20.0 * np.log10(abs(gain)), float(snr)))
    lsd, _ = quality_race.spectral_scores(reference, aligned)
    mos = quality_race.perceptual_score(reference, aligned, rate)
    tile = float(np.mean(np.abs(tiles))) if tiles else None
    return Measured(lag, channels, float(lsd), tile, mos, aligned)


def routing_failures(name, aligned, rate):
    failures = []
    decoding.RATE = rate
    for c in range(aligned.shape[1]):
        own = decoding.tone_power(aligned[:, c], baseline.TONE_HZ[c])
        for other in range(aligned.shape[1]):
            if other != c:
                leak = decoding.tone_power(aligned[:, c], baseline.TONE_HZ[other])
                margin = 10.0 * np.log10(own / max(leak, 1e-30))
                if margin < ROUTING_MARGIN_DB:
                    failures.append(
                        f"{name} ch{c}: its tone only {margin:.1f} dB above ch{other}'s"
                    )
    return failures


def row(label, m):
    cells = "  ".join(
        f"ch{c} {gain:+.3f} dB {snr:6.2f} dB" for c, (gain, snr) in enumerate(m.channels)
    )
    tiles = "" if m.tiles is None else f"  tiles {m.tiles:4.2f} dB"
    mos_text = "-" if m.mos is None else f"{m.mos:.2f}"
    return f"{label:<26} lag {m.lag:5d}  {cells}  LSD {m.lsd:5.2f} dB{tiles}  MOS {mos_text}"


def pinned_failures(name, pins, m):
    failures = []
    if m.lag != LAG:
        failures.append(f"{name}: lag {m.lag}, expected {LAG}")
    for c, (gain, snr) in enumerate(m.channels):
        if snr >= GAIN_MIN_SNR_DB and abs(gain) > GAIN_TOLERANCE_DB:
            failures.append(f"{name} ch{c}: gain {gain:+.3f} dB, beyond +-{GAIN_TOLERANCE_DB} dB")
    if pins is None:
        return [*failures, f"{name}: nothing pinned"]
    snr_floors, lsd_ceiling, tile_ceiling, mos_floor = pins
    for c, (_, snr) in enumerate(m.channels):
        if snr < snr_floors[c]:
            failures.append(f"{name} ch{c}: SNR {snr:.2f} dB below its floor {snr_floors[c]}")
    if m.lsd > lsd_ceiling:
        failures.append(f"{name}: LSD {m.lsd:.2f} dB above its ceiling {lsd_ceiling}")
    if (tile_ceiling is None) != (m.tiles is None):
        failures.append(f"{name}: tiles {m.tiles}, pinned {tile_ceiling}: the codec mode changed")
    elif m.tiles is not None and m.tiles > tile_ceiling:
        failures.append(f"{name}: A-SPX tiles {m.tiles:.2f} dB above the ceiling {tile_ceiling}")
    if m.mos is not None and m.mos < mos_floor:
        failures.append(f"{name}: MOS {m.mos:.2f} below its floor {mos_floor}")
    return failures


def pin_text(name, m):
    snrs = ", ".join(f"{snr - SNR_MARGIN_DB:.1f}" for _, snr in m.channels)
    snrs += "," if len(m.channels) == 1 else ""
    tiles = "None" if m.tiles is None else f"{m.tiles + TILE_MARGIN_DB:.2f}"
    mos_text = "0.0" if m.mos is None else f"{m.mos - MOS_MARGIN:.2f}"
    return f'    "{name}": (({snrs}), {m.lsd + LSD_MARGIN_DB:.2f}, {tiles}, {mos_text}),'


def encode(cli, source_path, kbps, out, options=()):
    run([cli, "ac4-encode", source_path, out, kbps, "quiet", *options])


def acpl_mode(trace):
    """The A-CPL codec mode of a decode's syntax trace, from its first 5_X_codec_mode or
    stereo_codec_mode, or None."""
    for line in Path(trace).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and fields[5] in ("5_X_codec_mode", "stereo_codec_mode"):
            return ACPL_MODES.get(int(fields[4]))
    return None


def score_acpl_leg(args, name, source_name, original, decoded, trace, failures, pins, table):
    """An A-CPL leg's lag and score_ac4_decode.py's A-CPL checks against `table`; what they
    measured, or None where the trace holds no A-SPX configuration."""
    lag, ref, out = decoding.align(original, decoded)
    if not args.measure and lag != LAG:
        failures.append(f"{name}: lag {lag}, expected {LAG}")
    found = decoding.trace_values(trace)
    if found is None:
        failures.append(f"{name}: no aspx_config() in its syntax trace")
        return None
    config, offsets = found
    return decoding.score_acpl(name, acpl_mode(trace), source_name, ref, out, offsets, config, args,
                               failures, pins, table=table)


# The pairs whose level difference and correlation the parametric modes' parameters shape, by
# score_ac4_decode.py's IMMERSIVE_CHANNELS: the top pairs A-CPL rebuilds, (Tfl, Tbl) and (Tfr,
# Tbr); and A-JCC's fronts (L, Tfl) and (R, Tfr) and backs (Ls, Tbl) and (Rs, Tbr).
IMMERSIVE_BAND_PAIRS = {
    "ASPX_ACPL_1": ((6, 8), (7, 9)),
    "ASPX_ACPL_2": ((6, 8), (7, 9)),
    "ASPX_AJCC": ((0, 6), (1, 7), (4, 8), (5, 9)),
}
# The fewest tiles above the floor an immersive leg's tile distance is taken over. The programme
# fixtures hold next to nothing above 13 kHz: from 384 kbps, where A-SPX starts at 12.75 kHz, a
# music leg has one tile above the floor, whose distance says nothing of the tiles.
IMMERSIVE_TILES_MIN = 20


def larger(a, b):
    """The larger of two band distances, either of which may be None."""
    if a is None or b is None:
        return b if a is None else a
    return max(a, b)


def immersive_extras(source_name, source, full):
    """What an immersive leg's full decoding measures besides score_immersive()'s checks, from its
    dict `full`: (the A-SPX tiles' mean absolute distance from the source's energy in dB, or None
    in SCPL and where fewer than IMMERSIVE_TILES_MIN tiles are above the floor; the level
    difference and correlation distances per A-CPL parameter band of IMMERSIVE_BAND_PAIRS' pairs,
    the largest of the pairs', or None outside the parametric modes, on a tone leg and where no
    band has frames to score)."""
    decoding.RATE = 48000
    tiles = None
    if full["mode"] != "SCPL" and full["found"] is not None:
        config, offsets = full["found"]
        groups = decoding.aspx_groups(config, min(offsets))
        differences = []
        for c in full["heard"]:
            if full["names"][c] != "LFE":
                differences += decoding.tile_error(full["ref"][:, c], full["out"][:, c], groups)
        if len(differences) >= IMMERSIVE_TILES_MIN:
            tiles = float(np.mean(np.abs(differences)))
    ild = rho = None
    if full["mode"] in IMMERSIVE_BAND_PAIRS and not source_name.startswith("tones"):
        _, ref, out = decoding.align(source, full["decoded"])
        for a, b in IMMERSIVE_BAND_PAIRS[full["mode"]]:
            pair_ild, pair_rho = decoding.acpl_band_scores(ref[:, [a, b]], out[:, [a, b]])
            if ild is None:
                ild, rho = pair_ild, pair_rho
            else:
                ild = [larger(x, y) for x, y in zip(ild, pair_ild, strict=True)]
                rho = [larger(x, y) for x, y in zip(rho, pair_rho, strict=True)]
        # A source whose pairs never sound together in a frame (the transients' tops) leaves no
        # band to score.
        if all(v is None for v in ild):
            ild = rho = None
    return tiles, ild, rho


def extra_failures(label, extras, pin):
    """`extras` (immersive_extras()) against its IMMERSIVE_EXTRA pin."""
    if pin is None:
        return [f"{label}: nothing pinned in IMMERSIVE_EXTRA"]
    tiles, ild, rho = extras
    tile_ceiling, ild_ceilings, rho_ceilings = pin
    failures = []
    if (tile_ceiling is None) != (tiles is None):
        failures.append(f"{label}: tiles {tiles}, pinned {tile_ceiling}: the codec mode changed")
    elif tiles is not None and tiles > tile_ceiling:
        failures.append(f"{label}: A-SPX tiles {tiles:.2f} dB above the ceiling {tile_ceiling}")
    if (ild_ceilings is None) != (ild is None):
        failures.append(f"{label}: A-CPL bands measured where none are pinned, or none where some "
                        "are: the codec mode changed")
        return failures
    if ild is None:
        return failures
    for band, (value, ceiling) in enumerate(zip(ild, ild_ceilings, strict=True)):
        if (value is None) != (ceiling is None):
            failures.append(f"{label} band {band}: frames to score where none were pinned, or "
                            "none where some were")
        elif value is not None and value > ceiling:
            failures.append(f"{label} band {band}: the pairs' level difference {value:.2f} dB "
                            f"from the source's, above its ceiling {ceiling}")
    for band, (value, ceiling) in enumerate(zip(rho, rho_ceilings, strict=True)):
        if value is not None and ceiling is not None and value > ceiling:
            failures.append(f"{label} band {band}: the pairs' correlation {value:.3f} from the "
                            f"source's, above its ceiling {ceiling}")
    return failures


def extra_pin_text(name, extras):
    """An IMMERSIVE_EXTRA entry as --measure prints it, within 100 columns."""
    tiles, ild, rho = extras
    tile = "None" if tiles is None else f"{tiles + TILE_MARGIN_DB:.2f}"
    if ild is None:
        return f'    "{name}": ({tile}, None, None),'
    lines = [f'    "{name}": (', f"        {tile},"]
    for values, margin, digits in ((ild, decoding.ACPL_ILD_MARGIN_DB, 2),
                                   (rho, decoding.ACPL_RHO_MARGIN, 3)):
        cells = ["None" if v is None else f"{v + margin:.{digits}f}" for v in values]
        line = "        ("
        for i, cell in enumerate(cells):
            piece = cell + ("), " if i == len(cells) - 1 else ", ")
            if len(line) + len(piece.rstrip()) > 99:
                lines.append(line.rstrip())
                line = "         "
            line += piece
        lines.append(line.rstrip())
    lines[-1] = lines[-1].removesuffix(",") + "),"
    return "\n".join(lines)


def extra_text(extras):
    """immersive_extras()' measurements, as a line."""
    tiles, ild, rho = extras
    cells = [] if tiles is None else [f"tiles {tiles:.2f} dB"]
    if ild is not None:
        worst_ild = max(v for v in ild if v is not None)
        worst_rho = max(v for v in rho if v is not None)
        cells.append(f"pairs' ILD {worst_ild:.2f} dB, rho {worst_rho:.3f} (worst band)")
    return "  ".join(cells)


def librempeg_reading(args, stream, out_wav, source):
    """What librempeg makes of an immersive stream, as a line: its error, or its channel count, the
    lag, and for each of the source's channels the output channel that follows it most closely,
    with their correlation and the least-squares gain."""
    try:
        other, _ = decode_librempeg(args, stream, out_wav)
    except SystemExit as refusal:
        lines = [line for line in str(refusal).splitlines()[1:] if line.strip()]
        return f"  librempeg: fails, {lines[-1] if lines else 'with no message'}"
    lag, ref, out = decoding.align(source, other)
    cells = []
    for c, column in enumerate(decoding.IMMERSIVE_CHANNELS[:source.shape[1]]):
        r = ref[:, c]
        best, closest = 0.0, None
        for k in range(out.shape[1]):
            o = out[:, k]
            energy = float(np.sqrt(np.dot(r, r) * np.dot(o, o)))
            if energy > 0.0 and abs(float(np.dot(r, o)) / energy) > abs(best):
                best, closest = float(np.dot(r, o)) / energy, k
        if closest is None:
            cells.append(f"{column} -")
            continue
        gain = float(np.dot(r, out[:, closest]) / np.dot(r, r))
        cells.append(f"{column} ch{closest} r {best:.3f} {20.0 * np.log10(abs(gain)):+.2f} dB")
    return (f"  librempeg: {other.shape[1]} channels, lag {lag}; each source channel's closest "
            f"output: {'  '.join(cells)}")


def wrapped(line):
    """A pin line as --measure prints it, broken within 100 columns, each further line under the
    first value."""
    if len(line) <= 100:
        return line
    indent = " " * (line.index(": (") + 3)
    lines = []
    while len(line) > 100:
        cut = line.rindex(", ", 0, 99) + 1
        lines.append(line[:cut])
        line = indent + line[cut:].lstrip()
    lines.append(line)
    return "\n".join(lines)


def score_immersive_leg(args, name, stream, source, source_name, failures, pins, extra_pins,
                        tables, work):
    """An immersive leg's full and core decoding (IMMERSIVE_LEGS' comment) against `tables`, its
    IMMERSIVE_FLOORS and IMMERSIVE_EXTRA forms: what score_immersive() measured, by "full" and
    "core", and the full decoding's immersive_extras(), or None where a decode is not the leg's
    layout."""
    table, extra_table = tables
    measured = {}
    for core in (False, True):
        measured["core" if core else "full"] = decoding.score_immersive(
            name, stream, source, source_name, core, args, work, failures, pins, table=table,
            gain_tolerance_db=GAIN_TOLERANCE_DB, gain_min_snr_db=GAIN_MIN_SNR_DB)
    if measured["full"] is None or measured["core"] is None:
        return None
    extras = immersive_extras(source_name, source, measured["full"])
    text = extra_text(extras)
    if text:
        print(f"{'':<40} {text}", flush=True)
    extra_pins.append(extra_pin_text(name, extras))
    if not args.measure:
        failures += extra_failures(name, extras, extra_table.get(name))
    return measured, extras


def immersive_run(args, work, paths):
    """The immersive legs, against IMMERSIVE_FLOORS and IMMERSIVE_EXTRA."""
    failures, pins, extra_pins = [], [], []
    count = 0
    for name, (source_name, kbps, *options) in IMMERSIVE_LEGS.items():
        if args.only is not None and args.only not in name:
            continue
        count += 1
        path = paths[(source_name, 48000)]
        stream = work / f"{name}.ac4"
        encode(args.cli, path, kbps, stream, options)
        source, _ = decoding.read_wav(path)
        score_immersive_leg(args, name, stream, source, source_name, failures, pins, extra_pins,
                            (IMMERSIVE_FLOORS, IMMERSIVE_EXTRA), work)
        if args.librempeg:
            print(librempeg_reading(args, stream, work / f"{name}.librempeg.wav", source))
    if args.measure and count:
        print("\nIMMERSIVE_FLOORS = {\n" + "\n".join(wrapped(pin) for pin in pins) + "\n}")
        print("\nIMMERSIVE_EXTRA = {\n" + "\n".join(extra_pins) + "\n}")
    return failures, count


def committed_run(args, work):
    paths = build_sources(work)
    failures = []
    pins = []
    acpl_pins = []
    legs = {name: leg for name, leg in LEGS.items() if args.only is None or args.only in name}
    for name, (source, rate, kbps, *options) in legs.items():
        stream = work / f"{name}.ac4"
        encode(args.cli, paths[(source, rate)], kbps, stream, options)
        original, _ = decoding.read_wav(paths[(source, rate)])
        decoded, decoded_rate, groups = decode_with_groups(args.cli, stream, work / f"{name}.wav")
        if decoded_rate != rate or decoded.shape[1] != original.shape[1]:
            failures.append(f"{name}: decoded {decoded.shape[1]} channels at {decoded_rate} Hz")
            continue
        trace = Path(str(work / f"{name}.wav") + ".trace")
        if acpl_mode(trace) is not None:
            score_acpl_leg(args, name, source, original, decoded, trace, failures, acpl_pins,
                           ACPL_FLOORS)
            continue
        m = measure(original, decoded, rate, groups)
        print(row(name, m), flush=True)
        pins.append(pin_text(name, m))
        if args.librempeg:
            other, _ = decode_librempeg(args, stream, work / f"{name}.librempeg.wav")
            print(row("  librempeg", measure(original, other, rate, groups)))
            print(row("  librempeg - decoder", measure(decoded, other, rate, groups)))
        if args.measure:
            continue
        failures += pinned_failures(name, FLOORS.get(name), m)
        if source.startswith("tones"):
            failures += routing_failures(name, m.aligned, rate)
    if args.measure:
        print("\nFLOORS = {\n" + "\n".join(pins) + "\n}")
        print("\nACPL_FLOORS = {\n" + "\n".join(acpl_pins) + "\n}")
    rate_failures, rate_legs = frame_rate_run(args, work, paths)
    immersive_failures, immersive_legs = immersive_run(args, work, paths)
    return (failures + rate_failures + immersive_failures,
            len(legs) + rate_legs + immersive_legs)


def frame_rate_scores(args, work, source_path, kbps, index):
    """The log-spectral distance and MOS of `source_path` encoded at `kbps` and frame_rate_index
    `index`, decoded and aligned by cross-correlation."""
    stream = work / f"fr-{index}.ac4"
    options = () if index == 13 else (f"frame-rate={FRAME_RATE_NAMES[index]}",)
    encode(args.cli, source_path, kbps, stream, options)
    original, rate = decoding.read_wav(source_path)
    decoded, _ = decoding.decode(args.cli, stream, work / f"fr-{index}.wav")
    _, reference, aligned = decoding.align(original, decoded)
    lsd, _ = quality_race.spectral_scores(reference, aligned)
    return float(lsd), quality_race.perceptual_score(reference, aligned, rate)


def frame_rate_run(args, work, paths):
    """The frame-rate legs: each against the same source and rate at index 13."""
    failures = []
    pins = []
    count = 0
    for base, (source, kbps, indices) in FRAME_RATE_SOURCES.items():
        if args.only is not None and args.only not in base:
            continue
        path = paths[(source, 48000)]
        lsd13, mos13 = frame_rate_scores(args, work, path, kbps, 13)
        for index in indices:
            name = f"{base}-fr{index}"
            count += 1
            lsd, mos = frame_rate_scores(args, work, path, kbps, index)
            lsd_delta = lsd - lsd13
            mos_delta = None if mos is None or mos13 is None else mos - mos13
            mos_text = "-" if mos_delta is None else f"{mos_delta:+.3f}"
            print(f"{name:<26} {FRAME_RATE_NAMES[index]:>7} fps  LSD {lsd:5.2f} dB "
                  f"({lsd_delta:+.2f} on index 13)  MOS {mos_text} on index 13", flush=True)
            pins.append(f'    "{name}": ({lsd_delta + FRAME_RATE_LSD_MARGIN_DB:.2f}, '
                        + ("None" if mos_delta is None
                           else f"{mos_delta - FRAME_RATE_MOS_MARGIN:.2f}") + "),")
            if args.measure:
                continue
            pin = FRAME_RATE_PINS.get(name)
            if pin is None:
                failures.append(f"{name}: nothing pinned")
                continue
            if lsd_delta > pin[0]:
                failures.append(f"{name}: LSD {lsd_delta:+.2f} dB on index 13's, beyond {pin[0]}")
            if mos_delta is not None and pin[1] is not None and mos_delta < pin[1]:
                failures.append(f"{name}: MOS {mos_delta:+.3f} on index 13's, below {pin[1]}")
    if args.measure and pins:
        print("\nFRAME_RATE_PINS = {\n" + "\n".join(pins) + "\n}")
    return failures, count


def acpl_gaps(ours, dee):
    """The encoder's A-CPL scores less DEE's, as a line."""
    pairs = enumerate(zip(ours["snrs"], dee["snrs"], strict=True))
    snrs = "  ".join(f"dmx{i} {o - d:+.2f} dB" for i, (o, d) in pairs)
    text = f"SNR {snrs}, LSD {ours['lsd'] - dee['lsd']:+.2f} dB"
    if ours["ild"] is not None and dee["ild"] is not None:
        mean = lambda values: float(np.mean([v for v in values if v is not None]))  # noqa: E731
        text += (f", ILD mean {mean(ours['ild']) - mean(dee['ild']):+.2f} dB"
                 f", rho mean {mean(ours['rho']) - mean(dee['rho']):+.3f}")
    if ours["routing"] is not None and dee["routing"] is not None:
        text += f", routing {ours['routing'] - dee['routing']:+.1f} dB"
    if ours["mos"] is not None and dee["mos"] is not None:
        text += f", MOS {ours['mos'] - dee['mos']:+.3f}"
    return text


def gold_run(args, work):
    manifest = json.loads((args.gold / "gold-manifest.json").read_text(encoding="utf-8"))
    legs = [
        (name, leg)
        for name, leg in sorted(manifest["legs"].items())
        if name.split("-")[0] in ("20", "51")
        and len(name.split("-")) == 3
        and leg.get("codec_mode") in ("SIMPLE", "ASPX", "ASPX_ACPL_2", "ASPX_ACPL_3")
        and leg.get("frame_rate_index") == 13
        and leg.get("bitrate_kbps") in RACE_RATES
        and (args.only is None or args.only in name)
    ]
    immersive = immersive_race_legs(args, manifest)
    if not legs and not immersive:
        raise SystemExit(f"no 2.0, 5.1 or 5.1.4 leg at the race's rates in {args.gold}")
    failures = []
    pins = []
    acpl_pins = []
    for name, leg in legs:
        source_path = args.gold / "sources" / f"{leg['source']}.wav"
        original, rate = decoding.read_wav(source_path)
        dee_stream = args.gold / "streams" / name / "dee.ac4"
        ours_stream = work / f"{name}.ours.ac4"
        encode(args.cli, source_path, leg["bitrate_kbps"], ours_stream)
        if leg["codec_mode"] in ACPL_MODES.values():
            scores = {}
            for label, stream in (("DEE", dee_stream), ("ours", ours_stream)):
                out_wav = work / f"{name}.{label}.wav"
                trace = work / f"{name}.{label}.trace"
                decoded, _ = decoding.decode(args.cli, stream, out_wav, trace)
                # DEE's scores are printed, not checked.
                checked = failures if label == "ours" else []
                table = ({f"{leg_name} ours": pin for leg_name, pin in RACE_ACPL.items()}
                         if label == "ours" else {})
                scores[label] = score_acpl_leg(args, f"{name} {label}", leg["source"], original,
                                               decoded, trace, checked,
                                               acpl_pins if label == "ours" else [], table)
            if scores["ours"] is not None and scores["DEE"] is not None:
                print(f"{'':<26} ours less DEE's: {acpl_gaps(scores['ours'], scores['DEE'])}",
                      flush=True)
            continue
        scores = {}
        for label, stream in (("DEE", dee_stream), ("ours", ours_stream)):
            decoded, _, groups = decode_with_groups(args.cli, stream, work / f"{name}.{label}.wav")
            scores[label] = measure(original, decoded, rate, groups)
            print(row(f"{name} {label}", scores[label]), flush=True)
            if args.librempeg:
                other, _ = decode_librempeg(args, stream, work / f"{name}.{label}.librempeg.wav")
                print(row("  librempeg", measure(original, other, rate, groups)))
                print(row("  librempeg - decoder", measure(decoded, other, rate, groups)))
        ours, dee = scores["ours"], scores["DEE"]
        pairs = zip(ours.channels, dee.channels, strict=True)
        gaps = "  ".join(f"ch{c} {o[1] - d[1]:+.2f} dB" for c, (o, d) in enumerate(pairs))
        mos_gap = "" if ours.mos is None or dee.mos is None else f", MOS {ours.mos - dee.mos:+.3f}"
        print(
            f"{'':<26} ours less DEE's: SNR {gaps}, LSD {ours.lsd - dee.lsd:+.2f} dB{mos_gap}",
            flush=True,
        )
        pins.append(pin_text(name, ours))
        if not args.measure:
            failures += pinned_failures(name, RACE.get(name), ours)
            if leg["source"].startswith("tones"):
                failures += routing_failures(f"{name} ours", ours.aligned, rate)
    if args.measure and legs:
        print("\nRACE = {\n" + "\n".join(pins) + "\n}")
        print("\nRACE_ACPL = {\n" + "\n".join(acpl_pins) + "\n}")
    failures += immersive_race(args, work, immersive)
    return failures, len(legs) + len(immersive)


def immersive_race_legs(args, manifest):
    """The race's 5.1.4 legs (RACE_IMMERSIVE_CONTENT at RACE_RATES), from G0's legs and G1's."""
    legs = []
    for key in ("legs", "g1_legs"):
        for name, leg in sorted(manifest.get(key, {}).items()):
            parts = name.split("-")
            if (len(parts) == 3 and parts[0] == "514" and parts[1] in RACE_IMMERSIVE_CONTENT
                    and leg.get("frame_rate_index") == 13
                    and leg.get("bitrate_kbps") in RACE_RATES
                    and (args.only is None or args.only in name)):
                legs.append((name, leg))
    return legs


def immersive_gaps(ours, dee):
    """The encoder's immersive scores less DEE's, as a line, from two score_immersive_leg()
    results of one decoding: ((measured, extras), (measured, extras))."""
    (o, o_extras), (d, d_extras) = ours, dee
    if o["names"] != d["names"]:
        return f"codec modes differ: ours {o['mode']}, DEE's {d['mode']}"
    snrs = "  ".join(f"{column} {a - b:+.2f}" for column, a, b in zip(o["names"], o["snrs"],
                                                                      d["snrs"], strict=True)
                     if a is not None and b is not None)
    text = f"SNR {snrs} dB, LSD {o['lsd'] - d['lsd']:+.2f} dB"
    if o["routing"] is not None and d["routing"] is not None:
        text += f", routing {o['routing'] - d['routing']:+.1f} dB"
    if o["mos"] is not None and d["mos"] is not None:
        text += f", MOS {o['mos'] - d['mos']:+.3f}"
    if o_extras is not None and d_extras is not None:
        if o_extras[0] is not None and d_extras[0] is not None:
            text += f", tiles {o_extras[0] - d_extras[0]:+.2f} dB"
        if o_extras[1] is not None and d_extras[1] is not None:
            def mean(values):
                return float(np.mean([v for v in values if v is not None]))

            text += (f", pairs' ILD mean {mean(o_extras[1]) - mean(d_extras[1]):+.2f} dB"
                     f", rho mean {mean(o_extras[2]) - mean(d_extras[2]):+.3f}")
    return text


def immersive_race(args, work, legs):
    """The race's 5.1.4 legs: DEE's stream and the encoder's of the same source and rate, each
    scored as the immersive legs are, the encoder's against RACE_IMMERSIVE and
    RACE_IMMERSIVE_EXTRA."""
    failures, pins, extra_pins = [], [], []
    for name, leg in legs:
        source_path = args.gold / "sources" / f"{leg['source']}.wav"
        source, _ = decoding.read_wav(source_path)
        dee_stream = args.gold / "streams" / name / "dee.ac4"
        ours_stream = work / f"{name}.ours.ac4"
        encode(args.cli, source_path, leg["bitrate_kbps"], ours_stream)
        results = {}
        for label, stream in (("DEE", dee_stream), ("ours", ours_stream)):
            ours = label == "ours"
            # DEE's scores are printed, not checked, nor pinned.
            tables = (({(f"{leg_name} ours", decode): pin
                        for (leg_name, decode), pin in RACE_IMMERSIVE.items()},
                       {f"{leg_name} ours": pin for leg_name, pin in RACE_IMMERSIVE_EXTRA.items()})
                      if ours else ({}, {}))
            results[label] = score_immersive_leg(args, f"{name} {label}", stream, source,
                                                 leg["source"], failures if ours else [],
                                                 pins if ours else [],
                                                 extra_pins if ours else [], tables, work)
            if args.librempeg:
                print(librempeg_reading(args, stream, work / f"{name}.{label}.librempeg.wav",
                                        source))
        if results["ours"] is None or results["DEE"] is None:
            continue
        for decode in ("full", "core"):
            ours = (results["ours"][0][decode], results["ours"][1] if decode == "full" else None)
            dee = (results["DEE"][0][decode], results["DEE"][1] if decode == "full" else None)
            print(f"{'':<40} ours less DEE's ({decode}): {immersive_gaps(ours, dee)}", flush=True)
    if args.measure and legs:
        pins = [wrapped(pin.replace(' ours"', '"')) for pin in pins]
        print("\nRACE_IMMERSIVE = {\n" + "\n".join(pins) + "\n}")
        print("\nRACE_IMMERSIVE_EXTRA = {\n" + "\n".join(extra_pins).replace(' ours"', '"')
              + "\n}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cli", required=True, type=Path, help="the ac3cli to encode and decode with"
    )
    parser.add_argument(
        "--gold", type=Path, help="run the race against G0's gold set in this directory"
    )
    parser.add_argument(
        "--librempeg", help="librempeg's ffmpeg, to decode with as well (a WSL path on Windows)"
    )
    parser.add_argument(
        "--wsl-distro", default="Ubuntu-26.04", help="the WSL distribution librempeg is built in"
    )
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument(
        "--measure", action="store_true", help="print the measurements, check nothing"
    )
    parser.add_argument("--only", help="run only the legs whose names contain this")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        failures, count = gold_run(args, work) if args.gold else committed_run(args, work)
    if failures:
        print("\nFAILED:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    if not args.measure:
        print(f"\n{count} legs: lag, level, SNR, tiles, LSD and MOS floors all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
