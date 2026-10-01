"""Does the move change what any unit means? The LLVM IR of the old tree against the new one's.

The builds and the tests say that the programs behave the same; this says it of the code. S6 moves
names, and a name that is still found but is another declaration builds and means something else
(ac3ns_shadows.py: `kSyncWord` in `iclforge::ac3::emdf` found the AC-3 sync word for the
container's). Compiled without optimisation and without debug information, a unit's IR is its
meaning written down, so two trees that differ only in the namespace of their names give the same
IR once the names are read alike. What is left of a difference is another constant, another callee,
another overload, or a string that prints a qualified name.

    ir_compare.py compile <build dir> <out dir> [--jobs N] [--tests] [--only <substring>]
                  [--asserts]
    ir_compare.py compare <old dir> <new dir> --old-root <src dir> --new-root <src dir>
                  [--diffs <dir>] [--jobs N]

`compile` takes the compile_commands.json of a tree configured with a Clang preset (a source archive
of the parent and one of the new commit, each configured and not built: the IR of a unit needs only
the headers that configure writes) and writes one .ll file per unit whose source is in the tree,
with the command CMake gave it changed to `-O0 -g0 -Wno-error -S -emit-llvm`. Units the build
generates (moc, the QML cache) are left out, and so are the tests unless --tests says otherwise:
Catch2 writes `__LINE__` into every assertion, which a reflow moves.

`compile --asserts` compiles with `-UNDEBUG`, so that what is inside `assert(...)` is compiled too.
A Release build compiles none of it (the loop of S6 saw `assert(domain != Domain::kQmf ...)` of
joc.cpp only when a Debug build of another job failed on it), so the build of a unit that a move
touches is checked this way as well; the IR of such a build is not compared, since an assertion
carries its line.

`compare` pairs the files of the two directories and says, per unit, whether they are the same IR or
not; a unified diff of each that differs goes to --diffs. The names are read alike this way:

  - every mangled name is demangled (llvm-cxxfilt) and the tree's path is replaced by `<T>`;
  - in the new IR `iclforge::ac3::` reads as `iclforge::`, in a name and in the text of a string
    (whose length in its definition follows: a string that prints a qualified name is one of those
    `n1b_ac3ns.py --report` lists);
  - the line a Catch2 macro passes, the length of the text it stringifies and the length of a string
    literal in the name of a template made over it are not compared, which is what lets --tests work
    on units a reflow moved and whose messages name a qualified name;
  - the typeinfo name of a type, which is a string of mangled text, is not compared;
  - the build directory is `<B>`, as the source directory is `<T>`;
  - a mangled name llvm-cxxfilt cannot read (a requires-clause, a local lambda) is compared without
    the component `3ac3` and with the substitution indices it shifts taken out of both sides.

On the S6 tree the comparison sees the bug the tests found: put `kSyncWord` back in
frame_layout.cpp and its unit differs by one constant, `icmp eq i32 %x, 22584` against `2935`.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import difflib
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path

CXXFILT = "llvm-cxxfilt-22"
NEW_NAMESPACE = "iclforge::ac3::"
OLD_NAMESPACE = "iclforge::"

MANGLED = re.compile(r"_Z[A-Za-z0-9_$.]+")
SUBSTITUTION = re.compile(r"S[0-9A-Z]*_")
STRING_DEF = re.compile(
    r'^(?P<head>@[^=\n]*= [^\n]*?constant \[)(?P<n>\d+)(?P<mid> x i8\] c")(?P<body>.*)'
    r'(?P<tail>"[^"\n]*)$'
)
TYPEINFO_NAME = re.compile(r'\[\d+ x i8\] c"[^"\n]*"')
TYPEINFO_LINE = ("@_ZTS", "@typeinfo name", '@"typeinfo name')
# the length of a string literal is part of the name of a template made over it (Catch2's messages)
CHAR_ARRAY = re.compile(r"(char (?:const )?(?:\(&\) )?)\[\d+\]")
ARRAY_BOUND = re.compile(r"\[\d+\]")
WHITE_SPACE = re.compile(r"\s+")
DEREFERENCEABLE = re.compile(r"dereferenceable\(\d+\)")
GEP_BYTES = re.compile(r"(getelementptr [a-z ]*)\[\d+ x i8\]")
HEX_ESCAPE = re.compile(r"\\[0-9A-Fa-f]{2}")
# what Catch2 puts in every assertion: the line of the source, and the length of its text
CATCH_LINE = re.compile(r"(SourceLineInfo\(char const\*, unsigned long\)\(.*i64 noundef )\d+(\))")
CATCH_LENGTH = re.compile(
    r'(@operator"" _catch_sr\(char const\*, unsigned long\)\(ptr noundef @\.str[.0-9]*, '
    r"i64 noundef )\d+(\))"
)

Demangler = Callable[[list[str]], dict[str, str]]


# --- compile -------------------------------------------------------------------------------------


def convert(command: str, out: str, asserts: bool = False) -> list[str]:
    """The command of a compile_commands.json entry, changed to write unoptimised IR to `out`."""
    keep: list[str] = []
    skip = 0
    for a in shlex.split(command):
        if skip:
            skip -= 1
        elif a in ("-o", "-MT", "-MF", "-MQ"):
            skip = 1
        elif a in ("-c", "-MD", "-MMD", "-fcolor-diagnostics", "-Werror") or (
            a.startswith(("-O", "-g")) and len(a) <= 4
        ):
            continue
        else:
            keep.append(a)
    # CMake writes `-c <source>` last
    undefine = ["-UNDEBUG"] if asserts else []
    return [
        *keep[:-1],
        *undefine,
        "-O0",
        "-g0",
        "-Wno-error",
        "-S",
        "-emit-llvm",
        "-o",
        out,
        keep[-1],
    ]


def unit_name(source: str, output: str, directory: str, build: Path, src: Path) -> str | None:
    """The file a unit's IR is written to, or None when the source is not the tree's own."""
    try:
        rel = Path(source).resolve().relative_to(src)
    except ValueError:
        return None  # generated, or a dependency
    obj = os.path.relpath(os.path.join(directory, output), build)
    tag = hashlib.sha1(obj.encode()).hexdigest()[:6]
    return rel.as_posix().replace("/", "__") + f".{tag}.ll"


def run_one(entry: dict, out: Path, asserts: bool) -> tuple[str, int, str]:
    cmd = convert(entry["command"], str(out), asserts)
    p = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True, check=False)
    return entry["file"], p.returncode, p.stderr[-2000:]


def compile_tree(
    build: Path, out_dir: Path, jobs: int, tests: bool, only: str | None, asserts: bool = False
) -> int:
    build = build.resolve()
    src = (build.parent / "src").resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    db = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    work: list[tuple[dict, Path]] = []
    for e in db:
        name = unit_name(e["file"], e["output"], e["directory"], build, src)
        if name is None:
            continue
        rel = Path(e["file"]).resolve().relative_to(src).as_posix()
        if (rel.startswith("tests/") and not tests) or (only and only not in rel):
            continue
        work.append((e, out_dir / name))
    print(f"{len(work)} units of {len(db)}", flush=True)
    t0 = time.time()
    failed: list[tuple[str, str]] = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futures = [ex.submit(run_one, e, o, asserts) for e, o in work]
        for n, fu in enumerate(concurrent.futures.as_completed(futures), 1):
            source, rc, err = fu.result()
            if rc != 0:
                failed.append((source, err))
            if n % 50 == 0:
                print(f"{n}/{len(work)} {time.time() - t0:.0f}s, {len(failed)} failed", flush=True)
    print(
        f"done in {time.time() - t0:.0f}s: {len(work) - len(failed)} compiled, {len(failed)} failed"
    )
    for source, err in failed:
        print("FAILED", source)
        print("   ", err.strip().replace("\n", "\n    ")[:600])
    return 1 if failed else 0


# --- compare -------------------------------------------------------------------------------------


def demangle(names: list[str]) -> dict[str, str]:
    if not names:
        return {}
    p = subprocess.run(
        [CXXFILT], input="\n".join(names) + "\n", capture_output=True, text=True, check=True
    )
    return dict(zip(names, p.stdout.split("\n"), strict=False))


def string_length(body: str) -> int:
    """The bytes of the text of an IR string constant, `\\0A` being one."""
    return len(HEX_ESCAPE.sub("x", body))


def crude(token: str, new: bool) -> str:
    """A mangled name that could not be demangled, as it is compared: without the new component
    and the substitution indices that component shifts."""
    if new:
        token = token.replace("8iclforge3ac3", "8iclforge")
    return SUBSTITUTION.sub("S_", token)


def read_line(line: str, new: bool) -> str:
    """One line of IR with its names read alike, and the line and length Catch2 writes left out."""
    line = CHAR_ARRAY.sub(r"\g<1>[N]", read_names(line, new))
    if "Catch::" in line:
        line = ARRAY_BOUND.sub("[N]", line)
    if "<char [N]>" in line:
        line = DEREFERENCEABLE.sub("dereferenceable(N)", line)
    if "getelementptr" in line:
        line = GEP_BYTES.sub(r"\g<1>[N x i8]", line)
    if "SourceLineInfo" in line:
        line = CATCH_LINE.sub(r"\g<1>L\g<2>", line)
    elif "_catch_sr" in line:
        line = CATCH_LENGTH.sub(r"\g<1>L\g<2>", line)
    return line


def read_names(line: str, new: bool) -> str:
    """One line of IR with the new namespace read as the old."""
    if line.startswith(TYPEINFO_LINE):
        line = TYPEINFO_NAME.sub('[? x i8] c"?"', line)
        return line.replace(NEW_NAMESPACE, OLD_NAMESPACE) if new else line
    if not new or NEW_NAMESPACE not in line:
        return line
    m = STRING_DEF.match(line)
    if m and NEW_NAMESPACE in m.group("body"):
        head, body, tail = (
            m.group(k).replace(NEW_NAMESPACE, OLD_NAMESPACE) for k in ("head", "body", "tail")
        )
        return f"{head}{string_length(body)}{m.group('mid')}{body}{tail}"
    return line.replace(NEW_NAMESPACE, OLD_NAMESPACE)


def normalise(text: str, root: str, new: bool, demangler: Demangler = demangle) -> list[str]:
    """The lines of one unit's IR, names read alike (see the module)."""
    text = text.replace(root, "<T>").replace((Path(root).parent / "build").as_posix(), "<B>")
    table = demangler(sorted(set(MANGLED.findall(text))))

    def read(m: re.Match[str]) -> str:
        tok = m.group(0)
        out = table.get(tok, tok)
        return crude(tok, new) if out == tok else out

    return [read_line(line, new) for line in MANGLED.sub(read, text).split("\n")]


