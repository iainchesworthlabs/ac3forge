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
| S2, paths in text | `python tools/n1b/n1b_paths.py --root <worktree> --plan <plan.json>`: every other file that names a moved file by its repository path (pages, plans, comments, strings, a Python path built from its components, workflows, scripts) follows it, so `check_doc_paths.py` stays green. It lists the directories whose files went to several libraries and are still named. |
| S3, the namespace root | `python tools/n1b/n1b_names.py --root <worktree>`; then `python tools/n1b/n1b_reflow.py --root <worktree> --base HEAD~1`, which wraps the lines the first pass pushed past the column limit. Each is committed alone (below). |

The plan a stage writes is what the build-file pass reads, since the pass runs after the files have moved.
All the passes are idempotent: a second run on a finished tree changes nothing.

## S2, start to finish

In a worktree of `main` after the seven cuts, the fix of the static `ac3forge_c.pc` (#1157) and the
docs sweep have merged, with `<before>` an MSVC build of that `main` (every option on, no `--target`)
and `<work>` a directory outside the tree:

    python tools/n1b/baseline.py record --build <before> --out <work>/before
    python tools/n1b/n1b_apply.py --root . --phase all --json <work>/plan.json     # 404 moves, half a minute
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

Then the proof. On Windows, build every default target with MSVC, `baseline.py record --build <after>
--out <work>/after`, and

    python tools/n1b/baseline.py compare <work>/before <work>/after --only hashes,cli        # identical
    python tools/n1b/export_diff.py --old <work>/before/symbols-msvc.json \
        --new <work>/after/symbols-msvc.json --map l2      # every library the same, plus has_avx2()
    python tools/checks/check_layering.py                  # 0 debts, 217 edges, 0 failures
    python tools/checks/check_doc_paths.py
    python tools/ci/precheck.py

On Linux, the `-Werror` builds with GCC 16 and Clang 22 and the whole `ac3tests`, and three checks that
only the nightly run holds (`shared_libs` is deep-only in `.github/ci/legs.jsonc`), each of which a moved
layout can break without a test noticing:

    cmake --preset config-linux-llvm-shared && cmake --build build/config-linux-llvm-shared
    ctest --preset test-linux-llvm-shared -LE Performance
    bash tools/checks/check_shared_forge_binding.sh build/config-linux-llvm-shared/bin/ac3tests \
        <the six libiclforge_{ac3,base,dsp,objects,render,iec61937}.so, in src/<name>/ of that tree>
    python tools/ci/check_abi_symbols.py --allowlist-dir tools/ci/abi-allowlist --lib <each .so of it>
    bash tools/checks/check_install_consumer.sh <the three trees _ci-linux.yml builds for it>
    python tools/n1b/abi_compare.py <the parent's allowlists> tools/ci/abi-allowlist

The last one names what each library lost or gained against the parent's allowlists, which should be
`has_avx2()` alone. Take the parent's from `check_abi_symbols.py --update` over the shared libraries of the
same `main` before S2, not from the committed files: those were seven names short of the build when this was
written, which is no doing of S2.

`s2-hand.patch` is the part of S2 no script does, made on the output of the scripts above (51 files, 858
lines added and 756 removed):

- The build of the split: `cmake/IclforgeLibrary.cmake` (new: `iclforge_add_library()` and
  `iclforge_install_library()`, which also writes each library's `.pc` file), the `CMakeLists.txt` of
  `base`, `dsp`, `objects`, `render` and `iec61937` (new), `src/ac3/CMakeLists.txt` rewritten to link
  them, the root's `add_subdirectory` list, the install rules for six export sets and
  `ac3forgeConfig.cmake.in`; `ICLFORGE_BASE_EXPORT` on `has_avx2()`, the one source edit; the
  minimum-footprint profile's archive, made of files from six libraries, with the five export headers it
  generates; the ESP-IDF component and packer.
- What a moved layout does to the checks and the CI that name a tree, a build output or an installed
  name: the path filters of the change planner and of `esp-component.yml` and `wheels.yml`; the Sonar
  job's archives; `check_install_consumer.sh` (the installed include directory, the library names);
  `check_shared_forge_binding.sh` (six libraries, not one); the gcovr filter and a floor row for each new
  library in `coverage_report.sh`, measured from one run; the sixteen ABI allowlists; the Rust sys
  crate's build script; and two generators, whose output directories and emitted `#include` lines the
  include pass cannot know.
- Five comment lines that the rewrites made longer than 100 columns.

It goes stale as `main` moves. On a `main` that has changed a file it touches, `git apply --3way`
merges where the repository has the blobs it was made from and otherwise stops, changing nothing, with
the files that do not fit; `git apply --reject tools/n1b/s2-hand.patch` applies every hunk that fits and
leaves each one that does not in a `.rej` beside its file, and the hunk's own lines say what the edit
was for. To make it again on a later `main`: run the four steps, apply the old patch with `--reject`,
settle the `.rej` files by hand, commit, and write

    git diff --binary -M <scripts> HEAD -- . ':!tools/n1b' --output=tools/n1b/s2-hand.patch

where `<scripts>` is the commit "S2: build files and paths in text" (`--output`, not a shell redirect,
which would not write the file as bytes). Applying it to the script
output alone (`git read-tree` into a scratch index, then `git apply --cached`) must give the tree of
the commit. What the patch leaves, and the scripts list: variant directories that became
`variants/<axis>-<choice>/` and are still named in comments, and the comments and pages that name a
library file by its old name (`libac3forge.so`), which S4 and S5 rewrite.

## S3, start to finish

In a worktree of `main` with S2 merged, with `<before>` an MSVC build of that `main` (every option on, no
`--target`) and `<work>` a directory outside the tree:

    python tools/n1b/baseline.py record --build <before> --out <work>/before
    python tools/n1b/n1b_names.py --root . --dry-run          # 1,208 files, under a minute
    python tools/n1b/n1b_names.py --root .
    git add -A && git commit -m "S3: namespace root ac3 to iclforge"
    python tools/n1b/n1b_reflow.py --root . --base HEAD~1     # 1,365 lines in 294 files, 6 left over
    git add -A && git commit -m "S3: wrap the lines the namespace pass pushed past 100 columns"

Both commits are what the script gives on its parent, and a run of the two in a scratch repository made
from `git -c core.autocrlf=false archive` of the parent gives their trees blob for blob. The reflow uses
the clang-format of the machine (22.1.2 for S3): nothing in CI checks the length of a C++ line, so it
keeps to `.clang-format` and not to a gate, and it touches only the lines the pass lengthened. The plan's
count of 657 lines (190 files) was of `ac3::` alone; `ac4::` and `mp4::` gain ten columns, not five.

What the two passes cannot see is done by hand, in a commit of its own after them:

- Libraries that were top-level namespaces nest under `iclforge`, so libadm's own `adm` (a third-party
  namespace) is found from inside `iclforge::adm` first: `n1b_names.py` writes every unqualified `adm::`
  as `::adm::`. It surfaced as C2039 in `src/adm` on the first build; the plan's two hazards did not
  include it.
- Mangled names cannot be rewritten as text (`_ZN3ac3...` becomes `_ZN8iclforge3ac4...`; the length
  prefix and the substitution indices change): `check_shared_forge_binding.sh` (its pattern and the
  `has_avx2` exclusion), the `-Wl,--undefined=` of the `hearth_sink` example, and the sample text of
  `footprint_report.py` and its test. The ABI allowlists hold demangled names: regenerate them with
  `check_abi_symbols.py --update` over the shared libraries of the shared-libs pass, and check the
  result against the old files rewritten as text (the same sets).
- Qt names the context of a `tr()` after the class's qualified name, so the six `ac3hearth_*.ts` name
  `iclforge::hearth::ui::HearthController` and `NetworkController` now: rename them before the `*_lupdate`
  targets run, or every translation of the two classes turns obsolete. The three targets restamp the
  `<location>` lines the reflow moved (the GUI and Crucible gates compare them).
- The generators that emit `namespace ac3::...` or `namespace ac4::...` (nine scripts under `tools/` and
  `tools/references/`) and `tools/packaging/pack_esp_component.py`, which writes a `main.cpp`. Each
  generator, run against the tree, reproduces the committed header; `gen_joc_tables.py` needs TS 103 420's
  text and was not run.
- The lines over the limit that the formatter does not touch (`// clang-format off` regions).

The proof: `baseline.py compare <work>/before <work>/after --only hashes,cli` is identical (the headers
and the exported names change by design, so those two are not compared), and

    python tools/n1b/export_diff.py --old <work>/before/symbols-msvc.json \
        --new <work>/after/symbols-msvc.json --rewrite names     # every library the same

What S3 leaves for S4: the CMake helper targets (`ac3::warnings`, `coverage`, `fmt`, `fmt_private`,
`tracy`, `minimal_profile`), the C++ namespaces named for a program or a package (`ac3cli`, `ac3gui`,
`ac3probe`, `ac3forge`, `ac3forge_c`) and `sendspin::ac3forge`.

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
