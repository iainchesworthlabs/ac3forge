"""Generates the AC-4 external-baseline fixtures under
tests/golden/external-baseline/ac4-*/dee.ac4, and ac4-manifest.json beside them.

AC-4 has no decode oracle in this project at all (see docs/verification.md's
AC-4 section) - unlike gen_external_baseline.py's AC-3/E-AC-3 legs, there is
no SNR/MOS scoring machinery here, because there is nothing to score a
decode against. What this script produces is simpler: licensed Dolby
Encoding Engine 6.5.4 output for the `ac4::` parser, the AC-4 decoder and
tools/references/ac4_parse.py to be cross-checked against - the same
"somebody else's bitstream" role tests/golden/external-baseline/*/dee.ec3
plays for E-AC-3 (CONTRIBUTING.md's Oracles list, #3).

Legs (ac4 = dee_ac4_encoder.exe, ims = dee_ac4ims_encoder.exe):

  leg                    enc  source            kbps  codec mode; what else the stream carries
  ac4-stereo-64          ac4  reference_stereo    64  ASPX; DE parameters
  ac4-20-music-192       ac4  music_20           192  SIMPLE
  ac4-20-speech-128      ac4  speech_20          128  ASPX, companding off; DE parameters
  ac4-51-film-96         ac4  film_51             96  ASPX_ACPL_3; DE parameters
  ac4-51-music-128       ac4  music_51           128  ASPX_ACPL_2
  ac4-51-music-192       ac4  music_51           192  ASPX
  ac4-51-music-384       ac4  music_51           384  SIMPLE
  ac4-51-drc-ltrt-192    ac4  music_51           192  ASPX; explicit DRC curves, Lt/Rt downmix
  ac4-ims-music-64-2997  ims  music_51            64  ASPX; 29.97 fps (1536 samples)
  ac4-ims-film-96-24     ims  film_51             96  ASPX; 24 fps (1920 samples); DE parameters
  ac4-ims-music-128-25   ims  music_51           128  ASPX; 25 fps (2048 samples)

ac4-stereo-64 is the version 1 leg. The ten others were chosen from a census
of 119 DEE 6.5.4 AC-4 encodes of 20 s material, in which the codec mode
depended only on the layout and the data rate (music, speech and speech over
music never changed it): stereo is ASPX up to 144 kbps, with companding on at
48-96 and off at 128-144, and SIMPLE from 192; 5.1 is ASPX_ACPL_3 at 96,
ASPX_ACPL_2 at 128-144, ASPX at 192-320 and SIMPLE from 384. Of the ten, the
six dee_ac4_encoder legs with default options each use the lowest rate that
gives their mode (for ac4-20-speech-128, ASPX with companding off). dee_ac4_encoder
always writes frame_rate_index 13 (2048 samples) and has no frame-rate option,
so the other frame lengths come from dee_ac4ims_encoder's --target-fps.

ac4-51-drc-ltrt-192 is one encode with both option sets in one --encoder
argument: per-device DRC profiles that differ (film_standard, speech for
headphones, music_light for home theatre) and the Lt/Rt preferred downmix
with explicit mix levels. dee_ac4_encoder otherwise runs with its defaults,
including measure_and_correct loudness at -24 LKFS. dee_ac4ims_encoder's
default mode is general, which sends advanced dialogue enhancement data in
the I-frames of all three IMS legs; measure_only is its only loudness mode.

Every IMS stream signals presentation_version 2 with channel_mode code
0b1111000, which TS 103 190-2 Table 56 maps to 7.0, and codes a
channel_pair_element: walked as 7.0, 5.0 or 5.1, at least every I-frame fails
its substream size checks; walked as stereo, every frame ends exactly. The
IMS legs' codec_mode comes from the stereo walk.

Sources. The new legs encode 3.0 s cuts of the committed programme fixtures,
built the way the census built its 20 s sources and written as 24-bit PCM
WAVs (format tag 1) into --scratch-dir, never into the tree. Both encoders
accept 3.0 s of input.

  music_20   programme_music_stereo.flac, 0-3 s.
  speech_20  programme_speech_stereo.flac, 0-3 s.
  music_51   L R C LFE Ls Rs. L/R are the music at 0-3 s; C is its mono mix
             at 5-8 s x 0.6; LFE is its mono mix at 0-3 s, low-passed at
             120 Hz, x 0.8; Ls/Rs are the music at 10-13 s x 0.5. Each pair
             comes from a different excerpt, so no channel is a copy of
             another.
  film_51    The speech's mono mix (0-3 s) in C over music_51's bed, with
             L/R x 0.45, Ls/Rs x 0.35 and the same LFE.

The FLACs are decoded with ffmpeg, the route quality_race.py's
materialise_fixture() takes (that function caches under build/, so it is not
called here). Every channel except the LFE is sample-identical to the first
3 s of the census source it comes from. The LFE differs because it is
filtered with the linear-phase windowed-sinc low-pass of
tools/listening/gen_listening_stimuli.py, where the census used scipy's
4th-order Butterworth; no generator in tools/generators/ uses scipy. A mix
peaking above 0.98 would be scaled down to 0.98, as in the census; none of
these peaks above 0.52.

Channel order is L R C LFE Ls Rs, which dee_ac4ims_encoder's help calls a
5.1 WAVE file's "SMPTE channel order" and which both encoders' wav_list input
uses. gen_external_baseline.py records dee_ddp_encoder losing Ls from a
single 6-channel WAV. The AC-4 encoders read a single 6-channel WAV the same
way as a wav_list of its channels: music_51 encoded both ways gives
byte-identical streams from dee_ac4_encoder at 192 kbps and from
dee_ac4ims_encoder at 128 kbps and 25 fps, and replacing Ls with silence in
the wav_list changes both streams.

Manifest. baseline_version 2 (see BASELINE_VERSION). One entry per leg:

  encoder, source, output_channel_layout, bitrate_kbps, options
      What was encoded and how. options are the DEE arguments beyond input,
      output, layout and data rate; [] is DEE's defaults. The IMS encoder
      takes no layout argument; "IMS" names its immersive-stereo output.
      ac4-stereo-64 also keeps its version 1 source_wav.
  duration_s, size_bytes
      The source WAV's length and the stream's size.
  frame_count, frame_rate_index, frame_len_base
      Read from the bytes on every run with tools/references/ac4_parse.py,
      which also requires every sync frame's CRC and TOC to read.
      frame_len_base is TS 103 190-1 Table 83's, for 48 kHz.
  codec_mode, explicit_drc_curves, custom_downmix_data, de_parameters,
  advanced_de_data
      What a walk of every frame of the stream found. The reference parser
      stops at substream framing and cannot recompute these, so LEGS records
      them and main() copies them. The walk was a local extension of that
      parser (not in the repository): it reads each audio substream up to
      its first entropy-coded field plus its metadata(), reads the
      presentation substream in full, and requires every substream walk to
      end exactly on the size the substream index table gives it. A DEE
      release that changes a stream needs the walk repeated before these
      values are trusted again. Each is defined as:
        codec_mode           the audio substream's stereo_codec_mode or
                             5_X_codec_mode, the same in every frame.
        explicit_drc_curves  drc_config() transmits a drc_compression_curve()
                             for at least one DRC decoder mode, instead of
                             every mode naming a default profile.
        custom_downmix_data  custom_dmx_data() sets b_stereo_dmx_coeff or
                             b_cdmx_data_present in at least one frame. Every
                             dee_ac4_encoder 5.1 stream does in its I-frames,
                             with -3 dB LoRo/LtRt gains and LoRo preferred
                             unless the options say otherwise.
        de_parameters        dialog_enhancement() sends de_par sets
                             (de_channel_config not 0, de_keep_data_flag 0)
                             in at least one frame.
        advanced_de_data     the presentation substream's additional data
                             carries advanced_de_data() in at least one
                             frame.

Usage (repo root):
  python tools/generators/gen_ac4_baseline.py [--scratch-dir DIR] [--with-5114 WAV]

Never run in CI - see guard_not_ci(), the same rule
gen_external_baseline.py's own copy of this function states.
"""

