"""Include graph of the tree, from the real #include lines.

Resolves every project include to the owning node (a src/ library, or a consumer group such as
apps/cli or tests) with the compiler's own search order approximated:
  1. quote-relative to the including file;
  2. the spelling index (public headers by their path under include/, private headers by every
     path suffix), preferring the including library's own file;
  3. anything else is external (standard library, Qt, fmt, Catch2...) and counted apart.

Usage: include_graph.py [--root R] [--json out.json] [--md out.md]
"""

from __future__ import annotations

import json
import posixpath
from collections import Counter, defaultdict

from n1b_lib import CPP_EXT, HEADER_EXT, INCLUDE_RE, Repo, base_parser, emit, md_table, tarjan_scc


def node_of(path: str) -> str:
    p = path.split("/")
    if p[0] == "src" and len(p) > 2:
        return p[1]
    if p[0] == "apps" and len(p) > 2:
        return "apps/" + p[1]
    if p[0] == "tests":
        return "tests"
    if p[0] in (
        "fuzz",
        "examples",
        "python",
        "rust",
        "js",
        "esp-idf",
        "tools",
        "esphome",
        "packaging",
        "docs",
        "docs-snippets",
        "cmake",
        "overrides",
    ):
        return p[0]
    return "(root)"


def sub_of_forge(path: str) -> str | None:
    """Directory-level node inside src/forge."""
    p = path.split("/")
    if p[:2] != ["src", "forge"]:
        return None
    if len(p) > 4 and p[2] == "include" and p[3] == "ac3":
        return p[4] if len(p) > 5 else "(root)"
    if len(p) > 3 and p[2] == "src":
        return p[3] if len(p) > 4 else "(root)"
    return "(build)"


def public_spelling(path: str) -> str | None:
    if "/include/" in path:
        return path.split("/include/", 1)[1]
    return None


def build_index(repo: Repo):
    index: dict[str, list[str]] = defaultdict(list)
    public_of: dict[str, str] = {}
    for f in repo.files:
        ext = repo.ext(f)
        is_tmpl = f.endswith(".hpp.in") or f.endswith(".h.in")
        if ext not in HEADER_EXT and not is_tmpl:
            continue
        g = f[:-3] if is_tmpl else f
        ps = public_spelling(g)
        if ps:
            index[ps].append(f)
            public_of[f] = ps
        if ps:
            continue  # a public header is reachable only by its public spelling
        parts = g.split("/")
        # private spellings: every suffix of the path (bounded to 5 components)
        for i in range(len(parts)):
            suf = "/".join(parts[i:])
            if suf.count("/") <= 4:
                index[suf].append(f)
    return index, public_of


def resolve(repo: Repo, index, frm: str, spelling: str, from_lib: str):
    """Return (owner_node, resolved_file or None, is_public)."""
    d = posixpath.dirname(frm)
    cand = posixpath.normpath(posixpath.join(d, spelling))
    if repo.exists(cand):
        return node_of(cand), cand, "/include/" in cand
    files = index.get(spelling)
    if not files:
        return None, None, False
    # prefer the includer's own library
    own = [c for c in files if node_of(c) == from_lib]
    pick = own[0] if own else sorted(files)[0]
    if not own:
        pub = [c for c in files if "/include/" in c]
        if pub:
            pick = sorted(pub)[0]
        if len({node_of(c) for c in files}) > 1:
            AMBIGUOUS[(from_lib, spelling)] = sorted({node_of(c) for c in files})
    return node_of(pick), pick, "/include/" in pick


