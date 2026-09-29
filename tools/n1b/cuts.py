"""Stage S1 of planning/layout.md: cut the couplings that stop src/forge from splitting, in place.

    cuts.py [--root <worktree>] [--only C1,C3]

Each cut moves a declaration (no algorithm changes, no renames) into a header of its own, in the
namespace it will have, and leaves a using-declaration where the old spelling was used inside the
AC-3 code. The blocks are found by the code that opens them (`struct PcmBlock {`) and carried with
the comment lines directly above, never by a phrase from a comment, so the script keeps working
while the comments are reworded. It is idempotent (a cut whose new header exists is skipped) and
loud (a marker that is not found stops the run).

  C1  Location, Layout, kMaxChannels, name()   core/eac3_tables.hpp  -> core/layout.hpp
  (base) C2  DownmixTarget                            decoder/output.hpp    ->
  core/downmix_target.hpp   (base) C3  PcmBlock, BlockSink                      decoder/decoder.hpp
  -> render/pcm_block.hpp      (render) C4  joc::Domain, reconstruction_delay        oba/joc.hpp
  -> oba/joc_domain.hpp        (objects) C5  ObjectPlacement                          oba/atmos.hpp
  -> oba/placement.hpp         (objects) C6  blocks_per_syncframe                     iec61937.cpp
  -> a constexpr of its own    (iec61937) C7  serving.hpp                              render/
  -> decoder/                  (ac3)

The include lines it rewrites are put where the include block's sort order puts them, and the
lines a removal leaves blank are closed up, so the result needs no formatting pass. What it does
not touch: docs (header-map.md and the pages that name a header say where a declaration lives,
and a person words that), planning/ and CHANGELOG.md (records of the tree as it was).
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

from n1b_lib import base_parser

INC = "src/forge/include/ac3"
SRC = "src/forge/src"

# Text files C7 leaves alone: a record of the tree as it was, which the move does not change.
C7_KEEP = ("planning/", "CHANGELOG.md", "docs/", "README.md", "ROADMAP.md", "CONTRIBUTING.md")


def fail(message: str) -> None:
    sys.exit(f"cut failed: {message}")


class Source:
    """A file read as text, its line endings remembered so that it is written back as it was."""

    def __init__(self, root: Path, rel: str):
        self.path = root / rel
        raw = self.path.read_bytes().decode("utf-8")
        self.crlf = "\r\n" in raw
        self.text = raw.replace("\r\n", "\n")

    def save(self) -> None:
        out = self.text.replace("\n", "\r\n") if self.crlf else self.text
        self.path.write_bytes(out.encode("utf-8"))


def new_file(root: Path, rel: str, text: str, like: Source) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    out = text.replace("\n", "\r\n") if like.crlf else text
    path.write_bytes(out.encode("utf-8"))


# --- finding a declaration ------------------------------------------------------------------------


def line_start(text: str, index: int) -> int:
    return text.rfind("\n", 0, index) + 1


def comment_start(text: str, pos: int) -> int:
    """Move `pos`, the start of a line, up over the comment lines directly above it."""
    while pos > 0:
        prev = line_start(text, pos - 1)
        if not text[prev:pos].lstrip().startswith("//"):
            break
        pos = prev
    return pos


def close_of(text: str, open_brace: int) -> int:
    """The index just past the brace closing the one at `open_brace` (comments, strings skipped)."""
    depth = 0
    i = open_brace
    n = len(text)
    while i < n:
        two = text[i : i + 2]
        c = text[i]
        if two == "//":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if two == "/*":
            i = text.find("*/", i + 2) + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    fail(f"no closing brace for the one at offset {open_brace}")
    return -1


def find_once(text: str, head: str, what: str) -> int:
    i = text.find(head)
    if i < 0:
        fail(f"{what}: {head!r} not found")
    if text.find(head, i + 1) >= 0:
        fail(f"{what}: {head!r} is not unique")
    return i


def start_of(text: str, head: str, what: str) -> int:
    """Where the declaration that `head` opens begins: its own line and the comment lines above."""
    return comment_start(text, line_start(text, find_once(text, head, what)))


def end_of(text: str, head: str, what: str) -> int:
    """Just past the line that ends the declaration `head` opens (`};` for a type, `}` for code)."""
    i = find_once(text, head, what)
    end = close_of(text, text.index("{", i))
    rest = text[end:]
    if rest.lstrip(" ").startswith(";"):
        end += rest.index(";") + 1
    nl = text.find("\n", end)
    return len(text) if nl < 0 else nl + 1


def splice(text: str, start: int, end: int, replacement: str) -> str:
    """Replace text[start:end], closing up the blank line a bare removal would leave doubled."""
    before, after = text[:start], text[end:]
    if not replacement and before.endswith("\n\n") and after.startswith("\n"):
        after = after[1:]
    return before + replacement + after


# --- include lines --------------------------------------------------------------------------------


def include_line(spelling: str) -> str:
    return f'#include "{spelling}"'


def quoted_run(lines: list[str], index: int) -> tuple[int, int]:
    """The run of consecutive quoted #include lines around lines[index], as [first, last]."""
    first = last = index
    while first > 0 and lines[first - 1].startswith('#include "'):
        first -= 1
    while last + 1 < len(lines) and lines[last + 1].startswith('#include "'):
        last += 1
    return first, last


