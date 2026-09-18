#!/usr/bin/env python3
"""Replace legacy ROADMAP XXn comment references with plain English."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SCAN_DIRS = (
    "src",
    "apps",
    "tests",
    "cmake",
    "examples",
    "fuzz",
    "python",
    ".github",
    "esp-idf",
    "tools",
)

SKIP_FILES = {
    "CHANGELOG.md",
    "ROADMAP.md",
    "check_doc_paths.py",
    "test_check_doc_paths.py",
    "rewrite_roadmap_comments.py",
}

SKIP_DIR_PARTS = {"planning", ".cxx", "build", "node_modules", ".venv"}

LEGACY_ID_NAMES = {
    "A1": "Atmos dec3 repair remux",
    "A2": "fMP4/CMAF",
    "A4": "CLI stdin/stdout streaming",
    "AP6": "Python bindings completeness",
    "AP9": "Rust bindings",
    "AP12": "research trace export",
    "B1": "ADM BWF reader",
    "C3": "GUI QC verification",
    "DR2": "manifest bump automation",
    "DR7": "Windows NSIS installer",
    "DR8": "Windows ARM64 / reach builds",
    "DR9": "hardware verification",
    "EQ3": "E-AC-3 bamode transmission",
    "EQ7": "E-AC-3 fast-gain control",
    "F2": "Python on PyPI",
    "F3": "WASM browser demo",
    "G3": "differential decoder fuzzing",
    "G4": "encoder input-space fuzzing",
    "IM1": "IAB reader",
    "IM3": "IAMF writer",
    "IM4": "AC-4 bitstream inspector",
    "IO2": "container readers (mkv/mp4/ts)",
    "IO3": "IEC 61937 de-framing",
    "IO6": "MPEG-TS broadcast profiles",
    "IO7": "object-layer strip",
    "IO8": "CLI shell completions",
    "IO9": "wide-layout record/live paths",
    "IO11": "QC preset refresh",
    "PF1": "encoder/decode benchmarks",
    "PF5": "SIMD kernels",
    "PF7": "minimum-footprint decoder profile",
    "PF8": "JOC bed forward-transform reuse",
    "UX1": "GUI stream player",
    "UX2": "GUI AppStream packaging",
    "UX3": "GUI localisation",
    "UX4": "live OSC object positions",
    "UX5": "WASM streaming decoder package",
    "UX6": "in-browser encoding",
    "UX8": "Windows spatial object renderer",
    "UX9": "play/monitor follow mode",
    "UX11": "WASAPI loopback tap",
    "UX12": "Crucible cross-platform promotion",
    "VX1": "E-AC-3 encoder fuzzing",
    "VX3": "signing-verify fuzz walk",
    "VX10": "reference-mode end-to-end gate",
    "VX11": "cross-platform bitstream reproducibility",
    "VX12": "cross-toolchain bitstream audit",
    "VX13": "promoted fuzz jobs",
    "VX14": "non-C++ lint and scan",
    "VX15": "coverage floors",
    "VX16": "ThreadSanitizer leg",
    "VX17": "PR-time performance comparison",
    "VX18": "WASM/mobile headless coverage",
    "VX18a": "WASM Playwright coverage",
    "VX18b": "Android JNI instrumented coverage",
    "VX20": "conformance vectors publication",
    "VX2": "E-AC-3 mirror self-check",
    "VX4": "third-party decode interop",
    "PF6": "bare-metal probe harness",
    "DC1": "decoder output stage",
    "DC2": "decoder concealment",
    "DC5": "broadcast DD+ decode",
    "DC9": "stream tools",
    "EQ1": "content-driven bit allocation",
    "EQ5": "AHT scope boundary",
    "EQ11": "E-AC-3 short syncframes",
    "EQ12": "Python trim gap",
    "EQ13": "per-frame bit-allocation search",
    "IO1": "probe command",
    "IO4": "fragmented MP4 writer",
    "C2": "bitstream-aware loudness QC",
    "AP1": "C API version macros",
    "AP7": "packaging metadata parity",
    "AP11": "decoder diagnostics describe()",
    "F1": "C API",
    "DR6": "macOS code signing",
    "A5": "Hearth About page",
    "A7": "Hearth package component",
}

REPLACEMENTS: list[tuple[str, str]] = [
    (
        r'roadmap item IM1 phase 3 \("IAB \(SMPTE ST 2098-2\) reader", see ROADMAP\.md\)',
        "IAB reader bridge, phase 3",
    ),
    (
        r'Roadmap IM1 phase 3 of 3 \("IAB \(SMPTE ST 2098-2\) reader", see ROADMAP\.md\)',
        "IAB reader bridge, phase 3",
    ),
    (
        r'roadmap item B1 phase 3 of 3 \("ADM BWF reader feeding the JOC encoder", see ROADMAP\.md\)',
        "ADM BWF → JOC bridge, phase 3",
    ),
    (
        r'roadmap item B1 phase 2 \("ADM BWF reader feeding the JOC encoder", see ROADMAP\.md\)',
        "ADM BWF → JOC bridge, phase 2",
    ),
    (
        r'roadmap item B1 phase 1 \("ADM BWF reader feeding the JOC encoder", see ROADMAP\.md\)',
        "ADM BWF reader, phase 1",
    ),
    (r"roadmap item IM3 phase 1", "IAMF writer, phase 1"),
    (r"roadmap item IM1 phase 3", "IAB reader bridge, phase 3"),
    (r"roadmap item IM1 phase 2", "IAB reader, phase 2"),
    (r"roadmap item IM1 phase 1", "IAB reader, phase 1"),
    (r"roadmap item B1 phase 2", "ADM BWF → JOC bridge, phase 2"),
    (r"roadmap item B1 phase 1", "ADM BWF reader, phase 1"),
    (r"roadmap item F1", "C API"),
    (r"roadmap item A4", "CLI stdin/stdout streaming"),
    (r"roadmap item C1", "full R128 metering"),
    (r"the old roadmap A1 cited", "the Atmos dec3-repair remux case"),
    (r"old roadmap A1 cited", "the Atmos dec3-repair remux case"),
    (r"ROADMAP\.md's B2 entry \(a future DAMF", "a future DAMF reader (out of scope; was B2"),
    (r"ROADMAP\.md's IO2", "container readers (mkv/mp4/ts)"),
    (r"ROADMAP\.md's IO4", "Matroska/MP4/MPEG-TS muxers"),
    (r"ROADMAP\.md's A2", "fMP4/CMAF segmenting"),
    (r"ROADMAP\.md's IM1 entry", "IAB reader scope"),
    (r"ROADMAP\.md's VX22", "CLI container command tests"),
    (r"ROADMAP\.md's DR7", "Windows NSIS installer"),
    (r"ROADMAP\.md DR9", "hardware verification on real Macs"),
    (r"ROADMAP\.md DR6", "macOS/Windows code signing"),
    (r"\(ROADMAP\.md DR9\)", "(hardware verification)"),
    (r"ROADMAP PF5's dynamic-dispatch follow-on", "runtime SIMD dispatch"),
    (r"PF5's dynamic-dispatch follow-on", "runtime SIMD dispatch"),
    (r"ROADMAP PF5's batch-axis follow-on", "batched SIMD kernels"),
    (r"ROADMAP PF5 phase 4c", "batched MDCT (four blocks)"),
    (r"ROADMAP PF5 phase", "SIMD batched MDCT"),
    (r"\(ROADMAP PF5\)", "(SIMD kernels)"),
    (r"ROADMAP PF5", "SIMD kernels"),
    (r"ROADMAP PF4", "FFT core follow-ups"),
    (r"ROADMAP PF2", "inline to_fixed25 fusion"),
    (r"ROADMAP PF1", "encoder/decode benchmarks"),
    (r"ROADMAP PF8", "JOC bed forward-transform reuse"),
    (r"ROADMAP UX4", "live OSC object positions"),
    (r"ROADMAP VX11", "cross-platform bitstream reproducibility"),
    (r"confirming ROADMAP\.md's claim", "confirming the IAB MXF planning record"),
    (r"\(ROADMAP\s*\n\s*#\s*PF5", "(SIMD kernels"),
    (r"\(ROADMAP\s*\n\s*//\s*PF5", "(SIMD kernels"),
    (r"See ROADMAP\.md's\s*\n#\s*VX11 entry", "See cross-platform reproducibility notes below"),
    (r"See cross-platform reproducibility \(VX11\)\s*\n#\s*VX11 entry", "See cross-platform reproducibility notes below"),
    (r"See cross-platform bitstream reproducibility, which this pins the policy half\s*\n#\s*of\.", "See cross-platform bitstream reproducibility policy for the half this option pins."),
    (r"See ROADMAP\.md's", "See the roadmap's"),
    (r", see ROADMAP\.md\)", ")"),
    (r"see ROADMAP\.md\)", ")"),
    (r" \(see ROADMAP\.md\)", ""),
    (r", see ROADMAP\.md", ""),
    (r"see ROADMAP\.md", ""),
    (r"--- roadmap ", "--- "),
    (r"also VX11\)", "also cross-platform bitstream reproducibility)"),
    (r"\(ROADMAP DC1/DC2\)", "(decoder output stage and concealment)"),
    (r"\(roadmap\s*\n\s*#\s*VX14\)", "(non-C++ lint and scan)"),
    (
        r"See cross-platform reproducibility \(VX11\)\s*\n#\s*VX11 entry for the fuller note",
        "See cross-platform reproducibility notes below for the fuller context",
    ),
]


def name_for_id(legacy_id: str) -> str:
    if legacy_id in LEGACY_ID_NAMES:
        return LEGACY_ID_NAMES[legacy_id]
    base = re.sub(r"[a-z]$", "", legacy_id)
    if base in LEGACY_ID_NAMES:
        return LEGACY_ID_NAMES[base]
    return f"legacy item {legacy_id}"


def replace_bare_roadmap_ids(text: str) -> str:
    def repl(match: re.Match[str]) -> str:
        legacy_id = match.group(1)
        tail = match.group(2) or ""
        name = name_for_id(legacy_id)
        if tail.startswith("'s"):
            return f"{name}'s{tail[2:]}"
        if tail.startswith(" phase") or tail.startswith(" phases"):
            return f"{name}{tail}"
        if tail.startswith(")"):
            return f"{name})"
        if tail.startswith(","):
            return f"{name},{tail[1:]}"
        if tail.startswith(":"):
            return f"{name}:{tail[1:]}"
        return name + tail

    text = re.sub(
        r"(?i)\b(?:roadmap|ROADMAP(?!\.md))\s+([A-Z]{1,2}[0-9]+[a-z]?)((?:'s\b[^)\n]*)?|\s+phase[s]?[^)\n]*)?",
        repl,
        text,
    )
    return re.sub(
        r"roadmap ([A-Z]{1,2}[0-9]+[a-z]?)/([A-Z]{1,2}[0-9]+[a-z]?)",
        lambda m: (
            f"{name_for_id(m.group(1))}/"
            f"{name_for_id(m.group(2))}"
        ),
        text,
    )


def iter_files() -> list[Path]:
    out: list[Path] = []
    for rel in SCAN_DIRS:
        base = ROOT / rel
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            if any(part in SKIP_DIR_PARTS for part in path.parts):
                continue
            if path.suffix.lower() not in {
                ".cpp",
                ".hpp",
                ".h",
                ".cmake",
                ".txt",
                ".yml",
                ".yaml",
                ".md",
                ".py",
                ".rb",
                ".mm",
                ".idl",
                ".qml",
                ".js",
                ".kt",
                ".kts",
                ".html",
                ".xml",
                ".ld",
                ".gradle",
                ".sh",
                ".ps1",
                ".pyi",
                ".toml",
            }:
                continue
            if "golden" in path.parts and path.suffix.lower() == ".json":
                continue
            if path.name in SKIP_FILES:
                continue
            out.append(path)
    for extra in (
        ROOT / "CMakeLists.txt",
        ROOT / "CMakePresets.json",
        ROOT / "ruff.toml",
        ROOT / "python" / "pyproject.toml",
        ROOT / "rust" / "README.md",
    ):
        if extra.exists():
            out.append(extra)
    return sorted(set(out))


def rewrite(text: str) -> str:
    for pattern, repl in REPLACEMENTS:
        text = re.sub(pattern, repl, text)
    return replace_bare_roadmap_ids(text)


def main() -> int:
    write = "--write" in sys.argv
    changed: list[Path] = []
    for path in iter_files():
        original = path.read_text(encoding="utf-8")
        if not re.search(
            r"(?i)ROADMAP(?!\.)|roadmap [A-Z]{1,2}[0-9]|roadmap item",
            original,
        ):
            continue
        updated = rewrite(original)
        if updated != original:
            changed.append(path)
            if write:
                path.write_text(updated, encoding="utf-8", newline="\n")
    print(f"{'Wrote' if write else 'Would change'} {len(changed)} files")
    for path in changed:
        print(path.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
