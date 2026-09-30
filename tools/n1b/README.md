# tools/n1b: the scripts of the re-layout

[planning/layout.md](../../planning/layout.md) plans a new layout for the libraries under `src/` and
names the stages that reach it. These scripts make the stages, so that each stage is a run of them on
a fresh `main` and not a hand edit of two thousand files. Everything here reads the tracked files of the
worktree it is given (`--root`, default this repository) and nothing runs in CI except the unit tests
(`python -m unittest discover -s tools/n1b`) and `tools/checks/check_layering.py`, which is a check of
its own and stays when these are gone.

Each script's header says what it does and takes. This page says in what order.

## The stages

| stage | what runs, in a worktree of `main` with the stage before it merged |
|---|---|
| S1, the cuts | `python tools/n1b/cuts.py --root <worktree> [--only C1,C3]`; one commit per cut. |
| S2, moves and include spellings | `python tools/n1b/n1b_apply.py --root <worktree> --phase all --json <plan.json>` moves `src/` and `tests/` (the default scope, `src,tests`): it stages the renames (`git mv`) and edits the includes; the first commit is the renames alone (`git commit` with nothing added), the second is `git add -A`. |
| S2, build files | `python tools/n1b/n1b_cmake.py --root <worktree> --plan <plan.json>`: target names, output names and moved paths, and the paths `tests/CMakeLists.txt` names relative to itself. The build files of the split libraries are written by hand. |
| S2, paths in text | `python tools/n1b/n1b_paths.py --root <worktree> --plan <plan.json>`: every other file that names a moved file by its repository path (pages, plans, comments, strings, workflows, scripts) follows it, so `check_doc_paths.py` stays green. It lists the directories whose files went to several libraries and are still named. |
| S3, the namespace root | `python tools/n1b/n1b_names.py --root <worktree>`. |

The plan a stage writes is what the build-file pass reads, since the pass runs after the files have moved.
All the passes are idempotent: a second run on a finished tree changes nothing.

## S2, start to finish

In a worktree of `main` after the seven cuts and the docs sweep have merged, with `<before>` an MSVC
build of that `main` (every option on, no `--target`) and `<work>` a directory outside the tree:

    python tools/n1b/baseline.py record --build <before> --out <work>/before
    python tools/n1b/n1b_apply.py --root . --phase all --json <work>/plan.json     # 402 moves, about a minute
    git commit -m "S2: move src and tests"          # nothing added: the staged renames alone
    python tools/n1b/baseline.py check-moves --plan <work>/plan.json --baseline <work>/before --pure
    git add -A && git commit -m "S2: include spellings"
    python tools/n1b/n1b_cmake.py --root . --plan <work>/plan.json
    python tools/n1b/n1b_paths.py --root . --plan <work>/plan.json
    git add -A && git commit -m "S2: build files and paths in text"
    git apply --3way tools/n1b/s2-hand.patch        # the hand-written part, below
    git add -A && git commit -m "S2: the build of the split libraries"

`n1b_apply.py` prints `PROBLEM` for nine includes (on the day of writing): tests that include a private
header of `src/ac3/src/` by its bare name, `test_mdct_fixed.cpp` and eight like it. Each is met by an
include directory that `tests/CMakeLists.txt` already names and `n1b_cmake.py` moves with the header,
so there is nothing to do; a `PROBLEM` outside `tests/` would be a real include across libraries.

Then build every default target with MSVC, `baseline.py record --build <after> --out <work>/after`, and

    python tools/n1b/baseline.py compare <work>/before <work>/after --only hashes,cli        # identical
    python tools/n1b/export_diff.py --old <work>/before/symbols-msvc.json \
        --new <work>/after/symbols-msvc.json --map l2      # every library the same, plus has_avx2()
    python tools/checks/check_layering.py                  # 0 debts, 213 edges, 0 failures
    python tools/checks/check_doc_paths.py
    python tools/ci/precheck.py

`s2-hand.patch` is the part of S2 no script does, made on the output of the scripts above (28 files,
about 600 lines): `cmake/IclforgeLibrary.cmake` and the `CMakeLists.txt` of `base`, `dsp`, `objects`,
`render` and `iec61937` (new), `src/ac3/CMakeLists.txt` rewritten to link them, the root's
`add_subdirectory` list and the install rules for six export sets; `ICLFORGE_BASE_EXPORT` on
`has_avx2()`, the one source edit; the minimum-footprint profile's archive, made of files from six
libraries, with the five export headers it generates; the ESP-IDF component and packer; the path
filters of the change planner (`tools/ci/classify_changes.py`) and of `esp-component.yml` and
`wheels.yml`, which need the five new trees beside `src/ac3/`, and the test that names one; and five
comment lines the rewrites made longer than 100 columns. It goes stale as `main` moves: on a `main`
that has changed those files, `git apply --3way` leaves conflict markers where it did, and the file's
own hunks say what each edit was for. What it leaves, and the scripts list: variant directories that
became `variants/<axis>-<choice>/` and are still named in comments, the build tree's own path in
`_ci-linux.yml`, and the coverage floors of `tools/checks/coverage_report.sh` (six components from one
run, where the script now has one row for `src/ac3`).

## Proof

`tools/n1b/baseline.py` records and compares what a stage must not change, against a build tree:
the public headers, the pinned-hash gate's streams, the exports of every shared library and the bytes of
44 `ac3cli` commands. `record` writes one file per kind; `verify` records again and compares with the
files committed under `tools/n1b/baselines`; `compare` compares two recorded directories; `check-moves`
checks a move plan against the recorded headers. The header record is read from git, so
`record --root <worktree> --only headers` needs no build; a change to the doc comment of a public
header changes it, and the committed one is recorded again when that happens. `export_diff.py` compares two `symbols` records where a
library became several (`--map l2`) or a namespace changed (`--rewrite cuts,names`), and with `--copies`
lets a library lose names that another library of the new record still exports: `admbridge.dll` links
the codec statically and re-exports the members it pulls in, so a change to what its headers include
changes how many it carries. The union of every library's names is compared in any case.

`tools/checks/check_layering.py` fails an include that crosses from one library into another its row of
`tools/checks/layering.json` does not list. The includes a pending cut still removes are listed in
`tools/checks/layering_debt/`, one file per cut, and the change that makes the cut deletes its file.

## The census

`regen_inventory.ps1 -Root <worktree> -Out <dir>` runs `include_graph.py`, `inventory.py`,
`namespaces.py`, `ident_census.py`, `naming_counts.py`, `path_keyed.py`, `overlap.py`, `reflow_cost.py`,
`layout_dryrun.py`, `violations.py`, `privcross.py`, `symtab.py`, `cmake_repeat.py` and
`truehd_includes.py` over the tree, and with `-Appendix <file>` writes `planning/layout-inventory.md`
from what they found (`appendix.py`). It takes about a minute. `layoutdef.py` is the layout as data: where
every tracked path goes, and the header spelling that reaches it before and after.

## Open branches

`adapt_branch.ps1 -Apply` runs a stage's scripts on a branch, records them as merged and merges `main`, so
that the branch meets only the hand-written commits of the stage; `-Measure` counts, without touching the
branch, the files that conflict by hand and after the scripts. `check_anchors.py`, `check_tables.py`,
`json_diff.py`, `show_conflicts.py`, `branch_table.py` and `control_experiment.ps1` are the small tools
the study used to check its pages and its measurements.
