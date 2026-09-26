"""Check the gains of ac3cli's AC-4 output processing on DEE's and the encoder's streams against
Part 1's formulas and Part 2's channel renderer.

For each leg the decoder turns into PCM, at every frame rate, this decodes the stream as coded and
again with an output option, and holds the second decode to the first through the formula the option
applies, with the stream's own values read from `ac3cli decode ... syntax-trace=` (planning/ac4.md,
phase D6):

  level    output-level=-31, -24 and -17 with drcmode=off: every channel is the coded output times
           2^((Lout - dialnorm) / 6) (ETSI TS 103 190-1 clause 5.7.9.3.3), dialnorm being
           -dialnorm_bits / 4 dBFS, to 0.01 dB, and nothing else: what the gain leaves is 100 dB
           under the output.
  downmix  a 5.1 leg's channels=2 (the stream's preferred method), downmix=loro, ltrt and mono: the
           output is clause 6.2.17's matrix with the stream's values applied to the coded output,
           which is to say Table 218's Lo/Ro or Lt/Rt, the latter in its Pro Logic II form (+1.8 and
           -3.2 dB) where the stream prefers that, with Tables 149 and 149a's centre and surround
           gains, the LFE at 5.5 - lfe_mixgain dB when the stream sends lfe_mixgain, the stream's
           loudness correction of (15 - x) / 2 dB2, and for mono L + R. What the matrix leaves is 80
           dB under the output: the downmix works in the QMF domain before the synthesis bank, which
           is linear, so the two differ only in the output's rounding.
  render   a 5.1.4 leg's immersive element, in full decoding speakers=5.1, 5.1.2, 5.1.4, 7.1,
           7.1.2 and 7.1.4, in core decoding speakers=5.1 (and 5.1.2 and 7.1.4, which are its
           as-coded 5.X.2), and in both channels=2, downmix=loro, ltrt and mono: the output is
           Part 2's channel renderer (ETSI TS 103 190-2 clause 5.10.2) applied to the coded
           output, with the custom downmix gains the stream sends (Tables 129 and 130, read from
           the trace with the codes cdmx_parameters() assigns rather than reads) and the output's
           loudness correction where it downmixes: in full decoding Tables 38 to 43 from the
           source's configuration, in core decoding Table 46 over the inverse of Table 45, which
           the as-coded output took, and for two channels and one Table 218 after 5.X.0 with the
           stereo correction alone, the core's in core decoding (src/ac4dec/ERRATA.md, "The
           channel renderer"). Every channel with signal to 0.01 dB, and what the matrix leaves
           80 dB under the output.

  dialogue enhancement
           on the encoder's streams (--encoder), dialogue-enhancement=3, 6 and 12: each channel
           the stream marks as carrying dialogue alone, whose parameters are 1 in every band, is
           the coded output raised by the gain or the stream's cap where that is lower (clause
           5.7.8, 1 + g p with g = 10^(G / 20) - 1), with the Mid method both of a pair carrying
           the same tone; the other channels are the coded output. To 0.01 dB, and what the gain
           leaves is 80 dB under the output.

All skip the first three frames, where the stream's values have not yet reached the QMF domain,
and stop three frames before the first frame whose values differ from the first frame's: DEE's
immersive stereo at 24 and 25 fps sends a dialnorm of -24 dBFS in its last frame. DRC's curves and
dialogue enhancement's gains are held on known input by tests/ac4dec/test_ac4dec_drc.cpp and
test_ac4dec_de.cpp; this script reads the gains from the stream, as those tests cannot.

The committed legs (tests/golden/external-baseline/) are checked by default. --gold DIR checks
phase G0's local gold set in DIR (DIR/streams/<leg>/dee.ac4, DIR/gold-manifest.json), which never
runs in CI, and with --g1 the G1 legs G1_LEGS names besides. --encoder checks streams
`ac3cli ac4-encode` writes from tones here, a leg for each metadata option the output processing
reads (ENCODER_LEGS), at several frame rates (planning/ac4.md, phase E5).

Usage:
    python tools/checks/gain_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli
    python tools/checks/gain_ac4_decode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold
    python tools/checks/gain_ac4_decode.py --cli ac3cli.exe --gold D:/ac3bld/ac4-gold --g1
    python tools/checks/gain_ac4_decode.py --cli build/config-linux-llvm/bin/ac3cli --encoder
"""

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "checks"))
from score_ac4_decode import BASELINE_DIR, RESAMPLING, read_wav  # noqa: E402

OUTPUT_LEVELS = (-31.0, -24.0, -17.0)
LEVEL_TOLERANCE_DB = 0.01
LEVEL_RESIDUAL_DB = -100.0
DOWNMIX_RESIDUAL_DB = -80.0
RENDER_TOLERANCE_DB = 0.01
SKIP_FRAMES = 3
CODEC_MODES = ("SIMPLE", "ASPX", "ASPX_ACPL_1", "ASPX_ACPL_2", "ASPX_ACPL_3")
IMMERSIVE_MODES = ("SCPL", "ASPX_SCPL", "ASPX_ACPL_1", "ASPX_ACPL_2", "ASPX_AJCC")
LAYOUTS = ("stereo", "mono", "IMS", "5.1")
# The fields of the stream's downmix values, wherever the stream sends them: the presentation
# substream's custom_dmx_data() and loud_corr(), or basic_metadata().
DOWNMIX_FIELDS = ("loro_centre_mixgain", "loro_surround_mixgain", "b_ltrt_mixinfo",
                  "ltrt_centre_mixgain", "ltrt_surround_mixgain", "lfe_mixgain",
                  "preferred_dmx_method", "loro_dmx_loud_corr", "ltrt_dmx_loud_corr")
