"""Local-only reference baseline vs FFmpeg's and Dolby DEE's encoders.

This is the one-time (or occasional) step that produces the checked-in
numbers CI's `quality_race.py trend` mode compares every build against.
DEE (Dolby Encoding Engine, bundled in "Dolby Media Encoder" - not the
decode-only Dolby Reference Player used elsewhere) is licensed commercial
software and must never run unattended in CI; FFmpeg's *encoder* is open
but is likewise kept out of the per-commit path so a build never depends on
either tool being present. Run this locally, review the manifest diff it
produces, commit it.

Each score also carries a MOS-LQO prediction (score_tool(), via
quality_race.py's perceptual_score()/ViSQOL) when `visqol-python` is
installed in the environment this runs in - "mos_lqo": null in the manifest
otherwise, same graceful-degradation contract as everywhere else this
project uses it. Installing it here is a one-time, local, opt-in choice by
whoever regenerates the baseline; it is never required to run this script.

Eight fixed legs (baseline_version 2; the first three were the whole set at
version 1 and are unchanged, so their published series stay continuous):

  ac3-51-448             AC-3,   5.1,    448 kbps, reference_51.wav
  eac3-stereo-192        E-AC-3, stereo, 192 kbps, reference_stereo.wav
  eac3-51-256            E-AC-3, 5.1,    256 kbps, reference_51.wav
  eac3-stereo-96         E-AC-3, stereo,  96 kbps, reference_stereo.wav
  eac3-stereo-64         E-AC-3, stereo,  64 kbps, reference_stereo.wav
  ac3-music-stereo-192   AC-3,   stereo, 192 kbps, programme_music_stereo.flac
  eac3-music-stereo-96   E-AC-3, stereo,  96 kbps, programme_music_stereo.flac
  eac3-speech-stereo-64  E-AC-3, stereo,  64 kbps, programme_speech_stereo.flac

The two new synthetic stereo legs exist because the Annex E tools were never
being compared against FFmpeg or DEE at a rate where they run. `auto` enables
coupling below 12 + 14n kbit/s per channel and spectral extension below 56
(eac3_frame.cpp's coupling_rate_ceiling/kSpxRateCeiling), and the only stereo
leg sat at 96 per channel - above both - so every published stereo comparison
was of an encoder that had chosen no tools at all. 96 kbps total is 48 per
channel (spectral extension only) and 64 is 32 (both).

The three programme legs are the other half of the same problem: the first
five are all 2.5-3 s of sin()/noise/FIR, which carries a flat noise plateau
across its whole top octave and has already produced one measured, fake
2.1 dB "win" (src/forge/src/encoder/encoder.cpp's chbwcod comment). See
tools/generators/gen_programme_fixtures.py for the sources, licences and
measured spectra. Those fixtures ship as FLAC and are materialised to WAV
under build/ by quality_race.py's materialise_fixture(), which is what the
`wav` key below goes through.

For each leg: encode with FFmpeg, encode with DEE, encode with the current
build's ac3cli (a sanity check, not the point of this script - CI's `trend`
mode re-encodes with whatever build is under test).

Scoring decodes FFmpeg's and DEE's output with FFmpeg's own decoder (its
established role everywhere else in this project - see decode_scores'
"neutral referee" docstring), and ac3cli's own output with ac3cli's own
decoder (self-consistency, the same pattern decode_scores_ours uses
elsewhere). This is a deliberate change from decoding every bitstream with
ac3cli's own decoder: FFmpeg's encoder turns out to legally choose Table
E2.12's default coupling band structure (cplbndstrce=0), which this
project's own decoder declines to read (eac3_decoder.cpp - no stream this
project's own encoder produces ever sets that flag, so it was never
exercised against one that does - see the tracked follow-up to fix this and
add CI coverage decoding real third-party output). Scoring external
encoders' output by how well *our* decoder happens to parse their bitstream
choices would also conflate their encoder's quality with our decoder's
coverage - using FFmpeg for both avoids that confound entirely, not just
the crash. (The Dolby Reference Player was also tried here, as a genuine
reference-implementation oracle, and appeared to decode DEE's own E-AC-3
stereo output to near-total garbage while FFmpeg decoded the identical file
to a sane score. That has since been run down and it is not a decode defect
at all - see "Resolved" below.)

Writes tests/golden/external-baseline/<leg>/{ffmpeg,dee}.<ac3|ec3> (the raw
bitstreams, committed) and one tests/golden/external-baseline/manifest.json
(the numbers CI reads). Bump BASELINE_VERSION by hand before rerunning this
to regenerate the baseline against a new DEE/FFmpeg release - it is the
only marker distinguishing one baseline generation from the next in a PR
diff.

Resolved at baseline_version 2: DEE's two 5.1 legs used to score as
"unverified" rather than a number. This installed DEE build
(v6.5.4-dme+b56bc97e) reproducibly drops the Ls (surround-left) channel's
content when 5.1 arrives as ONE discrete 6-channel file, confirmed with a
per-channel tone probe: the silence was locked to channel index 3 whatever
content was placed there, independent of WAV-vs-raw-PCM input, dd-vs-ddp
codec, and the surround_90deg_phase_shift flag.

The fix is DEE's other documented input path. `--input-format wav_list`
takes one mono WAV per channel ("A list of mono WAVE files, one for each
channel, in order of L:R:C:LFE:LS:RS:LRS:RRS", per `dee_ddp_encoder
--morehelp input-format`) and does not lose Ls. Verified by re-measuring
per-channel RMS through the full encode/decode: 6983/6983/3146/7282/700/1151
out against 7025/7025/3165/7517/717/1174 in, every channel present including
index 4. That list order is also this project's own WAV channel order
(FL FR FC LFE BL BR - ac3::io::ac3_layout_for), unlike the SMPTE order the
single-multichannel path wants, so split_for_dee() below needs no
permutation at all where reorder_for_dee() needed _SMPTE_FROM_WAVE_51. The
single-file path is gone rather than kept as a fallback: it is the one that
loses a channel.

Two of DEE's own production defaults had to come off at the same time, for
the same reason `measure_only`/`drc_profile=none` were already off - see
_DEE_ENCODER_OPTS for the measured cost of each. The surround one is worth
knowing about generally: DEE phase-shifts Ls/Rs by 90 degrees by default,
which preserves their magnitude spectrum exactly and decorrelates their
waveform completely. A per-channel RMS check passes; SNR reads -2.9 dB. That
is a failure mode that looks like a dropped channel from one angle and like
a broken encoder from another, and is neither.

Still unverified for DEE: both 64 kbit/s stereo legs, because DEE's stereo
Dolby Digital Plus data rate starts at 96 - see UNVERIFIED_DEE_LEGS.

Known issue: FFmpeg cannot decode the FIRST frame of DEE's stereo E-AC-3
stream from cold, which is why eac3-stereo-192's DEE entry carries a
"decoder_note" (see DECODER_NOTES below). FFmpeg 8.0.1 reads 93 of that
stream's 94 frames cleanly - a median 44 dB per frame - and fails only frame
0, reporting "exponent 25 is out-of-range" once and concealing it by
repeating block 0 across blocks 1-4. It is the frame's own shape that FFmpeg
cannot read cold, not its position: frames 0, 75 and 89 share a payload
shape, all three fail when made the first frame, and frame 0 decodes with no
error at all when any other frame is prepended ahead of it. Frames 75 and 89
therefore decode correctly in place, at 51.0 and 42.9 dB, because by then
there is decoder state to fall back on.

This does not move the number recorded here, and that is not a lucky
accident worth hiding: score_fixed aligns on FIXED_ALIGN, whose skip is
0.2 s, so the first 9600 samples - the only ones FFmpeg gets wrong - are
outside the scored window for every leg alike. Scored across the whole file
instead, FFmpeg's decode of this fixture is 14.30 dB rather than 33.32 dB,
and the entire difference is that one frame. The reason the 33.32 dB is
nonetheless the right number for DEE's ENCODER is independent corroboration,
not the window: ac3forge's own decoder reads frame 0 correctly (42.30 dB on
it) and scores the same fixture 33.3236 dB through the same window - 0.005 dB
from FFmpeg's 33.3186 dB. Two independent decoders agreeing that closely is
what says this measures DEE's encoder rather than either decoder's
concealment. (That cross-check needs the Annex E decoder fixes that arrive
with the third-party interop gate, PR #320 - before them this project's own
decoder could not read DEE's Annex E syntax at all, which is why no earlier
baseline run could have made this comparison.)

Resolved: the Dolby Reference Player's apparent "near-total garbage" on this
same stereo stream, noted above and left open until now, is a level shift
and not a decode defect. DEE writes a MEASURED dialnorm of 12 (dialogue at
-12 dBFS) because invoke_dee asks for loudness-management measure_only; the
reference player then does what a player is supposed to do and normalises
dialogue to -31 dBFS, attenuating the output by exactly 31 - 12 = 19 dB.
Neither FFmpeg's decoder nor ac3cli's applies dialnorm, and the scoring here
compares against an un-normalised source WAV, so the player's output scored
1.02 dB. Undo that 19 dB and the same decode scores 32.19 dB, with an
LSD of 1.878 sitting between FFmpeg's 1.899 and ac3cli's 1.857 - a good
decode all along. FFmpeg's own encoder writes dialnorm 31, a 0 dB shift,
which is the only reason its stream looked fine through the same pipeline.
The player is therefore usable as an oracle, provided dialnorm is either
compensated for or encoded as 31.

Usage (repo root, after building ac3cli):  python tools/generators/gen_external_baseline.py
Set AC3CLI to override the ac3cli binary, same as quality_race.py.
"""