def sorted_position(lines: list[str], first: int, last: int, line: str) -> int:
    for i in range(first, last + 1):
        if lines[i] > line:
            return i
    return last + 1


def swap_include(src: Source, old: str, new: str) -> bool:
    """Replace `#include "old"` by `#include "new"`, sorted into the run the old line sat in."""
    lines = src.text.split("\n")
    old_line, new_line = include_line(old), include_line(new)
    if old_line not in lines:
        return False
    index = lines.index(old_line)
    first, last = quoted_run(lines, index)
    del lines[index]
    last -= 1
    if new_line not in lines:
        lines.insert(sorted_position(lines, first, last, new_line), new_line)
    src.text = "\n".join(lines)
    return True


def add_include(src: Source, spelling: str) -> None:
    """Add `#include "spelling"` to the run of quoted includes that holds the `ac3/` headers."""
    line = include_line(spelling)
    lines = src.text.split("\n")
    if line in lines:
        return
    anchor = next(
        (i for i, existing in enumerate(lines) if existing.startswith('#include "ac3/')), None
    )
    if anchor is None:
        fail(f'no #include "ac3/..." line to put {spelling} beside')
    first, last = quoted_run(lines, anchor)
    lines.insert(sorted_position(lines, first, last, line), line)
    src.text = "\n".join(lines)


# --- the new headers ------------------------------------------------------------------------------


def header(
    includes_system: list[str],
    includes_project: list[str],
    namespace: str,
    preamble: str,
    body: str,
) -> str:
    out = ["#pragma once", ""]
    if includes_system:
        out += [f"#include <{h}>" for h in includes_system] + [""]
    if includes_project:
        out += [include_line(h) for h in includes_project] + [""]
    if preamble:
        out += [preamble.rstrip("\n"), ""]
    out += [
        f"namespace {namespace} {{",
        "",
        body.rstrip("\n"),
        "",
        f"}}  // namespace {namespace}",
        "",
    ]
    return "\n".join(out)


def rewrite(
    root: Path,
    rel: str,
    *,
    swaps: tuple[tuple[str, str], ...] = (),
    subs: tuple[tuple[str, str], ...] = (),
) -> None:
    """Swap includes and apply regex substitutions in one file; an unchanged file is left alone."""
    src = Source(root, rel)
    before = src.text
    for old, new in swaps:
        swap_include(src, old, new)
    for pattern, replacement in subs:
        src.text = re.sub(pattern, replacement, src.text)
    if src.text != before:
        src.save()


# --- the cuts -------------------------------------------------------------------------------------


def cut_c1(root: Path) -> None:
    if (root / f"{INC}/core/layout.hpp").exists():
        return
    rel = f"{INC}/core/eac3_tables.hpp"
    src = Source(root, rel)
    start = start_of(src.text, "enum class Location : std::uint8_t {", "C1 Location")
    end = end_of(src.text, "struct Layout {", "C1 Layout")
    block = src.text[start:end]
    intro = (
        "// The speaker vocabulary: where a channel sits, named the way TS 103 420 and A/52\n"
        "// Annex E name them, and an ordered set of them. It lives here, not in eac3_tables.hpp,\n"
        "// because the renderer, the audio backends and the spatial panner use it for streams of\n"
        "// any codec."
    )
    new_file(
        root,
        f"{INC}/core/layout.hpp",
        header(
            ["array", "cstddef", "cstdint", "iterator", "string_view"],
            [],
            "ac3::base",
            intro,
            block,
        ),
        src,
    )
    using = (
        "// The vocabulary moved to ac3/core/layout.hpp; the Annex E code keeps its old spelling.\n"
        "using ac3::base::kMaxChannels;\nusing ac3::base::Layout;\nusing ac3::base::Location;\n"
        "using ac3::base::name;\n"
    )
    src.text = splice(src.text, start, end, using)
    add_include(src, "ac3/core/layout.hpp")
    src.save()
    # what becomes codec-blind code spells the new home
    qualified = (
        (r"\bac3::eac3::chanmap::(Location|Layout|kMaxChannels|name)\b", r"ac3::base::\1"),
        (r"\beac3::chanmap::(Location|Layout|kMaxChannels|name)\b", r"base::\1"),
    )
    swap = (("ac3/core/eac3_tables.hpp", "ac3/core/layout.hpp"),)
    for f in (
        f"{INC}/render/layout.hpp",
        f"{INC}/render/render.hpp",
        f"{INC}/spatial/spatial.hpp",
        f"{SRC}/spatial/spatial.cpp",
        "src/audio/include/ac3/audio/speakers.hpp",
    ):
        rewrite(root, f, swaps=swap, subs=qualified)
    # speakers.hpp's comment names the alias speakers.cpp had for the old namespace; the alias goes
    # with it
    rewrite(
        root,
        "src/audio/include/ac3/audio/speakers.hpp",
        subs=((r"\bchanmap::name\(\)", "base::name()"),),
    )
    rewrite(
        root,
        "src/audio/src/speakers.cpp",
        swaps=swap,
        subs=(
            (r"\n[ \t]*namespace chanmap = ac3::eac3::chanmap;\n[ \t]*\n", "\n"),
            (r"\bchanmap::name\(", "ac3::base::name("),
        ),
    )