# loud_corr()'s corrections for the immersive element's outputs and its core (Part 2 6.2.9.1).
IMMERSIVE_CORRECTIONS = ("loud_corr_5_X", "loud_corr_5_X_2", "loud_corr_5_X_4", "loud_corr_7_X",
                         "loud_corr_7_X_2", "loud_corr_7_X_4", "loud_corr_core_5_X_2",
                         "loud_corr_core_5_X", "loud_corr_core_loro", "loud_corr_core_ltrt")
# custom_dmx_data()'s fields (Part 2 6.2.9.2 to 6.2.9.10), each out_ch_config's run in a frame.
CDMX_FIELDS = ("out_ch_config", "b_top_front_to_front", "b_top_front_to_side",
               "b_top_back_to_front", "b_top_back_to_side", "b_top_to_front", "b_top_to_side",
               "gain_b_code", "gain_t1_code", "gain_t2a_code", "gain_t2b_code", "gain_t2c_code",
               "gain_t2d_code", "gain_t2e_code", "gain_t2f_code")
# The coded order of a 5.1 leg's output, and ac3cli's WAV order, which is the same.
L, R, C, LFE, LS, RS = range(6)
DE_GAINS = (3.0, 6.0, 12.0)
DE_TOLERANCE_DB = 0.01
DE_RESIDUAL_DB = -80.0
# The encoder's legs: name, channels, rate, ac4-encode's options, and the channels the stream marks
# as carrying dialogue alone (dialogue-channels=), as output channel indices, or "mid" where the
# Mid of L and R is raised.
ENCODER_LEGS = (
    ("enc-2.0-192-dialnorm31", 2, 192, ("dialnorm=31",), None),
    ("enc-2.0-128-29.97-dialnorm24.5", 2, 128, ("frame-rate=29.97", "dialnorm=24.5"), None),
    ("enc-1.0-64-120-dialnorm17.25", 1, 64, ("frame-rate=120", "dialnorm=17.25"), None),
    ("enc-5.1-384-loro", 6, 384, ("dialnorm=27", "lorocmixlev=-1.5", "lorosurmixlev=-4.5",
                                  "lfemix=-4.5", "dmixmod=loro", "loro-correction=-2"), None),
    ("enc-5.1-384-ltrt", 6, 384, ("ltrtcmixlev=-3", "ltrtsurmixlev=-6", "dmixmod=ltrt",
                                  "ltrt-correction=1.5", "lfemix=+2.5"), None),
    ("enc-5.1-128-25-pl2", 6, 128, ("frame-rate=25", "dmixmod=pl2", "lorocmixlev=0",
                                    "lorosurmixlev=off"), None),
    ("enc-5.0-192-none", 5, 192, ("dmixmod=none", "cmixlev=+3", "surmixlev=0"), None),
    ("enc-1.0-96-de-c", 1, 96, ("dialogue-channels=c", "dialogue-max-gain=12"), (0,)),
    ("enc-2.0-192-de-lr", 2, 192, ("dialogue-channels=l,r", "dialogue-max-gain=9"), (0, 1)),
    ("enc-2.0-192-de-mid", 2, 192, ("dialogue-channels=l,r", "dialogue-method=mid",
                                    "dialogue-max-gain=6"), "mid"),
    ("enc-5.1-384-50-de-c", 6, 384, ("frame-rate=50", "dialogue-channels=c",
                                     "dialogue-max-gain=9"), (C,)),
)
# Each channel's tone, under Table 173's last dialogue enhancement band (subband 41, 15.4 kHz) and
# the LFE's under 140 Hz, 20 dB under full scale, for four seconds.
TONES_HZ = (440.0, 620.0, 800.0, 90.0, 1030.0, 1270.0)
ENCODER_SECONDS = 4
# The G1 legs (the gold manifest's g1_legs) --g1 adds, for the immersive element's renders: DEE's
# 5.1.4 tones with each height downmix its options give (custom downmix data for 5.X.0), with each
# stereo downmix method and each centre and surround mix gain, and film at each of DEE's 5.1.4
# rates, which take its three immersive codec modes.
_HEIGHT_GAINS = ("minf", "m12", "m9", "m6", "m4.5", "m3", "m1.5", "0")
G1_LEGS = tuple(
    [f"514-tones-256-height-{route}-{gain}" for route in ("front", "front_and_surround")
     for gain in _HEIGHT_GAINS]
    + [f"514-tones-256-height-surround-{gain}" for gain in _HEIGHT_GAINS if gain != "m3"]
    + [f"514-tones-256-dmx-{method}" for method in ("loro", "ltrt", "ltrt-pl2", "not_indicated")]
    + [f"514-tones-256-mix-{method}_cmix-{gain}" for method in ("loro", "ltrt")
       for gain in ("m6", "m4.5", "m1.5", "0", "p1.5", "p3")]
    + [f"514-tones-256-mix-{method}_smix-{gain}" for method in ("loro", "ltrt")
       for gain in ("minf", "m6", "m4.5", "m1.5")]
    + [f"514-film-{kbps}" for kbps in (192, 256, 288, 320, 384, 448, 512, 768)])


