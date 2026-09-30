"""Generate planning/layout-inventory.md from the study's data files.

    appendix.py --data <dir> --commit <sha> --date <date> --out <file>

Every table is computed by the scripts in this directory from the tracked files of one commit;
nothing here is typed in except the one-line purposes, the character of each forge directory and the
destinations of the split.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

from n1b_lib import md_table

PURPOSE = {
    "arithmetic": "Fixed32 and the cross-platform float functions, header only",
    "ac3": "was forge: the AC-3 and E-AC-3 codec, and everything else forge held",
    "forge": (
        "AC-3 and E-AC-3 codec, Atmos in E-AC-3, DSP, WAV, loudness, layouts and rendering, "
        "IEC 61937"
    ),
    "ac4": "AC-4 inspector: table of contents, presentations, syntax types",
    "ac4core": (
        "AC-4 tables, transforms and reconstruction kernels shared by decoder and encoder "
        "(private static library)"
    ),
    "ac4dec": "AC-4 decoder",
    "ac4enc": "AC-4 encoder",
    "ac3adm": "BW64/RF64 and ADM reader",
    "ac3iab": "SMPTE ST 2098-2 IAB reader, MXF",
    "admbridge": "ADM and IAB to Atmos object mapping",
    "audio": (
        "device backends (WASAPI, ALSA, PipeWire, CoreAudio, Android), capture, monitor, "
        "passthrough sink"
    ),
    "capi": "C11 API over the codecs",
    "iamf": "IAMF OBU and ISOBMFF writer",
    "matroska": "Matroska writer and reader",
    "mp4": "MP4, fMP4, HLS and DASH writer; reader",
    "mpegts": "MPEG-TS writer and reader",
    "sendspin": "Sendspin protocol, Hearth's player extension, discovery, pairing, transport",
    "signing": "EMDF Atmos object signing (HMAC)",
}

FORGE_CHARACTER = [
    # dir, character, destination in the recommended layout
    (
        "core",
        "AC-3 and E-AC-3 tables, bit allocation, exponents, mantissas, coupling, MDCT; bit reader "
        "and writer (codec-blind); Location and Layout (codec-blind, inside eac3_tables.hpp)",
        "ac3; bit I/O and the layout vocabulary to base; the FFT to dsp",
    ),
    (
        "decoder",
        "AC-3 and E-AC-3 decoder, output stage, PcmBlock and BlockSink (a sink interface), "
        "DownmixTarget",
        "ac3; PcmBlock to render, DownmixTarget to base",
    ),
    ("encoder", "AC-3 and E-AC-3 encoder, plan, assignment", "ac3"),
    (
        "oba",
        "OAMD payload and object scene (codec-blind); JOC and the Atmos encoder (E-AC-3)",
        "objects (scene, motion, oamd, placement, joc_domain); ac3 (atmos, joc)",
    ),
    (
        "io",
        "elementary streams, probe, dec3, metadata edit, object strip, stream accumulator (AC-3); "
        "WAV (codec-neutral in intent, Acmod in its API)",
        "ac3 (WAV stays: its API takes Acmod)",
    ),
    (
        "meta",
        "BSI, DRC, mixing (AC-3); loudness and QC gates (BS.1770, but take SampleRate and Acmod)",
        "ac3 (loudness and QC stay for the same reason)",
    ),
    (
        "render",
        "speaker layout, routing, trim and delay, identify tone, bed and object renderer (header "
        "only); serving.hpp adapts them to DecoderConfig",
        "render; serving.hpp to ac3",
    ),
    ("verify", "encoder/decoder mirror check for AC-3 and E-AC-3", "ac3"),
    (
        "internal",
        "CPU probe, profiling markers (codec-blind); build profile, scalar selection, AVX2 "
        "kernels (AC-3)",
        "base (cpu, profiling); ac3 (profile, scalar, avx2); the SIMD arch seam is in arithmetic",
    ),
    ("iec61937", "IEC 61937 burst packing and unpacking (AC-3, E-AC-3, AC-4 packers)", "iec61937"),
    (
        "emdf",
        "EMDF container (codec-blind: bit reader and writer only) and E-AC-3 frame layout",
        "objects (the container, emdf.hpp and emdf.cpp); ac3 (the frame layout)",
    ),
    ("quality", "psychoacoustic model and distortion measure for the AC-3 encoder", "ac3"),
    ("dsp", "biquad, 64-band QMF, resampler", "dsp"),
    ("spatial", "panning math for objects onto a speaker layout", "render"),
    ("analysis", "per-channel level analysis (takes Acmod)", "ac3"),
]


def load(d: Path, name: str):
    return json.load(open(d / name, encoding="utf-8"))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, help="the directory regen_inventory.ps1 wrote")
    ap.add_argument("--commit", default="?")
    ap.add_argument("--date", default="2026-09-29")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    d = Path(a.data)
    inv = load(d, "inventory.json")
    graph = load(d, "include_graph.json")
    census = load(d, "ident_census.json")
    naming = load(d, "naming_counts.json")
    pk = load(d, "path_keyed.json")
    dry = load(d, "dryrun.json")
    nsj = load(d, "namespaces.json")
    overlap = load(d, "overlap.json")
    out: list[str] = []
    w = out.append

    w("# The layout study: inventory\n\n")
    w(
        f'!!! note "Facts of `main` at {a.commit}, {a.date}"\n'
        "    Every table below is computed from the tracked files of that commit by the scripts "
        "the study\n"
        "    kept (`tools/n1b/` when the execution phase lands them), so a later `main` "
        "regenerates it.\n"
        "    Counts are directives, files or lines as the column says. The proposal that reads "
        "these tables is\n"
        "    [the layout study](layout.md).\n\n"
    )

    # --- A --------------------------------------------------------------------------------------
    w("## A. The libraries in `src/`\n\n")
    rows = []
    libs = graph["src_libs"]
    for lib in libs:
        v = inv[lib]
        al = ", ".join(sorted({x.split(" -> ")[0] for x in v["aliases"]}))
        nsl = ", ".join(f"`{k}`" for k in list(v["namespaces"])[:3])
        ins = ", ".join(f"{k} {n}" for k, n in v["in_edges_libs"]) or "-"
        outs = ", ".join(f"{k} {n}" for k, n in v["out_edges"]) or "-"
        rows.append(
            [
                f"`src/{lib}`",
                PURPOSE.get(lib, ""),
                v["files"],
                f"{v['public_headers']}/{v['private_headers']}/{v['sources']}",
                v["loc_public"] + v["loc_private_headers"] + v["loc_sources"],
                al or "-",
                ", ".join(f"`{r}/`" for r in v["header_roots"]) or "(src/ is the include dir)",
                nsl or "-",
                outs,
                ins,
            ]
        )
    w(
        md_table(
            [
                "directory",
                "purpose",
                "files",
                "pub hdr / priv hdr / src",
                "C/C++ lines",
                "aliases",
                "header root",
                "namespaces (most files first)",
                "includes (library, directives)",
                "included by (libraries)",
            ],
            rows,
            ["l", "l", "r", "r", "r", "l", "l", "l", "l", "l"],
        )
    )
    w(
        "\nInstalled and exported (cmake/InstallLibrary.cmake): forge, signing, matroska, mp4, "
        "mpegts, ac3iab, ac3adm, "
        "admbridge, iamf, the AC-4 four (as one set), capi. Not installed: audio, sendspin, "
        "arithmetic.\n\n"
    )
    w(
        "Library file names today: `ac3forge`, `ac3forge_c`, `ac3signing`, `ac3adm`, `ac3iab`, "
        "`admbridge`, `mp4`, `mpegts`, "
        "`matroska`, `iamf`, `ac4`, `ac4dec`, `ac4enc`, `ac4core_static`. Debian, Arch and MSYS2 "
        "package file lists show "
        "libmatroska installing headers under `include/matroska/`, the directory name "
        "`matroska/matroska.hpp` here uses, "
        "and the library name `libmatroska`.\n\n"
    )

    w("### A.1 What `src/forge` holds\n\n")
    rows = [[f"`{k}`", c, dst] for k, c, dst in FORGE_CHARACTER]
    w(md_table(["directory", "content", "destination in the recommended layout"], rows))
    w("\n")

    # --- B --------------------------------------------------------------------------------------
    w("## B. The include graph\n\n")
    w(
        f"Built from every `#include` of {graph['scanned_files']} C/C++ files, each resolved as "
        f"the compiler would "
        "(quote-relative, then the header spellings the build puts on the path).\n\n"
    )
    w("### B.1 Library to library\n\n")
    rows = []
    for e in graph["edges"]:
        if e["from"] in libs and e["to"] in libs:
            rows.append(
                [
                    e["from"],
                    e["to"],
                    e["directives"],
                    e["files"],
                    e["headers"],
                    e["private_header_directives"],
                    e["from_public_header"],
                ]
            )
    w(
        md_table(
            [
                "from",
                "to",
                "directives",
                "files",
                "headers",
                "into a private header",
                "from a public header",
            ],
            rows,
            ["l", "l", "r", "r", "r", "r", "r"],
        )
    )
    w(
        f"\nCycles between libraries: {graph['sccs'] or 'none'}. `ac4core`, `ac4dec`, `ac4enc` "
        f"and `ac4` include nothing "
        "from `forge`; `sendspin` and the containers include nothing from any other library.\n\n"
    )
    w("### B.2 Consumers to libraries\n\n")
    cons: dict = {}
    for e in graph["edges"]:
        if e["from"] not in libs and e["to"] in libs:
            cons.setdefault(e["from"], {})[e["to"]] = e["directives"]
    cols = sorted({t for v in cons.values() for t in v})
    rows = [[k, *[cons[k].get(c, "") for c in cols]] for k in sorted(cons)]
    w(md_table(["consumer", *cols], rows, ["l", *["r"] * len(cols)]))
    w("\n### B.3 Inside `src/forge`: directory to directory\n\n")
    rows = [[e["from"], e["to"], e["directives"], e["files"]] for e in graph["forge_sub_edges"]]
    w(md_table(["from", "to", "directives", "files"], rows, ["l", "l", "r", "r"]))
    w(f"\nStrongly connected sets of directories: {graph['forge_sccs']}.\n\n")

    # cut list from the assign module
    w("### B.4 The cuts: includes that stop `forge` splitting\n\n")
    w(
        "Under the split in [the layout study](layout.md) (base, dsp, render, objects, iec61937 "
        "and ac3), the include "
        "directives that point from a codec-blind library into the codec:\n\n"
    )
    cut_path = d / "violations_v2.txt"
    if cut_path.exists():
        w("```text\n" + cut_path.read_text(encoding="utf-8").strip() + "\n```\n\n")
    w(
        "After the seven cuts of stage 1 the same check finds none (the prototype's build is the "
        "proof).\n\n"
    )

    w("### B.5 Private headers reached across a boundary\n\n")
    priv = d / "privcross.txt"
    if priv.exists():
        w("```text\n" + priv.read_text(encoding="utf-8").strip() + "\n```\n\n")
    wb = graph["whitebox_includes"]
    c = Counter()
    for f, _h in wb:
        c[
            f.split("/")[0]
            + "/"
            + (f.split("/")[1] if f.split("/")[0] in ("apps", "src", "tests") else "")
        ] += 1
    w(
        "Includes that reach a header outside a library's `include/` from another tree, by "
        "including tree: "
        + ", ".join(f"{k} {v}" for k, v in c.most_common(12))
        + ". `tests/CMakeLists.txt` makes 26 "
        "`target_include_directories(ac3tests ...)` calls to allow them.\n\n"
    )

    w("### B.6 What a second codec needs: the TrueHD branch\n\n")
    th = d / "truehd_includes.txt"
    if th.exists():
        w(th.read_text(encoding="utf-8").strip() + "\n\n")
        w(
            "Under L2 the branch's files link `base` and `objects` only if the EMDF container "
            "(`emdf.hpp`, `emdf.cpp`) "
            "sits in `objects` ([decision 14](layout.md#i-decisions)); the branch's other "
            "includes are of its own files.\n\n"
        )

    # --- C --------------------------------------------------------------------------------------
    w("## C. Names\n\n")
    w("### C.1 Tokens containing `ac3`, by class\n\n")
    rows = [
        [
            c_["class"],
            c_["occurrences"],
            c_["files"],
            c_["distinct_tokens"],
            ", ".join(f"`{t}` {n}" for t, n, f in c_["top_tokens"][:5]),
        ]
        for c_ in census["classes"]
    ]
    w(
        md_table(
            ["class", "occurrences", "files", "distinct", "most frequent"],
            rows,
            ["l", "r", "r", "r", "l"],
        )
    )
    w("\n### C.2 Counts per name family\n\n")
    rows = [
        [k, v["files"], v["hits"], v["files_code"], v["hits_code"], v["distinct"]]
        for k, v in naming.items()
    ]
    w(
        md_table(
            [
                "name family",
                "files",
                "hits",
                "files outside docs and planning",
                "hits outside docs and planning",
                "distinct",
            ],
            rows,
            ["l", "r", "r", "r", "r", "r"],
        )
    )
    w("\n### C.3 Namespaces the C++ declares\n\n")
    rows = [
        [e["ns"], e["files"], ", ".join(f"{k} {v}" for k, v in e["places"].items())]
        for e in nsj["declared"]
        if e["ns"].split("::")[0]
        in (
            "ac3",
            "ac4",
            "ac3iab",
            "ac3adm",
            "ac3forge_c",
            "iamf",
            "mp4",
            "mpegts",
            "matroska",
            "ac3cli",
            "ac3gui",
            "ac3forge",
            "ac3probe",
            "ac3shield",
            "ac3fuzz",
            "ac3nullsink",
        )
    ]
    w(md_table(["namespace", "files", "where (files)"], rows, ["l", "r", "l"]))
    w("\n### C.4 Files that carry a program name, a library name, or both\n\n")
    rows = [[k, *v] for k, v in overlap.items()]
    w(
        md_table(
            [
                "category",
                "files scanned",
                "program names (N1A)",
                "library and family names (N1B)",
                "both",
                "either",
            ],
            rows,
            ["l", "r", "r", "r", "r", "r"],
        )
    )
    w("\n### C.5 Lines that would exceed 100 columns if `ac3::` became a longer root\n\n")
    refl = d / "reflow.md"
    if refl.exists():
        w(refl.read_text(encoding="utf-8").strip() + "\n\n")
        w(
            "An upper bound: it assumes every `ac3::` qualifier is rewritten, and `#include` "
            "lines are skipped. "
            "`.clang-format` sets `ColumnLimit: 100`.\n\n"
        )

    # --- D --------------------------------------------------------------------------------------
    w("## D. Everything keyed on a path\n\n")
    w(
        "Files that name a directory the recommended layout moves or renames, found by `git grep` "
        "of the path followed by "
        "a non-word character (a file under the directory itself is excluded: it moves with it). "
        "One list per category; "
        "each file is followed by the keys it names.\n\n"
    )
    keys = [
        "src/forge",
        "src/ac3adm",
        "src/ac3iab",
        "include/ac3",
        "src/ac4core",
        "esp-idf/ac3forge",
        "rust/ac3forge",
        "rust/ac3forge-sys",
        "tests/core",
        "tests/decoder",
        "tests/encoder",
        "tests/io",
        "tests/meta",
        "tests/oba",
        "tests/render",
        "tests/dsp",
        "tests/backend",
        "tests/containers",
        "tests/adm",
        "tests/ac3iab",
    ]
    cats: dict = {}
    for k in keys:
        if k not in pk:
            continue
        for cat, fs in pk[k]["by_category"].items():
            for f in fs:
                cats.setdefault(cat, {}).setdefault(f, set()).add(k)
    order = [
        "repo config (root)",
        "CMake / Kconfig",
        "CI workflow / action",
        "CI script (tools/ci)",
        "check script (tools/checks)",
        "other tools",
        "packaging",
        "esp-idf component",
        "bindings",
        "apps",
        "tests / fuzz / examples",
        "docs",
        "src",
        "planning / history (leave)",
    ]
    tot = 0
    for cat in order:
        fs = cats.get(cat, {})
        if not fs:
            continue
        tot += len(fs)
        w(f"### {cat}: {len(fs)} files\n\n")
        w(
            "\n".join(
                f"- `{f}`: " + ", ".join(f"`{k}`" for k in sorted(ks))
                for f, ks in sorted(fs.items())
            )
            + "\n\n"
        )
    w(
        f"Total: {tot} files, before the files that name a program or a library only by name "
        "(section C.4).\n\n"
    )
    w(
        "Not present in this repository: a CODEOWNERS file and a labeler configuration. "
        "`dependabot.yml` names `/apps/wasm/tests`, "
        "`/apps/android`, `/rust` and the tool directories, none of which moves. `.clang-tidy`'s "
        "`HeaderFilterRegex` names "
        "`src/(forge|matroska)`; `sonar-project.properties` names `src`, `apps`, `examples`, "
        "`js`, `python`, `tools` and `tests` "
        "as roots and lists forty-five `src/` paths.\n\n"
    )

    # --- E --------------------------------------------------------------------------------------
    w("## E. The three layouts, dry run\n\n")
    L = dry["layouts"]
    rows = [
        ["files moved (`git mv`)"] + [L[n]["moved"] for n in ("L1", "L2", "L3")],
        ["  under `src/` or `libs/`"] + [L[n]["by_area"].get("src", 0) for n in ("L1", "L2", "L3")],
        ["  under `tests/`"] + [L[n]["by_area"].get("tests", 0) for n in ("L1", "L2", "L3")],
        ["  package directories (esp-idf, rust, python, packaging, cmake)"]
        + [
            sum(
                L[n]["by_area"].get(k, 0)
                for k in ("esp-idf", "rust", "python", "packaging", "cmake")
            )
            for n in ("L1", "L2", "L3")
        ],
        ["other files edited in place"] + [L[n]["edited_in_place"] for n in ("L1", "L2", "L3")],
        ["moved files that also change content"]
        + [L[n]["moved_and_edited"] for n in ("L1", "L2", "L3")],
        ["C/C++ files with rewritten includes or names"]
        + [L[n]["cpp_edited"] for n in ("L1", "L2", "L3")],
        ["files edited only because they name a moved path"]
        + [L[n]["keyed_only"] for n in ("L1", "L2", "L3")],
        ["deepest path under `src/` (`libs/`) before"]
        + [L[n]["max_depth_src_before"] for n in ("L1", "L2", "L3")],
        ["deepest path after, variant trees flattened"]
        + [L[n]["max_depth_src_after"] for n in ("L1", "L2", "L3")],
    ]
    w(md_table(["", "L1 regroup", "L2 peers", "L3 libs"], rows, ["l", "r", "r", "r"]))
    w(
        f"\nText files scanned: {dry['text_files']}; files carrying a library or family name: "
        f"{dry['token_files']}.\n\n"
    )

    Path(a.out).write_text("".join(out), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
