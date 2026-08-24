#!/usr/bin/env python3
"""Turn one bare-metal probe run into the footprint table (roadmap PF7).

    python3 tools/checks/footprint_report.py \\
        --probe /tmp/footprint.txt \\
        --elf build/config-arm-none-eabi-minimal/bin/ac3probe \\
        --map build/config-arm-none-eabi-minimal/apps/baremetal/ac3probe.map \\
        --markdown

Three inputs, because no one of them answers the whole question:

  --probe  The key=value lines apps/baremetal/probe.cpp printed while running:
           peak heap, allocations per frame, the per-instance decoder sizes.
           Runtime cost, observed rather than derived.
  --elf    arm-none-eabi-size over the linked image: text, data, bss. Static
           cost, from the linker rather than from a source-level guess.
  --map    The linker map, read to attribute .text and .bss to the objects that
           contributed them. This is the part that says WHERE a regression came
           from, which is the difference between a number moving and a number
           being explained.

Output goes to stdout: plain text by default, GitHub-flavoured Markdown with
--markdown (which is what the CI leg appends to its step summary). This script
never fails a build - the gate is tools/checks/run_baremetal_probe.sh's own
ceilings, and a reporting script that can fail a run is a reporting script that
gets removed from the run.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

# Objects big enough to be worth a line of their own. Everything smaller is
# summed into "other" rather than listed - a table with sixty rows communicates
# less than one with eight.
SIGNIFICANT_BYTES = 2048


def read_probe(path: pathlib.Path) -> dict[str, str]:
    """Every key=value token the probe printed, flattened into one mapping."""
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        for token in line.split():
            if "=" in token:
                key, _, value = token.partition("=")
                values[key] = value
    return values


def read_size(elf: pathlib.Path) -> dict[str, int]:
    """text/data/bss from arm-none-eabi-size, or the host size(1)."""
    for tool in ("arm-none-eabi-size", "size"):
        try:
            out = subprocess.run([tool, str(elf)], capture_output=True, text=True, check=True)
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
        rows = out.stdout.strip().splitlines()
        if len(rows) < 2:
            continue
        fields = rows[1].split()
        return {
            "text": int(fields[0]),
            "data": int(fields[1]),
            "bss": int(fields[2]),
            "total": int(fields[3]),
        }
    return {}


# GNU ld map lines for an input section look like
#     .text._ZN3ac3...   0x00001234       0x56 path/to/file.o
# or, when the name is long enough to need one, the name on its own line and
# the address/size/origin on the next. Both forms are handled: the second is
# the common one for C++ mangled section names, and missing it would silently
# under-report exactly the objects this report is about.
_SECTION_NAME = re.compile(r"^\s(\.\w[\w.$-]*)")
_SECTION_BODY = re.compile(r"^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)")
_SECTION_FULL = re.compile(r"^\s(\.\w[\w.$-]*)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)")


def read_map(path: pathlib.Path) -> dict[str, dict[str, int]]:
    """Per-object .text and .bss totals from a GNU ld map."""
    per_object: dict[str, dict[str, int]] = {}
    if not path.exists():
        return per_object

    def note(section: str, size: int, origin: str) -> None:
        if size == 0:
            return
        kind = "text" if section.startswith((".text", ".rodata")) else (
            "bss" if section.startswith((".bss", ".tbss", "COMMON")) else None)
        if kind is None:
            return
        # "archive.a(member.o)" and "path/member.o" both reduce to the member.
        name = origin.rsplit("(", 1)[-1].rstrip(")")
        name = name.rsplit("/", 1)[-1]
        per_object.setdefault(name, {"text": 0, "bss": 0})[kind] += size

    pending: str | None = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        full = _SECTION_FULL.match(line)
        if full:
            pending = None
            note(full.group(1), int(full.group(3), 16), full.group(4))
            continue
        if pending is not None:
            body = _SECTION_BODY.match(line)
            if body:
                note(pending, int(body.group(2), 16), body.group(3))
            pending = None
            continue
        name = _SECTION_NAME.match(line)
        if name and len(line.rstrip()) == name.end():
            pending = name.group(1)
    return per_object


def human(value: int) -> str:
    if value >= 1024 * 1024:
        return f"{value / (1024 * 1024):.2f} MiB"
    if value >= 1024:
        return f"{value / 1024:.1f} KiB"
    return f"{value} B"


def emit(rows: list[tuple[str, str]], title: str, markdown: bool) -> None:
    if markdown:
        print(f"### {title}\n")
        print("| Measure | Value |")
        print("| --- | --- |")
        for label, value in rows:
            print(f"| {label} | {value} |")
        print()
    else:
        print(f"== {title} ==")
        width = max((len(label) for label, _ in rows), default=0)
        for label, value in rows:
            print(f"  {label.ljust(width)}  {value}")
        print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=pathlib.Path, help="the probe's key=value output")
    parser.add_argument("--elf", type=pathlib.Path, help="the linked probe")
    parser.add_argument("--map", dest="map_file", type=pathlib.Path, help="the linker map")
    parser.add_argument("--markdown", action="store_true", help="emit Markdown tables")
    args = parser.parse_args()

    probe = read_probe(args.probe) if args.probe else {}
    sizes = read_size(args.elf) if args.elf else {}

    if sizes:
        emit(
            [
                (".text (code + read-only data)", human(sizes["text"])),
                (".data (initialised)", human(sizes["data"])),
                (".bss (zero-initialised)", human(sizes["bss"])),
                ("**Image total**", f"**{human(sizes['total'])}** ({sizes['total']} bytes)"),
            ],
            "Static footprint",
            args.markdown,
        )

    if probe:
        rows = []
        for key, label in (
            ("heap.peak_bytes", "Peak heap"),
            ("static.frame_decoder_bytes", "sizeof(ac3::FrameDecoder)"),
            ("static.eac3_decoder_bytes", "sizeof(ac3::Eac3Decoder)"),
            ("static.pcm_bytes", "Caller-owned PCM (16 x 1536 float)"),
        ):
            if key in probe:
                rows.append((label, human(int(probe[key]))))
        for key, label in (
            ("ac3.steady_allocs_per_frame", "AC-3 allocations per frame (steady state)"),
            ("eac3.steady_allocs_per_frame", "E-AC-3 allocations per frame (steady state)"),
            ("ac3.first_frame_allocs", "AC-3 allocations, first frame"),
            ("eac3.first_frame_allocs", "E-AC-3 allocations, first frame"),
            ("heap.leaked_bytes", "Leaked at exit"),
        ):
            if key in probe:
                rows.append((label, probe[key]))
        if "result" in probe:
            rows.append(("Probe verdict", probe["result"]))
        if rows:
            emit(rows, "Runtime footprint", args.markdown)

    per_object = read_map(args.map_file) if args.map_file else {}
    if per_object:
        ranked = sorted(per_object.items(), key=lambda kv: -(kv[1]["text"] + kv[1]["bss"]))
        rows = []
        other_text = other_bss = 0
        for name, totals in ranked:
            if totals["text"] + totals["bss"] < SIGNIFICANT_BYTES:
                other_text += totals["text"]
                other_bss += totals["bss"]
                continue
            rows.append((name, f"{human(totals['text'])} text, {human(totals['bss'])} bss"))
        if other_text or other_bss:
            rows.append(("(everything smaller, summed)",
                         f"{human(other_text)} text, {human(other_bss)} bss"))
        emit(rows, "Where it went, by object", args.markdown)

    if not sizes and not probe and not per_object:
        print("footprint_report: nothing to report (no --probe/--elf/--map produced data)",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
