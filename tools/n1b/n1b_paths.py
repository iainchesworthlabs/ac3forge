"""Paths in text: pages, plans, comments, strings and scripts that name a file by its path.

    n1b_paths.py --root <worktree> --plan <plan.json> [--dry-run]

n1b_cmake.py follows the moves in the build files and renames the targets there. Everything else
that names a moved file by its path in the repository is done here, after the files have moved: the
documentation pages and plans, the comments and strings of the C and C++ sources (a test that reads
a source file by path, a comment that says where a function lives), the workflows, the scripts and
the data files. The rule is n1b_cmake.rewrite_paths: a whole old path, or a directory that the moves
kept together, becomes its new path, and nothing that merely ends the same way. A Python path built
from its components (`REPO / "src" / "forge" / "src" / "dsp"`) is joined, moved by the same rule and
split again. The pages also follow the files that the hand-written part of S2 renames and no plan
lists (HAND_RENAMES: the ABI allowlists).

It leaves alone the files that quote the old layout as history: the changelog, the layout study and
its inventory, the scripts and baselines of the migration (KEEP_OLD_PATHS), and the byte-exact
trees (the golden files, the fuzz seeds). Binary files and files that are not UTF-8 are skipped.

A directory whose files went to more than one library is rewritten to where most of them went when
that is at least 60%, and is listed, with the files that name it, when it is not: those are for a
person, as in n1b_cmake.py.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from n1b_cmake import (
    _PATH_END,
    _PATH_START,
    dir_rules,
    held_dirs,
    path_index,
    rewrite_paths,
)
from n1b_lib import Repo, base_parser

KEEP_OLD_PATHS = (
    "CHANGELOG.md",
    "planning/layout.md",
    "planning/layout-inventory.md",
    "tools/n1b/",
    "tests/golden/",
    "fuzz/seeds/",
    "fuzz/regressions/",
)
# Files the hand-written part of S2 renames (tools/n1b/s2-hand.patch) and no move plan lists: the
# exported-symbol allowlists take the name of the library that replaces the one they list, and the
# pages that name one follow the rename as they follow a move.
_ALLOWLISTS = "tools/ci/abi-allowlist/"
HAND_RENAMES = {
    f"{_ALLOWLISTS}{old}.so.txt": f"{_ALLOWLISTS}{new}.so.txt"
    for old, new in {
        "libac3forge": "libiclforge_ac3",
        "libac3forge_c": "libiclforge_c",
        "libac3iab": "libiclforge_iab",
        "libac3signing": "libiclforge_signing",
        "libac4": "libiclforge_ac4",
        "libac4dec": "libiclforge_ac4dec",
        "libac4enc": "libiclforge_ac4enc",
        "libiamf": "libiclforge_iamf",
        "libmatroska": "libiclforge_matroska",
        "libmp4": "libiclforge_mp4",
        "libmpegts": "libiclforge_mpegts",
    }.items()
}
BINARY_EXT = {
    ".png",
    ".ttf",
    ".bin",
    ".wav",
    ".ec3",
    ".ac3",
    ".ac4",
    ".wasm",
    ".jpg",
    ".ico",
    ".icns",
    ".gif",
    ".woff",
    ".woff2",
    ".pdf",
    ".zip",
    ".gz",
    ".qm",
    ".mkv",
    ".mp4",
    ".m4a",
    ".mka",
    ".flac",
    ".jar",
    ".keystore",
}


def is_text(data: bytes) -> bool:
    return b"\0" not in data[:8192]


# A path built the way pathlib builds it, `REPO / "src" / "forge" / "src" / "dsp" / "qmf.hpp"`: a
# generator or a check names a file so, and no repository path stands in the text for the pass above
# to see. It starts at a component named src or tests, the trees the moves are in.
_CHAIN_RX = re.compile(
    r"""(?P<chain>(?P<q>["'])(?:src|tests)(?P=q)(?:\s*/\s*(?P=q)[\w.\-]+(?P=q))+)"""
)


def chain_parts(chain: str, quote: str) -> list[str]:
    return re.findall(re.escape(quote) + r"([\w.\-]+)" + re.escape(quote), chain)


def rewrite_chains(
    text: str,
    moves: dict[str, str],
    dirs: list[tuple[str, str]] | None,
    hold: list[str] | None = None,
    known: set[str] | None = None,
) -> str:
    """The chains of `text` that name a moved file or directory, by the rule of rewrite_paths."""

    def follow(m: re.Match) -> str:
        chain, quote = m.group("chain"), m.group("q")
        path = "/".join(chain_parts(chain, quote))
        new = rewrite_paths(path, moves, dirs, hold, known)
        if new == path:
            return chain
        sep = re.search(re.escape(quote) + r"(\s*/\s*)" + re.escape(quote), chain)
        assert sep is not None
        return sep.group(1).join(f"{quote}{part}{quote}" for part in new.split("/"))

    return _CHAIN_RX.sub(follow, text)


def run(root: Path, plan: Path, dry_run: bool = False) -> tuple[int, dict[str, list[str]]]:
    """Rewrite the paths in every text file; return the files changed and the split directories
    that are still named (old directory -> the files that name it)."""
    repo = Repo(str(root))
    moves = json.loads(plan.read_text(encoding="utf-8"))["moves"]
    dirs, split = dir_rules(moves)
    hold = held_dirs(dirs, split)
    known = path_index(repo.files)
    changed = 0
    hits: dict[str, list[str]] = {}
    for f in repo.files:
        if f.startswith(KEEP_OLD_PATHS) or repo.ext(f) in BINARY_EXT:
            continue
        p = root / f
        try:
            data = p.read_bytes()
            if not is_text(data):
                continue
            text = data.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out = rewrite_paths(text, moves, dirs, hold, known)
        out = rewrite_paths(out, HAND_RENAMES, None)
        named = out
        if f.endswith(".py"):
            out = rewrite_chains(out, moves, dirs, hold, known)
            named = out + "\n" + "\n".join(
                "/".join(chain_parts(m.group("chain"), m.group("q")))
                for m in _CHAIN_RX.finditer(out)
            )
        for old_dir in split:
            if old_dir in named and re.search(_PATH_START + re.escape(old_dir) + _PATH_END, named):
                hits.setdefault(old_dir, []).append(f)
        if out != text:
            changed += 1
            if not dry_run:
                p.write_bytes(out.encode("utf-8"))
    print(f"{'would change' if dry_run else 'changed'} {changed} files")
    if hits:
        print("directories split between libraries and still named (review each):")
        applied = {r[0] for r in dirs}
        for old_dir, files in sorted(hits.items()):
            s = split[old_dir]
            state = "rewritten to" if old_dir in applied else "left as is; candidate"
            print(
                f"  {old_dir}: {state} {s['to']} ({s['elsewhere']} of {s['files']} files went "
                f"elsewhere): {len(files)} files, {', '.join(files[:4])}"
            )
    return changed, hits


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--plan", required=True, help="the plan n1b_apply.py --json wrote before it moved the files"
    )
    a = ap.parse_args()
    run(Path(a.root), Path(a.plan), a.dry_run)


if __name__ == "__main__":
    main()
