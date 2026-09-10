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
side is missing from the other.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"^([a-z0-9_]+)\.pcm_hash=([0-9a-f]{16})\s*$")


def hashes_of_run(path: Path) -> dict[str, str]:
    found: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.match(line.strip())
        if match:
            found[match.group(1)] = match.group(2)
    return found


def hashes_of_pins(path: Path) -> dict[str, str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    return {str(k): str(v) for k, v in document["hashes"].items()}


def compare(a: dict[str, str], a_name: str, b: dict[str, str], b_name: str) -> bool:
    failed = False
    for codec in sorted(set(a) | set(b)):
        if codec not in a or codec not in b:
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
        expected, actual = hashes_of_pins(pins), hashes_of_run(run)
        if not actual:
            print(f"::error::no pcm_hash lines in {run}", file=sys.stderr)
            return 1
        return 0 if compare(expected, pins.name, actual, run.name) else 1
    if len(args) == 2:
        a_path, b_path = Path(args[0]), Path(args[1])
        a, b = hashes_of_run(a_path), hashes_of_run(b_path)
        if not a or not b:
            print(f"::error::no pcm_hash lines in {a_path if not a else b_path}", file=sys.stderr)
            return 1
        return 0 if compare(a, a_path.name, b, b_path.name) else 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
