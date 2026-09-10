"""Compare a bare-metal probe run's PCM hashes with another run's, or with the
pinned ones (planning/arithmetic-tiers.md).

The probe prints one `<codec>.pcm_hash=<16 hex digits>` line per fixture it
decodes: FNV-1a over every delivered sample's bit pattern, in delivery order.
For the fixed-point tier the decode is integer arithmetic, so the value is
the same on every leg - the x86 host, the Cortex-M3 under QEMU, a RISC-V
part - and that identity is the tier's own gate: a hash that differs between
legs is arithmetic that is not integer somewhere, or a platform difference the
tier was built to have none of. The pinned values in
tests/golden/fixed-probe-pcm-hashes.json are what both CI legs are held to;
re-pin them when the tier's arithmetic changes on purpose. For the floating
tiers the hashes vary with the compiler and this check has nothing to say.

Usage:
    check_probe_hashes.py <run_a.txt> <run_b.txt>
    check_probe_hashes.py --expected <pins.json> <run.txt>

Exits non-zero when a fixture's hash differs, or when a fixture hashed on one
side is missing from the other without the run having said why. A run that
prints `<codec>.skipped=<reason>` - a target whose heap cannot hold that
fixture - is reported as skipped rather than failed.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"^([a-z0-9_]+)\.pcm_hash=([0-9a-f]{16})\s*$")
# A fixture the probe declined to decode, and why - today only the per-target
# heap budget (apps/baremetal/probe.cpp's over_budget). A run that says so is
# not a run that is missing a fixture: an ESP32-C3 has 400 KB of internal SRAM
# and the 7.1.4 programme peaks at 238,094 bytes, so that leg decodes eleven
# of the twelve and states which one it did not. Absence WITHOUT one of these
# lines is still a failure - the point is that the skip has to be declared.
SKIP = re.compile(r"^([a-z0-9_]+)\.skipped=(\S+)")


def hashes_of_run(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    """The run's hashes, and the fixtures it declared skipped with the reason."""
    found: dict[str, str] = {}
    skipped: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        match = LINE.match(stripped)
        if match:
            found[match.group(1)] = match.group(2)
            continue
        skip = SKIP.match(stripped)
        if skip:
            skipped[skip.group(1)] = skip.group(2)
    return found, skipped


def hashes_of_pins(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    return {str(k): str(v) for k, v in document["hashes"].items()}


def compare(a: dict[str, str], a_name: str, b: dict[str, str], b_name: str,
            skipped: dict[str, str] | None = None) -> bool:
    failed = False
    declared = skipped or {}
    for codec in sorted(set(a) | set(b) | set(declared)):
        if codec in declared and codec not in b:
            print(f"[skipped]  {codec}: {b_name} declined it ({declared[codec]})")
        elif codec not in a or codec not in b:
            print(f"::error::{codec}: hashed in {a_name if codec in a else b_name} only",
                  file=sys.stderr)
            failed = True
        elif a[codec] != b[codec]:
            print(f"::error::[FAIL] {codec}: {a[codec]} ({a_name}) != {b[codec]} ({b_name})",
                  file=sys.stderr)
            failed = True
        else:
            print(f"[ok]       {codec}: {a[codec]}")
    return not failed


def main() -> int:
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--expected":
        pins, run = Path(args[1]), Path(args[2])
        expected = hashes_of_pins(pins)
        actual, skipped = hashes_of_run(run)
        if not actual:
            print(f"::error::no pcm_hash lines in {run}", file=sys.stderr)
            return 1
        return 0 if compare(expected, pins.name, actual, run.name, skipped) else 1
    if len(args) == 2:
        a_path, b_path = Path(args[0]), Path(args[1])
        a, a_skipped = hashes_of_run(a_path)
        b, b_skipped = hashes_of_run(b_path)
        if not a or not b:
            print(f"::error::no pcm_hash lines in {a_path if not a else b_path}", file=sys.stderr)
            return 1
        # A fixture either run declared skipped is excused on that side.
        ok = compare(a, a_path.name, b, b_path.name, b_skipped)
        for codec, reason in a_skipped.items():
            if codec not in a and codec in b:
                print(f"[skipped]  {codec}: {a_path.name} declined it ({reason})")
        return 0 if ok else 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