import argparse
import json
import os
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

# tools/references/ac4_parse.py, the independent transcription of the AC-4
# sync frame and TOC, reads frame counts and frame_rate_index back out of
# every encode.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "references"))
import ac4_parse

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO = REPO / "tests" / "golden" / "audio"
OUT = REPO / "tests" / "golden" / "external-baseline"
SCRATCH = REPO / "build" / "ac4_baseline_scratch"

DEE_DIR = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir")
AC4 = "dee_ac4_encoder"
IMS = "dee_ac4ims_encoder"

# Bump by hand whenever this script is rerun to regenerate the baseline
# against a new DEE release - the same discipline
# gen_external_baseline.py's BASELINE_VERSION follows.
#
# 2: ten legs added (see the module docstring) and the manifest extended to
#    describe each stream. ac4-stereo-64's bytes are the version 1 bytes:
#    main() now checks a fresh encode against the committed file instead of
#    overwriting it.
BASELINE_VERSION = 2

RATE = 48000
SECONDS = 3.0

# TS 103 190-1 Table 83, 48 kHz: frame_rate_index -> frame_len_base in samples.
FRAME_LEN_BASE = {0: 1920, 1: 1920, 2: 2048, 3: 1536, 4: 1536, 5: 960, 6: 960, 7: 1024,
                  8: 768, 9: 768, 10: 512, 11: 384, 12: 384, 13: 2048}

