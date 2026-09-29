"""Library inventory: one row per src/ directory.

Usage: inventory.py [--root R] [--json out.json] [--md out.md]
Facts only: file and line counts, CMake targets and aliases, the namespaces the C++ declares, the
header roots, the include-graph neighbours (from include_graph.json) and the install/export state.
"""

from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

from n1b_lib import HEADER_EXT, NAMESPACE_RE, Repo, base_parser, emit, md_table

ADD_LIB = re.compile(
    r"add_library\(\s*([A-Za-z0-9_:+.\-${}]+)\s+(OBJECT|STATIC|SHARED|INTERFACE|ALIAS|MODULE)\s*([A-Za-z0-9_:+.\-${}]*)",
    re.M,
)
ADD_EXE = re.compile(r"add_executable\(\s*([A-Za-z0-9_:+.\-${}]+)", re.M)
OUT_NAME = re.compile(r"OUTPUT_NAME\s+\"([^\"]+)\"")
EXPORT_SET = re.compile(r"install\(EXPORT\s+(\w+)\s+FILE\s+\S+\s+NAMESPACE\s+([\w:]+)", re.M)


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    ap.add_argument("--graph", required=True, help="include_graph.json from include_graph.py")
    a = ap.parse_args()
    repo = Repo(a.root)
    graph = json.loads(Path(a.graph).read_text(encoding="utf-8"))
    libs = graph["src_libs"]
    install_text = repo.read("cmake/InstallLibrary.cmake")
    exports = {m.group(1): m.group(2) for m in EXPORT_SET.finditer(install_text)}

    rows = []
    data = {}
    for lib in libs:
        prefix = f"src/{lib}/"
        files = [f for f in repo.files if f.startswith(prefix)]
        pub_h = [
            f
            for f in files
            if "/include/" in f and (repo.ext(f) in HEADER_EXT or f.endswith(".in"))
        ]
        priv_h = [f for f in files if repo.ext(f) in HEADER_EXT and "/include/" not in f]
        srcs = [f for f in files if repo.ext(f) in (".cpp", ".cc", ".c", ".mm")]
        cmakes = [f for f in files if f.endswith("CMakeLists.txt") or f.endswith(".cmake")]
        other = [f for f in files if f not in set(pub_h) | set(priv_h) | set(srcs) | set(cmakes)]

        def loc(fs):
            return sum(repo.loc(f) for f in fs)

        # cmake targets
        targets, aliases, exes, outs = [], [], [], []
        for c in cmakes:
            t = repo.read(c)
            for m in ADD_LIB.finditer(t):
                if m.group(2) == "ALIAS":
                    aliases.append(f"{m.group(1)} -> {m.group(3)}")
                else:
                    targets.append(f"{m.group(1)} ({m.group(2).lower()})")
            exes += ADD_EXE.findall(t)
            outs += OUT_NAME.findall(t)
        # namespaces
        ns = Counter()
        for f in pub_h + priv_h + srcs:
            seen = {m.group(1) for m in NAMESPACE_RE.finditer(repo.read(f))}
            for n in seen:
                ns[n] += 1
        # header roots
        roots = sorted({f.split("/include/", 1)[1].split("/")[0] for f in pub_h})
        # direction
        out_edges = [
            (e["to"], e["directives"])
            for e in graph["edges"]
            if e["from"] == lib and e["to"] in libs
        ]
        in_lib = [
            (e["from"], e["directives"])
            for e in graph["edges"]
            if e["to"] == lib and e["from"] in libs
        ]
        in_cons = [
            (e["from"], e["directives"])
            for e in graph["edges"]
            if e["to"] == lib and e["from"] not in libs
        ]
        data[lib] = {
            "files": len(files),
            "public_headers": len(pub_h),
            "private_headers": len(priv_h),
            "sources": len(srcs),
            "cmake": len(cmakes),
            "other": len(other),
            "loc_public": loc(pub_h),
            "loc_private_headers": loc(priv_h),
            "loc_sources": loc(srcs),
            "targets": targets,
            "aliases": aliases,
            "executables": exes,
            "output_names": outs,
            "namespaces": dict(ns.most_common()),
            "header_roots": roots,
            "out_edges": out_edges,
            "in_edges_libs": in_lib,
            "in_edges_consumers": in_cons,
        }
        rows.append(
            [
                lib,
                len(files),
                len(pub_h),
                len(priv_h),
                len(srcs),
                loc(pub_h) + loc(priv_h) + loc(srcs),
            ]
        )
    data["_exports"] = exports
    if a.json:
        emit(json.dumps(data, indent=1), a.json)
    lines = [
        md_table(
            ["library", "files", "public hdr", "private hdr", "sources", "C/C++ lines"],
            rows,
            ["l", "r", "r", "r", "r", "r"],
        )
    ]
    lines.append("\n")
    for lib in libs:
        d = data[lib]
        lines.append(f"### {lib}\n")
        lines.append(f"- targets: {', '.join(d['targets']) or '-'}\n")
        lines.append(f"- aliases: {', '.join(d['aliases']) or '-'}\n")
        lines.append(f"- output names: {', '.join(d['output_names']) or '-'}\n")
        lines.append(
            f"- header roots: {', '.join(d['header_roots']) or '(none: src/ is the include dir)'}\n"
        )
        lines.append(
            "- namespaces (files): "
            + ", ".join(f"{k} ({v})" for k, v in list(d["namespaces"].items())[:14])
            + "\n"
        )
        lines.append(
            f"- out: {d['out_edges']}\n- in (libs): {d['in_edges_libs']}\n"
            f"- in (consumers): {d['in_edges_consumers']}\n\n"
        )
    lines.append("Export sets: " + repr(exports) + "\n")
    text = "".join(lines)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
