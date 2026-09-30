"""The wire extension's namespace, stage 4 of the plan: `sendspin::ac3forge` to `sendspin::player`.

    n1b_sendspin.py --root <worktree> [--dry-run]

The extension role `_ac3forge_player@v1` of Sendspin has its C++ in
`iclforge::sendspin::ac3forge` (src/sendspin/include/iclforge/sendspin/ac3forge_player.hpp). The
identifier pass (n1b_idents.py) turns every `ac3forge` into `iclforge`, and a namespace
`iclforge::sendspin::iclforge` would be found by the unqualified name `iclforge` from inside
`iclforge::sendspin`, before the family's root: every `iclforge::render::...` written there would
look in the wrong place. So this namespace is renamed first, and to a word of its own, the one the
role's name ends in (`player@v1`).

For every C and C++ file:
  sendspin::ac3forge, ss::ac3forge  -> sendspin::player, ss::player
      every qualification, the declarations and their closing comments, and the right side of a
      namespace alias: `namespace ac = ss::ac3forge;`
  ac3forge::x, unqualified          -> player::x
      only where the enclosing scope is iclforge::sendspin: src/sendspin, and the comments of the
      Hearth controller that quote it; elsewhere an unqualified `ac3forge::` is the ESP-IDF
      component's own namespace

`ss` is the alias every user of the namespace gives `iclforge::sendspin`; a file that writes
`ss::ac3forge` without that alias is listed. The names that merely start with the old word
(`ac3forge_player.hpp`, `ac3forge_support`, `.ac3forge = ...`) are the identifier pass's.
"""

from __future__ import annotations

import re
from pathlib import Path

from n1b_lib import CPP_EXT, Repo, base_parser

# Where an unqualified `ac3forge::` names the sendspin namespace.
UNQUALIFIED_IN = ("src/sendspin/", "apps/hearth/ui/network_controller.hpp")

QUALIFIED = re.compile(r"(?<![\w])((?:sendspin|ss)::)ac3forge(?![\w])")
UNQUALIFIED = re.compile(r"(?<![\w:])ac3forge::")
SS_ALIAS = re.compile(r"namespace\s+ss\s*=\s*(?:::)?(?:iclforge::)?sendspin\s*;")
NAMESPACE_ALIAS_TO_OLD = re.compile(r"namespace\s+\w+\s*=\s*[\w:]*ac3forge\s*;")


def transform(path: str, text: str) -> str:
    text = QUALIFIED.sub(r"\1player", text)
    if path.startswith(UNQUALIFIED_IN):
        text = UNQUALIFIED.sub("player::", text)
    return text


def problems(path: str, before: str, after: str) -> list[str]:
    """What a person has to look at in a file the pass changed."""
    found = []
    if re.search(r"(?<![\w:])ss::ac3forge", before) and not SS_ALIAS.search(before):
        found.append(f"{path}: writes ss::ac3forge without a `namespace ss = ...sendspin;`")
    for m in NAMESPACE_ALIAS_TO_OLD.finditer(after):
        found.append(f"{path}: an alias still names the old namespace: {m.group(0)}")
    return found


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    files = lines = 0
    listed: list[str] = []
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT:
            continue
        try:
            before = (root / f).read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        after = transform(f, before)
        if after == before:
            continue
        files += 1
        old, new = before.split("\n"), after.split("\n")
        lines += sum(1 for x, y in zip(old, new, strict=True) if x != y)
        listed += problems(f, before, after)
        if not a.dry_run:
            (root / f).write_bytes(after.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {files} files, {lines} lines")
    for p in listed:
        print("  PROBLEM", p)


if __name__ == "__main__":
    main()
