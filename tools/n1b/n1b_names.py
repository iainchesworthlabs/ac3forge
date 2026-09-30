"""Namespace rewrite, stage 3a of the plan: the family root `ac3` becomes `iclforge`, in one pass.

    n1b_names.py --root <worktree> [--dry-run]

For every C/C++ file (and header template):
  namespace ac3 ...            -> namespace iclforge ...        (declarations and closing comments)
  ac3::x                       -> iclforge::x                   (every qualifier)
  using namespace ac3;         -> using namespace iclforge;
  ac4::, mp4::, mpegts::, matroska::, iamf::   -> iclforge::<same>::  (the libraries that were top-
  level) ac3iab::, ac3adm::           -> iclforge::iab::, iclforge::adm:: library aliases in
  comments  (ac3::forge_static ...) -> the new CMake alias names (n1b_cmake.ALIASES)
  adm::x                       -> ::adm::x                      (libadm's namespace, now that
                                  iclforge::adm exists: the only unqualified `adm::` is libadm's)

The sub-namespaces keep their names (iclforge::render, iclforge::meta, iclforge::oba ...), so every
unqualified lookup that worked from inside `namespace ac3` works from inside `namespace iclforge`.
Nesting the AC-3 codec's own symbols under iclforge::ac3 is the next stage (planning/layout.md): it
needs a scope-aware rewrite, and its size is counted there.
"""

from __future__ import annotations

import re
from pathlib import Path

from n1b_cmake import ALIASES
from n1b_lib import CPP_EXT, Repo, base_parser

_ALIAS = [(re.compile(r"(?<![\w])(?<!::)" + re.escape(a) + r"(?![\w:])"), b) for a, b in ALIASES]

RULES = [
    # libadm's own namespace is `adm`, and ac3adm becomes iclforge::adm: from inside iclforge an
    # unqualified `adm::` would find the project's. Every unqualified `adm::` in the tree was
    # libadm's (the project's was `ac3adm::`, which the `\w` before `adm` leaves alone), so it is
    # written from the global namespace. `<::adm::x>` is `<` `::`, which C++11 lexes as it must.
    (re.compile(r"(?<![\w:])adm::"), "::adm::"),
    # global-qualified names
    (re.compile(r"(?<![\w>)\]])::ac3iab::"), "::iclforge::iab::"),
    (re.compile(r"(?<![\w>)\]])::ac3adm::"), "::iclforge::adm::"),
    (re.compile(r"(?<![\w>)\]])::ac3::"), "::iclforge::"),
    (re.compile(r"(?<![\w>)\]])::(ac4|mp4|mpegts|matroska|iamf)::"), r"::iclforge::\1::"),
    # declarations (heads and closing comments)
    (re.compile(r"\bnamespace[ \t]+ac3(?![\w])"), "namespace iclforge"),
    (re.compile(r"\bnamespace[ \t]+ac3iab(?![\w])"), "namespace iclforge::iab"),
    (re.compile(r"\bnamespace[ \t]+ac3adm(?![\w])"), "namespace iclforge::adm"),
    (
        re.compile(r"\bnamespace[ \t]+(ac4|mp4|mpegts|matroska|iamf)(?![\w])(?![ \t]*=)"),
        r"namespace iclforge::\1",
    ),
    # using-directives
    (re.compile(r"\busing[ \t]+namespace[ \t]+ac3(?![\w])"), "using namespace iclforge"),
    (
        re.compile(r"\busing[ \t]+namespace[ \t]+(ac4|mp4|mpegts|matroska|iamf)(?![\w])"),
        r"using namespace iclforge::\1",
    ),
    # qualifiers
    (re.compile(r"(?<![\w])(?<!::)ac3iab::"), "iclforge::iab::"),
    (re.compile(r"(?<![\w])(?<!::)ac3adm::"), "iclforge::adm::"),
    (re.compile(r"(?<![\w])(?<!::)ac3::"), "iclforge::"),
    (re.compile(r"(?<![\w])(?<!::)(ac4|mp4|mpegts|matroska|iamf)::"), r"iclforge::\1::"),
]


def transform(text: str) -> str:
    for rx, b in _ALIAS:
        text = rx.sub(b, text)
    for rx, r in RULES:
        text = rx.sub(r, text)
    return text


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    changed = 0
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT and not f.endswith((".hpp.in", ".h.in")):
            continue
        p = root / f
        try:
            t = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        o = transform(t)
        if o != t:
            changed += 1
            if not a.dry_run:
                p.write_bytes(o.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files")


if __name__ == "__main__":
    main()