def cut_c2(root: Path) -> None:
    if (root / f"{INC}/core/downmix_target.hpp").exists():
        return
    rel = f"{INC}/decoder/output.hpp"
    src = Source(root, rel)
    start = start_of(src.text, "enum class DownmixTarget : std::uint8_t {", "C2 DownmixTarget")
    end = end_of(src.text, "enum class DownmixTarget : std::uint8_t {", "C2 DownmixTarget")
    block = src.text[start:end]
    new_file(
        root, f"{INC}/core/downmix_target.hpp", header(["cstdint"], [], "ac3::base", "", block), src
    )
    src.text = splice(
        src.text, start, end, "using base::DownmixTarget;  // ac3/core/downmix_target.hpp\n"
    )
    add_include(src, "ac3/core/downmix_target.hpp")
    src.save()
    rewrite(
        root,
        f"{INC}/render/layout.hpp",
        swaps=(("ac3/decoder/output.hpp", "ac3/core/downmix_target.hpp"),),
        subs=((r"\bac3::DownmixTarget\b", "ac3::base::DownmixTarget"),),
    )


def cut_c3(root: Path) -> None:
    if (root / f"{INC}/render/pcm_block.hpp").exists():
        return
    rel = f"{INC}/decoder/decoder.hpp"
    src = Source(root, rel)
    start = start_of(src.text, "struct PcmBlock {", "C3 PcmBlock")
    end = end_of(src.text, "class BlockSink {", "C3 BlockSink")
    block = src.text[start:end]
    new_file(
        root,
        f"{INC}/render/pcm_block.hpp",
        header(
            ["concepts", "memory", "span", "type_traits"],
            ["ac3/oba/oamd.hpp"],
            "ac3::render",
            "",
            block,
        ),
        src,
    )
    using = "using render::BlockSink;  // ac3/render/pcm_block.hpp\nusing render::PcmBlock;\n"
    src.text = splice(src.text, start, end, using)
    add_include(src, "ac3/render/pcm_block.hpp")
    src.save()
    rewrite(
        root,
        f"{INC}/render/render.hpp",
        swaps=(("ac3/decoder/decoder.hpp", "ac3/render/pcm_block.hpp"),),
        subs=((r"\bac3::PcmBlock\b", "ac3::render::PcmBlock"),),
    )


def cut_c4(root: Path) -> None:
    if (root / f"{INC}/oba/joc_domain.hpp").exists():
        return
    rel = f"{INC}/oba/joc.hpp"
    src = Source(root, rel)
    start = start_of(src.text, "enum class Domain : std::uint8_t {", "C4 Domain")
    end = end_of(
        src.text, "constexpr int reconstruction_delay(Domain domain) {", "C4 reconstruction_delay"
    )
    block = src.text[start:end]
    new_file(
        root,
        f"{INC}/oba/joc_domain.hpp",
        header(["cstdint"], ["ac3/dsp/qmf.hpp"], "ac3::oba::joc", "", block),
        src,
    )
    src.text = splice(
        src.text,
        start,
        end,
        "// Domain and reconstruction_delay() live in ac3/oba/joc_domain.hpp.\n",
    )
    add_include(src, "ac3/oba/joc_domain.hpp")
    src.save()
    rewrite(
        root, f"{INC}/render/render.hpp", swaps=(("ac3/oba/joc.hpp", "ac3/oba/joc_domain.hpp"),)
    )


