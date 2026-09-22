"""Quality race: our encoder vs FFmpeg's, at matched bitrates.

Synthesizes stereo program material (tones with vibrato, a sweep, filtered
noise, correlated near-mono content for rematrixing, tone bursts), encodes it
with both encoders, decodes both with FFmpeg (the neutral referee), aligns by
cross-correlation, and reports SNR vs the original - plus a MOS-LQO column
(ViSQOL audio mode, see perceptual_score()'s own docstring for why ViSQOL
over PEAQ and which package) on the modes that print or export a table.
Gracefully "-" when `visqol-python` isn't installed - it never becomes a
required dependency just to run a quality race.

Modes:
  ac3        - our AC-3 encoder vs FFmpeg's, at 192-448 kbps
  fast-mdct  - direct-form (fast-mdct=off) vs the default §7.9.4 fast
               forward MDCT (EncoderConfig::fast_mdct), both decoded by
               FFmpeg - the quality evidence that made fast the default
  eac3       - our E-AC-3 encoder, one row per Annex E tool set, vs FFmpeg's
               E-AC-3 encoder, at the low rates the tools exist to serve
  eac3-51    - the same for 5.1, with genuinely decorrelated channels
  fgaincod   - roadmap EQ7's E-AC-3 half: whether §7.2.2.4's measured
               fast-gain curve still wins once E-AC-3 charges for the
               per-block fgaincode element §8.2.12's implied 0x4 avoids.
               Five legs a rate: the default, the default pinned
               explicitly (a byte-identical zero-noise control), the curve,
               and search=distortion with fgaincod pinned and free - the
               last pair separating what EQ7's second axis adds from what
               EQ13's one-axis search was already worth. Scored per time
               window, so every delta carries a paired standard error. `--with-51` adds the coupled
               5.1 legs on synthetic material; `--json-out PATH` writes the
               rows. See race_fgaincod().
  vbr        - the E-AC-3 rate-distortion curve VBR shipped without: sweeps
               VbrConfig.quality, measures what rate each point actually
               costs, and scores CBR and FFmpeg CBR at that same measured
               rate - so "does VBR at a given average rate beat CBR at that
               rate" has a number rather than a warning. Also checks the
               average-rate (ABR) mode lands on the rate it was asked for.
               See race_vbr(); `--json-out PATH` writes the rows as JSON.
  seam       - where the spectral extension notch lands, and how deep
  crosscheck - every tool set through BOTH decoders: FFmpeg and Dolby's own,
               via the reference player's GStreamer elements. Agreeing with
               one decoder says a stream is readable; agreeing with the
               reference implementation says it is right. Skips gracefully
               when the reference player is not installed.
  ci         - the CI gate: AC-3 and every E-AC-3 tool variant (stereo and
               5.1) against a numeric SNR/LSD floor per variant, real
               non-zero exit on regression. No table to read; see race_ci().
               Enhanced coupling and transient pre-noise processing are
               included too, scored through this project's own decoder
               rather than FFmpeg's (see decode_scores_ours) - neither tool
               has an external oracle at all.
  trend      - the CI-time half of the external-encoder landscape
               comparison (tools/generators/gen_external_baseline.py is the local-only
               half that actually invokes FFmpeg/DEE): encodes every
               committed fixed leg with THIS build and scores everything
               through this project's own decoder, so it needs neither
               FFmpeg's nor DEE's own encoder. Compute-only, no gate; see
               race_trend(). `--json-out PATH` writes the rows as JSON.
               `--spectrogram-dir PATH` additionally renders one PNG per leg
               (original/ac3forge/FFmpeg/DEE spectrograms) via
               render_spectrograms() - this part DOES need an `ffmpeg`
               binary, only ever to decode the already-committed
               tests/golden/external-baseline/ bitstreams, never to encode.
  objects    - object-reconstruction quality: the committed five-object
               scene (tests/golden/audio/reference_objects.wav plus its
               .paths placements) encoded with `atmos-encode` and decoded
               back to per-object WAVs, one SNR/LSD/MOS row per object per
               rate. Compute-only, no gate; see race_objects(). There is no
               external oracle for object decode at all - not FFmpeg's
               decoder, not Dolby's - so this is a self-consistency series
               throughout, which that function's own comment spells out.
               `--json-out PATH` writes the rows as JSON.

Every mode except `ci` and `trend` takes `--material synth|speech|music`,
which swaps the synthesized source for one of the committed 30 s CC0
programme fixtures - see MATERIALS near main() for why that matters and why
those two modes decline it.

Usage (repo root, after building):  python tools/ci/quality_race.py [mode]
Set AC3CLI to override the ac3cli binary (see CLI below); CI always does.
"""

import itertools
import json
import math
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
BUILD = REPO / "build"
# AC3CLI overrides the binary: the "dev" preset this default assumes does not
# exist (see CMakePresets.json - there is no such preset, only per-platform
# config-<leg> ones), and there is no ac3cli.exe on Linux at all. CI sets
# AC3CLI to the leg's real build/config-<preset>/bin/ac3cli; the hardcoded
# default is left as-is for whatever local workflow it used to match.
CLI = Path(os.environ.get("AC3CLI", str(BUILD / "dev" / "bin" / "ac3cli.exe")))
RATE = 48000
SEG = 2 * RATE  # 2 s per segment
# Bin 85, sub-band 4: the lowest the encoder will ever start coupling, so
# everything below it is coded per channel whatever the rate.
CPL_HZ = 85 * (RATE / 512.0)


def make_material():
    rng = np.random.default_rng(0x0B77)
    t = np.arange(SEG) / RATE
    segments = []

    # a) chord with vibrato
    vib = 1.0 + 0.002 * np.sin(2 * np.pi * 5.0 * t)
    chord = sum(np.sin(2 * np.pi * f * vib * t) for f in (220.0, 277.2, 329.6, 440.0))
    left = 0.15 * chord + 0.02 * np.sin(2 * np.pi * 1234.0 * t)
    right = 0.15 * chord + 0.02 * np.sin(2 * np.pi * 987.0 * t)
    segments.append((left, right))

    # b) sweep 100 -> 8000 Hz
    phase = 2 * np.pi * (100.0 * t + (8000.0 - 100.0) / (2 * t[-1]) * t * t)
    segments.append((0.4 * np.sin(phase), 0.4 * np.sin(phase * 1.0005)))

    # c) band-limited noise (pink-ish via cumulative smoothing)
    w = rng.standard_normal(SEG + 512)
    kernel = np.hanning(64)
    smooth = np.convolve(w, kernel / kernel.sum(), mode="same")[:SEG]
    noise = 0.3 * smooth / np.max(np.abs(smooth))
    segments.append((noise, 0.9 * noise + 0.1 * rng.standard_normal(SEG) * 0.05))

    # d) near-mono speech-ish (correlated -> rematrix territory)
    carrier = np.sin(2 * np.pi * 180.0 * t) * (0.5 + 0.5 * np.sin(2 * np.pi * 3.0 * t))
    formants = sum(0.3 * np.sin(2 * np.pi * f * t) for f in (700.0, 1220.0, 2600.0))
    mono = 0.25 * carrier * (1.0 + 0.3 * formants)
    segments.append((mono, mono * 0.98))

    # e) tone bursts (transient-ish, long-block stress)
    burst = np.zeros(SEG)
    for k in range(8):
        at = k * SEG // 8
        n = np.arange(4096)
        burst[at:at + 4096] += 0.5 * np.sin(2 * np.pi * 1500.0 * n / RATE) * np.exp(-n / 800.0)
    segments.append((burst, burst[::-1].copy()))

    left = np.concatenate([s[0] for s in segments]).astype(np.float32)
    right = np.concatenate([s[1] for s in segments]).astype(np.float32)
    return np.clip(left, -0.98, 0.98), np.clip(right, -0.98, 0.98)


def make_material_transient():
    """Material whose level moves BETWEEN the blocks of one frame.

    make_material() above is mostly stationary across a 32 ms frame, or moves
    slowly inside it - the case a single exponent set per frame handles well,
    and therefore the case that says nothing about how the set is chosen. This
    is the other case: onsets closer together than a frame, hard gates and
    decays short enough that a frame's loudest block sits 20-30 dB above its
    quietest. That gap is the whole cost of one exponent set per frame - every
    quiet block quantized against a scale chosen by the loud one - so it is
    what an exponent-run plan has to be measured on (roadmap EQ1).

    Stereo, same 2 s-per-segment shape and same seed discipline as the two
    generators above.
    """
    rng = np.random.default_rng(0x0B77 + 6)
    t = np.arange(SEG) / RATE
    segments = []

    # a) percussive hits every 40 ms - about one per frame and a quarter, so
    #    the attack lands on a different block of the frame each time.
    def hits(period_ms, decay_ms, tone_hz, seed):
        local = np.random.default_rng(seed)
        out = np.zeros(SEG)
        period = int(RATE * period_ms / 1000)
        length = min(period, int(RATE * decay_ms / 1000) * 6)
        n = np.arange(length)
        env = np.exp(-n / (RATE * decay_ms / 1000))
        for at in range(0, SEG - length, period):
            body = np.sin(2 * np.pi * tone_hz * n / RATE)
            out[at:at + length] += 0.85 * env * (0.7 * body + 0.3 * local.standard_normal(length))
        return out

    segments.append((hits(40, 8, 190.0, 1), hits(40, 8, 240.0, 2)))

    # b) gated noise, switched every 256 samples - a block-rate square wave in
    #    level, which is the worst case one set per frame can be handed.
    w = rng.standard_normal(SEG)
    gate = np.repeat(np.tile([1.0, 0.02], SEG // 512 + 1), 256)[:SEG]
    segments.append((0.5 * w * gate, 0.5 * np.roll(w, 331) * np.roll(gate, 256)))

    # c) sparse clicks over near-silence: high crest factor, and long stretches
    #    where a frame contains exactly one loud block.
    clicks_l = np.zeros(SEG)
    clicks_r = np.zeros(SEG)
    for at in rng.integers(0, SEG - 600, 24):
        n = np.arange(600)
        shape = np.exp(-n / 60.0) * np.sin(2 * np.pi * 3200.0 * n / RATE)
        clicks_l[at:at + 600] += 0.9 * shape
        clicks_r[at + 40:at + 640] += 0.8 * shape
    floor = 0.004 * rng.standard_normal(SEG)
    segments.append((clicks_l + floor, clicks_r + floor))

    # d) a chord amplitude-modulated at 30 Hz: about two cycles per frame, so
    #    the level swings smoothly but fully inside every frame.
    chord = sum(np.sin(2 * np.pi * f * t) for f in (330.0, 415.3, 494.0, 660.0))
    env = (0.5 + 0.5 * np.sin(2 * np.pi * 30.0 * t)) ** 3
    segments.append((0.22 * chord * env, 0.22 * chord * np.roll(env, 400)))

    left = np.concatenate([s[0] for s in segments]).astype(np.float32)
    right = np.concatenate([s[1] for s in segments]).astype(np.float32)
    return np.clip(left, -0.98, 0.98), np.clip(right, -0.98, 0.98)


def make_material_51():
    """Six DECORRELATED channels, in WAV order (FL FR FC LFE BL BR).

    An upmix would be the wrong test: coupling exists to exploit that channels
    share a high-frequency envelope but not a waveform, and correlated
    channels make it look better than it is. These share program material but
    differ in level, delay and detuning, which is what real multichannel
    content does.
    """
    rng = np.random.default_rng(0x0B77 + 51)
    left, right = make_material()
    n = left.size
    t = np.arange(n) / RATE
    centre = 0.6 * (left + right) / 2 + 0.15 * np.sin(2 * np.pi * 620.0 * t)
    lfe = 0.5 * np.sin(2 * np.pi * 45.0 * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.7 * t))
    # Surrounds: delayed, detuned and noise-dusted, so nothing above the
    # coupling frequency lines up with the fronts.
    delay = 719
    back_l = 0.55 * np.roll(right, delay) + 0.05 * rng.standard_normal(n)
    back_r = 0.55 * np.roll(left, -delay) + 0.05 * rng.standard_normal(n)
    channels = [left, right, centre, lfe, back_l, back_r]
    return [np.clip(c, -0.98, 0.98).astype(np.float32) for c in channels]


def make_material_dynamic(left, right, loud_s=0.5, quiet_gain=0.05):
    """make_material()'s programme with a slow loud/quiet envelope on top.

    A bit reservoir has nothing to redistribute unless some frames genuinely
    cost less than their share, and make_material() is deliberately dense
    from end to end - every segment is a steady, fully-scored signal. Real
    programme material is not: speech has gaps, music has rests, and that is
    where an average-rate encoder banks the bits it spends on the loud parts.
    This is the cheapest honest stand-in - alternating half-second passages at
    full level and at `quiet_gain` (-26 dB by default) - applied with a raised
    -cosine ramp so the gate itself is not a transient the encoder has to
    spend bits coding.
    """
    n = len(left)
    block = int(loud_s * RATE)
    ramp = int(0.02 * RATE)
    envelope = np.full(n, quiet_gain, dtype=np.float64)
    for start in range(0, n, 2 * block):
        envelope[start:start + block] = 1.0
    # Smooth the steps: a hard gate is an impulse, and coding an impulse is
    # not what this material is meant to be measuring.
    window = np.hanning(2 * ramp + 1)
    envelope = np.convolve(envelope, window / window.sum(), mode="same")
    return (left * envelope).astype(np.float32), (right * envelope).astype(np.float32)


def write_wav_f32(path, *channels):
    if len(channels) == 1:
        channels = channels[0]
    count = len(channels)
    data = np.empty(channels[0].size * count, dtype=np.float32)
    for i, channel in enumerate(channels):
        data[i::count] = channel
    payload = data.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, count, RATE, RATE * 4 * count,
                                      4 * count, 32))
        f.write(b"data" + struct.pack("<I", len(payload)) + payload)