import datetime
import json
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# quality_race.py lives in the sibling tools/ci/ directory (CI-orchestration
# bucket), not here (tools/generators/, table/fixture-generator bucket) - it
# just also happens to be where its own CLI/WAV-IO/scoring helpers live.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "ci"))
from quality_race import (
    CLI,
    decode_scores_ours_fixed,
    materialise_fixture,
    measured_kbps,
    read_wav_any,
    read_wav_f32,
    run,
    score_fixed,
)

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO = REPO / "tests" / "golden" / "audio"
OUT = REPO / "tests" / "golden" / "external-baseline"
SCRATCH = REPO / "build" / "external_baseline_scratch"

DEE = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir\dee_ddp_encoder.exe")

# Bump by hand each time this script is rerun to regenerate the baseline
# against a new DEE or FFmpeg release - the manifest diff this produces is
# meant to be reviewed like any other change, not silently overwritten.
#
# 2: DEE's 5.1 legs stopped being "unverified" (split_for_dee below), five
#    legs were added (see the module docstring), and every score gained a
#    real mos_lqo - version 1 was generated in an environment without
#    visqol-python, so every MOS delta in docs/landscape.md read n/a.
BASELINE_VERSION = 2

# Legs DEE cannot produce a trustworthy number for. Both 5.1 legs used to be
# here for a reason that turned out to be fixable (see the module docstring);
# what is left is a hard limit of the tool rather than a workaround waiting to
# be found.
#
# DEE's stereo Dolby Digital Plus data rate starts at 96 kbit/s
# (`dee_ddp_encoder --morehelp data-rate`: "Stereo [96-1024]"), so it simply
# declines 64 - "ERROR: Requested data rate '64' is not supported by the
# selected codec". That is the whole rate band below coupling's stereo
# crossover (12 + 14n = 40 kbit/s per channel, i.e. 80 total), so DEE cannot
# be an external reference for ANY stereo leg where this project's coupling
# runs. FFmpeg still can, and does, on both of these legs.
#
# Unlike the 5.1 case, no dee.ec3 is written for these: there is nothing to
# write, so main() skips the invocation rather than letting it fail.
UNVERIFIED_DEE_LEGS = {
    "eac3-stereo-64": "DEE's stereo Dolby Digital Plus data rate starts at 96 kbit/s "
                      "(dee_ddp_encoder --morehelp data-rate); 64 is refused outright.",
    "eac3-speech-stereo-64": "DEE's stereo Dolby Digital Plus data rate starts at 96 kbit/s "
                             "(dee_ddp_encoder --morehelp data-rate); 64 is refused outright.",
}