AMBIGUOUS: dict = {}


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    a = ap.parse_args()
    repo = Repo(a.root)
    index, public_of = build_index(repo)

    edges = Counter()  # (from_node, to_node) -> directives
    edge_files = defaultdict(set)  # includer files
    edge_headers = defaultdict(set)  # included headers
    edge_private = Counter()  # directives resolving to a header outside include/
    edge_pubhdr_from = Counter()  # directives made from a PUBLIC header of the source library
    unresolved = Counter()
    external = Counter()
    sub_edges = Counter()  # forge directory-level
    sub_files = defaultdict(set)
    whitebox = []  # (from, header) consumer -> private header of another node
    per_file_includes = {}

    scanned = 0
    for f in repo.files:
        ext = repo.ext(f)
        if ext not in CPP_EXT and not (f.endswith(".hpp.in") or f.endswith(".h.in")):
            continue
        scanned += 1
        text = repo.read(f)
        frm = node_of(f)
        for m in INCLUDE_RE.finditer(text):
            kind, sp = m.group(1), m.group(2).strip()
            owner, target, is_pub = resolve(repo, index, f, sp, frm)
            if owner is None:
                # system or external header, or a generated one
                head = sp.split("/")[0]
                if head in (
                    "ac3",
                    "ac4",
                    "ac3forge_c",
                    "ac4dec",
                    "ac4enc",
                    "iamf",
                    "mp4",
                    "mpegts",
                    "matroska",
                    "ac3iab",
                    "ac3adm",
                ):
                    unresolved[sp] += 1
                else:
                    external[head if "/" in sp else "<" + sp + ">" if kind == "<" else sp] += 1
                continue
            per_file_includes.setdefault(f, []).append((sp, target, owner))
            if owner == frm:
                # intra-node: for forge keep the directory-level graph
                if frm == "forge":
                    a_sub, b_sub = sub_of_forge(f), sub_of_forge(target)
                    if a_sub and b_sub and a_sub != b_sub:
                        sub_edges[(a_sub, b_sub)] += 1
                        sub_files[(a_sub, b_sub)].add(f)
                continue
            key = (frm, owner)
            edges[key] += 1
            edge_files[key].add(f)
            edge_headers[key].add(target)
            if not is_pub:
                edge_private[key] += 1
                whitebox.append((f, target))
            if f in public_of or "/include/" in f:
                edge_pubhdr_from[key] += 1

    src_libs = sorted({node_of(f) for f in repo.files if f.startswith("src/") and f.count("/") > 2})
    lib_graph: dict[str, set[str]] = {lib: set() for lib in src_libs}
    for a_, b_ in edges:
        if a_ in lib_graph and b_ in lib_graph:
            lib_graph[a_].add(b_)
    sccs = tarjan_scc(lib_graph)
    forge_graph: dict[str, set[str]] = defaultdict(set)
    for a_, b_ in sub_edges:
        forge_graph[a_].add(b_)
    forge_sccs = tarjan_scc(dict(forge_graph))

    data = {
        "scanned_files": scanned,
        "src_libs": src_libs,
        "edges": [
            {
                "from": a_,
                "to": b_,
                "directives": n,
                "files": len(edge_files[(a_, b_)]),
                "headers": len(edge_headers[(a_, b_)]),
                "private_header_directives": edge_private[(a_, b_)],
                "from_public_header": edge_pubhdr_from[(a_, b_)],
            }
            for (a_, b_), n in sorted(edges.items())
        ],
        "sccs": sccs,
        "forge_sub_edges": [
            {"from": a_, "to": b_, "directives": n, "files": len(sub_files[(a_, b_)])}
            for (a_, b_), n in sorted(sub_edges.items())
        ],
        "forge_sccs": forge_sccs,
        "unresolved_project_spellings": dict(unresolved),
        "whitebox_includes": sorted(set(whitebox)),
        "ambiguous": [[k[0], k[1], v] for k, v in sorted(AMBIGUOUS.items())],
        "per_file_includes": {
            f: [[s, t, o] for (s, t, o) in v] for f, v in per_file_includes.items()
        },
    }
    if a.json:
        emit(json.dumps(data, indent=1), a.json)

    lines = []
    lines.append(f"scanned {scanned} C/C++ files\n")
    lines.append("## Library-to-library include edges (src/ libraries only)\n")
    rows = []
    for e in data["edges"]:
        if e["from"] in lib_graph and e["to"] in lib_graph:
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
    lines.append(
        md_table(
            ["from", "to", "directives", "files", "headers", "into private hdr", "from public hdr"],
            rows,
            ["l", "l", "r", "r", "r", "r", "r"],
        )
    )
    lines.append("\nSCCs (library level): " + repr(sccs) + "\n")
    lines.append("\n## Consumer groups -> libraries\n")
    rows = []
    for e in data["edges"]:
        if e["from"] not in lib_graph and e["to"] in lib_graph:
            rows.append(
                [e["from"], e["to"], e["directives"], e["files"], e["private_header_directives"]]
            )
    lines.append(
        md_table(
            ["consumer", "library", "directives", "files", "into private hdr"],
            rows,
            ["l", "l", "r", "r", "r"],
        )
    )
    lines.append("\n## forge directory graph\n")
    rows = [[e["from"], e["to"], e["directives"], e["files"]] for e in data["forge_sub_edges"]]
    lines.append(md_table(["from", "to", "directives", "files"], rows, ["l", "l", "r", "r"]))
    lines.append("\nforge SCCs: " + repr(forge_sccs) + "\n")
    lines.append("\nUnresolved project spellings: " + repr(dict(unresolved)) + "\n")
    lines.append("\nExternal top spellings: " + repr(external.most_common(40)) + "\n")
    text = "".join(lines)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