def cut_c5(root: Path) -> None:
    if (root / f"{INC}/oba/placement.hpp").exists():
        return
    rel = f"{INC}/oba/atmos.hpp"
    src = Source(root, rel)
    start = start_of(src.text, "struct ObjectPlacement {", "C5 ObjectPlacement")
    end = end_of(src.text, "struct ObjectPlacement {", "C5 ObjectPlacement")
    block = src.text[start:end]
    new_file(
        root,
        f"{INC}/oba/placement.hpp",
        header([], ["ac3/oba/oamd.hpp"], "ac3::oba", "", block),
        src,
    )
    src.text = splice(src.text, start, end, "")
    add_include(src, "ac3/oba/placement.hpp")
    src.save()
    for f in (
        f"{INC}/oba/scene.hpp",
        f"{INC}/oba/motion.hpp",
        f"{INC}/oba/scene_osc.hpp",
        f"{SRC}/oba/scene.cpp",
        f"{SRC}/oba/motion.cpp",
        f"{SRC}/oba/scene_osc.cpp",
    ):
        rewrite(root, f, swaps=(("ac3/oba/atmos.hpp", "ac3/oba/placement.hpp"),))


def cut_c6(root: Path) -> None:
    rel = f"{SRC}/iec61937/iec61937.cpp"
    src = Source(root, rel)
    if (
        "eac3::blocks_per_syncframe(" not in src.text
    ):  # the helper's comment names it without a call
        return
    src.text = src.text.replace("eac3::blocks_per_syncframe(", "blocks_per_syncframe(")
    # The codec's table is the one include this file takes from outside its library; drop it with
    # the blank line that separated it from the standard headers.
    src.text = src.text.replace('\n#include "ac3/core/eac3_tables.hpp"\n', "\n", 1)
    src.text = src.text.replace(
        "\n\n\nnamespace ac3::iec61937 {", "\n\nnamespace ac3::iec61937 {", 1
    )
    helper = (
        "// E-AC-3's audio blocks per syncframe by numblkscod (A/52 Table E2.4), which the burst\n"
        "// length follows from. A copy of eac3::blocks_per_syncframe: this library includes\n"
        "// none of the codec's tables.\n"
        "[[nodiscard]] constexpr int blocks_per_syncframe(int numblkscod) {\n"
        "    constexpr std::array<int, 4> counts = {1, 2, 3, 6};\n"
        "    return counts[static_cast<std::size_t>(numblkscod & 0x3)];\n"
        "}\n\n"
    )
    match = re.search(r"\nnamespace \{\n\n", src.text)
    if not match:
        fail("C6 needs an anonymous namespace in iec61937.cpp")
    src.text = src.text[: match.end()] + helper + src.text[match.end() :]
    if "#include <array>" not in src.text:
        lines = src.text.split("\n")
        first = next(i for i, line in enumerate(lines) if line.startswith("#include <"))
        last = first
        while last + 1 < len(lines) and lines[last + 1].startswith("#include <"):
            last += 1
        lines.insert(sorted_position(lines, first, last, "#include <array>"), "#include <array>")
        src.text = "\n".join(lines)
    src.save()


def git(root: Path, *args: str, check: bool = True) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=check
    ).stdout


def cut_c7(root: Path) -> None:
    old = f"{INC}/render/serving.hpp"
    new = f"{INC}/decoder/serving.hpp"
    if not (root / old).exists():
        return
    git(root, "mv", old, new)
    # It is no longer "beside the renderer": say where it went and why.
    rewrite(
        root,
        new,
        subs=(
            (
                r"ESP32 player's policy, moved beside the renderer so that every player that\n"
                r"// renders",
                "ESP32 player's policy, moved out of the player so that every player that\n"
                "// renders",
            ),
        ),
    )
    for f in git(root, "grep", "-l", "ac3/render/serving.hpp", check=False).split("\n"):
        if not f or f.startswith(C7_KEEP):
            continue
        src = Source(root, f)
        swap_include(src, "ac3/render/serving.hpp", "ac3/decoder/serving.hpp")
        src.text = src.text.replace("ac3/render/serving.hpp", "ac3/decoder/serving.hpp")
        src.save()


CUTS = {
    "C1": cut_c1,
    "C2": cut_c2,
    "C3": cut_c3,
    "C4": cut_c4,
    "C5": cut_c5,
    "C6": cut_c6,
    "C7": cut_c7,
}


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument(
        "--only", default=",".join(CUTS), help="comma-separated cuts to apply (default: all)"
    )
    a = ap.parse_args()
    root = Path(a.root)
    for name in a.only.split(","):
        if name not in CUTS:
            sys.exit(f"unknown cut {name!r}; the cuts are {', '.join(CUTS)}")
        CUTS[name](root)
        print("applied", name)


if __name__ == "__main__":
    main()
