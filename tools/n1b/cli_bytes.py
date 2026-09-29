"""The CLI corpus: a fixed list of ac3cli commands over the golden inputs, every output hashed.

    cli_bytes.py --cli <ac3cli.exe> --repo <worktree> --work <scratch dir> --out <result.json>
    cli_bytes.py --compare a.json b.json

Each command names its output files; the result records the exit code, the size and SHA-256 of every
output file and of stdout. A command that differs between two runs of the same binary is
nondeterministic and is reported so it can be dropped from the corpus. `--compare` prints every
difference between two result files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

G = "tests/golden"


def corpus(repo: Path):
    a = f"{G}/audio"
    ac4 = sorted((repo / G / "ac4dec/constructed").glob("*.ac4"))
    ext = sorted((repo / G / "external-baseline").glob("*/dee.ac3")) + sorted(
        (repo / G / "external-baseline").glob("*/dee.ec3")
    )
    cmds = [
        # ---- encode: each codec, several layouts, from the checked-in sources
        ("ac3_51_448", ["encode", f"{a}/reference_51.wav", "@out.ac3", "448"], ["out.ac3"]),
        ("ac3_stereo_192", ["encode", f"{a}/reference_stereo.wav", "@out.ac3", "192"], ["out.ac3"]),
        ("eac3_51_384", ["eac3-encode", f"{a}/reference_51.wav", "@out.ec3", "384"], ["out.ec3"]),
        (
            "eac3_stereo_128",
            ["eac3-encode", f"{a}/reference_stereo.wav", "@out.ec3", "128"],
            ["out.ec3"],
        ),
        (
            "ac4_stereo_128",
            ["ac4-encode", f"{a}/reference_stereo.wav", "@out.ac4", "128"],
            ["out.ac4"],
        ),
        ("ac4_51_256", ["ac4-encode", f"{a}/reference_51.wav", "@out.ac4", "256"], ["out.ac4"]),
        ("ac4_51_mp4", ["ac4-encode", f"{a}/reference_51.wav", "@out.mp4", "256"], ["out.mp4"]),
        (
            "atmos_objects",
            [
                "atmos-encode",
                f"{a}/reference_objects.wav",
                "@out.ec3",
                "768",
                "8",
                f"{a}/reference_objects.paths",
            ],
            ["out.ec3"],
        ),
        ("atmos_synth", ["atmos", "@out.ec3", "2", "768", "8", "1"], ["out.ec3"]),
        ("sine", ["sine", "@out.ac3", "2", "448", "1000", "50"], ["out.ac3"]),
        ("silence", ["silence", "@out.ac3", "1", "192"], ["out.ac3"]),
        ("orbit", ["orbit", "@out.ac3", "2", "448", "1"], ["out.ac3"]),
        # ---- decode: the checked-in streams and this run's own encodes
        (
            "dec_eac3_ref",
            ["decode", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.wav"],
            ["out.wav"],
        ),
    ]
    for p in ext[:4]:
        n = p.parent.name
        cmds.append(
            (
                f"dec_{n}_{p.suffix[1:]}",
                ["decode", str(p.relative_to(repo)), "@out.wav"],
                ["out.wav"],
            )
        )
        cmds.append(
            (f"probe_{n}_{p.suffix[1:]}", ["probe", str(p.relative_to(repo)), "json=1"], [])
        )
    for p in ac4[:5]:
        cmds.append(
            (f"dec_ac4_{p.stem}", ["decode", str(p.relative_to(repo)), "@out.wav"], ["out.wav"])
        )
        cmds.append((f"probe_ac4_{p.stem}", ["probe", str(p.relative_to(repo)), "json=1"], []))
    cmds += [
        ("probe_ref_ec3", ["probe", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "json=1"], []),
        ("levels_51", ["levels", f"{a}/reference_51.wav"], []),
        ("loudness_51", ["loudness", f"{a}/reference_51.wav"], []),
        ("qc_ref", ["qc", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3"], []),
        # ---- transformations of a stream
        (
            "mp4_wrap",
            ["mp4", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.mp4"],
            ["out.mp4"],
        ),
        (
            "mkv_wrap",
            ["mkv", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.mkv"],
            ["out.mkv"],
        ),
        ("ts_wrap", ["ts", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.ts"], ["out.ts"]),
        (
            "spdif",
            ["spdif", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.wav"],
            ["out.wav"],
        ),
        (
            "cut",
            ["cut", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.ec3", "0.2", "0.5"],
            ["out.ec3"],
        ),
        (
            "metadata",
            ["metadata", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.ec3", "dialnorm=27"],
            ["out.ec3"],
        ),
        (
            "normalize",
            ["normalize", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.ec3"],
            ["out.ec3"],
        ),
        (
            "transcode_to_ac4",
            ["transcode", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@out.ac4"],
            ["out.ac4"],
        ),
        ("fmp4", ["fmp4", f"{a}/reference_51_eac3_448k_cplbndstrce0.ec3", "@outdir"], ["outdir/"]),
    ]
    return cmds


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cli: Path, repo: Path, work: Path) -> dict:
    result = {}
    for name, args, outs in corpus(repo):
        d = work / name
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
        argv = [str(cli)]
        for x in args:
            if x.startswith("@"):
                argv.append(str(d / x[1:]))
            else:
                argv.append(x)
        try:
            r = subprocess.run(argv, cwd=repo, capture_output=True, timeout=180, check=False)
            out = r.stdout
            # the work directory differs between runs; the rest of stdout must not
            for variant in (
                str(d),
                str(d).replace("\\", "/"),
                str(work),
                str(work).replace("\\", "/"),
            ):
                out = out.replace(variant.encode(), b"<WORK>")
            rec = {"rc": r.returncode, "stdout": hashlib.sha256(out).hexdigest()}
        except subprocess.TimeoutExpired:
            rec = {"rc": "timeout"}
        files = {}
        for o in outs:
            if o.endswith("/"):
                base = d / o.rstrip("/")
                if base.is_dir():
                    for q in sorted(base.rglob("*")):
                        if q.is_file():
                            files[str(q.relative_to(d))] = sha(q)
            elif (d / o).is_file():
                files[o] = sha(d / o)
        rec["files"] = files
        result[name] = rec
    return result


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli")
    ap.add_argument("--repo")
    ap.add_argument("--work")
    ap.add_argument("--out")
    ap.add_argument("--compare", nargs=2)
    a = ap.parse_args()
    if a.compare:
        x = json.loads(Path(a.compare[0]).read_text(encoding="utf-8"))
        y = json.loads(Path(a.compare[1]).read_text(encoding="utf-8"))
        bad = 0
        for k in sorted(set(x) | set(y)):
            if x.get(k) != y.get(k):
                bad += 1
                print("DIFF", k, x.get(k), "|", y.get(k))
        print(f"{len(x)} commands compared, {bad} differ")
        sys.exit(1 if bad else 0)
    res = run(Path(a.cli), Path(a.repo), Path(a.work))
    Path(a.out).write_text(json.dumps(res, indent=1), encoding="utf-8")
    ok = sum(1 for v in res.values() if v.get("rc") == 0)
    hashed = sum(len(v["files"]) for v in res.values())
    print(f"{len(res)} commands, {ok} exit 0, {hashed} output files hashed")


if __name__ == "__main__":
    main()