_SPEECH = AUDIO / "programme_speech_stereo.flac"
_MUSIC = AUDIO / "programme_music_stereo.flac"

# Mirrored by hand in quality_race.py's TREND_LEGS - see that list's own
# comment, and this file's docstring for what each leg is for. `wav` may be a
# FLAC; materialise_fixture() turns it into a WAV path.

# Attached to a leg's score entry as "decoder_note", keyed (leg, tool). The
# same hand-maintained, regenerate-stable mechanism as UNVERIFIED_DEE_LEGS
# above, for the other case: a leg whose number is sound but whose
# provenance needs a caveat recorded beside it rather than left to whoever
# next reads the manifest and tries to reproduce it. Unlike "status":
# "unverified" this does not suppress the number - every consumer keeps
# reading snr_db exactly as before (append_external_comparison_history.py
# gates on "snr_db" in entry, render_spectrograms on "status"), so this is
# additive to them.
DECODER_NOTES = {
    ("eac3-stereo-192", "dee"): (
        "FFmpeg 8.0.1 reads 93 of this stream's 94 frames cleanly but fails "
        "frame 0 from cold (\"exponent 25 is out-of-range\"), concealing it by "
        "repeating block 0 across blocks 1-4. The score here is unaffected "
        "because score_fixed's FIXED_ALIGN skips the first 0.2 s, which is "
        "where that frame sits; scored across the whole file FFmpeg's decode "
        "is 14.30 dB instead. What says 33.32 dB measures DEE's encoder and "
        "not FFmpeg's concealment is that ac3forge's own decoder reads frame 0 "
        "correctly and scores this same fixture 33.3236 dB through the same "
        "window, 0.005 dB away - see tools/generators/gen_external_baseline.py's "
        "module docstring."
    ),
}