def read_wav_f32(path):
    b = Path(path).read_bytes()
    i = b.find(b"fmt ")
    ch = struct.unpack_from("<H", b, i + 10)[0]
    j = b.find(b"data")
    n = struct.unpack_from("<I", b, j + 4)[0]
    return np.frombuffer(b, dtype=np.float32, count=n // 4, offset=j + 8).reshape(-1, ch)


def read_wav_any(path):
    """Like read_wav_f32, but format-aware: the checked-in fixed fixtures
    (reference_51.wav, reference_stereo.wav) are PCM16, written by the
    stdlib `wave` module (see tools/generators/gen_gold_reference_wav.py), not the
    float32 this project's own decode output and make_material()'s WAVs
    always are. read_wav_f32 stays a fast, format-blind path for the
    latter; this is for reading those committed fixtures as `original`.
    """
    b = Path(path).read_bytes()
    i = b.find(b"fmt ")
    fmt_tag, ch = struct.unpack_from("<HH", b, i + 8)
    bits = struct.unpack_from("<H", b, i + 22)[0]
    j = b.find(b"data")
    n = struct.unpack_from("<I", b, j + 4)[0]
    if fmt_tag == 3 and bits == 32:
        return np.frombuffer(b, dtype=np.float32, count=n // 4, offset=j + 8).reshape(-1, ch)
    if fmt_tag == 1 and bits == 16:
        pcm = np.frombuffer(b, dtype=np.int16, count=n // 2, offset=j + 8).reshape(-1, ch)
        return (pcm.astype(np.float32) / 32768.0)
    raise SystemExit(f"{path}: unsupported format tag {fmt_tag}/{bits}-bit")


def run(cmd):
    # check=False, then the explicit returncode test below: the raise has to
    # carry the command line and the captured stderr, which CalledProcessError
    # alone would not put in the CI log.
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(map(str, cmd))}\n{result.stderr}")


def align(original, decoded, skip=RATE, probe_len=32768, window_extra=65536):
    """Align by cross-correlation on a probe window; return the overlap.

    Defaults (skip 1s, 32768-sample probe, +65536 search window) match
    make_material()'s ~10s synthesized material, the only length this was
    originally written against. Committed fixtures used elsewhere (e.g.
    tests/golden/audio/reference_51.wav at 2.5s) are far shorter than
    2*skip + probe_len - the trimmed overlap below would come out empty or
    inverted - so callers scoring those pass smaller values explicitly
    rather than this function guessing a length-appropriate scale itself.
    """
    probe = original[skip:skip + probe_len, 0]
    window = decoded[: skip + window_extra, 0]
    corr = np.correlate(window, probe, mode="valid")
    lag = int(np.argmax(np.abs(corr))) - skip
    n = min(len(original), len(decoded) - lag) - 2 * skip
    o = original[skip:skip + n - skip]
    d = decoded[skip + lag:skip + lag + len(o)]
    return o, d, lag


def aligned_snr(original, decoded):
    o, d, _lag = align(original, decoded)
    noise = d - o
    return 10 * np.log10(np.sum(o**2) / max(np.sum(noise**2), 1e-30))


NFFT = 1024
_HANN = np.hanning(NFFT)


def _spectrogram(x):
    """Magnitude-squared STFT of one channel, frames along axis 0."""
    hop = NFFT // 2
    count = (len(x) - NFFT) // hop
    frames = np.lib.stride_tricks.as_strided(
        x, shape=(count, NFFT), strides=(x.strides[0] * hop, x.strides[0]))
    return np.abs(np.fft.rfft(frames * _HANN, axis=1)) ** 2


def _bark_bands():
    """Band edges (rfft bin indices) on a Bark-like scale up to Nyquist."""
    hz = np.fft.rfftfreq(NFFT, 1.0 / RATE)
    bark = 13 * np.arctan(0.00076 * hz) + 3.5 * np.arctan((hz / 7500.0) ** 2)
    edges = [0]
    for step in np.linspace(bark[1], bark[-1], 25)[1:]:
        edges.append(int(np.searchsorted(bark, step)))
    return [(a, b) for a, b in itertools.pairwise(edges) if b > a]


BANDS = _bark_bands()


def spectral_scores(o, d):
    """Log-spectral distance, and the high-band energy ratio, both in dB.

    Waveform SNR is the wrong lens for parametric tools: coupling replaces a
    channel's high band with a scaled copy of a shared one, and spectral
    extension synthesizes it outright, so both destroy the waveform there by
    construction while preserving the banded envelope, which is what they set
    out to preserve and what a listener hears. LSD scores that envelope; the
    HF ratio says whether the top of the spectrum is present at all.
    """
    lsd = []
    hf = int(10000.0 / (RATE / NFFT))
    hf_o = hf_d = 0.0
    for c in range(o.shape[1]):
        so = _spectrogram(np.ascontiguousarray(o[:, c]))
        sd = _spectrogram(np.ascontiguousarray(d[:, c]))
        hf_o += so[:, hf:].sum()
        hf_d += sd[:, hf:].sum()
        # Ignore near-silent frames: their band ratios are dominated by the
        # floor and would swamp the average with meaningless dBs.
        energy = so.sum(axis=1)
        loud = energy > 1e-6 * max(energy.max(), 1e-30)
        for lo, hi in BANDS:
            eo = so[loud, lo:hi].sum(axis=1) + 1e-12
            ed = sd[loud, lo:hi].sum(axis=1) + 1e-12
            lsd.append(np.mean(np.abs(10 * np.log10(ed / eo))))
    return float(np.mean(lsd)), 10 * np.log10((hf_d + 1e-20) / (hf_o + 1e-20))


# --- Perceptual quality (ViSQOL) ---------------------------------------------
#
# SNR and the Bark-banded LSD/HF measures above are waveform- and envelope-
# level proxies for what a listener actually hears. ViSQOL (Google's Virtual
# Speech Quality Objective Listener, https://github.com/google/visqol) scores
# that more directly: a full-reference perceptual model that predicts a
# MOS-LQO (Mean Opinion Score - Listening Quality Objective), 1 (bad) to
# audio mode's ~4.75 ceiling.
#
# Chosen over PEAQ (ITU-R BS.1387): the PEAQ algorithm itself is FRAND-
# licensed ITU IP, not something this clean-room project wants to take a
# dependency on (see docs/concepts/object-signing.md for the same reasoning
# applied to Dolby's own authentication tag), and the one credible open
# implementation, GstPEAQ, documents its own non-conformance to the BS.1387-1
# tolerance and ships only as a GStreamer/C plugin - no Python surface, a
# heavier and less certain dependency than ViSQOL's Apache-2.0 Python package
# for a column that has to stay optional.
#
# Google's own google/visqol repo has no published PyPI wheel: its Python API
# is `pip install .` after a Bazel + TensorFlow source build (confirmed
# against the repo's own README, not assumed), which is not viable as an
# optional dependency for a casual local run or a CI container. `visqol-
# python` (PyPI, Apache-2.0, https://github.com/talker93/visqol-python) is a
# pure-Python reimplementation with prebuilt wheels and no Bazel/C++
# toolchain requirement; its own conformance suite scores audio-mode output
# within 0.0002 MOS-LQO of the reference C++ implementation across its test
# vectors. It bundles the audio-mode SVR model file, so no separate model
# download is needed once the package itself is installed - verified
# locally: identical audio scores ~4.73, a heavily noised copy scores 1.0,
# confirming the number actually moves with quality rather than being a
# stub.
#
# Optional exactly like AC3FORGE_WITH_ALSA (src/audio/CMakeLists.txt): a
# missing `visqol-python` install skips this column with one clear message,
# printed once, and never fails the run. Nobody running a quality race
# locally should be forced to install it, and CI does not either - see
# ffmpeg-validate's own comment in .github/workflows/ci.yml for why.
try:
    from visqol import VisqolApi
except ImportError:
    VisqolApi = None

_visqol_api = None
_visqol_warned = False

# How much audio ViSQOL is ever handed for one score, in seconds.
#
# ViSQOL's patch matching is super-linear in signal length. Measured on this
# package (visqol-python 3.7.0, one machine, same synthetic pair truncated to
# each length):
#
#   2 s -> 2.2 s     4 s -> 5.8 s     6 s -> 9.8 s     8 s -> 14.1 s
#   3 s -> 3.9 s    10 s -> 20.5 s   30 s -> 127.8 s
#
# so a 30 s programme fixture costs 30x what a 3 s synthetic one does, and
# `trend` mode makes 62 distinct scoring calls per run across its eight legs.
# Uncapped, the programme legs alone would add over half an hour to a
# pull-request job. Capped at 4 s, the whole column measured about seven
# minutes (a full `trend` run took 10m48s with ViSQOL and 3m59s with it
# stubbed out).
#
# 4 s is where the score itself has converged: the same pair reads 4.336 at
# 2 s, 4.272 at 4 s, 4.257 at 6 s and 4.251 at 8 s, so the residual against a
# much longer window is ~0.02 MOS - and it is a CONSTANT offset, because the
# window is always the same deterministic span of the same fixture. Trend
# deltas, which is what every consumer of this column actually reads, are
# unaffected by a constant offset; only the absolute value moves, and only by
# less than the difference between two adjacent tool sets.
#
# Anything shorter than this is scored whole, so the existing 2.5-3 s
# synthetic fixtures are unaffected by this cap existing.
MOS_WINDOW_S = 4.0


def perceptual_score(o, d, rate=RATE):
    """MOS-LQO via ViSQOL audio mode, or None if it isn't available.

    o/d are aligned original/decoded arrays from align()/score_fixed, any
    channel count. Trimmed to MOS_WINDOW_S (see its own comment for the
    measured cost curve that makes the cap necessary) around the MIDDLE of
    the overlap rather than its start - a fixture's first second is the most
    likely place to find a fade-in, and align()'s own leading trim varies by
    a handful of samples between calls, so a centred window is both more
    representative and less sensitive to that. The same span is taken from
    both arrays, so the alignment align() established is preserved.

    Downmixed to mono by hand (plain channel average) before
    scoring rather than handed through as-is: measure_from_arrays() accepts
    a multi-channel array without erroring, but a real 6-channel A/B scored
    3.38 through its own internal handling against 1.60 for the identical
    pair downmixed by hand first - undocumented and not something to rely
    on. A plain mean matches ViSQOL audio mode's documented file-based
    behaviour ("down-mixes multi-channel to mono") and is at least a mixdown
    this function can promise callers.

    None on three distinct causes: the package isn't installed, the call
    itself raised (some native/runtime failure), or it returned a
    non-finite score - observed for real with a fully-silent degraded
    signal (NaN, not an exception) rather than assumed. The first two are
    each logged once per run, not once per row, so a long table doesn't
    repeat the same line; the non-finite case is quiet on purpose - it can
    legitimately recur per row (e.g. several silent-segment rows in the
    same run) without being a "the tool is unavailable" condition worth
    repeating a message for.
    """
    global _visqol_api, _visqol_warned  # noqa: PLW0603 - memoised handle + once-per-run flag, see the docstring
    if VisqolApi is None:
        if not _visqol_warned:
            print("  (visqol-python not installed - skipping the perceptual-quality "
                  "column; pip install visqol-python to enable it)")
            _visqol_warned = True
        return None
    if _visqol_api is None:
        _visqol_api = VisqolApi()
        _visqol_api.create(mode="audio")
    window = int(MOS_WINDOW_S * rate)
    n = min(len(o), len(d))
    if n > window:
        start = (n - window) // 2
        o, d = o[start:start + window], d[start:start + window]
    mono_o = np.ascontiguousarray((o.mean(axis=1) if o.ndim > 1 else o), dtype=np.float64)
    mono_d = np.ascontiguousarray((d.mean(axis=1) if d.ndim > 1 else d), dtype=np.float64)
    try:
        mos = float(_visqol_api.measure_from_arrays(mono_o, mono_d, sample_rate=rate).moslqo)
    except Exception as exc:  # every failure mode degrades to None, see the docstring
        if not _visqol_warned:
            print(f"  (visqol scoring failed ({exc}) - skipping the perceptual-quality column)")
            _visqol_warned = True
        return None
    return mos if math.isfinite(mos) else None


def _fmt_mos(mos):
    return "-" if mos is None else f"{mos:.2f}"


def decode_scores(original, coded, wav_path, strict=True, perceptual=False):
    """Decode with FFmpeg (the neutral referee) and score against the source."""
    cmd = ["ffmpeg", "-v", "error", "-y"]
    if strict:
        # Only our own output gets the strict reader: a frame-layout error
        # shows up here as a CRC failure rather than as quiet noise. -xerror
        # is required alongside -err_detect, not optional: -err_detect alone
        # only controls what the decoder treats as an error internally
        # (concealing a bad frame and moving on) - it does not by itself
        # change ffmpeg's exit code, which run() below is the only thing
        # checking. -xerror is what turns a detected error into a failing
        # process and a raised SystemExit here.
        cmd += ["-xerror", "-err_detect", "crccheck+bitstream+buffer+explode"]
    run([*cmd, "-i", coded, "-c:a", "pcm_f32le", wav_path])
    o, d, _ = align(original, read_wav_f32(wav_path))
    snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
    lsd, hf = spectral_scores(o, d)
    mos = perceptual_score(o, d) if perceptual else None
    return snr, lsd, hf, mos


def decode_scores_ours(original, coded, wav_path, perceptual=False):
    """Decode with THIS project's own decoder and score against the source.

    Enhanced coupling (ecpl) and transient pre-noise processing (tpn) change
    the bitstream in ways FFmpeg's own Annex E parser has never read - it is
    not that it declines them cleanly, it does not know the syntax exists, so
    -xerror against it would reject a correctly-formed stream rather than
    catch a real regression (see decoder.hpp's own module comment and the
    ci-ffmpeg-validation-and-coverage-initiative / eac3-annex-e-tools-decode
    project history: neither tool has ever had an external oracle). Self-
    consistency is what is available instead: this project's own encoder and
    decoder share the same tables and the same reading of the spec, so a
    genuine regression in either still collapses SNR/LSD by many dB here,
    same as everywhere else on this gate - it just cannot catch a defect
    both sides agree on.
    """
    run([CLI, "decode", coded, wav_path])
    o, d, _ = align(original, read_wav_f32(wav_path))
    snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
    lsd, hf = spectral_scores(o, d)
    mos = perceptual_score(o, d) if perceptual else None
    return snr, lsd, hf, mos


# The committed fixed fixtures (reference_51.wav, reference_stereo.wav) are
# a few seconds long, not make_material()'s ~10s - align()'s own defaults
# would trim them to an empty or inverted overlap (see its docstring), so
# external-baseline scoring (tools/generators/gen_external_baseline.py, and the `trend`
# mode built on the same fixtures) uses this scaled-down window instead.
FIXED_ALIGN = {"skip": int(0.2 * RATE), "probe_len": 8192, "window_extra": 16384}


def score_fixed(original, decoded, perceptual=False):
    """SNR + spectral scores between two already-decoded PCM arrays, using
    FIXED_ALIGN rather than align()'s make_material()-scaled defaults."""
    o, d, _ = align(original, decoded, **FIXED_ALIGN)
    snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
    lsd, hf = spectral_scores(o, d)
    mos = perceptual_score(o, d) if perceptual else None
    return snr, lsd, hf, mos


def decode_scores_ours_fixed(original, coded, wav_path, perceptual=False):
    """decode_scores_ours, scaled for the short checked-in fixtures (see
    score_fixed) instead of make_material()'s synthesized ~10s material."""
    run([CLI, "decode", coded, wav_path])
    return score_fixed(original, read_wav_f32(wav_path), perceptual=perceptual)


def measured_kbps(path, seconds):
    return Path(path).stat().st_size * 8 / seconds / 1000.0


def race_ac3(original, source, seconds):
    print(f"{'kbps':>5} | {'ours dB':>8} | {'ffmpeg dB':>9} | {'gap':>6} | "
          f"{'ours MOS':>8} | {'ffmpeg MOS':>10}")
    print("-" * 61)
    worst_gap = -1e9
    for kbps in (192, 256, 320, 448):
        ours = BUILD / f"race_ours_{kbps}.ac3"
        theirs = BUILD / f"race_ff_{kbps}.ac3"
        run([CLI, "encode", source, ours, str(kbps)])
        run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "ac3",
             "-b:a", f"{kbps}k", theirs])
        ours_snr, _, _, ours_mos = decode_scores(original, ours, BUILD / f"race_ours_{kbps}.wav",
                                                  perceptual=True)
        ff_snr, _, _, ff_mos = decode_scores(original, theirs, BUILD / f"race_ff_{kbps}.wav",
                                             strict=False, perceptual=True)
        gap = ff_snr - ours_snr
        worst_gap = max(worst_gap, gap)
        print(f"{kbps:>5} | {ours_snr:>8.2f} | {ff_snr:>9.2f} | {gap:>+6.2f} | "
              f"{_fmt_mos(ours_mos):>8} | {_fmt_mos(ff_mos):>10}")
    print(f"\nworst gap vs ffmpeg: {worst_gap:+.2f} dB (positive = ffmpeg better)")
    print("MOS-LQO (ViSQOL audio mode, 1-4.75): '-' means visqol-python isn't installed -")
    print("see perceptual_score()'s own docstring.")


