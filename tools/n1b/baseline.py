"""The baselines the stages of planning/layout.md are proved against: record, compare, re-check.

    baseline.py record  --build <cmake build dir> [--label msvc] [--out <dir>] [--only <kinds>]
    baseline.py record  --root <worktree> --only headers [--out <dir>]
    baseline.py verify  --build <cmake build dir> [--label msvc] [--baseline <dir>] [--only <kinds>]
    baseline.py compare <recorded dir> <recorded dir> [--only <kinds>]
    baseline.py check-moves --plan <plan.json> [--baseline <dir>] [--root <worktree>] [--pure]

<kinds> is a comma-separated list of headers, hashes, symbols and cli (the default is all four).

A stage that changes names and paths must change nothing else. Four things stand for "nothing else":

  headers  every public header (a tracked file under src/*/include/) with its git blob id. After
           the moves each one must exist at the place the move plan sends it (`check-moves`), and
           after the pure `git mv` commit with the same blob id (`--pure`): a header cannot be
           lost or edited by a move. It is read from git, so it needs no build.
  hashes   the three streams the pinned-hash gate encodes from tests/golden/audio/reference_51.wav,
           in fast and reference mode, and what tools/checks/check_cross_platform_hash.py says of
           them against tests/golden/bitstream-hashes.json, which S1 to S6 leave unchanged.
  symbols  the names each shared library exports, undecorated (MSVC: dumpbin /exports and undname),
           so that the union of the libraries a library was split into can be compared with what
           it exported (export_diff.py).
  cli      the bytes ac3cli writes over a fixed corpus of commands (cli_bytes.py): exit code,
           SHA-256 of every output file and of stdout. Recorded per compiler, since the float code
           of the AC-4 codec is only bit-exact within one.

`record` measures a built tree and writes one JSON file per kind, `<kind>-<label>.json` (headers
carry no label). The record names the commit it was measured at, which `compare` and `verify`
ignore. `verify` records into a scratch directory and compares with the committed baseline: the
check that a re-run reproduces it. The baselines committed under tools/n1b/baselines were measured
on the merge of the pull request that added them; a stage takes its own `before` from its parent,
since main moves between stages, and a change to the doc comment of a public header, which the
`headers` record notices, is a reason to record it again.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import cli_bytes
from n1b_lib import DEFAULT_ROOT

HERE = Path(__file__).resolve().parent
DEFAULT_BASELINE = HERE / "baselines"
KINDS = ("headers", "hashes", "symbols", "cli")
SCHEMA = 1
IGNORED_KEYS = ("measured_at", "tools")
GOLD_WAV = "tests/golden/audio/reference_51.wav"
HEADER_EXT = {".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp"}


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


def head_of(root: Path) -> str:
    return git(root, "rev-parse", "HEAD").strip()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cache_value(build: Path, key: str) -> str:
    text = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    m = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", text, re.MULTILINE)
    if not m:
        raise SystemExit(f"baseline: {key} is not in {build / 'CMakeCache.txt'}")
    return m.group(1).strip()


def source_root(build: Path) -> Path:
    return Path(cache_value(build, "CMAKE_HOME_DIRECTORY"))


def executable(build: Path, name: str) -> Path:
    for candidate in (build / "bin" / f"{name}.exe", build / "bin" / name):
        if candidate.is_file():
            return candidate
    raise SystemExit(f"baseline: no {name} under {build / 'bin'}; build the tree first")


# --- headers --------------------------------------------------------------------------------------


def public_headers(root: Path) -> dict[str, str]:
    """Every tracked public header under src/ and its git blob id."""
    listed = git(root, "ls-files", "-s", "-z", "--", "src")
    out: dict[str, str] = {}
    for entry in listed.split("\0"):
        if not entry:
            continue
        meta, path = entry.split("\t", 1)
        blob = meta.split()[1]
        is_header = posixpath.splitext(path)[1] in HEADER_EXT or path.endswith((".hpp.in", ".h.in"))
        if is_header and "/include/" in path:
            out[path] = blob
    return dict(sorted(out.items()))


def record_headers(root: Path) -> dict:
    return {"headers": public_headers(root)}


# --- hashes ---------------------------------------------------------------------------------------


def record_hashes(root: Path, cli: Path, work: Path) -> dict:
    """Encode the pinned gate's streams in both modes and run the gate itself over them."""
    result: dict = {
        "pins_blob": git(root, "rev-parse", "HEAD:tests/golden/bitstream-hashes.json").strip()
    }
    wav = str(root / GOLD_WAV)
    for mode, extra, suffix in (("fast", [], ""), ("reference", ["mode=reference"], "_reference")):
        out = work / f"hashes-{mode}"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        for args, name in (
            (["encode", wav, str(out / "gold.ac3"), "448", "51", "dither=off"], "gold.ac3"),
            (["eac3-encode", wav, str(out / "gold.ec3"), "256", "nodither", "51"], "gold.ec3"),
            (
                ["eac3-encode", wav, str(out / "gold_cpl.ec3"), "256", "cpl+nodither", "51"],
                "gold_cpl.ec3",
            ),
        ):
            done = subprocess.run(
                [str(cli), *args, *extra], capture_output=True, cwd=root, check=False
            )
            if done.returncode != 0:
                raise SystemExit(
                    f"baseline: {' '.join(args[:1])} for {name} exited {done.returncode}"
                )
        gate = subprocess.run(
            [
                sys.executable,
                str(root / "tools" / "checks" / "check_cross_platform_hash.py"),
                "--cli",
                str(cli),
                "--workdir",
                str(out),
                "--label-suffix",
                suffix,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        result[mode] = {
            "gate_exit": gate.returncode,
            "gate_output": sorted(
                line for line in gate.stdout.splitlines() if line.startswith("[")
            ),
            "streams": {n: sha256_file(out / n) for n in ("gold.ac3", "gold.ec3", "gold_cpl.ec3")},
        }
    return result


# --- symbols --------------------------------------------------------------------------------------


def msvc_tools(build: Path) -> tuple[Path, Path]:
    """dumpbin.exe and undname.exe, which sit beside the link.exe the tree was configured with."""
    linker = Path(cache_value(build, "CMAKE_LINKER"))
    dumpbin, undname = linker.with_name("dumpbin.exe"), linker.with_name("undname.exe")
    if not dumpbin.is_file() or not undname.is_file():
        raise SystemExit(f"baseline: dumpbin.exe and undname.exe are not beside {linker}")
    return dumpbin, undname


def dll_targets(build: Path) -> list[str]:
    """The shared libraries this tree links, by file name, from its ninja file."""
    text = (build / "build.ninja").read_text(encoding="utf-8", errors="replace")
    found = {
        posixpath.basename(m.group(1).replace("\\", "/"))
        for m in re.finditer(r"^build (bin[\\/][^\s:|$]+\.dll)\b", text, re.MULTILINE)
    }
    return sorted(found)


def dumpbin_exports(dumpbin: Path, dll: Path) -> list[str]:
    out = subprocess.run(
        [str(dumpbin), "/exports", str(dll)], capture_output=True, text=True, check=True
    ).stdout
    names = []
    for line in out.splitlines():
        m = re.match(r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)", line)
        if m:
            names.append(m.group(1))
    return names


def undecorate(undname: Path, names: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for i in range(0, len(names), 40):
        chunk = names[i : i + 40]
        out = subprocess.run(
            [str(undname), *chunk], capture_output=True, text=True, check=True
        ).stdout
        current = None
        for line in out.splitlines():
            m = re.match(r'^Undecoration of :- "(.*)"$', line)
            if m:
                current = m.group(1)
                continue
            m = re.match(r'^is :- "(.*)"$', line)
            if m and current is not None:
                result[current] = m.group(1)
                current = None
    return result


def record_symbols(build: Path) -> dict:
    dumpbin, undname = msvc_tools(build)
    libraries: dict[str, list[str]] = {}
    for name in dll_targets(build):
        dll = build / "bin" / name
        if not dll.is_file():
            raise SystemExit(
                f"baseline: {name} is a target of {build} but is not built; build every target"
            )
        raw = dumpbin_exports(dumpbin, dll)
        undone = undecorate(undname, raw)
        libraries[name] = sorted({undone.get(r, r) for r in raw})
    return {"libraries": libraries}


# --- cli ------------------------------------------------------------------------------------------


def record_cli(root: Path, cli: Path, work: Path) -> dict:
    scratch = work / "cli"
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)
    return {"commands": cli_bytes.run(cli, root, scratch)}


# --- record, compare ------------------------------------------------------------------------------


def label_of(build: Path) -> str:
    compiler = Path(cache_value(build, "CMAKE_CXX_COMPILER")).name.lower()
    if "clang-cl" in compiler:
        return "clangcl"
    if compiler.startswith("cl"):
        return "msvc"
    return re.sub(r"[^a-z0-9]+", "", compiler.replace(".exe", ""))


def file_name(kind: str, label: str) -> str:
    return "headers.json" if kind == "headers" else f"{kind}-{label}.json"


def record(
    build: Path | None,
    label: str,
    kinds: tuple[str, ...],
    work: Path,
    root: Path | None = None,
) -> dict[str, dict]:
    """Measure `kinds`. Only the header record can do without a build: it is read from git."""
    if build is None and set(kinds) != {"headers"}:
        raise SystemExit("baseline: --build is needed for every kind but headers")
    if root is None:
        if build is None:
            raise SystemExit("baseline: give --build, or --root with --only headers")
        root = source_root(build)
    made: dict[str, dict] = {}
    for kind in kinds:
        if kind == "headers":
            body = record_headers(root)
        elif kind == "hashes":
            body = record_hashes(root, executable(build, "ac3cli"), work)
        elif kind == "symbols":
            body = record_symbols(build)
        elif kind == "cli":
            body = record_cli(root, executable(build, "ac3cli"), work)
        else:
            raise SystemExit(f"baseline: unknown kind {kind!r}")
        made[kind] = {
            "schema": SCHEMA,
            "kind": kind,
            # the headers are the same whatever compiler built the tree
            "label": "" if kind == "headers" else label,
            "measured_at": head_of(root),
            **body,
        }
    return made


def write_records(made: dict[str, dict], out: Path, label: str) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    paths = []
    for kind, body in made.items():
        path = out / file_name(kind, label)
        path.write_text(
            json.dumps(body, indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n"
        )
        paths.append(path)
    return paths


def differences(a: dict, b: dict, path: str = "") -> list[str]:
    """Where two records differ, one line each; the keys in IGNORED_KEYS are not compared."""
    out: list[str] = []
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key in IGNORED_KEYS:
                continue
            here = f"{path}/{key}"
            if key not in a:
                out.append(f"+ {here}")
            elif key not in b:
                out.append(f"- {here}")
            else:
                out.extend(differences(a[key], b[key], here))
    elif isinstance(a, list) and isinstance(b, list):
        if a != b:
            gone, new = [x for x in a if x not in b], [x for x in b if x not in a]
            out.append(
                f"~ {path}: {len(gone)} removed, {len(new)} added; e.g. -{gone[:3]} +{new[:3]}"
            )
    elif a != b:
        out.append(f"~ {path}: {str(a)[:80]} -> {str(b)[:80]}")
    return out


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def compare_dirs(a: Path, b: Path, kinds: tuple[str, ...]) -> int:
    bad = 0
    for path_a in sorted(a.glob("*.json")):
        kind = path_a.name.split("-")[0].removesuffix(".json")
        if kind not in kinds:
            continue
        path_b = b / path_a.name
        if not path_b.is_file():
            print(f"{path_a.name}: missing from {b}")
            bad += 1
            continue
        diff = differences(load(path_a), load(path_b))
        print(f"{path_a.name}: {'identical' if not diff else f'{len(diff)} differences'}")
        for line in diff[:40]:
            print("   ", line)
        bad += 1 if diff else 0
    return bad


# --- check-moves ----------------------------------------------------------------------------------


def tracked_blobs(root: Path) -> dict[str, str]:
    """Every tracked file and its git blob id."""
    out: dict[str, str] = {}
    for entry in git(root, "ls-files", "-s", "-z").split("\0"):
        if entry:
            meta, path = entry.split("\t", 1)
            out[path] = meta.split()[1]
    return out


def check_moves(plan: Path, baseline: Path, root: Path, pure: bool) -> int:
    moves = json.loads(plan.read_text(encoding="utf-8"))["moves"]
    recorded = load(baseline / "headers.json")["headers"]
    tracked = tracked_blobs(root)
    missing, changed = [], []
    for old, blob in recorded.items():
        new = moves.get(old, old)
        if new not in tracked:
            missing.append((old, new))
        elif pure and tracked[new] != blob:
            changed.append((old, new))
    print(
        f"{len(recorded)} public headers in the baseline; "
        f"{len(missing)} missing, {len(changed)} changed"
    )
    for old, new in missing[:20]:
        print(f"  missing: {old} -> {new}")
    for old, new in changed[:20]:
        print(f"  changed: {old} -> {new}")
    return 1 if missing or changed else 0


def parse_kinds(text: str) -> tuple[str, ...]:
    kinds = tuple(k for k in text.split(",") if k)
    for k in kinds:
        if k not in KINDS:
            raise SystemExit(f"baseline: unknown kind {k!r}; the kinds are {', '.join(KINDS)}")
    return kinds


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("record", "verify"):
        p = sub.add_parser(name)
        p.add_argument("--build", type=Path, default=None, help="required but for --only headers")
        p.add_argument(
            "--root", type=Path, default=None, help="the source tree, if not the build's"
        )
        p.add_argument("--label", default=None)
        p.add_argument("--only", default=",".join(KINDS))
        p.add_argument(
            "--out" if name == "record" else "--baseline", type=Path, default=DEFAULT_BASELINE
        )
    p = sub.add_parser("compare")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--only", default=",".join(KINDS))
    p = sub.add_parser("check-moves")
    p.add_argument("--plan", required=True, type=Path)
    p.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    p.add_argument("--root", type=Path, default=Path(DEFAULT_ROOT))
    p.add_argument("--pure", action="store_true", help="also require the same blob at the new path")
    args = ap.parse_args(argv)

    if args.cmd == "compare":
        return 1 if compare_dirs(args.a, args.b, parse_kinds(args.only)) else 0
    if args.cmd == "check-moves":
        return check_moves(args.plan, args.baseline, args.root, args.pure)

    label = args.label or (label_of(args.build) if args.build else "none")
    kinds = parse_kinds(args.only)
    with tempfile.TemporaryDirectory(prefix="n1b-baseline-") as tmp:
        made = record(args.build, label, kinds, Path(tmp), args.root)
        if args.cmd == "record":
            for path in write_records(made, args.out, label):
                print("wrote", path)
            return 0
        scratch = Path(tmp) / "recorded"
        write_records(made, scratch, label)
        return 1 if compare_dirs(args.baseline, scratch, kinds) else 0


if __name__ == "__main__":
    sys.exit(main())