def db(x):
    return 20.0 * np.log10(x)


def from_db(value):
    return 10.0 ** (value / 20.0)


def centre_gain(code):
    """Table 149: +3, +1.5, 0, -1.5, -3, -4.5, -6 dB and silence; -3 dB where none is sent."""
    if code is None:
        return from_db(-3.0)
    return 0.0 if code == 7 else from_db((3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0)[code])


def surround_gain(code):
    """Table 149a: codes 0 and 1 reserved, read as none sent (-3 dB); then 0, -1.5, -3, -4.5 and
    -6 dB and silence."""
    if code is None or code < 2:
        return from_db(-3.0)
    return 0.0 if code == 7 else from_db((0.0, -1.5, -3.0, -4.5, -6.0)[code - 2])


def loudness_correction(code):
    """(15 - x) / 2 dB2, 31 reading as 0 dB; 1 where none is sent."""
    if code is None or code == 31:
        return 1.0
    return 2.0 ** ((15.0 - code) / 2.0 / 6.0)


def stereo_matrix(values, target):
    """Rows Lo and Ro (or C for mono) over L R C LFE Ls Rs, for `target` 'stereo', 'loro', 'ltrt'
    or 'mono' with the stream's `values` (DOWNMIX_FIELDS, absent where the stream sends none)."""
    preferred = values.get("preferred_dmx_method", 0)
    method = preferred if preferred in (2, 3) else 1
    if target == "loro":
        method = 1
    elif target == "ltrt":
        method = 3 if preferred == 3 else 2
    loro = method == 1
    gains = "ltrt" if not loro and values.get("b_ltrt_mixinfo", 0) == 1 else "loro"
    cmg = centre_gain(values.get(f"{gains}_centre_mixgain"))
    smg = surround_gain(values.get(f"{gains}_surround_mixgain"))
    lo = np.zeros(6)
    ro = np.zeros(6)
    lo[L] = ro[R] = 1.0
    lo[C] = ro[C] = cmg
    if loro:
        lo[LS] = ro[RS] = smg
    else:
        near = smg * from_db(1.8) if method == 3 else smg
        far = smg * from_db(-3.2) if method == 3 else smg
        lo[LS], lo[RS] = -near, -far
        ro[RS], ro[LS] = near, far
    if values.get("lfe_mixgain") is not None:
        lo[LFE] = ro[LFE] = from_db(5.5 - values["lfe_mixgain"])
    correction = loudness_correction(values.get("loro_dmx_loud_corr" if loro
                                                else "ltrt_dmx_loud_corr"))
    lo, ro = lo * correction, ro * correction
    return (lo + ro)[np.newaxis, :] if target == "mono" else np.stack([lo, ro])


# Part 2 5.10.2.2's generalized rendering matrix: its channels by index.
GENERAL = ("L", "R", "C", "Ls", "Rs", "Lb", "Rb", "Tfl", "Tfr", "Tbl", "Tbr", "LFE", "Tsl", "Tsr")
# The immersive element's layouts by speakers= name: the configuration each is (Table 34's names)
# and its channels in ac3cli's WAV order (apps/cli/ac4_channels.hpp).
IMMERSIVE_LAYOUTS = {
    "7.1.4": ("7.X.4", ("L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs", "Tfl", "Tfr", "Tbl", "Tbr")),
    "7.1.2": ("7.X.2", ("L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs", "Tsl", "Tsr")),
    "7.1": ("7.X.0", ("L", "R", "C", "LFE", "Lb", "Rb", "Ls", "Rs")),
    "5.1.4": ("5.X.4", ("L", "R", "C", "LFE", "Ls", "Rs", "Tfl", "Tfr", "Tbl", "Tbr")),
    "5.1.2": ("5.X.2", ("L", "R", "C", "LFE", "Ls", "Rs", "Tsl", "Tsr")),
    "5.1": ("5.X.0", ("L", "R", "C", "LFE", "Ls", "Rs")),
}
# The layouts DEE codes the immersive element from, by the manifests' output_channel_layout.
IMMERSIVE_SOURCES = ("5.1.4", "7.1.4")
# Table 126's bs_ch_config and Table 127's out_ch_config, by configuration.
BS_CH_CONFIG = {"7.X.4": 1, "5.X.4": 2, "7.X.2": 4, "5.X.2": 5}
OUT_CH_CONFIG = {"5.X.0": 0, "5.X.2": 1, "5.X.4": 2, "7.X.0": 3, "7.X.2": 4}
# Clause 4.8.5.3's corrections by output configuration, in full and in core decoding.
CORRECTION = {"5.X.0": "loud_corr_5_X", "5.X.2": "loud_corr_5_X_2", "5.X.4": "loud_corr_5_X_4",
              "7.X.0": "loud_corr_7_X", "7.X.2": "loud_corr_7_X_2", "7.X.4": "loud_corr_7_X_4"}
CORE_CORRECTION = {"5.X.0": "loud_corr_core_5_X", "5.X.2": "loud_corr_core_5_X_2"}


def _diag(*indices):
    return [(i, i, "0") for i in indices]