LEGS = [
    {"name": "ac3-51-448", "codec": "ac3", "ext": "ac3", "dee_codec": "dd",
     "ffmpeg_codec": "ac3", "dee_layout": "5.1", "kbps": 448,
     "wav": AUDIO / "reference_51.wav"},
    {"name": "eac3-stereo-192", "codec": "eac3", "ext": "ec3", "dee_codec": "ddp",
     "ffmpeg_codec": "eac3", "dee_layout": "stereo", "kbps": 192,
     "wav": AUDIO / "reference_stereo.wav"},
    {"name": "eac3-51-256", "codec": "eac3", "ext": "ec3", "dee_codec": "ddp",
     "ffmpeg_codec": "eac3", "dee_layout": "5.1", "kbps": 256,
     "wav": AUDIO / "reference_51.wav"},
    {"name": "eac3-stereo-96", "codec": "eac3", "ext": "ec3", "dee_codec": "ddp",
     "ffmpeg_codec": "eac3", "dee_layout": "stereo", "kbps": 96,
     "wav": AUDIO / "reference_stereo.wav"},
    {"name": "eac3-stereo-64", "codec": "eac3", "ext": "ec3", "dee_codec": "ddp",
     "ffmpeg_codec": "eac3", "dee_layout": "stereo", "kbps": 64,
     "wav": AUDIO / "reference_stereo.wav"},
    {"name": "ac3-music-stereo-192", "codec": "ac3", "ext": "ac3", "dee_codec": "dd",
     "ffmpeg_codec": "ac3", "dee_layout": "stereo", "kbps": 192, "wav": _MUSIC},
    {"name": "eac3-music-stereo-96", "codec": "eac3", "ext": "ec3", "dee_codec": "ddp",
     "ffmpeg_codec": "eac3", "dee_layout": "stereo", "kbps": 96, "wav": _MUSIC},
    {"name": "eac3-speech-stereo-64", "codec": "eac3", "ext": "ec3", "dee_codec": "ddp",
     "ffmpeg_codec": "eac3", "dee_layout": "stereo", "kbps": 64, "wav": _SPEECH},
]


def guard_not_ci():
    if os.environ.get("GITHUB_ACTIONS"):
        raise SystemExit(
            "gen_external_baseline.py invokes licensed, non-CI-safe tooling "
            "(Dolby DEE) and must never run in a CI job - refusing because "
            "GITHUB_ACTIONS is set.")


def ffmpeg_version():
    # check=False: a missing or broken ffmpeg records "unknown" in the manifest
    # rather than aborting the whole baseline generation.
    result = subprocess.run(["ffmpeg", "-version"], capture_output=True, text=True, check=False)
    return result.stdout.splitlines()[0].strip() if result.returncode == 0 else "unknown"


def dee_version():
    # No --version option; -h's help text carries a "belongs to the Dolby
    # Encoding Engine version X" line instead.
    # check=False: `dee -h` exits non-zero on some builds even though it printed
    # the version banner this parses.
    result = subprocess.run([str(DEE), "-h"], capture_output=True, text=True, check=False)
    for line in (result.stdout or result.stderr).splitlines():
        if "Dolby Encoding Engine version" in line:
            return line.strip()
    return "unknown"


