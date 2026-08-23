"""Do the committed fixtures still match tests/golden/audio/corpus.json?

The fixture corpus is the one input every published quality number in this
project is measured against - docs/landscape.md, docs/quality-trend.md and
docs/tool-comparison-trend.md all plot series whose points are only
comparable to each other because the material under them never moved. It is
also the input least likely to be noticed changing: a fixture is a binary, a
regenerated one still decodes, still has the right duration and channel
count, and still produces a perfectly plausible SNR. The series it belongs to
would simply have a step in it that looks like an encoder change.

So this hashes every fixture the manifest names and fails if any of them
differs, plus the cheap structural facts (channels, sample rate, bit depth,
duration) so a mismatch says what actually changed rather than just "hash
differs". Regenerating a fixture on purpose means rerunning its generator AND
bumping corpus_version - see tools/generators/gen_programme_fixtures.py's
CORPUS_VERSION - which is a visible line in a PR diff instead of a silent
binary swap.

stdlib-only, no build required, seconds to run: same no-new-CI-provisioning
reasoning as every tools/ci/append_*.py script.

Usage (repo root):  python tools/checks/check_corpus.py
"""

import hashlib
import json
import sys
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO = REPO / "tests" / "golden" / "audio"
MANIFEST = AUDIO / "corpus.json"


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def flac_streaminfo(path: Path):
    """(channels, sample_rate, bits, frames) from a FLAC STREAMINFO block.

    Hand-parsed rather than shelling out to ffprobe: this check has to stay
    stdlib-only so it can run in any job without provisioning, and
    STREAMINFO is fixed-layout and always the first metadata block (RFC 9639
    section 8.2). It is 34 bytes; the four fields wanted here are the 64 bits
    at offset 10, packed across byte boundaries as sample rate (20 bits),
    channels - 1 (3), bits per sample - 1 (5) and total interchannel samples
    (36). Everything before them is the block-size and frame-size pairs
    (2 + 2 + 3 + 3 bytes); everything after is the MD5.
    """
    with path.open("rb") as f:
        if f.read(4) != b"fLaC":
            raise ValueError(f"{path}: not a FLAC file")
        header = f.read(4)
        block_type = header[0] & 0x7F
        if block_type != 0:
            raise ValueError(f"{path}: first metadata block is type {block_type}, not STREAMINFO")
        info = f.read(int.from_bytes(header[1:4], "big"))
    packed = int.from_bytes(info[10:18], "big")  # 64 bits covering the 8 packed fields
    sample_rate = (packed >> 44) & 0xFFFFF
    channels = ((packed >> 41) & 0x7) + 1
    bits = ((packed >> 36) & 0x1F) + 1
    frames = packed & 0xFFFFFFFFF
    return channels, sample_rate, bits, frames


def wav_facts(path: Path):
    with wave.open(str(path), "rb") as r:
        return r.getnchannels(), r.getframerate(), r.getsampwidth() * 8, r.getnframes()


def main() -> int:
    if not MANIFEST.exists():
        print(f"::error::{MANIFEST} missing - the fixture corpus has no manifest")
        return 1
    manifest = json.loads(MANIFEST.read_text())
    failures = []

    print(f"corpus_version {manifest['corpus_version']}")
    for entry in manifest["fixtures"]:
        path = AUDIO / entry["fixture"]
        if not path.exists():
            failures.append(f"{entry['fixture']}: named in the manifest but not in the tree")
            continue
        actual = sha256_of(path)
        if actual != entry["sha256"]:
            failures.append(
                f"{entry['fixture']}: SHA-256 {actual} != manifest {entry['sha256']}. If this "
                "fixture was regenerated on purpose, rerun its generator and bump "
                "CORPUS_VERSION in tools/generators/gen_programme_fixtures.py; every published "
                "trend series is measured against these bytes.")
            continue
        try:
            facts = flac_streaminfo(path) if path.suffix == ".flac" else wav_facts(path)
        except Exception as exc:  # noqa: BLE001 - report, don't abort the whole sweep
            failures.append(f"{entry['fixture']}: could not read audio parameters ({exc})")
            continue
        channels, rate, bits, frames = facts
        for label, got, want in (("channels", channels, entry["channels"]),
                                 ("sample_rate", rate, entry["sample_rate"]),
                                 ("bits", bits, entry["bits"])):
            if got != want:
                failures.append(f"{entry['fixture']}: {label} {got} != manifest {want}")
        duration = round(frames / rate, 3)
        if abs(duration - entry["duration_s"]) > 0.01:
            failures.append(
                f"{entry['fixture']}: duration {duration:.3f}s != manifest {entry['duration_s']}s")
        print(f"  ok  {entry['fixture']:<32} {entry['kind']:<9} "
              f"{channels}ch {rate} Hz {bits}-bit {duration:.2f}s")

    if failures:
        for f in failures:
            print(f"::error title=Fixture corpus mismatch::{f}")
        return 1
    print(f"all {len(manifest['fixtures'])} fixtures match the corpus manifest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
