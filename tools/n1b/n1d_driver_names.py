"""The Windows null-sink driver and its endpoint take their own names, change N1D of the re-layout.

    n1d_driver_names.py --root <worktree> --phase mv|text|check [--dry-run]

Three phases, each a commit's worth. `mv` is the `git mv` of every tracked file whose name carries
the driver's old identity (the solution, the INF source and the version resource): the renames are
staged and nothing else is, so that a `git commit` with nothing added is the commit of the renames
alone (all R100). `text` rewrites what the files say: the identity in every case form, the .NET
namespace the scripts compile for themselves, the INF's provider and manufacturer strings, and the
endpoint's name wherever it names the device. `check` reads and writes nothing: it lists what still
carries an old name, by family, which is the list the pull request gives and the proof that the
rewrite left nothing it should have taken.

The one place the names are is NAMES below. To change one (the endpoint is the likely one), set the
row's `old` to what the tree says now and its `new` to what it should say, and run the phases. A
second run of the same table changes nothing.

What the pass leaves alone, and why:

  * the pages (`docs/`, `planning/`, `CHANGELOG.md`, `README.md`, `ROADMAP.md`) are the docs stage's
    and the history's: `check` lists the pages that name the driver;
  * this migration's own tools (`tools/n1b/`), where the old names are the data the passes read;
  * `tests/golden/` and the released winget manifests, which are byte-exact records;
  * `DesktopAtmos`, with no space, which is the name the demo's settings were stored under and which
    the settings migration reads: it is stored data, not a device name;
  * a line that names the demo ("the Desktop Atmos Demo") or the old name of the driver test guest:
    those are sentences about something else, and are rewritten by hand in the commit after
    this one.

Read and write are by bytes, so a file keeps its line endings and its byte-order mark. The INF
source is UTF-16 with a byte-order mark, which `stampinf` preserves and `inf2cat` insists on; it is
decoded and encoded as that. A file that is neither UTF-16 nor UTF-8, or that does not give its own
bytes back from its decoded text, is reported and not written.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# --- the table ------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Name:
    what: str  # where the name is read, for the reader of this file
    old: str
    new: str


IDENTITY: tuple[Name, ...] = (
    Name(
        "the driver's identity: hardware id ROOT\\<name>, the service, the SYS, INF, SLN, RC and "
        "INX names, the version resource, the scripts that build, install, remove and verify it",
        "Ac3ForgeNullSink",
        "IclForgeNullSink",
    ),
    Name(
        "the same, lower case: the catalogue file name the CI job checks",
        "ac3forgenullsink",
        "iclforgenullsink",
    ),
    Name("the same, upper case: the INF's model section", "AC3FORGENULLSINK", "ICLFORGENULLSINK"),
    Name(
        "the CI artifact, as the driver's README spells it",
        "ac3forge-nullsink",
        "iclforge-nullsink",
    ),
    Name(
        "the MSBuild property that names the WDK NuGet version",
        "Ac3ForgeWdkNuGetVersion",
        "IclForgeWdkNuGetVersion",
    ),
)

# The endpoint and device description Windows shows ("Speakers (<name>)"), the string Crucible
# matches it by and the guest scripts look for. Crucible's own constant for it is
# kWindowsSilentDeviceName in apps/crucible/engine/virtual_device.hpp, which is set by hand.
ENDPOINT = Name("the endpoint's name", "Desktop Atmos", "Crucible Silent Output")

# The .NET namespace the scripts compile for themselves with Add-Type. Only these two files: the
# word means the project in every other place and is the brand pass's, not this one's.
NAMESPACE = Name("the .NET namespace of the driver's scripts", "Ac3Forge", "IclForge")
NAMESPACE_FILES = (
    "apps/windows/driver/NullSinkDevice.ps1",
    "apps/windows/driver-vm/Build-Iso.ps1",
)

# The INF's provider and manufacturer: what Windows lists as the driver's publisher.
PROVIDER_OLD = "ac3forge"
PROVIDER_NEW = "ICL Forge"
PROVIDER_LINE = re.compile(
    r'^(?P<head>(?:ProviderName|MfgName)\s*=\s*)"' + PROVIDER_OLD + '"', re.M
)

# A line that names something else by the endpoint's old words. It is left for the hand-written
# commit, and `check` still lists it until it is.
EXEMPT_LINE = (
    re.compile(r"Desktop Atmos Demo"),
    re.compile(r"Desktop Atmos (?:driver )?test (?:guest|VM)"),
    re.compile(r"Desktop Atmos null-sink audio driver"),
    # A name that ends a line is a phrase that wraps ("the Desktop Atmos" / "Demo plan"): the next
    # line says what it names, and a device name is never the last words of a line of code.
    re.compile(r"Desktop Atmos\s*$"),
)

# What the pass does not read.
PAGE_PREFIXES = ("docs/", "planning/")
PAGE_FILES = ("CHANGELOG.md", "README.md", "ROADMAP.md")
TOOL_PREFIXES = ("tools/n1b/",)
RECORD_PREFIXES = ("tests/golden/", "packaging/winget/manifests/")
EXCLUDED_PREFIXES = PAGE_PREFIXES + TOOL_PREFIXES + RECORD_PREFIXES
EXCLUDED_FILES = (*PAGE_FILES, ".git-blame-ignore-revs")

# The families `check` sorts what is left into.
FAMILIES = (
    ("the pages and the history", PAGE_PREFIXES, PAGE_FILES),
    ("the N1 migration tools, whose data these are", TOOL_PREFIXES, ()),
    ("byte-exact records", RECORD_PREFIXES, ()),
)
IDENTITY_FORMS = re.compile("|".join(re.escape(n.old) for n in IDENTITY), re.I)
ENDPOINT_FORM = re.compile(re.escape(ENDPOINT.old), re.I)
STORED_DATA_FORM = re.compile(r"DesktopAtmos")


def excluded(path: str) -> bool:
    return path in EXCLUDED_FILES or path.startswith(EXCLUDED_PREFIXES)


def moved_path(path: str) -> str | None:
    """The new path of a file named for the old identity, or None where it keeps its name."""
    head, _, name = path.rpartition("/")
    renamed = name
    for form in IDENTITY:
        renamed = renamed.replace(form.old, form.new)
    if renamed == name:
        return None
    return f"{head}/{renamed}" if head else renamed


# --- reading and writing a file by bytes --------------------------------------------------------


def decode(data: bytes) -> tuple[str, str] | None:
    """(text, encoding) of a file, or None for one that is binary or in neither encoding."""
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        try:
            return data.decode("utf-16"), "utf-16"
        except UnicodeDecodeError:
            return None
    if b"\x00" in data:
        return None
    try:
        return data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        return None


def encode(text: str, encoding: str) -> bytes:
    """The bytes `decode` read the text from: UTF-16 with its little-endian mark, or UTF-8 (a mark
    that was there is a character at the start of the text and goes back as itself)."""
    if encoding == "utf-16":
        return b"\xff\xfe" + text.encode("utf-16-le")
    return text.encode("utf-8")


# --- the rewrite ----------------------------------------------------------------------------------


def rewrite(path: str, text: str) -> tuple[str, list[str]]:
    """The text after the pass, and the lines it left for the hand-written commit."""
    for name in IDENTITY:
        text = text.replace(name.old, name.new)
    if path in NAMESPACE_FILES:
        text = re.sub(r"\b" + NAMESPACE.old + r"\b", NAMESPACE.new, text)
    if path.endswith(".inx"):
        text = PROVIDER_LINE.sub(lambda m: m.group("head") + f'"{PROVIDER_NEW}"', text)
    left: list[str] = []
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if ENDPOINT.old not in line:
            continue
        if any(rule.search(line) for rule in EXEMPT_LINE):
            left.append(f"{path}:{i + 1}: {line.strip()}")
            continue
        lines[i] = line.replace(ENDPOINT.old, ENDPOINT.new)
    return "".join(lines), left


# --- the repository -----------------------------------------------------------------------------


def tracked(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], check=True, capture_output=True
    ).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def phase_mv(root: Path, dry: bool) -> int:
    moves = [(p, m) for p in tracked(root) if (m := moved_path(p)) is not None]
    for old, new in moves:
        print(f"mv {old} -> {new}")
        if not dry:
            subprocess.run(["git", "-C", str(root), "mv", old, new], check=True)
    print(f"{len(moves)} file(s) renamed")
    return 0


def phase_text(root: Path, dry: bool) -> int:
    changed = 0
    left: list[str] = []
    unreadable: list[str] = []
    for path in tracked(root):
        if excluded(path):
            continue
        target = root / path
        if not target.is_file():
            continue
        data = target.read_bytes()
        read = decode(data)
        if read is None or encode(read[0], read[1]) != data:
            if IDENTITY_FORMS.search(data.decode("latin-1")) or ENDPOINT_FORM.search(
                data.decode("latin-1")
            ):
                unreadable.append(path)
            continue
        text, encoding = read
        new_text, kept = rewrite(path, text)
        left.extend(kept)
        if new_text == text:
            continue
        changed += 1
        print(f"text {path}")
        if not dry:
            target.write_bytes(encode(new_text, encoding))
    print(f"{changed} file(s) rewritten")
    if left:
        print(f"{len(left)} line(s) left for the hand-written commit:")
        print("\n".join(f"  {line}" for line in left))
    if unreadable:
        print("not read (neither UTF-8 nor UTF-16, or not reproduced byte for byte):")
        print("\n".join(f"  {p}" for p in unreadable))
        return 1
    return 0


def family(path: str) -> str:
    for name, prefixes, files in FAMILIES:
        if path.startswith(prefixes) or path in files:
            return name
    return "everything else"


def phase_check(root: Path) -> int:
    found: dict[str, list[tuple[str, int, int, int]]] = {}
    for path in tracked(root):
        target = root / path
        if not target.is_file():
            continue
        data = target.read_bytes()
        read = decode(data)
        text = read[0] if read else data.decode("latin-1")
        counts = (
            len(IDENTITY_FORMS.findall(text)),
            len(ENDPOINT_FORM.findall(text)),
            len(STORED_DATA_FORM.findall(text)),
        )
        if any(counts):
            found.setdefault(family(path), []).append((path, *counts))
    grand = [0, 0, 0]
    for name in [f[0] for f in FAMILIES] + ["everything else"]:
        rows = found.get(name, [])
        if not rows:
            continue
        sums = [sum(r[i] for r in rows) for i in (1, 2, 3)]
        print(
            f"{name}: {len(rows)} file(s); identity {sums[0]}, endpoint {sums[1]}, "
            f"stored-data name {sums[2]}"
        )
        for path, a, b, c in sorted(rows):
            print(f"  identity {a:3d}  endpoint {b:3d}  stored {c:3d}  {path}")
        grand = [grand[i] + sums[i] for i in range(3)]
    print(f"in all: identity {grand[0]}, endpoint {grand[1]}, stored-data name {grand[2]}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--phase", choices=("mv", "text", "check"), required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.phase == "mv":
        return phase_mv(root, args.dry_run)
    if args.phase == "text":
        return phase_text(root, args.dry_run)
    return phase_check(root)


if __name__ == "__main__":
    sys.exit(main())