def invoke_ffmpeg(wav, kbps, codec, out):
    run(["ffmpeg", "-v", "error", "-y", "-i", str(wav), "-c:a", codec,
         "-b:a", f"{kbps}k", str(out)])


# DEE's `wav_list` input path: one mono WAV per channel, "in order of
# L:R:C:LFE:LS:RS:LRS:RRS" (`dee_ddp_encoder --morehelp input-format`).
#
# This is the path that works. DEE's other input path - one discrete
# multichannel WAV - silently drops the Ls channel on this build, which is
# what left both 5.1 legs unverified at baseline_version 1; see the module
# docstring for the tone probe that pinned it down and the per-channel RMS
# that confirms this path does not.
#
# It also removes a whole class of mistake rather than just the Ls one. The
# single-file path is documented as reading SMPTE order (L C R Ls Rs LFE)
# where this project's WAVs are Microsoft/WAVE order (FL FR FC LFE BL BR), so
# it needed a permutation table - and re-measured while this was being
# replaced, it does not honour that order either. Feeding it a correctly
# SMPTE-permuted file and decoding the result gives back, in WAVE positions:
#
#   L, C, R, ~silence, Rs, LFE     (rms 0.2131 0.0960 0.2131 0.0073 0.0351 0.2156)
#   against a source of
#   L, R, C, LFE,      Ls, Rs      (rms 0.2144 0.2144 0.0966 0.2294 0.0219 0.0358)
#
# i.e. the input's own order straight through, with Ls gone. So the
# permutation makes it worse rather than better, and turning off the
# surround phase shift and the LFE filter changes none of it - the drop is
# independent of both, exactly as the earlier tone probe reported.
#
# wav_list's documented order IS this project's WAV order, so the split below
# is a plain de-interleave with no permutation to get wrong.
def split_for_dee(wav_path, scratch_dir, tag):
    """Interleaved PCM16 WAV -> one mono WAV per channel; returns DEE's
    colon-joined --input argument."""
    with wave.open(str(wav_path), "rb") as r:
        params = r.getparams()
        raw = r.readframes(r.getnframes())
    ch = params.nchannels
    samples = struct.unpack(f"<{len(raw) // 2}h", raw)
    n = len(samples) // ch
    paths = []
    for c in range(ch):
        out = scratch_dir / f"{tag}_dee_ch{c}.wav"
        with wave.open(str(out), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(params.sampwidth)
            w.setframerate(params.framerate)
            w.writeframes(struct.pack(f"<{n}h", *samples[c::ch]))
        paths.append(str(out))
    return ":".join(paths)


# Every one of these turns OFF a DEE default that changes the signal for
# production reasons rather than coding ones. ac3cli does none of them, so
# leaving them on measures the difference between two mastering policies and
# calls it a difference in encoder quality.
#
# measure_only (rather than DEE's default measure_and_correct, -24 LKFS)
#   applies no loudness gain correction, which would otherwise show up as a
#   level mismatch swamping SNR.
# drc_profile=none
#   sends no dynamic-range control profile.
# surround_90deg_phase_shift=0
#   DEE applies a 90-degree phase shift to Ls/Rs by default, for Dolby
#   Surround Lt/Rt compatibility. It preserves the magnitude spectrum and
#   destroys the waveform, which is exactly what a waveform metric cannot
#   see past: with it on, both surrounds scored -2.9 dB SNR at a correlation
#   of 0.02 against the source - uncorrelated, but level-matched, which is
#   what made it look for a while like DEE was dropping the channel.
# lfe_filter=0
#   DEE low-passes the LFE by default. The 5.1 fixture's LFE is a 55/82.5 Hz
#   tone pair, well inside any LFE passband, and it still cost 18 dB.
#
# Measured on ac3-51-448, total SNR and then per channel:
#
#   defaults                      15.76 | 47.4 47.9 46.3 18.9 -2.9 -2.9
#   + surround_90deg_phase_shift=0 23.47 | 47.3 47.9 46.2 18.9 18.1 19.6
#   + lfe_filter=0                 36.57 | 47.3 47.9 46.2 36.8 18.1 19.6
#
# For reference FFmpeg scores 39.00 and ac3cli 39.95 on the same leg, so the
# last row is the first one that puts DEE in the same conversation. The
# surrounds staying near 18-19 dB is not a residual problem: FFmpeg's own
# are 20.2/20.2, because that fixture's surrounds are band-limited noise and
# are simply the hardest thing in it to code.
_DEE_ENCODER_OPTS = "drc_profile=none:surround_90deg_phase_shift=0:lfe_filter=0"


def invoke_dee(wav_list_arg, kbps, codec, layout, out):
    """codec is "dd" (AC-3) or "ddp" (E-AC-3). See _DEE_ENCODER_OPTS above
    for why each default is turned off and what each one was worth."""
    run([str(DEE), "--input-format", "wav_list", "--input", wav_list_arg,
         "--encoder", f"{codec}:{_DEE_ENCODER_OPTS}",
         "--loudness-management", "measure_only",
         "--data-rate", str(kbps),
         "--output-channel-layout", layout,
         "--overwrite", "1",
         "--output", str(out)])


def invoke_ours(wav, kbps, is_eac3, out):
    if is_eac3:
        # "auto": a bare `eac3-encode` with no tools argument leaves every
        # Annex E tool off by default (FrameConfig's own defaults - see
        # docs/library/encoding-eac3.md's table), which would compare ac3forge
        # with its hands tied against FFmpeg's/DEE's own automatic best-effort
        # tool selection. "auto" is this encoder's own automatic choice from
        # the per-channel rate, which is the like-for-like answer to theirs.
        # AC-3 has no such toggle at all - coupling/rematrix/delta bit
        # allocation are unconditionally automatic there - so only the E-AC-3
        # branch needs this.
        #
        # This said "all" until baseline_version 2, which forced every tool on
        # at every rate. quality_race.py's race_trend() had already moved its
        # own landscape row off "all" for the measured reason in its docstring
        # (at 192 kbit/s stereo the forced set costs about 10 dB of SNR
        # against choosing nothing) and its docstring already claimed this
        # function agreed with it; now it does.
        run([CLI, "eac3-encode", str(wav), str(out), str(kbps), "auto"])
    else:
        run([CLI, "encode", str(wav), str(out), str(kbps)])


def decode_scores_ffmpeg_fixed(original, coded, wav_path, perceptual=False):
    """decode_scores' FFmpeg-decode step, scaled for the short checked-in
    fixtures (see score_fixed) instead of make_material()'s ~10s material -
    not strict (-xerror), since these are foreign encoders' own output, not
    something whose exact bitstream layout this project is checking."""
    run(["ffmpeg", "-v", "error", "-y", "-i", str(coded), "-c:a", "pcm_f32le", str(wav_path)])
    return score_fixed(original, read_wav_f32(wav_path), perceptual=perceptual)


def score_tool(original, coded, wav_scratch, is_eac3, decoder):
    """decoder: "ours" (ac3cli's own decoder) or "ffmpeg" (FFmpeg's own
    decoder - used for FFmpeg's own output and DEE's, matching FFmpeg's
    established "neutral referee" role elsewhere in this project).

    The Dolby Reference Player (dolby_decode) was tried here too, since it
    is a genuine reference-implementation oracle when it works, and appeared
    to produce near-total garbage (1.02 dB SNR) decoding DEE's own E-AC-3
    stereo output while FFmpeg decoded the identical file to ~33 dB. It is
    not a "decoder", though: it is a PLAYER, and it applies dialnorm. DEE
    writes a measured dialnorm of 12 on that stream, so the player correctly
    attenuates by 31 - 12 = 19 dB, and scoring its output against an
    un-normalised source WAV charges the whole 19 dB to the decode. Undo it
    and the same decode scores 32.19 dB. FFmpeg is not "better" here, only
    dialnorm-blind, and FFmpeg's own encoder writes dialnorm 31 (a 0 dB
    shift) which is why its stream looked fine either way. Kept on FFmpeg
    regardless, for the neutral-referee reason above rather than because the
    player cannot do it - but a caller that wants the player as an oracle has
    to compensate dialnorm or encode it as 31. See the module docstring.

    Requests a MOS-LQO score too (perceptual_score() in quality_race.py) -
    unlike lsd_db/hf_db it isn't gated on is_eac3, since ViSQOL scores
    perceived quality in general rather than a specific Annex E tool's
    banded envelope. None when visqol-python isn't installed, same
    graceful-degradation contract as everywhere else it's used - this
    script staying runnable without it matters here too, DEE alone is
    already the hard local-only requirement.
    """
    if decoder == "ours":
        snr, lsd, hf, mos = decode_scores_ours_fixed(original, coded, wav_scratch, perceptual=True)
    else:
        snr, lsd, hf, mos = decode_scores_ffmpeg_fixed(original, coded, wav_scratch,
                                                       perceptual=True)
    return {
        "snr_db": float(snr),
        "lsd_db": float(lsd) if is_eac3 else None,
        "hf_db": float(hf) if is_eac3 else None,
        "mos_lqo": None if mos is None else float(mos),
        "decoded_with": decoder,
    }


def main():
    guard_not_ci()

    if not DEE.exists():
        raise SystemExit(f"Dolby DEE not found at {DEE} - install Dolby Media Encoder "
                          "or fix this path before running the baseline.")

    OUT.mkdir(parents=True, exist_ok=True)
    SCRATCH.mkdir(parents=True, exist_ok=True)

    manifest = {
        "baseline_version": BASELINE_VERSION,
        "generated_date": datetime.date.today().isoformat(),
        "tools": {
            "ffmpeg": {"version": ffmpeg_version()},
            "dee": {"version": dee_version()},
        },
        "legs": {},
    }

    print(f"{'leg':<18} | {'tool':<20} | {'SNR dB':>7} | {'LSD dB':>6} | "
          f"{'HF dB':>6} | {'MOS':>4} | {'kbps':>6}")
    print("-" * 85)

    for leg in LEGS:
        name, codec, ext, kbps = leg["name"], leg["codec"], leg["ext"], leg["kbps"]
        is_eac3 = codec == "eac3"
        wav = materialise_fixture(leg["wav"])
        original = read_wav_any(wav)
        seconds = len(original) / 48000.0

        leg_dir = OUT / name
        leg_dir.mkdir(parents=True, exist_ok=True)

        ffmpeg_out = leg_dir / f"ffmpeg.{ext}"
        dee_out = leg_dir / f"dee.{ext}"
        ours_out = SCRATCH / f"{name}_ours.{ext}"

        invoke_ffmpeg(wav, kbps, leg["ffmpeg_codec"], ffmpeg_out)
        if name not in UNVERIFIED_DEE_LEGS:
            invoke_dee(split_for_dee(wav, SCRATCH, name), kbps, leg["dee_codec"],
                       leg["dee_layout"], dee_out)
        invoke_ours(wav, kbps, is_eac3, ours_out)

        scores = {}
        for tool_label, coded, decoder in (
            ("ffmpeg", ffmpeg_out, "ffmpeg"),
            ("dee", dee_out, "ffmpeg"),
            ("ours_at_baseline_time", ours_out, "ours"),
        ):
            if tool_label == "dee" and name in UNVERIFIED_DEE_LEGS:
                scores[tool_label] = {"status": "unverified", "reason": UNVERIFIED_DEE_LEGS[name]}
                print(f"{name:<18} | {tool_label:<20} | {'unverified':>7} | "
                      f"{'-':>6} | {'-':>6} | {'-':>4} | {'-':>6}")
                continue
            wav_scratch = SCRATCH / f"{name}_{tool_label}.wav"
            entry = score_tool(original, coded, wav_scratch, is_eac3, decoder)
            entry["measured_kbps"] = measured_kbps(coded, seconds)
            note = DECODER_NOTES.get((name, tool_label))
            if note is not None:
                entry["decoder_note"] = note
            scores[tool_label] = entry
            lsd_str = "-" if entry["lsd_db"] is None else f"{entry['lsd_db']:.2f}"
            hf_str = "-" if entry["hf_db"] is None else f"{entry['hf_db']:+.1f}"
            mos_str = "-" if entry["mos_lqo"] is None else f"{entry['mos_lqo']:.2f}"
            print(f"{name:<18} | {tool_label:<20} | {entry['snr_db']:>7.2f} | "
                  f"{lsd_str:>6} | {hf_str:>6} | {mos_str:>4} | {entry['measured_kbps']:>6.1f}")

        manifest["legs"][name] = {
            "codec": codec,
            "layout": leg["dee_layout"],
            "bitrate_kbps": kbps,
            "source_wav": str(wav.relative_to(REPO)).replace("\\", "/"),
            "scores": scores,
        }
        print()

    manifest_path = OUT / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {manifest_path} (baseline_version {BASELINE_VERSION})")
    print("Reminder: docs/verification.md carries a static dated snapshot of these "
          "numbers too - refresh it by hand, it will not pick this up automatically.")


if __name__ == "__main__":
    main()
