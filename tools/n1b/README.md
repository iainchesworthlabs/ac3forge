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
| S2, moves and include spellings | `python tools/n1b/n1b_apply.py --root <worktree> --phase all --scope src --json <plan.json>` stages the renames (`git mv`) and edits the includes; the first commit is the renames alone (`git commit` with nothing added), the second is `git add -A`. |
| S2, build files | `python tools/n1b/n1b_cmake.py --root <worktree> --plan <plan.json>`: target names, output names and moved paths. The build files of the split libraries are written by hand. |
| S3, the namespace root | `python tools/n1b/n1b_names.py --root <worktree>`. |

The plan a stage writes is what the build-file pass reads, since the pass runs after the files have moved.
All the passes are idempotent: a second run on a finished tree changes nothing.

## Proof

`tools/n1b/baseline.py` records and compares what a stage must not change, against a build tree:
the public headers, the pinned-hash gate's streams, the exports of every shared library and the bytes of
44 `ac3cli` commands. `record` writes one file per kind; `verify` records again and compares with the
files committed under `tools/n1b/baselines`; `compare` compares two recorded directories; `check-moves`
checks a move plan against the recorded headers. `export_diff.py` compares two `symbols` records where a
library became several (`--map l2`) or a namespace changed (`--rewrite cuts,names`).

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