# One column per E-AC-3 variant: the label, and the tool token handed to
# `ac3cli eac3-encode`. "none" is the tool-free coding path the Annex E tools
# have to beat to earn their place.
EAC3_VARIANTS = [("none", None), ("auto", "auto"), ("cpl", "cpl"), ("spx", "spx"),
                 ("aht", "aht"), ("cpl+spx", "cpl+spx"), ("all", "all")]

# Enhanced coupling and transient pre-noise processing: FFmpeg has no reading
# of either's syntax at all (see decode_scores_ours' docstring), so these are
# scored separately from EAC3_VARIANTS above, through this project's own
# decoder rather than race_eac3's FFmpeg path.
EAC3_SELF_VARIANTS = [("ecpl", "cpl+ecpl"), ("tpn", "tpn"), ("ecpl+tpn", "cpl+ecpl+tpn")]


def race_eac3(original, source, seconds, rates=(96, 128, 192)):
    print(f"{'kbps':>5} | {'variant':<10} | {'SNR dB':>7} | {'LSD dB':>6} | "
          f"{'HF dB':>6} | {'MOS':>4} | {'rate':>6}")
    print("-" * 62)
    for kbps in rates:
        for label, tools in [*EAC3_VARIANTS, ("ffmpeg", "ffmpeg")]:
            coded = BUILD / f"race_e_{label}_{kbps}.ec3"
            if tools == "ffmpeg":
                run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "eac3",
                     "-b:a", f"{kbps}k", coded])
            else:
                cmd = [CLI, "eac3-encode", source, coded, str(kbps)]
                if tools:
                    cmd.append(tools)
                run(cmd)
            snr, lsd, hf, mos = decode_scores(original, coded,
                                              BUILD / f"race_e_{label}_{kbps}.wav",
                                              strict=tools != "ffmpeg", perceptual=True)
            rate = measured_kbps(coded, seconds)
            print(f"{kbps:>5} | {label:<10} | {snr:>7.2f} | {lsd:>6.2f} | "
                  f"{hf:>+6.1f} | {_fmt_mos(mos):>4} | {rate:>6.1f}")
        print()


# --- EQ7: does the fast-gain curve survive E-AC-3's side-info bill? ------------
#
# AC-3 has followed a measured fgaincod curve since 0.7.0 because it costs
# nothing there: §7.2.2.4's fast gain rides the snroffst element AC-3 already
# sends every block. E-AC-3's baie does not carry fgaincod at all, so any code
# other than Table E1.4's implied 0x4 opens a separate per-block fgaincode
# element in ALL SIX blocks - 1 + 3*(nchans + cplinu) bits each - paid out of
# the mantissa budget. Whether the better masking curve is worth that is a
# measurement, which is what this mode is.
#
# Five legs per rate:
#
#   auto      - the shipped default: no element, implied 0x4. The baseline.
#   pin-0x4   - fgaincod=4 explicitly. Byte-identical to `auto` BY
#               CONSTRUCTION - encode_frame only sets frmfgaincode when the
#               pinned code differs from §8.2.12's, so stating 0x4 writes no
#               element either. It is carried as a control anyway, and it is
#               the most useful row in the table for reading the rest: every
#               one of its deltas must come out exactly 0.000, which is the
#               proof that encode -> FFmpeg decode -> align -> score has no
#               run-to-run noise at all. Any non-zero delta elsewhere is
#               therefore signal, not scatter. (What it is NOT is a
#               cost-only control - there is no way to buy the element
#               without also changing the code, so the side-info bill is
#               priced arithmetically instead: 1 + 3*(nchans + cplinu) bits a
#               block, six blocks, against the frame's own bit count.)
#   pin-N     - fgaincod pinned at rate_adaptive_fgaincod's own value for
#               this rate. (pin-N - auto) is the curve's benefit NET of the
#               element it had to open to get there, which is the question.
#   search-1ax- search=distortion with fgaincod pinned at the default, which
#               takes it out of the candidate set: exactly the one-axis
#               dbpbcod-only search EQ13 shipped, and the thing roadmap EQ8
#               recorded as not moving the stereo/192 cell.
#   search-2ax- search=distortion left to move both axes, refitting each
#               candidate against its own side-info cost. The per-frame
#               answer, and (search-2ax - search-1ax) is what EQ7 added to
#               EQ13 rather than what EQ13 was already worth.
#
# Every metric is reported per WINDOW, not once per file (see
# window_profile): the encoder here is deterministic - encoding the same WAV
# twice is byte-identical, so there is no run-to-run noise to average out -
# but 30 s of programme material is not homogeneous, and a MOS-LQO scored on
# one 4 s slice of it is one sample of a distribution. Differences are taken
# PAIRED, window by window against the baseline's same window, so the spread
# reported alongside each delta is the spread of the DIFFERENCE and not the
# much larger spread of the material.
FGAINCOD_RATES = (96, 128, 192, 256, 448, 640)
# 5.1 with coupling, where the element is at its most expensive: five
# full-bandwidth channels plus the LFE and the coupling channel is 22 bits a
# block, 132 a frame - about 1.1% of a 384 kbit/s frame against about 0.4% of
# a 192 kbit/s stereo one.
FGAINCOD_RATES_51 = (384, 640)
FGAINCOD_WINDOWS = 5


def rate_adaptive_fgaincod(kbps, nfchans):
    """ac3::rate_adaptive_fgaincod (src/forge/src/core/bitalloc.cpp), mirrored.

    Deliberately a second copy of the line rather than a number typed per
    row: this mode exists to pin the code the ENCODER would have chosen, so
    if that line ever moves the table has to move with it. Truncation is
    toward zero to match C++ integer division, which matters on every leg
    where the numerator goes negative (any rate above 128 kbit/s per channel)
    before the clamp catches it.
    """
    per_channel = kbps // max(nfchans, 1)
    numerator = (128 - per_channel) * 7 + 90 // 2
    quotient = abs(numerator) // 90
    return max(0, min(7, quotient if numerator >= 0 else -quotient))


