# Recasting ac3forge as a family: the library, Forge and Crucible

!!! note "Status: plan, written and decided 2026-09-05"
    This page plans the recasting of the repository from one name over everything into three
    named members: **the library**, **Forge** (the `ac3cli` and `ac3gui` tooling) and
    **Crucible** ([the Crucible guide](../crucible/index.md)). It keeps the shape of
    [the promotion plan](../crucible/promotion.md): the design sections say what changes and
    why, each phase carries an exit criterion and says how it is verified, and
    [Decisions](#decisions) lists what only the owner could decide, with the option taken. All
    fifteen were decided on 2026-09-05, each as recommended; Phase 1 started the same day.

Today `ac3forge` names everything at once. It is the repository, the site, `project(ac3forge)`
(`CMakeLists.txt:8`), the CPack package, the PyPI project, the vcpkg port, the Conan recipe, the
winget identifier, the Homebrew formula and its tap; it is the library (`ac3::forge`,
`libac3forge`, `find_package(ac3forge)`, 215 C functions prefixed `ac3forge_`); and it is the
tooling's product name, because the GUI sets `setApplicationName("ac3forge")`
(`apps/gui/main.cpp:515`), titles its window `ac3forge — <source>` and heads its About box
`ac3forge`, and `ac3cli` opens its usage text with `ac3forge — clean-room AC-3 / E-AC-3`
(`apps/cli/usage.cpp:760`). The word appears in 331 files. Crucible, named on 2026-09-04, is the
one member with a name of its own applied consistently: display "Crucible", binary
`ac3crucible`, namespace `ac3::crucible`, option `AC3FORGE_BUILD_CRUCIBLE`, package
`ac3forge-crucible`. "Forge" as a word on its own appears nowhere in the tree.

This page names the members, says what each owns, and lists the cheapest way to make the family
visible without touching an identifier anyone has installed.

## The model

Three members, one family name, and a shared floor.

**The family is `ac3forge`.** The repository, the site, the CMake project, the CPack name, the
`v*` tag stream, the SBOM and signing-key names, the GitHub slug (91 files). None of it changes.

**The library** keeps the family's identifiers, because every published thing already carries
them: `ac3::forge`, `find_package(ac3forge)`, `ac3forge_c/ac3forge.h`, `import ac3forge`,
`libac3forge0`. It is `src/forge` and its siblings, the three bindings, the examples, the fuzz
harnesses, the conformance vectors and the footprint probe.

**Forge** is the tooling over the library: `ac3cli`, `ac3gui` and the `apps/common` sources
they share. The tree already treats the pair as one thing in every generator and registry
(CPack component `runtime`; one `ac3forge` archive, installer, DEB, RPM, `.dmg` and winget
entry; docs/cli beside docs/gui). Forge is a display and docs name for that pair. Its binaries,
packages and paths stay as they are through 0.x.

**Crucible** is `apps/crucible`, `tests/crucible`, `docs/crucible`, the `crucible` component and
its packages, and by purpose the Windows null-sink driver in `apps/windows/driver`, which stays
under its own name until attestation signing lands.

**The shared floor** belongs to the family and to no member: `src/audio`, which is never
installed (`cmake/InstallLibrary.cmake:4-7`) and is linked by `ac3cli`, `ac3gui`,
`ac3crucible_engine` and the Shield JNI; `tools/`; the single `ac3tests` binary; CI; the
packaging tooling; the version line.

**Beside the family, joining no member:** the Shield app under `apps/android`, documented as
the library's Android demo, and the browser pages under `apps/wasm`, documented as the library's
live demos, with the npm package listed among the library's bindings.

## What each member owns

| | The library | Forge | Crucible |
|---|---|---|---|
| Source | `src/forge`, `src/signing`, `src/capi`, `src/matroska`, `src/mp4`, `src/mpegts`, `src/ac3iab`, `src/iamf`, `src/ac3adm`, `src/admbridge`, `src/ac4` (built, never exported); `python/`, `js/`, `rust/`; `examples/`, `fuzz/`, `apps/baremetal` | `apps/cli`, `apps/gui`, `apps/common` | `apps/crucible`; by purpose `apps/windows/driver` and `driver-vm` |
| Build identity | `ac3::forge`, `ac3::forge_c`, `ac3::signing`, `matroska::matroska`, `mp4::mp4`, `mpegts::mpegts`, `ac3iab::ac3iab`, `iamf::iamf`, `ac3adm::ac3adm`, `ac3::admbridge`; ten export sets and ten `.pc` files | targets `ac3cli`, `ac3gui`; QML URI `Ac3Forge`; options `AC3FORGE_BUILD_CLI`, `AC3FORGE_BUILD_GUI` | targets `ac3crucible`, `ac3crucible-run`, `ac3::crucible_engine`; QML URI `Ac3ForgeCrucible`; option `AC3FORGE_BUILD_CRUCIBLE`; root guard `WIN32 OR (UNIX AND NOT APPLE)` (`CMakeLists.txt:450`) |
| Tests and checks | most of `ac3tests`; `tests/capi`, `python/tests`, `apps/wasm/tests`; coverage floors `src/*` (`tools/checks/coverage_report.sh:108-116`); abi-gate; fuzz.yml; interop.yml | `tests/cli`, `tests/gui`, `ac3gui_qmltests` (label `gui`); floor `apps/cli` (:117); `.clang-tidy:95` | `tests/crucible` (compiled into `ac3tests`, `tests/CMakeLists.txt:436-456`), `ac3crucible_qmltests`; labels `crucible`, `crucible-ui`; `tools/ci/check_crucible_package.py`; `tools/checks/coverage_crucible.ps1`, `crucible_platform_probe.cpp` |
| Docs | Library (22 pages), Concepts (4), Validation, Threat model, Conformance vectors, Performance & quality (6), `platforms/wasm.md`, the two WASM demo pages | CLI reference (3), GUI guide (12) | Crucible guide (5), `platforms/windows-demo.md` (the record), `platforms/windows-driver-acx.md` |
| Packages | `ac3forge-dev-<full>-<sys>`; DEB `libac3forge0`, `libac3forge-dev`; RPM `libac3forge0`, `ac3forge-devel`; PyPI `ac3forge` (live); npm `ac3forge-wasm-decoder` (unpublished); crates `ac3forge`, `ac3forge-sys` (unpublished); vcpkg port and Conan recipe `ac3forge` (staged); `ac3forge-conformance-vectors-<ver>.tar.gz` | component `runtime`: `ac3forge-<M.m.p>-<sys>` zip/tgz, NSIS `.exe`, DEB/RPM `ac3forge`, `.dmg`; `ac3gui-*.AppImage`; winget `iainchesworthlabs.ac3forge` (4 versions staged); Homebrew formula `ac3forge` and cask `ac3gui` (live tap) | component `crucible`: `ac3forge-crucible-<full>-<sys>` zip/tgz, DEB/RPM `ac3forge-crucible` (no tag contains it yet) |
| CI | all 11 matrix legs; `build-rust`, `build-wasm`, `build-footprint`; wheels.yml, npm.yml | legs with `gui: true` (Linux GCC/LLVM, both arm64, both macOS; Windows always); `linux-appimage`; ffmpeg-validate builds `ac3cli` | `crucible: true` on windows-msvc, windows-llvm, linux-llvm (`_build.yml:329,342,430`); `windows-driver` |
| Settings and ids | none | QSettings `ac3forge`/`ac3forge`; bundle id `com.iainchesworthlabs.ac3gui`; ProgID `AC3Forge.Stream` | QSettings `ac3forge`/`Crucible` (migrated from `DesktopAtmos`, `apps/crucible/ui/main.cpp:38-60`); no bundle id yet |

Three things do not sit in one column and the plan says where they go.

- **`src/audio`** stays private and is named, in the family docs, as the shared platform layer.
  Crucible includes `src/audio/src/backend/pipewire` directly
  (`apps/crucible/CMakeLists.txt:81-82`); promoting those helpers to `ac3/audio/` headers is a
  follow-up outside this plan, and exporting the library is not proposed (it would put WASAPI,
  ALSA and PipeWire dependencies into a package that declares none).
- **The Shield app** keeps `com.ac3forge.shield` and its `.apk`; its display name is
  [decision 10](#decisions).
- **`apps/wasm` and `js/`** are library documentation and a library binding
  ([decision 11](#decisions)); nothing moves.

## The name

Two facts constrain the naming. The word "forge" already means the library in every
consumer-facing identifier (`ac3::forge`, `src/forge`, `forgeTargets.cmake`, `ac3::forge_c`,
`ac3::forge_minimal`, `libac3forge`, `ac3forge.pc`, and 23 docs pages that say `ac3::forge`).
And the family's spelling differs by surface: `ac3forge` in 5,016 sites, `AC3Forge` only inside
"AC3Forge Crucible" (53 sites) and the NSIS ProgID `AC3Forge.Stream`, `Ac3Forge` in the QML
URIs, the driver and the npm exports.

| Scheme | Family | Library | Forge | Crucible | Binaries | Packages | Cost |
|---|---|---|---|---|---|---|---|
| **S1, recommended** | `ac3forge` in identifiers, "AC3Forge" in prose | "the ac3forge library"; `ac3::forge` unchanged | the `ac3cli` + `ac3gui` pair; "Forge" in docs, nav, README, the About heading and the CLI banner, with the family named beneath | as now | unchanged | unchanged | about ten display sites, `lupdate`, recaptured screenshots, one sentence on two index pages; the word "forge" keeps two meanings |
| S2, per-member package tokens | as S1 | as S1 | runtime archive `forge-<ver>-<sys>`, DEB/RPM `forge`, winget `iainchesworthlabs.Forge`, formula `forge` | `crucible-*`, DEB/RPM `crucible` | unchanged | renamed | deprecation paths for the NSIS installer, DEB/RPM `ac3forge`, the formula token and the winget identifier; availability checks for two generic words in seven namespaces; three tools' hardcoded asset patterns |
| S3, Forge-branded binaries | as S1 | as S1 | `ac3forge`/`ac3forge-gui`, or `forge`/`forge-gui` | `crucible` for symmetry | renamed | as S1 or S2 | `ac3cli` in 252 files, four shells' completions, the man page, four winget manifest versions, the formula; `ac3forge.exe` beside `ac3forge.dll`/`.pdb` from `src/forge/CMakeLists.txt:622`; every user's script |
| S4, rename the library's identifiers | as S1 | `ac3::codec` or similar | Forge is unambiguous | as now | unchanged | `libac3forge0` and the `-dev` packages renamed | the C ABI, the ABI allowlists, every package config and registry, 683 files using `ac3::`, 245 using `AC3FORGE_`; a breaking release |
| S5, a new umbrella name | new | `ac3forge` becomes a member name | Forge | Crucible | any | every published id | 331 files; the repository, Pages URL, PyPI trusted publisher, tap name and every verify snippet key on the literal `ac3forge` |

Under S1 three rules are written down once, on the Library index page, the Forge index page and
in CONTRIBUTING.md:

1. `ac3forge` and `ac3::forge` name the library and the family's identifiers. **Forge**,
   capitalised and standing alone, names the tooling.
2. "AC3Forge" is the family in prose; every identifier stays lowercase. "AC3Forge Crucible" and
   "Forge" are the member names; "AC3Forge Forge" is never written.
3. `ac3cli --version` keeps printing `ac3forge <version>` (`src/forge/src/version.cpp:19-21`):
   it is the library's version line, and the live Homebrew formula's test asserts it
   (`packaging/homebrew/Formula/ac3forge.rb:65`).

The tooling's namespaces are the one code-level inconsistency worth folding while here:
`ac3cli` and `ac3cli::commands` (38 declarations), `ac3::cli::platform` (3), `ac3gui` (2) and
`ac3::apps` against Crucible's `ac3::crucible`. A fold to `ac3::cli`, `ac3::gui` and `ac3::apps`
is internal, touches no consumer, and is optional ([Phase 4](#phase-4-display-strings-and-in-tree-identities)).

## Directory layout

Recommended: nothing moves. The tree already separates the members at the directory level
(`src/`, `apps/cli` + `apps/gui` + `apps/common`, `apps/crucible`), and the places that would
say the family are prose: the README's layout block, CONTRIBUTING's layout rule, the configure
summary.

| Path | Today | After (recommended) | The move that was considered, and what it breaks |
|---|---|---|---|
| `src/forge`, `src/*` | the library | unchanged | renaming to `src/ac3` frees the word for the tooling at the cost of 24 CMake files, wheels.yml and interop.yml triggers, `.clang-tidy:95`, the coverage floors, `cmake/InstallLibrary.cmake`'s header install paths, and 385 commits of directory history |
| `apps/cli`, `apps/gui`, `apps/common` | Forge | unchanged | `apps/forge/{cli,gui,common}` breaks `CMakeLists.txt:437-441`, `.clang-tidy:95`, the `apps/cli` floor row, interop.yml's path filter, 14 `apps/gui` references in `apps/crucible/CMakeLists.txt` (icons, fonts, four QML files, `ac3gui.rc.in`, `system_theme`), `cmake/Packaging.cmake:58-59`, 34 workflow path lines, 17 tool and cmake files, 61 doc path mentions, and `git log -- apps/cli` (296 commits) |
| `apps/crucible` | Crucible | unchanged | none proposed |
| `apps/windows/driver`, `driver-vm` | Crucible's driver, frozen | unchanged until signing; optionally `apps/crucible/driver` in the signing-time change | breaks the `windows-driver` job's paths, `apps/crucible/CMakeLists.txt:372-376`, driver-vm's relative paths, and the signing session's checkout |
| `apps/android` | the Shield demo | unchanged | a `demos/` parent breaks the depth-sensitive `add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")` (`apps/android/app/src/main/cpp/CMakeLists.txt:66`), the Android and CodeQL job paths |
| `apps/wasm`, `js/` | library demos and binding | unchanged | a `web/` parent breaks docs.yml's and npm.yml's path triggers, `build-wasm`'s working directory, `js/package.json:23` `directory: "js"`, and the relative loads inside the checked-in `docs/assets/wasm-*-demo` bundles |
| `python/`, `rust/` | bindings | unchanged | `pyproject.toml:37` `cmake.source-dir = ".."` and `:77` `root = ".."`, wheels.yml `package-dir`, `rust/ac3forge-sys/build.rs:9-13` (asserts it lives two directories below the root) |
| `tests/` | one `ac3tests` | unchanged | a per-member binary duplicates `tests/CMakeLists.txt`'s backend, ADM and IAB conditionals for no gate that does not already key on source paths |
| `docs/` | 73 pages | 73 pages plus `docs/forge/index.md`, `docs/family/recasting.md` (this page), and, under [decision 8](#decisions), `docs/library/capabilities.md` and a `docs/security.md` wrapper | see [The docs](#the-docs) |

What does change on disk is small: the README's layout block gains member headings and the rows
it is missing today (`src/capi`, `src/ac4`, `src/ac3iab`, `src/iamf`, `src/admbridge`,
`apps/wasm`, `apps/common`, `apps/baremetal`, `apps/windows`, `python/`, `js/`, `rust/`), and
`CONTRIBUTING.md:50-51`'s consumer list `apps/{cli,gui,wasm,android}` names the tree as it is.

## Packaging and release identities

Under the recommended scheme every token below is unchanged. The table records what each is,
which member owns it, and what a rename would need, so the cost of any later scheme is on the
page rather than discovered.

| Identity | Member | Where | State | Under S2 | Deprecation path if ever renamed |
|---|---|---|---|---|---|
| `project(ac3forge)`, `CPACK_PACKAGE_NAME`, `find_package(ac3forge)` at `lib/cmake/ac3forge`, `ac3::` namespace, ten export sets, ten `.pc` files | family / library | `CMakeLists.txt:8`; `cmake/Packaging.cmake:24`; `cmake/InstallLibrary.cmake:425-522` | shipped; consumed by the vcpkg port (`portfile.cmake:56`), Conan (`conanfile.py`), 18 files | unchanged | a compatibility config shim; source-compatibility break |
| `libac3forge.so`/`ac3forge.dll`/`ac3forge_static`, `ac3forge_c`, `ac3signing`; `AC3FORGE_EXPORT`; 215 `ac3forge_*` C symbols | library | `src/forge/CMakeLists.txt:622-623`; `src/capi` | shipped; abi-gate allowlists keyed per basename; `rust/ac3forge-sys` `links = "ac3forge_c"` | unchanged | an ABI break, a new allowlist set, a breaking release |
| PyPI `ac3forge` | library | `python/pyproject.toml:6`; wheels.yml publishes on `v*` through environment `pypi` | live | unchanged | a new project plus a stub release under the old name; never |
| npm `ac3forge-wasm-decoder` | library | `js/package.json:2`; publish gated to `workflow_dispatch` (npm.yml:86) | unpublished | unchanged | free until first publish; the name is reserved on first publish |
| crates `ac3forge`, `ac3forge-sys` | library | `rust/Cargo.toml` | unpublished | unchanged | free; `links` follows the C library's name |
| vcpkg port `ac3forge`, Conan `ac3forge` | library | `packaging/vcpkg-port/ac3forge`, `packaging/conan` | staged, pending upstream | unchanged | a port rename is a new port |
| `ac3forge-dev-<full>-<sys>`; DEB `libac3forge0`, `libac3forge-dev` (`Depends libac3forge0 (= version)`); RPM `libac3forge0`, `ac3forge-devel` | library | `cmake/Packaging.cmake:165-166,212,231-233,365` | shipped | unchanged | `Replaces`/`Conflicts`/`Provides` (RPM `Obsoletes`) |
| component `runtime`: `ac3forge-<M.m.p>-<sys>` zip/tgz; NSIS `.exe`, install dir `ac3forge`, ProgID `AC3Forge.Stream` for `.ac3`/`.ec3`; DEB/RPM `ac3forge`; `.dmg` volume `ac3forge` | Forge | `cmake/Packaging.cmake:32,52,81-94,164,227,357-360` | shipped since the first release | `forge-<ver>-<sys>`, DEB/RPM `forge` | NSIS: the new installer must uninstall the old key or users get two installs; DEB/RPM: `Replaces`/`Conflicts`/`Provides`; three tools' patterns (manifest-bump.yml:116-117, `tools/release/bump_manifests.py:146,285,295`, `Casks/ac3gui.rb:54`) |
| winget `iainchesworthlabs.ac3forge`, Moniker `ac3forge`, aliases `ac3cli`/`ac3gui` | Forge | `packaging/winget/manifests/i/iainchesworthlabs/ac3forge/` (0.8.0-beta.1, 0.8.0-beta.2, 0.9.0-beta.1, 0.10.0-beta.1) | staged; submission blocked on DR6 | `iainchesworthlabs.Forge` | a new identifier is a new package upstream; staged version directories are never rewritten (docs/releasing.md); `check_packaging_versions.sh:41-47` and `bump_manifests.py:278` address the path by literal |
| Homebrew formula `ac3forge` (class `Ac3forge`), cask `ac3gui`, tap `iainchesworthlabs/homebrew-ac3forge` | Forge | `packaging/homebrew/` | live | formula `forge` | `tap_migrations.json` or a deprecated alias; users re-tap if the tap name changes |
| `ac3gui-<ver>-x86_64.AppImage` | Forge | `_build.yml:2436` | shipped | follows the binary | free |
| `ac3forge-crucible-<full>-<sys>`; DEB/RPM `ac3forge-crucible` (`Depends pipewire, wireplumber \| pipewire-media-session`); `ac3crucible.desktop`; AppStream id `ac3crucible.desktop`; hicolor `ac3crucible.png` | Crucible | `cmake/Packaging.cmake:174-182,228-230,363-364`; `apps/crucible/packaging/linux` | no tag contains it | `crucible-*`, DEB/RPM `crucible` | free until the first Crucible tag; a bare `crucible` needs a namespace check in Debian and Fedora first |
| settings stores `ac3forge`/`ac3forge` (GUI) and `ac3forge`/`Crucible` | Forge / Crucible | `apps/gui/main.cpp:515-516`; `apps/crucible/ui/main.cpp:88-89` | user data | unchanged | a migration like `migrate_demo_settings()` (`apps/crucible/ui/main.cpp:38-60`); the driver-vm scripts read `HKCU\Software\ac3forge\Crucible` |
| bundle ids `com.iainchesworthlabs.ac3gui`; Crucible none | Forge / Crucible | `apps/gui/CMakeLists.txt:336`; `apps/crucible/CMakeLists.txt:265-269` | installed | unchanged; Crucible gains `com.iainchesworthlabs.ac3crucible` | LaunchServices identity |
| Android `com.ac3forge.shield`, `ac3forge-shield-<ver>.apk`, `app_name` "Shield Atmos Demo", `versionName "0.3.0-beta.1"` | the Shield demo | `apps/android/app/build.gradle.kts:33,41,48`; `strings.xml:3` | sideloaded | ids unchanged; display name is [decision 10](#decisions) | an applicationId change is a new app on the device |
| driver `Ac3ForgeNullSink` (`.sln`, `.inx`, `.sys`, `.cat`, service, `ROOT\Ac3ForgeNullSink`); INF strings "Desktop Atmos"; artifact `ac3forge-nullsink-driver-testsigned` | Crucible | `apps/windows/driver`; `_build.yml:3111` | test-signed; frozen | strings change at signing time, ids kept | frozen after attestation is paid for |
| release-wide `ac3forge-<bare>.spdx.json`, `ac3forge-signing-key.asc`, `ac3forge-conformance-vectors-<ver>.tar.gz`, `SHA512SUMS`, `gh attestation verify --repo iainchesworthlabs/ac3forge` | family | `release.yml:370,394`; `_build.yml:1325`; docs/releasing.md | shipped | unchanged | the verify snippets users may have saved |

Three release-shape facts are open and the first tag that contains Crucible freezes them
([decision 13](#decisions)):

1. **The Linux Crucible package is a run artifact, not a release asset.** The linux-llvm leg
   uploads it as `ac3forge-crucible-linux-<preset>` (`_build.yml:1507`), outside the
   `packages-*` pattern release.yml downloads (`release.yml:279`); docs/releasing.md:578-581
   records this as the next step. A `packages-crucible-<preset>` name is the one-line fix, and
   the `.deb`, `.tar.gz` globs in release.yml's signing and attestation steps already cover it.
2. **Two version-string styles in one release.** The runtime archive, `.exe` and `.dmg` carry
   `M.m.p` (`cmake/Packaging.cmake:357-360`; the cask parses it, `Casks/ac3gui.rb:45-54`); the
   `crucible` and `dev` archives carry `PROJECT_VERSION_FULL` with the prerelease suffix
   (:363-365).
3. **docs/releasing.md:644 and :785 say no leg is `experimental: true`**, while
   `windows-msvc-arm64` is both `experimental: true` and `release_package: true`
   (`_build.yml:388-393`). One of them is wrong.

A fourth, cosmetic and already noted in `cmake/Packaging.cmake:183-191`: every DEB component's
one-line synopsis is the library's `PROJECT_DESCRIPTION`, so `apt show ac3forge-crucible` opens
with "Clean-room AC-3 encoder". CPack offers no per-component override that takes effect; the
family docs say so and this plan does not fight it.

## The docs

The site has 73 pages under 12 tabs (`mkdocs.yml:65-147`). Crucible is absent from the home page,
the quick start and the concepts overview; `README.md:64-68` still places it in `apps/windows/`
and links the historical record instead of the guide. The library owns most of the tree; the
tools own two tabs; Crucible owns the one it gained on 2026-09-05.

Two constraints decide the shape. `mkdocs build --strict` (docs.yml) validates internal links
and nothing outside `docs/`, and the tree already carries two stale sets from earlier moves
(CHANGELOG.md:1519,1801,1901,1996 link `docs/project/history.md`;
`tools/checks/verify_gold_reference.sh:7,29,187` cite `docs/RESEARCH.md`). And `mkdocs.yml`
declares no plugins, so there is no redirects plugin, while published metadata pins today's URLs:
`js/package.json:19` (`platforms/wasm/`), `python/pyproject.toml:26` (`library/python-api/`),
`ac3crucible.metainfo.xml` (`crucible/`), and four winget manifests
(`docs/releasing.md#what-gets-published`).

So the nav regroups and the files stay. Material's `navigation.tabs` renders each top-level
entry as a tab and `navigation.indexes` lets a section open on an `index.md` that is its first
child; `library/index.md` and `crucible/index.md` exist, and Forge gains one.

Before: Home · Getting started (quickstart, building, 8 platform pages) · Concepts (4) ·
Validation · Threat model · Conformance vectors · Library (22) · CLI reference (3) · GUI guide
(12) · Crucible guide (5) · Performance & quality (6) · Project (7).

After, seven tabs:

```yaml
nav:
  - Home: index.md                       # the family: three members, one paragraph and one link each
  - Getting started:
      - Quick start: quickstart.md
      - Building from source: building.md
  - Library:
      - Conventions: library/index.md
      - Capabilities: library/capabilities.md      # decision 8(b): the tables from index.md
      - Concepts:                                  # concepts/ (4 pages), unchanged paths
      - Using the library:                         # library/ (19 pages), unchanged paths
      - Bindings:
          - C API: library/c-api.md
          - Python: library/python-api.md
          - Rust: library/rust-api.md
          - WebAssembly (npm): platforms/wasm.md   # relabelled, not moved
      - Validation:
          - How output is checked: verification.md
          - Threat model: threat-model.md
          - Conformance vectors: conformance-vectors.md
      - Performance & quality:                     # the 6 pages, unchanged
      - Live demos:
          - Decode in the browser: wasm-demo.md
          - Encode in the browser: wasm-encode-demo.md
  - Forge:
      - What it is: forge/index.md                 # new: the pair, and how to get it
      - CLI reference:                             # cli/ (3), unchanged paths
      - GUI guide:                                 # gui/ (12), unchanged paths
  - Crucible:
      - What it is: crucible/index.md
      - Install & first run: crucible/install.md
      - The signal path: crucible/signal-path.md
      - Troubleshooting: crucible/troubleshooting.md
      - Promotion plan & record: crucible/promotion.md
      - The Windows demo, the record: platforms/windows-demo.md      # relabelled, not moved
      - The null-sink driver on ACX: platforms/windows-driver-acx.md # relabelled, not moved
  - Platforms:
      - Windows: platforms/windows.md
      - Linux: platforms/linux.md
      - Raspberry Pi: platforms/raspberry-pi.md
      - macOS: platforms/macos.md
      - Android, the Shield demo: platforms/android.md
  - Project:
      - Contributing: contributing.md
      - Releasing: releasing.md
      - Security policy: security.md               # new wrapper over SECURITY.md, optional
      - History: history.md
      - Roadmap: roadmap.md
      - The family recasting: family/recasting.md  # this page
      - Self-hosted CI runners: ci-self-hosted-runners.md
```

`docs/forge/index.md` is the page that says what Forge is and how to get it: the three install
paths lifted from `docs/cli/index.md:22-45` (winget, Homebrew, the release archives), the
sentence about the word "forge", and links into the CLI reference and GUI guide.
`docs/gui/index.md:8` then points at it instead of `../cli/index.md#installing`.

`README.md` leads with the family: the H1, one sentence saying the project is one clean-room
AC-3/E-AC-3/Atmos codec and two applications built on it, then a three-row table (Library: what
it is, how to get it, where its docs are; Forge; Crucible), then the trademark and status
paragraphs unchanged, an "Also in the tree" line for the Shield demo and the browser demos, and
the existing Building, Validation (labelled as the library's numbers), Repository layout
(grouped by member), Documentation and Licence sections. The two authority statements that
contradict each other (`README.md:37` says the docs are current; `docs/history.md:5-7` says the
README is right) resolve to what `CONTRIBUTING.md:261-266` already says: `docs/index.md` and
`docs/verification.md` own the capability claims.

Cross-mentions that read as one product are reworded, without moving: `docs/platforms/linux.md:120-128`,
`docs/platforms/windows.md:100-102`, `docs/gui/localisation.md:49-57` (which still says "The
Windows demo (`apps/windows/`)"), `docs/library/muxing-and-sinks.md:698` (which links
"AC3Forge Crucible" to the record instead of the guide), and the "Where to go next" list at
`docs/index.md:254-265`, which gains Crucible.

## The roadmap

`ROADMAP.md`'s nine themes are library themes except UX and DR, which hold Forge, Crucible, WASM
and Shield items under one code each. IDs are load-bearing: 389 citations in
src/apps/tests/tools/cmake/.github, 99 in docs, 11 in CHANGELOG.md, and the header (lines 6-8)
promises they are stable. So the regrouping is a tag, not a renumbering:

- The Overview table (lines 13-25) gains a Member column, and the UX row is recounted: it reads
  9/0/1 while the section holds UX12 under In progress (line 2389) and UX7 and UX10 under
  Considering.
- Each UX, DR, IM and AP item's first line carries its member, in the form
  `**UX12 (XL, Crucible)**`; the seven codec themes are tagged once at the theme heading.
- Future Crucible items take a new code (`CR1`...) so the Overview can count them; UX11 and UX12
  stay where they are with a one-line pointer.
- DR stays the shared distribution theme with per-item tags.
- The two DR8 record links at lines 2631-2632 (`platforms/macos.md#...`,
  `releasing.md#what-gets-published`) are docs-relative and 404 on GitHub; the rule from the
  dual-context fix (inline code, or an absolute `https://github.com/.../blob/main/docs/...` URL,
  as line 2271 does) is applied to them and written into `docs/roadmap.md`'s wrapper so the next
  author sees it.

## CI

Seventeen workflow files; `_build.yml` is the reusable matrix (11 legs) plus eight standalone
jobs; `ci.yml` aggregates 22 jobs behind the required check `CI Status`
(`.github/branch-protection.md:27-31`, alongside `Branch Name` and `Scan dependency diff`).

| Leg or job | Member exercised | Flags |
|---|---|---|
| windows-msvc | library, Forge, Crucible | `packageable`, `release_package`, `crucible` |
| windows-llvm | library, Forge, Crucible | `packageable`, `crucible` |
| windows-msvc-arm64 | library, Forge (CLI only) | `experimental`, `packageable`, `release_package` |
| linux-gcc, linux-gcc-arm64 | library, Forge | `gui`, `packageable`, `release_package` |
| linux-llvm | library, Forge, Crucible (the only PipeWire pass) | `gui`, `packageable`, `crucible` |
| linux-llvm-arm64 | library, Forge | `gui`, `packageable` |
| linux-llvm-asan-ubsan, linux-llvm-tsan | library | |
| macos-llvm, macos-llvm-x64 | library, Forge | `gui`, `packageable` |
| package-macos-universal, linux-appimage | Forge | |
| build-android | the Shield demo | |
| build-wasm, build-rust, build-footprint | library | |
| windows-driver | Crucible's driver | |
| wheels.yml, npm.yml, fuzz.yml, interop.yml, abi-gate, coverage | library (coverage also gates `apps/cli`) | |

Under the recommended scheme no leg is added, renamed or reflagged. The job names that are
branch-protection contracts, the `packages-*` artifact prefix and the ctest labels `gui`,
`crucible`, `crucible-ui` and `Performance` are left alone. Two CI facts shape the phases:
`ci.yml`'s `changes` job classifies a PR as docs-only by the regex at line 311 (`docs/`, any
`*.md`, `mkdocs.yml`, docs.yml), so a phase that touches only prose runs the docs strict build
and skips the matrix; a phase that touches a CMake help string or a display string runs all
eleven legs. And docs.yml's deploy job rebuilds and byte-compares the WASM demo bundles
(lines 80-108), so the nav regroup must not touch `docs/assets/`.

## Phases

Each ends with something that can be checked and says how. Phases 1 and 2 are the same under
every scheme and can start before a single decision is taken. Phase 3 onward depends on the
decisions named in each.

### Phase 1: the stale facts and Crucible's loose ends

One rename already happened and left the tree saying two things. This phase makes the current
state true everywhere before the family is drawn over it.

- `README.md:64-68`: Crucible is under `apps/crucible/`, on Windows and Linux, documented in
  `docs/crucible/index.md`. The layout block (`README.md:162-189`) says the driver and its guest
  live under `apps/crucible`; they live under `apps/windows`. The Documentation table row 206
  links the record; it links the guide.
- `CONTRIBUTING.md:50-51`: `apps/{cli,gui,wasm,android}` names a tree that also holds
  `crucible`, `common`, `baremetal` and `windows`.
- `CMakeLists.txt:138`: the `AC3FORGE_BUILD_CRUCIBLE` help string says "Windows only".
- `tests/CMakeLists.txt:416-424`: the comment says `apps/windows/engine/` and "Windows-only".
- `tools/checks/coverage_crucible.ps1:1-6` and `CMakePresets.json:280`: both say `apps/windows`.
- `docs/gui/localisation.md:49-57` and `docs/library/muxing-and-sinks.md:698` as above.
- `ROADMAP.md:13-25`: the UX row; lines 2631-2632: the two relative links.
- The 50 `AC3DESK_*` CMake variables in `apps/crucible/CMakeLists.txt`,
  `ui/crucible_controller.{cpp,hpp}` and `ui/tests/CMakeLists.txt` become `AC3CRUCIBLE_*`;
  `apps/crucible/ui/tests/qml_test_main.cpp:38` isolates under `DesktopAtmos` and should isolate
  under `Crucible`.
- `apps/crucible/ui/qml/AboutDialog.qml:58` still says "An ac3forge demonstration"; the
  product's wording replaces it. `ac3crucible.desktop:4` `GenericName=Desktop Atmos Mixer` uses
  the mark as a generic name; `Application audio mixer` or similar. The six
  `ac3crucible_*.ts` files still carry `Desktop Atmos` as source text (13 in `ac3crucible_de.ts`):
  `cmake --build <dir> --target ac3crucible_lupdate` regenerates them.
- `docs/releasing.md:644,785` against `_build.yml:388-393`: the experimental claim.
- The four CHANGELOG links to `docs/project/history.md` and the three `docs/RESEARCH.md` comments
  in `verify_gold_reference.sh`.

What stays: `null_sink_substring = "Desktop Atmos"` (`engine.hpp:33`, `output_stage.hpp:52`),
the driver's name in the About licence line, and every mention inside the driver subtree, the
CHANGELOG, ROADMAP records, `promotion.md` and `windows-demo.md`. They change in
[Phase 7](#phase-7-the-driver-at-signing-time).

**Exit:** a grep for `AC3DESK` under `apps/`, `tools/`, `cmake/` and `tests/` returns nothing;
"Desktop Atmos" outside the records and the driver appears only as the endpoint-match string,
its tests and fakes, and the driver's own name; the README, CONTRIBUTING, the help string, the
comments and the coverage header describe the tree as it is.

**Verified by:** `cmake --preset config-windows-msvc -DAC3FORGE_BUILD_CRUCIBLE=ON`, build, and
`ctest --preset test-windows-msvc -L crucible` plus `-L crucible-ui`; the same on Linux with the
command `docs/crucible/install.md:77-81` gives (`config-linux-gcc`, `-DAC3FORGE_WITH_ALSA=OFF
-DAC3FORGE_WITH_PIPEWIRE=ON`); `mkdocs build --strict`; the greps above; the `.ts` diff shows
only source-string changes.

### Phase 2: a path-literal check

Every earlier move left a stale path somewhere `--strict` cannot see. Before any page or nav
moves, a small check makes that class of drift visible: `tools/checks/check_doc_paths.py`
resolves every markdown link target under `docs/`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md`
and `CHANGELOG.md`, and every `docs/...md`, `apps/...`, `src/...`, `tools/...` path literal in
`.github/workflows/*.yml`, `cmake/*.cmake`, `CMakeLists.txt`, `CMakePresets.json` and
`tools/**`, against the tree. Inline code inside ROADMAP records is out of its scope on purpose:
those are history. It joins `ci.yml`'s `script-lint` job.

**Exit:** the check is green on `main` after Phase 1's fixes.

**Verified by:** the check's own run in CI; and once, on a scratch branch, a deliberately broken
link turns it red.

### Phase 3: the family statement

Depends on decisions 1, 3, 7, 8, 10 and 11. Lands the README first screen, the `docs/index.md`
landing page (and `docs/library/capabilities.md` under 8(b)), the seven-tab nav,
`docs/forge/index.md`, this page in the nav, the CONTRIBUTING layout rule, the `security.md`
wrapper if wanted, the reworded cross-mentions, the three naming rules on the Library and Forge
index pages, and the Shield and WASM placements. No file under `docs/` moves and no URL changes.

**Exit:** a reader arriving at the home page, the README or any tab sees three members and can
reach each member's guide and its download in one link; the phrase "AC3Forge Forge" appears
nowhere; `docs/index.md`, `quickstart.md` and `concepts/index.md` each name Crucible at least
once.

**Verified by:** `mkdocs build --strict`; the Phase 2 check; `mkdocs serve` and a walk of all
seven tabs; the README rendered on GitHub; `grep -rn "AC3Forge Forge"` returns nothing; docs.yml's
bundle comparison still passes (nothing under `docs/assets/` changed).

### Phase 4: display strings and in-tree identities

Depends on decisions 2, 3, 4 and 12. Lands the About heading and CLI usage banner (`usage.cpp:760`;
the man page `.TH` line at :793 keeps `ac3forge` as the version source), the `.desktop`
`Comment` lines, the H1 casing under decision 3, the root option help strings and the configure
summary grouped by member (`CMakeLists.txt:499-503` prints CLI/GUI/tests/examples/Python and
nothing about Crucible), Crucible's `MACOSX_BUNDLE_GUI_IDENTIFIER`, and, optionally, the
namespace fold `ac3cli` → `ac3::cli`, `ac3gui` → `ac3::gui`. `setApplicationName`,
`setOrganizationName`, the window title format, the `.desktop` `Name`, the AppStream `<name>`
and `ac3::version_details()` do not change.

**Exit:** every changed string is in the seven `ac3gui_*.ts` and six `ac3crucible_*.ts` files;
the GUI's settings are read from the same store as before; `ac3cli --version` still prints
`ac3forge <version>`.

**Verified by:** a PR touching `apps/` runs all eleven legs; `ctest -L gui` and `-L crucible-ui`;
`ac3gui_lupdate`/`ac3crucible_lupdate` regenerated and diffed; on Windows, run the built `ac3gui`
and confirm `HKCU\Software\ac3forge\ac3forge` is the store it reads; the Homebrew formula's test
string (`Formula/ac3forge.rb:65`) unchanged; screenshots in `docs/gui/screenshots/` recaptured
where the About box or banner appears, and `mkdocs build --strict` after.

### Phase 5: the roadmap

Depends on decision 9. Lands the Member column, the tags, the recount, the `CR` code, the two
link fixes and the wrapper note.

**Exit:** the Overview counts equal the section counts for every theme; no `](…)` href in
`ROADMAP.md` is docs-relative.

**Verified by:** `mkdocs build --strict` (the wrapper includes the file); the Phase 2 check;
opening `ROADMAP.md` on GitHub and following each of the DR8 links; a one-off count of
`**XXn (` lines per section against the table.

### Phase 6: packaging and release shape

Depends on decisions 5, 6, 13 and 14. Under S1 nothing renames; this phase lands the three
pre-tag facts: the `packages-crucible-<preset>` artifact name and its row in docs/releasing.md's
table (:548-554), one version-string style in `cmake/Packaging.cmake:357-365` with the two
asserts in `_build.yml:1478-1500` and `tools/ci/check_crucible_package.py`'s docstring following,
and the experimental claim reconciled. `CPACK_COMPONENT_<C>_DESCRIPTION` strings say which member
each component is, for the generators that show them.

**Exit:** a release dry run publishes `ac3forge-crucible-*` for Linux beside the Windows zip, and
every filename in the release follows one rule.

**Verified by:** `cpack --preset pack-windows-msvc` and
`python tools/ci/check_crucible_package.py packages/ac3forge-crucible-*.zip` locally; the
linux-llvm leg's Crucible pass in CI; a `release.yml` dry run (the way DR8 was verified); `tools/checks/check_packaging_versions.sh` green; `tools/release/bump_manifests.py` against the
dry run's assets finds both patterns it looks for.

### Phase 7: the driver, at signing time

Sequenced by [the promotion plan](../crucible/promotion.md#coordination-with-the-driver-signing-session),
and by decision 14. In one change after the signing session lands: the four INF strings
(`Ac3ForgeNullSink.inx:97-102`) and the `.rc` description to Crucible; `null_sink_substring` and
its tests and fakes; the About licence line; the `windows-driver` artifact name;
`apps/windows/README.md`; and, if chosen, the move to `apps/crucible/driver` with
`apps/crucible/CMakeLists.txt:372-376`, the `windows-driver` job and `driver-vm`'s paths. Then
attestation is submitted once.

**Exit:** the endpoint in a user's sound settings reads "Speakers (Crucible)" and the application
finds it; "Desktop Atmos" survives only in records.

**Verified by:** the `windows-driver` job builds and Code-Analyses the driver;
`apps/windows/driver-vm/Test-Driver.ps1` in the guest; `Deploy-Desk.ps1` pushes the built
`ac3crucible` and the signal path renders with the renamed device; `ctest -L crucible` with the
updated fakes.

### Later, outside this plan

The 1.0 questions: whether the binaries take Forge-branded names, whether the library's
identifiers give up the word, whether members get their own tag streams. Each is a breaking
change somewhere users have typed a name, and each is cheaper to take once, at the ABI freeze,
than twice.

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| No user script calls `ac3cli` or `ac3gui` by name | **no** | no telemetry; the reason the binaries keep their names |
| `forge` and `crucible` are free on PyPI, npm, crates.io, Homebrew, Debian and Fedora | yes, by hand per registry | outside the tree; only needed under S2 |
| No external page links to `…/ac3forge/cli/` or `…/gui/` | **no** | no analytics on Pages; the reason the docs directories stay |
| The winget and vcpkg submissions survive a naming change | yes, by reading the open upstream PRs | outside the tree |
| The GUI's settings store is unaffected by Phase 4 | yes | a manual run on Windows and Linux; the QML tests isolate to a temporary INI and cannot see the native store |

## Coordination

**The driver-signing session.** `apps/windows/driver/` is being worked in a separate session.
Nothing in Phases 1 to 6 touches it, `Ac3ForgeNullSink`, or the "Desktop Atmos" endpoint string;
Phase 7 is that session's landing plus one coordinated change.

**Open pull requests.** A tree-wide edit (Phase 1's `AC3DESK_*` fold, Phase 4's strings) lands
in one short-lived PR with the queue drained, the way UX12's Phase 1 did; check `gh pr list`
first. Phases 2, 3 and 5 are docs-heavy and can ride beside code PRs.

**The docs-only fast path.** A PR that touches only `docs/`, `*.md` and `mkdocs.yml` runs the docs
strict build and skips the matrix (`ci.yml:311`). Phase 3 should stay inside that set; the moment
it touches a `.cmake` or a `.qml` it pays for eleven legs.

## Deliberately not in scope

- **A repository, site or umbrella rename.** GitHub redirects git and web URLs; the Pages URL,
  the PyPI trusted publisher, the tap name and every verify snippet key on the literal name.
- **Renaming the library's identifiers** (`ac3::forge`, `find_package(ac3forge)`, `ac3forge_c`,
  `libac3forge`, the `AC3FORGE_` prefix). A 1.0 question at the earliest.
- **Renaming the binaries** or any package token, registry identifier, installer name or ProgID.
- **Per-member tag prefixes** or version lines.
- **Exporting `src/audio`** or splitting `ac3tests` per member.
- **Moving `apps/cli`, `apps/gui`, `apps/common`, `src/forge`, `apps/android`, `apps/wasm`,
  `js/`, `python/` or `rust/`** on disk.
- **Moving any page under `docs/`**, including `docs/cli`, `docs/gui`, `platforms/wasm.md`,
  `platforms/windows-demo.md` and `platforms/windows-driver-acx.md`.
- **Changing the Android applicationId** or the GUI's and Crucible's QSettings organisation and
  application names.
- **Renumbering roadmap IDs** or splitting the roadmap into files.
- **The driver's identity** before signing; the endpoint string after it.
- **A fourth member.** The Shield demo and the browser demos stay demos of the library.

## Decisions

Only what the owner had to decide. Each carries the recommendation and the cost of taking it;
every one was taken as recommended on 2026-09-05, so the recommendation is the decision.

1. **What Forge covers.** (a) the CLI alone; (b) `ac3cli` + `ac3gui` + `apps/common`; (c) the
   pair plus the bindings. **Recommend (b)**: it is the boundary the build, install rules,
   packaging and docs already draw. Cost: the word Forge also names the library's build
   identifiers until decision 2 is taken.
2. **The word "forge".** (a) keep every library identifier and write the rule once; (b) rename
   the library's identifiers; (c) another word for the tooling. **Recommend (a)** now, (b) only
   at the 1.0 freeze. Cost: one sentence on two index pages, and "AC3Forge Forge" is never written.
3. **Family spelling.** (a) `ac3forge` everywhere; (b) "AC3Forge" in prose and product names,
   `ac3forge` in identifiers; (c) "Ac3Forge". **Recommend (b)**. Cost: about ten display sites,
   `lupdate`, recaptured screenshots.
4. **The tooling's display strings.** (a) unchanged; (b) About heading, usage banner and
   `.desktop` `Comment` say Forge with the family beneath, everything keyed to a settings store or
   a registry stays; (c) everything says Forge. **Recommend (b)**. Cost: seven `.ts` files and the
   screenshots that show the About box.
5. **Package tokens.** (1) `ac3forge` prefix everywhere, as today; (2) per-member tokens.
   **Recommend (1)**. Cost: the member names live in display strings and docs only.
6. **Binary names.** Keep `ac3cli`, `ac3gui`, `ac3crucible` through 0.x. **Recommend keep**. Cost:
   Forge reaches no binary.
7. **Directory layout.** (a) nothing moves; (b) `apps/forge/{cli,gui,common}`; (c) top-level
   members. **Recommend (a)**. Cost: the tree does not state the family; the README and the
   configure summary do.
8. **Docs.** (a) nav only, tables stay on the landing page; (b) nav only, tables to
   `docs/library/capabilities.md` with three anchors repointed; (c) move directories with stubs.
   **Recommend (b)**. Cost: one new page and three anchor edits.
9. **Roadmap.** (a) themes plus Member column, tags, recount and a `CR` code; (b) reorder per
   member; (c) three files. **Recommend (a)**. Cost: about 35 tag edits.
10. **The Shield app.** (a) leave; (b) the library's Android demo, ids unchanged, display name
    changed later to drop the two marks; (c) a fourth member; (d) retire it. **Recommend (b)**.
    Cost: the launcher label on sideloaded devices, and the hardcoded `versionName` fixed with it.
11. **WASM.** (a) the npm package under Library > Bindings and the pages under Library > Live
    demos, nothing moved; (b) a Demos section. **Recommend (a)**. Cost: nav lines.
12. **Reverse-DNS root.** `com.iainchesworthlabs.*`, `com.ac3forge.*` or
    `io.github.iainchesworthlabs.*`. **Recommend `com.iainchesworthlabs.*`** and set Crucible's
    bundle identifier before promotion Phase 5. Cost: one line.
13. **Before the first Crucible tag.** The `packages-*` artifact name; `M.m.p` in every filename;
    the experimental claim reconciled. **Recommend all three now**. Cost: three edits and a table
    row.

    *Taken 2026-09-05, with one part refused on inspection.* The artifact is now
    `packages-crucible-<preset>`, which `release.yml` collects, and the experimental sentences
    in `docs/releasing.md` now say what `_build.yml` does: `windows-msvc-arm64` is experimental
    and ships. The filename unification was examined and **not** made. Two archives carry
    `PROJECT_VERSION_FULL` (`crucible`, `dev`) and the runtime carries `M.m.p`, and neither can
    take the other's style: the runtime's is what the Homebrew cask's URL is built from
    (`packaging/homebrew/Casks/ac3gui.rb` downloads `ac3forge-<M.m.p>-Darwin.dmg`), while
    dropping the prerelease suffix from the other two would make `0.10.0-beta.1` and
    `0.10.0-beta.2` produce the same filename. The two styles are therefore deliberate, and
    this page is where that is written down.
14. **The driver at signing time.** Strings, artifact name and substring only, ids kept; the move
    optional and in the same change. **Recommend that**. Cost: the endpoint name is frozen once
    attestation is paid for, so this lands first and once.
15. **One tag stream** or per-member tags. **Recommend one**. Cost: Crucible's first release
    carries the family's version number.