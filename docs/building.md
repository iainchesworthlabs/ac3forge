# Building ICL Forge

Every command here has been run on the configuration described under
[Verified configuration](#verified-configuration). Anything not verified is marked as such.

## Requirements

| | Version | Notes |
|---|---|---|
| A compiler | MSVC (VS 2026, toolset 14.5x), clang-cl 22, GCC 16, Clang 22, or Homebrew LLVM on macOS | C++23, including `std::expected` and deducing `this`. CI pins GCC 16 (`.github/toolchain/02-gcc-toolchain.sh`), LLVM 22 (`03-llvm-toolchain.sh`) and the MSVC toolset prefix 14.5 (`.github/toolchain-versions.json`); the Linux toolchain files fall back to older GCC versions so a machine one release behind still configures. See [Verified configuration](#verified-configuration) for the CI matrix. |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28...4.3)`. |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| vcpkg | a checkout that contains the commit in `vcpkg.json`'s `builtin-baseline` | Supplies fmt and Catch2, and the `hearth` feature's cpp-httplib, mbedTLS, libFLAC, Opus and mdns (selected by every hosted preset), plus the opt-in `adm` (Boost) and `profiling` (Tracy) features. A shallow or old checkout that lacks the baseline commit cannot resolve it. fmt and Catch2 can use `FetchContent` when vcpkg is unavailable. See [Options](#options). |
| Qt | 6.5+ prebuilt for `forge-gui`, 6.8+ for `hearth` and Crucible | GUI applications only. CI pins 6.10.3 (`.github/toolchain-versions.json`). **Never from vcpkg** — see [Qt](#qt). |
| ALSA (`libasound2-dev`) | any recent | Optional Linux audio backend. See [Linux audio](#linux-audio). |
| PipeWire (`libpipewire-0.3-dev`) | any recent | Optional Linux audio backend; required by Crucible. See [Linux audio](#linux-audio). |
| libxcb (`libxcb1-dev`) | any recent | Optional; used by Crucible for X11 full-screen detection. |
| Python 3 + numpy | 3.10+ | Only for `tools/`; not part of the build. `ruff.toml` targets 3.10, the floor of the Python package; CI's dependency locks use 3.14. |
| FFmpeg CLI | 8.x | Only for validation scripts; not part of the build. |
| Other targets | see the platform pages | WebAssembly needs Emscripten (`config-wasm-emscripten`), the ESP32 builds ESP-IDF v6.1, the bare-metal probe an `arm-none-eabi` GCC and QEMU, and the Android app the NDK: [WebAssembly](platforms/wasm.md), [Bare metal](platforms/bare-metal/index.md), [Android](platforms/android.md). |

## The short version

With `VCPKG_ROOT` set to a vcpkg checkout, from any shell — a Developer PowerShell is not
required; see [The compiler is pinned, not PATH-found](#the-compiler-is-pinned-not-path-found):

```bash
cmake --preset config-windows-msvc-debug
```

```bash
cmake --build --preset build-windows-msvc-debug
```

```bash
ctest --preset test-windows-msvc-debug
```

Drop `-debug` from all three preset names for a Release build, or swap `msvc` for `llvm` to
build with clang-cl instead. The `ci-windows-msvc` workflow preset runs the same three steps in
one command: `cmake --workflow --preset ci-windows-msvc` (Release only — the workflow presets in
`CMakePresets.json` don't have `-debug` variants).

See [Building on Linux](#building-on-linux) below for the equivalent on GCC/Clang.

## The compiler is pinned, not PATH-found

Every Windows preset chainloads `cmake/toolchains/windows.msvc.toolchain.cmake` (or
`windows.llvm.toolchain.cmake` for clang-cl), which finds `cl.exe`/`clang-cl.exe` and `link.exe`
by `find_program` against the MSVC tools directory, `NO_DEFAULT_PATH` — so whatever else is
first on `PATH` (LLVM installed for something unrelated, Git for Windows' own `link.exe`) cannot
be picked up by mistake the way a bare `find_package`-less configure would.

That toolchain directory has to come from somewhere. If `VCToolsInstallDir` and `INCLUDE` are
already set — a Developer PowerShell — it uses them. Otherwise
`cmake/toolchains/windows.msvc.environment.cmake` locates the newest Visual Studio install with
`vswhere`, runs its `vcvarsall.bat x64` (the arm64 form for `windows-msvc-arm64`) in a subprocess,
and imports the result into the CMake
process so every compiler check, `try_compile` and the actual `ninja` invocation inherit it —
which is what makes an ordinary shell work at all. The include/lib search paths are then baked
onto the compile and link lines themselves, not left in the environment, so the build tree stays
correct regardless of which shell later runs `cmake --build`.

The failure mode this leaves is not "wrong compiler picked up silently" but "no compiler found
at all": if no Visual Studio install carrying the `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`
component exists, configure fails with a `FATAL_ERROR` naming what was missing (`vswhere.exe`,
or a matching VS install) rather than picking something else and failing later. Install the
Visual Studio Build Tools (or Community/Professional/Enterprise) with the "Desktop development
with C++" workload if you hit that.

## Presets

`CMakePresets.json` is checked in and holds only what is machine-independent. It is built from
hidden fragments composed together, not a flat list:

- `core` — the Ninja generator, the vcpkg toolchain file from `$env{VCPKG_ROOT}`,
  `ICLFORGE_BUILD_CLI`/`ICLFORGE_BUILD_TESTS` pinned `ON`, `VCPKG_MANIFEST_FEATURES=hearth`, the
  vcpkg overlay triplets under `cmake/vcpkg/triplets/` and the overlay ports under
  `cmake/vcpkg/ports/`. (`CMAKE_EXPORT_COMPILE_COMMANDS` comes from the top-level
  `CMakeLists.txt` itself, not the presets.)
- `debug` / `release` — just `CMAKE_BUILD_TYPE`.
- `windows-msvc`, `windows-msvc-arm64`, `windows-llvm`, `linux-gcc`, `linux-llvm`,
  `linux-gcc-arm64`, `linux-llvm-arm64`, `macos-llvm`, `macos-llvm-x64` — one per
  platform/compiler pair. Each sets `VCPKG_TARGET_TRIPLET`, chainloads that platform's toolchain
  file (see [above](#the-compiler-is-pinned-not-path-found)) via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`,
  and is gated by a `condition` on `hostSystemName` so only the presets for the machine you're on
  even appear. `ICLFORGE_BUILD_GUI` is `ON` for `windows-msvc` and `windows-llvm` and `OFF` for
  the rest, `windows-msvc-arm64` included — see [Verified configuration](#verified-configuration).
- `wasm-emscripten` — the Emscripten toolchain for the browser demos. It does not inherit `core`:
  there is no host condition and no vcpkg toolchain, so it sets the generator, the binary
  directory and the options itself, and turns off everything a browser build has no use for (the
  CLI, GUI, tests, examples, Hearth, the three container writers and the C API). The AC-4
  libraries stay on. See [WebAssembly](platforms/wasm.md).
- `sanitize-asan-ubsan`, `sanitize-tsan`, `coverage`, `shared-libs`, `minimal-decoder` and
  `minimal-encoder` — the variant fragments described below, each adding a few cache variables to
  a platform preset.

Eighteen concrete `config-<platform>[-debug]` presets inherit `[ release|debug, <platform>, core ]`,
each with a matching `build-<platform>[-debug]` and `test-<platform>[-debug]` preset:

| Platform | Compiler | Configure preset | Build preset | Test preset |
|---|---|---|---|---|
| Windows | MSVC | `config-windows-msvc[-debug]` | `build-windows-msvc[-debug]` | `test-windows-msvc[-debug]` |
| Windows (arm64) | MSVC | `config-windows-msvc-arm64[-debug]` | `build-windows-msvc-arm64[-debug]` | `test-windows-msvc-arm64[-debug]` |
| Windows | clang-cl | `config-windows-llvm[-debug]` | `build-windows-llvm[-debug]` | `test-windows-llvm[-debug]` |
| Linux | GCC 16 | `config-linux-gcc[-debug]` | `build-linux-gcc[-debug]` | `test-linux-gcc[-debug]` |
| Linux | Clang 22 | `config-linux-llvm[-debug]` | `build-linux-llvm[-debug]` | `test-linux-llvm[-debug]` |
| Linux (arm64) | GCC 16 | `config-linux-gcc-arm64[-debug]` | `build-linux-gcc-arm64[-debug]` | `test-linux-gcc-arm64[-debug]` |
| Linux (arm64) | Clang 22 | `config-linux-llvm-arm64[-debug]` | `build-linux-llvm-arm64[-debug]` | `test-linux-llvm-arm64[-debug]` |
| macOS | Homebrew LLVM | `config-macos-llvm[-debug]` | `build-macos-llvm[-debug]` | `test-macos-llvm[-debug]` |
| macOS (x64) | Homebrew LLVM | `config-macos-llvm-x64[-debug]` | `build-macos-llvm-x64[-debug]` | `test-macos-llvm-x64[-debug]` |

The `-arm64` Linux rows are the same `linux.gcc.toolchain.cmake`/`linux.llvm.toolchain.cmake`
files as their x64 counterparts — only the vcpkg triplet (`arm64-linux-gcc`/`arm64-linux-llvm`)
differs, because the toolchain files already resolve aarch64 vs x86_64 from
`VCPKG_TARGET_ARCHITECTURE`. `windows-msvc-arm64` and `macos-llvm-x64` follow the same pattern
(`arm64-windows-msvc`, `x64-macos-llvm`). See [Raspberry Pi](platforms/raspberry-pi.md), the
primary hardware the Linux arm64 target is validated against.

The presets that are not platform/compiler pairs are variants of the Linux and Windows ones, or
build something other than the whole project. `cmake --list-presets` (and `cmake --build
--list-presets`, `ctest --list-presets`, `cpack --list-presets`, `cmake --workflow --list-presets`)
lists the ones that apply on the machine you are on; the WebAssembly and minimum-footprint
presets have no test preset.

**Sanitizers.** `config-linux-llvm-asan-ubsan` / `build-linux-llvm-asan-ubsan` /
`test-linux-llvm-asan-ubsan` is Debug-only, an instrumented variant of `linux-llvm`, and inherits
`linux-llvm` plus a `sanitize-asan-ubsan` fragment setting
`ICLFORGE_SANITIZERS=address,undefined` (see `cmake/Sanitizers.cmake`; MSVC is rejected outright,
so this only exists for GCC/Clang). Its test preset leaves out the `Performance` label, because
the throughput guards are not meant to run under a sanitizer. With any `ICLFORGE_SANITIZERS` set,
`tests/CMakeLists.txt` defines `ICLFORGE_TEST_SANITIZED=1` for `iclforge-tests` (0 otherwise), and
`tests/sanitized.hpp` gives it to the tests as `iclforge::test::kSanitized`.
The heaviest AC-4 tests take less under it: fewer frames, shorter signals, a stride through their
cases, one leg per frame rate, one committed stream of each kind (`tests/ac4_stream_kinds.hpp`,
which the decoder's engine test and Hearth's share), the first frames of a stream, one frame of
input for a CLI run that reads its first frame's syntax alone, each still running every code
path it covers, to the same tolerances. A debug build under ASan and UBSan runs the codecs many
times slower, and CI's sanitizer leg runs ctest serially; a new test that takes minutes there takes
the flag the same way.

`config-linux-llvm-tsan` / `build-linux-llvm-tsan` / `test-linux-llvm-tsan` is the ThreadSanitizer
sibling, inheriting `linux-llvm` plus a `sanitize-tsan` fragment setting
`ICLFORGE_SANITIZERS=thread`. It is a separate preset rather than more entries in the ASan/UBSan
list because the two runtimes are mutually exclusive — Clang refuses `-fsanitize=address,thread`
outright — and because they answer different questions: ASan/UBSan ask whether one thread's
memory and arithmetic are sound, TSan asks whether two threads agree on who owns what. Nothing
else in this repository can see a data race, and `src/audio` is a lock-free SPSC ring, a silence
watchdog and a clock-drift servo shared between a real-time callback thread and the encoder
thread.

Its test preset runs only the `concurrency` ctest label — the cases under `tests/audio/`, the
CLI's live-capture commands (`tests/cli/test_cli_live*.cpp`), the Crucible engine's threads
(`tests/crucible/`) and the Hearth engine's threaded cases (`tests/hearth/`) — because TSan's
shadow memory makes everything several times slower and the rest of the suite is single-threaded
codec maths. The label comes from the Catch2 tags themselves
(`catch_discover_tests(... ADD_TAGS_AS_LABELS)` in `tests/CMakeLists.txt`), so `ctest -L ring`,
`-L encoder` and the rest work the same way ([Running the tests](#running-the-tests)). `tsan.supp`
at the repository root holds the suppressions, and is meant to stay near-empty; `iclforge-membench` is
not built under this preset, because its global `operator new`/`delete` replacements collide with
TSan's own runtime at link time.

```bash
cmake --preset config-linux-llvm-tsan
cmake --build --preset build-linux-llvm-tsan -- -k 0
ctest --preset test-linux-llvm-tsan
```

**The minimum-footprint profiles.** A `minimal-decoder` fragment and a `minimal-encoder` fragment,
and the configure/build presets that inherit them, build the decode-only library and its probe (or
the encode-only ones) and nothing else, so they are not part of the tables above and have no test
presets. The decoder ones are `config-linux-gcc-minimal`, `config-linux-llvm-minimal` and
`config-arm-none-eabi-minimal`; `config-arm-none-eabi-minimal-encoder` and
`config-linux-gcc-minimal-encoder` are the encoder's; and `config-linux-gcc-minimal-ac4` and
`config-arm-none-eabi-minimal-ac4` add the AC-4 decoder to the decoder profile. A `-icount` suffix
on the `arm-none-eabi` ones (`config-arm-none-eabi-minimal-icount`,
`-minimal-ac4-icount`, `-minimal-encoder-icount`) puts the probe's clock on the CMSDK timer so
that QEMU's `-icount` counts instructions, in its own build directory. The `arm-none-eabi` ones
do not inherit `core` either: there is no vcpkg triplet for bare-metal arm and nothing the
profile builds has a third-party dependency. See
[Minimum-footprint decoder profile](#minimum-footprint-decoder-profile).

**Coverage.** `config-linux-gcc-coverage` / `build-linux-gcc-coverage` /
`test-linux-gcc-coverage` is Debug-only, an instrumented variant of `linux-gcc` rather than a
platform/compiler pair. It inherits a `coverage` fragment setting
`ICLFORGE_ENABLE_COVERAGE=ON` (see `cmake/Coverage.cmake`: gcov's `--coverage` on GCC and Clang,
and on clang-cl LLVM's source-based coverage; MSVC just warns and skips it),
`ICLFORGE_BUILD_ADM=ON` with vcpkg's `adm` feature (so the opt-in ADM pair — `iclforge::adm` and its
bridge — is measured alongside the always-on library components) and `ICLFORGE_BUILD_CLI=ON`,
since `apps/cli` is gated too. Only `ICLFORGE_BUILD_EXAMPLES` stays off, as a build-time saving: `examples/` is
documentation that happens to compile, over an API surface `tests/` already covers, and each one
is its own `ctest` process. `config-linux-gcc-coverage` itself (not the shared `coverage`
fragment, since `config-windows-llvm-coverage` also inherits that fragment and stays
Crucible-scoped) extends `VCPKG_MANIFEST_FEATURES` to `adm;hearth` so `src/sendspin` and
`apps/hearth`'s engine and test sink — on by default like everywhere else — are measured too; see
[`tools/checks/coverage_report.sh`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/checks/coverage_report.sh)
for their floors. `config-windows-llvm-coverage` is the Windows counterpart, Release-based (the
profile runtime is built against the release CRT), with Crucible on and ADM and Hearth off; its
test preset runs the `crucible` and `crucible-ui` labels, and
`tools/checks/coverage_crucible.ps1` reads its result.

Note that `forge` has to link `iclforge::coverage` itself (`apps/cli/CMakeLists.txt`) and not merely
link an instrumented library. The gcov *runtime* propagates to consumers automatically, but
`--coverage` is target-scoped at compile time — so without that link every `.cpp` under
`apps/cli` compiles uninstrumented and emits no `.gcno` at all, which reads as *no data* rather
than as low coverage. The same applies to any other executable added to the report later.

After `ctest`, `tools/checks/coverage_report.sh` (the same script the `coverage` job of
`_ci-core.yml` runs) makes one `gcovr` extraction pass and then gates line *and* branch coverage
per component — the `src/` library components, `apps/cli`, `apps/common`, Crucible's platform-free
engine and Hearth's engine and test sink — and prints a per-command
breakdown of `apps/cli` below the gate, reported but not gated, so a thin command module shows as
thin instead of averaging away inside the aggregate. See the script's own floor table for the
current thresholds and the measured baseline each was calibrated against:

```bash
cmake --preset config-linux-gcc-coverage
cmake --build --preset build-linux-gcc-coverage -- -k 0
ctest --preset test-linux-gcc-coverage -LE Performance
./tools/checks/coverage_report.sh -g gcov-16
```

`apps/gui` is deliberately absent from that report: instrumenting its C++ needs a Qt kit on the
coverage job, which installs none (CI puts Qt only on the plain `gui` build legs, which are not
instrumented). Its interactive surfaces are covered by `apps/gui/tests`' own Qt Quick suite, and
its one Qt-free class (`RecordingSink`) is already in `iclforge-tests`. `python/` has its own floor
instead, in `.github/workflows/wheels.yml`'s `python-coverage` job — `pytest --cov` against the
built wheel; see that job's own comment for what a Python percentage does and does not measure
when nearly all of the binding surface is C++.

**Shared libraries.** `config-linux-llvm-shared` / `build-linux-llvm-shared` /
`test-linux-llvm-shared`, same shape again: an instrumented variant of `linux-llvm`, Debug-only.
It inherits a `shared-libs` fragment setting `BUILD_SHARED_LIBS=ON`, proving
`iclforge::ac3_shared`/`iclforge::matroska_shared` actually work — not just that the CMake topology
configures, but that every in-tree consumer (`forge`, `forge-gui`, `iclforge-tests`, `examples/`) links
and runs against the real `.so`. `.github/workflows/_ci-linux.yml` runs it as an extra step inside
the existing `linux-llvm` leg rather than a new matrix entry (in the nightly run only), the same
shape as the ASan/UBSan pass.

Passing tests do not show that `iclforge-tests` ran against `libiclforge_ac3.so`, so that is checked
separately: `tools/checks/check_shared_forge_binding.sh` reads the dynamic linker's bindings
(`LD_DEBUG=bindings`) and fails if any `iclforge::` symbol the test binary takes from `libiclforge_ac3.so`
binds to another library, or if the binary carries its own copy of the codec. The few test files
that reach into the library's internals — the AC-4 syntax cases and `core/test_fixed32_ecpl.cpp` —
are built only when `iclforge::ac3` is the static library, because a `.so` exports none of that.

There are 11 `ci-<platform>` `workflowPresets` (Release except for the two sanitizer ones, which
are Debug-only) that chain configure→build→test in one call, for the nine platform/compiler pairs
and the ASan/UBSan and TSan variants: `cmake --workflow --preset ci-windows-msvc`. There is no
coverage or shared-library workflow preset, and CI does not call `cmake --workflow` at all: the
`build-leg` composite action (`.github/actions/build-leg`, which every leg of `ci.yml` and both
jobs of `pr-gate.yml` run) runs `cmake --preset config-<leg>`, `cmake --build --preset
build-<leg>` and `ctest --preset test-<leg>` as three separate steps, because a leg has per-leg
overrides to append (`-DICLFORGE_BUILD_GUI=ON` on the GUI legs, `-DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"`
where a Qt kit was installed) that a single `--workflow` invocation has nowhere to put. The
workflow presets are the one-command local equivalent of those three steps, not the path CI takes.

Anything machine-specific belongs in `CMakeUserPresets.json`, which is gitignored. The pattern
is a hidden `local` preset carrying the paths, inherited alongside the checked-in fragments:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local",
      "hidden": true,
      "environment": {
        "VCPKG_ROOT": "D:/vcpkg",
        "VCPKG_DOWNLOADS": "D:/vcpkg-downloads",
        "VCPKG_DEFAULT_BINARY_CACHE": "D:/vcpkg-cache"
      },
      "cacheVariables": {
        "VCPKG_INSTALL_OPTIONS": "--x-buildtrees-root=D:/vcpkg-buildtrees;--x-packages-root=D:/vcpkg-packages"
      }
    },
    { "name": "dev", "inherits": [ "local", "debug", "windows-msvc", "core" ] }
  ],
  "buildPresets": [ { "name": "dev", "configurePreset": "dev" } ],
  "testPresets": [
    { "name": "dev", "configurePreset": "dev", "output": { "outputOnFailure": true } }
  ]
}
```

`debug` alone has no generator or binary directory — those live on the hidden `core` preset,
and the compiler selection on a platform preset (`windows-msvc` here; `windows-msvc-arm64`,
`windows-llvm`, `linux-gcc`, `linux-llvm`, `linux-gcc-arm64`, `linux-llvm-arm64`, `macos-llvm` and
`macos-llvm-x64` are the others — see `CMakePresets.json`). Missing either from `dev`'s
`inherits` list still configures, but silently: CMake
falls back to its platform default generator (Visual Studio, on this machine) and an in-source
binary directory instead of `build/dev`, which is a mess to notice and worse to undo. Inherit
all four.

That keeps vcpkg's working directories off the system drive, which matters because they run to
several gigabytes. Substitute your own paths, and swap `windows-msvc` for whichever
platform/compiler fragment matches your machine.

## Running the tests

The test presets (`test-<platform>[-debug]`) run `ctest` over what CMake registered for that
build. `iclforge-tests`, the Catch2 binary that holds nearly all of the C++ cases, registers one ctest
entry per test case, and `catch_discover_tests(... ADD_TAGS_AS_LABELS)` turns every Catch2 tag on a
case into a ctest label, so a tag selects a subset in two ways:

```bash
ctest --preset test-linux-gcc-debug -L ac4dec           # the cases tagged [ac4dec], through ctest
ctest --preset test-linux-gcc-debug -N -L ac4           # list what a label selects, run nothing
build/config-linux-gcc-debug/bin/iclforge-tests "[ac4dec]"    # the same cases, through the Catch2 binary
build/config-linux-gcc-debug/bin/iclforge-tests --list-tags   # every tag and how many cases carry it
```

A case carries several tags, one for the component under test and others for what it checks. The
codecs have `eac3`, `ac4`, `ac4core`, `ac4dec` and `ac4enc`; the libraries and applications
`cli`, `capi`, `hearth`, `sendspin` and `crucible`; and there are `oba` (Atmos objects), `dsp`,
`iec61937`, `fixed32`, `simd` and `avx2`, and `concurrency` for the cases ThreadSanitizer runs.
`iclforge-tests --list-tags` has the full list.

Three things register their own ctest entries beside `iclforge-tests`. `iclforge-perf`, the real-time
throughput guards, is a separate binary whose cases carry the `Performance` label, so
`ctest -LE Performance` leaves them out. The Qt Quick suites register one entry per `tst_*.qml`
file: `forge_gui_qml_tests_*` (label `gui`), `hearth_qml_tests_*` (`hearth-ui`) and
`crucible_qml_tests_*` (`crucible-ui`), and only when the matching application is built.
`ctest -R <name>` selects by test name and `ctest --rerun-failed --output-on-failure` repeats
the failures of the last run; add `--output-on-failure` to any run to see a failing case's output.

## Options

| Option | Default | Effect |
|---|---|---|
| `ICLFORGE_BUILD_CLI` | `ON` | Build `forge`. |
| `ICLFORGE_BUILD_GUI` | `ON` on `windows-msvc` and `windows-llvm`, `OFF` on every other preset | Build `forge-gui`. Requires Qt 6.5+. Off by default elsewhere because a Qt kit isn't assumed present there — see [Building on Linux](#building-on-linux). |
| `ICLFORGE_FETCH_FMT` | `ON` | When no local {fmt} 11.1.0 or newer is found (vcpkg, a distro package, an explicit `CMAKE_PREFIX_PATH`), fetch and build v12.2.0 from source via `FetchContent` instead of failing. An older local copy, such as Ubuntu 26.04's `libfmt-dev` 10.1.1, is skipped and named in the configure output. Turn off to insist on a package-manager copy — see `cmake/Fmt.cmake`. Unlike the other `ICLFORGE_FETCH_*` options, this one is never irrelevant: {fmt} is a base dependency needed by every build. |
| `ICLFORGE_BUILD_TESTS` | `ON` | Build the Catch2 suite. Requires Catch2. |
| `ICLFORGE_FETCH_CATCH2` | `ON` | When no local Catch2 3 is found (vcpkg, a distro package, an explicit `CMAKE_PREFIX_PATH`), fetch and build v3.15.3 from source via `FetchContent` instead of failing. Turn off to insist on a package-manager copy — see `tests/CMakeLists.txt`. Irrelevant when `ICLFORGE_BUILD_TESTS` is off. |
| `ICLFORGE_BUILD_EXAMPLES` | `ON` | Build `examples/`, and register them as tests. |
| `ICLFORGE_BUILD_MATROSKA` | `ON` | Build `iclforge::matroska` (`src/matroska`), the standalone Matroska container writer. `OFF` only makes sense with the CLI, GUI, tests and examples all `OFF` too — they link it unconditionally, and configure fails with a clear message otherwise (see the root `CMakeLists.txt` guard). |
| `ICLFORGE_BUILD_MP4` | `ON` | Build `iclforge::mp4` (`src/mp4`), the standalone MP4/ISOBMFF container writer. Same all-off constraint as `ICLFORGE_BUILD_MATROSKA`. |
| `ICLFORGE_BUILD_MPEGTS` | `ON` | Build `iclforge::mpegts` (`src/mpegts`), the standalone MPEG-TS container writer. Same all-off constraint as `ICLFORGE_BUILD_MATROSKA`. |
| `ICLFORGE_BUILD_IAB` | `ON` | Build `iclforge::iab` (`src/iab`), the standalone SMPTE ST 2098-2 Immersive Audio Bitstream reader. Like the three container writers above it needs no opt-in third-party library, so it defaults on the same way; unlike them nothing in `apps/` or `examples/` links it yet, so there is no all-off guard — `tests/CMakeLists.txt` simply adds its test file when this is on. |
| `ICLFORGE_BUILD_IAMF` | `ON` | Build `iclforge::iamf` (`src/iamf`), the standalone IAMF v1.1 OBU and ISOBMFF writer. Same zero-third-party-dependency shape as `iclforge::iab`, and like it linked by nothing in `apps/` (`examples/mux_iamf.cpp` builds when this is on). The vcpkg port's `iamf` feature and the Conan recipe's `iamf` option install it, off by default. |
| `ICLFORGE_BUILD_AC4` | `ON` | Build the AC-4 libraries: the inspector `iclforge::ac4` (`src/ac4`), the decoder `iclforge::ac4dec` (`src/ac4dec`), the encoder `iclforge::ac4enc` (`src/ac4enc`) and the core the decoder and the encoder share (`src/ac4core`) — see [AC-4](library/ac4.md). The inspector, the decoder and the encoder are installed and exported, and the core with the static libraries that call into it. `OFF` needs the CLI, the GUI and the tests off too, and Hearth unless it is the ESP-IDF player half (the root `CMakeLists.txt` guards), since they link them. The Python wheel binds them (`iclforge.ac4`), the WebAssembly preset builds them for the `iclforge_wasm_ac4` module, and the Android app builds them without linking them yet; the ESP-IDF component and the minimum-footprint presets turn the option off and take the decoder alone through `ICLFORGE_MINIMAL_AC4`. The vcpkg port's `ac4` feature and the Conan recipe's `ac4` option install them, off by default. |
| `ICLFORGE_BUILD_CAPI` | `ON` | Build `iclforge::c` (`src/capi`), the C API over the encode/decode core — see [C API](library/c-api.md). Depends on nothing but `iclforge::ac3_static`, so unlike `ICLFORGE_BUILD_ADM` there is no extra dependency footprint to opt out of. |
| `ICLFORGE_BUILD_PYTHON` | `OFF` | Build the pybind11 extension module (`python/`). Off by default for the same reason as `ICLFORGE_BUILD_ADM`: nothing under `src/`, `apps/`, `tests/` or `examples/` links it, so a normal C++ build is unaffected either way. `python/pyproject.toml` turns it on itself via scikit-build-core when `pip install`/cibuildwheel drives the configure. |
| `ICLFORGE_BUILD_ADM` | `OFF` | Build `iclforge::adm` (`src/adm`), the standalone BW64/RF64 + ADM parser — see [ADM / BW64 reading](library/adm.md). Off by default, unlike every other library component: it vendors libbw64/libadm via `FetchContent`, and libadm needs several Boost header libraries, resolved separately via `-DVCPKG_MANIFEST_FEATURES=adm` (`vcpkg.json`'s `adm` feature) — turning this `ON` without also selecting that feature fails with a clear configure-time message rather than a bare "Boost not found". |
| `ICLFORGE_BUILD_CRUCIBLE` | `OFF` | Build the Crucible engine, console runner, and desktop window. Linux requires PipeWire; see [Crucible installation](crucible/install.md#linux). |
| `ICLFORGE_BUILD_HEARTH` | `ON` | Build `iclforge::sendspin`, the Hearth engine, `hearth` (the desktop window, Windows/macOS/Linux with a Qt 6.8+ kit), `hearth-testsink`, `hearth-testserver`, and `hearth-render` (an item through the engine into a WAV file, for the checks). Qt not found skips just `hearth` with a configure warning rather than failing; the engine and its tests still build. Every CI leg has built and tested it since A7, so this defaults on the same way — a plain preset configure needs no extra flag any more. The vcpkg side follows: `CMakePresets.json`'s `core` fragment selects the root manifest's `hearth` feature by default too, for its network, pairing, FLAC, and Opus dependencies. A few presets that cannot build Hearth turn both back off explicitly — the minimum-footprint decoder/encoder profiles (no OS), the Emscripten/WASM demo (no vcpkg toolchain), and the Windows LLVM coverage leg (deliberately Crucible-only) — see their own entries in `CMakePresets.json`. The ESP-IDF component builds only `src/sendspin`'s player half, behind `CONFIG_ICLFORGE_SENDSPIN` (`esp-idf/iclforge/Kconfig`). The vcpkg port and the Conan recipe (`packaging/`) pin it off: they build the library only. See [Hearth](hearth/index.md). |
| `ICLFORGE_WITH_ALSA` | `AUTO` | Linux only. `AUTO` builds the ALSA audio backend when libasound's headers are present; `ON` requires them; `OFF` never builds it. Takes precedence over `ICLFORGE_WITH_PIPEWIRE` when both are found — see [Linux audio](#linux-audio). |
| `ICLFORGE_WITH_PIPEWIRE` | `AUTO` | Linux only. `AUTO` builds the PipeWire audio backend when libpipewire-0.3's headers are present *and* ALSA was not selected; `ON` requires the headers (independently of ALSA); `OFF` never builds it. See [Linux audio](#linux-audio). |
| `ICLFORGE_CRUCIBLE_X11` | `AUTO` | Linux only, with `ICLFORGE_BUILD_CRUCIBLE`. `AUTO` compiles Crucible's X11 full-screen check over libxcb when `libxcb1-dev` is present; `ON` requires it; `OFF` never builds it. Without it the rule is off at runtime and the Room page says so. The configure summary prints `Crucible X11   : xcb` or `none`. |
| `ICLFORGE_SIMD` | `auto` | Which `arch-*` directory of `src/arithmetic/variants/` supplies the codec's vector kernels: `auto` picks `x86_64` or `aarch64` from the *effective target* architecture (`CMAKE_SYSTEM_PROCESSOR`, or `CMAKE_OSX_ARCHITECTURES` where a macOS cross-build sets one) and falls back to `generic` everywhere else, including a macOS universal binary, and `generic`/`x86_64`/`aarch64` force one. See [SIMD kernels and the architecture tree](#simd-kernels-and-the-architecture-tree). The configure summary prints the resolved value, and so does `forge --version`. |
| `ICLFORGE_AVX2` | `ON` | x86_64 only. Compiles an AVX2 SIMD tier alongside the baseline SSE2 one, selected at *runtime* rather than at configure time. See [Runtime AVX2 dispatch](#runtime-avx2-dispatch). `OFF` (or a non-x86_64 target) yields a provably AVX2-free binary. |
| `ICLFORGE_SANITIZERS` | empty | Comma-separated `-fsanitize=` value, e.g. `address,undefined` — see `cmake/Sanitizers.cmake`. Empty is a no-op; GCC/Clang only, MSVC is a configure error. Set via the `-asan-ubsan` or `-tsan` preset above rather than by hand. |
| `ICLFORGE_ENABLE_COVERAGE` | `OFF` | Coverage instrumentation over every target it's linked into — see `cmake/Coverage.cmake`. gcov's `--coverage` on GCC and Clang, LLVM source-based coverage on clang-cl; off is a no-op, and other compilers get a configure-time warning and no instrumentation. Set via the `-coverage` presets above rather than by hand. |
| `ICLFORGE_ENABLE_TRACY` | `OFF` | Tracy profiler instrumentation (`iclforge::tracy` — see `cmake/Tracy.cmake`). Needs vcpkg's `profiling` manifest feature (`-DVCPKG_MANIFEST_FEATURES=profiling`), which supplies Tracy itself; off is a no-op. |
| `ICLFORGE_BUILD_FUZZERS` | `OFF` | Build the libFuzzer harnesses under `fuzz/`. Clang only (GCC and MSVC ship no libFuzzer); use `fuzz/run.sh` rather than this option directly — it configures a dedicated `build/fuzz` with the right compiler. See [`fuzz/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/fuzz/README.md). |
| `ICLFORGE_MINIMAL_DECODER` | `OFF` | Build **only** `iclforge::ac3_minimal`: one decode-only static library with no exceptions, no RTTI and no direct-form transform tables, for a target with a few hundred kilobytes of RAM and no operating system. Not a "build X too" option — it replaces what `src/ac3` builds, and configure fails with a list if any component that needs the full library is still on. GCC/Clang only. See [Minimum-footprint decoder profile](#minimum-footprint-decoder-profile). |
| `ICLFORGE_MINIMAL_ENCODER` | `OFF` | The same profile pointed the other way: build **only** an encode-only `iclforge::ac3_minimal` carrying the AC-3 and the E-AC-3 encoder. Mutually exclusive with `ICLFORGE_MINIMAL_DECODER`, which configure enforces (see [The encode direction](#the-encode-direction)). GCC/Clang only. |
| `ICLFORGE_MINIMAL_AC4` | `OFF` | Only with `ICLFORGE_MINIMAL_DECODER`: also build the AC-4 inspector, core and decoder (`src/ac4`, `src/ac4core`, `src/ac4dec`) as static archives with the profile's own compile options - no encoder, no shared library, no position-independent code. The ESP-IDF component sets it from `CONFIG_ICLFORGE_AC4` ([ESP32-P4](platforms/bare-metal/esp32-p4.md#ac-4) is the part measured on a board, [ESP32-C6](platforms/bare-metal/esp32-c6.md#ac-4) has the fixed-point tier's figures), and the `-minimal-ac4` presets set it together with `ICLFORGE_DECODE_SCALAR=float`; `tools/checks/run_baremetal_probe.sh --ac4 --scalar=fixed` builds them at the fixed-point tier. `ICLFORGE_BUILD_AC4` stays the full build's option and the profile refuses it. |
| `ICLFORGE_STAGE_TIMERS` | `OFF` | Minimum-footprint profiles only: route the library's zone markers (the ones `ICLFORGE_ENABLE_TRACY` turns into Tracy zones) to two functions the application supplies, so a bare-metal probe can report microseconds per stage. Configure fails outside the profile. |
| `ICLFORGE_DECODE_SCALAR` | `double` | The arithmetic the AC-3 and E-AC-3 decoders carry their coefficients in: `double`, `float`, or `fixed` (`iclforge::internal::Fixed32`, Q7.24 in an `int32`, for a part with no FPU). The AC-4 decoder follows it: `float` builds its kernels in single precision, and `fixed` builds them on `Fixed32` with a block exponent per transform block and per QMF slot. The AC-4 encoder is `double` in every build. The minimum-footprint profile takes `float` unless it is given `fixed`, and the ESP-IDF component picks `float` or `fixed` from the part. See [A float32-only path](#gaps) and the fixed-point section there. A `float` or `fixed` build evaluates the AC-4 sample rate converter's tables while it compiles `src/ac4core/src/dsp/resampler.cpp`, which takes the compiler 5 to 14 seconds more (MSVC the longest), with its limit on constant evaluation raised for that file in `src/ac4core/CMakeLists.txt`. |
| `ICLFORGE_ENCODE_SCALAR` | `double` | The arithmetic of the AC-3 and E-AC-3 encoders' analysis front end (transient detection, the forward transform and the coefficient store through the coupling, spectral-extension and enhanced-coupling fits): `double` or `float`. The AC-4 encoder always runs in `double`. The minimum-footprint profile takes `float`. |
| `ICLFORGE_INSTALL_BOTH_LINKAGES` | `ON` | Install and export both the static and the shared variant of each library. `OFF` installs only the one `BUILD_SHARED_LIBS` selects, which is what the vcpkg port and the Conan recipe pass. See `cmake/InstallLibrary.cmake` and [Releasing](releasing.md#vcpkg-port). |
| `ICLFORGE_QT_ROOT` | empty | Path to a Qt kit or a Qt install root, searched before the default install roots `cmake/FindQt6.cmake` looks in; a value that yields no kit is an error. The `ICLFORGE_QT_ROOT`, `QT_ROOT_DIR` and `QTDIR` environment variables work the same way (a stale `QT_ROOT_DIR` or `QTDIR` falls through to the defaults instead), and `-DCMAKE_PREFIX_PATH` and `-DQt6_DIR` take priority over all of them. See [Qt](#qt). |

The ESP-IDF component and the bare-metal probes read further variables and Kconfig symbols (`ICLFORGE_ESP_PROFILE`, `ICLFORGE_MINIMAL_HOT_O2`, `ICLFORGE_BAREMETAL_CLOCK`, `CONFIG_ICLFORGE_AC4`, `CONFIG_ICLFORGE_SENDSPIN`, and others); the Kconfig symbols are in `esp-idf/iclforge/Kconfig`, and the rest are described in [`esp-idf/iclforge/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/esp-idf/iclforge/README.md) and on the [ESP32-S3](platforms/bare-metal/esp32-s3.md), [ESP32-P4](platforms/bare-metal/esp32-p4.md) and [Cortex-M3](platforms/bare-metal/cortex-m3.md) pages.

Building the library and CLI alone, with neither Qt nor vcpkg's extra features involved:

```bash
cmake --preset config-windows-msvc-debug -DICLFORGE_BUILD_GUI=OFF -DICLFORGE_BUILD_TESTS=OFF -DICLFORGE_BUILD_HEARTH=OFF
```

The vcpkg toolchain file is still referenced by the preset, so `VCPKG_ROOT` must still point
at a checkout — it simply has nothing to install once `ICLFORGE_BUILD_HEARTH` is also off (the
preset's own `VCPKG_MANIFEST_FEATURES=hearth` default otherwise still asks vcpkg to build
Hearth's network/FLAC/Opus dependencies even with the library and CLI alone). To build with no
vcpkg at all, configure without the preset and pass the generator and build type by hand.

## Minimum-footprint decoder profile

The next users of the decoder are set-top boxes, receivers and DSP ports. What they need is a
build that is small, a target it runs on, and a footprint number that CI holds.

```bash
# Cross-compile for arm-none-eabi and run on QEMU's mps2-an385 (Cortex-M3, no OS)
tools/checks/run_baremetal_probe.sh

# The same profile natively, no emulator
tools/checks/run_baremetal_probe.sh --host
```

Both go through the presets, which you can also drive directly:
`config-arm-none-eabi-minimal` / `build-arm-none-eabi-minimal`, and
`config-linux-gcc-minimal` / `config-linux-llvm-minimal` for the host.

### What the profile changes

| | Effect |
|---|---|
| Decode-only sources | The encoder, the container writers, WAV I/O, the analysis/QC layers and the object *encoder* are not compiled. `src/ac3/minimal.cmake` lists what is, with a line on why each file is reachable from a decode. |
| No direct-form transform tables | `src/ac3/src/core/transform/stub/` replaces `.../reference/`, removing **1,900,544 bytes of `.bss`** — the four (k, n) matrices §8.2.3.2's forward MDCT and §7.9.4.2 step 3's inverse sums need. |
| `-fno-exceptions -fno-rtti` | The codec's own error mechanism is `std::expected` throughout, so there is nothing of its own to disable. |
| `-ffunction-sections -fdata-sections`, `--gc-sections` | An integrator linking a subset pays for a subset. |

That table's second row is the profile's largest single win and its only behavioural difference.
Measured with `dumpbin /HEADERS` over `mdct.cpp.obj`:

| Table | Bytes | Used by |
|---|---|---|
| `ForwardCosTable<512>` | 1,048,576 | Direct-form forward MDCT, long — encode only |
| `ForwardCosTable<256>` × 2 | 524,288 | Direct-form forward MDCT, the two short halves — encode only |
| `InnerSumTable` | 262,144 | Direct-form inverse, long — decode |
| `InnerSumPairTable` | 65,536 | Direct-form inverse, short — decode |
| **Total** | **1,900,544** | |
| *(every table the fast paths need)* | *~12,600* | |

They are lazily *constructed* but statically *allocated*: the linker reserves that storage
whether or not any of them is ever built. Leaving them out means `DecoderConfig::fast_imdct =
false` returns `DecodeError::kNoReferenceTransform` in this profile rather than being silently served by
the fast path — that switch exists so a caller can validate against the arithmetic the spec
writes down, and substituting a different arithmetic would defeat its only purpose.

### The probe

`apps/baremetal/probe.cpp` links the archive and decodes six frames of each of fourteen rows, four
AC-3 and ten E-AC-3, built from ten committed streams. The AC-3 rows are 5.1 (448 kbit/s,
coupling) and its Lo/Ro fold, 2/0 (192 kbit/s, the layout §7.5.4 rematrixing exists in) and 1/0.
The E-AC-3 rows are 5.1 (384 kbit/s, AHT + spx + standard coupling) and its Lo/Ro fold, the same
with §E3.5 enhanced coupling (`cpl+ecpl`, which `tools=all` does not select), 2/0, a 5.1 stream
with dynrng words and dialnorm 24 in line mode, 7.1.4 (a bed and two dependent substreams) and its
fold, and three Atmos rows: the bed alone, the objects reconstructed, and the objects placed onto
7.1.4. It compares every channel's level against `apps/baremetal/fixture.hpp`, and prints
`key=value` lines that `tools/checks/run_baremetal_probe.sh` gates on. It is not a unit test — the profile requires
`ICLFORGE_BUILD_TESTS=OFF`, since nothing under `tests/` builds against a decode-only archive —
and it answers three questions a test could not: does the archive link with everything else
absent, does it produce the right audio on a 32-bit soft-float target, and what did it cost.
Regenerate its fixtures with
`python tools/generators/gen_baremetal_fixture.py --forge <path>`. Adding a configuration is a
row in that script's `STREAMS`, a layout is a row in its `LAYOUTS`, and a fixture is a row in
`probe.cpp`'s `kEac3Fixtures` (`kAc3Fixtures` for AC-3); neither runner script names a fixture, so
nothing else has to be widened to keep gating one.

Nothing regenerates the fixtures automatically and nothing detects that they have drifted from
the encoder — the probe decodes the committed bitstream and compares it against the committed
levels, so both moving together is invisible to it. The header committed in August 2026 was 131
encoder commits stale by the time the §E3.5 and 2/0 streams were added and every stream was
re-based onto the encoder of the day. This does not weaken what the probe measures — it is a
decode regression reference either way — but a fixture is only evidence about the encoder that
produced it.

### The encode direction

The same profile pointed the other way. `ICLFORGE_MINIMAL_ENCODER` builds an encode-only
`iclforge::ac3_minimal` carrying both codecs, and `apps/baremetal/encode_probe.cpp` is its probe:

```bash
tools/checks/run_baremetal_probe.sh --encoder          # arm-none-eabi under QEMU
tools/checks/run_baremetal_probe.sh --encoder --host   # natively
```

It is **mutually exclusive** with the decoder, and that is measured rather than a simplification.
On an ESP32-S3 with 277,400 bytes of internal SRAM free:

| | Peak heap |
|---|---|
| Decode, including Atmos objects | 233,546 |
| AC-3 encode | 201,770 |
| E-AC-3 encode | 243,770 |
| Both encoders at once | 440,420 |

Each fits alone; no two fit together. A build offering both would be offering something the part
cannot run, so the option refuses the combination rather than letting it arrive as `out_of_memory`
on a device. Sequential use is fine — tear one down, build the other.

### What the encode direction costs

Six rows since 2026-09-10, each six frames of the same synthesised programme through one
encoder, and each printing its peak heap and its time per frame on the terms the decode probe
uses (`<row>.us_per_frame`, `realtime_permille` against a 32 ms frame). Peaks are the same on
the `arm-none-eabi` leg and the ESP32-S3 under QEMU; the host's are about one per cent higher
for its wider pointers. Instructions per frame are the `--encoder --icount` leg's: Thumb-2 on
the Cortex-M3, `-Os`, soft float throughout, held to the ceilings in
`run_baremetal_probe.sh`'s `ICOUNT_CEILING_ENCODE` table.

| Row | Peak heap | Allocations per frame | Instructions per frame | Ceiling | Decode row's count |
|---|---:|---:|---:|---:|---:|
| `ac3_stereo` 2/0, 192 kbit/s | 52,707 | 34 | 9,136,000 | 16,000,000 | 3,550,000 |
| `eac3_stereo` 2/0, 192 kbit/s, no tools | 79,894 | 76 | 12,683,000 | 30,000,000 | 4,858,000 |
| `eac3_tools` 2/0, 192 kbit/s, cpl + spx + AHT | 143,037 | 47 | 16,920,000 | 31,000,000 | - |
| `eac3_ecpl` 2/0, 192 kbit/s, §E3.5 | 130,887 | 87 | 48,217,000 | 104,000,000 | 28,863,000 |
| `ac3` 5.1, 448 kbit/s | 110,918 | 67 | 24,866,000 | 43,000,000 | 10,228,000 |
| `eac3` 5.1, 384 kbit/s | 158,602 | 173 | 33,207,000 | 78,000,000 | 12,965,000 |

Between 1.7 and 2.6 times the decode row's count for the same layout, with the encoders in
`float` end to end since 2026-09-10 (the analysis front end first, then the coefficient store and
every analysis behind it; the decode path has been `float` under this profile since 2026-09-09)
and the rate-control search and exponent-run planner made cheaper the same day. What is left of
the gap is the search - exponent-run planning, the allocation probes, mantissa bit counts -
which is integer work the decoder does once a block. On an ESP32-S3 the board encodes AC-3 2/0
at 0.35x real time, E-AC-3 2/0 at 0.73x, AC-3 5.1 at 1.01x and E-AC-3 5.1 at 1.74x - the
[ESP32-S3 page](platforms/bare-metal/esp32-s3.md#encoding) has the six rows and the stage tables, and the
first platform choice on the search, `delta_allocation`.

`eac3_tools` is the row that reaches the coupling, spectral-extension and AHT encoders at all:
the 5.1 row's default is no tool. It is 2/0 with its band edges pinned (`cplbegf` 0, `spxbegf`
7), and `encode_fixture.hpp` has the two findings behind that shape, with the host profile's
numbers:

| Shape | Peak heap (host) | Fits an ESP32-S3's encode build (241,664-byte largest free run)? |
|---|---:|---|
| 5.1 at 256 kbit/s, spx alone | 205,718 | Yes |
| 5.1 at 256 kbit/s, standard coupling alone | 289,202 | No |
| 5.1 at 256 kbit/s, AHT alone | 312,744 | No |
| 5.1 at 256 kbit/s, all three | 369,790 | No |
| 5.1 at 384 kbit/s, §E3.5 enhanced coupling | 343,483 | No (the ecpl row's own finding) |
| 7.1.4 at 640 kbit/s through `AccessUnitEncoder` (a bed and two dependents, 14 coded channels) | 601,954 | No - three encoders resident at once, 416 allocations a frame |
| The Atmos object encoder | about 300,000 | No, and it is not in the profile |

And at 2/0 with both merely permitted, the rate defaults start spectral extension below where
coupling would begin and §E3.3.1 then drops coupling, so the frame is spx + AHT - the pinned
edges are what keep all three live, which `forge probe` confirms on the frame. The Atmos
figure is a bench estimate rather than a probe row: `iclforge-membench` shows `AtmosEncoder`
constructing with 138,743 bytes live against the plain E-AC-3 encoder's 58,912 and its first
frame allocating what that encoder's does, which puts it about 80 KB above the 5.1 row - and
its per-frame QMF analysis of the bed and every object is `double` as well. What the part can
encode is therefore one substream at a time, 5.1 with no tool or 2/0 with any, in the
configurations the six rows are.

Two things about the probe differ from the decode one, and both follow from the direction:

- **The input is synthesised.** A decoder's fixture is a 10,752-byte bitstream; an encoder's is the
  221,184 bytes of PCM behind it, which is most of an ESP32-S3's internal SRAM. Six sines at
  non-harmonic frequencies, computed in `double` with a single narrowing to `float`, so every IEEE
  target produces identical samples.
- **The check is a checksum**, because there is no decoder in this profile to reconstruct with. It
  says the target produced what the host produced from the same input. Measured, the two agree
  exactly — so this project's encoder is bit-exact between x86_64 hardware doubles and
  `arm-none-eabi` soft float, which extends what
  `tests/golden/bitstream-hashes.json` already pins across x86_64 and aarch64 to a target with no
  FPU at all.

Steady-state churn is **34 to 87 allocations per frame on the 2/0 rows, 67 for AC-3 5.1 and 173
for E-AC-3 5.1**, against the decoders' 1–35, and the runner gates it at 260. That gap is in the
API rather than the implementation: both encoders return `std::vector<std::byte>` from
`encode_frame`, and there is no `encode_frame_into` to match `decode_frame_into`. It is the same
zero-heap gap [above](#gaps) records for the decode side, wider here, and it is the thing to close
before this profile is fit for a real-time encode.

The measured numbers are in [the footprint table](performance-trend.md#minimum-footprint-decoder).
CI runs this in the `esp` lane (`build-footprint` in `.github/workflows/_build.yml`): after a merge
that changes the probe, the ESP-IDF component or a tree the component ships, and in the nightly run
([CI lane partitions](ci-lanes.md)).

### The AC-4 decoder in the profile

AC-4 shares no bitstream syntax with AC-3 and E-AC-3, so the profile carries it as a build of its
own, with a probe of its own. `ICLFORGE_MINIMAL_AC4=ON`, which needs
`ICLFORGE_MINIMAL_DECODER`, builds `iclforge::ac4dec`, its inspector `iclforge::ac4` and the core
`iclforge::ac4core` as static libraries without exceptions or RTTI, in `float` (`ICLFORGE_DECODE_SCALAR`),
and `apps/baremetal/ac4_probe.cpp` in place of the AC-3 and E-AC-3 probe. The AC-4 encoder is not
built, and `ICLFORGE_BUILD_AC4` stays off in every minimal preset (it also builds the encoder, the
applications and the tests).

```bash
tools/checks/run_baremetal_probe.sh --ac4              # arm-none-eabi under QEMU
tools/checks/run_baremetal_probe.sh --ac4 --host       # natively
tools/checks/run_baremetal_probe.sh --ac4 --icount     # instructions per frame, gated
```

The presets are the ones above with `-ac4` after `minimal`. The probe decodes six committed
streams, gates each channel's level, the image, the peak heap (each fixture's own ceiling), the
stack a decode used (read by painting a window of it before the decode and looking for what changed
after), the allocations per frame and, under `--icount`, the instructions per frame; the
measured rows and their ceilings are in [the AC-4 table](performance-trend.md#the-ac-4-decoder).
The ESP-IDF component leaves the option off until its own switch sets it, so a board build does
not carry AC-4 by default.

### Gaps

One of the profile's requirements is not met, and is recorded here rather than half-enforced: no
heap traffic in the decode loop. The float32-only path is met for the decode path, and the retained
scratch below has since been closed; both are kept here with what they cost and what closed them.

**No heap traffic in the decode loop — not met.** The profile does not allocate the output PCM
(`decode_frame_into`/`decode_access_unit_into` write through caller-owned spans, and the
`_by_block` forms hand the decoder's own storage over a block at a time, which is what the probe
uses) and no frame leaks (what stays live after teardown is the bounded scratch below,
not per-frame growth), but the steady state is **3 allocations per frame for AC-3 5.1 (1 for its
2/0 and 1/0 rows), 12 for E-AC-3 and for E-AC-3 with §E3.5 enhanced coupling, 10 for 2/0, 20 for an
Atmos bed, 31 for Atmos with objects and 35 for 7.1.4**. The per-block geometry vectors inside the decoders no longer account for any of it —
they are `Impl` members, reused frame to frame. What is left is the `std::vector` members of
the returned `DecodedFrame`/`DecodedSubstream` (`blksw` is AC-3's whole remainder, `channels`
is 7 of E-AC-3's 12) and, on the Atmos fixtures, the EMDF payload chain. Reaching zero means
the first of those becoming fixed-capacity or pooled storage, which changes the public types —
a design change, not a build option. The runner gates the number at 100 for every fixture, with
no exemption ([the footprint table](performance-trend.md#minimum-footprint-decoder) has the
detail), so the distance from zero cannot grow while the gap is open.

**Scratch that was never released — closed.** `eac3_tools.cpp` kept enhanced coupling's
32,768-byte `EcplSpectrumScratch` and its 1,440-byte bin-angle vector in `thread_local` storage,
so §E3.5 neither allocates per call nor puts 32 KB on the stack. On a target whose only thread
never exits, the destructor that would release them never runs, and 34,232 bytes stayed live for
the life of the decoding task. That was bounded and paid once, so it was never the heap gap above
— but on an ESP32-S3 it was 32 KB of internal SRAM that object reconstruction then had nowhere to
fit into.

`iclforge::ac3::eac3::release_ecpl_scratch()` hands them back and the next call rebuilds what it needs. The
probe calls it between fixtures, and retained bytes at exit went from 34,232 to 24, and to **12**
once the bin-angle vector became a stack array. What is left is one `__cxa_thread_atexit`
registration record, for the pointer to the spectrum scratch — the one `thread_local` the library
still declares, and 23,552 bytes on this profile in its float form rather than the 32,768 above.
Both runners gate it at 1,024 — deliberately tight, because nothing here grows a little: either
the scratch is handed back or it is not, and the difference is five figures.
[The ESP32-S3 page](platforms/bare-metal/esp32-s3.md#objects) has what it unblocked.

**A float32-only path — met for the decode path.** `src/ac3/variants/decode-scalar-{float32,float64}/`'s
seam carries `decode_scalar_t`: `float` under this profile, `double` by default in every other
build, and selectable there with `-DICLFORGE_DECODE_SCALAR=float`. Both
decoders' coefficient stores, transform scratch and overlap-add history follow it, and
`imdct512_windowed`/`imdct256_pair_windowed` have float32 overloads built from the same templated
body as the double ones, so §7.9.4.1 is implemented once.

For a while that was the buffers only. The arithmetic between the bitstream and them - mantissa
dequantisation and the 2^-exp scale, dither, coupling and spectral-extension coordinates,
decoupling, the whole of spectral-extension synthesis, the AHT's dequantiser and six-point
inverse, and JOC's object mixing - stayed `double` and was narrowed at the store. On a desktop
that costs nothing; on the ESP32-S3's single-precision FPU every one of those operations was a
call into the ROM's software routines, and a board profile on 2026-09-09 found them to be 80% of
a 5.1 E-AC-3 decode ([the ESP32-S3 page](platforms/bare-metal/esp32-s3.md#timing) has the stage table). Those
paths now run in `decode_scalar_t` too, through templates whose `<double>` instantiations are the
exported functions the ordinary build always called, so its arithmetic is unchanged. What still
runs in `double` on this profile is stated rather than hidden: the per-block DRC gain, and the
output stage's gains and mix coefficients - the stage's per-sample arithmetic, the dialnorm
scale, the folds, the Hilbert phase shift and RF mode's protection, followed on 2026-09-10.
Enhanced coupling's reconstruction followed in a second pass - its routines
are shared with the encoder, so they exist in both scalars now, the double forms being the
encoder's - and with it the last of the decode path is in `decode_scalar_t`.

**A fixed-point decode path, for parts with no FPU.** The third value of the same axis,
`-DICLFORGE_DECODE_SCALAR=fixed`, carries `decode_scalar_t` as `iclforge::internal::Fixed32`
(`src/arithmetic/include/iclforge/arithmetic/fixed32.hpp`): a signed 32-bit integer read as Q7.24, products through 64
bits and rounded once, sums wrapping, conversions saturating. It is the tier for an ESP32-C3 or
a Cortex-M3, where even `float` is a compiled subroutine, and the minimum-footprint profile
honours it (every other value of the option is `float` there). The ESP-IDF component
(`esp-idf/iclforge/`) sets it for a part with no FPU when a project has not set the option
itself, and `float` for a part with one. What the tier does, in the order
the decode runs: dequantisation, dither, coordinates and decoupling in `Fixed32`; a coupling
or spectral extension coordinate kept as its mantissa and its power of two, so the product with
a coefficient is a shift; the §7.9.4 inverse pair as its own kernel
(`src/ac3/src/core/mdct_fixed.hpp` - the same pre-twiddle, N/4-point FFT, post-twiddle and window as the fast
branch, with no scaling inside the transform: the input's bound gives the seven bits the FFT can
grow by); the overlap-add in 64 bits with one float conversion at the end; and Annex E's own
tools - the adaptive hybrid transform's dequantisers and six-point inverse, the spectral
extension notch, and enhanced coupling's spectrum, amplitudes, angles and reconstruction
(`src/ac3/src/core/eac3_tools_fixed.hpp` and the bodies beside the floating ones in
`eac3_tools.cpp`). Enhanced coupling's 512-point DFT is the one place the tier does scale inside
a transform: an unscaled one can grow by nine bits where the format has seven, so its stages
shed bits only where the next would otherwise overflow and what they shed is carried in the
exponent, the spec's own 1/N with it. On the Cortex-M3 leg that row is
10.1 M instructions against the float tier's 28.9 M.

What is not in the tier: JOC's object reconstruction, which runs in `float` in every build of
this library including the double one (`recon_scalar_t`), so it is not a seam of this tier's at
all - what the tier does with it is convert each matrix coefficient it reads.

What makes the precision is not the word but the exponent. Q7.24 is an absolute format - a
raw unit is 2^-24 of full scale wherever a value sits - and stored directly, a quiet dense
channel came out 99 dB from the double decode and a coupled one 88, the mantissas' bits lost at
dequantisation. So each stream's coefficients are stored under a block exponent
(`src/ac3/src/decoder/block_norm.hpp`): scaled up so the largest sits just below one half,
which is the transform's precondition, and every mantissa keeps all of its bits. The exponent
travels with the block - a tool that needs more room lowers it where it runs, an AHT stream's
is exact from its reconstructed peaks - and the overlap-add aligns the two halves it sums before
the conversion applies the power of two exactly. Measured with
`tools/checks/check_decode_scalar_snr.py` on 2026-09-10: 121, 122 and 122 dB on the worst
channel of the three gold streams, and no channel of the thirteen third-party streams then checked in
(Dolby Encoding Engine and FFmpeg, AC-3 and E-AC-3, with coupling, spectral extension and the
AHT) below 111 dB. The gold-reference gate passes with the fixed CLI at the same floors as the
double one, and its encoder - `encode_scalar_t` is a separate axis - writes the pinned bitstreams
byte for byte. The tier's own gate is a different kind: integer arithmetic is the same on every
machine, so the bare-metal probe's `<codec>.pcm_hash` lines are identical on the x86 host and
the Cortex-M3 leg, and CI holds both to the pinned ones in
`tests/golden/fixed-probe-pcm-hashes.json` (`tools/checks/check_probe_hashes.py`). The
Catch2 suite is not one of its gates: two of the encoder's mirror self-checks compare the
encoder's model against a real decode at a tolerance set for the double decoder, and fail under
the fixed one. The plan, phases, and measurements are in
[`planning/arithmetic-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/arithmetic-tiers.md).

**And the encoders' analysis front end, on its own axis.**
`src/ac3/variants/encode-scalar-{float64,float32}/` carries `encode_scalar_t`: the type
the two encoders run in, from transient detection and the forward transform through the
coefficient store, the coupling, spectral-extension and enhanced-coupling analyses and fits, the
dither and delta-segment decisions and the fixed-point conversion - `double` by default, `float`
under this profile, and selectable in any build with `-DICLFORGE_ENCODE_SCALAR=float`. A second axis rather than a second alias beside
`decode_scalar_t`, for the reason that one was split from the profile: a full build with either
scalar `float` and the other `double` is what lets the float front end's bitstreams be decoded
and measured against the double encoder's through the CLI and the oracles. What is not in it:
the adaptive hybrid transform (its six-block DCT and vector quantiser are `double`), the masking
model's own arithmetic, and the allocation search, which is integer. `TransientDetector` is the
`double` instantiation of `BasicTransientDetector<Scalar>` and `DitherBallot` of
`BasicDitherBallot<Scalar>`; `to_fixed25` is a template, exact in either scalar for the same
reasons; `to_fixed25_block`, `accumulate_peak_exponents`, `choose_delta_segments` and
`PerceptualModel::analyse` have `float` overloads; the short-block forward pair and the two peak
meters that read the overlap history take either. The float path's `log2` and `exp` are the
project's own (`src/arithmetic/include/iclforge/arithmetic/scalar_math.hpp`: a bit-level `frexp` and a short series,
Cody-Waite reduction and a short series), because the profile's fixture hashes are checked on
the x86 host, the Cortex-M3 leg and the ESP32-S3 and three C libraries' `logf` do not agree in
their last bit; the `double` overloads are libm's, called as before. Every `<double>`
instantiation is the function the ordinary build always called, so
`tests/golden/bitstream-hashes.json` holds; the profile's own fixtures
(`apps/baremetal/encode_fixture.hpp`) are the float encoder's streams, identical on the x86
host, the Cortex-M3 leg and the ESP32-S3 - and the same bytes the float front end alone had
produced, so converting everything behind it moved no fixture's hash.

Why the front end first: on the ESP32-S3, with the encoders wholly in `double`, the forward
transform and transient detection were 64% of an AC-3 5.1 frame and 36% of an E-AC-3 one. With
them in `float` the board encoded AC-3 2/0 in 28.3 ms of its 32 (0.88x, from 2.34x over) and
AC-3 5.1 in 71.0 ms (2.22x, from 6.25x); E-AC-3 5.1 went from 349 ms to 220. With the rest
converted the same day: AC-3 2/0 in 12.1 ms (0.38x), E-AC-3 2/0 in 33.8 (1.06x), AC-3 5.1 in 35.1
(1.10x), E-AC-3 5.1 in 81.2 (2.54x), and the §E3.5 row from 426 ms to 55. The
[ESP32-S3 page](platforms/bare-metal/esp32-s3.md#encoding) has every row and the stage table; what remains
is the integer search. CI's `linux-gcc` leg builds this scalar's full CLI beside the float
decoder's to run its streams through the gold-reference gate and
`tools/checks/check_encode_scalar_quality.py`, which holds the float encoder's worst channel to
within 0.5 dB of the double encoder's on five gold streams (they are identical to the hundredth
of a decibel), and `tests/golden/bitstream-hashes.json` pins its three streams on x86-64 under
the `encfloat` mode.

No gold-reference number moved, because the choice is per-profile rather than global. The
ordinary build's `decode_scalar_t` is `double`, so its arithmetic is unchanged and the suite
passes identically.

The oracle run was done separately, and for a long time it could not be done again. `decode_scalar_t` used to live in `iclforge/ac3/detail/profile.hpp` alongside the profile's
other facts, so `float` was reachable only in a build that was also decode-only, exception-free
and without a CLI — there was no float32 `forge` any preset could produce, and the ~139 dB
figure came from one made by hand. Which profile a build is and which scalar its decoder carries
are independent questions, and they are two CMake axes now.

So the number is a gate rather than a recollection. `tools/checks/check_decode_scalar_snr.py`
decodes the three streams `verify_gold_reference.sh` encodes with both builds and holds the worst
channel to 120 dB:

| Stream | Worst channel |
|---|---|
| `gold.ac3` | Ls, 138.85 dB |
| `gold.ec3` | Rs, 139.05 dB |
| `gold_cpl.ec3` | Rs, 138.98 dB |

The floor sits well below those on purpose: it is there to catch a float32 path that has broken,
not to police the last decibel of a figure already under the double decode's own quantisation
noise. For scale the gold-reference gate's tightest per-channel floor is 60 dB, and at the
transform alone the disagreement is 2.7e-7 peak-normalised (`tests/ac3/core/test_mdct_fast.cpp`),
about one LSB at 24 bits.

What that gate does **not** say is whether either decode is right — two builds agreeing says only
that they agree. `verify_gold_reference.sh` itself is the other half, and CI runs it against each
variant: the `scalar_variants` passes of the Linux GCC leg (in the nightly run) build a float32
decoder, a fixed-point decoder and a float32 encoder as their own binaries and run the
gold-reference gate against each, beside their comparisons with the double build.

Two things it does not cover:

- The **encoder**. This gate compares decodes. The encoder's `float` front end has its own,
  `tools/checks/check_encode_scalar_quality.py` (above), and `tests/golden/bitstream-hashes.json`
  pins the encoder's bitstreams per kernel and transform mode: the `x86_64-sse2`, `aarch64-neon`
  and `generic` kernels with the fast transform, the two that have a reference transform, and the
  float front end on x86-64 (`encfloat`).
- The **transforms' direct form** and the QMF bank. The float32 forms of the transforms take no
  `fast` parameter: the direct form is the spec's own evaluation and the oracle the fast path is
  validated against, so it stays double-precision, and the QMF bank keeps its history in `double`
  (`iclforge/dsp/qmf.hpp`) whatever the decode scalar. JOC's object reconstruction is `float` in every
  build (`recon_scalar_t`). Both directions have float32 fast paths otherwise: the forward's exist
  for `oba::joc`, which analyses the bed inside a *decode* before un-mixing it — the only forward
  transform a decode runs — and for the encoders' float front end.

The profile still exercises the `double` path without hardware floating point. `decode_scalar_t`
is a profile choice, so an `arm-none-eabi` build of the ordinary profile software-emulates every
operation as before.

**`-fno-exceptions` removes the tables, not the throw sites.** The codec has no `throw`, `try` or
`catch` of its own. What remains is the standard library's: `std::vector`'s `length_error` and
`bad_alloc`, which under `-fno-exceptions` become `std::terminate`. That is the correct behaviour
for a decoder that has run out of memory on a device with no swap, but it is termination rather
than a return, and closing it properly is the same design change as the heap gap above.

## Building on Linux

`config-linux-gcc` and `config-linux-llvm` (each with a `-debug` variant, same as the Windows
presets) are GCC 16 and Clang 22 respectively. They do **not** share the `debug`/`release` bare
names used elsewhere in this document — there is no `cmake --preset debug` on any platform; see
[Presets](#presets) above.

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-linux-gcc-debug
cmake --build --preset build-linux-gcc-debug
ctest --preset test-linux-gcc-debug
```

Substitute `linux-llvm` for `linux-gcc` to build with Clang instead. `VCPKG_ROOT` works the same
way as on Windows: it must point at a vcpkg checkout for the toolchain file the preset
references, even though (as on Windows) it supplies nothing but fmt and Catch2. Any path
works — there is nothing Linux-specific about vcpkg here.

### GUI on Linux

Both Linux presets default `ICLFORGE_BUILD_GUI` to `OFF`. That is not because the GUI cannot be
built on Linux — `cmake/FindQt6.cmake` resolves a Linux Qt kit the same way it resolves a
Windows one (distro packages land on CMake's own prefixes; relocated or `aqtinstall` kits are
searched under `~/Qt`, `/opt/Qt` and friends), and `forge-gui` builds clean and passes its headless
`--smoke` run under both Linux presets. It defaults off because, unlike on Windows/macOS, a Qt
kit is not assumed to be present on every Linux machine that builds this project — see
`linux-gcc`'s own `description` in `CMakePresets.json`. Opt in explicitly once Qt is installed:

```bash
cmake --preset config-linux-gcc-debug -DICLFORGE_BUILD_GUI=ON
```

Qt 6.5+ is required, same as everywhere else. On Debian/Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools
```

Other distros need the equivalent Qt6 base + declarative (QML/Quick) development packages;
package names vary (Fedora's are `qt6-qtbase-devel` / `qt6-qtdeclarative-devel`, for example).

**The `qmlshapesplugin` / `labsmodelsplugin` / `qmlfolderlistmodelplugin` CMake warnings.**
Configuring with the GUI on prints warnings that these — and, in fact, every other built-in
QML plugin `forge-gui` transitively touches through `QtQuick.Controls` (its styles, dialogs,
layouts, and so on) — "will not be linked", because the `Qt6::<name>plugin` CMake target each
would need does not exist. This is a property of how Ubuntu's apt-packaged Qt6 is built, not of
this project: the official Qt installer exports a static-link CMake target for every built-in
plugin so a fully self-contained executable can embed them, but a distro's dynamically-linked
Qt6 package does not need that and does not export it. It does **not** mean the plugins are
missing. Confirmed on Ubuntu 26.04's Qt 6.10: the `.so` files are installed at the normal QML
import path with valid `qmldir` files, and the QML engine loads them from there at runtime the
same way it loads every other Qt Quick module, independent of whether CMake could statically
link them in. `forge-gui`'s own QML never imports `Qt.labs.*` or `QtQuick.Shapes` directly — the
three named in the warning are pulled in transitively by `QtQuick.Dialogs`' non-native
fallback implementation, which backs `Main.qml`'s `FileDialog`s only when no native/portal
dialog is available, and which a headless `--smoke` run (verified with
`QT_LOGGING_RULES=qt.qml.import.debug=true`) never even requests. Safe to ignore.

### Linux audio

Three of ICL Forge's features touch the sound hardware — live capture (`forge devices`,
`record`, `live`), monitor playback (`monitor`, and `identify`, which walks a test tone across an
output's speakers), and IEC 61937 bitstream passthrough (`outputs`, `play`). `play` decodes an
AC-4 stream and plays it as PCM, since no receiver takes AC-4 over IEC 61937 yet. (`spatial`, the
fourth audio command, is Windows-only.) Everything else is file I/O and needs no audio stack at
all; `forge spdif` in particular reaches an AV receiver by writing a WAV, on any machine.

On Linux those three are implemented over **ALSA** when its headers are present, and over
**PipeWire** when they are not but PipeWire's are — see [Why ALSA still comes
first](#why-alsa-still-comes-first) for the precedence between them. Installing ALSA's headers
is one package:

```bash
sudo apt-get install libasound2-dev
```

(`alsa-lib-devel` on Fedora, `alsa-lib` on Arch.) Nothing else is needed: no PulseAudio
development headers, no vcpkg port, no runtime daemon. Recording from the ALSA `default` device
goes through PipeWire or PulseAudio automatically wherever one is running, because that is what
those install themselves as.

PipeWire's headers are the alternative, for a machine without ALSA's:

```bash
sudo apt-get install libpipewire-0.3-dev
```

(`pipewire-devel` on Fedora.) Located via pkg-config rather than a CMake find module, since
PipeWire ships none of its own.

Both dependencies are **optional and detected**. Configure reports which one it picked:

```
-- ALSA 1.2.15.3: live capture, monitor playback and IEC 61937 passthrough enabled
--   Audio backend  : alsa
```

```
-- PipeWire 1.6.2: live capture and monitor playback enabled; IEC 61937 passthrough negotiates
   for real but needs a compressed codec enabled on the target node by the session manager first
   - see src/audio/src/backend/pipewire/passthrough.cpp
--   Audio backend  : pipewire
```

Without either set of headers, configure succeeds anyway and says so; the build then selects
`src/audio/src/backend/posix/`, whose entry points all return `kNoBackend`, and `forge` marks
the affected commands `UNAVAILABLE HERE` in its usage rather than pretending they exist. Pass
`-DICLFORGE_WITH_ALSA=ON` and/or `-DICLFORGE_WITH_PIPEWIRE=ON` to turn a missing set of headers
into a configure error instead, which is what a packaging build wants.

#### Why ALSA still comes first

Capture and monitor playback are ordinary PCM and both backends do them for real — native
`pw_stream` on PipeWire's side, not its ALSA-compatibility shim. Passthrough is the
discriminator, and it is what the whole project is for: sending an AC-3 or E-AC-3 elementary
stream down an S/PDIF or HDMI link so the receiver decodes it.

That is not a "format" on Linux the way it is on Windows. A bitstream is opened as plain 16-bit
stereo PCM, and what tells the receiver these bytes are Dolby Digital rather than music is the
IEC 60958 **channel status** travelling beside them — specifically the non-audio bit, AES0
bit 1. ALSA is where that bit is expressed (as arguments on the device name,
`iec958:CARD=PCH,DEV=0,AES0=0x06,…`), and it works the moment compatible hardware exists — no
extra configuration.

PipeWire has its own real, current, native mechanism for the same thing —
`SPA_MEDIA_SUBTYPE_iec958`, `spa_format_audio_iec958_build()`, `PW_STREAM_FLAG_EXCLUSIVE` — not
aspirational API surface; `src/audio/src/backend/pipewire/passthrough.cpp`'s own header comment cites a
real shipped client (Kodi's PipeWire passthrough support) that negotiates exactly this way. What
it does not have is ALSA's "just works": a PipeWire sink only offers a compressed codec once its
`iec958.codecs` property has been populated by the session manager. WirePlumber does that on its
own, from the display's EDID: on 2026-09-05 a stock Raspberry Pi OS desktop that nobody had
configured brought its HDMI sink up with `[PCM, DTS, AC3, EAC3, TrueHD, DTS-HD]` the moment an
Atmos receiver was on the cable, and this library's PipeWire backend then streamed E-AC-3 bursts
to it. What remains configuration — a WirePlumber ALSA-monitor rule, or a one-off `pw-cli`
call — is the sink that advertises nothing, or a session manager that does not read EDID, and
that this library has no portable way to perform on a caller's behalf. The precedence below is
about that remainder, and about the fact that the exact same hardware is reachable directly
through ALSA underneath the very PipeWire daemon that's running.

That is why ALSA keeps first precedence in `src/audio/CMakeLists.txt` whenever both are found,
rather than PipeWire winning by default for being the modern norm on most current desktops:
preferring it unconditionally would silently regress `forge outputs`/`play` on exactly the
common case where nobody has configured `iec958Codecs`. The explicit escape hatch for a machine
where PipeWire's compressed codecs are configured is
`-DICLFORGE_WITH_ALSA=OFF -DICLFORGE_WITH_PIPEWIRE=ON`, the same shape `-DICLFORGE_WITH_ALSA=OFF`
alone already has today.

#### What has and has not been verified

**ALSA.** Verified on WSL2 Ubuntu 26.04 with the local development loop's gcc 15.2 and clang 22.1
(CI's own Linux legs pin GCC 16 — see [Requirements](#requirements); this record was not re-run
for this revision), in every configuration:
with libasound present and absent, and under ASan+UBSan with leak detection. The full suite
passes in all of them. The device-independent halves of the backend — device-name construction,
channel-status derivation, the negotiation, the render and capture threads, start/stop, and the
error mapping — were additionally driven end to end against ALSA's software `null` PCM.

**What real hardware has and has not shown.** ALSA device enumeration has since run on real
hardware — a Raspberry Pi 4B enumerated and correctly classified its `vc4hdmi` HDMI outputs
(see [Raspberry Pi](platforms/raspberry-pi.md#verified-configuration)). What remains unverified
on Linux is bitstreaming to a real receiver: WSL2 has no sound devices and no kernel sound
modules, so nothing here has been played to an actual S/PDIF or HDMI output, and no AV receiver
has been asked to lock onto the result. Whether a given output accepts a bitstream is
per-device anyway — `forge outputs` probes each one and says.

**PipeWire.** Verified on the same WSL2 Ubuntu 26.04 host with libpipewire-0.3 1.6.2, gcc 15.2
and clang 22.1, with `-DICLFORGE_WITH_ALSA=OFF -DICLFORGE_WITH_PIPEWIRE=ON` forcing the
selection (WSL2's image has both sets of headers installed, and ALSA wins by default — see
above). The full suite passes on both compilers. There is no PipeWire session running in that
environment at all (no `pipewire`/`wireplumber` daemon, confirmed by `pw_context_connect()`
failing fast rather than hanging), so `enumerate_devices()`/`enumerate_render_devices()` were
exercised against that "no session" path — returning an empty list rather than erroring, the
PipeWire-side equivalent of ALSA's "no sound card" case — not against a real graph with real
nodes to enumerate, negotiate with, or bitstream to. Nothing here has connected to a live
PipeWire session, requested a compressed format from a real node, or been played to a real
receiver.

## Qt

Qt is a **prebuilt dependency and never a vcpkg port**. Building Qt from source through vcpkg
takes hours and produces a kit that is harder to debug against than the official one.

`cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the usual prebuilt-kit install roots — on
Windows `C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`; on Linux and macOS `~/Qt`, `/opt/Qt`, Homebrew and
MacPorts prefixes, and so on — newest kit first, and then defers to Qt's own config package. Every
Linux and macOS preset still forces `ICLFORGE_BUILD_GUI=OFF` by default — pass
`-DICLFORGE_BUILD_GUI=ON` explicitly on a machine that has Qt 6.5+, which is verified to work on
Linux both locally (see [GUI on Linux](#gui-on-linux) above) and in CI, which installs a Qt6 kit
and turns the flag on for the four Linux build legs (x64 and arm64, GCC and Clang) plus both
macOS legs (the official kit via `install-qt-action`, not Homebrew's `qt` formula — see
[macOS](platforms/macos.md#gui-on-macos)). See
[Verified configuration](#verified-configuration). If your kit is somewhere else, say so explicitly and it
wins over the search — the project's own `-DICLFORGE_QT_ROOT=` (or the `ICLFORGE_QT_ROOT`,
`QT_ROOT_DIR` or `QTDIR` environment variables) is the preferred way:

```bash
cmake --preset config-windows-msvc-debug -DICLFORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

Plain `-DCMAKE_PREFIX_PATH=...` or `-DQt6_DIR=...` also work, and take priority over everything
`FindQt6.cmake` does. If you do not want the GUI, `-DICLFORGE_BUILD_GUI=OFF` removes the
dependency entirely.

## Packaging

`cmake/Packaging.cmake` wires CPack up behind the platform preset matrix. A plain ZIP is
always produced; NSIS (Windows), TGZ and DEB/RPM (Linux) and DragNDrop (macOS) are added on top
when the packaging tool for that format is found on `PATH`, so `cpack` degrades gracefully instead
of failing outright on a machine that does not have e.g. `makensis` installed.

From a Developer PowerShell, with `VCPKG_ROOT` set:

```bash
cmake --preset config-windows-msvc
```

```bash
cmake --build --preset build-windows-msvc
```

```bash
cpack --preset pack-windows-msvc
```

The equivalent `pack-<platform>` preset exists for every entry in the platform matrix
(`pack-windows-msvc-arm64`, `pack-windows-llvm`, `pack-linux-gcc`, `pack-linux-llvm`,
`pack-linux-gcc-arm64`, `pack-linux-llvm-arm64`, `pack-macos-llvm`, `pack-macos-llvm-x64`). A pack
preset reuses whatever the matching build
tree was configured with — on a non-Windows preset that includes the GUI only if you opted in
(`-DICLFORGE_BUILD_GUI=ON`, which is exactly what CI's Linux and macOS packaging legs pass — see
[GUI on Linux](#gui-on-linux) and [macOS](platforms/macos.md#gui-on-macos)). Beyond `windows-msvc`'s
packaging in every run of `ci.yml` that builds it, the
`release_package` legs have run for real on tagged releases, and `pack-linux-gcc-arm64` has
additionally been run by hand on a real Raspberry Pi 4B with the resulting `.deb` inspected —
see [Raspberry Pi](platforms/raspberry-pi.md#verified-configuration). Packages land in
`packages/` at the repository root.
`cmake --build --preset build-windows-msvc --target pack-iclforge` runs the same thing from
inside an IDE's target list instead of the command line.

`forge`/`forge-gui` and `iclforge::ac3`/`iclforge::matroska` are both installed and packaged, as
independent CPack components (`runtime`, plus `library`/`libruntime` for the codec itself) - a
second `iclforge-dev-*` archive alongside the usual end-user one, for a third party consuming the
codec via `find_package(iclforge)` rather than running it as a program; on Linux, `library`/
`libruntime` also become real `libFOO`/`libFOO-dev`-style DEB/RPM packages rather than only an
archive. See
[Using the libraries](library/index.md) for the CMake side and
[docs/releasing.md](releasing.md#what-gets-published) for exactly what ships where.
`iclforge::audio` (live capture/monitor/passthrough, `src/audio/`) stays link-only and unpackaged -
a CLI/GUI implementation detail, not part of either component.

CI packages the `windows-msvc` leg in every run of `ci.yml` that builds it and uploads the result
as a workflow artifact (`.github/workflows/_ci-windows.yml`), and the nightly run also packages
both macOS legs, so the packaging path is exercised continuously rather than only when someone
remembers to run it locally.

A tag-triggered release workflow (`.github/workflows/release.yml`) builds, signs, attests and
publishes packages for the four `release_package` legs — `windows-msvc`, `windows-msvc-arm64`,
`linux-gcc` and `linux-gcc-arm64` — plus a `package-macos-universal` job that `lipo`-merges
`macos-llvm`'s (arm64) and `macos-llvm-x64`'s (x86_64) install trees into one universal `.dmg`
rather than either leg packaging solo: one canonical build per OS/architecture, whenever a
`vX.Y.Z` tag is pushed; a packaging failure on any of them blocks the release like any other
required leg. See [docs/platforms/macos.md](platforms/macos.md#universal-binaries) for how
the macOS merge works. The release carries GPG signing (when the key is provisioned, which it is),
keyless Sigstore/OIDC build provenance, an SPDX SBOM, and a GitHub Release; ten beta releases
(v0.2.0-beta.1 through v0.10.0-beta.1) have shipped through this path for real. See
[docs/releasing.md](releasing.md) for the full process, including how to provision the GPG key.

There is also a staged vcpkg port at `packaging/vcpkg-port/iclforge/`
(`portfile.cmake`, `usage`, its own `vcpkg.json`), which is not in the curated `microsoft/vcpkg`
registry: it was submitted as pull request #53470, a draft with changes requested. It exposes a
feature for each library beside the codec — `matroska`,
`mp4` and `mpegts` for the container writers, `capi` for the C API, and `ac4`, `iab` and `iamf`
for the AC-4 libraries, the IAB reader and the IAMF writer — and declares no `default-features`,
so none of them is on by default: a plain `vcpkg install iclforge` gets the codec alone, and
`vcpkg install iclforge[matroska,mp4,mpegts]` or `iclforge[ac4]` (or any subset) opts in. The
staged Conan recipe at `packaging/conan/` has an option for each of the same.
[docs/releasing.md](releasing.md#vcpkg-port) records why — a
curated-registry port's default features may only enable behaviors, not additional public
APIs/targets. A consumer uses the installed package via `find_package(iclforge)` exactly as
[Using the libraries](library/index.md) documents. The per-release submission flow is in
[docs/releasing.md](releasing.md#vcpkg-port).

## The standards documents

`spec/` is gitignored: the standards are free to download but are not redistributed here.
The build does not need them — every table is already transcribed into the source. They are
needed only to re-run the generators in `tools/`.

To set that up, fetch:

| Document | Why |
|---|---|
| ATSC A/52:2018 | The master standard. E-AC-3 is normative Annex E. |
| ETSI TS 102 366 | Carries the EMDF metadata format in Annex H. |
| ETSI TS 103 420 | Joint Object Coding. |
| `ts_103420v010201p0.zip` | The TS 103 420 companion archive. The JOC Huffman tables are in `ts_103420_tables.c` inside it, and nowhere in the PDF. |
| ETSI TS 103 190-1 (V1.4.1) | AC-4 part 1: the bitstream syntax, the decoding process and the tables the AC-4 generators transcribe. |
| `ts_10319001v010401p0.zip` | The TS 103 190-1 companion archive. Every Huffman codebook's lengths and codewords are in `ts_103190_tables.c` inside it, unzipped to `spec/ts_10319001_attach/`. |
| ETSI TS 103 190-2 (V1.3.1) | AC-4 part 2: the immersive and personalized audio tools, A-JCC and A-JOC. |
| `ts_10319002v010301p0.zip` | The TS 103 190-2 companion archive: `ts_103190_tables_part2.c`, the A-JCC and A-JOC codebooks and the intermediate spatial format's rendering matrices, unzipped to `spec/ts_10319002_attach/`. |

Extract each PDF to page-marked text beside the PDF, with page separators of the form
`===== PDF PAGE n =====`. The generators locate tables by page. The two AC-4 table generators,
`tools/generators/gen_ac4_tables.py` and `gen_ac4_reference_tables.py`, read the extracted text as
`ts_10319001v010401p.txt` and `ts_10319002v010301p.txt` and the unzipped archives from `--spec-dir`
(`spec/` by default).

## Verified configuration

The Windows instructions in this document were run on:

| | |
|---|---|
| OS | Windows 11 Pro for Workstations 10.0.26200 |
| Compiler | MSVC 14.51.36231 (Visual Studio 2026 Community) |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.8.3 msvc2022_64 |
| vcpkg | checkout at `D:/vcpkg` |
| FFmpeg | 8.0.1 |
| Python | 3.14.6 |

Result: configure, build and `ctest` all clean — the full suite passes, windows-msvc and
windows-llvm both.

The Linux instructions were run on:

| | |
|---|---|
| OS | Ubuntu 26.04 (WSL2) |
| Compilers | GCC 15.2.0 (the default `gcc`) and Clang 22.1.x, both tried — this is the local development loop, not the CI pin. CI installs GCC 16 (`.github/toolchain/02-gcc-toolchain.sh`), which is what [Requirements](#requirements) and [Linux](platforms/linux.md#toolchains) state, and `g++-16` is installed beside GCC 15 on this machine: `linux.gcc.toolchain.cmake` prefers it, and its `find_program` fallback list is why an older GCC still configures and passes. |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.10.2, apt-packaged (`qt6-base-dev`, `qt6-declarative-dev`) |
| ALSA | `libasound2-dev`, both present and as the no-ALSA fallback — see [Linux audio](#linux-audio) |
| PipeWire | `libpipewire-0.3-dev` 1.6.2, present and forced selected (`-DICLFORGE_WITH_ALSA=OFF -DICLFORGE_WITH_PIPEWIRE=ON`) — see [Linux audio](#linux-audio) |
| vcpkg | checkout at `/opt/vcpkg` |

Result: configure, build and `ctest` all clean on both compilers, GUI and ALSA both included.
The base suite is `iclforge-tests` and `iclforge-perf`'s Catch2 cases plus one ctest entry per example
program; `ICLFORGE_WITH_ALSA`'s `tests/audio/backend/alsa/` adds its own cases (or, on a build that
selected pipewire/ instead, `tests/audio/backend/pipewire/` does), and the GUI's Qt Quick
Test harness (`forge_gui_qmltests`, `apps/gui/tests/CMakeLists.txt`) adds one more per `tst_*.qml`
suite under `apps/gui/tests/qml/` — unlike every other GUI-related target, that one
harness *does* register its own `ctest` entries, gated on both
`ICLFORGE_BUILD_GUI` and `ICLFORGE_BUILD_TESTS`. A Linux build with neither ALSA nor the GUI
runs the base suite; with the GUI on and ALSA off it matches Windows exactly. `forge-gui --smoke`
also runs clean headless (`QT_QPA_PLATFORM=offscreen`), encoding real audio and instantiating
real QML channel meters. See [Linux audio](#linux-audio) for what the ALSA verification did,
and did not (real hardware), prove.

A pull request and each merge-queue entry run the gate in `pr-gate.yml` (the static checks,
Linux GCC, and in the queue Windows MSVC); see [CI for many agents](ci-agentic.md). The legs
below run after the merge, on main.

CI no longer has one cross-OS build matrix. The build legs are data: `.github/ci/legs.jsonc`
lists them, `_build.yml`'s `plan-legs` job picks the ones a run needs, and the reusable workflows
`_ci-windows.yml`, `_ci-linux.yml` and `_ci-macos.yml` run each platform's list as their matrix
([CI for many agents](ci-agentic.md#the-legs)). There are 11 legs, each with a tier: the run after
a merge builds the six of tier `t2`, and the nightly run builds all 11 and adds the slow extra
passes the `t2` legs leave out ([The tiers](ci-agentic.md#the-tiers)).

| Runs | Legs |
|---|---|
| After each merge (`t2`) | windows-msvc, windows-llvm, linux-gcc, linux-llvm, linux-gcc-arm64, macos-llvm |
| Nightly only | windows-msvc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, linux-llvm-tsan (ThreadSanitizer over the `concurrency` ctest label, via `config-linux-llvm-tsan`), macos-llvm-x64 |

The four Linux build legs install the same Qt6/ALSA packages and build and smoke-test the GUI too,
and so do both macOS legs. One leg, `windows-msvc-arm64`, is still marked experimental, and still
packages for release.

Beside the legs, the run after a merge builds the satellite jobs whose own tree changed
(`build-android`, the Shield app's debug APK, the WebAssembly, Rust and ESP32 jobs), and runs
`adm-validate` (the opt-in ADM module), `hearth-validate` and the performance trend. The nightly
run adds `linux-appimage` (builds `forge-gui`'s self-contained AppImage in an older `ubuntu:22.04`
container and smoke-tests it in a second container that never had Qt installed at all — see
[Linux](platforms/linux.md#appimage)), coverage, the ABI gate, ffmpeg-validate and every satellite
whatever changed. A run at tier `all`, which the `ci:deep` label dispatches, adds the same. The static checks in `_static.yml` (ruff over every `.py`, shellcheck over every
`.sh`, actionlint over the workflows, all three pinned in `requirements/requirements-lint.txt`) run
in the gate, not after the merge; `python tools/ci/precheck.py` runs the ones that need no build on
your machine before a push. clang-tidy runs nightly against `main` from
`.github/workflows/static-analysis.yml`, on the Debug preset with `-warnings-as-errors='*'`, and a
finding opens a `nightly-analysis` issue instead of failing a pull request. ffmpeg-validate is a
separate, CLI-only linux-llvm build that runs FFmpeg as an independent oracle against the full
layout/tool/metadata option space (see
[CONTRIBUTING.md's Oracles section](https://github.com/iainchesworthlabs/iclforge/blob/main/CONTRIBUTING.md#oracles)) — a different question from the
[gold-reference gate](#gold-reference-correctness-gate) below, which every leg runs against one
fixed sample to check output *quality*; ffmpeg-validate instead checks that every option
combination produces a *structurally correct* stream at all, plus a numeric fidelity floor for
the Annex E tool combinations the one fixed gold-reference sample does not itself exercise.

The coverage job (nightly only) gates line and branch coverage per component, not as one blended
number, using the same GCC 16 pin as the other Linux legs; the floor table, the measurement each
floor was calibrated against, and why `src/audio` and `apps/common` sit on a hardware-class floor
(their device paths run headless against alsa-lib's software devices, but card enumeration
needs a real card) all live in `tools/checks/coverage_report.sh`, with the calibration history in the coverage job's own
comment in `_ci-core.yml`.

In the merge queue, for an entry that changes `src/`, `pr-gate.yml` calls `_compare.yml`. Its
`performance-compare` and `memory-compare` jobs build the benchmarks at the commit the entry is
queued on and at the entry's head, and write per-workload deltas to the job summary, using the same
soft and hard tiers `tools/ci/append_performance_history.py` applies on merge. They are
informational and have `continue-on-error`; a hard regression (twice as slow, or twice the heap
churn) reaches the separate, blocking `performance-gate` and `memory-gate` jobs, unless the pull
request carries the matching approval label. `iclforge-perf` also enforces its absolute real-time budget
on every leg whose test preset includes it, and the performance and memory trend jobs that run after
a merge fail at the same +100% relative thresholds.

The `abi-gate` job (`_ci-core.yml`, nightly only) runs on an advisory footing: it builds
`config-linux-llvm-shared` for HEAD and for the newest `v*` tag in a git worktree beside HEAD,
then runs `abidiff` between the two and checks the actual exported dynamic-symbol set
(`tools/ci/check_abi_symbols.py`, `nm -D --defined-only`) against the checked-in allowlist.
The comparison point is the last release tag, the release-notes view of how far the ABI has moved
this cycle; the job also has a mode that compares a pull request with its own merge base, which
never runs while pull requests go through the gate. `abidiff` runs under
`tools/ci/abi-suppressions.ini`, which drops the libstdc++ template instantiations that are not
part of any ABI this project controls.

Both checks report into the job summary and leave the job green, gated on a single
`ABI_ENFORCE: 'false'` job-level variable; [the interface freeze](library/api-stability.md) is what would make
the gate required, by flipping that one value. When enforcing, `abidiff` fails only on an
*incompatible* change — a pure addition passes. `Verify Status` does not read the job's result
either way.

`ABI_ENFORCE` deliberately replaces the `continue-on-error: true` this job used to carry.
That setting stops a failing job from failing the *workflow run*, but GitHub still reports the
job's own check run as `failure` — so the gate showed a red X on every pull request while
blocking nothing, and it hid build failures behind the same state as an expected
pre-1.0 ABI change. With it gone, anything unexpected in this job is red and a policy finding
is not.

No macOS host exists for this project, so `config-macos-llvm`/`config-macos-llvm-debug` are only
ever exercised by CI (`macos-latest`, Apple Silicon) — never locally — and
`config-macos-llvm-x64` on `macos-15-intel`, native Intel hardware, in the nightly run. The
`macos-llvm` CI leg is green:
configure, build and `ctest` all clean, using a Homebrew-installed LLVM
(`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's bundled clang) rather than
a version-pinned one — Homebrew's core `llvm` formula has no versioned sibling the way
apt.llvm.org or the official Windows installer do, so unlike the other LLVM legs this one tracks
whatever Homebrew currently ships. The gold-reference correctness gate
(`tools/checks/verify_gold_reference.sh` — see [Gold-reference correctness gate](#gold-reference-correctness-gate)
below) also passes: real SNR numbers from that CI run were 61.81/61.82 dB on macOS, against
67.84/67.82 dB on Linux and Windows for the same material - a real but modest cross-compiler
floating-point difference, comfortably clear of the 30 dB gate. `macos-llvm` now builds the GUI
too (Qt installed via `install-qt-action`, not Homebrew's `qt` formula — see
[GUI on macOS](platforms/macos.md#gui-on-macos)), which adds
the same per-suite `forge_gui_qml_tests_*` entries to that same suite the same way it does on Linux:
confirmed on a real run before the harness split into one ctest entry per `tst_*.qml` suite (see
`apps/gui/tests/CMakeLists.txt`), 582 ctest entries total, 100% passing, the GUI harness (then
still a single entry) in 39.74s (56.81s for the whole suite) — the first time that number had
existed for macOS at all, so there was no prior baseline to compare it against. Getting there
needed two real fixes, not just turning the option on: `QSG_RENDER_LOOP=basic` (`apps/gui/tests/CMakeLists.txt`,
`APPLE` only) for a Qt Quick threaded-render-loop deadlock that hung the suite outright before a
single test ran, and forcing the `Fusion` style in `qml_test_main.cpp` — matching what `main.cpp` already does —
for a second, narrower hang in a native `ComboBox` populated by real capture-device data once a
test entered live-session mode: the same native-style-under-offscreen fragility a comment in
`apps/gui/qml/Main.qml` already documents one earlier instance of, on Windows, in a different
control (a `Repeater`'s per-device `Button`, worked around there directly in QML rather than at
the style level). See `apps/gui/tests/CMakeLists.txt` and `qml_test_main.cpp` for the full detail
on both macOS fixes.

`src/audio/CMakeLists.txt` selects a real CoreAudio backend on macOS (`src/audio/src/backend/macos/`,
built on the Audio HAL — `AudioObjectID`/`AudioDeviceIOProc` — the same layer WASAPI and ALSA
occupy on their own platforms), not the no-backend stub it fell back to before. `ICLFORGE_BUILD_GUI`
still defaults off there (`macos-llvm` opts it on in CI the same way the Linux legs do — see
[GUI on macOS](platforms/macos.md#gui-on-macos)) — capture, monitor playback and IEC 61937
passthrough compile and link for real either way, and `iclforge-tests` exercises the backend's
device-free logic (format matching, sample conversion) directly. What CI cannot exercise is a
real device: the hosted runner enumerates
whatever HAL objects macOS itself reports and touches nothing beyond that, same as ALSA's own
"verified headless" story below — see [Linux audio](#linux-audio) for the general shape of what
that does and does not prove, and [macOS](platforms/macos.md) for the backend's own header
comments on where its research came from (three independent real-world CoreAudio passthrough
implementations, surveyed since no Mac is available to try it on directly).

The `-arm64` presets are exercised in CI on GitHub's hosted `ubuntu-24.04-arm` runner (real ARM
hardware, not QEMU; `windows-msvc-arm64` on `windows-11-vs2026-arm`) and, separately, on a real
Raspberry Pi 4B — see
[Raspberry Pi](platforms/raspberry-pi.md#verified-configuration) for the on-device numbers, which
are tracked there rather than duplicated here since that page is the canonical source for
Pi-specific hardware findings (real ALSA/HDMI device names, resolved compiler versions on Raspberry
Pi OS, and so on).

## SIMD kernels and the architecture tree

The codec's hot kernels are vectorised, and the vector types they are written against come from
a directory CMake chooses — never from an `#ifdef`. `src/arithmetic/variants/` holds
`arch-generic/`, `arch-x86_64/` and `arch-aarch64/`, each carrying one identically-pathed
`iclforge/arithmetic/detail/simd.hpp`; `src/arithmetic/CMakeLists.txt` puts exactly one of them on
`iclforge::arithmetic`'s include path, which `iclforge_ac3_objects` and `src/ac4core` link, so every `#include "iclforge/arithmetic/detail/simd.hpp"` in the
core resolves to it and no translation unit ever asks what it is being compiled for. This is the
same mechanism `src/base/variants/profiling-tracy_{enabled,disabled}/` uses for the
profiling seam and `src/audio/src/backend/<backend>/` uses for the operating system, and it is what
`tools/checks/check_platform_macros.ps1` exists to keep true (no preprocessor conditional anywhere
in `src/`, `apps/`, `tests/`, `fuzz/`, `examples/`, `tools/` or `python/`).

`ICLFORGE_SIMD` forces a directory; `auto` (the default) resolves `x86_64` on x86-64, `aarch64` on
arm64, and `generic` on everything else — 32-bit x86, WebAssembly, anything unrecognised.
`generic` is a complete scalar implementation, not a stub: it is the reference the other two are
measured against, and `-DICLFORGE_SIMD=generic` is what a reproducibility comparison should reach
for first when two machines disagree. The resolved value is printed by the configure summary and
by `forge --version`, which reads it from the compiled header rather than from a
CMake-substituted string, so a binary cannot claim a directory it was not built with.

**Cross-builds, and why `CMAKE_SYSTEM_PROCESSOR` is not the question `auto` asks.** On Apple
platforms `CMAKE_OSX_ARCHITECTURES` overrides `CMAKE_SYSTEM_PROCESSOR` per compile line, so the two
disagree whenever a Mac builds for the other architecture — and the compile line, not the host, is
what the kernels have to be right for. `auto` therefore takes `-arch` as the truth wherever one is
set: an Intel Mac configured with `-DCMAKE_OSX_ARCHITECTURES=arm64` resolves `aarch64` and not
`x86_64`. Reading the host variable instead
selects SSE2 intrinsics and an `-mavx2` flag for an ARM compile, which does not degrade quietly; it
fails the build outright with `clang++: error: unsupported option '-mavx2' for target
'x86_64-apple-darwin24.6.0'` — a diagnostic that names the host triple rather than the `-arch` the
flag is actually invalid for, which is why the real cause is easy to misread from the log alone.

This is not hypothetical: it is how `Build wheels (macos-15-intel)` failed when that runner was
first added and inherited a wheel config that cross-built arm64. That matrix now names each row's
architecture explicitly (`CIBW_ARCHS` in `.github/workflows/wheels.yml`), so nothing in CI
cross-builds today — but a hand-run `cibuildwheel`, a `-DCMAKE_OSX_ARCHITECTURES` configure, or any
future cross-targeting matrix row would hit the same gate, which is why it is fixed here rather
than only routed around there.

A macOS *universal* binary — more than one `-arch` from a single configure, so every source is
compiled once per slice — resolves `generic`, because no single compile-time architecture choice can
be correct for both slices at once. That is a real, if conservative, cost: universal builds get the
scalar kernels on both halves. Configure the two slices separately and `lipo` them together if you
want SSE2 and NEON in one binary.

**What is vectorised.** The kernels live once, in shared code, written against the two 128-bit
types the header defines (`f64x2`, two doubles; `i32x4`, four 32-bit integers) — the directories
carry the types, not a copy of each kernel:

| Kernel | Where | Note |
|---|---|---|
| DCT-IV pre/post twiddles | `src/ac3/src/core/mdct.cpp` | The complex multiplies either side of the FFT/DCT-IV core (`fft_kernel.hpp`, see the boundary note below). The core wants its input digit-reversed and returns its output in natural order, so the pre-twiddle gathers at stride ±2 and scatters to `bitrev[m]`, and the post-twiddle reads at stride +1 and scatters to stride ±2. Every gather and scatter stays scalar; only the arithmetic between them goes two-wide. Also has a real AVX2 tier — see [Runtime AVX2 dispatch](#runtime-avx2-dispatch). |
| IMDCT twiddle stages | `src/ac3/src/core/mdct.cpp` | Both inverses' pre- and post-transform complex multiplies, same gather/scatter-around-the-core shape as above for the pre-twiddle, unit stride for the post-twiddle. Also has a real AVX2 tier. |
| analysis windowing | `src/ac3/src/core/mdct.cpp` | 512 independent multiplies, unit stride throughout. Also has a real AVX2 tier. |
| `dft512` normalisation | `src/dsp/src/fft.cpp` | Multiply by the exact reciprocal of 512, which is the same correctly-rounded result as the division it replaced. SSE2/NEON only — Phase 1's own measurement (below) found no reproducible AVX2 signal, since this is a small slice of a function `fft_kernel.hpp`'s own unaccelerated core dominates. |
| §7.2.2.2 exponent to PSD | `src/ac3/src/core/bitalloc.cpp` | Four bins at a time. The only loop in the bit allocator that vectorises at all: §7.2.2.4's excitation function is a serial recurrence and §7.2.2.5's masking curve is a per-band conditional over 50 elements. SSE2/NEON only, same reason as `dft512` above. |
| `to_fixed25_block` | `src/ac3/src/core/exponents.cpp` | Batched form of `to_fixed25`, about 9,100 calls a frame. SSE2/NEON only, same reason as `dft512` above. |

**The FFT/DCT-IV core itself is not part of this seam.** `src/dsp/include/iclforge/dsp/detail/fft_kernel.hpp`
 is a radix-4 decimation-in-time kernel with a trailing radix-2 stage where
`log2(P)` is odd, trivial-twiddle elimination on its first stage, and the digit-reversal
permutation folded into each caller's own input-producing loop rather than run as a pass of its
own. That is an *algorithmic* speedup — fewer operations, not wider lanes — and it carries its
own correctness argument in that header's comment, independent of the seam described here. The
kernels this seam does vectorise sit around it: they gather from and scatter to its
digit-reversed layout rather than to sequential slots, which is why their gather/scatter ends
stay scalar even though the arithmetic between them is two-wide.

**What is not, and why.** The direct-form (`mode=reference`) MDCT and IMDCT are dot products, and
splitting a reduction into per-lane partial sums reassociates the additions — which changes the
result. Those paths are the normative oracle every fast path is validated against, so their
numbers are not something to trade for speed. `band_energy`'s per-band accumulation is a reduction
for the same reason (its cost is the MDCT inside it, which does get faster). Steps 1 and 5 of both
inverses are permutation-dominated. This seam itself stays SSE2-width on x86-64: AVX and FMA3 are
CPU features rather than architecture, so a compile-time `-march=` for them would produce a binary
that faults on older hardware. [Runtime AVX2 dispatch](#runtime-avx2-dispatch) below covers the
`cpuid`-gated mechanism that makes a *wider* tier safe to ship without that risk — the
dynamic-dispatch follow-on wired it to the analysis windowing and DCT-IV/IMDCT twiddle kernels in
the table above (the two Phase 1's own measurement found a real win for); `dft512`'s normalisation,
exponent-to-PSD and `to_fixed25_block` stay SSE2/NEON-only for the same reason. 128 bits is the native width of
NEON and of WASM's `simd128` in any case, and those are the platforms with the least headroom —
which is also why this dispatch mechanism is x86-64 only: NEON double-precision is mandatory
ARMv8-A baseline, so aarch64 has no equivalent feature gap to close.

**Why it is bit-exact.** Every operation in the seam is exactly one IEEE-754 add, subtract or
multiply per lane, so a kernel written against `f64x2` performs precisely the operations, in
precisely the order, that the scalar loop it replaced performed — it produces the same doubles,
not nearby ones. That matters more here than in most numerical code: encoded output is a bit-exact
function of those doubles, and a last-place difference in an MDCT coefficient sitting on a power
of two moves an exponent, which is 6.02 dB.

Two gates hold it. `tests/ac3/core/test_simd_kernels.cpp` compares each seam *primitive* — `f64x2`/
`i32x4` arithmetic, `round_ties_away`, `to_fixed25_block` — against a scalar reference in the same
binary and requires bit-for-bit equality, never a tolerance; on a `generic` build most of it is a
tautology, on every other build it is the whole argument. The kernels built from those primitives
are composition, not new arithmetic, so they inherit the guarantee rather than needing their own
bit-exact unit test — their correctness end to end is instead covered by
`tests/ac3/core/test_mdct_fast.cpp`'s existing tolerance check against the direct-form oracle and by
the corpus check below. Above it,
`tools/ci/run_codec_matrix.sh` run against two builds differing only in `ICLFORGE_SIMD` must
produce byte-identical output; that one covers restructuring the unit test cannot see:

```bash
cmake -S . -B build/simd-generic -DICLFORGE_SIMD=generic
```

```bash
./tools/ci/run_codec_matrix.sh build/config-linux-gcc/bin/forge /tmp/mx-simd && ./tools/ci/run_codec_matrix.sh build/simd-generic/bin/forge /tmp/mx-generic && diff <(cd /tmp/mx-simd && find . -type f | sort | xargs sha256sum) <(cd /tmp/mx-generic && find . -type f | sort | xargs sha256sum)
```

## Runtime AVX2 dispatch

`ICLFORGE_SIMD` above answers "which architecture" and is a compile-time question — SSE2, NEON and
scalar are all guaranteed present on the architecture they target, so there is nothing to ask a
running CPU. AVX2 is different: it is a real feature a deployment machine might not have, so the
build machine cannot bake in a yes/no answer safely. `ICLFORGE_AVX2` (`ON` by default, x86_64 only)
compiles a second, AVX2-flagged tier alongside the baseline one; `iclforge::internal::cpu::has_avx2()`
(`src/base/include/iclforge/base/detail/cpu_features.hpp`) decides at runtime, once per process, whether it is
safe to use it on the machine actually running the binary.

**Detection.** GCC/Clang/AppleClang use `__builtin_cpu_init()` + `__builtin_cpu_supports("avx2")`,
which already performs both the CPUID check and the OS-support (XSAVE/XGETBV) check correctly.
MSVC and clang-cl (sharing a path, keyed on `_MSC_VER` rather than compiler identity, since
`__builtin_cpu_supports` needs compiler-rt support this project's clang-cl configuration does not
guarantee) do it by hand with `<intrin.h>`: `__cpuid`/`__cpuidex` confirm CPUID leaf 7 exists and
OSXSAVE+AVX are set (leaf 1, ECX bits 27 and 28), only then `_xgetbv(0)` confirms XCR0 enables both
XMM and YMM state, only then a second `__cpuidex(_, 7, 0)` reads the actual AVX2 bit (EBX bit 5).
That order is load-bearing — calling `_xgetbv` before confirming OSXSAVE can fault on hardware
without XSAVE at all. The result is cached in a function-local `static const bool` (a C++11 magic
static — thread-safe on every toolchain in the matrix), so the detection sequence runs once per
process regardless of how many call sites ask.

**`ICLFORGE_SIMD_TIER`** (environment variable, read once inside that same cached initialisation)
overrides the answer: `sse2` always forces `has_avx2()` false, `avx2` forces it true — except when
the hardware cannot run AVX2, where forcing up `std::abort()`s with a clear message
rather than risk an illegal-instruction fault. `auto` (the default, same as unset) is the real
detected answer. This is what makes cross-tier correctness checking possible without needing AVX2
hardware physically present for the "does this at least build and dispatch correctly" half of the
question, and what proves the "does it actually execute correctly" half wherever it does run.

**Compilation.** The AVX2 tier is one CMake `OBJECT` library, `forge_simd_avx2` — the only target in
the whole build that ever sees `/arch:AVX2` (MSVC/clang-cl) or `-mavx2` (GCC/Clang/AppleClang).
Never `-mfma`: this project's code must not call an FMA intrinsic regardless of what the flag would
otherwise permit — see [Floating-point contraction](#floating-point-contraction) — and not
requesting it keeps the CPUID gate to the single AVX2 bit. `INTERPROCEDURAL_OPTIMIZATION` is forced
`OFF` on this target specifically: LTO/LTCG is the one mechanism that could hoist AVX2-flagged
codegen across a translation-unit boundary into a caller `has_avx2()` never approved for it. The
target exists only where `ICLFORGE_SIMD` resolved to `x86_64`, so it follows the effective target
architecture through a [cross-build](#simd-kernels-and-the-architecture-tree) rather than the host:
an arm64 or universal macOS build does not build it at all, rather than building it without the
flag.

**Testing — compile everywhere, execute only where capable.** `forge_simd_avx2` links into
`iclforge-tests` on every x86_64 leg unconditionally, proving the AVX2 code is valid, compilable,
linkable C++ on MSVC, clang-cl, GCC, Clang and AppleClang alike, with zero hardware dependency.
`tests/ac3/core/test_simd_kernels.cpp`'s `[avx2]`-tagged cases go further and actually execute it —
guarded by `has_avx2()`, with a loud, explicit `SKIP()` (never a silent pass) on hardware that
lacks it. The x86_64 CI legs resolve to a self-hosted or a GitHub-hosted runner per run, and
the self-hosted CPU features are not documented anywhere in this repo, so no leg may assume the
host it landed on qualifies. `ICLFORGE_REQUIRE_AVX2=1` turns that skip into a hard failure instead.
`.github/actions/build-leg/action.yml` sets it for the `linux-llvm-asan-ubsan` leg, so that leg
fails, rather than skips, on a host without AVX2, and "the AVX2 path actually ran and passed" is
something the leg enforces. Nothing pins that leg to a hosted machine: it takes its runner from a
fleet slot (`linux_runner_3` in `.github/ci/legs.jsonc`) like the other legs.

`tools/ci/run_codec_matrix.sh`'s own `ICLFORGE_CROSS_TIER_CHECK=1` mode is the corpus-level
analogue of the `ICLFORGE_SIMD=generic` cross-*build* check above, but cross-*tier* from the SAME
binary: it runs the whole matrix twice, once under `ICLFORGE_SIMD_TIER=sse2` and once under `=avx2`,
and byte-diffs the two output trees. `.github/workflows/_ci-linux.yml` wires it into the same
`linux-llvm-asan-ubsan` leg. On a host that cannot execute AVX2 (`/proc/cpuinfo` has no `avx2` flag) the second pass is
skipped with an explicit message and the script still exits 0 — degrading to exactly today's
guarantee, never silently claiming a check that did not run:

```bash
ICLFORGE_CROSS_TIER_CHECK=1 ./tools/ci/run_codec_matrix.sh build/config-linux-llvm/bin/forge
```

The dynamic-dispatch follow-on proved this whole mechanism end to end against one trivial
function first (`iclforge::ac3::internal::avx2::avx2_probe_matches_expected()`, no codec bit-exactness stakes
of its own) — deliberately, so the build/link/dispatch/test pipeline was proven before any kernel's
correctness depended on it — then wired two real kernels behind it: `apply_analysis_window` and the
DCT-IV/IMDCT twiddle stages (`mdct.cpp`'s `dct4_pre_twiddle`/`dct4_post_twiddle`,
`imdct512_pre_twiddle`/`imdct512_negate_copy`/`imdct512_post_twiddle`, `imdct256_post_twiddle` —
see `src/ac3/src/internal/avx2/mdct_avx2.hpp`).

**Which kernel gets wired is decided by measurement, not by which ones happen to be easiest.** A
symbol-scoped `perf record` comparison (generic-vs-x86_64-tier `iclforge-kernelbench`, PID-attributed
cycles rather than whole-process wall-clock — the latter turned out to be confounded by link-time
code layout even between builds differing in nothing SIMD-related, an early false lead this project
ruled out empirically) found a real, reproducible ~15-20% cycle reduction for `apply_analysis_window`
and the twiddle stages, comfortably clear of the ~2-4% cross-build measurement noise floor a known-
identical control function (`reference_mdct512_forward`, never touched by any SIMD tier) established.
The same measurement found no signal above that noise floor for `dft512`'s normalisation,
exponent-to-PSD or `to_fixed25_block` — each vectorises a small slice of a much larger, non-
accelerated function (the FFT core, the §7.2.2.4/.5 excitation/masking recurrence, and the exponent
pipeline respectively), so whatever benefit the vectorised slice contributes is swamped by the rest
of its caller at the per-call granularity this method can resolve. Those three stay SSE2/NEON-only;
extending them to AVX2 would need proof of aggregate (not per-call) benefit first.

### Batching across transforms, and where the transpose tax lands

The kernels above widen operations *within* one transform. The follow-on phases took the other
axis — running four INDEPENDENT same-size transforms in lockstep, one per SIMD lane — because
the FFT core (`fft_kernel.hpp`) has no clean within-one-transform grouping to widen at all. It is
templated on its arithmetic type (`typename VecType = double`), so the identical body serves the
existing scalar instantiation and a new `f64x4` one; `imdct512_windowed_batch4` and
`mdct512_forward_batch4` (`mdct.hpp`) are the batched entry points, used by JOC's object loop,
JOC's bed analysis, and both encoders' per-channel loops. Each checks `has_avx2()` internally and
falls back to four ordinary calls, so a caller only ever decides "are four ready to batch".

**The batched inverse took three designs, and the reason is worth stating** because the first two
look reasonable and both lost. A batched kernel needs its four objects interleaved; the caller
holds them contiguous per object; so *something* must transpose, and the only question is what
currency it is paid in. Paying inside the kernel with `f64x4::set` gathers and `lane0()..lane3()`
extraction — several dependent instructions per four doubles, run 128 and 512 times a call — came
out ~1% slower than the scalar path it replaced. Moving the interleaving out into the caller's own
storage made the kernel faster but the caller ~8% slower, because its accumulation and overlap-add
passes then strode one cache line per double (L1d misses roughly doubled across a decode); net
~7% worse than the first attempt. Paying it in 4x4 block transposes at the kernel boundary
(`transpose4x4`, `simd_avx2.hpp` — eight independent shuffles per sixteen doubles, both sides
keeping their natural layout) is what finally won. At this transform size the arithmetic saved is
small enough that the transpose's *form* decides the outcome, not its presence.

**FMA3 was measured and declined.** Compiling the AVX2 kernels with `-mfma -ffp-contract=fast`
fuses 46 instructions across both batch kernels, the twiddle stages and the FFT instantiations —
an upper bound, since hand-written `_mm256_fmadd_pd` cannot beat what the optimiser already finds.
It buys about **1%**. It also changes results: one of thirteen decoded object WAVs differed.
Encoded bitstreams happened to match on the material tried, which is luck (the coefficient deltas
quantised to the same mantissas), not a property. One percent does not justify retiring the
cross-tier byte-identity gate or having a single binary emit different bitstreams depending on the
CPU it lands on, so [Floating-point contraction](#floating-point-contraction) stays pinned.

One trap for anyone re-running that experiment: **the `[avx2]` bit-exactness cases do not detect
contraction.** They compare a batched kernel against `imdct512_windowed`, which also routes
through AVX2 twiddle kernels — so with FMA enabled both sides fuse and still agree. They pass on
an FMA build. Compare decoded PCM, not bitstream hashes.

### What the transform work was actually worth

Batching moved `joc_reconstruct_mdct_4obj` about 18% against the pre-batching scalar baseline, and
a real 12-object `joc-domain=mdct` decode a few percent. Encode barely moved (−0.9% to −2.2%) for a
reason worth recording: **the entire transform stack is only ~2.3% of an E-AC-3 encode profile**, so
even a large multiple on it cannot show. Two later, non-SIMD changes each dwarfed all of it — see
[Performance trend](performance-trend.md)'s note on profiling by source line. The transforms are
now fully 256-bit (582 ymm register operands against 55 xmm in the AVX2 kernel object); the
remaining cost in this codec is not in them.

## Floating-point contraction

The project pins `-ffp-contract=off` (`/fp:precise` on MSVC, `/clang:-ffp-contract=off` on
clang-cl) for every target, in the top-level `CMakeLists.txt`.

GCC and Clang both let the optimiser fuse `a * b + c` into a single fused-multiply-add by default,
which keeps one extra rounding step's worth of precision. That is a good default for numerical
code and the wrong one here, because it is **architecture-dependent**: FMA is a base ARMv8-A
instruction, so every arm64 and Apple-silicon leg contracts, while x86-64 has no FMA below AVX2
and this project passes no `-march=`, so no x86 leg does. The same source computes different
numbers on different legs purely because of what the instruction set offers.

The [gold-reference gate](#gold-reference-correctness-gate) has been recording a 6.02 dB
gap since the arm64 legs were added — `linux-gcc-arm64`, `linux-llvm-arm64` and `macos-llvm` score
about 61.8 dB where every x86 leg scores about 67.8 dB. That number is not a vague
"floating-point differences" figure: it is precisely one AC-3 exponent step (§7.2.2.2's PSD units
are 128 per exponent, and one exponent is 6.02 dB). FMA contraction was the standing hypothesis —
it is architecture-dependent in exactly the way the gap is (present on every leg that has FMA as a
base instruction, absent on every leg that does not) — and pinning `-ffp-contract=off` project-wide
was this item's test of it.

**The test came back negative.** With the flag applied on every leg, the three low-scoring legs
still measure 61.83–61.87 dB against 67.73–67.90 dB on x86 — the same numbers, to within normal
run-to-run noise, as before the flag existed. Contraction is therefore ruled out, not confirmed,
as the explanation for this gap; the correlation that matters is architecture (aarch64 in all
three cases — `macos-llvm`'s GitHub-hosted runner is Apple Silicon), not compiler family or libm
package.

**Architecture-specific libm `sin`/`cos` — tested directly, also ruled out.** The
standing hypothesis was aarch64's own compiled `libm` (glibc ships an architecture-specific
`sincos`/`cos`/`sin`, so "the same libm" as a source package does not mean bit-identical machine
code) producing different last-bit results in the transform twiddle tables. Two things needed
correcting before that hypothesis could even be tested precisely: `kAnalysisWindow` is not built
from `std::cos`/`std::sin` at all — it is a `consteval` construction (`iclforge/ac3/core/window.hpp`) using
a hand-written `bessel_i0`/`constexpr_sqrt`, evaluated entirely by the compiler's own constant
interpreter at compile time, so it cannot carry a *runtime* libm difference between architectures
by construction; the real runtime `std::cos`/`std::sin` call sites are `mdct.cpp`'s `Twiddles`,
`Twiddles2` and `FastMdctTables` and `fft_kernel.hpp`'s `FftTables`, all `static const` objects
built once on first use.

Measured (WSL2, real x86-64 hardware; aarch64 via `gcc-16-aarch64-linux-gnu` cross-compiling GCC
16 — the same major version `linux-gcc-arm64` uses — and `qemu-user`, which implements IEEE-754
arithmetic in software rather than approximating it): every one of the 2,170 `std::cos`/`std::sin`
calls those table constructors make at this codec's actual transform sizes (P = 64/128/512, the
QMF fold at 128) is **bit-identical**, x86-64 GCC to x86-64 Clang to aarch64 GCC under emulation —
zero differing values, not "close." Going one step further, the real `gold-reference gate` itself
was run end to end against a real `ICLFORGE_SIMD=aarch64` cross-build (same `-ffp-contract=off`
flag, same GCC 16, the actual `aarch64-neon` kernel real CI selects) under that same emulation, and
every one of its 32 checks' SNR numbers — not just the twiddle tables — came back bit-identical to
the native x86-64 build's, including the three streams this project's own encoder produces
(channel 4: 67.80/67.82/67.76 dB, matching x86-64 to the reported precision, not the ~61.8 dB every
real arm64/macOS CI leg measures). A `generic` (scalar reference kernel) build on the same x86-64
hardware matched both, for the same reason: IEEE-754 correctly-rounded arithmetic has no room for
two conforming implementations to disagree on `+`, `-`, `*`, or a `round()` pinned bit-exact by its
own test (`tests/ac3/core/test_simd_kernels.cpp`).

So the 6.02 dB gap does **not** reproduce under any standards-conformant aarch64 execution this
project can construct without the real hardware CI already has. That rules out "the aarch64
instruction set" as an explanation in the abstract, and narrows what is left to two candidates
neither locally reproducible: the specific *natively*-packaged aarch64 compiler GitHub's hosted
arm64/macOS runners use (as opposed to a Debian **cross**-compiler package, the only kind available
without that hardware — a native package can carry different default codegen/tuning even with
identical flags and the identical GCC version), or a real-silicon floating-point behaviour
`qemu-user`'s software emulation does not reproduce. Both need the real runners to test further.

**FFmpeg's own architecture-specific kernels — tested directly, ruled out.** Every hypothesis
above is about this project's side of the comparison, but the gold-reference gate measures
*agreement between two decoders*, and the other one is FFmpeg — which ships hand-written SIMD for
its AC-3 decoder and therefore runs different code on x86-64 (SSE/AVX) than on aarch64 (NEON). If
FFmpeg's NEON decode differed from its SSE decode by a last bit, that alone would move the measured
agreement and nothing in this project would be at fault.

It does not. Decoding `tests/golden/external-baseline/ac3-51-448/dee.ac3` twice on the same x86-64
host, once normally and once with `ffmpeg -cpuflags 0` forcing its plain-C reference path, gives
two WAVs that are *not* byte-identical — FFmpeg's kernel choice does change its output — but the
difference sits at **100–117 dB** per channel. That is 30 dB or more below the ~88 dB level at
which this project's decode and FFmpeg's actually agree, so it is buried: `forge`'s SNR against
FFmpeg is identical to two decimal places (57.50 / 63.84 / 58.19 / 88.23 / 22.76 / 22.67 dB)
whether FFmpeg decoded with SIMD or without. A difference that cannot move the number by 0.01 dB
on one architecture cannot produce 6.02 dB across two. The reference side is exonerated; whatever
is left is on this project's side of the comparison.

**Where the gap appears is level-dependent, which constrains what it can be.** Sorting all 52
(check, channel) pairs in the recorded history by their x86-64 SNR gives a step rather than a
gradient: every pair below 67 dB shows an arm64 difference of 0.00–0.11 dB, every pair above it
shows 5.85–6.05 dB, and nothing lands in between. So the gap is not a systematic codec error,
which would be level-independent and shift every channel equally. It appears only where the two
decoders agree closely enough that arithmetic is the only thing left to disagree about — which is
also why the LFE is the only channel to split on the fixed third-party fixtures, at 88 dB the one
channel there whose comparison is rounding-limited. See
[Validation](verification.md#why-arm64-and-x86-64-disagree).

The flag stays pinned regardless of either result, for an unrelated and unconditional reason: it is
what makes the SIMD seam's bit-exactness argument hold. The seam maps every operation to one
IEEE-754 add, subtract or multiply so a vectorised kernel is bit-identical to the scalar loop it
replaced, and that only holds if the scalar loop is not silently getting a fused form the
intrinsics cannot express — Clang will happily re-fuse a NEON `vmulq_f64` and `vsubq_f64` pair
back into `vfmsq_f64` when contraction is on. That argument does not depend on whether contraction
also explains the gold-gate gap, which it turns out not to.

Measured cost: none on x86-64, where the flag is a no-op because baseline x86-64 has no FMA
instruction to emit — proven, not assumed, by the corpus comparison above coming out
byte-identical against a build without the flag. On aarch64 it gives up FMLA in the transform
inner loops for a bit-exactness guarantee, not for a change in the gold-gate numbers.

That mystery therefore stays open — both hypotheses proposed for it are now closed out
by direct measurement rather than by argument — but it is no longer *unwatched*: a cross-platform
bitstream-hash gate (`tools/checks/check_cross_platform_hash.py`, wired into
[the gold-reference gate](#gold-reference-correctness-gate)) pins a SHA-256 of the actual encoded
bytes per `(kernel, transform mode)` pair in `tests/golden/bitstream-hashes.json`, so this specific
divergence — resolved or not — cannot silently change size without a CI failure pointing straight
at it. `x86_64-sse2` and `generic` were pinned from the measurements above, and `aarch64-neon` is now
pinned too — from the real arm64 CI legs rather than the qemu cross-build this file declined to
pre-fill from (PR #503, CI run 33635430769: `linux-gcc-arm64`, `linux-llvm-arm64`,
`windows-msvc-arm64`, `macos-llvm`).

**All three streams came back byte-identical to `x86_64-sse2`.** That is a result, not a
formality: this project's *encoder* is bit-exact across architectures, so the ~6.02 dB
gold-reference gap the arm64 legs measure is entirely **decode-side** — the same bytes go in and
different PCM comes out.

It also resolves what looked like it needed two mechanisms. Only the LFE splits on the fixed
third-party fixtures, but *every* channel splits on this project's own gold-reference streams,
which invites the inference that the arm64 encoder must be producing different bytes. It is not.
The gold-reference streams are encoded `dither=off` (`nodither` for E-AC-3), so nothing is
dither-limited and every channel sits in the rounding-limited regime above 67 dB where the
last-bit difference is all that is left; the third-party fixtures carry dither, which dominates
every channel except the LFE. One mechanism, two fixture populations.

With the encoder excluded by these hashes, FFmpeg's own kernels excluded by the `-cpuflags 0`
test above, and contraction and libm excluded before that, what remains open is the decode
path on real arm64 silicon — which is also the one thing no emulated run has reproduced.

## Gold-reference correctness gate

`tools/checks/verify_gold_reference.sh` (invoked in CI on every leg except linux-llvm-asan-ubsan,
which stays diagnostic-only) is the first real implementation of the project's original
validation-pyramid design (now [docs/verification.md](verification.md)), which had never been
wired into CI before this: encode a fixed, checked-in 5.1 WAV
(`tests/golden/audio/reference_51.wav`, synthesized once by `tools/generators/gen_gold_reference_wav.py` —
independent of this codec's own encoder/decoder, not bootstrapped from one of our own encodes),
strict-decode the result with FFmpeg (`-err_detect crccheck+bitstream+buffer+explode`, checked
via stderr content rather than exit code — confirmed locally that ffmpeg's own process exits 0
even on a CRC mismatch), decode it again with forge's own decoder, and assert the two decodes
agree via `tools/checks/compare_wav.py`'s delay-compensated SNR (stdlib-only Python, no numpy — every
CI-hosted runner already ships Python 3, so this needs no new provisioning). The gate's own three
checks are perceptual/SNR-based rather than a bit-exact bitstream comparison deliberately: nothing
in this project verifies that Homebrew LLVM, GCC and MSVC round the codec's floating-point
pipeline identically, and the real numbers above show they in fact do not, by a small but
measurable margin. `tools/checks/check_cross_platform_hash.py` runs immediately after and does add a bit-exact comparison, but as a pinned-regression gate over each leg's own
encoded bytes rather than a cross-leg equality assertion — see
[Floating-point contraction](#floating-point-contraction) above for why the latter cannot pass
today.

This is a narrow, cross-platform *quality* check — one sample, two codecs, every OS — not a
conformance sweep. `tools/checks/check_matrix_coverage.py`, `tools/ci/quality_race.py`'s `ci` mode and the
rest of the `ffmpeg-validate` CI leg (Linux-only, see [Verified configuration](#verified-configuration)
above) cover the *correctness* question instead: does every layout, every Annex E tool token and
every metadata option actually produce a structurally valid, spec-conformant stream, across the
full option space this gate does not attempt.