SOURCE_DESCRIPTIONS = {
    "reference_stereo": "reference_stereo.wav: the committed 3.0 s synthetic stereo reference",
    "music_20": "music 2.0: programme_music_stereo.flac 0-3 s",
    "speech_20": "speech 2.0: programme_speech_stereo.flac 0-3 s",
    "music_51": "music 5.1 bed from programme_music_stereo.flac: L R 0-3 s, C mono 5-8 s x 0.6, "
                "LFE mono 0-3 s low-passed at 120 Hz x 0.8, Ls Rs 10-13 s x 0.5",
    "film_51": "film 5.1: programme_speech_stereo.flac mono 0-3 s in C over the music_51 bed "
               "with L R x 0.45, Ls Rs x 0.35 and the same LFE",
}

WALKED_KEYS = ("codec_mode", "explicit_drc_curves", "custom_downmix_data", "de_parameters",
               "advanced_de_data")

_DRC_AND_LTRT = ("drc_profile=film_standard:drc_profile_portable_hp=speech:"
                 "drc_profile_home_theatre=music_light:preferred_downmix_mode=ltrt:"
                 "ltrt_cmix=-6:ltrt_smix=-inf:loro_cmix=0:loro_smix=-6")

# "walked" holds what walking every frame of the committed stream found (see
# the module docstring's Manifest section). main() re-reads frame_rate_index
# from the bytes and stops if it disagrees; the rest is copied as recorded.
LEGS = [
    # tests/ac4/test_ac4.cpp pins this stream's frame count and its
    # MediaInfo-checked TOC fields; tests/cli and fuzz/generate-seeds.sh read
    # it too. "pinned": main() refuses to replace it with different bytes.
    {"name": "ac4-stereo-64", "encoder": AC4, "source": "reference_stereo", "layout": "stereo",
     "kbps": 64, "options": [], "pinned": True,
     "walked": {"frame_rate_index": 13, "codec_mode": "ASPX", "explicit_drc_curves": False,
                "custom_downmix_data": False, "de_parameters": True,
                "advanced_de_data": False}},
    {"name": "ac4-20-music-192", "encoder": AC4, "source": "music_20", "layout": "stereo",
     "kbps": 192, "options": [],
     "walked": {"frame_rate_index": 13, "codec_mode": "SIMPLE", "explicit_drc_curves": False,
                "custom_downmix_data": False, "de_parameters": False,
                "advanced_de_data": False}},
    {"name": "ac4-20-speech-128", "encoder": AC4, "source": "speech_20", "layout": "stereo",
     "kbps": 128, "options": [],
     "walked": {"frame_rate_index": 13, "codec_mode": "ASPX", "explicit_drc_curves": False,
                "custom_downmix_data": False, "de_parameters": True,
                "advanced_de_data": False}},
    {"name": "ac4-51-film-96", "encoder": AC4, "source": "film_51", "layout": "5.1",
     "kbps": 96, "options": [],
     "walked": {"frame_rate_index": 13, "codec_mode": "ASPX_ACPL_3",
                "explicit_drc_curves": False, "custom_downmix_data": True,
                "de_parameters": True, "advanced_de_data": False}},
    {"name": "ac4-51-music-128", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 128, "options": [],
     "walked": {"frame_rate_index": 13, "codec_mode": "ASPX_ACPL_2",
                "explicit_drc_curves": False, "custom_downmix_data": True,
                "de_parameters": False, "advanced_de_data": False}},
    {"name": "ac4-51-music-192", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 192, "options": [],
     "walked": {"frame_rate_index": 13, "codec_mode": "ASPX", "explicit_drc_curves": False,
                "custom_downmix_data": True, "de_parameters": False,
                "advanced_de_data": False}},
    {"name": "ac4-51-music-384", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 384, "options": [],
     "walked": {"frame_rate_index": 13, "codec_mode": "SIMPLE", "explicit_drc_curves": False,
                "custom_downmix_data": True, "de_parameters": False,
                "advanced_de_data": False}},
    {"name": "ac4-51-drc-ltrt-192", "encoder": AC4, "source": "music_51", "layout": "5.1",
     "kbps": 192, "options": ["--encoder", _DRC_AND_LTRT],
     "walked": {"frame_rate_index": 13, "codec_mode": "ASPX", "explicit_drc_curves": True,
                "custom_downmix_data": True, "de_parameters": False,
                "advanced_de_data": False}},
    {"name": "ac4-ims-music-64-2997", "encoder": IMS, "source": "music_51", "layout": "IMS",
     "kbps": 64, "options": ["--target-fps", "29.97"],
     "walked": {"frame_rate_index": 3, "codec_mode": "ASPX", "explicit_drc_curves": False,
                "custom_downmix_data": False, "de_parameters": False,
                "advanced_de_data": True}},
    {"name": "ac4-ims-film-96-24", "encoder": IMS, "source": "film_51", "layout": "IMS",
     "kbps": 96, "options": ["--target-fps", "24"],
     "walked": {"frame_rate_index": 1, "codec_mode": "ASPX", "explicit_drc_curves": False,
                "custom_downmix_data": False, "de_parameters": True,
                "advanced_de_data": True}},
    {"name": "ac4-ims-music-128-25", "encoder": IMS, "source": "music_51", "layout": "IMS",
     "kbps": 128, "options": ["--target-fps", "25"],
     "walked": {"frame_rate_index": 2, "codec_mode": "ASPX", "explicit_drc_curves": False,
                "custom_downmix_data": False, "de_parameters": False,
                "advanced_de_data": True}},
]

