"""Generates the AC-4 external-baseline fixtures under
tests/golden/external-baseline/ac4-*/dee.ac4.

AC-4 has no decode oracle in this project at all (see docs/verification.md's
AC-4 section) - unlike gen_external_baseline.py's AC-3/E-AC-3 legs, there is
no SNR/MOS scoring machinery here, because there is nothing to score a
decode against. What this script produces is simpler: real, licensed Dolby
Encoding Engine 6.5.4 output for the `ac4::` parser and
tools/references/ac4_parse.py to be cross-checked against - the same
"somebody else's bitstream" role tests/golden/external-baseline/*/dee.ec3
plays for E-AC-3 (CONTRIBUTING.md's Oracles list, #3).

Two legs, chosen to exercise both bitstream layouts this project's parser
distinguishes:
  - ac4-stereo-64: plain stereo, dee_ac4_encoder.exe --input-format wav.
  - ac4-5114: 5.1.4 channel-based immersive, dee_ac4_encoder.exe
    --input-format cbi_wav (SMPTE channel order) --output-channel-layout
    5.1.4 - exercises Table 56's extended (8/9-bit) channel_mode codes,
    which the plain-stereo leg cannot reach.

Never run in CI - see guard_not_ci(), the same rule
gen_external_baseline.py's own copy of this function states.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO = REPO / "tests" / "golden" / "audio"
OUT = REPO / "tests" / "golden" / "external-baseline"
SCRATCH = REPO / "build" / "ac4_baseline_scratch"

DEE = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir\dee_ac4_encoder.exe")

# Bump by hand whenever this script is rerun to regenerate the baseline
# against a new DEE release - the same discipline
# gen_external_baseline.py's BASELINE_VERSION follows.
BASELINE_VERSION = 1

LEGS = [
    {"name": "ac4-stereo-64", "input_format": "wav", "wav": AUDIO / "reference_stereo.wav",
     "output_channel_layout": "stereo", "kbps": 64},
]

# The 5.1.4 leg needs a 10-channel (L R C LFE Ls Rs Ltm Rtm Lbm Rbm) source
# WAV in SMPTE order, which nothing under tests/golden/audio/ provides (the
# committed fixtures top out at reference_51.wav's 6 channels). Rather than
# commit a synthetic 10-channel WAV solely for this one generator to consume
# once, this leg is opt-in: pass --with-5114 <path-to-10ch-wav> to include it.


def guard_not_ci():
    if os.environ.get("GITHUB_ACTIONS"):
        raise SystemExit(
            "gen_ac4_baseline.py invokes licensed, non-CI-safe tooling "
            "(Dolby DEE) and must never run in a CI job - refusing because "
            "GITHUB_ACTIONS is set.")


def dee_version():
    # No --version option; -h's help text carries a "belongs to the Dolby
    # Encoding Engine version X" line instead. check=False: some builds exit
    # non-zero even after printing the banner this parses.
    result = subprocess.run([str(DEE), "-h"], capture_output=True, text=True, check=False)
    for line in result.stdout.splitlines():
        if "Dolby Encoding Engine version" in line:
            return line.strip()
    return "unknown"


def encode(leg, scratch_dir):
    scratch_dir.mkdir(parents=True, exist_ok=True)
    out = scratch_dir / f"{leg['name']}.ac4"
    manifest = scratch_dir / f"{leg['name']}.dee_manifest.json"
    cmd = [str(DEE), "--input-format", leg["input_format"], "-i", str(leg["wav"]),
           "-o", str(out), "--output-channel-layout", leg["output_channel_layout"],
           "--data-rate", str(leg["kbps"]), "--output-manifest", str(manifest), "--overwrite", "1"]
    # DEE writes its own log relative to the current directory - run from the
    # scratch dir, the same workaround gen_object_fixture.py uses, since the
    # dee-dir installation directory itself is not writable.
    subprocess.run(cmd, cwd=scratch_dir, check=True, capture_output=True, text=True)
    return out


def main():
    guard_not_ci()
    if not DEE.exists():
        raise SystemExit(f"DEE AC-4 encoder not found at {DEE} - this generator only runs "
                          "on a machine with Dolby Media Encoder installed.")
    SCRATCH.mkdir(parents=True, exist_ok=True)

    legs = list(LEGS)
    if "--with-5114" in sys.argv:
        wav = Path(sys.argv[sys.argv.index("--with-5114") + 1])
        legs.append({"name": "ac4-5114", "input_format": "cbi_wav", "wav": wav,
                     "output_channel_layout": "5.1.4", "kbps": 256})

    manifest = {"baseline_version": BASELINE_VERSION, "dee_version": dee_version(), "legs": {}}
    for leg in legs:
        out_dir = OUT / leg["name"]
        out_dir.mkdir(parents=True, exist_ok=True)
        encoded = encode(leg, SCRATCH)
        dest = out_dir / "dee.ac4"
        dest.write_bytes(encoded.read_bytes())
        manifest["legs"][leg["name"]] = {
            "output_channel_layout": leg["output_channel_layout"],
            "bitrate_kbps": leg["kbps"],
            "source_wav": str(leg["wav"].relative_to(REPO)) if leg["wav"].is_relative_to(REPO)
            else str(leg["wav"]),
            "size_bytes": dest.stat().st_size,
        }
        print(f"wrote {dest} ({dest.stat().st_size} bytes)")

    manifest_path = OUT / "ac4-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {manifest_path}")


if __name__ == "__main__":
    main()