# Tables 38 to 43 (Part 2 5.10.2.5, pp. 105 to 107), transcribed here from the text as the
# decoder's tests transcribe them: {(output, input): [(out, in, gain)]}, indices as GENERAL numbers
# them, for the configurations a 7.X.4 mode's source can have. "0" is ri,i = 0 dB, "-3" -3 dB, the
# rest the custom downmix gains.
_TOP_M3 = [(7, 12, "-3"), (9, 12, "-3"), (8, 13, "-3"), (10, 13, "-3")]
_BACK_B = [(3, 3, "b"), (3, 5, "b"), (4, 4, "b"), (4, 6, "b")]
_BACK_M3 = [(3, 3, "-3"), (3, 5, "-3"), (4, 4, "-3"), (4, 6, "-3")]
_T1 = [(12, 7, "t1"), (12, 9, "t1"), (13, 8, "t1"), (13, 10, "t1")]
_FRONT_SIDE_4 = [(0, 7, "t2a"), (1, 8, "t2a"), (3, 7, "t2b"), (4, 8, "t2b"), (0, 9, "t2d"),
                 (1, 10, "t2d"), (3, 9, "t2e"), (4, 10, "t2e")]
_FRONT_SIDE_2 = [(0, 12, "t2a"), (1, 13, "t2a"), (3, 12, "t2b"), (4, 13, "t2b")]
FULL_TABLES = {
    # Table 38, to 7.X.4.
    ("7.X.4", "7.X.4"): _diag(*range(11)),
    ("7.X.4", "7.X.2"): _diag(*range(7)) + _TOP_M3,
    ("7.X.4", "7.X.0"): _diag(*range(7)),
    ("7.X.4", "5.X.4"): _diag(0, 1, 2, 3, 4, 7, 8, 9, 10),
    ("7.X.4", "5.X.2"): _diag(*range(5)) + _TOP_M3,
    ("7.X.4", "5.X.0"): _diag(*range(5)),
    # Table 39, to 7.X.2.
    ("7.X.2", "7.X.4"): _diag(*range(7)) + _T1,
    ("7.X.2", "7.X.2"): _diag(*range(7), 12, 13),
    ("7.X.2", "7.X.0"): _diag(*range(7)),
    ("7.X.2", "5.X.4"): [*_diag(*range(5)), (12, 7, "-3"), (12, 9, "-3"), (13, 8, "-3"),
                         (13, 10, "-3")],
    ("7.X.2", "5.X.2"): _diag(*range(5), 12, 13),
    ("7.X.2", "5.X.0"): _diag(*range(5)),
    # Table 40, to 7.X.0.
    ("7.X.0", "7.X.4"): [*_diag(*range(7)), *_FRONT_SIDE_4, (5, 7, "t2c"), (6, 8, "t2c"),
                         (5, 9, "t2f"), (6, 10, "t2f")],
    ("7.X.0", "7.X.2"): [*_diag(*range(7)), *_FRONT_SIDE_2, (5, 12, "t2c"), (6, 13, "t2c")],
    ("7.X.0", "7.X.0"): _diag(*range(7)),
    ("7.X.0", "5.X.4"): [*_diag(*range(5)), (3, 7, "-3"), (4, 8, "-3"), (3, 9, "-3"),
                         (4, 10, "-3")],
    ("7.X.0", "5.X.2"): [*_diag(*range(5)), (3, 12, "-3"), (4, 13, "-3")],
    ("7.X.0", "5.X.0"): _diag(*range(5)),
    # Table 41, to 5.X.4.
    ("5.X.4", "7.X.4"): _diag(0, 1, 2, 7, 8, 9, 10) + _BACK_B,
    ("5.X.4", "7.X.2"): _diag(0, 1, 2) + _BACK_B + _TOP_M3,
    ("5.X.4", "7.X.0"): _diag(0, 1, 2) + _BACK_M3,
    ("5.X.4", "5.X.4"): _diag(0, 1, 2, 3, 4, 7, 8, 9, 10),
    ("5.X.4", "5.X.2"): _diag(*range(5)) + _TOP_M3,
    ("5.X.4", "5.X.0"): _diag(*range(5)),
    # Table 42, to 5.X.2.
    ("5.X.2", "7.X.4"): _diag(0, 1, 2) + _BACK_B + _T1,
    ("5.X.2", "7.X.2"): _diag(0, 1, 2, 12, 13) + _BACK_B,
    ("5.X.2", "7.X.0"): _diag(0, 1, 2) + _BACK_M3,
    ("5.X.2", "5.X.4"): _diag(*range(5)) + _T1,
    ("5.X.2", "5.X.2"): _diag(*range(5), 12, 13),
    ("5.X.2", "5.X.0"): _diag(*range(5)),
    # Table 43, to 5.X.0.
    ("5.X.0", "7.X.4"): _diag(0, 1, 2) + _BACK_B + _FRONT_SIDE_4,
    ("5.X.0", "7.X.2"): _diag(0, 1, 2) + _BACK_B + _FRONT_SIDE_2,
    ("5.X.0", "7.X.0"): _diag(0, 1, 2) + _BACK_M3,
    ("5.X.0", "5.X.4"): _diag(*range(5)) + _FRONT_SIDE_4,
    ("5.X.0", "5.X.2"): _diag(*range(5)) + _FRONT_SIDE_2,
    ("5.X.0", "5.X.0"): _diag(*range(5)),
}