# The 5.1.4 leg needs a 10-channel (L R C LFE Ls Rs Ltm Rtm Lbm Rbm) source
# WAV in SMPTE order, which nothing under tests/golden/audio/ provides (the
# committed fixtures top out at reference_51.wav's 6 channels). Rather than
# commit a synthetic 10-channel WAV solely for this one generator to consume
# once, this leg is opt-in: pass --with-5114 <path-to-10ch-wav> to include it.
# Nothing has walked its output, so its walked values are recorded as null.


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


def cut(x, start_s):
    """SECONDS of x, starting start_s seconds in."""
    first = round(start_s * RATE)
    count = round(SECONDS * RATE)
    if first + count > len(x):
        raise SystemExit(f"a {SECONDS} s cut at {start_s} s runs past the end of the fixture")
    return x[first:first + count]


def lowpass(x, cutoff_hz=120.0, taps=2047):
    """Linear-phase windowed-sinc low-pass; mode="same" removes its group delay."""
    n = np.arange(taps) - (taps - 1) / 2.0
    kernel = 2.0 * (cutoff_hz / RATE) * np.sinc(2.0 * (cutoff_hz / RATE) * n) * np.blackman(taps)
    return np.convolve(x, kernel / kernel.sum(), mode="same")


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


def build_sources(source_dir):
    """Writes the new legs' source WAVs (see the module docstring's Sources)."""
    source_dir.mkdir(parents=True, exist_ok=True)
    music = read_fixture(AUDIO / "programme_music_stereo.flac", source_dir)
    speech = read_fixture(AUDIO / "programme_speech_stereo.flac", source_dir)
    music_mono = music.mean(axis=1)
    left, right = cut(music, 0.0).T
    surround_left, surround_right = cut(music, 10.0).T
    lfe = 0.8 * lowpass(cut(music_mono, 0.0))
    mixes = {
        "music_20": [left, right],
        "speech_20": list(cut(speech, 0.0).T),
        "music_51": [left, right, 0.6 * cut(music_mono, 5.0), lfe,
                     0.5 * surround_left, 0.5 * surround_right],
        "film_51": [0.45 * left, 0.45 * right, cut(speech.mean(axis=1), 0.0), lfe,
                    0.35 * surround_left, 0.35 * surround_right],
    }
    paths = {}
    for name, columns in mixes.items():
        paths[name] = source_dir / f"{name}.wav"
        write_wav24(paths[name], columns)
    return paths


