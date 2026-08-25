"""Local-only generator for the committed third-party Dolby Atmos fixture.

`tests/golden/object-fixture/dee_joc_514.ec3` is a DD+ JOC bitstream produced
by the Dolby Encoding Engine (bundled in "Dolby Media Encoder") from the
synthetic 5.1.4 tone bed this script also writes. It is the ONLY Atmos stream
in this repository that this project's own encoder did not make, and so the
only check that the object layer reads syntax nobody here writes:

  - a bed program (b_dyn_object_only_program 0) with a twelve-channel
    7.1.4 bed_channel_assignment and no dynamic objects at all;
  - b_bed_chan_distribute set;
  - object_gain_idx 3, "the previous object's gain", on eleven of the twelve;
  - two oa_elements, the second a trim_element with a custom global trim mode
    and a per-object disable list;
  - joc_dmx_config_idx 3 (5.X with a 90 degree phase shift), a nonzero
    joc_clipgain, joc_num_bands_idx 5, and sparse coding for every object;
  - and, in the EMDF container, an OAMD payload with payload_frame_aligned 0
    beside a JOC payload with it set, plus two further payloads whose
    configurations use duratione and discard_unknown_payload.

Every one of those was refused outright before roadmap DC6.

Why channel-based immersive and not an ADM master: DEE's `atmos_mezz` input
accepts BWF ADM, but its reader gates on content provenance ("Content was not
authored with Dolby tools") and refuses a master this project authors, so the
`cbi_wav` path is the only one available here. That is why the fixture has no
dynamic objects and therefore no object size, zone or snap on the wire - the
encode-side round trip in tests/oba/test_oba.cpp covers those instead.

DEE is licensed commercial software and must never run in CI, exactly as
gen_external_baseline.py says of the same binary - hence the same
GITHUB_ACTIONS guard. Run this locally, listen to or measure the result, and
commit the bitstream.

Usage (repo root):  python tools/generators/gen_object_fixture.py
"""

import os
import subprocess
import wave
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
OUT_DIR = REPO / "tests" / "golden" / "object-fixture"
DEE = Path(r"C:\Program Files\Dolby\Dolby Media Encoder\resources\dee-dir"
           r"\dee_ddpjoc_encoder.exe")

SAMPLE_RATE = 48000
SECONDS = 2.0
DATA_RATE_KBPS = 448

# DEE's cbi_wav input reads a 5.1.4 file in Dolby's own channel order, NOT the
# L/C/R/Ls/Rs/LFE "SMPTE order" its 5.1 dee_ddp_encoder path documents. That is
# not stated in `--morehelp input-format`; it was measured, by encoding a file
# with one distinct tone per channel and identifying each reconstructed JOC
# object by which tone dominates it (tools/checks/check_object_fixture.py runs
# the same identification as a regression check).
CHANNELS = [
    ("L", 220.0),
    ("R", 277.2),
    ("C", 330.0),
    ("LFE", 55.0),
    ("Ls", 554.4),
    ("Rs", 660.0),
    ("Ltf", 740.0),
    ("Rtf", 831.6),
    ("Ltr", 880.0),
    ("Rtr", 1108.8),
]


def guard_not_ci():
    if os.environ.get("GITHUB_ACTIONS"):
        raise SystemExit(
            "gen_object_fixture.py invokes licensed, non-CI-safe tooling "
            "(Dolby DEE) and must never run in a CI job - refusing because "
            "GITHUB_ACTIONS is set.")


def write_tone_bed(path):
    """One distinct tone per channel, each slowly breathing at its own rate.

    The per-channel amplitude sweep matters: a stationary bed gives the JOC
    solver nothing to track, and identical envelopes make neighbouring
    channels degenerate in the downmix, which is precisely the case a
    parametric object coder cannot separate.
    """
    n = int(SAMPLE_RATE * SECONDS)
    t = np.arange(n) / SAMPLE_RATE
    columns = []
    for index, (_, frequency) in enumerate(CHANNELS):
        envelope = 0.35 * (0.55 + 0.45 * np.sin(2 * np.pi * (0.25 + 0.1 * index) * t))
        columns.append(envelope * np.sin(2 * np.pi * frequency * t))
    pcm = np.clip(np.stack(columns, axis=1), -1.0, 1.0)
    samples = (pcm * 32767.0).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(len(CHANNELS))
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.tobytes())


def invoke_dee(wav, out):
    """loudness-management measure_only + drc_profile=none, matching
    gen_external_baseline.py: DEE's correcting mode would apply a gain the
    decoder cannot know about, which turns a level comparison into a
    measurement of DEE's loudness target rather than of the object layer."""
    subprocess.run(
        [str(DEE),
         "--input-format", "cbi_wav",
         "--input", str(wav),
         "--encoder", "drc_profile=none",
         "--loudness-management", "measure_only",
         "--data-rate", str(DATA_RATE_KBPS),
         "--overwrite", "1",
         "--output", str(out)],
        check=True)


def main():
    guard_not_ci()
    if not DEE.exists():
        raise SystemExit(f"Dolby Encoding Engine not found at {DEE}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    wav = OUT_DIR / "tone_bed_514.wav"
    stream = OUT_DIR / "dee_joc_514.ec3"
    write_tone_bed(wav)
    invoke_dee(wav, stream)
    # The WAV is scratch: it is 1.8 MB, it is fully reproducible from the
    # table above, and nothing in the repository reads it.
    wav.unlink()
    print(f"wrote {stream} ({stream.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