def configuration(name):
    """(width, top channels) of one of Table 34's configurations."""
    return int(name[0]), int(name[-1])


def layout_names(config, lfe):
    """A configuration's channels in ac3cli's WAV order, without the LFE where there is none."""
    names = next(n for c, n in IMMERSIVE_LAYOUTS.values() if c == config)
    return tuple(n for n in names if lfe or n != "LFE")


def gain_129(code):
    """Table 129: 0, -1.5, -3, -4.5, -6, -9, -12 dB, and 7 silence."""
    return 0.0 if code == 7 else from_db((0.0, -1.5, -3.0, -4.5, -6.0, -9.0, -12.0)[code])


def render_gains(cdmx, out_ch_config, bs_ch_config):
    """Clause 6.3.10.3.10: out_ch_config's gains from the stream's custom downmix data (cdmx_data),
    Table 130's defaults for what it does not send, and from bs_ch_config 1 to out_ch_config 4
    out_ch_config 1's gain_t1 where 4 sends none."""
    m3 = from_db(-3.0)
    gains = {"b": m3, "t1": m3, "t2a": 0.0, "t2b": m3, "t2c": 0.0, "t2d": 0.0, "t2e": m3,
             "t2f": 0.0}
    own = cdmx.get(out_ch_config, {})
    for key in gains:
        if f"gain_{key}_code" in own:
            gains[key] = gain_129(own[f"gain_{key}_code"])
    first = cdmx.get(1, {})
    if (bs_ch_config == 1 and out_ch_config == 4 and "gain_t1_code" not in own
            and "gain_t1_code" in first):
        gains["t1"] = gain_129(first["gain_t1_code"])
    return gains


def downmixes(source, output):
    """Whether rendering configuration `source` to `output` downmixes, the output narrower or
    lower (src/ac4dec/ERRATA.md, "The loudness correction of a render")."""
    (width_in, tops_in), (width_out, tops_out) = configuration(source), configuration(output)
    return width_out < width_in or tops_out < tops_in


def full_matrix(source, output, gains, lfe):
    """Tables 38 to 43 from full decoding's as-coded channels, a source of configuration `source`,
    to configuration `output`: [out][in], both in WAV order."""
    in_names, out_names = layout_names(source, lfe), layout_names(output, lfe)
    entries = FULL_TABLES[(output, source)] + ([(11, 11, "0")] if lfe else [])
    matrix = np.zeros((len(out_names), len(in_names)))
    for out, inp, gain in entries:
        value = {"0": 1.0, "-3": from_db(-3.0)}.get(gain)
        matrix[out_names.index(GENERAL[out]), in_names.index(GENERAL[inp])] = (
            gains[gain] if value is None else value)
    return matrix


def core_matrix(output, backs, tops, gains, lfe):
    """Tables 45 and 46 (p. 108) from the core's channels, L R C [LFE] Ls Rs Tsl Tsr, to
    configuration `output`, [out][in] in WAV order, by top_channels_present and
    b_4_back_channels_present."""
    core_names = layout_names("5.X.2", lfe)
    out_names = layout_names(output, lfe)
    p3 = from_db(3.0)
    entries = [("L", "L", 1.0), ("R", "R", 1.0), ("C", "C", 1.0), ("LFE", "LFE", 1.0)]
    side = gains["b"] * p3 if backs else p3
    entries += [("Ls", "Ls", side), ("Rs", "Rs", side)]
    if output == "5.X.2" and tops != 0:
        top = gains["t1"] * p3 if tops == 3 else p3
        entries += [("Tsl", "Tsl", top), ("Tsr", "Tsr", top)]
    elif tops != 0:
        entries += [("L", "Tsl", gains["t2a"] * p3), ("R", "Tsr", gains["t2a"] * p3),
                    ("Ls", "Tsl", gains["t2b"] * p3), ("Rs", "Tsr", gains["t2b"] * p3)]
    matrix = np.zeros((len(out_names), len(core_names)))
    for out, inp, value in entries:
        if out in out_names and inp in core_names:
            matrix[out_names.index(out), core_names.index(inp)] = value
    return matrix