def encode(leg, wav, scratch_dir):
    """One DEE encode; returns the stream's bytes.

    DEE keeps its temporary files in the current directory (--temp-dir
    defaults to it), and the dee-dir installation directory itself is not
    writable - so each leg runs from its own directory under the scratch dir,
    which also keeps each leg's DEE output apart. dee_ac4_encoder refuses to
    start without --output-manifest; dee_ac4ims_encoder has no such option,
    and no layout option either.
    """
    work = scratch_dir / leg["name"]
    work.mkdir(parents=True, exist_ok=True)
    out = work / "dee.ac4"
    out.unlink(missing_ok=True)
    cmd = [str(dee_exe(leg["encoder"])), "--input-format", leg.get("input_format", "wav"),
           "-i", str(wav), "-o", str(out), "--overwrite", "1"]
    if leg["encoder"] == AC4:
        cmd += ["--output-channel-layout", leg["layout"],
                "--output-manifest", str(work / "dee_manifest.json")]
    cmd += ["--data-rate", str(leg["kbps"]), *leg["options"]]
    result = subprocess.run(cmd, cwd=work, capture_output=True, text=True, check=False)
    log = work / "dee_output.txt"
    log.write_text(subprocess.list2cmdline(cmd) + "\n\n" + result.stdout + result.stderr)
    if result.returncode != 0 or not out.is_file():
        raise SystemExit(f"{leg['name']}: DEE exited {result.returncode} - see {log}\n"
                         + (result.stdout + result.stderr)[-2000:])
    return out.read_bytes()


