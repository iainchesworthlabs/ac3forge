"""Shared helpers for the N1B layout study scripts.

Every script takes --root <worktree> (default: the study worktree) and reads the tracked files
of that worktree with `git ls-files`, so the scripts re-run against a moving main.

Nothing here writes to the repository. Output goes to stdout or to --out.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

DEFAULT_ROOT = str(Path(__file__).resolve().parents[2])

CPP_EXT = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".mm"}
HEADER_EXT = {".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp"}
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.M)
NAMESPACE_RE = re.compile(r"^[ \t]*(?:inline[ \t]+)?namespace[ \t]+([A-Za-z_][\w:]*)[ \t]*\{", re.M)


def base_parser(description: str) -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--root", default=DEFAULT_ROOT, help="worktree to read (tracked files only)")
    p.add_argument("--out", default=None, help="write to this file instead of stdout")
    return p


class Repo:
    def __init__(self, root: str):
        self.root = Path(root)
        raw = subprocess.run(
            ["git", "-C", str(self.root), "ls-files", "-z"],
            capture_output=True,
            check=True,
        ).stdout.decode("utf-8", "surrogateescape")
        self.files = sorted(f for f in raw.split("\0") if f)
        self.fileset = set(self.files)
        self._text: dict[str, str] = {}

    def exists(self, rel: str) -> bool:
        return rel in self.fileset

    def read(self, rel: str) -> str:
        t = self._text.get(rel)
        if t is None:
            try:
                t = (self.root / rel).read_bytes().decode("utf-8", "replace")
            except OSError:
                t = ""
            self._text[rel] = t
        return t

    def loc(self, rel: str) -> int:
        t = self.read(rel)
        if not t:
            return 0
        return t.count("\n") + (0 if t.endswith("\n") else 1)

    def ext(self, rel: str) -> str:
        return os.path.splitext(rel)[1].lower()


def emit(text: str, out: str | None) -> None:
    if out:
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        Path(out).write_text(text, encoding="utf-8", newline="\n")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stdout.write(text)


def md_table(headers: list[str], rows: list[list], align: list[str] | None = None) -> str:
    def cell(v) -> str:
        return str(v).replace("|", "\\|").replace("\n", " ")

    out = ["| " + " | ".join(headers) + " |"]
    if align is None:
        align = ["l"] * len(headers)
    sep = []
    for a in align:
        sep.append({"l": "---", "r": "---:", "c": ":---:"}[a])
    out.append("| " + " | ".join(sep) + " |")
    for r in rows:
        out.append("| " + " | ".join(cell(c) for c in r) + " |")
    return "\n".join(out) + "\n"


def tarjan_scc(graph: dict[str, set[str]]) -> list[list[str]]:
    """Strongly connected components of size > 1 (or with a self loop), sorted."""
    index = {}
    low = {}
    stack: list[str] = []
    onstack: set[str] = set()
    result: list[list[str]] = []
    counter = [0]
    sys.setrecursionlimit(100000)

    def strong(v: str) -> None:
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        onstack.add(v)
        for w in sorted(graph.get(v, ())):
            if w not in index:
                strong(w)
                low[v] = min(low[v], low[w])
            elif w in onstack:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                onstack.discard(w)
                comp.append(w)
                if w == v:
                    break
            if len(comp) > 1 or v in graph.get(v, ()):
                result.append(sorted(comp))

    nodes = set(graph)
    for vs in graph.values():
        nodes |= vs
    for v in sorted(nodes):
        if v not in index:
            strong(v)
    return sorted(result)