def window_profile(o, d, rate=RATE, windows=FGAINCOD_WINDOWS):
    """SNR/LSD/MOS per disjoint MOS_WINDOW_S span, evenly tiled across o/d.

    Returns three lists of equal length. MOS entries are None wherever
    perceptual_score declined (see its docstring) - callers pair the lists
    positionally, so a hole stays a hole rather than shifting the pairing.

    Half a second is dropped from each end before tiling: align()'s own
    leading trim varies by a handful of samples between calls, and a
    fixture's very start is the most likely place to find a fade-in.
    """
    span = int(MOS_WINDOW_S * rate)
    margin = rate // 2
    n = min(len(o), len(d))
    usable = max(n - 2 * margin, span)
    count = max(1, min(windows, usable // span))
    chunk = usable // count
    snrs, lsds, moses = [], [], []
    for i in range(count):
        start = margin + i * chunk + (chunk - span) // 2
        ow, dw = o[start:start + span], d[start:start + span]
        # float(), not the np.float32 these come back as: read_wav_f32 hands
        # back float32, so every reduction over it stays float32 - and a
        # float32 is not JSON serialisable, which --json-out finds only after
        # the whole (long) sweep has already run.
        snrs.append(float(10 * np.log10(np.sum(ow**2) / max(np.sum((dw - ow) ** 2), 1e-30))))
        lsds.append(float(spectral_scores(ow, dw)[0]))
        mos = perceptual_score(ow, dw, rate=rate)
        moses.append(None if mos is None else float(mos))
    return snrs, lsds, moses


def paired_delta(leg, base):
    """Mean and standard error of the paired per-window difference.

    Pairs positionally and skips any window where either side is None, which
    is the only way a MOS list here has holes. (None, None) when nothing
    pairs at all, so a row prints "-" rather than inventing a zero.
    """
    # strict=True: every leg at a rate is scored by the same window_profile
    # over the same-length overlap, so a length mismatch would mean the
    # pairing had silently drifted - which is the one failure this whole
    # paired treatment cannot survive, and so is worth raising over.
    pairs = [a - b for a, b in zip(leg, base, strict=True)
             if a is not None and b is not None]
    if not pairs:
        return None, None
    mean = float(np.mean(pairs))
    if len(pairs) < 2:
        return mean, None
    return mean, float(np.std(pairs, ddof=1) / math.sqrt(len(pairs)))


def _fmt_delta(mean, sem, width=13, places=3):
    if mean is None:
        return f"{'-':>{width}}"
    text = f"{mean:+.{places}f}"
    if sem is not None:
        text += f"+-{sem:.{places}f}"
    return f"{text:>{width}}"


def fgaincod_legs(kbps, nfchans):
    """(label, extra CLI token) per leg, with the curve's own value resolved.

    pin-N drops out wherever the curve lands back on §8.2.12's own code -
    which it does at 5.1/384, 76 kbit/s a channel - because there it IS
    pin-0x4: the encoder writes no element for either, so a second identical
    row would only pad the table. The rate still has a real answer, and it is
    "the curve asks for nothing here", which the curve_fgaincod column says.
    """
    curve = rate_adaptive_fgaincod(kbps, nfchans)
    legs = [("auto", None), ("pin-0x4", "fgaincod=4")]
    if curve != 4:
        legs.append((f"pin-{curve}", f"fgaincod={curve}"))
    # fgaincod=4 alongside search= is not redundant with pin-0x4: it pins the
    # code, which is what takes it out of the search's candidate set (see
    # eac3_frame.cpp's `config_.fgaincod < 0` guard), leaving dbpbcod as the
    # only axis. That is the shipped one-axis search, reproduced here as the
    # control the two-axis row is actually measured against.
    legs.append(("search-1ax", "search=distortion fgaincod=4"))
    legs.append(("search-2ax", "search=distortion"))
    return legs, curve


def race_fgaincod(original, source, seconds, rates=FGAINCOD_RATES, tools="none",
                  layout="stereo", nfchans=2, json_out=None, rows=None):
    rows = [] if rows is None else rows
    print(f"{'kbps':>5} | {'leg':<10} | {'SNR dB':>7} | {'dSNR':>13} | {'LSD dB':>6} | "
          f"{'dLSD':>13} | {'MOS':>5} | {'dMOS':>13} | {'rate':>6}")
    print("-" * 100)
    for kbps in rates:
        legs, curve = fgaincod_legs(kbps, nfchans)
        base = None
        for label, extra in legs:
            coded = BUILD / f"race_fg_{layout}_{tools}_{label}_{kbps}.ec3"
            cmd = [CLI, "eac3-encode", str(source), str(coded), str(kbps), tools, layout]
            if extra:
                # split(): a leg may carry more than one token (search-1ax is
                # search= and fgaincod= together), and argv entries never
                # contain spaces here.
                cmd.extend(extra.split())
            run(cmd)
            wav = BUILD / f"race_fg_{layout}_{tools}_{label}_{kbps}.wav"
            # FFmpeg throughout, and with the strict reader on: the point of
            # this table is a comparison of ENCODERS, so the decoder has to be
            # one constant. Its own Annex E parser does read Table E1.4's
            # fgaincode element - verified before this mode was written, by
            # decoding a pinned stream both ways and finding 63-66 dB
            # agreement with this project's own decoder, which is the §7.3.4
            # dither floor rather than an allocation divergence.
            run(["ffmpeg", "-v", "error", "-y", "-xerror", "-err_detect",
                 "crccheck+bitstream+buffer+explode", "-i", str(coded),
                 "-c:a", "pcm_f32le", str(wav)])
            o, d, _ = align(original, read_wav_f32(wav))
            snrs, lsds, moses = window_profile(o, d)
            if base is None:
                base = (snrs, lsds, moses)
            dsnr = paired_delta(snrs, base[0])
            dlsd = paired_delta(lsds, base[1])
            dmos = paired_delta(moses, base[2])
            scored = [m for m in moses if m is not None]
            rate = measured_kbps(coded, seconds)
            print(f"{kbps:>5} | {label:<10} | {float(np.mean(snrs)):>7.2f} | "
                  f"{_fmt_delta(*dsnr)} | {float(np.mean(lsds)):>6.2f} | "
                  f"{_fmt_delta(*dlsd)} | "
                  f"{(f'{np.mean(scored):.2f}' if scored else '-'):>5} | "
                  f"{_fmt_delta(*dmos)} | {rate:>6.1f}")
            rows.append({"layout": layout, "tools": tools, "kbps": kbps, "leg": label,
                         "curve_fgaincod": curve, "windows": len(snrs),
                         "snr_db": snrs, "lsd_db": lsds, "mos": moses,
                         "d_snr_db": dsnr[0], "d_snr_sem": dsnr[1],
                         "d_lsd_db": dlsd[0], "d_lsd_sem": dlsd[1],
                         "d_mos": dmos[0], "d_mos_sem": dmos[1],
                         "measured_kbps": rate})
        print()
    print("deltas are PAIRED per-window differences against this rate's own auto row,")
    print("mean +- standard error over the windows. LSD is an error, so NEGATIVE is")
    print("better there; SNR and MOS are qualities, so positive is better.")
    if json_out:
        Path(json_out).write_text(json.dumps(rows, indent=2), encoding="utf-8")
    return rows


# --- VBR characterisation ----------------------------------------------------
#
# VBR shipped as a per-frame quality knob with no evidence at all: no race
# leg, no trend row, no rate-distortion curve - only a warning in
# docs/concepts/ac3-eac3.md that cost "rises steeply in the top part of the
# quality range". This is the measurement that warning was standing in for.
#
# The question a VBR mode exists to answer is not "what does quality 0.6
# sound like", it is "at the rate this ends up costing, would plain CBR have
# done better". So every sweep point is scored against a CBR encode at the
# rate the VBR point actually measured - not at some nominal rate - and
# against FFmpeg's own CBR encoder at the same number, which is the neutral
# third opinion the rest of this file already leans on.
#
# The format's own ceiling is 2048 words per frame (frmsiz is 11 bits), which
# at 48 kHz is 1024 kbit/s. Every sweep point carries that as its max_kbps:
# without a bound, a high quality on real material asks for more than any
# legal frame can hold and the encoder refuses the frame outright (see
# VbrConfig::quality). Bounding at the FORMAT ceiling rather than at some
# tighter number keeps the curve honest - nothing is capped that the
# bitstream itself could have carried.
VBR_FORMAT_MAX_KBPS = 1024
VBR_QUALITIES = (0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90)

# The ABR targets checked for delivery. Spread across the rates a streaming
# ladder actually asks E-AC-3 for, since "hits the rate it was told to hit"
# is the whole deliverable.
ABR_TARGETS = (96, 128, 192, 256, 384)


def _encode_eac3(source, out, kbps, vbr=None, tools="none"):
    cmd = [CLI, "eac3-encode", source, out, str(kbps), tools, "stereo"]
    if vbr is not None:
        cmd.append(vbr)
    run(cmd)
    return out


def race_vbr(original, source, seconds, json_out=None):
    """Sweep VbrConfig.quality; score each point against CBR at the SAME
    measured rate, and against FFmpeg CBR at that rate.

    Read the `gap` columns, not the absolute SNR: a sweep point that costs
    203 kbit/s is only interesting next to what 203 kbit/s of CBR buys.
    Positive means VBR won.

    Three comparisons per point, all at the rate the VBR point measured:
    CBR (a fixed frame size), ABR (the same rate asked for as an average),
    and FFmpeg's own CBR encoder. The ABR column is there because "average
    rate" and "variable rate" sound alike and are not: holding an average is
    the same constraint CBR encodes under, so ABR is expected to score with
    CBR rather than with VBR, and that expectation is worth having on the
    table rather than in a comment.
    """
    rows = []
    print("=== E-AC-3 VBR rate-distortion sweep (stereo, tools=none) ===")
    print(f"{'quality':>7} | {'kbps':>6} | {'SNR dB':>7} | {'LSD dB':>6} | {'MOS':>4} | "
          f"{'CBR SNR':>7} | {'vs CBR':>7} | {'ABR SNR':>7} | {'vs ABR':>7} | "
          f"{'ff SNR':>7} | {'vs ff':>7}")
    print("-" * 104)
    for quality in VBR_QUALITIES:
        tag = f"{quality:.2f}".replace(".", "")
        coded = BUILD / f"vbr_q{tag}.ec3"
        _encode_eac3(source, coded, 192, f"q:{quality},max:{VBR_FORMAT_MAX_KBPS}")
        kbps = measured_kbps(coded, seconds)
        snr, lsd, _hf, mos = decode_scores(original, coded, BUILD / f"vbr_q{tag}.wav",
                                           perceptual=True)

        # The comparison encode: CBR at the rate the VBR point actually cost,
        # rounded to whole kbit/s. Anything else compares two different rates.
        # Clamped to the format ceiling: measured_kbps divides by the SOURCE's
        # duration, but the encoder pads the last frame out to a whole 1536
        # samples, so a stream already pinned to 1024 kbit/s measures a
        # fraction of a per cent above it and CBR has no legal frame that big.
        cbr_kbps = min(VBR_FORMAT_MAX_KBPS, max(32, round(kbps)))
        cbr = BUILD / f"vbr_cbr{cbr_kbps}.ec3"
        _encode_eac3(source, cbr, cbr_kbps)
        cbr_snr, cbr_lsd, _cbr_hf, cbr_mos = decode_scores(
            original, cbr, BUILD / f"vbr_cbr{cbr_kbps}.wav", perceptual=True)

        # ...and the same rate asked for as an AVERAGE rather than as a fixed
        # frame size. This is the column that says what ABR is and is not
        # for: holding an average is the same constraint CBR encodes under,
        # so ABR is expected to land on CBR, not on VBR - and a change that
        # quietly turned ABR into either one of them shows up right here.
        abr = BUILD / f"vbr_abr{cbr_kbps}.ec3"
        _encode_eac3(source, abr, 192, f"avg:{cbr_kbps}")
        abr_snr, abr_lsd, _abr_hf, abr_mos = decode_scores(
            original, abr, BUILD / f"vbr_abr{cbr_kbps}.wav", perceptual=True)
        abr_kbps = measured_kbps(abr, seconds)

        ff = BUILD / f"vbr_ff{cbr_kbps}.ec3"
        run(["ffmpeg", "-v", "error", "-y", "-i", source, "-c:a", "eac3",
             "-b:a", f"{cbr_kbps}k", ff])
        ff_snr, ff_lsd, _ff_hf, ff_mos = decode_scores(
            original, ff, BUILD / f"vbr_ff{cbr_kbps}.wav", strict=False, perceptual=True)

        rows.append({"mode": "vbr", "quality": quality, "measured_kbps": float(kbps),
                     "snr_db": float(snr), "lsd_db": float(lsd),
                     "mos_lqo": None if mos is None else float(mos),
                     "cbr_kbps": cbr_kbps, "cbr_snr_db": float(cbr_snr),
                     "cbr_lsd_db": float(cbr_lsd),
                     "cbr_mos_lqo": None if cbr_mos is None else float(cbr_mos),
                     "abr_measured_kbps": float(abr_kbps), "abr_snr_db": float(abr_snr),
                     "abr_lsd_db": float(abr_lsd),
                     "abr_mos_lqo": None if abr_mos is None else float(abr_mos),
                     "ffmpeg_snr_db": float(ff_snr), "ffmpeg_lsd_db": float(ff_lsd),
                     "ffmpeg_mos_lqo": None if ff_mos is None else float(ff_mos)})
        print(f"{quality:>7.2f} | {kbps:>6.1f} | {snr:>7.2f} | {lsd:>6.2f} | "
              f"{_fmt_mos(mos):>4} | {cbr_snr:>7.2f} | {snr - cbr_snr:>+7.2f} | "
              f"{abr_snr:>7.2f} | {snr - abr_snr:>+7.2f} | "
              f"{ff_snr:>7.2f} | {snr - ff_snr:>+7.2f}")

    print()
    print("=== E-AC-3 ABR: does the average land where it was asked to? ===")
    print(f"{'target':>6} | {'measured':>8} | {'error %':>7} | {'SNR dB':>7} | "
          f"{'LSD dB':>6} | {'MOS':>4} | {'CBR SNR':>7} | {'vs CBR':>7}")
    print("-" * 72)
    for target in ABR_TARGETS:
        coded = BUILD / f"abr_{target}.ec3"
        # quality 1.0 makes the reservoir the only thing bounding the rate,
        # which is what "hold this average" is being asked to mean here.
        _encode_eac3(source, coded, 192, f"avg:{target}")
        kbps = measured_kbps(coded, seconds)
        snr, lsd, _hf, mos = decode_scores(original, coded, BUILD / f"abr_{target}.wav",
                                           perceptual=True)
        cbr = BUILD / f"abr_cbr{target}.ec3"
        _encode_eac3(source, cbr, target)
        cbr_snr, _cbr_lsd, _cbr_hf, _cbr_mos = decode_scores(
            original, cbr, BUILD / f"abr_cbr{target}.wav", perceptual=False)
        error = 100.0 * (kbps - target) / target
        rows.append({"mode": "abr", "target_kbps": target, "measured_kbps": float(kbps),
                     "error_pct": float(error), "snr_db": float(snr), "lsd_db": float(lsd),
                     "mos_lqo": None if mos is None else float(mos),
                     "cbr_snr_db": float(cbr_snr)})
        print(f"{target:>6} | {kbps:>8.1f} | {error:>+7.2f} | {snr:>7.2f} | {lsd:>6.2f} | "
              f"{_fmt_mos(mos):>4} | {cbr_snr:>7.2f} | {snr - cbr_snr:>+7.2f}")

    # --- where the reservoir actually earns its keep -----------------------
    #
    # Every row above lands on its target and scores IDENTICALLY to CBR,
    # which is not a bug and not a coincidence: at quality 1.0 the composite
    # SNR offset saturates the bit allocation for every band regardless of
    # what the band contains, so every frame asks for more than its share,
    # every frame is capped at its share, and ABR reduces exactly to CBR.
    # A reservoir can only redistribute bits that some frame declined to
    # spend - which means a quality ceiling BELOW 1.0, on material that has
    # cheap frames in it. make_material() has neither. This block supplies
    # both, and is the only place in this mode where ABR is measured against
    # CBR on a level footing rather than being it.
    print()
    print("=== E-AC-3 ABR on material with dynamics (loud/quiet passages) ===")
    dyn_left, dyn_right = make_material_dynamic(*make_material())
    dyn_source = BUILD / "vbr_dynamic.wav"
    write_wav_f32(dyn_source, dyn_left, dyn_right)
    dyn_original = read_wav_f32(dyn_source)
    print(f"{'mode':<14} | {'kbps':>6} | {'SNR dB':>7} | {'LSD dB':>6} | {'MOS':>4} | "
          f"{'vs CBR':>7}")
    print("-" * 60)
    dyn_target = 192
    cbr = BUILD / f"vbr_dyn_cbr{dyn_target}.ec3"
    _encode_eac3(dyn_source, cbr, dyn_target)
    cbr_snr, cbr_lsd, _hf, cbr_mos = decode_scores(
        dyn_original, cbr, BUILD / f"vbr_dyn_cbr{dyn_target}.wav", perceptual=True)
    cbr_rate = measured_kbps(cbr, seconds)
    rows.append({"mode": "dynamic-cbr", "target_kbps": dyn_target,
                 "measured_kbps": float(cbr_rate), "snr_db": float(cbr_snr),
                 "lsd_db": float(cbr_lsd),
                 "mos_lqo": None if cbr_mos is None else float(cbr_mos)})
    print(f"{'cbr':<14} | {cbr_rate:>6.1f} | {cbr_snr:>7.2f} | {cbr_lsd:>6.2f} | "
          f"{_fmt_mos(cbr_mos):>4} | {0.0:>+7.2f}")
    # The window is the one knob ABR has, so sweep it: too short and the
    # reservoir cannot carry a quiet passage's savings as far as the loud one
    # that needs them, too long and the offset drifts away from the content it
    # is meant to be tracking.
    for window in (8, 16, 32, 64):
        coded = BUILD / f"vbr_dyn_abr_w{window}.ec3"
        _encode_eac3(dyn_source, coded, 192, f"avg:{dyn_target},win:{window}")
        rate = measured_kbps(coded, seconds)
        snr, lsd, _hf2, mos = decode_scores(dyn_original, coded,
                                            BUILD / f"vbr_dyn_abr_w{window}.wav",
                                            perceptual=True)
        rows.append({"mode": "dynamic-abr", "window_frames": window,
                     "target_kbps": dyn_target, "measured_kbps": float(rate),
                     "snr_db": float(snr), "lsd_db": float(lsd),
                     "mos_lqo": None if mos is None else float(mos),
                     "cbr_snr_db": float(cbr_snr)})
        print(f"{'abr win=' + str(window):<14} | {rate:>6.1f} | {snr:>7.2f} | "
              f"{lsd:>6.2f} | {_fmt_mos(mos):>4} | {snr - cbr_snr:>+7.2f}")

    print()
    print("gap columns are positive when ac3forge VBR/ABR beats the comparison at the")
    print("SAME measured rate. MOS-LQO (ViSQOL audio mode, 1-4.75): '-' means")
    print("visqol-python isn't installed - see perceptual_score()'s own docstring.")
    if json_out is not None:
        Path(json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(json_out).write_text(json.dumps({"rows": rows}, indent=2) + "\n")
        print(f"wrote {json_out}")
    return rows


# --- CI gate mode ------------------------------------------------------------
#
# Everything above prints a table for a human to read. This turns the same
# measurement - real material (make_material()/make_material_51(), each
# several real signal types concatenated, decoded from frame 0 onward like
# every other check here - see CONTRIBUTING.md's "test with real audio, from
# frame 1 onward"), decoded by FFmpeg (the same independent oracle
# tools/ci/run_codec_matrix.sh strict-decodes) - into a numeric floor a CI job
# can enforce with a real exit code, no table-reading required.
#
# The floors are regression tripwires, not targets. They sit well below what
# tools/ci/quality_race.py's own tables report today (see README.md's own
# numbers) on purpose: a legitimate encoder change can trade a couple of dB
# one way for a win elsewhere, and this gate must not fail CI over that. A
# genuine regression - the kind this exists to catch - collapses a score by
# many dB, not a fraction of one, so it clears this bar by a wide margin.
CI_STEREO_KBPS = 192
CI_51_KBPS = 256
# The transient leg (make_material_transient) runs at the same stereo rate as
# the stationary one, so the two rows differ only in the material - which is
# the comparison the exponent-run plan has to be read against.
CI_TRANSIENT_KBPS = 192

# (min SNR dB, max LSD dB) per E-AC-3 tool variant, one table per material
# set - 256 kbps split six ways (5.1, decorrelated) is a far tighter
# per-channel budget than 192 kbps split two ways, so the two regimes score
# maybe 20+ dB apart on the same variant and cannot share one floor. Measured
# against a real build (2026-08-09, FFmpeg 8.0.1): stereo/192kbps scored
# 37-40 dB SNR / 4.3-5.9 dB LSD across every variant; 5.1/256kbps scored
# 14-15.5 dB SNR / 7.0-8.7 dB LSD. spx/aht/cpl+spx/all trade waveform
# fidelity for the banded envelope on purpose (see spectral_scores'
# docstring) - that is why their SNR floors are lower and LSD ceilings
# higher than "none"/"cpl" rather than every row sharing one bar.
# "auto" resolves to a different tool set per leg, and since the tool
# decisions read the frame's own spectrum as well as the rate (see
# eac3_frame.cpp's own "What the content says" block) that set depends on the
# material here, not only on the bitrate - so its floor is the floor of the
# sets it can reach rather than a number of its own. Measured 2026-08-23
# against a real build on make_material()/make_material_51(): stereo 41.55 dB
# SNR / 6.08 dB LSD, 5.1 14.29 dB / 7.59 dB, both comfortably inside the bars
# below. A change to the policy that silently flipped either leg to the wrong
# set would land well under them.
#
# One thing this leg is load-bearing for beyond its own numbers: it is
# strict-decoded by FFmpeg, so it is what catches `auto` reaching for a tool
# FFmpeg cannot read. That is not hypothetical - it is how enhanced coupling
# was found to be unshippable in `auto` (docs/library/encoding-eac3.md).
CI_EAC3_THRESHOLDS = {
    "stereo": {
        "none": (28.0, 7.5),
        "auto": (28.0, 8.0),
        "cpl": (28.0, 7.0),
        "spx": (26.0, 7.0),
        "aht": (28.0, 8.0),
        "cpl+spx": (25.0, 7.0),
        "all": (25.0, 7.5),
    },
    "51": {
        "none": (10.0, 11.0),
        "auto": (9.0, 10.5),
        "cpl": (10.0, 10.0),
        "spx": (9.0, 9.5),
        "aht": (10.0, 11.0),
        "cpl+spx": (9.0, 9.5),
        "all": (9.0, 10.5),
    },
    # Transient material, measured 2026-08-23 against a real build (FFmpeg
    # 8.0.1): none/auto/cpl/aht 5.37 dB SNR and 0.94-0.95 dB LSD, the three
    # spectral-extension rows -0.42 to -0.43 dB SNR and 1.81-1.85 dB LSD.
    #
    # The absolute numbers are not comparable to the stationary stereo row's
    # and are not meant to be: a click train over near-silence at 192 kbit/s is
    # a hard thing to code, and a waveform SNR near zero on parametric rows is
    # what a synthesized high band scores by construction. What this row exists
    # for is the exponent-run plan, so its bars sit closer to the measurement
    # than the other rows' do - about 1.4 dB of SNR margin and 0.5 dB of LSD.
    # That is deliberate: every regression this leg actually caught while it
    # was being built cost 1 to 3 dB of SNR and 0.5 to 1.7 dB of LSD, not the
    # many dB a collapse elsewhere would, and a floor loose enough to ignore
    # them would have made the leg pointless.
    "transient": {
        "none": (4.0, 1.5),
        "auto": (4.0, 1.5),
        "cpl": (4.0, 1.5),
        "spx": (-2.0, 2.6),
        "aht": (4.0, 1.5),
        "cpl+spx": (-2.0, 2.6),
        "all": (-2.0, 2.6),
    },
}

# Transient material (make_material_transient): onsets closer together than a
# frame, hard gates, decays that leave a frame's loudest block 20-30 dB above
# its quietest. It exists to hold the exponent-run plan (roadmap EQ1) against a
# regression: one exponent set per frame quantizes every quiet block against a
# scale chosen by the loud one, and this is the material where that costs the
# most. Absolute scores here are far below the stationary stereo row's and are
# not comparable to it - a click train over near-silence is a hard thing to
# code at 192 kbit/s, and the floors are set from a measured build the same way
# every other row's are.
CI_EAC3_MEASURED_NOTE = "measured 2026-08-23 against a real build, FFmpeg 8.0.1"
CI_AC3_MIN_SNR_DB = 30.0

# The AC-3 gate was stereo-only for a long time, which left 5.1 - and with it
# the LFE and the coupling-eligible channel count - with no absolute gate at
# all. Two separate faults have now shipped through that hole: a stale delta
# bit allocation that made real 5.1 streams undecodable, and an LFE pinned to
# one exponent set per frame. Neither was visible to a stereo encode.
#
# The floor is deliberately loose - this material scores about 19.9 dB at
# 448 - because the point is not to police a fraction of a dB. decode_scores()
# runs FFmpeg with -xerror, so a malformed frame fails this gate as a hard
# decode error long before the SNR number is even reached, and that is the
# failure mode both of those bugs actually had.
CI_AC3_51_KBPS = 448
CI_AC3_51_MIN_SNR_DB = 15.0

# Same shape as CI_EAC3_THRESHOLDS, for EAC3_SELF_VARIANTS - measured against
# a real build (2026-08-12) via decode_scores_ours: stereo/192kbps scored
# 37.6 dB SNR / 4.6 dB LSD (ecpl) and 24.1-24.2 dB SNR / 4.6-5.5 dB LSD (tpn,
# ecpl+tpn); 5.1/256kbps scored 8.7-8.8 dB SNR / 7.5 dB LSD (ecpl, ecpl+tpn)
# and 13.8 dB SNR / 9.1 dB LSD (tpn). tpn's material here is the same tone-
# burst-heavy mix every other row uses, not audio shaped around a single
# clean onset the way tests/decoder/test_eac3_decoder.cpp's dedicated unit test is -
# that is why its own floor sits well below ecpl's despite the tool working
# correctly; see that test for a tighter, onset-specific assertion.
CI_EAC3_SELF_THRESHOLDS = {
    "stereo": {
        "ecpl": (28.0, 7.0),
        "tpn": (18.0, 7.5),
        "ecpl+tpn": (18.0, 7.0),
    },
    "51": {
        "ecpl": (6.0, 9.0),
        "tpn": (10.0, 11.0),
        "ecpl+tpn": (6.0, 9.0),
    },
}


def gate(name, ok, detail):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")
    return ok


def race_ci(original, source, original_51, source_51, original_tr, source_tr):
    failures = []

    print(f"=== AC-3 @ {CI_STEREO_KBPS} kbps ===")
    ac3_path = BUILD / f"ci_ac3_{CI_STEREO_KBPS}.ac3"
    run([CLI, "encode", source, ac3_path, str(CI_STEREO_KBPS)])
    snr, _, _, _ = decode_scores(original, ac3_path, BUILD / "ci_ac3.wav")
    if not gate(f"ac3 @ {CI_STEREO_KBPS}kbps", snr >= CI_AC3_MIN_SNR_DB,
                f"SNR {snr:.2f} dB (floor {CI_AC3_MIN_SNR_DB})"):
        failures.append("ac3")

    print(f"=== AC-3 5.1 @ {CI_AC3_51_KBPS} kbps ===")
    ac3_51_path = BUILD / f"ci_ac3_51_{CI_AC3_51_KBPS}.ac3"
    run([CLI, "encode", source_51, ac3_51_path, str(CI_AC3_51_KBPS)])
    snr_51, _, _, _ = decode_scores(original_51, ac3_51_path, BUILD / "ci_ac3_51.wav")
    if not gate(f"ac3 5.1 @ {CI_AC3_51_KBPS}kbps", snr_51 >= CI_AC3_51_MIN_SNR_DB,
                f"SNR {snr_51:.2f} dB (floor {CI_AC3_51_MIN_SNR_DB})"):
        failures.append("ac3-51")

    for label, source_wav, original_pcm, kbps, self_variants in (
        ("stereo", source, original, CI_STEREO_KBPS, EAC3_SELF_VARIANTS),
        ("51", source_51, original_51, CI_51_KBPS, EAC3_SELF_VARIANTS),
        # Transient material runs the FFmpeg-checked variants only: what this
        # leg exists to measure is how the exponent run plan copes with level
        # moving between the blocks of a frame, and ecpl/tpn change neither
        # the plan nor how it is spent. They have their own rows above.
        ("transient", source_tr, original_tr, CI_TRANSIENT_KBPS, ()),
    ):
        print(f"=== E-AC-3 {label} @ {kbps} kbps ===")
        for variant, tools in EAC3_VARIANTS:
            coded = BUILD / f"ci_eac3_{label}_{variant}_{kbps}.ec3"
            cmd = [CLI, "eac3-encode", source_wav, coded, str(kbps)]
            if tools:
                cmd.append(tools)
            run(cmd)
            snr, lsd, _, _ = decode_scores(original_pcm, coded,
                                           BUILD / f"ci_eac3_{label}_{variant}.wav")
            min_snr, max_lsd = CI_EAC3_THRESHOLDS[label][variant]
            ok = snr >= min_snr and lsd <= max_lsd
            if not gate(f"eac3-{label} {variant} @ {kbps}kbps", ok,
                        f"SNR {snr:.2f} dB (floor {min_snr}), "
                        f"LSD {lsd:.2f} dB (ceiling {max_lsd})"):
                failures.append(f"eac3-{label}-{variant}")

        if not self_variants:
            continue
        print(f"=== E-AC-3 {label} @ {kbps} kbps (ecpl/tpn, own-decoder oracle) ===")
        for variant, tools in self_variants:
            coded = BUILD / f"ci_eac3_{label}_{variant}_{kbps}.ec3"
            run([CLI, "eac3-encode", source_wav, coded, str(kbps), tools])
            snr, lsd, _, _ = decode_scores_ours(original_pcm, coded,
                                                BUILD / f"ci_eac3_{label}_{variant}.wav")
            min_snr, max_lsd = CI_EAC3_SELF_THRESHOLDS[label][variant]
            ok = snr >= min_snr and lsd <= max_lsd
            if not gate(f"eac3-{label} {variant} @ {kbps}kbps (self)", ok,
                        f"SNR {snr:.2f} dB (floor {min_snr}), "
                        f"LSD {lsd:.2f} dB (ceiling {max_lsd})"):
                failures.append(f"eac3-{label}-{variant}-self")

    print()
    if failures:
        print(f"{len(failures)} CI gate check(s) failed: {', '.join(failures)}")
        sys.exit(1)
    print("all CI gate checks passed")


# --- Landscape trend mode ----------------------------------------------------
#
# The CI-time half of the external-encoder landscape comparison (see
# tools/generators/gen_external_baseline.py for the local-only half that actually
# invokes FFmpeg's and Dolby DEE's encoders). This mode never invokes
# either: it reads the same three committed fixed WAVs
# gen_external_baseline.py measures FFmpeg/DEE against, encodes them with
# THIS build, and scores everything through decode_scores_ours_fixed (this
# project's own decoder) - the same self-consistency pattern race_ci
# already uses for ecpl/tpn, just applied to every row here rather than a
# few. Kept in sync with gen_external_baseline.py's LEGS by hand (like
# EAC3_VARIANTS/EAC3_SELF_VARIANTS already are between race_eac3 and
# race_ci) - this file must never import that one, since it is explicitly
# the local-only, never-in-CI script and this mode is the opposite: CI-only,
# no external encoder ever invoked.
AUDIO_DIR = REPO / "tests" / "golden" / "audio"


# The programme fixtures ship as FLAC (tools/generators/gen_programme_fixtures.py's
# own docstring says why: 3.1 MB against 5.8 MB of WAV each, under a standing
# repo constraint on fixture bytes). Nothing else in this file, and nothing in
# ac3cli, reads FLAC - so every fixture path goes through this one function,
# which hands back a WAV path either way and only ever shells out for the FLAC
# case. ffmpeg is already a hard dependency of every mode here.
#
# Cached under BUILD by name: `trend` alone reads the same fixture ten-plus
# times across variants, and re-decoding 30 s of FLAC per row would be pure
# waste. Regenerated when the FLAC is newer than the WAV, so editing a fixture
# locally does not leave a stale decode behind.
def materialise_fixture(path):
    path = Path(path)
    if path.suffix != ".flac":
        return path
    out = BUILD / f"fixture_{path.stem}.wav"
    if not out.exists() or out.stat().st_mtime < path.stat().st_mtime:
        BUILD.mkdir(parents=True, exist_ok=True)
        run(["ffmpeg", "-v", "error", "-y", "-i", str(path), "-c:a", "pcm_s16le", str(out)])
    return out


SPEECH_FIXTURE = AUDIO_DIR / "programme_speech_stereo.flac"
MUSIC_FIXTURE = AUDIO_DIR / "programme_music_stereo.flac"

# Kept in sync by hand with tools/generators/gen_external_baseline.py's LEGS -
# see this section's own header for why this file must never import that one.
#
# Two things changed at baseline_version 2. First, real programme material:
# the five reference_* legs are 2.5-3 s of sin()/noise/FIR (see
# gen_programme_fixtures.py for what that costs), and the three programme_*
# ones are 30 s CC0 recordings that roll off the way real material does. The
# synthetic legs are NOT retired - their series go back to the first
# baseline, and breaking that continuity to swap material would throw away
# the history the landscape page exists to show.
#
# Second, rates where the Annex E tools actually run. `auto` turns coupling on
# below 12 + 14n kbit/s per channel and spectral extension below 56 (see
# eac3_frame.cpp's coupling_rate_ceiling/kSpxRateCeiling), and the only stereo
# leg sat at 192 kbit/s - 96 per channel, above both - so `auto` chose no
# tools at all there and the landscape never once compared this project's
# Annex E work against FFmpeg's or DEE's at a rate where it exists. The two
# stereo legs added here bracket both crossovers: 96 kbit/s is 48 per channel
# (spectral extension on, coupling off - it isolates spx) and 64 kbit/s is 32
# per channel (both on). The 5.1 legs already sat below both ceilings at
# 256 kbit/s, which is why no low-rate 5.1 leg is added.
TREND_LEGS = [
    {"name": "ac3-51-448", "codec": "ac3", "kbps": 448, "wav": AUDIO_DIR / "reference_51.wav"},
    {"name": "eac3-stereo-192", "codec": "eac3", "kbps": 192,
     "wav": AUDIO_DIR / "reference_stereo.wav"},
    {"name": "eac3-51-256", "codec": "eac3", "kbps": 256, "wav": AUDIO_DIR / "reference_51.wav"},
    {"name": "eac3-stereo-96", "codec": "eac3", "kbps": 96,
     "wav": AUDIO_DIR / "reference_stereo.wav"},
    {"name": "eac3-stereo-64", "codec": "eac3", "kbps": 64,
     "wav": AUDIO_DIR / "reference_stereo.wav"},
    {"name": "ac3-music-stereo-192", "codec": "ac3", "kbps": 192, "wav": MUSIC_FIXTURE},
    {"name": "eac3-music-stereo-96", "codec": "eac3", "kbps": 96, "wav": MUSIC_FIXTURE},
    {"name": "eac3-speech-stereo-64", "codec": "eac3", "kbps": 64, "wav": SPEECH_FIXTURE},
]


# The encoder's own "header room" refusal (ac3::eac3's budget check - see
# tools/ci/fuzz_eac3_encoder_space.py's REFUSALS, which names this exact
# message): a legitimate outcome at the two crossover legs TREND_LEGS' own
# comment added deliberately, not a defect. eac3-stereo-64's "none" row is
# the known case - 32 kbit/s per channel fits only with both coupling and
# spectral extension on, which "none" turns off - so _trend_encode reports
# it rather than raising, and race_trend prints "n/a" for that cell instead
# of aborting the whole trend run over an outcome the leg exists to show.
_HEADER_ROOM_REFUSAL = "the encoder cannot express this configuration"


def _trend_encode(wav, kbps, codec, tools, out):
    if codec == "ac3":
        run([CLI, "encode", str(wav), str(out), str(kbps)])
        return True
    cmd = [CLI, "eac3-encode", str(wav), str(out), str(kbps)]
    if tools:
        cmd.append(tools)
    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        if _HEADER_ROOM_REFUSAL in result.stderr:
            return False
        raise SystemExit(f"command failed: {' '.join(map(str, cmd))}\n{result.stderr}")
    return True


def race_trend(json_out=None):
    """One "landscape" row per leg - AC-3's automatic tools, or E-AC-3's
    "auto" (the tool set this encoder picks from the per-channel rate - the
    number comparable to FFmpeg's/DEE's own automatic best-effort choices,
    same reasoning as gen_external_baseline.py's invoke_ours) - plus one row
    per applicable EAC3_VARIANTS/EAC3_SELF_VARIANTS entry on the two E-AC-3
    legs, the commit-level per-tool detail. "landscape" and the "auto"
    variant row are the same encode for E-AC-3 - computed once, not twice.

    This used to report "all" instead, which forced every tool on at every
    rate. That is a real tool set a caller can still ask for, and it is
    still one of the variant rows, but it is not what a stream should use:
    at 192 kbit/s stereo it costs about 10 dB of SNR against simply not
    coupling or extending, while at 256 kbit/s 5.1 the same tools are worth
    about 10 dB the other way. Reporting the forced set as the headline
    number measured a choice this encoder was not making.

    Compute-only: no pass/fail gate here (that is what `ci` mode is for),
    just the numbers - persistence to quality-history is a later mode.
    """
    print(f"{'leg':<18} | {'row':<10} | {'SNR dB':>7} | {'LSD dB':>6} | "
          f"{'HF dB':>6} | {'MOS':>4} | {'kbps':>6}")
    print("-" * 75)

    results = []
    ext = {"ac3": "ac3", "eac3": "ec3"}
    for leg in TREND_LEGS:
        name, codec, kbps = leg["name"], leg["codec"], leg["kbps"]
        wav = materialise_fixture(leg["wav"])
        is_eac3 = codec == "eac3"
        original = read_wav_any(wav)
        seconds = len(original) / RATE

        rows = [("landscape", "auto" if is_eac3 else None)]
        if is_eac3:
            rows += list(EAC3_VARIANTS) + list(EAC3_SELF_VARIANTS)

        landscape_cache = {}
        for row_label, tools in rows:
            cache_key = tools if is_eac3 else None
            if cache_key in landscape_cache:
                scored = landscape_cache[cache_key]
            else:
                coded = BUILD / f"trend_{name}_{row_label}.{ext[codec]}"
                if _trend_encode(wav, kbps, codec, tools, coded):
                    wav_scratch = BUILD / f"trend_{name}_{row_label}.wav"
                    snr, lsd, hf, mos = decode_scores_ours_fixed(original, coded, wav_scratch,
                                                                  perceptual=True)
                    kbps_measured = measured_kbps(coded, seconds)
                    scored = (snr, lsd, hf, mos, kbps_measured)
                else:
                    scored = None
                landscape_cache[cache_key] = scored

            if scored is None:
                print(f"{name:<18} | {row_label:<10} | {'n/a':>7} | {'-':>6} | "
                      f"{'-':>6} | {'-':>4} | {'-':>6}")
                continue
            snr, lsd, hf, mos, kbps_measured = scored

            lsd_out = float(lsd) if is_eac3 else None
            hf_out = float(hf) if is_eac3 else None
            # Unlike LSD/HF (spectral_scores' own docstring: banded envelope
            # measures that only mean something for the Annex E tools that
            # trade waveform fidelity for it), ViSQOL's MOS-LQO is a general
            # quality prediction - meaningful on the AC-3 leg's "landscape"
            # row too, so it isn't nulled by is_eac3 the way lsd/hf are.
            mos_out = None if mos is None else float(mos)
            results.append({
                "leg": name, "codec": codec, "bitrate_kbps": kbps, "variant": row_label,
                "snr_db": float(snr), "lsd_db": lsd_out, "hf_db": hf_out, "mos_lqo": mos_out,
                "measured_kbps": float(kbps_measured),
            })
            lsd_str = "-" if lsd_out is None else f"{lsd_out:.2f}"
            hf_str = "-" if hf_out is None else f"{hf_out:+.1f}"
            print(f"{name:<18} | {row_label:<10} | {snr:>7.2f} | {lsd_str:>6} | "
                  f"{hf_str:>6} | {_fmt_mos(mos_out):>4} | {kbps_measured:>6.1f}")
        print()

    if json_out is not None:
        Path(json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(json_out).write_text(json.dumps({"rows": results}, indent=2) + "\n")
        print(f"wrote {json_out}")


# --- Spectrogram images (docs/landscape.md's visual supplement) -------------
#
# Renders one PNG per TREND_LEGS entry - stacked original/ac3forge/FFmpeg/DEE
# panels - for CI to persist to the quality-history branch alongside the
# JSON trend numbers above. Deliberately a separate function, called only
# when a caller passes `trend --spectrogram-dir`, not part of race_trend's
# own per-leg loop: it re-reads what that loop already wrote to BUILD rather
# than threading image state through the scoring path, and it never invokes
# FFmpeg's or DEE's own encoders - only decodes the committed
# tests/golden/external-baseline/<leg>/*.{ac3,ec3} bitstreams, the same
# never-runs-in-CI boundary docs/landscape.md documents for the numbers.


# How many seconds of each leg the spectrogram images cover. See the
# centring code in render_spectrograms() for why this is capped rather than
# rendering whole fixtures.
SPECTROGRAM_SPAN_S = 10.0


def _decode_baseline(coded_path, tag):
    scratch = BUILD / f"spectrogram_{tag}.wav"
    run(["ffmpeg", "-v", "error", "-y", "-i", str(coded_path), "-c:a", "pcm_f32le", str(scratch)])
    return read_wav_f32(scratch)


def _plot_spectrogram(ax, mono, title):
    """Uses _spectrogram() - the same STFT helper spectral_scores() already
    uses for LSD - rather than matplotlib's own specgram, so this file has
    one STFT recipe (NFFT/window/hop), not two disagreeing ones."""
    spec = _spectrogram(np.ascontiguousarray(mono))  # frames x bins, magnitude-squared
    db = 10 * np.log10(np.maximum(spec.T, 1e-12))
    freqs = np.fft.rfftfreq(NFFT, 1.0 / RATE)
    hop = NFFT // 2
    times = np.arange(spec.shape[0]) * hop / RATE
    ax.pcolormesh(times, freqs, db, cmap="magma", vmin=-100, vmax=-10, shading="auto")
    ax.set_ylim(0, 20000)
    ax.set_ylabel("Hz")
    ax.set_title(title, fontsize=10, loc="left")


def render_spectrograms(out_dir):
    """One PNG per TREND_LEGS entry: original / ac3forge / FFmpeg / DEE
    spectrograms stacked. Must run after race_trend()'s own per-leg loop has
    already produced BUILD/trend_<leg>_landscape.wav - this reads that file
    rather than re-encoding.

    DEE's row is skipped per-leg when tests/golden/external-baseline/
    manifest.json marks that leg's DEE score "unverified" (currently both
    5.1 legs, DEE's own Ls-channel-drop bug - see that file) - showing a
    spectrogram next to numbers the project itself doesn't trust would be
    worse than not showing it.

    matplotlib is imported here, not at module scope, so every other mode in
    this file stays matplotlib-free; only a caller that actually asks for
    spectrograms needs it installed.
    """
    import matplotlib  # noqa: PLC0415 - deliberate, see the docstring above
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt  # noqa: PLC0415 - must follow the use("Agg") call

    baseline_root = REPO / "tests" / "golden" / "external-baseline"
    manifest = json.loads((baseline_root / "manifest.json").read_text())
    ext = {"ac3": "ac3", "eac3": "ec3"}
    baseline_dir = baseline_root
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    for leg in TREND_LEGS:
        name, codec = leg["name"], leg["codec"]
        original = read_wav_any(materialise_fixture(leg["wav"]))
        ours_wav = BUILD / f"trend_{name}_landscape.wav"
        if not ours_wav.exists():
            raise SystemExit(f"{ours_wav} missing - render_spectrograms must run after "
                              f"race_trend()'s own encode/decode loop for this leg")

        _, d_ours, _ = align(original, read_wav_f32(ours_wav), **FIXED_ALIGN)
        panels = [("original", original), ("ac3forge", d_ours)]

        if name not in manifest["legs"]:
            raise SystemExit(
                f"leg '{name}' is in TREND_LEGS but not in the committed external baseline "
                f"(baseline_version {manifest['baseline_version']}). Those two lists are kept in "
                "sync by hand - see TREND_LEGS' own comment. Rerun "
                "tools/generators/gen_external_baseline.py locally, with its LEGS updated to "
                "match, and commit the manifest it writes.")
        leg_scores = manifest["legs"][name]["scores"]
        for tool_label in ("ffmpeg", "dee"):
            entry = leg_scores.get(tool_label, {})
            if entry.get("status") == "unverified":
                continue
            coded = baseline_dir / name / f"{tool_label}.{ext[codec]}"
            if not coded.exists():
                continue
            decoded = _decode_baseline(coded, f"{name}_{tool_label}")
            _, d, _ = align(original, decoded, **FIXED_ALIGN)
            panels.append((tool_label, d))

        # Same span for every row - align()'s own overlap trim can differ by
        # a handful of samples between calls - and capped at
        # SPECTROGRAM_SPAN_S, centred, mainly so every leg's image is drawn
        # at a comparable time scale instead of squeezing 30 s of programme
        # material into the width a 3 s synthetic fixture gets. It trims the
        # PNGs too, but only somewhat - most of a programme leg's file size
        # is spectral detail, not duration (1.78 MB whole, 1.43 MB capped,
        # against 0.49 MB for a synthetic leg) - so this is a legibility
        # change first and a size one second. What the image is for, showing
        # what each encoder did to the spectrum, reads the same either way.
        n = min(p[1].shape[0] for p in panels)
        span = int(SPECTROGRAM_SPAN_S * RATE)
        offset = (n - span) // 2 if n > span else 0
        n = min(n, span)

        fig, axes = plt.subplots(len(panels), 1, figsize=(11, 2.0 * len(panels)), sharex=True)
        if len(panels) == 1:
            axes = [axes]
        for ax, (label, data) in zip(axes, panels, strict=True):
            chunk = data[offset:offset + n]
            mono = chunk.mean(axis=1) if chunk.ndim > 1 else chunk
            _plot_spectrogram(ax, mono, f"{name} - {label}")
        axes[-1].set_xlabel("seconds")
        fig.tight_layout()
        out_path = out_dir / f"{name}.png"
        fig.savefig(out_path, dpi=130)
        plt.close(fig)
        print(f"wrote {out_path}")


# --- Object-reconstruction quality mode ---------------------------------------
#
# The only quality series this project has for its OBJECT layer. Every codec
# layer beside it - AC-3, E-AC-3, and each Annex E tool - has a per-commit
# trend row; object reconstruction had exactly one measurement anywhere in
# the tree, tests/oba/test_atmos.cpp's `snr_db > 10.0` against 18-35 dB
# measured, so a 10-20 dB JOC regression passed CI and appeared on no page.
#
# The loop is the one that unit test runs, moved out to the CLI and to a
# committed scene: encode tests/golden/audio/reference_objects.wav (five mono
# object essences as WAV channels) with `atmos-encode`, driven by the
# committed placements in reference_objects.paths, then decode with
# `decode <stream> <bed> <objects_dir>` and score each exported object_NN.wav
# against the source channel it came from. tools/generators/gen_object_scene_wav.py
# generates both committed files and documents the scene.
#
# THERE IS NO EXTERNAL ORACLE FOR OBJECT DECODE AT ALL - a weaker position
# than even ecpl/tpn are in (see decode_scores_ours' docstring). FFmpeg has no
# JOC reconstruction at all, and Dolby's own decoder gates object decoding on
# a keyed authenticity tag this project ships no key for (docs/concepts/
# atmos-joc.md), so it plays these streams as their 5.1 bed and never
# produces objects to compare against. This is a pure self-consistency
# series: this project's own encoder and decoder share one reading of TS 103
# 420, so a genuine regression in either still collapses these numbers, and a
# defect both sides agree on is invisible here. docs/object-quality-trend.md
# says exactly that on the page itself.
OBJECTS_WAV = AUDIO_DIR / "reference_objects.wav"
OBJECTS_PATHS = AUDIO_DIR / "reference_objects.paths"

# Kept in sync by hand with gen_object_scene_wav.py's OBJECT_NAMES (that
# module is stdlib-only and local-only, this one is numpy-based and CI-side -
# the same no-cross-import rule race_trend already follows with
# gen_external_baseline.py). Each name is a trend series' `variant`, so
# renaming one starts a new series rather than continuing the old one.
OBJECT_NAMES = ["broadcast", "comet", "engine", "pod-hi", "pod-lo"]

# Two rates, because they fail differently. At 256 kbit/s the five objects are
# reconstructed out of a bed whose own mantissas are the binding constraint;
# at 448 (AtmosConfig's default) the bed is comfortable and what is left is
# the reconstruction limit itself. Measured against a real build (2026-08-23):
# 9.6-18.6 dB at 256, 11.9-22.7 dB at 448. A regression in bit allocation
# moves the first far more than the second; one in the JOC matrix moves both.
OBJECT_LEGS = [
    {"name": "atmos-objects-256", "kbps": 256},
    {"name": "atmos-objects-448", "kbps": 448},
]

# oba::joc::reconstruct is a DELAYED identity, not an instantaneous one: two
# stacked algorithmic delays - 256 samples from the real encode+decode of
# the bed itself, plus reconstruct()'s own pass over that decoded bed. That
# second delay depends on the domain reconstruct() runs in, and these legs
# exercise the CLI's default (AtmosConfig::joc_domain / DecoderConfig::
# joc_domain = ac3::oba::joc::Domain::kQmf, apps/cli/support.hpp), which is
# ac3::dsp::kQmfDelay = kQmfTaps - kQmfHop = 576 samples (src/forge/include/
# ac3/dsp/qmf.hpp), not the 256-sample MDCT round trip the older
# Domain::kMdctBand path used. 256 + 576 = 832.
# ac3::oba::joc::reconstruction_delay(domain) is the single place either number
# is derived (src/forge/include/ac3/oba/joc.hpp); tests/oba/test_atmos.cpp
# calls it rather than hard-coding a figure, and its comment carries the
# full reasoning. Fixed here rather than searched for by cross-correlation
# the way align() does for the codec legs: the delay is a known property of
# the transform pair, and a correlation search would silently absorb a real
# timing regression as "just a different lag" instead of reporting it as
# the SNR collapse it is.
# Confirmed empirically against this exact scene before being fixed here -
# the correlation peak lands on 832 for every object.
OBJECT_DELAY = 832

# Skipped at each end, in samples: one syncframe of warm-up and cool-down,
# matching the unit test's own kSkip. The decoder's first frame has no
# previous half-block to overlap-add against and the last object frame is the
# tail of one, so scoring either would measure the transform's edges rather
# than the reconstruction.
OBJECT_SKIP = 1536


# A band is "occupied" by an object if its reference energy is within this
# much of the object's loudest band. Everything below the line is treated as
# out-of-band and scored as leakage instead of as spectral distance - see
# object_spectral_scores. 40 dB is a wide net on purpose: it has to keep an
# object's real skirts and harmonics inside, and how much of the spectrum
# that is varies enormously by object. Measured over this scene's five: the
# engine and pod-hi occupy 3 of the 24 Bark bands each, pod-lo 4, broadcast
# 13, and the comet - broadband hiss - all 24, which is why it is the one
# object with no leakage figure at all.
OBJECT_BAND_FLOOR_DB = 40.0


def _fmt_leak(leak):
    """"-" for an object that occupies every band, same convention as
    _fmt_mos' own missing-value cell."""
    return "-" if leak is None else f"{leak:+.1f}"


def object_spectral_scores(o, d):
    """Banded LSD over the bands this object occupies, plus its leakage.

    spectral_scores() as written cannot be used directly on an object.
    Objects are individually NARROW-BAND by nature - this scene's engine is
    58-232 Hz and its pod-hi is 4.7-9.4 kHz - so most of the 24 Bark bands
    hold nothing but the reference's own numerical floor, and any leakage
    from the four other objects into one of those bands is a ratio against
    ~zero. Run unmodified, that reports this scene's objects at 10-38 dB
    "log-spectral distance", which is a true statement about band ratios and
    a useless one about envelope fidelity: it is dominated by bands the
    object was never in. spectral_scores' own frame-level `loud` filter
    exists for exactly this reason one axis over (it drops near-silent
    frames); this is the same idea applied to bands.

    So the two questions are separated and both reported:

    lsd_db  - mean |10 log10(coded/reference)| per band per loud frame, over
              the OCCUPIED bands only. Directly comparable in meaning to
              every other LSD in this file, just restricted to the part of
              the spectrum the object is actually in.
    leak_db - all the coded energy that landed OUTSIDE those bands, against
              all the reference energy inside them. This is the measure
              specific to object coding: the audible failure of a JOC
              reconstruction is another object's content arriving in this
              one, and for well-separated objects that lands where this
              object has no content of its own. Lower is better. None - not
              a large negative number - for an object that occupies every
              band, since there is then nowhere outside for anything to leak
              into and no measurement to report; this scene's comet, a
              broadband hiss, is exactly that case.
    """
    so = _spectrogram(o)
    sd = _spectrogram(d)
    # Same near-silent-frame filter as spectral_scores, and for the same
    # reason: a frame whose band ratios are all floor-against-floor would
    # otherwise swamp the average.
    energy = so.sum(axis=1)
    loud = energy > 1e-6 * max(energy.max(), 1e-30)
    band_o = np.array([so[loud, lo:hi].sum() for lo, hi in BANDS])
    band_d = np.array([sd[loud, lo:hi].sum() for lo, hi in BANDS])
    occupied = band_o >= band_o.max() * 10 ** (-OBJECT_BAND_FLOOR_DB / 10.0)

    lsd = []
    leak = (None if occupied.all()
            else 10 * np.log10(max(band_d[~occupied].sum(), 1e-30) /
                               max(band_o[occupied].sum(), 1e-30)))
    for (lo, hi), inside in zip(BANDS, occupied, strict=True):
        if not inside:
            continue
        eo = so[loud, lo:hi].sum(axis=1) + 1e-12
        ed = sd[loud, lo:hi].sum(axis=1) + 1e-12
        lsd.append(np.mean(np.abs(10 * np.log10(ed / eo))))
    return float(np.mean(lsd)), (None if leak is None else float(leak))


def object_scores(source, recovered, perceptual=False):
    """SNR/LSD/leakage/MOS for one object, at the fixed OBJECT_DELAY.

    source/recovered are 1-D float arrays: the scene channel this object was
    built from, and the object_NN.wav the decoder exported for it. Both are
    trimmed to the common overlap with OBJECT_SKIP samples dropped at each
    end, and stay 1-D - _spectrogram and perceptual_score both read a single
    channel that way, and there is no second channel to average over here
    the way there is on every other mode's material.
    """
    n = min(len(source) - OBJECT_SKIP,
            len(recovered) - OBJECT_DELAY - OBJECT_SKIP) - OBJECT_SKIP
    if n <= 0:
        raise SystemExit("object audio is shorter than the warm-up/cool-down window")
    o = source[OBJECT_SKIP:OBJECT_SKIP + n]
    d = recovered[OBJECT_SKIP + OBJECT_DELAY:OBJECT_SKIP + OBJECT_DELAY + n]
    snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
    lsd, leak = object_spectral_scores(o, d)
    mos = perceptual_score(o, d) if perceptual else None
    return float(snr), lsd, leak, (None if mos is None else float(mos))


def race_objects(json_out=None):
    """One row per (leg, object), plus a `scene` row per leg.

    The `scene` row is the plain mean of that leg's per-object SNR/LSD/leakage
    (and of MOS where every object has one) - a single number per leg for a reader
    who wants "did the object layer move" without reading five rows, and the
    row docs/object-quality-trend.md charts by default. It is a mean of the
    objects, not a separate measurement: an object that collapses on its own
    shows up here diluted by four that did not, which is exactly why the
    per-object rows exist beside it rather than instead of it.
    """
    if not OBJECTS_WAV.exists() or not OBJECTS_PATHS.exists():
        raise SystemExit(f"missing {OBJECTS_WAV} / {OBJECTS_PATHS} - regenerate them with "
                         "python tools/generators/gen_object_scene_wav.py")
    source = read_wav_any(OBJECTS_WAV)
    if source.shape[1] != len(OBJECT_NAMES):
        raise SystemExit(f"{OBJECTS_WAV} has {source.shape[1]} channels, but OBJECT_NAMES lists "
                         f"{len(OBJECT_NAMES)} - regenerate the scene, or fix the list")
    seconds = len(source) / RATE

    print(f"{'leg':<18} | {'object':<10} | {'SNR dB':>7} | {'LSD dB':>6} | "
          f"{'leak dB':>7} | {'MOS':>4} | {'kbps':>6}")
    print("-" * 72)

    results = []
    for leg in OBJECT_LEGS:
        name, kbps = leg["name"], leg["kbps"]
        coded = BUILD / f"objects_{name}.ec3"
        bed = BUILD / f"objects_{name}_bed.wav"
        objects_dir = BUILD / f"objects_{name}"
        # Any previous run's exports are removed first: the decoder writes one
        # object_NN.wav per object it reconstructs, so a run that produced
        # FEWER objects than the last one would otherwise be scored partly
        # against stale files left behind by the earlier run.
        if objects_dir.exists():
            for stale in objects_dir.glob("object_*.wav"):
                stale.unlink()
        run([CLI, "atmos-encode", str(OBJECTS_WAV), str(coded), str(kbps),
             str(len(OBJECT_NAMES)), str(OBJECTS_PATHS)])
        run([CLI, "decode", str(coded), str(bed), str(objects_dir)])
        kbps_measured = measured_kbps(coded, seconds)

        rows = []
        for index, object_name in enumerate(OBJECT_NAMES):
            exported = objects_dir / f"object_{index:02}.wav"
            if not exported.exists():
                raise SystemExit(f"{exported} was not written - the decoder reconstructed fewer "
                                 f"than {len(OBJECT_NAMES)} objects from {coded}")
            recovered = read_wav_f32(exported)[:, 0]
            snr, lsd, leak, mos = object_scores(np.ascontiguousarray(source[:, index]),
                                                np.ascontiguousarray(recovered),
                                                perceptual=True)
            rows.append({"leg": name, "bitrate_kbps": kbps, "variant": object_name, "snr_db": snr,
                        "lsd_db": lsd, "leak_db": leak, "mos_lqo": mos,
                        "measured_kbps": float(kbps_measured)})

        # Every mean is taken over `objects` rather than over `rows`, which
        # the scene row is about to join: averaging a list while appending
        # the average to it is the kind of thing that works today and breaks
        # the moment someone moves the append.
        objects = list(rows)
        every_mos = [r["mos_lqo"] for r in objects]
        every_leak = [r["leak_db"] for r in objects if r["leak_db"] is not None]
        rows.append({
            "leg": name, "bitrate_kbps": kbps, "variant": "scene",
            "snr_db": float(np.mean([r["snr_db"] for r in objects])),
            "lsd_db": float(np.mean([r["lsd_db"] for r in objects])),
            # Averaged over the objects that HAVE a leakage figure: an
            # object occupying every band contributes no measurement rather
            # than a floor value that would drag the scene mean down by
            # hundreds of dB (see object_spectral_scores).
            "leak_db": (float(np.mean(every_leak)) if every_leak else None),
            "mos_lqo": (None if any(m is None for m in every_mos)
                        else float(np.mean(every_mos))),
            "measured_kbps": float(kbps_measured)})

        for row in rows:
            print(f"{row['leg']:<18} | {row['variant']:<10} | {row['snr_db']:>7.2f} | "
                  f"{row['lsd_db']:>6.2f} | {_fmt_leak(row['leak_db']):>7} | "
                  f"{_fmt_mos(row['mos_lqo']):>4} | {row['measured_kbps']:>6.1f}")
        print()
        results.extend(rows)

    print("No external oracle exists for object decode - FFmpeg has no JOC reconstruction, and")
    print("Dolby's own decoder needs a key this project does not ship, so it plays these streams")
    print("as their 5.1 bed. These are self-consistency numbers; see race_objects' own comment.")

    if json_out is not None:
        Path(json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(json_out).write_text(json.dumps({"rows": results}, indent=2) + "\n")
        print(f"wrote {json_out}")


DRP = Path(r"C:\Program Files\Dolby\Dolby Reference Player")


def dolby_decode(coded, wav):
    """Decode with Dolby's own decoder, through the reference player's
    GStreamer elements. Returns False when the player is not installed.

    This is a better oracle than FFmpeg for the Annex E tools, because it is
    the implementation the standard is written to describe rather than one
    more reading of it. It is also the only one that can settle a question
    FFmpeg cannot: whether a tool it does not implement is being emitted
    correctly.
    """
    launch = DRP / "gst-launch-1.0.exe"
    if not launch.exists():
        return False
    env = dict(os.environ)
    env["GST_PLUGIN_PATH"] = str(DRP / "gst-plugins")
    env["PATH"] = f"{DRP};{env.get('PATH', '')}"
    # gst-launch parses one pipeline token per argv entry, so "filesrc" and
    # its property have to arrive separately.
    result = subprocess.run(
        [str(launch), "-q", "filesrc", f"location={Path(coded).as_posix()}",
         "!", "dlbac3parse", "!", "dlbac3dec", "!", "audioconvert",
         "!", "audio/x-raw,format=F32LE", "!", "wavenc",
         "!", "filesink", f"location={Path(wav).as_posix()}"],
        capture_output=True, text=True, env=env, check=False)
    if result.returncode != 0:
        print(f"  (dolby decode failed: {result.stderr.strip().splitlines()[:1]})")
        return False
    return Path(wav).exists()


def agreement_db(a, b):
    """How closely two decodings of the same bits match, in dB.

    Presumes both decodes came from a stream encoded with nodither: §7.3.4
    leaves the actual dither VALUES decoder-defined ("any reasonably random
    sequence"), so once dithflag is really decided from content, two
    independent, spec-correct decoders given the same dithered stream diverge
    in the dithered bins by design, not by bug - which would silently lower
    every number this function reports for no reason related to a regression.
    crosscheck() below asks for nodither on every tools set it puts through
    here so this premise actually holds; see tools/checks/verify_gold_
    reference.sh, which needed the same fix for the same reason.
    """
    a, b = a[:, 0].astype(np.float64), b[:, 0].astype(np.float64)
    probe = b[200000:232768]
    corr = np.correlate(a[199000:234000], probe, mode="valid")
    lag = int(np.argmax(np.abs(corr))) - 1000
    n = min(len(b), len(a) - lag) - 300000
    if n < RATE:
        return float("nan"), lag
    diff = a[200000 + lag:200000 + lag + n] - b[200000:200000 + n]
    return 10 * np.log10(np.sum(b[200000:200000 + n] ** 2)
                         / max(np.sum(diff ** 2), 1e-30)), lag


def crosscheck(original, source):
    """Put every tool set through both decoders and compare them.

    Agreeing with one decoder proves the stream is readable. Agreeing with
    two independent ones - where the second is the reference implementation -
    is the difference between "FFmpeg accepts this" and "this is right".
    """
    print(f"{'tools':<10} | {'ffmpeg dB':>9} | {'dolby dB':>8} | {'agree dB':>8}")
    print("-" * 46)
    for tools in ("none", "cpl", "spx", "aht", "all"):
        coded = BUILD / f"x_{tools}.ec3"
        cmd = [CLI, "eac3-encode", source, coded, "128"]
        # nodither on every tools set - see agreement_db's own comment for
        # why: it keeps this a comparison of the SAME bitstream through two
        # decoders instead of a measurement of dither's own legitimate
        # decoder-to-decoder divergence. "none" has no '+' form of its own
        # (parse_tools() treats it as an exact-match special case that cannot
        # join with another token), so it becomes bare "nodither" rather than
        # "none+nodither".
        cmd.append("nodither" if tools == "none" else f"{tools}+nodither")
        run(cmd)
        ff_wav = BUILD / f"x_{tools}_ff.wav"
        run(["ffmpeg", "-v", "error", "-y", "-xerror", "-err_detect",
             "crccheck+bitstream+buffer+explode", "-i", coded,
             "-c:a", "pcm_f32le", ff_wav])
        ff = read_wav_f32(ff_wav)
        o, d, _ = align(original, ff)
        ff_snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
        dlb_wav = BUILD / f"x_{tools}_dlb.wav"
        if not dolby_decode(coded, dlb_wav):
            print(f"{tools:<10} | {ff_snr:>9.2f} | {'n/a':>8} | {'n/a':>8}")
            continue
        dlb = read_wav_f32(dlb_wav)
        o, d, _ = align(original, dlb)
        dlb_snr = 10 * np.log10(np.sum(o**2) / max(np.sum((d - o) ** 2), 1e-30))
        agree, _ = agreement_db(dlb, ff)
        print(f"{tools:<10} | {ff_snr:>9.2f} | {dlb_snr:>8.2f} | {agree:>8.1f}")


def seam_check(source, spxbegf=4):
    """Is the spectral extension notch where the standard says, and how deep?

    The banded scores cannot see this: the notch removes energy from a handful
    of bins and the band's own scale factor puts it straight back, so LSD and
    SNR move by hundredths either way. What CAN be checked is the decoded
    spectrum itself - the dip has to sit on the first synthesized coefficient
    and deepen with spxattencod. That also answers whether the decoder
    implements the tool at all, which is not a given.
    """
    seam_bin = 25 + 12 * (spxbegf + 2 if spxbegf < 6 else spxbegf * 2 - 3)
    seam_hz = seam_bin * (RATE / 2) / 256
    nfft = 4096
    depths = {}
    for code in (None, 2, 8, 16, 31):
        label = "off" if code is None else f"cod{code}"
        tools = f"spx:{spxbegf}+" + ("noatten" if code is None else f"atten:{code}")
        coded = BUILD / f"seam_{label}.ec3"
        run([CLI, "eac3-encode", source, coded, "128", tools])
        wav = BUILD / f"seam_{label}.wav"
        run(["ffmpeg", "-v", "error", "-y", "-i", coded, "-c:a", "pcm_f32le", wav])
        d = read_wav_f32(wav)[RATE:-RATE, 0]
        frames = len(d) // nfft
        acc = np.zeros(nfft // 2 + 1)
        for i in range(frames):
            acc += np.abs(np.fft.rfft(d[i * nfft:(i + 1) * nfft] * np.hanning(nfft))) ** 2
        depths[label] = acc / frames
    freqs = np.fft.rfftfreq(nfft, 1.0 / RATE)
    base = depths["off"]
    print(f"spectral extension notch, seam at coefficient {seam_bin} = {seam_hz:.0f} Hz")
    print(f"{'spxattencod':>12} | {'notch dB':>9} | {'at Hz':>7}")
    for label, spec in depths.items():
        if label == "off":
            continue
        rel = 10 * np.log10(np.maximum(spec, 1e-30) / np.maximum(base, 1e-30))
        window = (freqs > seam_hz - 500) & (freqs < seam_hz + 500)
        at = int(np.argmin(np.where(window, rel, 0.0)))
        print(f"{label:>12} | {rel[at]:>9.2f} | {freqs[at]:>7.0f}")
        # The dip has to land on the seam, not somewhere else in the band.
        if abs(freqs[at] - seam_hz) > 250:
            print(f"  WARNING: notch is {freqs[at] - seam_hz:+.0f} Hz off the seam")




CPL_HZ = 85 * (RATE / 512.0)


def snr_db(o, d):
    noise = d - o
    return 10 * np.log10(np.sum(o**2) / max(np.sum(noise**2), 1e-30))


def band_measures(o, d, size=2048):
    """SNR below the coupling frequency, level error above it.

    Above it a coupled decoder restores the band's envelope rather than its
    waveform, so a waveform SNR up there measures the tool's premise, not its
    correctness. The level per short window is the thing that must survive -
    and the thing a coupling coordinate carrying the wrong block's scale
    destroys, in exactly the three blocks of six that reuse a coordinate.
    """
    hop = size // 2
    win = np.hanning(size)
    freqs = np.fft.rfftfreq(size, 1.0 / RATE)
    low = freqs < CPL_HZ
    high = ~low
    low_signal = 0.0
    low_noise = 0.0
    pairs = []
    for start in range(0, len(o) - size, hop):
        for ch in range(o.shape[1]):
            spec_o = np.fft.rfft(o[start:start + size, ch] * win)
            spec_d = np.fft.rfft(d[start:start + size, ch] * win)
            low_signal += float(np.sum(np.abs(spec_o[low]) ** 2))
            low_noise += float(np.sum(np.abs(spec_d[low] - spec_o[low]) ** 2))
            pairs.append((float(np.sum(np.abs(spec_o[high]) ** 2)),
                          float(np.sum(np.abs(spec_d[high]) ** 2))))
    # Score only the windows that carry real high-frequency content. A window
    # 40 dB below the loudest one is inaudible up there, and counting it would
    # drown the measurement in the near-silence between events.
    loudest = max((p[0] for p in pairs), default=0.0)
    errors = [abs(10 * np.log10(max(got, 1e-30) / want))
              for want, got in pairs if want > loudest * 1e-4]
    return (10 * np.log10(low_signal / max(low_noise, 1e-30)),
            float(np.mean(errors)) if errors else float("nan"))


def encode_and_decode(source, tag, kbps, couple=False, extra_flag=None):
    """Our encoder, then FFmpeg as the neutral decoder."""
    ac3 = BUILD / f"race_{tag}_{kbps}.ac3"
    wav = BUILD / f"race_{tag}_{kbps}.wav"
    cmd = [CLI, "encode", source, ac3, str(kbps)]
    if couple:
        cmd.append("couple")
    if extra_flag:
        cmd.append(extra_flag)
    run(cmd)
    run(["ffmpeg", "-v", "error", "-y", "-xerror",
         "-err_detect", "crccheck+bitstream+buffer+explode",
         "-i", ac3, "-c:a", "pcm_f32le", wav])
    return read_wav_f32(wav)


def race_coupling(source, original):
    print(f"{'kbps':>5} | {'mode':>6} | {'all dB':>7} | "
          f"{'<8.0k dB':>9} | {'>8.0k err dB':>13}")
    print("-" * 56)
    for kbps in (96, 128, 192, 256):
        scores = {}
        for mode, couple in (("plain", False), ("couple", True)):
            decoded = encode_and_decode(source, f"cpl_{mode}", kbps, couple)
            o, d, _ = align(original, decoded)
            low_snr, high_err = band_measures(o, d)
            scores[mode] = (snr_db(o, d), low_snr, high_err)
            print(f"{kbps:>5} | {mode:>6} | {scores[mode][0]:>7.2f} | "
                  f"{low_snr:>9.2f} | {high_err:>13.2f}")
        low_gain = scores["couple"][1] - scores["plain"][1]
        print(f"{'':>5} | {'delta':>6} | {'':>7} | {low_gain:>+9.2f} | "
              f"{scores['couple'][2] - scores['plain'][2]:>+13.2f}")
    print("\nBaseband delta positive = coupling bought precision where it should.")
    print("Coupled-band error is |level error|, so lower is better either way.")


def race_fast_mdct(source, original):
    """direct-form (fast-mdct=off) vs the default §7.9.4 fast forward MDCT
    (mdct.hpp's `fast` parameter / EncoderConfig::fast_mdct), same shape as
    race_coupling above - originally the quality evidence the fast-MDCT PR's
    owner asked for before making fast the default, kept as the standing
    check that the two paths still agree. mdct512_forward's fast path is
    already verified bit-close (~1e-15 absolute error) against the direct
    form in isolation (tests/core/test_mdct_fast.cpp); what this measures is
    whether that residual ever flips a bap/exponent DECISION enough to show
    up against an independent oracle, at real bitrates, on real (if
    synthetic) material.
    """
    print(f"{'kbps':>5} | {'mode':>6} | {'SNR dB':>7} | {'delta dB':>8}")
    print("-" * 40)
    # Same range race_ac3 uses - below 192 this project's own AC-3 coupling
    # path has a pre-existing "invalid coupling range" decode failure
    # unrelated to fast_mdct (reproduces identically with fast_mdct off),
    # out of scope for the PR this function's evidence belongs to.
    for kbps in (192, 256, 320, 448):
        scores = {}
        # The default IS the fast path now, so it is the direct leg that
        # needs a flag - the inverse of how this table was first gathered.
        for mode, flag in (("direct", "fast-mdct=off"), ("fast", None)):
            decoded = encode_and_decode(source, f"fastmdct_{mode}", kbps, extra_flag=flag)
            o, d, _ = align(original, decoded)
            scores[mode] = snr_db(o, d)
        delta = scores["fast"] - scores["direct"]
        print(f"{kbps:>5} | {'direct':>6} | {scores['direct']:>7.2f} | {'':>8}")
        print(f"{kbps:>5} | {'fast':>6} | {scores['fast']:>7.2f} | {delta:>+8.3f}")
    print("\ndelta = fast SNR - direct SNR vs the original source (both through FFmpeg).")
    print("Near zero is the expected result - see mdct_forward_fast_core's own")
    print("verified-error comment; a large negative delta would be a regression.")


# `--material speech|music` on the interactive modes: score against one of the
# committed 30 s CC0 programme fixtures instead of make_material()'s
# synthesized tones-and-noise. Opt-in, never a default, and deliberately not
# accepted by `ci` or `trend` - `ci` is a hard gate whose floors were set
# against the synthesized material and would all have to move, and `trend`
# already carries the programme fixtures as their own legs (TREND_LEGS) rather
# than as an alternative source for the existing ones.
#
# This is the knob for the encoder-tuning question this project has already
# been caught by once: a bit-allocation or bandwidth policy that looks like a
# win on the synthetic fixtures needs re-measuring on material that is not
# band-limited like they are, and before this there was nowhere to do that
# except by hand (see src/lib/src/encoder/encoder.cpp's chbwcod comment, and
# tools/generators/gen_programme_fixtures.py for the measured spectra).
MATERIALS = {"speech": SPEECH_FIXTURE, "music": MUSIC_FIXTURE}


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "ac3"
    BUILD.mkdir(exist_ok=True)

    material = "synth"
    if "--material" in sys.argv:
        material = sys.argv[sys.argv.index("--material") + 1]
        if material not in MATERIALS and material != "synth":
            raise SystemExit(f"unknown material '{material}' (synth | speech | music)")
        if material != "synth" and which in ("ci", "trend"):
            raise SystemExit(
                f"--material is not accepted by '{which}' mode - see MATERIALS' own comment. "
                "`ci` gates against floors set on the synthesized material; `trend` already "
                "carries the programme fixtures as their own legs.")
        # Rejected rather than silently ignored: this mode replaces `source`
        # with make_material_51() unconditionally, and both programme fixtures
        # are stereo - there is no redistributable native 5.1 programme source
        # (see gen_programme_fixtures.py's own docstring for why an upmix is
        # not a substitute).
        if material != "synth" and which == "eac3-51":
            raise SystemExit(
                "--material is not accepted by 'eac3-51' mode: both programme fixtures are "
                "stereo and this mode is 5.1-only.")

    if material == "synth":
        left, right = make_material()
        source = BUILD / "race_src.wav"
        write_wav_f32(source, left, right)
        original = read_wav_f32(source)
        seconds = len(left) / RATE
    else:
        source = materialise_fixture(MATERIALS[material])
        original = read_wav_any(source)
        seconds = len(original) / RATE
        print(f"material: {MATERIALS[material].name} ({seconds:.1f}s, "
              f"{original.shape[1]} channels)")
    if which == "eac3":
        race_eac3(original, source, seconds)
    elif which == "eac3-51":
        # Coupling's saving scales with the channel count - five high bands
        # collapse into one, where stereo only collapses two - so 5.1 is where
        # it has the most to prove.
        source = BUILD / "race_src51.wav"
        write_wav_f32(source, make_material_51())
        race_eac3(read_wav_f32(source), source, seconds, rates=(192, 256, 384))
    elif which == "fgaincod":
        json_out = None
        if "--json-out" in sys.argv:
            json_out = Path(sys.argv[sys.argv.index("--json-out") + 1])
        # The stereo table's default tool set is `none`: this mode is about
        # one bit-allocation parameter, and coupling or spectral extension
        # moving underneath it would confound the very deltas it reports.
        # --tools auto is the follow-up that asks the same question of the
        # tool set a real stream at these rates would actually carry (`auto`
        # turns spx on below 56 kbit/s a channel and coupling below 12 + 14n),
        # and --rates narrows the sweep to the cells where that matters.
        tools = sys.argv[sys.argv.index("--tools") + 1] if "--tools" in sys.argv else "none"
        rates = FGAINCOD_RATES
        if "--rates" in sys.argv:
            rates = tuple(int(r) for r in sys.argv[sys.argv.index("--rates") + 1].split(","))
        rows = race_fgaincod(original, source, seconds, rates=rates, tools=tools,
                             json_out=json_out)
        # The 5.1 half runs off make_material_51() whatever --material said,
        # for the reason main()'s eac3-51 guard gives: both programme
        # fixtures are stereo and there is no redistributable native 5.1
        # programme source. Opt-in rather than automatic - it is synthetic
        # material answering a cost question the stereo table cannot (the
        # element is ~3x more expensive with six coded channels), and mixing
        # synthetic rows into a real-material table without saying so is
        # exactly how a measurement gets misread later.
        if "--with-51" in sys.argv:
            source_51 = BUILD / "race_src51.wav"
            write_wav_f32(source_51, make_material_51())
            original_51 = read_wav_f32(source_51)
            print("\n=== coupled 5.1, SYNTHETIC material (make_material_51) ===")
            race_fgaincod(original_51, source_51, len(original_51) / RATE,
                          rates=FGAINCOD_RATES_51, tools="cpl", layout="51", nfchans=5,
                          json_out=json_out, rows=rows)
    elif which == "vbr":
        json_out = None
        if "--json-out" in sys.argv:
            json_out = Path(sys.argv[sys.argv.index("--json-out") + 1])
        race_vbr(original, source, seconds, json_out=json_out)
    elif which == "seam":
        seam_check(source)
    elif which == "crosscheck":
        crosscheck(original, source)
    elif which == "couple":
        race_coupling(source, original)
    elif which == "fast-mdct":
        race_fast_mdct(source, original)
    elif which == "ac3":
        race_ac3(original, source, seconds)
    elif which == "eac3-transient":
        # The same table race_eac3 prints for the stationary material, on the
        # material an exponent-run plan actually has something to do on.
        source = BUILD / "race_src_transient.wav"
        write_wav_f32(source, *make_material_transient())
        race_eac3(read_wav_f32(source), source, seconds, rates=(128, 192, 256))
    elif which == "ci":
        source_51 = BUILD / "race_src51.wav"
        write_wav_f32(source_51, make_material_51())
        source_tr = BUILD / "race_src_transient.wav"
        write_wav_f32(source_tr, *make_material_transient())
        race_ci(original, source, read_wav_f32(source_51), source_51,
                read_wav_f32(source_tr), source_tr)
    elif which == "trend":
        json_out = None
        if "--json-out" in sys.argv:
            json_out = Path(sys.argv[sys.argv.index("--json-out") + 1])
        race_trend(json_out=json_out)
        if "--spectrogram-dir" in sys.argv:
            spectrogram_dir = Path(sys.argv[sys.argv.index("--spectrogram-dir") + 1])
            render_spectrograms(spectrogram_dir)
    elif which == "objects":
        json_out = None
        if "--json-out" in sys.argv:
            json_out = Path(sys.argv[sys.argv.index("--json-out") + 1])
        race_objects(json_out=json_out)
    else:
        raise SystemExit(
            f"unknown race '{which}' "
            f"(ac3 | couple | fast-mdct | eac3 | eac3-51 | eac3-transient | "
            f"fgaincod | vbr | seam | "
            f"crosscheck | ci | trend | objects)"
            "\n[--material synth|speech|music] on every mode except ci, trend and eac3-51.")


if __name__ == "__main__":
    main()