def describe(name, data):
    """(frame_count, frame_rate_index), read with tools/references/ac4_parse.py.

    Every sync frame has to pass its CRC and have a TOC and substream index
    table that read, and frame_rate_index has to stay the same throughout.
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


def manifest_entry(leg, wav, description, data, frame_count, frame_rate_index, built):
    with wave.open(str(wav), "rb") as r:
        duration_s = r.getnframes() / r.getframerate()
    entry = {"encoder": leg["encoder"], "source": description}
    if not built:
        entry["source_wav"] = (str(wav.relative_to(REPO)) if wav.is_relative_to(REPO)
                               else str(wav))
    entry.update({
        "output_channel_layout": leg["layout"],
        "bitrate_kbps": leg["kbps"],
        "options": leg["options"],
        "duration_s": duration_s,
        "size_bytes": len(data),
        "frame_count": frame_count,
        "frame_rate_index": frame_rate_index,
        "frame_len_base": FRAME_LEN_BASE[frame_rate_index],
    })
    walked = leg.get("walked") or {}
    entry.update({key: walked.get(key) for key in WALKED_KEYS})
    return entry


def main():
    guard_not_ci()
    parser = argparse.ArgumentParser(description="Generate the AC-4 external-baseline fixtures.")
    parser.add_argument("--scratch-dir", type=Path, default=SCRATCH,
                        help="where the source WAVs, encodes and DEE logs go "
                             "(default: build/ac4_baseline_scratch)")
    parser.add_argument("--with-5114", type=Path, metavar="WAV",
                        help="also encode the ac4-5114 leg from this 10-channel WAV")
    args = parser.parse_args()

    for encoder in (AC4, IMS):
        if not dee_exe(encoder).exists():
            raise SystemExit(f"{dee_exe(encoder)} not found - this generator only runs on a "
                             "machine with Dolby Media Encoder installed.")
    versions = {dee_version(encoder) for encoder in (AC4, IMS)}
    if len(versions) != 1:
        raise SystemExit(f"{AC4} and {IMS} report different DEE versions: {sorted(versions)}")
    scratch = args.scratch_dir.resolve()

    wavs = build_sources(scratch / "sources")
    built = set(wavs)
    wavs["reference_stereo"] = AUDIO / "reference_stereo.wav"
    descriptions = dict(SOURCE_DESCRIPTIONS)
    legs = list(LEGS)
    if args.with_5114:
        wavs["cbi_5114"] = args.with_5114.resolve()
        descriptions["cbi_5114"] = "10-channel 5.1.4 WAV passed with --with-5114"
        legs.append({"name": "ac4-5114", "encoder": AC4, "input_format": "cbi_wav",
                     "source": "cbi_5114", "layout": "5.1.4", "kbps": 256, "options": []})

    # Every leg is encoded and checked before anything in the tree changes, so
    # a refusal or a failed check part-way through leaves the committed
    # streams and manifest as they were.
    encoded = []
    for leg in legs:
        data = encode(leg, wavs[leg["source"]], scratch)
        frame_count, frame_rate_index = describe(leg["name"], data)
        expected = (leg.get("walked") or {}).get("frame_rate_index", frame_rate_index)
        if frame_rate_index != expected:
            raise SystemExit(f"{leg['name']}: frame_rate_index {frame_rate_index}, but LEGS "
                             f"records {expected} - walk this stream again before trusting "
                             "its recorded values.")
        dest = OUT / leg["name"] / "dee.ac4"
        if leg.get("pinned") and dest.is_file() and dest.read_bytes() != data:
            raise SystemExit(f"{leg['name']}: this encode differs from the committed "
                             f"{dest.relative_to(REPO)}, which tests pin. Replace it by hand, "
                             "together with the tests that read it, if that is intended.")
        encoded.append((leg, data, frame_count, frame_rate_index))

    manifest = {"baseline_version": BASELINE_VERSION, "dee_version": versions.pop(), "legs": {}}
    for leg, data, frame_count, frame_rate_index in encoded:
        dest = OUT / leg["name"] / "dee.ac4"
        if dest.is_file() and dest.read_bytes() == data:
            print(f"unchanged {dest} ({len(data)} bytes)")
        else:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            print(f"wrote {dest} ({len(data)} bytes)")
        manifest["legs"][leg["name"]] = manifest_entry(
            leg, wavs[leg["source"]], descriptions[leg["source"]], data, frame_count,
            frame_rate_index, leg["source"] in built)

    manifest_path = OUT / "ac4-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {manifest_path}")


if __name__ == "__main__":
    main()