def by_chunks(lines: list[str]) -> list[str]:
    """The blank-line separated blocks of IR (a function, a run of globals), sorted: the order the
    compiler emitted them in follows hashes of names, which the move changes."""
    chunks: list[str] = []
    cur: list[str] = []
    for line in lines:
        if line:
            cur.append(line)
        elif cur:
            chunks.append("\n".join(cur))
            cur = []
    if cur:
        chunks.append("\n".join(cur))
    return sorted(chunks)


def squash_text(line: str) -> str:
    """A string constant without its length and its white space: what Catch2 makes of an
    assertion is the text of the expression as written, and a reflow moves the white space of an
    expression that wraps."""
    m = STRING_DEF.match(line)
    if not m:
        return line
    body = WHITE_SPACE.sub("", m.group("body"))
    return f"{m.group('head')}?{m.group('mid')}{body}{m.group('tail')}"


def verdict(a: list[str], b: list[str]) -> str:
    """identical, order (the same blocks in another order), text (and the white space of the
    strings), or differs."""
    if a == b:
        return "identical"
    if by_chunks(a) == by_chunks(b):
        return "order"
    if by_chunks([squash_text(x) for x in a]) == by_chunks([squash_text(x) for x in b]):
        return "text"
    return "differs"


def compare_pair(args: tuple[str, str, str, str, str | None]) -> tuple[str, str, int]:
    old_file, new_file, old_root, new_root, diffs = args
    a = normalise(Path(old_file).read_text(errors="replace"), old_root, False)
    b = normalise(Path(new_file).read_text(errors="replace"), new_root, True)
    name = Path(old_file).name
    v = verdict(a, b)
    if v == "identical":
        return name, v, 0
    d = list(difflib.unified_diff(a, b, "old", "new", n=2, lineterm=""))
    if diffs:
        Path(diffs, name + ".diff").write_text("\n".join(d) + "\n", encoding="utf-8")
    changed = sum(1 for x in d if x[:1] in "+-" and x[:3] not in ("+++", "---"))
    return name, v, changed