def render_checks(source, backs, tops, lfe, values, cdmx, core):
    """(options, matrix from the as-coded output) for each render ac3cli decode is held to, in
    full or core decoding: None for a matrix the as-coded output cannot predict."""
    bs = BS_CH_CONFIG.get(source, -1)
    gains = {c: render_gains(cdmx, OUT_CH_CONFIG.get(c, -1), bs) for c in OUT_CH_CONFIG}
    gains["7.X.4"] = render_gains({}, -1, bs)

    def corrected(matrix, output):
        names = CORE_CORRECTION if core else CORRECTION
        if downmixes(source, output):
            return matrix * loudness_correction(values.get(names[output]))
        return matrix

    if core:
        # The as-coded output is Table 45's 5.X.2 of the core (5.X.0 without top channels), which
        # Table 46's 5.X.0 is predicted from through Table 45's inverse, where it has one.
        coded = "5.X.2" if tops else "5.X.0"
        columns = len(layout_names(coded, lfe))  # without top channels, Tsl and Tsr are silent
        through = corrected(core_matrix(coded, backs, tops, gains[coded], lfe), coded)[:, :columns]
        diagonal = np.diag(through)
        if np.any(diagonal == 0.0) or np.count_nonzero(through - np.diag(diagonal)):
            return []
        inverse = np.diag(1.0 / diagonal)
        five = core_matrix("5.X.0", backs, tops, gains["5.X.0"], lfe)[:, :columns]
        renders = [(("speakers=5.1",), corrected(five, "5.X.0") @ inverse)]
        if coded == "5.X.2":
            # Table 44: every layout with top channels is 5.X.2 in core decoding.
            renders += [((f"speakers={name}",), np.eye(columns)) for name in ("5.1.2", "7.1.4")]
        loro_key, ltrt_key = "loud_corr_core_loro", "loud_corr_core_ltrt"
        stereo_from = five @ inverse
    else:
        renders = []
        for name, (config, _) in IMMERSIVE_LAYOUTS.items():
            renders.append(((f"speakers={name}",),
                            corrected(full_matrix(source, config, gains[config], lfe), config)))
        loro_key, ltrt_key = "loro_dmx_loud_corr", "ltrt_dmx_loud_corr"
        stereo_from = full_matrix(source, "5.X.0", gains["5.X.0"], lfe)
    if not lfe:
        # stereo_matrix's rows take L R C LFE Ls Rs.
        stereo_from = np.insert(stereo_from, LFE, 0.0, axis=0)
    stereo_values = dict(values)
    stereo_values["loro_dmx_loud_corr"] = values.get(loro_key)
    stereo_values["ltrt_dmx_loud_corr"] = values.get(ltrt_key)
    for target, options in (("stereo", ("channels=2",)), ("loro", ("downmix=loro",)),
                            ("ltrt", ("downmix=ltrt",)), ("mono", ("downmix=mono",))):
        renders.append((options, stereo_matrix(stereo_values, target) @ stereo_from))
    return renders


def stream_values(trace):
    """The dialnorm and the downmix values of a syntax trace's first frames, the frame at which
    one of them first changes (None where none does), and the number of frames."""
    first = {}
    change = None
    frames = 0
    for line in Path(trace).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 6:
            continue
        frame = int(fields[0])
        frames = max(frames, frame + 1)
        name, value = fields[5], int(fields[4])
        if (name not in ("dialnorm_bits", "de_max_gain") and name not in DOWNMIX_FIELDS
                and name not in IMMERSIVE_CORRECTIONS):
            continue
        if change is None and first.setdefault(name, value) != value:
            change = frame
    return first, change, frames


def cdmx_values(trace):
    """The custom downmix data of the first frame that sends some, {out_ch_config: {field: code}}
    with the codes cdmx_parameters() assigns rather than reads (Part 2 6.2.9.7 to 6.2.9.10), and
    the frame at which a frame first sends other data (None where none does)."""
    by_frame = {}
    for line in Path(trace).read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) != 6 or fields[5] not in CDMX_FIELDS:
            continue
        configs = by_frame.setdefault(int(fields[0]), [])
        if fields[5] == "out_ch_config":
            configs.append({"out_ch_config": int(fields[4])})
        elif configs:
            configs[-1][fields[5]] = int(fields[4])
    first = change = None
    for frame in sorted(by_frame):
        cdmx = {}
        for config in by_frame[frame]:
            if (config.get("b_top_front_to_front") == 1 or config.get("b_top_front_to_side") == 0
                    or config.get("b_top_to_front") == 1 or config.get("b_top_to_side") == 0):
                config.setdefault("gain_t2b_code", 7)
            if config.get("b_top_back_to_front") == 1 or config.get("b_top_back_to_side") == 0:
                config.setdefault("gain_t2e_code", 7)
            cdmx[config["out_ch_config"]] = config
        if first is None:
            first = cdmx
        elif cdmx != first and change is None:
            change = frame
    return first or {}, change


def decodable(leg):
    # G0's manifest records no immersive codec mode; every 5.1.4 leg DEE writes has one of them.
    immersive = (leg.get("immersive_codec_mode") in (None, *IMMERSIVE_MODES)
                 and leg.get("output_channel_layout") in IMMERSIVE_SOURCES)
    return leg.get("frame_rate_index") in RESAMPLING and (
        immersive or (leg.get("codec_mode") in CODEC_MODES
                      and leg.get("output_channel_layout") in LAYOUTS))


def legs_committed():
    manifest = json.loads((BASELINE_DIR / "ac4-manifest.json").read_text(encoding="utf-8"))
    return [(name, BASELINE_DIR / name / "dee.ac4", leg["output_channel_layout"])
            for name, leg in sorted(manifest["legs"].items()) if decodable(leg)]


def legs_gold(gold, g1):
    """The gold set's legs under `legs`, and with `g1` the G1 legs G1_LEGS names."""
    manifest = json.loads((gold / "gold-manifest.json").read_text(encoding="utf-8"))
    legs = [(name, gold / "streams" / name / "dee.ac4", leg["output_channel_layout"])
            for name, leg in sorted(manifest["legs"].items()) if decodable(leg)]
    if g1:
        g1_legs = manifest.get("g1_legs", {})
        missing = [name for name in G1_LEGS if name not in g1_legs]
        if missing:
            raise SystemExit(f"G1_LEGS names legs the manifest's g1_legs lacks: {missing}")
        legs += [(name, gold / "streams" / name / "dee.ac4", g1_legs[name]["output_channel_layout"])
                 for name in G1_LEGS if decodable(g1_legs[name])]
    return legs


