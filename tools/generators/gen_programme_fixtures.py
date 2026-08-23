"""Real-programme fixtures: full-band speech and music, from redistributable sources.

Every landscape and trend number this project publishes used to rest entirely
on tools/generators/gen_gold_reference_wav.py's output - 2.5-3 s of sin(),
pseudo-random noise and boxcar-FIR smoothing. That is a fixture, not
programme material, and src/lib/src/encoder/encoder.cpp records for real what
tuning against it costs: narrowing the encoder's bandwidth to 14.7 kHz looked
like a 2.1 dB SNR win on reference_51.wav and would have made a 448 kbit/s
AC-3 encoder plainly worse to listen to.

Measured on the four committed fixtures as they ship, using the same STFT the
rest of the tooling uses (mean power per band in dB, relative to each
fixture's own 200 Hz - 2 kHz mean):

                     4-8k    8-12k   12-14.7k  14.7-16k  16-18k   18-20k   20-24k
  reference_51      -39.3   -43.5    -46.1     -45.6     -47.0    -47.6    -47.7
  reference_stereo  -10.5   -35.4    -38.6     -37.5     -39.9    -40.8    -40.2
  programme_music   -27.9   -43.3    -75.9     -85.6     -90.2    -91.3    -91.3
  programme_speech  -19.6   -35.4    -44.6     -60.1     -87.8    -89.0    -89.1

The synthetic pair is FLAT from 12 kHz to Nyquist: a boxcar FIR rolls off far
too slowly to stop white noise, so both fixtures carry a noise plateau across
the whole top octave at roughly the same level as the content below it. That
is the shape no real programme material has, and it is what makes discarding
the top 9 kHz nearly free there. Real material rolls off monotonically - the
music by 48 dB from 8 kHz to 16 kHz, without a brick wall anywhere - so the
same decision costs real energy, which is exactly the thing the synthetic
fixtures cannot show.

The programme fixtures' own flat tail above about 17 kHz is not that plateau
returning: it is 16-bit quantisation noise, and it sits at -90 dB where the
synthetic fixtures' plateau sits at -47 dB. In the 24-bit sources those bands
measure -96/-110/-118 dB (music) and -91/-93/-94 dB (speech), so the real
rolloff continues past where a 16-bit file can show it. 16 bits is kept
anyway: it matches the existing fixtures, it is the depth real AC-3 source
material arrives at, and 24-bit versions of these two are 6.9 MB against
3.1 MB. 43 dB below the synthetic plateau is far below anything this encoder
allocates bits to at any rate these legs use.

Both fixtures below are stereo. This project has no redistributable native
5.1 programme source, so the 5.1 legs stay synthetic; a matrix upmix of a
stereo recording would put derived, correlated content in the surrounds and
say more about the upmix than about the encoder.

Sources (see tests/golden/audio/corpus.json for the machine-readable copy,
and tools/generators/README.md for the human-readable licence record):

  programme_speech_stereo.flac
    "Sally Mann at VMFA 2024-12-05", Wikimedia Commons, CC0 1.0 Universal
    (public-domain dedication). 48 kHz, 24-bit, stereo, 31.25 s FLAC:
    unscripted connected speech recorded in a room, not a studio corpus.
    Trimmed to 30 s and peak-normalised (see NORMALISE_PEAK).
    Measured limitation, recorded here rather than left to be discovered by
    whoever tunes an HF policy against it: the source has a filter cliff at
    about 16 kHz (-60 dB at 14.7-16 kHz, -91 dB above it), so it is full-band
    in the sense that matters - a natural, monotonic rolloff rather than a
    noise plateau - but it is NOT evidence about anything above 16 kHz. Use
    programme_music_stereo.flac for that; it rolls off cleanly to 24 kHz.

  programme_music_stereo.flac
    Mendelssohn, Symphony No. 4 "Italian", IV. Saltarello (Presto), from
    Musopen's Kickstarter recordings, CC0 1.0 Universal. 48 kHz, 24-bit,
    stereo ALAC, 367.5 s. The excerpt is 30-60 s, chosen for spanning quiet
    to loud inside one window rather than for being the loudest 30 s
    available - dynamics are what a bit allocator is being asked about.

This script does NOT download anything. Both sources are fetched by hand
once, verified against the SHA-256 in corpus.json, and kept OUT of the repo
(they are 60 MB and 2.2 MB respectively; only the trimmed 30 s excerpts are
committed). Run it with --source-dir pointing at wherever they were put; it
prints the exact URL for anything missing.

Committed as FLAC, not WAV: 16-bit 48 kHz stereo at 30 s is 5.8 MB of WAV
each and about 3 MB of FLAC, and this repo has a standing constraint on how
much fixture material it carries. Consumers materialise a WAV under build/ on
demand - tools/ci/quality_race.py's materialise_fixture(), which every mode
that reads a fixture goes through - so nothing else in the tree has to learn
about FLAC. ffmpeg is already a hard dependency of every one of those callers.

Usage (repo root):
  python tools/generators/gen_programme_fixtures.py --source-dir <dir>
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "tests" / "golden" / "audio"
MANIFEST = OUT / "corpus.json"

RATE = 48000

# Peak-normalisation target for both fixtures, in linear full scale (-3 dBFS).
#
# Not cosmetic. The speech source peaks at 0.112 (-19 dBFS) because it is a
# real room recording made at a sensible input gain, and encoding it as-is
# would spend most of the comparison measuring how this encoder handles a very
# quiet signal rather than how it handles speech. Both fixtures get the same
# target so the two legs' absolute SNR numbers are comparable to each other
# and to the existing synthetic legs, and -3 dBFS leaves the headroom a
# decoder's own downmix/DRC path wants. This is a scalar gain: it moves
# nothing in the relative spectrum, which is the property these fixtures
# exist to carry.
NORMALISE_PEAK = 0.7079

# Bump when any entry in SOURCES changes - a different source file, a
# different excerpt window, a different normalisation. tests/golden/audio/
# corpus.json carries this, tools/checks/check_corpus.py enforces the hashes
# under it, and a PR diff that moves it is saying "the fixtures themselves
# changed", which is a different kind of review from "the encoder changed".
CORPUS_VERSION = 1

SOURCES = [
    dict(
        fixture="programme_speech_stereo.flac",
        source_file="speech_src.flac",
        source_sha256="6397603f159f0cf9d3b24053021aa9684fb7fbc93ce098fa9c49222044abb0b4",
        source_url="https://upload.wikimedia.org/wikipedia/commons/0/04/"
                   "Sally_Mann_at_VMFA_2024-12-05.flac",
        source_page="https://commons.wikimedia.org/wiki/File:Sally_Mann_at_VMFA_2024-12-05.flac",
        title="Sally Mann at VMFA, 2024-12-05",
        licence="CC0-1.0",
        licence_url="https://creativecommons.org/publicdomain/zero/1.0/",
        attribution="Not required (CC0). Recorded and dedicated to the public "
                    "domain by the Wikimedia Commons uploader.",
        kind="speech",
        start_s=0.5,
        duration_s=30.0,
    ),
    dict(
        fixture="programme_music_stereo.flac",
        source_file="music_src.m4a",
        source_sha256="4f8142ddba00a498625bfc456d2ff027e35837e51574ee7f2b570cb44be23776",
        source_url="https://archive.org/download/MusopenKickstarterRecordingsLossless/"
                   "Musopen%20DVD%20%28lossless%29.zip/"
                   "Musopen%20DVD%20%28lossless%29%2FMendelssohn%20-%20Italian%20Symphony%2F"
                   "Symphon%20No.%204%20in%20A%20Major%2C%20Op.%2090%20%27Italian%27%20-%20"
                   "IV.%20Saltarello%20%28Presto%29.m4a",
        source_page="https://archive.org/details/MusopenKickstarterRecordingsLossless",
        title="Mendelssohn, Symphony No. 4 'Italian', IV. Saltarello (Presto)",
        licence="CC0-1.0",
        licence_url="https://creativecommons.org/publicdomain/zero/1.0/",
        attribution="Not required (CC0). Musopen Kickstarter recordings, "
                    "dedicated to the public domain by Musopen.",
        kind="music",
        start_s=30.0,
        duration_s=30.0,
    ),
]

# The synthetic fixtures are listed in the manifest too. They are not produced
# here (gen_gold_reference_wav.py and gen_stereo_reference_wav.py own them),
# but a corpus that only describes half of what tests/golden/audio holds is
# not a corpus - and check_corpus.py hashing them is what stops one being
# regenerated with a different RNG and quietly shifting every historical
# trend number out from under the series it belongs to.
SYNTHETIC = [
    dict(fixture="reference_51.wav", kind="synthetic",
         generator="tools/generators/gen_gold_reference_wav.py",
         note="sin()/pseudo-random noise/boxcar FIR; flat noise plateau above "
              "12 kHz - see this file's own module docstring."),
    dict(fixture="reference_stereo.wav", kind="synthetic",
         generator="tools/generators/gen_stereo_reference_wav.py",
         note="sin()/pseudo-random noise/boxcar FIR; flat noise plateau above "
              "12 kHz - see this file's own module docstring."),
]


# Not audio material and not produced by any generator: a real FFmpeg-encoded
# E-AC-3 bitstream, committed so tools/checks/verify_gold_reference.sh can
# check this project's decoder against a third-party stream that sets
# cplbndstrce == 0 (Annex E's default coupling band structure), which nothing
# this project's own encoder emits ever does. It lives in the same directory
# and its bytes matter for exactly the same reason the audio fixtures' do -
# regenerating it silently would move a published floor - so it is in the
# manifest too, hashed but with no audio parameters to check.
BITSTREAMS = [
    dict(fixture="reference_51_eac3_448k_cplbndstrce0.ec3", kind="bitstream",
         note="FFmpeg-encoded from reference_51.wav: `ffmpeg -y -i "
              "tests/golden/audio/reference_51.wav -c:a eac3 -b:a 448k <out>` "
              "(ffmpeg 8.0.1). Confirmed to set cplbndstrce == 0 with cplbegf == 12 "
              "in every block - see tools/checks/verify_gold_reference.sh for why "
              "cplbegf != 0 matters here."),
]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"command failed: {' '.join(map(str, cmd))}\n{result.stderr}")


def decode_excerpt(source: Path, start_s: float, duration_s: float, scratch: Path):
    """Source -> float32 WAV of exactly the requested window, via ffmpeg.

    -ss before -i is the fast seek, which on a lossless source lands on an
    exact sample rather than the nearest keyframe, and -t bounds the read so
    a 367 s movement does not get fully decoded to pull 30 s out of it.
    """
    run(["ffmpeg", "-v", "error", "-y", "-ss", f"{start_s}", "-t", f"{duration_s}",
         "-i", str(source), "-map", "0:a:0", "-c:a", "pcm_f32le", "-ar", str(RATE),
         "-ac", "2", str(scratch)])
    raw = scratch.read_bytes()
    i = raw.index(b"data")
    n = struct.unpack_from("<I", raw, i + 4)[0]
    samples = struct.unpack_from(f"<{n // 4}f", raw, i + 8)
    return list(samples)  # interleaved stereo


def write_flac(pcm16: bytes, path: Path, channels: int = 2):
    """PCM16 -> FLAC, through a WAV ffmpeg can read.

    -compression_level 12 is FLAC's slowest/smallest setting. This runs once,
    locally, to produce a committed file, so there is no reason to leave bytes
    on the table for encode speed nobody waits on.
    """
    tmp_wav = path.with_suffix(".tmp.wav")
    with wave.open(str(tmp_wav), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm16)
    run(["ffmpeg", "-v", "error", "-y", "-i", str(tmp_wav),
         "-c:a", "flac", "-compression_level", "12", str(path)])
    tmp_wav.unlink()


def build_fixture(entry, source_dir: Path, scratch_dir: Path):
    source = source_dir / entry["source_file"]
    if not source.exists():
        raise SystemExit(
            f"missing source {source} - fetch it once by hand from:\n  {entry['source_url']}\n"
            f"(licence {entry['licence']}, see {entry['source_page']}). It is deliberately not "
            "committed and deliberately not downloaded by this script.")
    actual = sha256_of(source)
    if actual != entry["source_sha256"]:
        raise SystemExit(
            f"{source}: SHA-256 {actual} does not match the recorded "
            f"{entry['source_sha256']} - the upstream file changed, or this is a different "
            "file. Refusing to build a fixture whose provenance cannot be shown.")

    interleaved = decode_excerpt(source, entry["start_s"], entry["duration_s"],
                                 scratch_dir / (entry["fixture"] + ".f32.wav"))
    peak = max((abs(v) for v in interleaved), default=0.0) or 1.0
    scale = NORMALISE_PEAK / peak
    pcm16 = bytearray()
    for v in interleaved:
        pcm16 += struct.pack("<h", max(-32768, min(32767, round(v * scale * 32767.0))))

    out_path = OUT / entry["fixture"]
    write_flac(bytes(pcm16), out_path)
    frames = len(interleaved) // 2
    return {
        "fixture": entry["fixture"],
        "kind": entry["kind"],
        "channels": 2,
        "sample_rate": RATE,
        "bits": 16,
        "duration_s": round(frames / RATE, 3),
        "sha256": sha256_of(out_path),
        "bytes": out_path.stat().st_size,
        "source": {
            "title": entry["title"],
            "url": entry["source_url"],
            "page": entry["source_page"],
            "sha256": entry["source_sha256"],
            "licence": entry["licence"],
            "licence_url": entry["licence_url"],
            "attribution": entry["attribution"],
        },
        "excerpt": {"start_s": entry["start_s"], "duration_s": entry["duration_s"],
                     "normalised_peak": NORMALISE_PEAK},
    }


def describe_synthetic(entry):
    path = OUT / entry["fixture"]
    if not path.exists():
        raise SystemExit(f"{path} missing - regenerate it with {entry['generator']}")
    with wave.open(str(path), "rb") as r:
        params = r.getparams()
        frames = r.getnframes()
    return {
        "fixture": entry["fixture"],
        "kind": entry["kind"],
        "channels": params.nchannels,
        "sample_rate": params.framerate,
        "bits": params.sampwidth * 8,
        "duration_s": round(frames / params.framerate, 3),
        "sha256": sha256_of(path),
        "bytes": path.stat().st_size,
        "generator": entry["generator"],
        "note": entry["note"],
    }


def describe_bitstream(entry):
    path = OUT / entry["fixture"]
    if not path.exists():
        raise SystemExit(f"{path} missing - see this entry's own note for how it was produced")
    return {
        "fixture": entry["fixture"],
        "kind": entry["kind"],
        "sha256": sha256_of(path),
        "bytes": path.stat().st_size,
        "note": entry["note"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", type=Path, required=True,
                         help="Directory holding the fetched, uncommitted source files.")
    parser.add_argument("--scratch-dir", type=Path, default=REPO / "build" / "corpus_scratch")
    parser.add_argument("--synthetic-only", action="store_true",
                         help="Re-hash the synthetic fixtures into the manifest without "
                              "rebuilding the programme ones (which need the sources).")
    args = parser.parse_args()

    args.scratch_dir.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)

    fixtures = [describe_synthetic(e) for e in SYNTHETIC]
    fixtures += [describe_bitstream(e) for e in BITSTREAMS]
    if not args.synthetic_only:
        for entry in SOURCES:
            fixtures.append(build_fixture(entry, args.source_dir, args.scratch_dir))
    else:
        existing = json.loads(MANIFEST.read_text())["fixtures"] if MANIFEST.exists() else []
        fixtures += [f for f in existing if f["kind"] in ("speech", "music")]
    fixtures.sort(key=lambda f: f["fixture"])

    manifest = {"corpus_version": CORPUS_VERSION, "fixtures": fixtures}
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"{'fixture':<42} | {'kind':<9} | {'ch':>2} | {'secs':>6} | {'KiB':>7}")
    print("-" * 80)
    for f in fixtures:
        ch = f"{f['channels']:>2}" if "channels" in f else " -"
        secs = f"{f['duration_s']:>6.2f}" if "duration_s" in f else "     -"
        print(f"{f['fixture']:<42} | {f['kind']:<9} | {ch} | {secs} | {f['bytes'] / 1024:>7.0f}")
    print(f"\nwrote {MANIFEST} (corpus_version {CORPUS_VERSION})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