def compare_dirs(
    old: Path, new: Path, old_root: str, new_root: str, diffs: str | None, jobs: int
) -> int:
    olds = {p.name: p for p in old.glob("*.ll")}
    news = {p.name: p for p in new.glob("*.ll")}
    both = sorted(set(olds) & set(news))
    if diffs:
        Path(diffs).mkdir(parents=True, exist_ok=True)
    print(f"{len(both)} units in both ({len(olds)} old, {len(news)} new)")
    for name in sorted(set(olds) ^ set(news)):
        print("only in one tree:", name)
    work = [(str(olds[n]), str(news[n]), old_root, new_root, diffs) for n in both]
    counts = {"identical": 0, "order": 0, "text": 0, "differs": 0}
    with concurrent.futures.ProcessPoolExecutor(jobs) as ex:
        for name, v, changed in ex.map(compare_pair, work, chunksize=1):
            counts[v] += 1
            if v == "differs":
                print(f"differs: {name}: {changed} changed lines")
    print(
        f"{counts['identical']} identical, {counts['order']} the same blocks in another order, "
        f"{counts['text']} the same but for the white space of an assertion's text, "
        f"{counts['differs']} differ"
    )
    return 0 if counts["differs"] == 0 and set(olds) == set(news) else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("compile")
    c.add_argument("build")
    c.add_argument("out")
    c.add_argument("--jobs", type=int, default=8)
    c.add_argument("--tests", action="store_true")
    c.add_argument("--asserts", action="store_true")
    c.add_argument("--only", default=None)
    k = sub.add_parser("compare")
    k.add_argument("old")
    k.add_argument("new")
    k.add_argument("--old-root", required=True)
    k.add_argument("--new-root", required=True)
    k.add_argument("--diffs", default=None)
    k.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args(argv)
    if a.mode == "compile":
        return compile_tree(Path(a.build), Path(a.out), a.jobs, a.tests, a.only, a.asserts)
    return compare_dirs(Path(a.old), Path(a.new), a.old_root, a.new_root, a.diffs, a.jobs)


if __name__ == "__main__":
    sys.exit(main())