def write_wav_f32(path, samples, rate):
    """An IEEE float WAV of `samples`, shape (n, channels)."""
    data = np.asarray(samples, dtype="<f4").tobytes()
    channels = samples.shape[1]
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 3, channels, rate, rate * 4 * channels,
                                    4 * channels, 32)
    Path(path).write_bytes(header + b"data" + struct.pack("<I", len(data)) + data)


def legs_encoder(cli, work):
    """ENCODER_LEGS encoded from tones by `ac3cli ac4-encode`: (name, stream, dialogue)."""
    rate = 48000
    t = np.arange(ENCODER_SECONDS * rate) / rate
    legs = []
    for name, channels, kbps, options, dialogue in ENCODER_LEGS:
        hz = TONES_HZ if channels > 2 else (TONES_HZ[0], TONES_HZ[1])[:channels]
        if channels == 5:
            hz = (TONES_HZ[L], TONES_HZ[R], TONES_HZ[C], TONES_HZ[LS], TONES_HZ[RS])
        if dialogue == "mid":
            hz = (hz[0], hz[0])
        wav = work / f"{name}-in.wav"
        write_wav_f32(wav, np.stack([0.1 * np.sin(2.0 * np.pi * f * t) for f in hz], axis=1), rate)
        stream = work / f"{name}.ac4"
        command = [str(cli), "ac4-encode", str(wav), str(stream), str(kbps), *options, "quiet"]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise SystemExit(f"{name}: ac3cli ac4-encode failed ({result.returncode}):\n"
                             f"{result.stdout}{result.stderr}")
        legs.append((name, stream, dialogue))
    return legs


def decode(cli, stream, out_wav, *options):
    command = [str(cli), "decode", str(stream), str(out_wav), *options]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"{stream}: ac3cli decode {' '.join(options)} failed "
                         f"({result.returncode}):\n{result.stdout}{result.stderr}")
    samples, _ = read_wav(out_wav)
    return samples


def residual_db(expected, got):
    """How far under `expected` what separates it from `got` is, in dB; -inf where nothing does."""
    error = float(np.sum((got - expected) ** 2))
    if error == 0.0:
        return float("-inf")
    return 10.0 * np.log10(error / max(float(np.sum(expected**2)), 1e-300))


