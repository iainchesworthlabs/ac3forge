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
| S4, packages and identifiers | `n1b_apply.py --scope packages` and `n1b_paths.py` (the package directories and the files named for the brand), `n1b_sendspin.py`, `n1b_idents.py`, `n1b_reflow.py`, and then `cargo fmt` and `cargo update --workspace --offline` in `rust/`. Each is committed alone (below). |
| N1A, the programs' names | `python tools/n1b/n1b_programs.py --root <worktree> --phase all --json <table.json>` (the moves are staged, the text is not), `n1b_reflow.py`, and then `lupdate` and `gen_pseudo_locale.py` over the renamed sources. The settings migration, the JNI check and what the driver keeps are by hand. Each is committed alone (below). |
| S5, the pages and the addresses | `python tools/n1b/n1b_docs.py --root <worktree> --phase text`, then `--phase urls`, then `--phase words`, each committed alone (below). The install routes, the library pages, the code that names the repository, the status of the plans and the changelog are by hand. |

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
  `check_abi_symbols.py --update` over the libraries-only shared tree, and check the result against
  the old files rewritten as text (`abi_compare.py`, in the proof below).
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
    python tools/n1b/abi_compare.py <the parent's allowlists> tools/ci/abi-allowlist \
        --map identity --rewrite names                            # every library -0 +0

What S3 leaves for S4: the CMake helper targets (`ac3::warnings`, `coverage`, `fmt`, `fmt_private`,
`tracy`, `minimal_profile`), the C++ namespaces named for a program or a package (`ac3cli`, `ac3gui`,
`ac3probe`, `ac3forge`, `ac3forge_c`) and `sendspin::ac3forge`.

## S4, start to finish

In a worktree of `main` with S3 merged, with `<before>` a record of that `main` (`baseline.py record` over an
MSVC build with every option on and no `--target`; S3's own record of its result is one) and `<work>` a directory
outside the tree. Each script arrives in a commit of its own just before the pass that uses it, so the parent of a
scripted commit holds the script that made it: `layoutdef.py`'s package renames, then `n1b_sendspin.py`, then
`n1b_idents.py` (with `--rewrite idents` for `export_diff.py` and `abi_compare.py`).

    python tools/n1b/n1b_apply.py --root . --phase all --scope packages --json <work>/plan-s4.json   # 211 moves
    git commit -m "S4: move the package directories"      # nothing added: the staged renames alone
    git add -A && git commit -m "S4: include spellings"   # 96 includes in 66 files
    python tools/n1b/n1b_paths.py --root . --plan <work>/plan-s4.json                                # 99 files
    git add -A && git commit -m "S4: paths in text"
    python tools/n1b/n1b_sendspin.py --root .                                                        # 33 files, 164 lines
    git add -A && git commit -m "S4: sendspin::ac3forge becomes sendspin::player"
    python tools/n1b/n1b_idents.py --root . --dry-run                                                # 713 files, 20 s
    python tools/n1b/n1b_idents.py --root . --report <work>/idents-report.txt
    git add -A && git commit -m "S4: the brand ac3forge becomes iclforge ..."
    python tools/n1b/n1b_reflow.py --root . --base HEAD~1                                            # 19 lines in 9 files
    git add -A && git commit -m "S4: wrap the lines the identifier pass pushed past 100 columns"
    (cd rust && cargo fmt --all)                                                                     # 4 files
    git add -A && git commit -m "S4: cargo fmt puts the renamed use items in order"
    (cd rust && cargo update --workspace --offline)                                                  # Cargo.lock: 15 lines each way
    git add -A && git commit -m "S4: Cargo.lock lists the renamed crates in cargo's order"

Every one of these is what its script gives on its parent: a scratch repository made from
`git -c core.autocrlf=false archive` of the parent, with the script run there from the scratch tree itself, gives
the commit's blobs (3,034 paths, 3,038 with the two scripts), and a second run of each script changes nothing.
`cargo fmt` is needed because rustfmt sorts the items of a `use` block and `iclforge` sorts after `common`;
`cargo update --workspace --offline` because `cargo build --locked` refuses a `Cargo.lock` whose packages are not
in cargo's order, which is where the text rename leaves the two workspace crates.

The Sendspin namespace goes first. The C++ of the extension role `_ac3forge_player@v1` is
`iclforge::sendspin::ac3forge`, which the identifier pass would make `iclforge::sendspin::iclforge`: inside
`iclforge::sendspin` the unqualified `iclforge` then finds that namespace before the family's root. So
`n1b_sendspin.py` makes it `player`, the word the role's name ends in, on every qualification (`sendspin::`, `ss::`,
the fully spelled name, the namespace aliases) and on the unqualified name only where the scope is
`iclforge::sendspin` (`src/sendspin`, the Hearth controller's comments). An unqualified `ac3forge::` elsewhere is the
ESP-IDF component's own namespace, which the identifier pass merges into the root.

### What `n1b_idents.py` decides

One decision per occurrence, each with a name and a reason (`RULES` in the script; `--report` lists what was kept).
It renames the brand where it is an identifier, a name or a string that both ends of something read: the C API
(`ac3forge_*`, `AC3FORGE_*`, `AC3FORGEC_EXPORT`), CMake options and variables, the package config, Kconfig, the
environment variables, the wire and format strings (`_ac3forge_player@v1`, the OTA project name, `ac3forge.probe/1`,
`ac3forge_scene`, the container `writing_app`), file, package and release-asset names, the bindings (the Python
module, the crates, the npm package and the JS factories `Ac3Forge...` to `IclForge...`) and the entries of the Qt
catalogues that quote one. Four families carry a prefix of their own and move with it: the per-library export
macros (`MP4_EXPORT` becomes `ICLFORGE_MP4_EXPORT`, the name `iclforge_add_library()` makes), the CMake helper
targets (`ac3::warnings` becomes `iclforge::warnings`), the profiling macros (`AC3_ZONE_BEGIN` becomes
`ICLFORGE_ZONE_BEGIN`) and the AC-3 library's files as comments still name them (`libac3forge.so` becomes
`libiclforge_ac3.so`). It keeps, and says so:

