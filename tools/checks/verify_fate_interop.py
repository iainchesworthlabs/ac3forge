"""Decode pinned FFmpeg FATE AC-3/E-AC-3 samples and hold them against FFmpeg.

Roadmap VX4's second step. The first step decodes
tests/golden/external-baseline/ - six streams from Dolby's Encoding Engine and
FFmpeg's own encoder, produced from this repository's own source material -
with the in-repo decoder on every gold-reference leg. That is real third-party
structure, and it found five real Annex E decoder defects the moment it ran.
But both of those encoders were driven by this project, at rates and layouts
this project chose, on 2.5 s of synthetic fixture audio. This script widens the
corpus to streams nobody here commissioned: excerpts of commercially mastered
programme material, encoded years ago by whatever encoder the mastering house
used, exercising choices neither this project's encoder nor FFmpeg's makes -
spectral extension, 1536 kbit/s, a commentary track, dither on, per-block
exponent strategies, and the 3/1 acmod nothing in this tree can encode.

Why FATE and not a conformance suite: there isn't one to be had. ATSC A/52 and
ETSI TS 102 366 are both freely downloadable documents, but neither ATSC nor
ETSI publishes conformance BITSTREAMS for AC-3 or E-AC-3 the way MPEG does for
its own codecs, and Dolby's own verification material ships under licence with
its professional tools. Searching for a redistributable, citable vector set
came back empty (the same finding docs/verification.md records). FFmpeg's FATE
sample archive is the closest freely-fetchable substitute: the files are real
commercial encoder output, they are stable (most have not changed since 2010-
2011), and they are already the reference corpus for another decoder's own
regression suite.

Fetched, never committed. Each sample is pinned by SHA-256, so a silent change
upstream fails the run rather than quietly moving the numbers; the download
lands in a cache directory outside the repository. The archive is plain HTTP(S)
served content and the samples are excerpts of copyrighted films, which is the
other half of why they are not vendored here.

Usage:
    AC3CLI=build/config-linux-llvm/bin/ac3cli python3 tools/checks/verify_fate_interop.py

    --cache-dir DIR   where to keep downloads (default: $FATE_CACHE_DIR, else a
                      temporary directory removed on exit)
    --cli PATH        ac3cli, overriding $AC3CLI
    --list            print the pinned corpus and exit, downloading nothing

Exits non-zero on the first sample that fails.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
COMPARE = REPO / "tools" / "checks" / "compare_wav.py"
BASE_URL = "https://fate-suite.ffmpeg.org/"

# Every sample below was measured by hand against FFmpeg 8.0.1 on x86-64
# Windows and Linux while this file was written; the measured worst channel is
# quoted beside each floor, which is set about 9 dB under it - the same
# measured-minus-headroom basis tools/checks/verify_gold_reference.sh's own
# third-party floors use, and for the same reason: two independently correct
# decoders diverge on the parts of the reconstruction the standard leaves to
# the implementation (dither in bap == 0 bins above all, whose pseudo-random
# sequence is per-decoder), so the floor has to sit below that divergence
# while still failing hard on a defect.
#
# Three fields decide what each sample is actually gated on:
#
#   min_snr_db      the ratio form. Only meaningful where the excerpt has
#                   programme material in it.
#   max_diff_dbfs   the absolute form. Several of these excerpts are near-
#                   silent passages (-93 to -100 dBFS), where an SNR of 2 dB
#                   and an SNR of 40 dB are both consistent with a completely
#                   inaudible disagreement and no ratio floor can tell a
#                   defect apart from the noise floor. Those are gated on the
#                   difference signal's own level instead.
#   compare         False for a sample where the two decoders' output cannot
#                   be diffed at all. Then the assertions are "the in-repo
#                   decoder reads it" and "FFmpeg strict-decodes it", which is
#                   exactly the pair that caught the external-baseline defects.
#
# complete_bytes trims a trailing partial syncframe. Two of the AC-3 samples
# are byte-truncated excerpts (exactly 100,000 bytes), so their last frame is
# cut mid-way: FFmpeg reports "incomplete frame" and the in-repo decoder
# refuses the stream outright ("ends part-way through a frame"). Neither is a
# defect and neither is interesting, so the harness trims to the last complete
# frame first. The byte counts are pinned rather than computed, on the same
# basis as the hashes - the files they describe cannot change without the
# SHA-256 check firing first.
SAMPLES = [
    {
        "path": "ac3/monsters_inc_2.0_192_small.ac3",
        "sha256": "4df071457c23d1a39fe52322892bc9333921ad6dfc8f87ffa0797bdfb5101bae",
        "note": "AC-3 2/0 @ 192 kbit/s, commercial film mix, dither in use",
        "complete_bytes": 99840,  # 130 frames x 768 bytes; the file is 100000
        "min_snr_db": 22.0,       # measured 31.98 dB worst channel
    },
    {
        "path": "ac3/monsters_inc_5.1_448_small.ac3",
        "sha256": "74fefeef070f663378a0864beb064cf88ee1186152f7ac320984af15be2f22db",
        "note": "AC-3 3/2+LFE @ 448 kbit/s; this excerpt is a near-silent passage "
                "(-93 dBFS), so it is gated on absolute difference, not SNR",
        "complete_bytes": 98560,  # 55 frames x 1792 bytes; the file is 100000
        "max_diff_dbfs": -90.0,   # measured -101.72 dBFS loudest channel difference
    },
    {
        "path": "ac3/millers_crossing_4.0.ac3",
        "sha256": "faa06c7b1ccbc5cf3c7897afb50004dd3d49feaa19d26bdeb03dc9bb4ec20802",
        "note": "AC-3 3/1 (L R C S) - an acmod nothing in this tree can encode; "
                "a near-silent surround, so gated on absolute difference",
        # This used to be decode-and-parse only: ac3::io::wav_channel_order
        # wrote 2/1 and 3/1 in bitstream order (L C R S) on the grounds that no
        # WAV convention claims a mono-surround slot, while FFmpeg maps 3/1
        # onto WAVEFORMATEXTENSIBLE's FL/FR/FC/BC (SPEAKER_BACK_CENTER, 0x100)
        # and so wrote L R C S - a real speaker slot the comment's premise had
        # missed. wav_channel_order now places every acmod by WAV speaker
        # position, this file's decoders agree on order, and the files diff
        # cleanly: channel 0 (L) at 48.93 dB, channel 3 (S, -95 dBFS) at
        # -55.31 dBFS loudest difference. See docs/verification.md's
        # "Third-party bitstreams" section for the full before/after.
        "max_diff_dbfs": -46.0,  # measured -55.31 dBFS loudest channel difference
    },
    {
        "path": "eac3/csi_miami_5.1_256_spx_small.eac3",
        "sha256": "84f50317a49a98509bf174ace814b39860e6ab2a1d61bdbd10ada9081f2bb2b3",
        "note": "E-AC-3 5.1 @ 256 kbit/s with spectral extension",
        # 12, well under the others: spectral extension REGENERATES the high
        # band from a copied-down region plus a noise blend, so two
        # spec-correct decoders legitimately diverge much further there than
        # on a plainly-coded band - the same effect verify_gold_reference.sh's
        # own header records as ~31 dB for this project's spx encodes.
        "min_snr_db": 12.0,       # measured 19.00 dB worst channel (the LFE)
    },
    {
        "path": "eac3/csi_miami_stereo_128_spx_small.eac3",
        "sha256": "adc397f12472353237f7dc8139c2223937abc23eff9cd80ac2e3471814a04c3a",
        "note": "E-AC-3 2/0 @ 128 kbit/s with spectral extension",
        "min_snr_db": 25.0,       # measured 34.31 dB worst channel
    },
    {
        "path": "eac3/matrix2_commentary1_stereo_192_small.eac3",
        "sha256": "ea095d557bd6787266f4b7276a2be818a7fbc924997748be89d9ead962a3391c",
        "note": "E-AC-3 2/0 @ 192 kbit/s, a director's-commentary track - speech "
                "over a wide dynamic range, unlike every fixture in tests/golden",
        "min_snr_db": 32.0,       # measured 41.52 dB worst channel
    },
    {
        "path": "eac3/serenity_english_5.1_1536_small.eac3",
        "sha256": "3d81c22610a2602b7799ec5e59c2677e8ab23e03e4abc03c2a16b683aa3d5f0a",
        "note": "E-AC-3 5.1 @ 1536 kbit/s - the top of the rate range, far above "
                "anything else exercised here; also a near-silent passage, so "
                "gated on absolute difference",
        "max_diff_dbfs": -90.0,   # measured -102.33 dBFS loudest channel difference
    },
]

# Considered and left out, so the reasoning is not lost and nobody re-adds it
# expecting it to work:
#
#   eac3/the_great_wall_7.1.eac3
#     Not an E-AC-3 elementary stream. Each 4608-byte access unit is an AC-3
#     core frame (bsid 6, 3/2+LFE, no E-AC-3 header at all) followed by a
#     2304-byte E-AC-3 DEPENDENT substream - a legacy-core-plus-extension
#     delivery. `ac3cli decode` dispatches on the first frame's bsid, reads
#     the AC-3 core path, and then refuses the bsid-16 dependent frame that
#     follows it. Splitting the halves confirms it: the core alone decodes
#     cleanly here as 5.1 AC-3, and ffprobe reads it as ac3. Supporting the
#     arrangement is a feature, not a fix, so it is recorded rather than
#     gated.
#
#   ac3/diatonis_invisible_order_anfos_ac3-small.wav, ac3/mp3ac325-4864-small.ts
#     AC-3 inside a WAV and inside an MPEG-TS. `ac3cli decode` takes
#     elementary streams; demuxing is not what this harness is testing.


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(sample: dict, cache_dir: Path) -> Path:
    """Download (or reuse) one sample and check it against its pinned hash.

    A cached file whose hash no longer matches is re-fetched once rather than
    trusted or refused outright: the usual cause is a partial download from an
    earlier interrupted run, not an upstream change. A fresh download that
    still fails the check is a hard error - that IS the pin doing its job."""
    name = sample["path"].split("/")[-1]
    local = cache_dir / name
    for attempt in (1, 2):
        if not local.exists():
            url = BASE_URL + sample["path"]
            print(f"    fetching {url}")
            with urllib.request.urlopen(url, timeout=120) as response:  # noqa: S310
                local.write_bytes(response.read())
        actual = sha256_of(local)
        if actual == sample["sha256"]:
            return local
        print(f"    ::warning::{name}: sha256 {actual} != pinned {sample['sha256']}")
        local.unlink()
        if attempt == 2:
            raise SystemExit(
                f"::error::{name}: SHA-256 mismatch after a fresh download. Either the "
                f"FATE archive changed this file (re-pin it deliberately, having "
                f"looked at what changed) or the download is being tampered with."
            )
    raise AssertionError("unreachable")


def ffmpeg_strict_decode(source: Path, out_wav: Path) -> None:
    """FFmpeg's own strict read, the same flags every other gate in this repo
    uses (tools/ci/run_codec_matrix.sh's run_ffmpeg_check, and
    tools/checks/verify_gold_reference.sh's ffmpeg_strict_decode): without
    -err_detect FFmpeg conceals a bad frame and still exits 0. Pass is exit 0
    with empty stderr."""
    result = subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-err_detect", "crccheck+bitstream+buffer+explode",
         "-drc_scale", "0", "-i", str(source), "-f", "wav", str(out_wav)],
        capture_output=True, text=True, check=False)
    if result.returncode != 0 or result.stderr.strip():
        raise SystemExit(f"::error::ffmpeg strict decode of {source.name} failed:\n"
                         f"{result.stderr.strip()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache-dir", type=Path, default=None)
    parser.add_argument("--cli", default=os.environ.get("AC3CLI", ""))
    parser.add_argument("--list", action="store_true",
                         help="Print the pinned corpus and exit, downloading nothing.")
    args = parser.parse_args()

    if args.list:
        for sample in SAMPLES:
            print(f"{sample['path']}\n    {sample['note']}")
        return 0

    if not args.cli or not Path(args.cli).exists():
        raise SystemExit("::error::set AC3CLI (or --cli) to a built ac3cli")
    # Absolute, like tools/ci/run_codec_matrix.sh resolves its own argument and
    # for the same reason: callers pass a path relative to the repository root,
    # and every decode below runs with a scratch directory in play.
    cli = str(Path(args.cli).resolve())
    if shutil.which("ffmpeg") is None:
        raise SystemExit("::error::ffmpeg not on PATH")

    cache_dir = args.cache_dir or (Path(os.environ["FATE_CACHE_DIR"])
                                   if os.environ.get("FATE_CACHE_DIR") else None)
    scratch = None
    if cache_dir is None:
        scratch = tempfile.mkdtemp(prefix="ac3forge-fate-")
        cache_dir = Path(scratch)
    cache_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="ac3forge-fate-work-"))

    failures = []
    try:
        for index, sample in enumerate(SAMPLES, start=1):
            name = sample["path"].split("/")[-1]
            print(f"[{index}/{len(SAMPLES)}] {name}: {sample['note']}")
            source = fetch(sample, cache_dir)

            if "complete_bytes" in sample:
                trimmed = work / name
                trimmed.write_bytes(source.read_bytes()[:sample["complete_bytes"]])
                source = trimmed
                print(f"    trimmed to {sample['complete_bytes']} bytes "
                      f"(last complete syncframe)")

            ffmpeg_wav = work / f"{name}.ffmpeg.wav"
            ffmpeg_strict_decode(source, ffmpeg_wav)
            print("    ffmpeg strict decode: ok")

            our_wav = work / f"{name}.ours.wav"
            decode = subprocess.run([cli, "decode", str(source), str(our_wav)],
                                     capture_output=True, text=True, check=False)
            if decode.returncode != 0:
                failures.append(f"{name}: ac3cli decode failed: {decode.stderr.strip()}")
                print(f"    ::error::ac3cli decode failed: {decode.stderr.strip()}")
                continue
            print("    ac3cli decode: ok")

            if not sample.get("compare", True):
                print("    diff: skipped for this sample - see its own note in SAMPLES")
                continue

            # --max-lag-samples 0, unlike the gold-reference gate's default 512:
            # both sides are decoding the SAME bitstream, so there is no
            # priming difference to search for, and on a quiet opening passage
            # the unnormalised cross-correlation happily picks a spurious lag
            # and reports a real match as a total mismatch (confirmed by hand
            # on monsters_inc_2.0: lag -512 and -2.85 dB, versus lag 0 and
            # 31.98 dB on the identical pair of files).
            cmd = [sys.executable, str(COMPARE), str(ffmpeg_wav), str(our_wav),
                   "--max-lag-samples", "0",
                   "--min-snr-db", str(sample.get("min_snr_db", -200.0))]
            if "max_diff_dbfs" in sample:
                cmd += ["--max-diff-dbfs", str(sample["max_diff_dbfs"])]
            compare = subprocess.run(cmd, capture_output=True, text=True, check=False)
            for line in compare.stdout.rstrip().splitlines():
                print(f"    {line}")
            if compare.returncode != 0:
                failures.append(f"{name}: decoded audio disagrees with FFmpeg's")
    finally:
        shutil.rmtree(work, ignore_errors=True)
        if scratch is not None:
            shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        for failure in failures:
            print(f"::error::{failure}")
        return 1
    print(f"FATE interop: {len(SAMPLES)} pinned third-party samples decoded and checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
