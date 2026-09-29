# The layout of `src/` (N1B): a study

!!! note "Status as of 2026-09-30: a proposal; the pull request that adds it moves no source file"
    Asked for by the user on 2026-09-29 ("it's kind of weird ... the old stuff is over here in forge and
    the new AC4 stuff's over here which is two folders higher, not as a sibling"). This page and its
    appendix, [layout-inventory.md](layout-inventory.md), read the tree as it stands on `main` at
    `4ca84d66d` and propose a layout that puts the codecs side by side over a base that knows none of
    them. The user reads it, picks a layout and answers [the decisions](#i-decisions); a later phase
    runs the moves with the scripts written for this study.

    N1 is two tasks now. **N1A** names the programs and what they register with the system; **N1B**,
    this page, names and lays out the libraries. Decisions already taken: the family is "ICL Forge"
    (identifiers `iclforge`, `ICLFORGE_`), nothing is published so nothing needs a shim, N1B is layout
    and naming only (every output byte stays the same), and the duplicated DSP is a later phase.

## In brief

The recommended layout, L2 below, keeps `src/<library>/` as the unit and makes every codec a sibling.

- `src/forge` becomes `src/ac3` (AC-3 and E-AC-3), and five libraries the codec never owned leave it:
  `base`, `dsp`, `objects`, `render` and `iec61937`. `src/ac4`, `ac4core`, `ac4dec` and `ac4enc` stay
  where they are. TrueHD arrives as `src/truehd` and any later codec the same way.
- One name spells the family everywhere: the header directory `iclforge/<library>/`, the CMake target
  `iclforge::<library>`, the library file `iclforge_<library>` and the C++ namespace `iclforge`. The
  programs keep the plain names the user chose.
- The split costs seven moves of declarations in 37 files, done first and inside today's layout, each
  provable by the existing tests. The include graph between libraries has no cycle today; what blocks
  the split is which header a declaration sits in.
- One CMake function, `iclforge_add_library()`, replaces the 40-odd lines thirteen libraries repeat (57%
  of the per-library CMake), and generates the aliases, export headers and file names.
- A rewrite touches about 2,050 of the 2,916 tracked files whichever layout is chosen (the rename
  reaches nearly every C++ file); the layouts differ by 488, 592 and 1,010 `git mv` operations.
- A prototype of L2 ran the scripts over a scratch worktree. After one build and two fix rounds every
  default target builds with MSVC `/W4 /WX`, static and shared; the whole `ac3tests` runs with no
  failure; 44 of 44 `ac3cli` outputs are byte-identical; the exported symbols are the old set plus one
  function. Seven open branches merge with no conflict once the scripts have run on them, where by hand
  six of the seven conflict in two to ten files: see
  [What the prototype found](#what-the-prototype-found).

## (a) What is there

The tree has 2,916 tracked files: `apps` 695, `src` 590, `tests` 483, `fuzz` 291, `docs` 215, `tools`
197, `esp-idf` 145, and 300 more in `.github`, `cmake`, `examples`, `js`, `rust`, `python`, `packaging`
and the root. `src/` holds about 175,000 lines of C++ in 17 directories:

| directory | lines | what it is |
|---|---:|---|
| `forge` | 61,081 | AC-3 and E-AC-3, Atmos in E-AC-3, DSP, WAV, loudness, layouts and rendering, IEC 61937 |
| `ac4dec`, `ac4enc`, `ac4core`, `ac4` | 21,159; 17,421; 12,538; 3,651 | AC-4 decoder, encoder, shared kernels (private), inspector |
| `audio`, `sendspin` | 17,913; 20,767 | device backends; the Sendspin network protocol |
| `capi`, `mp4`, `mpegts`, `matroska`, `ac3adm`, `ac3iab`, `admbridge`, `iamf`, `signing`, `arithmetic` | 5,619 down to 564 | the rest |

```text
src/
  forge/  include/ac3/{analysis,core,decoder,dsp,emdf,encoder,iec61937,io,meta,oba,quality,render,spatial,verify}/
          src/{analysis,core,decoder,dsp,emdf,encoder,iec61937,internal,io,meta,oba,quality,spatial,verify}/   (+ CMake-selected variant trees)
  ac4/  ac4core/  ac4dec/  ac4enc/          AC-4: four siblings of forge, one level above its code
  audio/  sendspin/  signing/  arithmetic/  admbridge/  capi/  mp4/  mpegts/  matroska/  iamf/  ac3adm/  ac3iab/
tests/  core decoder encoder io meta oba quality verify emdf analysis dsp iec61937 render spatial ...  (forge's, by its inner names)
        ac4core ac4dec ac4enc  audio backend  capi cli containers  gui hearth crucible  golden performance platform ...
```

**The dependency graph, from the `#include` lines.** Every include of 1,300 C/C++ files was resolved as
the compiler resolves it ([appendix B](layout-inventory.md#b-the-include-graph)). Between libraries
there is no cycle:

```text
forge      -> arithmetic                        audio, signing, admbridge, capi -> forge (capi also -> ac4, ac4dec, ac4enc)
ac4core    -> arithmetic                        ac4dec, ac4enc -> ac4, ac4core       (no AC-4 file includes a forge header)
mp4, mpegts, matroska, iamf, ac3adm, ac3iab, sendspin: no include of another project library
admbridge  -> ac3adm, ac3iab, forge
```

`forge` is the one sink: `tests` make 591 include directives into it, `apps/cli` 167, `apps/hearth`
90. Inside `forge` the directories form two cycles: `core`, `internal` and `meta`; and `decoder`,
`encoder`, `io`, `oba` and `verify`. The AC-4 libraries carry their own bit reader, FFT, MDCT, QMF bank
and resampler, as decision 7 of `planning/ac4.md` chose, so the two codec stacks never meet at link time
and the split has no AC-4 side to disturb.

**Names and the rest.** The C++ namespace `ac3` holds the codec, the services (`ac3::audio`,
`ac3::sendspin`, `ac3::render`), the applications (`ac3::hearth`, `ac3::crucible`) and the tests; a
dozen top-level namespaces name a program or a library (`ac3cli`, `ac3gui`, `ac3iab`, `ac3adm`, `ac4`,
`mp4`, `mpegts`, `matroska`, `iamf`, and `ac3forge` in the ESP-IDF component). `tests/` builds one
binary, `ac3tests`, from 293 files through a 1,235-line `CMakeLists.txt`; `esp-idf/ac3forge` stages
`src/forge`, `src/arithmetic` and `cmake` wholesale (`STAGED_TREES` in
`tools/packaging/pack_esp_component.py`). [Appendix A](layout-inventory.md#a-the-libraries-in-src) has
the table for all 17 libraries and [appendix C](layout-inventory.md#c-names) every name.

## (b) What is wrong with it

1. **The codec is hidden two levels down, and the next one would go in the same place.** AC-3 and E-AC-3
   are `src/forge/{include/ac3,src}/<15 directories>`; AC-4 is four siblings. The TrueHD branch,
   `feature/truehd-atmos-support` (23 commits, 1,155 behind `main` on 2026-09-29), adds `ac3::mlp` under
   `src/forge/` because nowhere else exists for it.
2. **Codec-blind code is entangled with the AC-3 vocabulary.** Layouts and rendering, the panner, the
   object scene, IEC 61937 and the audio backends need speaker locations, a downmix target, a block of
   PCM and an object placement. All four are declared inside AC-3 headers. Under the split proposed
   here, 18 include directives in 14 files point from a codec-blind library into the codec
   ([appendix B.4](layout-inventory.md#b-the-include-graph)); the AC-4 side has its own `ac4::Speaker`
   and `ac4::DownmixTarget` for the same reason. A second, larger knot is left alone: `wav`, `loudness`
   and `analysis` take `Acmod` and `SampleRate`, the A/52 syntax enumerations, in their public
   functions.
3. **Cycles and unreadable seams inside `forge`**, listed above. `render/render.hpp` includes the whole
   1,021-line decoder header for one struct.
4. **Names disagree.** Libraries that know no codec carry `ac3` (`ac3audio`, `ac3::sendspin`, `ac3iab`,
   `ac3adm`, `ac3::signing`, `ac3::arithmetic`) while `mp4`, `mpegts`, `matroska`, `iamf` do not. Six
   libraries share the header directory `include/ac3/` and four more use `ac3iab/`, `ac3adm/`,
   `ac3forge_c/` and `ac4dec/`; four alias shapes coexist (`ac3::forge`, `matroska::matroska`,
   `ac4::decoder`, `ac3iab::ac3iab`); installed files are `ac3forge`, `ac3signing`, `admbridge`,
   `ac3adm`, `mp4`, `matroska`, `iamf`, `ac4`. `mp4`, `matroska` and `iamf` are names other packages
   use: Debian, Arch and MSYS2 ship libmatroska with headers under `include/matroska/`.
5. **Headers and implementation live apart, and some have no home.** Public headers sit in
   `include/ac3/<dir>` and their sources in `src/<dir>`. `render` is 2,268 lines of headers with no
   source directory. `ac4core` has no `include/`; its 21 private headers are reached by unqualified
   spellings (`"tables/huffman_tables.hpp"`), 69 directives from `ac4dec` and `ac4enc`, which can
   collide with any other library's `dsp/` or `tables/`.
6. **Tests do not mirror the source.** 25 test directories test `forge`, named for its old inner
   directories (`tests/core` also holds tests of `arithmetic`); `tests/containers` covers three
   libraries. 414 include directives reach a header outside a library's `include/` from another tree,
   and `ac3tests` gets 26 `target_include_directories` calls to allow them.
7. **The CMake is the same text thirteen times.** 583 of 1,014 code lines in the 13 per-library files
   (57%) are identical with the name swapped, and five of them (`mp4`, `mpegts`, `matroska`, `iamf`,
   `ac4`) are 97% that text. `cmake/InstallLibrary.cmake` has 388 code lines in 13 near-identical
   blocks, and `ac3forgeConfig.cmake.in` 92.
8. **Depth.** 18 files in CMake-selected variant trees are 9 to 11 levels deep
   (`src/forge/src/internal/cpu/probe/builtin/ac3/internal/cpu/hardware_avx2.hpp`); everything else in
   `src/` is at most 7.
9. **Two habits make a rename harder than it looks.** 881 uses in 53 files outside `forge` name an AC-3
   sub-namespace without its root (`meta::`, `plan::`, `eac3::`), relying on being inside
   `namespace ac3` (610 of them in `apps`); and `ac3::sendspin::ac3forge` is a nested namespace that
   would shadow an `iclforge` root inside `sendspin`.

## (c) Principles

Drawn from the user's words, the findings above and two published sources: the
[Pitchfork Layout](https://github.com/vector-of-bool/pitchfork/blob/develop/data/spec.bs) (separate
placement puts public headers in `include/<namespace>/…` and sources and private headers in `src/`,
mirroring them; several libraries sit as `libs/<name>/`, each laid out the same way, cannot nest
further, and replace the top-level `src/` and `include/`) and CMake's own documentation
([`cmake-packages(7)`](https://cmake.org/cmake/help/latest/manual/cmake-packages.7.html): exported
targets take a `Namespace::` prefix, and a package configuration file can include a targets file per
component;
[`install(TARGETS … FILE_SET HEADERS)`](https://cmake.org/cmake/help/latest/command/install.html), since
3.23, installs the headers a target declares public where `install(DIRECTORY)` copies a tree).

1. Codecs (AC-3 with E-AC-3, TrueHD/MLP, AC-4) are peers; each has one directory named for the standard.
2. A base that knows no codec sits under them: bit I/O, the speaker vocabulary, the kernels' build
   variants.
3. Containers, object-audio interchange, layouts and rendering, DSP, IEC 61937, network audio and device
   backends are libraries of their own, each with a stated set of dependencies.
4. The dependency direction is data, checked by a script over the include graph, not implied by
   directory nesting.
5. A public header lives under `include/iclforge/<library>/`; everything else is private to `src/`; a
   header another library needs but users should not is `detail/`, and every include that crosses a
   library says `iclforge/<library>/…`.
6. One name follows path, namespace and target: `iclforge/render/layout.hpp` declares into namespace
   `iclforge::render` and belongs to the target `iclforge::render`.
7. Tests mirror the source tree, so a reader finds `tests/<library>/` for `src/<library>/`.
8. A future codec's home is obvious: `src/<name>/`, the same template, `iclforge::<name>` and
   `iclforge/<name>/`.
9. One CMake function makes the shape every library repeats.

L2 follows Pitchfork's separate placement with two differences: a library's `src/` holds its sources
directly, since the library is already the directory the namespace would repeat, and the libraries sit
flat under `src/`, where Pitchfork's form for several libraries is `libs/` (L3).

## (d) Candidate layouts

All three use the family root and the single-function CMake; they differ in how far `forge` is cut and
where tests sit. Numbers are from a dry run over the tracked files
([appendix E](layout-inventory.md#e-the-three-layouts-dry-run)).

### L1: regroup, keep the boundaries

```text
src/  ac3/ (was forge: unsplit)  ac4/ ac4core/ ac4dec/ ac4enc/  adm/ (ac3adm)  iab/ (ac3iab)  admbridge/  arithmetic/
      audio/  capi/  iamf/  matroska/  mp4/  mpegts/  sendspin/  signing/       header root iclforge/<lib>/, target iclforge::<lib>
tests/ as now (tests/core, tests/decoder … keep their forge-era names)      fuzz/, tests/performance/, examples/, docs/library/ as now
apps/  esp-idf/iclforge  python/src/iclforge  rust/iclforge{,-sys}  js/  packaging/*/iclforge: same trees, renamed contents
```

The codec is a sibling of AC-4 and the names agree; render, WAV, IEC 61937 and the object scene stay
inside `ac3`.

- **For:** the smallest change: 488 moves, no header surgery, one stage.
- **Against:** the AC-3 library is still 61,000 lines, 10,800 of them (18%) codec-blind; TrueHD still
  lands inside it; nothing enforces a direction.

### L2: peers over a base, flat (recommended)

```text
src/
  arithmetic/  base/  dsp/  objects/  render/  iec61937/                 codec-blind, in this dependency order
  ac3/                                                                   AC-3 and E-AC-3 (was forge)
  ac4/  ac4core/  ac4dec/  ac4enc/                                       AC-4 (as now, headers re-rooted)
  mp4/  mpegts/  matroska/  iamf/   adm/  iab/  admbridge/               containers; object-audio interchange
  signing/  audio/  sendspin/  capi/                                     services and the C API
  each library:  CMakeLists.txt  include/iclforge/<lib>/…  src/…  variants/<axis>-<choice>/iclforge/<lib>/detail/…
tests/<lib>/…  (tests/ac3/{core,decoder,encoder,io,meta,oba,quality,verify,analysis}, tests/objects, tests/render, tests/audio/backend …)
tests/{cli,gui,hearth,crucible,golden,performance,platform,crt}   as now      fuzz/, examples/, apps/    as now
docs/library/: one page per library, and a layers page with the table below          benchmarks: tests/performance, as now
esp-idf/iclforge  python/src/iclforge  rust/iclforge{,-sys}  js/ (package iclforge-wasm-decoder)  packaging/*/iclforge
```

Target names, namespaces and header roots are in [the naming map](#e-the-naming-map). A codec is
`src/<name>/` with the same template. `render` is header-mostly; `base` holds `bitreader.hpp`,
`bitwriter.hpp`, `layout.hpp` (Location and Layout), `downmix_target.hpp`, the CPU probe and the SIMD,
CPU-probe and profiling variants. `wav`, `loudness`, `analysis`, the E-AC-3 frame layout and the JOC and
Atmos encoder stay in `ac3`. The EMDF container (`emdf.hpp` and `emdf.cpp`, 453 lines that need only the
bit reader and writer) goes to `objects`: the TrueHD branch's 24 `mlp` files include nothing from
outside `mlp` except the bit reader and writer, `oamd.hpp` and that container, so `truehd` links `base`
and `objects` (and `dsp`, through `objects`) and no other codec (decision 14; checked with the layering
script, not built). Each library links only what the table allows, and `check_layering.py` fails a new
edge:

| library | may use (the includes the prototype has after the cuts) |
|---|---|
| `arithmetic`, `base`, `iec61937`, `mp4`, `mpegts`, `matroska`, `iamf`, `adm`, `iab`, `sendspin`, `ac4` | nothing in the project |
| `dsp` | `base` |
| `objects` | `base`, `dsp` |
| `render` | `base`, `objects` |
| `ac3` | `base`, `dsp`, `objects`, `render`, `arithmetic` |
| `ac4core`; `ac4dec`, `ac4enc` | `arithmetic`; `ac4`, `ac4core` |
| `audio` | `base`, `render`, `iec61937`, `objects` |
| `admbridge`; `signing`; `capi` | `adm`, `iab`, `objects`; `ac3`; `ac3`, `ac4`, `ac4dec`, `ac4enc` |

- **For:** codecs beside each other, a base under them, five libraries a second codec can link (10,800
  lines). The cuts are small and provable first. Path depth falls from 11 to 8.
- **Against:** five new build targets, each with a DLL boundary. In a shared build every call across a
  library becomes exported ABI (one function, `has_avx2()`, crosses today and needs its export macro).
  The minimum-footprint profile (`minimal.cmake`) compiles sources from six directories and needs six
  export headers, the ESP-IDF pack's `STAGED_TREES` lists them, and the vocabulary debt of `wav`,
  `loudness` and `analysis` stays.

### L3: Pitchfork `libs/`, tests beside the code

```text
libs/<lib>/  CMakeLists.txt  include/iclforge/<lib>/  src/  tests/            (the same 22 libraries as L2)
tests/  cli/ gui/ hearth/ crucible/ golden/ performance/ platform/            integration tests and shared data
fuzz/, examples/  as now  (or fuzz targets into libs/<lib>/fuzz: 291 more files)         apps/ esp-idf/ …  as L2
```

Same libraries as L2, `src/` renamed `libs/`, each library's tests beside it, one test binary per
library (`ctest -L <lib>`).

- **For:** self-contained libraries and the literal published layout; CI can shard by library.
- **Against:** 1,010 moves (1,311 with the fuzz sources, seeds and regression inputs);
  `tests/CMakeLists.txt` splits into about twenty files; `plan_gate.py`, `sonar-project.properties`,
  `.clang-tidy` and the docs name `src/` as a root; cross-library test helpers need a home; the
  one-binary convenience goes.

| | L1 regroup | L2 peers | L3 libs |
|---|---:|---:|---:|
| files moved (`git mv`) | 488 | 592 | 1,010 |
| other files edited in place | 1,545 | 1,456 | 1,060 |
| C/C++ files with rewritten includes or names | 1,249 | 1,250 | 1,252 |
| files that only name a moved path | 3 | 18 | 31 |
| deepest path under `src/` (`libs/`), before → after | 11 → 8 | 11 → 8 | 11 → 8 |

Of the moves, 201 are the package directories every layout renames (`esp-idf/iclforge` 145, `rust/` 26,
`packaging` 17, `python` 12, `cmake` 1). The rename dominates the edit count: about 2,050 files change
under all three layouts, and the deeper layouts convert edits into moves. The deepest path after is the
flattened variant trees (decision 6); without flattening it stays at 12.

**Recommendation: L2.** It is the smallest change that makes the tree agnostic. L1 leaves the
codec-blind code inside the codec, so TrueHD's arrival puts `mlp` inside a library in which 18% of the
lines know no codec, and the cuts L2 makes first are the ones a second codec in Hearth's player will
need whenever it arrives. L3's extra 418 moves buy self-contained libraries, which the user did not ask
for, at the price of changing every path filter and splitting the test binary while the `ac4dec` and
`ac4enc` tests share helpers. L3 remains open as a later step: L2's `tests/<lib>/` directories move into
`libs/<lib>/tests/` with a scripted `git mv`. Whether to group libraries under `codecs/`, `formats/` and
so on is [decision 3](#i-decisions): nesting encodes a tree and the dependencies are a graph, so a flat
directory plus a checked table carries more information.

## (e) The naming map

The family root is `iclforge` in every spelling that is an identifier. The C++ namespace has three
candidates:

```cpp
// today
ac3::render::OutputLayout room = ac3::render::OutputLayout::parse("L R C LFE Ls Rs").value();
ac3::FrameEncoder encoder{config};        ac4::Decoder decoder{ac4_config};        auto wav = ac3::io::read_wav(path);
// iclforge::   (recommended)
iclforge::render::OutputLayout room = iclforge::render::OutputLayout::parse("L R C LFE Ls Rs").value();
iclforge::ac3::FrameEncoder encoder{config};   iclforge::ac4::Decoder decoder{ac4_config};   auto wav = iclforge::ac3::io::read_wav(path);
// forge::        forge::render::OutputLayout …   forge::ac3::FrameEncoder   forge::ac4::Decoder   forge::ac3::io::read_wav
// icl::          icl::render::OutputLayout …     icl::ac3::FrameEncoder     icl::ac4::Decoder     icl::ac3::io::read_wav
```

`iclforge::` is the same word as the package, the header directory, the macros and the C prefix, which
is what makes the names follow each other. It is five characters longer than `ac3`: replacing every
`ac3::` pushes at most 657 lines in 190 files past the 100-column limit
([appendix C.5](layout-inventory.md#c-names)), which a formatting pass on the changed lines absorbs.
`icl::` keeps every column in place (0 lines) and means something only to a reader who knows the
organisation. `forge::` costs 192 lines and matches the programs, but it is a common word: the family
chose `iclforge` for the registries, and C++ has no registry, yet headers, a CMake package and macros in
a consumer's tree share one flat space. Codecs nest beneath as peers: `iclforge::ac3`, `iclforge::ac4`,
`iclforge::truehd`; the services sit beside them (`iclforge::render`, `iclforge::audio`).

| name | today (count) | recommended | GitHub-side or wider cost |
|---|---|---|---|
| repository | `iainchesworthlabs/ac3forge` (130 files, 458 hits; 50 files outside docs) | `iainchesworthlabs/iclforge` | old URLs redirect, including clones, until the name is reused |
| docs site | `iainchesworthlabs.github.io/ac3forge` (14 files) | `…/iclforge` | Pages URLs are not redirected: the old address ends |
| release tags, assets | `v0.2.0-beta.1` to `v0.10.0-beta.1`; 127 assets on the latest, all `ac3forge-*` | tags stay; new assets `iclforge-*` | old assets stay as published; `SHA512SUMS` and the signing key file name change from the next release |
| in-repo packaging | Homebrew formula, cask, winget (4 versions), vcpkg port, Conan | `iclforge` identifiers; the four historic winget versions stay | nothing is on a registry |
| C++ namespace | `ac3::` (14,205 uses, 933 files); `namespace ac3` in 534 files; `ac4::` 4,318; `mp4::` and kin 1,125 | `iclforge::`, codecs nested | see [stage S3](#f-the-migration-plan) |
| header root | `ac3/` (2,501 includes, 673 files) and ten more roots | `iclforge/<lib>/`; C API `iclforge_c/iclforge.h` | every consumer and example |
| C API | 472 `ac3forge_*` names, 248 `AC3FORGE_*` macros in C and C++, `libac3forge_c` | `iclforge_*`, `ICLFORGE_*`, `libiclforge_c` | Rust bindgen allowlist, Python, WASM exports |
| CMake package | `find_package(ac3forge)` (26 files), 11 export sets | `iclforge` with one component per library | vcpkg and Conan features map 1:1 |
| CMake targets | 37 distinct `ac3::*` names in CMake | `iclforge::<lib>`, `_static`, `_shared` | one function generates them |
| options | 31 `option()`, 67 cache variables, 166 distinct `AC3FORGE_*` names in build files | `ICLFORGE_BUILD_<LIB>`, `ICLFORGE_<X>` | CI, presets, docs, every packaging file |
| Kconfig | 69 distinct `CONFIG_AC3FORGE_*` (47 files) | `CONFIG_ICLFORGE_*` | ESP-IDF sdkconfig defaults |
| library files | `ac3forge`, `ac3signing`, `mp4`, `matroska`, `ac4` … | `iclforge_<lib>`, `iclforge_<lib>_static` | pkg-config names `iclforge-<lib>` |
| Python, Rust, npm | `ac3forge` (23 files); crates `ac3forge`, `ac3forge-sys`; `ac3forge-wasm-decoder` | `iclforge`, `iclforge-sys`, `iclforge-wasm-decoder` | none: unpublished |
| environment | 14 variables (`AC3FORGE_SIMD_TIER`, `_SIGNING_KEY`, …) | `ICLFORGE_*` | scripts and docs |
| wire and format strings | `_ac3forge_player@v1` (65 files), `ac3forge_hearth_sink` (25), `ac3forge.probe/1`, `ac3forge.hearth.media/1`, `ac3forge_scene` | `_iclforge_player@v1` and so on | both ends of Sendspin and the OTA check change in one stage; Hearth's stored settings are N1A |

Two hazards the scripts must respect. A nested `iclforge` namespace inside `iclforge::sendspin` would
shadow the root, so `sendspin::ac3forge` becomes `sendspin::player`. And a partially qualified name
resolves through the enclosing `ac3`, so nesting AC-3 under `iclforge::ac3` needs a scope-aware rewrite
(stage S6), while the root rename (S3) needs none.

## (f) The migration plan

Each stage is one pull request, scripted from a fresh `main`, and its scripts are idempotent: a script
skips a move whose target exists and rewrites only what still carries the old name, so a second run
changes nothing (checked on the prototype) and a run after another pull request merged gives the same
result plus that pull request's files. The scripts (`layoutdef.py`, `n1b_apply.py`, `n1b_cmake.py`,
`n1b_names.py`, `cuts.py` and the census and check scripts) live in `D:\ac3bld\n1b\tools`; stage S0
lands them under `tools/n1b/`.

| stage | what | files (L2) | proof beyond the common set |
|---|---|---:|---|
| S0 | scripts, `tools/checks/check_layering.py` (the dependency table as data), baselines of hashes, symbols, headers and CLI bytes | 12 new | scripts re-run on `main` reproduce the baselines |
| S1 | seven cuts (C1 `Location`/`Layout` to `core/layout.hpp`; C2 `DownmixTarget`; C3 `PcmBlock`/`BlockSink`; C4 `joc::Domain`; C5 `ObjectPlacement`; C6 `blocks_per_syncframe`; C7 `serving.hpp`), as three or four small pull requests in today's layout | 37 | `check_layering.py` finds no violation |
| S2 | freeze begins. `git mv` of `src/` and `tests/` (commit 1); include spellings, the mechanical CMake pass and the hand-written CMake for the split (commits 2 and 3) | 396 moved; 877 C/C++ and 91 build files edited | exported symbols per library equal the baseline; ESP-IDF pack `--verify`; coverage floors for the six components set from one run |
| S3 | `ac3` to `iclforge` in namespaces, the libraries nested; sub-namespaces keep their names | 1,190 | symbol diff shows only the namespace prefix |
| S4 | identifiers: C API, macros, options, Kconfig, environment, file names, packages, wire strings; package directory renames (201 moves) | 930 carry a name, plus 201 moves | Python, Rust and WASM tests; ABI allowlists regenerated |
| S5 | docs, CI filters and planners, sonar, generators, `.git-blame-ignore-revs`; repository rename | 340 name a moved path; 128 docs carry a name | `check_doc_paths.py`, `mkdocs build --strict`, the full CI matrix on the branch |
| N1A | program names and registrations, one pass after S4 (same freeze) | 603 carry a program name | as N1A states |
| S6 | AC-3 nested under `iclforge::ac3` (about 480 files); the vocabulary of `wav`, `loudness`, `analysis` (about 50 more) | about 530 | the compiler names each unqualified use; same builds |

Scripts exist for S1 (`cuts.py`), for the `src/` part of S2 (`n1b_apply.py`, `n1b_cmake.py`) and for
S3's root rename (`n1b_names.py`), all run in the prototype. The 104 test moves and the 201 package
moves are defined in `layoutdef.py` and counted, not run. S4, S5 and S6 have their censuses and
checklists ([appendix C and D](layout-inventory.md)) and no script yet; each stage's pull request adds
its own before it runs.

**Common proof** for S1 to S6: MSVC and clang-cl builds of every default target (`cmake --build` with no
`--target`, since a default target that no test builds can break `main`), the WSL GCC 16 and Clang 22
`-Werror` gates, the whole `ac3tests`, the encoder's pinned hashes (`check_cross_platform_hash.py`
against `tests/golden/bitstream-hashes.json`, unchanged), and the bytes of a fixed CLI corpus before and
after (encode and decode with each codec over `tests/golden/audio/*`, `probe` JSON, each container)
compared per compiler. The heavy legs run only after a merge, so each stage branch is dispatched to the
full matrix first (`gh workflow run ci.yml --ref <branch>`; `workflow_dispatch` runs every leg) and
`main-health` is watched before the next stage.

**History.** The repository merges every pull request with a merge commit (the queue's `merge_method` is
`MERGE`; the last 25 merges on `main` are all two-parent), so the commits inside a stage survive. Commit
1 of S2 is `git mv` alone: in the prototype all 292 renames are `R100`, `git log --follow` reaches the
pre-move history and the second commit does not disturb it. Squash would fold the rename and the edit
together; the combined diff of S2 still detected all 292 renames (lowest similarity 85%), but S3's edits
are larger. Keep merge commits, and list each rewrite commit in `.git-blame-ignore-revs` so blame skips
it (GitHub's blame view reads that file).

**Freeze.** About 26 merges a day reach `main` on average (`docs/ci-agentic.md`); the nine days to
2026-09-29 ran from 2 to 150 a day, so the freeze starts after the current wave of AC-4 phase branches
has merged. The freeze holds S2, S3, S4, N1A and S5, an estimate of two to three days: each stage's full
matrix takes hours on the runner fleet, and the stages are sequential. Seven days out: announce, and
label every open pull request `n1b-wait` (it merges after the freeze and adapts) or `n1b-first` (it
merges before). Three days out: those that can merge do. At T-0 the queue is empty and only these pull
requests merge, each opened from the newest `main`. If another lands in between, merge `main` into the
stage branch and re-run the script (never rebase, never force-push). Thaw after S5. S1 and S6 sit
outside the freeze.

**Open branches.** Merging a stage by hand puts a developer's edits against a tree where every include,
namespace and path has changed. Running the stage's own scripts on the branch first, and merging
afterwards, leaves only lines the branch itself changed that a hand-written commit also changed. For
each stage, in order:

1. Merge the `main` that precedes the stage (the branch must contain it exactly, or the merge that
   follows reverts what it lacks).
2. Run the stage's scripts on the branch and commit (`tools/n1b/adapt_branch`, from S0, does steps 2 to
   4).
3. `git merge -s ours <the stage's last scripted commit>`: this records the scripted commits as merged
   and keeps the branch's tree.
4. `git merge main` at the stage's last commit. The merge base is now the scripts' own output, so the
   hand-written commits that follow it (the CMake of the split, the fixes) are the only changes the
   branch has to reconcile.

This needs three things of the stages: their commits stay in `main`'s history (decision 10: merge
commits), each stage's scripted commits come before its hand-written ones, and a scripted commit is
exactly what the script gives on its parent (a check in the stage's pull request re-runs the script and
compares trees).

Measured on the prototype, counting the files `git merge-tree` reports as conflicted (branch tips as
they stood on 2026-09-30; the prototype's S2 and S3 scripts and its hand-written CMake stand for the
stages):

| branch (tip) | commits | files it touches | of those, rewritten by the scripts | conflicts, merged by hand | conflicts, scripts first |
|---|---:|---:|---:|---:|---:|
| `feature/ac4dec-d14a-diet` (`b2767d1df`) | 3 | 39 | 38 | 10 | 0 |
| `feature/ac4-d14b-p4` (`e3841daaa`) | 10 | 35 | 22 | 8 | 0 |
| `feature/ac4-i4b-object-encoder-bindings` (`1d8e2b833`) | 7 | 34 | 10 | 7 | 0 |
| `feature/ac4-i5b-gui-objects` (`f8ac5e285`) | 9 | 53 | 24 | 9 | 0 |
| `feature/ac4enc-e10-aspx-high-band` (`1e253bc60`) | 5 | 9 | 3 | 0 | 0 |
| `bugfix/ac4-slow-tests-under-sanitizers` (`79b30fd69`) | 1 | 3 | 2 | 2 | 0 |
| `bugfix/ac4-decode-qml-suite-teardown-crash` (`d57ae11b8`) | 1 | 21 | 10 | 6 | 0 |

*Merged by hand* is the branch tip against the prototype's tip. *Scripts first* follows the four steps,
with the scripts' own output (the prototype's scripts run on `main` and nothing else) as the merge base,
which `--merge-base` reproduces without touching the branch. Every hand conflict is a line the rewrite
changed, in a hunk the branch also changed or edited beside: the include lists of `ac4core` and `ac4dec`
sources (`d14a`) and of the CLI and GUI (`i5b`), the target names in a CMake block the branch
restructured (`d14b`), the `ac3::` qualifiers on lines of the GUI's controllers (the teardown fix), the
C API and its bindings (`i4b`). None of the seven changes a line the hand-written CMake also rewrote,
which is why none conflicts with the scripts first. As a control, a branch whose only change is one
comment line of `src/ac3/CMakeLists.txt`, a line the hand-written commit rewrote, conflicts in that file
with the scripts first, and in 14 files merged by hand. The merged trees were not built.

`feature/truehd-atmos-support` (23 commits, 1,155 behind `main`) is measured by path only, since a merge
of `main` into it is large whatever this study does. Of its files, 28 sit under `src/forge` (24 of them
`mlp`), 8 in `apps/gui`, 5 in `apps/cli` and 12 in `docs` (mostly reference PDFs). A rule added to
`layoutdef.py` when the branch lands would send `src/forge/{include/ac3,src}/mlp` to
`src/truehd/{include/iclforge/truehd,src}`; the branch needs `main` merged in whichever layout is
chosen.

**Buildable at every stage.** S1 changes no path. Within S2 only its last commit builds: the `git mv`
commit has broken includes by design, and CI evaluates the pull request head. S3 and S4 change text
only, so each commit builds. Where a stage needs hand-written CMake (the five new libraries and the
helper in S2) the prototype's files are the starting point.

**N1A and N1B.** They edit the same files: 509 of the 2,067 text files that carry either kind of name
carry both (appendix C.4), and `apps/*/CMakeLists.txt` names library targets (N1B) and defines program
targets (N1A). Neither task moves an `apps/` directory (`cli`, `gui`, `hearth` and `crucible` are
already neutral names); N1A renames the program targets and their output names, N1B the library targets
they link. N1B goes first because it moves files and the rename detection wants the least other change
around them; N1A follows inside the same freeze; the docs are swept once, in S5, for both.

## (g) Everything keyed on a path or a name

The checklist, with every file name, is [appendix D](layout-inventory.md#d-everything-keyed-on-a-path).
By category, the 340 files that name a directory L2 moves: `tests`, `fuzz`, `examples` 73; `src` 53;
CMake and Kconfig 49; docs 40; `apps` 24; `tools/checks` 22; `esp-idf` 18; `planning` and history 18;
other tools 15; workflows and actions 11; bindings 6; `tools/ci` 5; root config 5; packaging 1. The ones
a reader would look for:

What a reader might not expect:

- **CMake**: the root `add_subdirectory` list, the install and package files
  (`cmake/InstallLibrary.cmake`, `cmake/ac3forgeConfig.cmake.in`, `cmake/Packaging.cmake`),
  `CMakePresets.json`, and the 26 `target_include_directories` calls of `tests/CMakeLists.txt`.
- **CI**: `tools/ci/plan_gate.py` classifies by path prefix and treats an unknown top-level directory as
  needing Qt, which is why L3's `libs/` costs more; `ci.yml`'s `docs_re` names no source path; six
  workflows and `.github/actions/build-leg/action.yml` name `src/forge`. The coverage gate keeps a line
  and a branch floor per `src/<dir>` (`tools/checks/coverage_report.sh`); `forge`'s 90 and 82 do not
  split into six floors, so S2 includes one coverage run to set them.
- **Configuration**: `sonar-project.properties` lists 45 `src/` paths; `.clang-tidy`'s
  `HeaderFilterRegex`, `.gitattributes` and `vcpkg.json` name paths. There is no CODEOWNERS file and no
  labeler configuration, and `dependabot.yml` names only trees that do not move.
- **Scripts and bindings**: the table generators write into `src/forge/include/ac3/core`; the ESP-IDF
  pack copies whole trees (`STAGED_TREES`, `PRUNE` in `tools/packaging/pack_esp_component.py`);
  `rust/iclforge-sys/build.rs` names the bindgen header and allowlist.
- **Docs**: `docs/library/header-map.md` and a page per library. `check_doc_paths.py` checks prose
  paths, so `layout.md` and its appendix need an entry in its `PROSE_PATHS_UNCHECKED`, as the other
  plans have, or every proposed path would fail it.

## (h) Proof per stage

| proof | S1 | S2 | S3 | S4 | S5 | S6 |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| MSVC `/W4 /WX`, all default targets | x | x | x | x | | x |
| clang-cl over the changed units | x | x | x | x | | x |
| WSL GCC 16 and Clang 22 `-Werror` gates | x | x | x | x | | x |
| whole `ac3tests`, `[cli][ac4]` | x | x | x | x | | x |
| pinned bitstream hashes unchanged | x | x | x | x | | x |
| CLI bytes on the fixed corpus | x | x | x | x | | x |
| exported symbols per library against the baseline | | x | x | x | | x |
| ESP-IDF pack `--verify` (about half an hour) | | x | x | x | | |
| Python, Rust, WASM tests on the branch (`ci.yml` dispatch) | | | | x | x | |
| `check_doc_paths.py`, `mkdocs build --strict`, `precheck.py` | | | | | x | |
| `check_layering.py` finds no violation | x | x | x | x | | x |

## (i) Decisions

1. **Which layout.** (a) **L2** (recommended): the goal met, 592 moves. (b) L1: 488 moves, the
   codec-blind code stays inside `ac3`. (c) L3: 1,010 moves, tests beside the code. Cost of (a) over
   (b): seven cuts and five library targets.
2. **The namespace root.** (a) **`iclforge`** (recommended): one word everywhere, 657 lines to reformat.
   (b) `forge`: matches the programs, shares a common word. (c) `icl`: no reformat, opaque.
3. **Grouping.** (a) **flat** `src/<lib>` (recommended): 22 directories, one rule for path, target and
   namespace. (b) `src/{base,codecs,formats,services}/<lib>`: easier to browse; every path is one level
   longer and the table of dependencies still has to be kept.
4. **Tests.** (a) **mirror `src/` in `tests/<lib>/`, one binary** (recommended, in S2): a test file goes
   to the library whose headers it mostly includes, so `tests/core`, `tests/io`, `tests/oba`,
   `tests/emdf` and `tests/containers`, which hold tests of more than one library, are split file by
   file. (b) beside the code, per-library binaries (L3, later).
5. **The vocabulary debt.** (a) **leave `wav`, `loudness` and `analysis` in `ac3` and record it**
   (recommended): their public functions take `Acmod` and `SampleRate`, so extracting them changes their
   API. (b) extract now with a neutral layout type and an integer rate: an API change in about 50 files.
6. **Variant directories.** (a) **flatten** to `variants/<axis>-<choice>/` (recommended): depth 11 to 8,
   56 include spellings that change anyway. (b) keep the shape.
7. **AC-4's header directories.** (a) **one per library** (`iclforge/ac4dec/decoder.hpp`, namespace
   `iclforge::ac4`; recommended: each library generates its own export header). (b) one `iclforge/ac4/`
   for all four, with distinct export header names.
8. **The C API header root.** (a) **`iclforge_c/`** (recommended, as now). (b) `iclforge/c/`.
9. **Order with N1A.** (a) **N1B, then N1A, one freeze** (recommended). (b) N1A first. (c) separate
   freezes, which doubles the disruption for 509 shared files.
10. **Merge method.** (a) **keep merge commits** (recommended). (b) squash, which loses the mv-only
    commit's guarantee.
11. **The repository name and the Pages URL.** (a) **rename the repository to `iclforge` after S5**
    (recommended); the docs address changes and does not redirect. (b) keep the repository name: the
    docs address survives; two names live side by side. (c) rename and take a custom domain first.
12. **The TrueHD branch.** Name its directory `src/truehd` (recommended: the codec's name, not the
    container family's `mlp`) or `src/mlp` (the branch's name).
13. **Internal programs** (`ac3tests`, the benchmarks, the probe): rename in N1A (recommended,
    `iclforge-tests` and so on) or keep.
14. **The EMDF container.** (a) **`emdf.hpp` and `emdf.cpp` to `objects`** (recommended): TrueHD needs
    it and it needs nothing of AC-3; no new edge for the layering check and no extra move, since every
    file of `forge` moves anyway. (b) Leave it in `ac3`: `truehd` then links `ac3` for it.

## (j) What stays out, and follow-on ideas

- **The duplicated DSP.** FFT, MDCT, QMF and resampler exist in `src/forge/src/core`,
  `src/forge/src/dsp` and `src/ac4core/src/dsp`; `iclforge::dsp` is where a unified version lands, in
  its own phase with its own numerical proof. Moving the files does not merge them.
- **A codec-blind channel vocabulary in `wav`, `loudness` and `analysis`** (decision 5).
- **Splitting `ac3` the way AC-4 is split** (core, decoder, encoder). The directories of `forge` form
  two cycles ([appendix B.3](layout-inventory.md#b-the-include-graph)) and `verify` includes both the
  decoder and the encoder, so it is a study of its own.
- **Per-library test binaries and co-located fuzz targets** (L3), once the dependency table is enforced.
- **A codec-scoped C API** (`iclforge_ac3_decoder_*`): S4 renames the prefix only.
- **Installing by file set**: `install(TARGETS … FILE_SET HEADERS)` in place of
  `install(DIRECTORY include/)`, so a `detail/` header stays out of the package.
- **The ESP-IDF pack from a manifest** of libraries instead of `STAGED_TREES`.

## What the prototype found

To find what the scripts and the plan get wrong before anyone runs them on `main`, L2 was applied to a
scratch worktree (`proto/n1b-layout`, branched from `b07dec6f3`, never pushed) and built with MSVC
14.51, `/W4 /WX`, Ninja, `-j 6`. Since then `main` has changed nothing under `src/`, `tests/`, `cmake/`
or `esp-idf/`. The same worktree served for the branch measurements below. The commits, each made by a
script unless marked:

| commit | what | files | lines |
|---|---|---:|---:|
| cuts (S1) | the seven declaration moves (`cuts.py`) | 37 | +302 −226 |
| moves (S2) | `git mv` alone: 292 renames, every one `R100` | 292 | 0 |
| includes (S2) | every project `#include` spelled by its new header root; export macros per library | 879 | 3,366 |
| CMake, mechanical (S2) | targets, output names, moved paths and directories | 91 | 1,308 |
| names (S3) and CMake by hand | `namespace ac3` to `iclforge` in 1,190 C++ files; the build files of the split (by hand) | 1,217 | +20,809 −20,931 |
| fix round 1 (by hand) | see below | 13 | +31 −21 |
| fix round 2 (a script rule, one hand edit) | see below | 21 | +37 −36 |

The scripts take under two minutes together on the shared machine (the namespace pass 48 s, the CMake
pass 12 s, the plan 1 s). The moves commit contains nothing else, and history follows it:
`git log --follow` on the moved `decoder.cpp` lists the 61 commits it lists on `main` under the old
path, plus the prototype's three.

**Builds.**

- *First build* (every default target, static and shared: the libraries, `ac3cli`, the C API): 82 steps
  failed in 2.2 minutes. The causes, by size: the namespace script's pattern crossed a line break in six
  files (`apps/cli/support.cpp` alone gave 75 errors); 36 source files of `ac4core`, `ac4dec`, `ac4enc`
  and the C API missed the new include roots the hand-written CMake had not yet given them; nine
  `::ac3::` names were skipped; and `iclforge_base.lib` did not exist, because `has_avx2()`, the one
  function that crosses a library boundary, had no export macro. Fix round 1 repaired the scripts and
  the hand-written files, and the second build finished all 254 steps in 6.8 minutes.
- *`ac3tests`*: seven units failed. Six included a private header through a directory the CMake pass had
  not renamed (it renamed file paths, not the directory in a `target_include_directories`; the plan
  phase had already listed them among nine private-header reaches). The seventh used
  `eac3::chanmap::Location` through an include the cut removed. Fix round 2 gave the CMake pass a
  directory rule derived from the file moves and pointed that test at `iclforge::base::Location`. Seven
  directories whose files went to more than one library are listed for review, and three of them
  (`src/forge/src/internal`, `src/forge/src/oba`, `src/forge/include/ac3/oba`) are left for a person.
  The rule also rewrote four GUI, Hearth and Crucible CMake files that this build (Qt off) could not
  have caught. `ac3tests` linked (231 steps, 4.2 minutes).

**Behaviour.**

- **The test suite.** The whole of `ac3tests` ran serially on the prototype (`ctest -j 1`, 12.6
  minutes). Of 2,418 registered tests, 2,382 passed, 8 skipped themselves on conditions they check for
  (a full disk and a symlink that Windows cannot make, streams chosen by environment variables that were
  not set) and none failed; the other 28 could not run because their programs (27 examples and
  `ac3perf`) were not built.
- **CLI bytes.** A corpus of 44 `ac3cli` commands (encode with each codec and layout, decode of the
  golden AC-3, E-AC-3 and AC-4 streams, `probe` JSON, `levels`, `loudness`, `qc`, each container, `cut`,
  `metadata`, `normalize`, `transcode`, `fmp4`) ran on the baseline twice and on the prototype: exit
  codes, the SHA-256 of every output file and of normalised standard output are identical in all 44
  (`cli_bytes.py --compare`).
- **Exported symbols.** `dumpbin /exports` of the old `ac3forge.dll` gives 729 names; the union of the
  six DLLs it became gives 730, the extra one being `has_avx2()`. The exports of every other library are
  unchanged. The old DLL was 1,254,912 bytes; the six total 1,546,752 (+23%), each with its own headers,
  section padding and export table.
- **Layering.** After the cuts `assign.py violations` (the prototype of `check_layering.py`) finds none,
  and the include edges in the prototype's `src/` are the ones the table lists; the first draft of the
  table allowed two that do not exist (`ac3` to `iec61937`, `iec61937` to `base`).
- **Idempotence.** Each script was run again on the finished prototype. The namespace and CMake passes
  changed nothing. The move plan wanted to move 170 public headers a second time, into
  `include/iclforge/iclforge/…`, because `layoutdef.py` did not recognise a header already under the new
  root. A guard fixed that: the plan now gives 0 moves and 0 include rewrites there, and the same 287
  moves and 3,008 rewrites on `main`. (`cuts.py` is idempotent only on the old layout, which is where S1
  runs.)

**Open branches.** Seven branches were measured against the prototype's tip, counting the files
`git merge-tree` reports as conflicted; the method, the results and one control are with
[the migration plan](#f-the-migration-plan). By hand, two to ten files per branch conflict, and none for
`feature/ac4enc-e10-aspx-high-band`; with the scripts run on the branch first, none does. The merged
trees were not built.

**What the prototype did not cover.** Each is unproven, and the plan's stage proofs cover it:

- the 104 mirrored test files and the 201 package moves were not moved (the tests build from their old
  paths), and stage S4 (identifiers) and S5 (docs, CI filters) were not run;
- the other twelve libraries still carry their own boilerplate `CMakeLists.txt`:
  `iclforge_add_library()` produced the six libraries of the split (the old 750-line
  `src/forge/CMakeLists.txt` became 256 + 140 + 18 + 20 + 14 + 13 lines and a 139-line helper) but `mp4`
  is still 99 lines, and `cmake/InstallLibrary.cmake` still has its thirteen blocks;
- `minimal.cmake` (the whole of AC-3 compiled as one minimum-footprint library) was renamed but not
  recomposed from six directories, and the ESP-IDF pack, `--verify` and `STAGED_TREES` were not run;
- GUI, Hearth and Crucible (Qt off), the examples, `ac3perf`, the fuzz targets, the Python, Rust and
  WASM bindings, and clang-cl, GCC 16 and Clang 22 were not built; the pinned-hash gate
  (`check_cross_platform_hash.py`, which needs the gold-reference script's outputs) was not run, and the
  CLI corpus stands in for it; the CI matrix was not dispatched.