def check_renders(cli, name, stream, layout, values, cdmx, skip, end, work, coded, failures):
    """The render check of an immersive leg (the module's docstring), in full and core decoding."""
    source = IMMERSIVE_LAYOUTS[layout][0]
    # DEE's 5.1.4 and 7.1.4 legs send all four top channels, and the backs where 7.1.4 has them.
    backs, tops, lfe = source.startswith("7"), 3, True
    for core in (False, True):
        mode = ("decoding=core",) if core else ()
        as_coded = decode(cli, stream, work / f"{name}-core.wav", *mode) if core else coded
        cells = []
        for options, matrix in render_checks(source, backs, tops, lfe, values, cdmx, core):
            label = " ".join(options)
            out = decode(cli, stream, work / f"{name}-render.wav", *options, *mode)
            if out.shape[1] != matrix.shape[0] or as_coded.shape[1] != matrix.shape[1]:
                failures.append(f"{name}: {label}{' core' if core else ''} gives {out.shape[1]} "
                                f"channels from {as_coded.shape[1]}, the matrix "
                                f"{matrix.shape[0]} from {matrix.shape[1]}")
                continue
            expected = (as_coded @ matrix.T)[skip:end]
            got = out[skip:end]
            left = residual_db(expected, got)
            worst = 0.0
            for c in range(got.shape[1]):
                energy = float(np.sum(expected[:, c] ** 2))
                if energy > 1e-6 * len(expected):
                    gain_db = db(abs(float(np.sum(expected[:, c] * got[:, c])) / energy))
                    worst = max(worst, abs(gain_db))
            cells.append(f"{label.removeprefix('speakers=')} {worst:.4f} dB {left:.0f} dB")
            if worst > RENDER_TOLERANCE_DB or left > DOWNMIX_RESIDUAL_DB:
                failures.append(f"{name}: {label}{' in core decoding' if core else ''} is "
                                f"{worst:.4f} dB and {left:.1f} dB from Part 2's renderer with the "
                                "stream's values")
        print(f"{'':<40} render ({'core' if core else 'full'}) {'  '.join(cells)}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="the ac3cli to decode with")
    parser.add_argument("--gold", type=Path, help="check G0's local gold set in this directory")
    parser.add_argument("--g1", action="store_true",
                        help="with --gold, check the G1 legs G1_LEGS names as well")
    parser.add_argument("--encoder", action="store_true",
                        help="check streams ac4-encode writes here (ENCODER_LEGS)")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument("--only", nargs="+", metavar="LEG", help="check only these legs")
    args = parser.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as temporary:
        work = args.work or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        if args.encoder:
            legs = [(name, stream, dialogue, None)
                    for name, stream, dialogue in legs_encoder(args.cli, work)]
        else:
            legs = [(name, stream, None, layout) for name, stream, layout in
                    (legs_gold(args.gold, args.g1) if args.gold else legs_committed())]
        if args.only:
            legs = [leg for leg in legs if leg[0] in args.only]
        if not legs:
            raise SystemExit("no leg to check")
        for name, stream, dialogue, layout in legs:
            trace = work / f"{name}.trace"
            coded = decode(args.cli, stream, work / f"{name}.wav", f"syntax-trace={trace}")
            values, change, frames = stream_values(trace)
            cdmx, cdmx_change = cdmx_values(trace)
            if cdmx_change is not None:
                change = cdmx_change if change is None else min(change, cdmx_change)
            if "dialnorm_bits" not in values:
                failures.append(f"{name}: no dialnorm_bits in its syntax trace")
                continue
            # At least SKIP_FRAMES frames at every rate: none is longer than 2 048 samples. Where
            # the stream's values change (DEE's immersive stereo at 24 and 25 fps sends another
            # dialnorm in its last frame), the checks stop SKIP_FRAMES frames before it.
            skip = SKIP_FRAMES * 2048
            end = coded.shape[0]
            if change is not None:
                end = change * coded.shape[0] // frames - skip
            if end <= 2 * skip:
                failures.append(f"{name}: its values change at frame {change}, too soon to check")
                continue
            dialnorm = -values["dialnorm_bits"] / 4.0
            cells = [] if change is None else [f"(to frame {change})"]
            for lout in OUTPUT_LEVELS:
                out = decode(args.cli, stream, work / f"{name}-level.wav", f"output-level={lout:g}",
                             "drcmode=off")
                a, b = coded[skip:end], out[skip:end]
                gain_db = db(float(np.sum(a * b) / np.sum(a * a)))
                expected_db = db(2.0 ** ((lout - dialnorm) / 6.0))
                left = residual_db(from_db(gain_db) * a, b)
                cells.append(f"{lout:g}: {gain_db - expected_db:+.4f} dB, {left:.0f} dB")
                if abs(gain_db - expected_db) > LEVEL_TOLERANCE_DB:
                    failures.append(f"{name}: output-level={lout:g} gives {gain_db:+.3f} dB, "
                                    f"2^((Lout - dialnorm) / 6) {expected_db:+.3f} dB")
                if left > LEVEL_RESIDUAL_DB:
                    failures.append(f"{name}: output-level={lout:g} leaves {left:.1f} dB beside "
                                    "its gain")
            print(f"{name:<40} dialnorm {dialnorm:6.2f}  level {'  '.join(cells)}", flush=True)
            if dialogue is not None:
                cap = 3.0 * (values.get("de_max_gain", 0) + 1)
                cells = []
                for gain in DE_GAINS:
                    out = decode(args.cli, stream, work / f"{name}-de.wav",
                                 f"dialogue-enhancement={gain:g}")
                    worst = 0.0
                    for c in range(coded.shape[1]):
                        a, b = coded[skip:end, c], out[skip:end, c]
                        got_db = db(float(np.sum(a * b) / np.sum(a * a)))
                        raised = c < 2 if dialogue == "mid" else c in dialogue
                        expected_db = min(gain, cap) if raised else 0.0
                        left = residual_db(from_db(got_db) * a, b)
                        worst = max(worst, abs(got_db - expected_db))
                        if abs(got_db - expected_db) > DE_TOLERANCE_DB:
                            failures.append(f"{name}: dialogue-enhancement={gain:g} gives channel "
                                            f"{c} {got_db:+.3f} dB, expected {expected_db:+.3f}")
                        if left > DE_RESIDUAL_DB:
                            failures.append(f"{name}: dialogue-enhancement={gain:g} leaves "
                                            f"{left:.1f} dB beside channel {c}'s gain")
                    cells.append(f"{gain:g}: within {worst:.4f} dB")
                print(f"{'':<40} dialogue enhancement (cap {cap:g} dB) {'  '.join(cells)}",
                      flush=True)
            if layout in IMMERSIVE_SOURCES:
                check_renders(args.cli, name, stream, layout, values, cdmx, skip, end, work, coded,
                              failures)
                shown = {k: v for k, v in values.items()
                         if k in DOWNMIX_FIELDS + IMMERSIVE_CORRECTIONS}
                print(f"{'':<40} values {shown}  custom downmix data {cdmx}", flush=True)
                continue
            if coded.shape[1] == 5:
                # 5.0: the matrix's LFE column meets silence.
                coded = np.insert(coded, LFE, 0.0, axis=1)
            elif coded.shape[1] != 6:
                continue
            downmix = values.copy()
            cells = []
            for target, options in (("stereo", ("channels=2",)), ("loro", ("downmix=loro",)),
                                    ("ltrt", ("downmix=ltrt",)), ("mono", ("downmix=mono",))):
                out = decode(args.cli, stream, work / f"{name}-{target}.wav", *options)
                matrix = stereo_matrix(downmix, target)
                if out.shape[1] != matrix.shape[0]:
                    failures.append(f"{name}: {target} gives {out.shape[1]} channels, not "
                                    f"{matrix.shape[0]}")
                    continue
                expected = coded @ matrix.T
                left = residual_db(expected[skip:end], out[skip:end])
                cells.append(f"{target} {left:.0f} dB")
                if left > DOWNMIX_RESIDUAL_DB:
                    failures.append(f"{name}: {target} leaves {left:.1f} dB beside clause 6.2.17's "
                                    "matrix with the stream's values")
            shown = {k: downmix[k] for k in DOWNMIX_FIELDS if k in downmix}
            print(f"{'':<40} downmix {'  '.join(cells)}  {shown}", flush=True)
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{len(legs)} legs, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