- **External identities**, which change with the repository (S5, and the owner's): the repository slug and the Pages
  address, the SonarCloud project, the tap repository, and the paths a runner derives from the repository's name.
- **What N1A renames** with the programs: a program's QSettings organisation, registry keys and user-data
  directories, its icons, its window titles and every string a person reads, the QML module URIs, the Android
  package and its JNI names, the Windows driver, the packages named for a program. In the trees that are a program's
  (`PROGRAM_TREES`), and in QML, HTML and the Qt catalogues, the bare word `ac3forge` is the program's; the identifiers
  and the strings both ends of the wire read are still renamed there.
- **What reaches a signature**: the example key of `examples/object_signing.cpp` is a key's own bytes. No other
  string of the family reaches a signature, an HMAC, a key derivation, a magic number or a tag: the strings were
  searched in `src/signing`, `src/objects`, `src/ac3` (OAMD, JOC, EMDF), `src/sendspin`, the OTA image checks and
  `apps/hearth`.
- **The old name written on purpose**: what the hand-written commit says about the past (the PyPI description
  "formerly ac3forge", the winget identity of the released manifests, the old names the tap maps) and the two files
  that are about it (`.git-blame-ignore-revs`, `tap_migrations.json`). They are named in `FORMER_NAME_LINES`, so a
  run on the tree after the hand-written commit changes nothing either, and does not undo it.

Pages (`.md`), the history (`CHANGELOG.md`, `planning/`, the scripts of this migration) and the byte-exact trees
(`tests/golden`, the released winget manifests) are not read. The winget package identity a later release is written
under is `iainchesworthlabs.iclforge`: the submission of the old one (winget-pkgs #419594) was closed unmerged on
2026-09-29, so nothing outside the repository holds it.

### What the passes cannot decide, by hand

- The pkg-config names, `iclforge-<library>` (`planning/layout.md` (e), the naming map): the library files stay `libiclforge_<library>`,
  the `.pc` files and their `Requires` take a hyphen. `cmake/InstallLibrary.cmake`, `cmake/IclforgeLibrary.cmake`,
  `cmake/PkgConfig.cmake`, `check_install_consumer.sh` and the consumers' comments.
- The ESPHome component's namespace is `esphome::iclforge`, which the unqualified `iclforge::` finds before the
  family's root: the references to the root are written `::iclforge::`. The component needs ESPHome's headers to
  build, which this repository does not carry; against stub headers a compile of it (clang-cl `-fsyntax-only`) passes,
  and fails without the four `::`.
- Homebrew: the formula and the cask are both `iclforge`, `tap_migrations.json` maps `ac3forge` and `ac3gui` to
  them and goes to the root of the tap (`manifest-bump.yml` copies it there).
- winget: `bump_manifests.py` writes a later release under `manifests/i/iainchesworthlabs/iclforge/`, the four
  released versions stay under `.../ac3forge/`, and `check_packaging_versions.sh` reads both directories with the
  identity each one's name gives.
- The stand-in of the device page's tests repeats the firmware's replies "character for character"
  (`apps/wasm/tests/device-ui/stub.js`, read against `esp-idf/iclforge/src/control.cpp` by `contract.spec.js`): it is
  in a program's tree, where the bare word stays, and the heading of `GET /api` is a string both ends read.
- The three table generators write where their headers are; `gen_joc_tables.py` needs TS 103 420's text and was not
  run. The PyPI project's description says "(formerly ac3forge)". The GUI test that reads the About dialog's
  version line expects the library's, which begins `iclforge` now.

The proof is S3's, with what S4 changes:

    python tools/n1b/baseline.py compare <before> <after> --only hashes,cli
    python tools/n1b/export_diff.py --old <before>/symbols-msvc.json --new <after>/symbols-msvc.json \
        --rewrite idents                                       # every library the same, -0 +0
    python tools/n1b/abi_compare.py <the parent's allowlists> tools/ci/abi-allowlist \
        --map identity --rewrite idents                        # every library -0 +0; only libiclforge_c.so.txt changes as a file

The hashes are identical. The CLI corpus differs in 14 of its 44 commands, and in nothing but the family's name:
the `schema` line of the ten `probe` outputs (`ac3forge.probe/1`) and the handler or writing-application string of
the four MP4 and Matroska files. `cli_bytes.py` hashes the outputs, so to see that nothing else moved run the corpus
with the old and the new `ac3cli` and compare the files byte for byte: every difference is the `a` and the `3` of
the old brand against the `i` and the `l` of the new, at the same offsets, and no length changes.

What S4 leaves: N1A's list (the programs' own names and what they register: `--report` names every place), S5's
(the pages, the changelog and the plans: 177 lines of `CHANGELOG.md`, 735 of `planning/`, 1,885 of `docs/` and the
other pages), and the external identities.

## N1A, start to finish

In a worktree of `main` with S4 merged, with `<before>` a record of that `main` (S4's own record of its result is one: `baseline.py
record` over an MSVC build with every option on and no `--target`) and `<work>` a directory outside the tree. The script arrives in a
commit of its own just before the pass that uses it, so the parent of a scripted commit holds the script that made it.

    python tools/n1b/n1b_programs.py --root . --dry-run                                           # 47 moves, 671 files and 3 pages
    python tools/n1b/n1b_programs.py --root . --phase all --report <work>/programs-report.txt --json <work>/programs-table.json   # 34 s
    git commit -m "N1A: move the files named for a program"      # nothing added: the staged renames alone, 47 files, all R100
    git add -A && git commit -m "N1A: the programs take their own names ..."                       # 674 files, 3,974 lines each way
    python tools/n1b/n1b_reflow.py --root . --base HEAD~1                                         # 27 lines in 14 files
    git add -A && git commit -m "N1A: wrap the lines the program-names pass pushed past 100 columns"
    (by hand: below)
    cmake --build <tree> --target forge-gui_lupdate hearth_lupdate crucible_lupdate               # 836, 832 and 385 texts, 0 new
    python tools/generators/gen_pseudo_locale.py
    git add -A && git commit -m "N1A: lupdate and the pseudo-locale generator on the renamed sources"   # 28 lines in 7 files

Each of the three scripted commits is what its script gives on its parent: a scratch repository made from `git -c core.autocrlf=false
archive` of the script commit, with the script run in it, has the trees of the three commits (`git write-tree` after the moves, after
the text and after the reflow), and a second run of the pass changes no file. `git mv` leaves the directories it emptied and
`check_doc_paths.py` finds them on disk, so the pass removes the four it emptied.

### What `n1b_programs.py` decides

One table, `PROGRAMS` (`--table` prints it as data, `--json` writes it with the moves): for each program the name it takes, the stem it
takes where an underscore joins it to a word, the C++ namespace it becomes where it is one, and the stem of its variables.

| was | is | stem | namespace | variables |
|---|---|---|---|---|
| `ac3cli` | `forge` | `forge` | `forge_cli` | `ICLFORGE_CLI_*` |
| `ac3gui` | `forge-gui` | `forge_gui` | `forge_gui` | `ICLFORGE_GUI_*` (and `AC3_GUI_*`) |
| `ac3hearth` | `hearth` | `hearth` | | `ICLFORGE_HEARTH_*` |
| `ac3hearth-render`, `-testsink`, `-testserver` | `hearth-render`, `hearth-testsink`, `hearth-testserver` | `hearth_...` | | |
| `ac3crucible` | `crucible` | `crucible` | | `ICLFORGE_CRUCIBLE_*` (and `AC3DESK_*`, the desktop demo's) |
| `ac3crucible-run` | `crucible-run` | `crucible_run` | | |
| `ac3tests`, `ac3perf`, `ac3bench`, `ac3membench`, `ac3kernelbench` | `iclforge-tests`, `-perf`, `-bench`, `-membench`, `-kernelbench` | `iclforge_...` | | |
| `ac3probe`, `ac3fuzz`, `ac3test` | `iclforge-probe`, `-fuzz`, `-test` | `iclforge_...` | `iclforge_probe`, `iclforge_fuzz`, `iclforge_test` | |
| `ac3shield` | `shield` | `shield` | `shield` | |
| `ac3nullsink` | `iclforge-nullsink` | `iclforge_nullsink` | `iclforge_nullsink` | |

The place decides where the name alone does not. `ac3cli encode` is `forge encode` and `ac3cli::` is `forge_cli::` (`forge` is a CMake
alias and a sub-namespace elsewhere, so no namespace is a bare program name); a name joined to a word by an underscore takes the stem
(`ac3gui_qmltests` is `forge_gui_qmltests`, `ac3gui_fr.ts` is `forge_gui_fr.ts`, which is the base name the translation loader is given)
and one joined by a hyphen or a dot takes the program's name; the names Qt and CMake make from a target's own spelling are written out
(`forge-gui_lupdate`, `forge-gui_autogen`, `libcrucible_engine`). A program's name that the language needs as an identifier, where
a hyphen is not allowed (a Python parameter or attribute: the `ac3tests` of `tools/sendspin/aiosendspin_exit.py`, which `--ac3tests`
sets), takes the stem form; the source is read by its tokens for that, and a source that parsed before the pass and would not after
it is not written (the run fails and lists it). A scan of the tree before the pass, for a program name in a name position of any other
language, found the C++ namespaces and one macro argument that the preprocessor makes a string, and nothing else.

The QML module URIs (`Ac3Forge` is `ForgeGui`, `Ac3ForgeHearth` is `Hearth`, `Ac3ForgeCrucible` is `Crucible`, and the `Test` and
`Language` modules of each) are read by the build and by every `import`, so they are one decision in the script (`MODULES`). A
translation in a Qt catalogue names what its source names, in whatever word order.

The brand where a program owns it is what `n1b_idents.py` kept for this stage (the `n1a-*` places of its report): the packages named for
a program (`iclforge-crucible`, `iclforge-hearth`, `iclforge-shield`), the family's icons, the Android package `com.iclforge.shield` with
its JNI names and library, the PipeWire node, the organisation the settings are stored under, and the display name by context ("AC3Forge
Hearth" is "Hearth", the family is "ICL Forge", and the GUI window's own words are "Forge"). The man page's title is the command's name
in capitals.

It keeps, and says so (`--report` lists every place): the Windows driver's installed identity (`Ac3ForgeNullSink`: its hardware id,
service, INF, SYS and CAT names, the endpoint's name, the names in the scripts that install it, and the .NET namespace `Ac3Forge` those
scripts compile for themselves); the old names the settings migration reads (`FORMER_NAME_FILES`) and the lines `n1b_idents.py` keeps
on purpose; the pages, `CHANGELOG.md`, `planning/`, `tests/golden` and the released winget manifests; the external identities. Only the
text of the INF's provider and manufacturer, the version resource's description and copyright line, and the notices follow the program
names, since no code reads them and no installed device is matched by them.

### What the pass cannot decide, by hand

- Stored settings move with the names. `forge-gui`, Hearth and Crucible stored them under the organisation `ac3forge` and now store
  them under `iclforge`, so a program started after this stage would find nothing. `apps/gui/settings_migration.{hpp,cpp}` copies, once
  and at start-up, the old application store (read without Qt's fallback to the organisation's keys) and the files under the old
  `QStandardPaths` directories (leaving Qt's own cache) to the new ones, and records it in a key of the new store; it never writes the
  old store or the old files. Crucible's older migration from the desktop demo's store is the same helper's second former store.
  `iclforge-settings-tests` (19 cases, in ctest) runs it over INI files in a temporary directory; a probe against the Windows registry
  (identities that exist only there, removed after) read and wrote every value type, and showed that Qt creates the empty registry key
  of any store it reads, so a machine with no former store gets the old names' keys, empty.
- The Android JNI names are the package written into about forty C++ function names, and a disagreement is an `UnsatisfiedLinkError`
  on a device, in a job CI reaches late. `tools/checks/check_android_jni.py` (17 tests, a step of `_static.yml`) checks that the Gradle
  namespace and `applicationId` are one name, that every Kotlin source declares it and sits in its directory, that every
  `external fun` has a native `Java_...` definition and the reverse, that the library loaded is one the app's CMake builds, and that
  every class path the C++ or the ProGuard rules name is a class the Kotlin declares.
- `apps/windows/README.md` says which text of the driver changed and which did not; `encoder_controller.cpp`'s comment about what
  `main()` sets follows the new names; `baseline.py` and `cli_bytes.py` record the CLI as `forge` and still find `ac3cli` in a tree built
  before this stage, which is what a stage's own before-record is.
- Two lines of `tools/sendspin/aiosendspin_exit.py` and `aiosendspin_group_exit.py` that the pass made longer than ruff allows (a
  docstring's and the flag's `add_argument` call) are wrapped.

### The proof

    python tools/n1b/baseline.py record --build <after> --out <work>/after
    python tools/n1b/baseline.py compare <before> <work>/after

The pinned-hash streams and the exports of every shared library are identical, and so are all 44 commands of the CLI corpus, in exit
code, stdout and every output file. The corpus does not print the program's name, so a second comparison runs the old `ac3cli` and the
new `forge` over those 44 commands and 96 more that do (the usage, the help of every command, the version, an unknown command, every
command with no arguments and a missing input) and compares the bytes, stderr included. The 44 are the same but for the decoder's
progress meter, which a timer writes to stderr and which is there or not in either run; of the 96, 91 differ, and every difference is
the program's name (430 times), the padding the usage aligns its columns with after a name of another length, the family's display
name, or the build's own commit, its count and its branch. The 22 public headers whose blob changed differ in comments that say `ac3cli`.
The ctest names after the stage are the ones before it with the program names renamed by this script's own rules (73), plus the 19 cases
of the settings migration.

What N1A leaves: S5's list (the pages, the changelog and the plans), the wording of what the programs say (U1), the external identities,
and the driver's identity until it is rebuilt and signed.

## S5, start to finish

In a worktree of `main` with N1A merged (the S5 branch is made from N1A's head, so the merge of `main` brings in
nothing) and `<work>` a directory outside the tree. The passes read the tracked text files outside the history
(`CHANGELOG.md`, `planning/`, `tests/golden`, the released winget manifests, `.git-blame-ignore-revs` and this
directory) and write them back in place. Each phase arrives in a commit of its own just before the pass that uses it,
so the parent of a scripted commit holds the script that made it.

    python tools/n1b/n1b_docs.py --root . --phase text --dry-run                                  # 256 files
    python tools/n1b/n1b_docs.py --root . --phase text --report <work>/text-report.txt            # 3,511 lines each way
    git add -A && git commit -m "S5: the pages, and the comments that name C++ and CMake, follow the new names"
    python tools/n1b/n1b_docs.py --root . --phase urls --report <work>/urls-report.txt            # 121 files, 481 lines
    git add -A && git commit -m "S5: every address names the repository iclforge"
    (the third phase arrives in a commit of its own: the words of build and tool text)
    python tools/n1b/n1b_docs.py --root . --phase words --report <work>/words-report.txt          # 20 files, 65 lines
    git add -A && git commit -m "S5: build and tool text names the libraries as it should"
    (by hand: below)
    python tools/n1b/n1b_docs.py --root . --phase all --dry-run                                   # 0 files in each phase

Each of the three scripted commits is what its script gives on its parent: a scratch repository made from
`git -c core.autocrlf=false archive` of the parent, with the phase run there from the scratch tree itself, has the
blobs of the commit (3,048 paths for the first two), and a second run changes nothing. The last command is the same
property of the stage's last commit, and it holds because of `FORMER_NAME_LINES` and `FORMER_NAME_FILES`: what the
hand-written commits say about the past on purpose (60 lines in 12 files, and `docs/renamed.md`) is left. One of those
lines is not new: the driver's .NET namespace in `apps/windows/README.md`, which the text phase changed and which the
owner's decision of 2026-10-01 keeps until N1D renames the driver, so the later version of the script run on the
parent of the text commit leaves that one line as it was.

### What `n1b_docs.py` decides

A page (every tracked `.md`, `mkdocs.yml`, `overrides/`, the docs site's data and scripts under `docs/`, the generated
snippets) takes these rules in this order; every other file that is not C, C++ or Rust takes the first three, and a Rust
file takes them in its comment lines. `--report` lists every decision and `--json` the counts.

| rule | what it renames | places |
|---|---|---:|
| `header` | the include spelling of a header that moved (`HEADER_MAP`: 159 spellings from `layoutdef.py` applied to the tree before S2, each held to a header of the tree by a test) | 260 |
| `cmake-target` | a CMake alias of the old single library (`ac3::forge` is `iclforge::ac3`; the table of `n1b_cmake.py`) | 473 |
| `namespace` | a C++ qualifier (`ac3::oba::X`, `ac4::X`; the rules of `n1b_names.py` without the C++-only ones) | 1,263 |
| `program`, `variable` | the table of `n1b_programs.py` (`ac3cli` is `forge`, `AC3GUI_X` is `ICLFORGE_GUI_X`) | 1,018, 13 |
| `identifier`, `owned` | the brand as an identifier, and what a program owns (the decisions of `n1b_idents.py` and `n1b_programs.py`) | 1,031, 47 |
| `bare-code`, `display-family`, `display-member`, `literal` | the one decision a page adds: the bare word is the identifier `iclforge` in code (a fenced block, a code span, a `<script>`, a data file), "ICL Forge" in prose, and "Hearth" and "Crucible" for the two members; a Kconfig menu title and the Homebrew cask's name are literals | 116, 102, 18, 11 |

A link to a heading whose text changed follows the new anchor, in the history too (a link is not text of the history):
34 pages have a heading with a new slug, and 10 pages link one. Reported and kept: an address (427, the `urls`
phase's), the file name of an asset of a release that exists (5), the output of a past release (2) and the Windows
driver's installed identity (16). `urls` moves the repository (455 places), the Pages site (26), the tap (23), an issue
reference (2), the path a runner makes from the repository's name (4) and a clone directory (1), and keeps the URL of a
release that exists, the winget manifests' directory, the SonarCloud key and the lines the hand-written commit writes.
`words` renames, in the comments of CMake, workflows, shell, Python and the presets, a library called by the name it
had (54: `ac3adm`, `ac3iab`, `libac3iab.so`) and a raw target of the old single library (13: `forge_shared`,
`forge_c_static`), and keeps the six lines that tell a past event in the name of its time (`HISTORICAL_LINES`).

### What the passes cannot decide, by hand

- What is published decides what an install route says, so each route is written for a release up to `v0.10.0-beta.1`
  and for the first release made after the rename (`docs/renamed.md` is the table, and `docs/releasing.md`, the Forge,
  macOS and Python pages, the support catalogue and the Homebrew README say it where it decides a step). A sentence
  that is true only after the owner has renamed the repository, the tap and the project key, or published, is
  listed in the pull request.
- The old single library is 22 libraries: the layout block of README and CONTRIBUTING, the library index, the header
  map and the pkg-config names are written from the tree (`check_pages.py` holds every header spelling and target a page names
  to the tree, and stays).
- Code that names the repository derives it, so that it is right before and after the rename: the two guards test
  `github.repository_owner`, `bump_manifests.py` reads `GITHUB_REPOSITORY`, `tools/hearth/ota.py` asks it and then the
  GitHub CLI, and the pages of the docs site that fetch the quality history read the repository off the address they are
  served under (the site is deployed at every push to `main`). The tap is named by its new name in `manifest-bump.yml`,
  so the tap has to be renamed before the first release.
- The status of the plans (`planning/ac4.md`, `layout.md`, `README.md`), the roadmap row, one changelog entry and a note
  on the three pages that narrate the old names.
- The pass reads a line at a time, so a code span over two lines is cut in two: two such spans were put right by hand.
- Not regenerated: the screenshots that show the old names (the 17 of the Forge GUI, the Shield app's, the demo's); a
  start of Hearth asks for a firewall rule, and the GUI and the Shield need a display and a device.

### The proof

    python tools/n1b/n1b_docs.py --root . --phase all --dry-run          # 0 files in each phase
    python tools/n1b/n1b_docs.py --root . --residual                     # a reason for every place that is left
    python tools/n1b/check_pages.py --root .                             # 0 problems
    python tools/checks/check_doc_paths.py                               # 0 missing of 5,650 paths
    python tools/checks/generate_support_matrices.py --check
    python -m mkdocs build --strict -d <work>/site                       # and a check of the links of the built site
    python tools/ci/precheck.py
    python tools/n1b/baseline.py record --build <msvc tree> --out <work>/after
    python tools/n1b/baseline.py compare <N1A's record> <work>/after     # all four kinds identical

The stage changes pages, comments, addresses and build text, and no code that runs but the address of the repository in
`bump_manifests.py` and `ota.py`, so the proof that nothing built changed is an MSVC build of every default target
(1,431 steps, no `--target`, after a clean) and the whole ctest, and a `baseline.py` record equal to N1A's in the
pinned-hash streams, the exports of every shared library, the public headers and the 44 commands of the CLI corpus. The
corpus does not print the man page, whose `.UR` address is the one text of the CLI this stage changes. Qt's `lupdate`
over the three programs finds 836, 832 and 385 texts, 0 new.

### What S5 leaves

The wording of what the programs and pages say, where it names no name (U1); S6, which nests the AC-3 codec's own
symbols under `iclforge::ac3`; the comments and strings of the C and C++ sources that still say `ac3adm`, `ac3iab` or
`ac3audio`; the Windows driver's identity (N1D); and the owner's steps outside the repository: the repository, the tap
and the SonarCloud project key are renamed, the pending publisher of the PyPI project `iclforge` is created, and the
first release is made under the new names.

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

`check_pages.py` holds every header spelling (`iclforge/base/layout.hpp`) and library target (`iclforge::dsp`) a page
names to the tree, which `check_doc_paths.py` cannot do since neither is a path.

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
the study used to check its pages and its measurements. `adapt_branch.ps1` runs the scripts of S2 and S3 only (`n1b_apply.py`,
`n1b_cmake.py`, `n1b_paths.py`, `n1b_names.py`); the scripts of S4 and N1A (`n1b_idents.py`, `n1b_programs.py`) are run on a branch by
hand, in the order of their sections above, and the commit of each is merged the same way.
