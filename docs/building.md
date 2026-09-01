# Building ac3forge

Every command here has been run on the configuration described under
[Verified configuration](#verified-configuration). Anything not verified is marked as such.

## Requirements

| | Version | Notes |
|---|---|---|
| A compiler | MSVC (VS 2026), clang-cl 22, GCC 16, or Clang 22 | C++23. `std::expected` and deducing-`this` are both used. Formatted output goes through {fmt} (`fmt::format`/`fmt::print`), not `std::format`/`std::print` — see [Options](#options) and `docs/platforms/android.md`. One [preset](#presets) per compiler; all seven platform/compiler legs are required, green CI (GCC 16 covers two of them — `linux-gcc` and `linux-gcc-arm64`; Clang 22 covers three — `linux-llvm`, `linux-llvm-arm64` and `macos-llvm`, each as a separate leg, though `macos-llvm` deliberately tracks Homebrew's unpinned `llvm` formula, currently also 22, rather than an exact pin) — see [Verified configuration](#verified-configuration). |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28...4.3)`. |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| vcpkg | any recent | Supplies fmt (a base dependency, needed by every build — see `cmake/Fmt.cmake`) and Catch2 (needed only when tests are on); with `-DVCPKG_MANIFEST_FEATURES=adm`, the Boost header libraries `AC3FORGE_BUILD_ADM=ON` needs; and with `-DVCPKG_MANIFEST_FEATURES=profiling`, the Tracy profiler `AC3FORGE_ENABLE_TRACY=ON` needs — see [Options](#options). vcpkg itself is never strictly required, though: fmt and Catch2 both fall back to a `FetchContent` build from source when no local copy is found (`AC3FORGE_FETCH_FMT`/`AC3FORGE_FETCH_CATCH2`, both default `ON`), and Boost/Tracy are opt-in features nobody gets by default. |
| Qt | 6.5+ prebuilt | GUI only. **Never from vcpkg** — see [Qt](#qt). |
| ALSA (`libasound2-dev`) | any recent | Linux only, optional. Live capture/monitor/passthrough — see [Linux audio](#linux-audio). |
| PipeWire (`libpipewire-0.3-dev`) | any recent | Linux only, optional, used only when ALSA is not — see [Linux audio](#linux-audio). |
| Python 3 + numpy | 3.11+ | Only for `tools/`; not part of the build. |
| FFmpeg CLI | 8.x | Only for validation scripts; not part of the build. |

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
`vswhere`, runs its `vcvarsall.bat x64` in a subprocess, and imports the result into the CMake
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
  `AC3FORGE_BUILD_CLI`/`AC3FORGE_BUILD_TESTS` pinned `ON`, and the vcpkg overlay triplets under
  `cmake/vcpkg/triplets/`. (`CMAKE_EXPORT_COMPILE_COMMANDS` comes from the top-level
  `CMakeLists.txt` itself, not the presets.)
- `debug` / `release` — just `CMAKE_BUILD_TYPE`.
- `windows-msvc`, `windows-llvm`, `linux-gcc`, `linux-llvm`, `linux-gcc-arm64`,
  `linux-llvm-arm64`, `macos-llvm` — one per platform/compiler pair. Each sets
  `VCPKG_TARGET_TRIPLET`, chainloads that platform's toolchain
  file (see [above](#the-compiler-is-pinned-not-path-found)) via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`,
  and is gated by a `condition` on `hostSystemName` so only the presets for the machine you're on
  even appear. `AC3FORGE_BUILD_GUI` is `ON` for the two Windows ones and `OFF` for the rest — see
  [Verified configuration](#verified-configuration).

Fourteen concrete `config-<platform>[-debug]` presets inherit `[ release|debug, <platform>, core ]`,
each with a matching `build-<platform>[-debug]` and `test-<platform>[-debug]` preset:

| Platform | Compiler | Configure preset | Build preset | Test preset |
|---|---|---|---|---|
| Windows | MSVC | `config-windows-msvc[-debug]` | `build-windows-msvc[-debug]` | `test-windows-msvc[-debug]` |
| Windows | clang-cl | `config-windows-llvm[-debug]` | `build-windows-llvm[-debug]` | `test-windows-llvm[-debug]` |
| Linux | GCC 16 | `config-linux-gcc[-debug]` | `build-linux-gcc[-debug]` | `test-linux-gcc[-debug]` |
| Linux | Clang 22 | `config-linux-llvm[-debug]` | `build-linux-llvm[-debug]` | `test-linux-llvm[-debug]` |
| Linux (arm64) | GCC 16 | `config-linux-gcc-arm64[-debug]` | `build-linux-gcc-arm64[-debug]` | `test-linux-gcc-arm64[-debug]` |
| Linux (arm64) | Clang 22 | `config-linux-llvm-arm64[-debug]` | `build-linux-llvm-arm64[-debug]` | `test-linux-llvm-arm64[-debug]` |
| macOS | Homebrew LLVM | `config-macos-llvm[-debug]` | `build-macos-llvm[-debug]` | `test-macos-llvm[-debug]` |

The two `-arm64` rows are the same `linux.gcc.toolchain.cmake`/`linux.llvm.toolchain.cmake` files as
their x64 counterparts — only the vcpkg triplet (`arm64-linux-gcc`/`arm64-linux-llvm`) differs; the
toolchain files already resolve aarch64 vs x86_64 from `VCPKG_TARGET_ARCHITECTURE`. See
[Raspberry Pi](platforms/raspberry-pi.md), the primary hardware this target is validated against.

There is a fifteenth configure/build/test trio, Debug-only and not part of the table above
because it isn't a platform/compiler pair but an instrumented variant of `linux-llvm`:
`config-linux-llvm-asan-ubsan` / `build-linux-llvm-asan-ubsan` / `test-linux-llvm-asan-ubsan`,
which inherits `linux-llvm` plus a `sanitize-asan-ubsan` fragment setting
`AC3FORGE_SANITIZERS=address,undefined` (see `cmake/Sanitizers.cmake`; MSVC is rejected outright,
so this only exists for GCC/Clang). See [Verified configuration](#verified-configuration) for what CI says
about all eighteen. There are also nine `ci-<platform>` `workflowPresets` (Release except for the
two sanitizer ones, which are Debug-only) that chain configure→build→test in one
`cmake --workflow --preset ci-windows-msvc` call. There is no coverage workflow preset — the
`config-`/`build-`/`test-linux-gcc-coverage` trio exists, but nothing chains it — and CI does not
call `cmake --workflow` at all: `_build.yml` runs `cmake --preset config-<leg>`,
`cmake --build --preset build-<leg>` and `ctest --preset test-<leg>` as three separate steps,
because a leg has per-leg overrides to append (`-DAC3FORGE_BUILD_GUI=ON` on the GUI legs,
`-DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"` on Windows) that a single `--workflow` invocation has
nowhere to put. The workflow presets are the one-command local equivalent of those three steps,
not the path CI takes.

There is an eighteenth trio, the ThreadSanitizer sibling of the pair above:
`config-linux-llvm-tsan` / `build-linux-llvm-tsan` / `test-linux-llvm-tsan`, inheriting
`linux-llvm` plus a `sanitize-tsan` fragment setting `AC3FORGE_SANITIZERS=thread`. It is a
separate preset rather than more entries in the ASan/UBSan list because the two runtimes are
mutually exclusive — Clang refuses `-fsanitize=address,thread` outright — and because they
answer different questions: ASan/UBSan ask whether one thread's memory and arithmetic are sound,
TSan asks whether two threads agree on who owns what. Nothing else in this repository can see a
data race, and `src/audio` is a lock-free SPSC ring, a silence watchdog and a clock-drift servo
shared between a real-time callback thread and the encoder thread.

Its test preset runs only the `concurrency` ctest label — `tests/audio/` plus
`tests/cli/test_cli_live.cpp`, 36 cases — because TSan's shadow memory makes everything several
times slower and the rest of the suite is single-threaded codec maths. The label comes from the
Catch2 tags themselves (`catch_discover_tests(... ADD_TAGS_AS_LABELS)` in `tests/CMakeLists.txt`),
so `ctest -L ring`, `-L encoder` and the rest work the same way. `tsan.supp` at the repository
root holds the suppressions, and is meant to stay near-empty; `ac3membench` is not built under
this preset, because its global `operator new`/`delete` replacements collide with TSan's own
runtime at link time.

```bash
cmake --preset config-linux-llvm-tsan
cmake --build --preset build-linux-llvm-tsan -- -k 0
ctest --preset test-linux-llvm-tsan
```

There is also a `minimal-decoder` fragment and the three configure/build presets that inherit
it — `config-arm-none-eabi-minimal`, `config-linux-gcc-minimal`, `config-linux-llvm-minimal`.
They are not part of the table above because they do not build the project: they build roadmap
PF7's decode-only library and its probe, and nothing else. The arm one does not inherit `core`
either — there is no vcpkg triplet for bare-metal arm and nothing that profile builds has a
third-party dependency, the same reasoning the Emscripten preset follows. See
[Minimum-footprint decoder profile](#minimum-footprint-decoder-profile).

There is a sixteenth trio, `config-linux-gcc-coverage` / `build-linux-gcc-coverage` /
`test-linux-gcc-coverage`, the same shape as the asan-ubsan one: an instrumented variant of
`linux-gcc`, Debug-only, not a platform/compiler pair. It inherits a `coverage` fragment setting
`AC3FORGE_ENABLE_COVERAGE=ON` (see `cmake/Coverage.cmake`, GCC/Clang's `--coverage` gcov
instrumentation; other compilers just warn and skip it), `AC3FORGE_BUILD_ADM=ON` with vcpkg's
`adm` feature (so the opt-in ADM pair — `ac3adm` and its bridge — is measured alongside the
always-on seven) and `AC3FORGE_BUILD_CLI=ON`, since `apps/cli` is gated too. Only
`AC3FORGE_BUILD_EXAMPLES` stays off, as a build-time saving: `examples/` is documentation that
happens to compile, over an API surface `tests/` already covers, and each one is its own `ctest`
process.

Note that `ac3cli` has to link `ac3::coverage` itself (`apps/cli/CMakeLists.txt`) and not merely
link an instrumented library. The gcov *runtime* propagates to consumers automatically, but
`--coverage` is target-scoped at compile time — so without that link every `.cpp` under
`apps/cli` compiles uninstrumented and emits no `.gcno` at all, which reads as *no data* rather
than as low coverage. The same applies to any other executable added to the report later.

After `ctest`, `tools/checks/coverage_report.sh` (the same script `.github/workflows/ci.yml`'s
`coverage` job runs) makes one `gcovr` extraction pass and then gates line *and* branch coverage
per component — the nine `src/` library components plus `apps/cli` — and prints a per-command
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
coverage leg, and no Linux CI leg installs one today. Its interactive surfaces are covered by
`apps/gui/tests`' own Qt Quick suite, and its one Qt-free class (`RecordingSink`) is already in
`ac3tests`. `python/` has its own floor instead, in `.github/workflows/wheels.yml`'s
`python-coverage` job — `pytest --cov` against the built wheel; see that job's own comment for
what a Python percentage does and does not measure when nearly all of the binding surface is C++.

There is a seventeenth trio, `config-linux-llvm-shared` / `build-linux-llvm-shared` /
`test-linux-llvm-shared`, same shape again: an instrumented variant of `linux-llvm`, Debug-only.
It inherits a `shared-libs` fragment setting `BUILD_SHARED_LIBS=ON`, proving `ac3::forge_shared`/
`matroska::matroska_shared` actually work — not just that the CMake topology configures, but that
every in-tree consumer (`ac3cli`, `ac3gui`, `ac3tests`, `examples/`) links and runs against the
real `.so`. `.github/workflows/_build.yml` runs it as an extra step inside the existing
`linux-llvm` leg rather than a new matrix entry, the same shape as the ASan/UBSan pass.

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
and the compiler selection on a platform preset (`windows-msvc` here; `windows-llvm`,
`linux-gcc`, `linux-llvm`, `linux-gcc-arm64`, `linux-llvm-arm64` and `macos-llvm` are the
others — see `CMakePresets.json`). Missing either from `dev`'s
`inherits` list still configures, but silently: CMake
falls back to its platform default generator (Visual Studio, on this machine) and an in-source
binary directory instead of `build/dev`, which is a mess to notice and worse to undo. Inherit
all four.

That keeps vcpkg's working directories off the system drive, which matters because they run to
several gigabytes. Substitute your own paths, and swap `windows-msvc` for whichever
platform/compiler fragment matches your machine.

## Options

| Option | Default | Effect |
|---|---|---|
| `AC3FORGE_BUILD_CLI` | `ON` | Build `ac3cli`. |
| `AC3FORGE_BUILD_GUI` | `ON` on the two Windows presets, `OFF` on Linux and macOS | Build `ac3gui`. Requires Qt. Off by default outside Windows because a Qt kit isn't assumed present there — see [Building on Linux](#building-on-linux). |
| `AC3FORGE_FETCH_FMT` | `ON` | When no local {fmt} is found (vcpkg, a distro package, an explicit `CMAKE_PREFIX_PATH`), fetch and build v12.2.0 from source via `FetchContent` instead of failing. Turn off to insist on a package-manager copy — see `cmake/Fmt.cmake`. Unlike the other `AC3FORGE_FETCH_*` options, this one is never irrelevant: {fmt} is a base dependency needed by every build. |
| `AC3FORGE_BUILD_TESTS` | `ON` | Build the Catch2 suite. Requires Catch2. |
| `AC3FORGE_FETCH_CATCH2` | `ON` | When no local Catch2 3 is found (vcpkg, a distro package, an explicit `CMAKE_PREFIX_PATH`), fetch and build v3.15.3 from source via `FetchContent` instead of failing. Turn off to insist on a package-manager copy — see `tests/CMakeLists.txt`. Irrelevant when `AC3FORGE_BUILD_TESTS` is off. |
| `AC3FORGE_BUILD_EXAMPLES` | `ON` | Build `examples/`, and register them as tests. |
| `AC3FORGE_BUILD_MATROSKA` | `ON` | Build `matroska::matroska` (`src/matroska`), the standalone Matroska container writer. `OFF` only makes sense with the CLI, GUI, tests and examples all `OFF` too — they link it unconditionally, and configure fails with a clear message otherwise (see the root `CMakeLists.txt` guard). |
| `AC3FORGE_BUILD_MP4` | `ON` | Build `mp4::mp4` (`src/mp4`), the standalone MP4/ISOBMFF container writer. Same all-off constraint as `AC3FORGE_BUILD_MATROSKA`. |
| `AC3FORGE_BUILD_MPEGTS` | `ON` | Build `mpegts::mpegts` (`src/mpegts`), the standalone MPEG-TS container writer. Same all-off constraint as `AC3FORGE_BUILD_MATROSKA`. |
| `AC3FORGE_BUILD_IAB` | `ON` | Build `ac3iab::ac3iab` (`src/ac3iab`), the standalone SMPTE ST 2098-2 Immersive Audio Bitstream reader. Same zero-third-party-dependency shape as the three container writers above, so it defaults on the same way; unlike them nothing in `apps/` or `examples/` links it yet, so there is no all-off guard — `tests/CMakeLists.txt` simply adds its test file when this is on. |
| `AC3FORGE_BUILD_CAPI` | `ON` | Build `ac3::forge_c` (`src/capi`), the C API over the encode/decode core — see [C API](library/c-api.md). Depends on nothing but `ac3::forge_static`, so unlike `AC3FORGE_BUILD_ADM` there is no extra dependency footprint to opt out of. |
| `AC3FORGE_BUILD_PYTHON` | `OFF` | Build the pybind11 extension module (`python/`). Off by default for the same reason as `AC3FORGE_BUILD_ADM`: nothing under `src/`, `apps/`, `tests/` or `examples/` links it, so a normal C++ build is unaffected either way. `python/pyproject.toml` turns it on itself via scikit-build-core when `pip install`/cibuildwheel drives the configure. |
| `AC3FORGE_BUILD_ADM` | `OFF` | Build `ac3adm::ac3adm` (`src/ac3adm`), the standalone BW64/RF64 + ADM parser — see [ADM / BW64 reading](library/adm.md). Off by default, unlike every other library component: it vendors libbw64/libadm via `FetchContent`, and libadm needs several Boost header libraries, resolved separately via `-DVCPKG_MANIFEST_FEATURES=adm` (`vcpkg.json`'s `adm` feature) — turning this `ON` without also selecting that feature fails with a clear configure-time message rather than a bare "Boost not found". |
| `AC3FORGE_WITH_ALSA` | `AUTO` | Linux only. `AUTO` builds the ALSA audio backend when libasound's headers are present; `ON` requires them; `OFF` never builds it. Takes precedence over `AC3FORGE_WITH_PIPEWIRE` when both are found — see [Linux audio](#linux-audio). |
| `AC3FORGE_WITH_PIPEWIRE` | `AUTO` | Linux only. `AUTO` builds the PipeWire audio backend when libpipewire-0.3's headers are present *and* ALSA was not selected; `ON` requires the headers (independently of ALSA); `OFF` never builds it. See [Linux audio](#linux-audio). |
| `AC3FORGE_SIMD` | `auto` | Which `src/forge/src/internal/arch/` directory supplies the codec's vector kernels: `auto` picks `x86_64` or `aarch64` from `CMAKE_SYSTEM_PROCESSOR` and falls back to `generic` everywhere else, and `generic`/`x86_64`/`aarch64` force one. See [SIMD kernels and the architecture tree](#simd-kernels-and-the-architecture-tree). The configure summary prints the resolved value, and so does `ac3cli --version`. |
| `AC3FORGE_AVX2` | `ON` | x86_64 only. Compiles an AVX2 SIMD tier alongside the baseline SSE2 one, selected at *runtime* rather than at configure time. See [Runtime AVX2 dispatch](#runtime-avx2-dispatch). `OFF` (or a non-x86_64 target) yields a provably AVX2-free binary. |
| `AC3FORGE_SANITIZERS` | empty | Comma-separated `-fsanitize=` value, e.g. `address,undefined` — see `cmake/Sanitizers.cmake`. Empty is a no-op; GCC/Clang only, MSVC is a configure error. Set via the `-asan-ubsan` preset above rather than by hand. |
| `AC3FORGE_ENABLE_COVERAGE` | `OFF` | `--coverage` gcov instrumentation over every target it's linked into — see `cmake/Coverage.cmake`. Off is a no-op; GCC/Clang only, other compilers get a configure-time warning and no instrumentation. Set via the `-coverage` preset above rather than by hand. |
| `AC3FORGE_ENABLE_TRACY` | `OFF` | Tracy profiler instrumentation (`ac3::tracy` — see `cmake/Tracy.cmake`). Needs vcpkg's `profiling` manifest feature (`-DVCPKG_MANIFEST_FEATURES=profiling`), which supplies Tracy itself; off is a no-op. |
| `AC3FORGE_BUILD_FUZZERS` | `OFF` | Build the libFuzzer harnesses under `fuzz/`. Clang only (GCC and MSVC ship no libFuzzer); use `fuzz/run.sh` rather than this option directly — it configures a dedicated `build/fuzz` with the right compiler. See [`fuzz/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/fuzz/README.md). |
| `AC3FORGE_MINIMAL_DECODER` | `OFF` | Build **only** `ac3::forge_minimal`: one decode-only static library with no exceptions, no RTTI and no direct-form transform tables, for a target with a few hundred kilobytes of RAM and no operating system. Not a "build X too" option — it replaces what `src/forge` builds, and configure fails with a list if any component that needs the full library is still on. GCC/Clang only. See [Minimum-footprint decoder profile](#minimum-footprint-decoder-profile). |

Building the library and CLI alone, with neither Qt nor vcpkg involved:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_BUILD_GUI=OFF -DAC3FORGE_BUILD_TESTS=OFF
```

The vcpkg toolchain file is still referenced by the preset, so `VCPKG_ROOT` must still point
at a checkout — it simply has nothing to install. To build with no vcpkg at all, configure
without the preset and pass the generator and build type by hand.

## Minimum-footprint decoder profile

Roadmap PF7. The next users of the decoder are set-top boxes, receivers and DSP ports, and what
they need is not a claim about being small but a build that is small, a target it demonstrably
runs on, and a number that stops moving quietly.

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
| Decode-only sources | The encoder, the container writers, WAV I/O, the analysis/QC layers and the object *encoder* are not compiled. `src/forge/minimal.cmake` lists what is, with a line on why each file is reachable from a decode. |
| No direct-form transform tables | `src/forge/src/core/transform/stub/` replaces `.../reference/`, removing **1,900,544 bytes of `.bss`** — the four (k, n) matrices §8.2.3.2's forward MDCT and §7.9.4.2 step 3's inverse sums need. |
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
false` returns `DecodeError::kUnsupported` in this profile rather than being silently served by
the fast path — that switch exists so a caller can validate against the arithmetic the spec
writes down, and substituting a different arithmetic would defeat its only purpose.

### The probe

`apps/baremetal/probe.cpp` links the archive, decodes six frames each of real 5.1 AC-3 (448
kbit/s, coupling) and E-AC-3 (384 kbit/s, AHT + spx + coupling), compares every channel's level
against `apps/baremetal/fixture.hpp`, and prints `key=value` lines that
`tools/checks/run_baremetal_probe.sh` gates on. It is not a unit test — the profile requires
`AC3FORGE_BUILD_TESTS=OFF`, since nothing under `tests/` builds against a decode-only archive —
and it answers three questions a test could not: does the archive link with everything else
absent, does it produce the right audio on a 32-bit soft-float target, and what did it cost.
Regenerate its fixture with
`python tools/generators/gen_baremetal_fixture.py --ac3cli <path>`.

The measured numbers are in [the footprint table](performance-trend.md#minimum-footprint-decoder).
CI runs this on every push (`build-footprint` in `.github/workflows/_build.yml`).

### Gaps

Three of PF7's requirements are not met, and are recorded here rather than half-enforced.

**No heap traffic in the decode loop — not met.** The profile does not allocate the output PCM
(`decode_frame_into`/`decode_access_unit_into` write through caller-owned spans, which is what
the probe uses) and it leaks nothing, but the steady state is **45 allocations per frame for
AC-3 and 87 for E-AC-3**, from the per-block geometry vectors inside the decoders and the
`std::vector` members of the returned `DecodedFrame`/`DecodedSubstream`. Reaching zero means
those becoming fixed-capacity storage, which changes the public types — a design change, not a
build option. The runner gates the number at 100 so the distance from zero cannot grow while the
gap is open.

**A float32-only path — not met.** The decoder's *output* is already `float`, but every
intermediate — the transform, the coefficients, the coupling coordinates — is `double`. A
float32 internal path would change every gold-reference number in
[the quality trend](quality-trend.md) and needs its own oracle run to establish that the change
is acceptable, so it is a project of its own rather than a flag. What the profile does instead is
prove the `double` path works without hardware floating point: the Cortex-M3 target has no FPU,
so every one of those operations is software-emulated, and the decoded levels still match the
host build.

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
references, even though (as on Windows) it supplies nothing but fmt and Catch2. This project's
own convention keeps that checkout under `/opt/vcpkg`, but any path works — there is nothing
Linux-specific about vcpkg here.

### GUI on Linux

Both Linux presets default `AC3FORGE_BUILD_GUI` to `OFF`. That is not because the GUI cannot be
built on Linux — `cmake/FindQt6.cmake` resolves a Linux Qt kit the same way it resolves a
Windows one (distro packages land on CMake's own prefixes; relocated or `aqtinstall` kits are
searched under `~/Qt`, `/opt/Qt` and friends), and `ac3gui` builds clean and passes its headless
`--smoke` run under both Linux presets. It defaults off because, unlike on Windows/macOS, a Qt
kit is not assumed to be present on every Linux machine that builds this project — see
`linux-gcc`'s own `description` in `CMakePresets.json`. Opt in explicitly once Qt is installed:

```bash
cmake --preset config-linux-gcc-debug -DAC3FORGE_BUILD_GUI=ON
```

Qt 6.5+ is required, same as everywhere else. On Debian/Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools
```

Other distros need the equivalent Qt6 base + declarative (QML/Quick) development packages;
package names vary (Fedora's are `qt6-qtbase-devel` / `qt6-qtdeclarative-devel`, for example).

**The `qmlshapesplugin` / `labsmodelsplugin` / `qmlfolderlistmodelplugin` CMake warnings.**
Configuring with the GUI on prints warnings that these — and, in fact, every other built-in
QML plugin `ac3gui` transitively touches through `QtQuick.Controls` (its styles, dialogs,
layouts, and so on) — "will not be linked", because the `Qt6::<name>plugin` CMake target each
would need does not exist. This is a property of how Ubuntu's apt-packaged Qt6 is built, not of
this project: the official Qt installer exports a static-link CMake target for every built-in
plugin so a fully self-contained executable can embed them, but a distro's dynamically-linked
Qt6 package does not need that and does not export it. It does **not** mean the plugins are
missing. Confirmed on Ubuntu 26.04's Qt 6.10: the `.so` files are installed at the normal QML
import path with valid `qmldir` files, and the QML engine loads them from there at runtime the
same way it loads every other Qt Quick module, independent of whether CMake could statically
link them in. `ac3gui`'s own QML never imports `Qt.labs.*` or `QtQuick.Shapes` directly — the
three named in the warning are pulled in transitively by `QtQuick.Dialogs`' non-native
fallback implementation, which backs `Main.qml`'s `FileDialog`s only when no native/portal
dialog is available, and which a headless `--smoke` run (verified with
`QT_LOGGING_RULES=qt.qml.import.debug=true`) never even requests. Safe to ignore.

### Linux audio

Three of ac3forge's features touch the sound hardware — live capture (`ac3cli devices`,
`record`), monitor playback (`monitor`), and IEC 61937 bitstream passthrough (`outputs`,
`play`). Everything else is file I/O and needs no audio stack at all; `ac3cli spdif` in
particular reaches an AV receiver by writing a WAV, on any machine.

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
`src/audio/src/backend/posix/`, whose entry points all return `kNoBackend`, and `ac3cli` marks
the affected commands `UNAVAILABLE HERE` in its usage rather than pretending they exist. Pass
`-DAC3FORGE_WITH_ALSA=ON` and/or `-DAC3FORGE_WITH_PIPEWIRE=ON` to turn a missing set of headers
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
`iec958Codecs` control has been populated by the session manager (a WirePlumber ALSA-monitor
rule, or a one-off `pw-cli` call) — configuration this library has no portable way to perform on
a caller's behalf. On a stock desktop where nobody has touched that setting, every PipeWire sink
honestly has no compressed codec enabled, even though the exact same hardware is reachable
directly through ALSA underneath the very PipeWire daemon that's running.

That is why ALSA keeps first precedence in `src/audio/CMakeLists.txt` whenever both are found,
rather than PipeWire winning by default for being the modern norm on most current desktops:
preferring it unconditionally would silently regress `ac3cli outputs`/`play` on exactly the
common case where nobody has configured `iec958Codecs`. The explicit escape hatch for a machine
where PipeWire's compressed codecs genuinely are configured is
`-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON`, the same shape `-DAC3FORGE_WITH_ALSA=OFF`
alone already has today.

#### What has and has not been verified

**ALSA.** Verified on WSL2 Ubuntu 26.04 with the local development loop's gcc 15.2 and clang 22.1
(CI's own Linux legs pin GCC 16 — see [Requirements](#requirements)), in every configuration:
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
per-device anyway — `ac3cli outputs` probes each one and says.

**PipeWire.** Verified on the same WSL2 Ubuntu 26.04 host with libpipewire-0.3 1.6.2, gcc 15.2
and clang 22.1, with `-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON` forcing the
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
Linux and macOS preset still forces `AC3FORGE_BUILD_GUI=OFF` by default — pass
`-DAC3FORGE_BUILD_GUI=ON` explicitly on a machine that has Qt 6.5+, which is verified to work on
Linux both locally (see [GUI on Linux](#gui-on-linux) above) and in CI, which installs a Qt6 kit
and turns the flag on for the four Linux build legs (x64 and arm64, GCC and Clang) plus
`macos-llvm` (Homebrew's `qt` formula — see [macOS](platforms/macos.md#gui-on-macos)). See
[Verified configuration](#verified-configuration). If your kit is somewhere else, say so explicitly and it
wins over the search — the project's own `-DAC3FORGE_QT_ROOT=` (or the `AC3FORGE_QT_ROOT`,
`QT_ROOT_DIR` or `QTDIR` environment variables) is the preferred way:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

Plain `-DCMAKE_PREFIX_PATH=...` or `-DQt6_DIR=...` also work, and take priority over everything
`FindQt6.cmake` does. If you do not want the GUI, `-DAC3FORGE_BUILD_GUI=OFF` removes the
dependency entirely.

## Packaging

`cmake/Packaging.cmake` wires CPack up behind the platform preset matrix. A plain ZIP is
always produced; NSIS (Windows), DEB/RPM (Linux) and DragNDrop (macOS) are added on top when
the packaging tool for that format is found on `PATH`, so `cpack` degrades gracefully instead
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
(`pack-windows-llvm`, `pack-linux-gcc`, `pack-linux-llvm`, `pack-linux-gcc-arm64`,
`pack-linux-llvm-arm64`, `pack-macos-llvm`). A pack preset reuses whatever the matching build
tree was configured with — on a non-Windows preset that includes the GUI only if you opted in
(`-DAC3FORGE_BUILD_GUI=ON`, which is exactly what CI's Linux and macOS packaging legs pass — see
[GUI on Linux](#gui-on-linux) and [macOS](platforms/macos.md#gui-on-macos)). Beyond `windows-msvc`'s continuous per-push packaging, the
`release_package` legs have run for real on tagged releases, and `pack-linux-gcc-arm64` has
additionally been run by hand on a real Raspberry Pi 4B with the resulting `.deb` inspected —
see [Raspberry Pi](platforms/raspberry-pi.md#verified-configuration). Packages land in
`packages/` at the repository root.
`cmake --build --preset build-windows-msvc --target pack-ac3forge` runs the same thing from
inside an IDE's target list instead of the command line.

`ac3cli`/`ac3gui` and `ac3::forge`/`matroska::matroska` are both installed and packaged, as
independent CPack components (`runtime`, plus `library`/`libruntime` for the codec itself) - a
second `ac3forge-dev-*` archive alongside the usual end-user one, for a third party consuming the
codec via `find_package(ac3forge)` rather than running it as a program; on Linux, `library`/
`libruntime` also become real `libFOO`/`libFOO-dev`-style DEB/RPM packages rather than only an
archive. See
[Using ac3::forge](library/index.md) for the CMake side and
[docs/releasing.md](releasing.md#what-gets-published) for exactly what ships where.
`ac3::audio` (live capture/monitor/passthrough, `src/audio/`) stays link-only and unpackaged -
a CLI/GUI implementation detail, not part of either component.

CI packages the `windows-msvc` leg on every push and uploads the result as a workflow
artifact (`.github/workflows/_build.yml`), so the packaging path is exercised continuously
rather than only when someone remembers to run it locally.

A tag-triggered release workflow (`.github/workflows/release.yml`) builds, signs, attests and
publishes packages for the three `release_package` legs — `windows-msvc`, `linux-gcc` and
`linux-gcc-arm64` — plus, since DR8, a `package-macos-universal` job that `lipo`-merges
`macos-llvm`'s (arm64) and `macos-llvm-x64`'s (x86_64) install trees into one universal `.dmg`
rather than either leg packaging solo: one canonical build per OS/architecture, whenever a
`vX.Y.Z` tag is pushed; a packaging failure on any of them blocks the release like any other
required leg. See [docs/platforms/macos.md](platforms/macos.md#universal-binaries-dr8) for how
the macOS merge works. The release carries GPG signing (optional, off until a key is provisioned),
keyless Sigstore/OIDC build provenance, an SPDX SBOM, and a GitHub Release; nine beta releases
(v0.2.0-beta.1 through v0.9.0-beta.1) have shipped through this path for real. See
[docs/releasing.md](releasing.md) for the full process, including how to provision the GPG key.

There is also a staged, unpublished vcpkg port at `packaging/vcpkg-port/ac3forge/`
(`portfile.cmake`, `usage`, its own `vcpkg.json`), pending submission to the curated
`microsoft/vcpkg` registry. It exposes three features — `matroska`, `mp4` and `mpegts`, one per
container writer — and declares no `default-features`, so none of them is on by default: a plain
`vcpkg install ac3forge` gets the codec alone, and `vcpkg install ac3forge[matroska,mp4,mpegts]`
(or any subset) opts in. [docs/releasing.md](releasing.md#vcpkg-port) records why — a
curated-registry port's default features may only enable behaviors, not additional public
APIs/targets. A consumer uses the installed package via `find_package(ac3forge)` exactly as
[Using ac3::forge](library/index.md) documents. The per-release submission flow is in
[docs/releasing.md](releasing.md#vcpkg-port).

## The standards documents

`docs/spec/` is gitignored: the standards are free to download but are not redistributed here.
The build does not need them — every table is already transcribed into the source. They are
needed only to re-run the generators in `tools/`.

To set that up, fetch:

| Document | Why |
|---|---|
| ATSC A/52:2018 | The master standard. E-AC-3 is normative Annex E. |
| ETSI TS 102 366 | Carries the EMDF metadata format in Annex H. |
| ETSI TS 103 420 | Joint Object Coding. |
| `ts_103420v010201p0.zip` | The TS 103 420 companion archive. The JOC Huffman tables are in `ts_103420_tables.c` inside it, and nowhere in the PDF. |

Extract each PDF to page-marked text beside the PDF, with page separators of the form
`===== PDF PAGE n =====`. The generators locate tables by page.

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
| Compilers | GCC 15.2.0 and Clang 22.1.x, both tried — this is the local development loop, not the CI pin. CI installs GCC 16 (`.github/toolchain/02-gcc-toolchain.sh`), which is what [Requirements](#requirements) and [Linux](platforms/linux.md#toolchains) state; the toolchain files' `find_program` fallback list is why an older GCC still configures and passes. |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.10.2, apt-packaged (`qt6-base-dev`, `qt6-declarative-dev`) |
| ALSA | `libasound2-dev`, both present and as the no-ALSA fallback — see [Linux audio](#linux-audio) |
| PipeWire | `libpipewire-0.3-dev` 1.6.2, present and forced selected (`-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON`) — see [Linux audio](#linux-audio) |
| vcpkg | checkout at `/opt/vcpkg` |

Result: configure, build and `ctest` all clean on both compilers, GUI and ALSA both included.
The base suite is `ac3tests` and `ac3perf`'s Catch2 cases plus one ctest entry per example
program; `AC3FORGE_WITH_ALSA`'s `tests/backend/alsa/` adds 15 entries (or, on a build that
selected pipewire/ instead, `tests/backend/pipewire/` adds 5), and the GUI's Qt Quick
Test harness (`ac3gui_qmltests`, `apps/gui/tests/CMakeLists.txt`) adds one more per `tst_*.qml`
suite under `apps/gui/tests/qml/` (21 today) — unlike every other GUI-related target, that one
harness *does* register its own `ctest` entries, gated on both
`AC3FORGE_BUILD_GUI` and `AC3FORGE_BUILD_TESTS`. A Linux build with neither ALSA nor the GUI
runs the base suite; with the GUI on and ALSA off it matches Windows exactly. `ac3gui --smoke`
also runs clean headless (`QT_QPA_PLATFORM=offscreen`), encoding real audio and instantiating
real QML channel meters. See [Linux audio](#linux-audio) for what the ALSA verification did,
and did not (real hardware), prove.

linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan,
linux-llvm-tsan (ThreadSanitizer over the `concurrency` ctest label — `tests/audio/` and the
headless CLI device paths — via `config-linux-llvm-tsan`), macos-llvm,
linux-appimage (builds `ac3gui`'s self-contained AppImage in an older `ubuntu:22.04` container and
smoke-tests it in a second container that never had Qt installed at all — see
[Linux](platforms/linux.md#appimage), roadmap DR8),
script-lint (ruff over every `.py`, shellcheck over every `.sh`, actionlint over the workflows,
all three pinned in `requirements/requirements-lint.txt`),
static-analysis (clang-tidy), coverage (`tools/checks/coverage_report.sh` over every `src/` library
component *and* `apps/cli`, via `config-linux-gcc-coverage`),
adm-validate (the opt-in ADM module) and ffmpeg-validate all run on every push, as does
build-android (the Shield app's debug APK) — the four Linux build legs install the same
Qt6/ALSA packages and build/smoke-test the GUI too. ffmpeg-validate is a
separate, CLI-only linux-llvm build that runs FFmpeg as an independent oracle against the full
layout/tool/metadata option space (see
[CONTRIBUTING.md's Oracles section](https://github.com/iainchesworthlabs/ac3forge/blob/main/CONTRIBUTING.md#oracles)) — a different question from the
[gold-reference gate](#gold-reference-correctness-gate) below, which every leg runs against one
fixed sample to check output *quality*; ffmpeg-validate instead checks that every option
combination produces a *structurally correct* stream at all, plus a numeric fidelity floor for
the Annex E tool combinations the one fixed gold-reference sample does not itself exercise. No
leg remains experimental.

The coverage job gates line and branch coverage per component, not as one blended
number, using the same GCC 16 pin as the other Linux legs; the floor table, the measurement each
floor was calibrated against, and why three components (`src/audio`'s device paths, `src/capi`'s
E-AC-3 surface, `apps/cli`'s device-dependent command modules) are honestly floored low all live
in `tools/checks/coverage_report.sh`, with the calibration history in the coverage job's own
comment in `ci.yml`.

On `pull_request` only, a `performance-compare` job builds `ac3bench`/`ac3kernelbench` at the
merge base and at the PR head on one runner and posts a table of per-workload deltas to the job
summary, using the same soft/hard tiers `tools/ci/append_performance_history.py` applies on
merge. It is informational and never fails a build — the blocking performance checks remain
`ac3perf`'s absolute real-time budget on every leg and the trend job's hard tier on push.

An `abi-gate` job (`ci.yml`) runs on the same advisory footing: on a code-touching change it
builds `config-linux-llvm-shared` at a comparison point in a git worktree beside HEAD, then
runs `abidiff` between the two and checks the actual exported dynamic-symbol set
(`tools/ci/check_abi_symbols.py`, `nm -D --defined-only`) against the checked-in allowlist.
On a pull request the comparison point is the PR's own merge base — the same ref
`performance-compare` uses, and the only one that answers "what does this branch do to the
ABI"; on a push or a tag it is the last release tag instead, which is the release-notes view.
`abidiff` runs under `tools/ci/abi-suppressions.ini`, which drops the libstdc++ template
instantiations that are not part of any ABI this project controls.

Both checks report into the job summary and leave the job green, gated on a single
`ABI_ENFORCE: 'false'` job-level variable; roadmap AP1's interface freeze is what would make
the gate required, by flipping that one value. When enforcing, `abidiff` fails only on an
*incompatible* change — a pure addition passes. The job is deliberately absent from
`CI Status`'s `needs` list either way.

`ABI_ENFORCE` deliberately replaces the `continue-on-error: true` this job used to carry.
That setting stops a failing job from failing the *workflow run*, but GitHub still reports the
job's own check run as `failure` — so the gate showed a red X on every pull request while
blocking nothing, and it hid genuine build failures behind the same state as an expected
pre-1.0 ABI change. With it gone, anything unexpected in this job is red and a policy finding
is not.

No macOS host exists for this project, so `config-macos-llvm`/`config-macos-llvm-debug` are only
ever exercised by CI (`macos-latest`, Apple Silicon) — never locally. That CI leg is green:
configure, build and `ctest` all clean, using a Homebrew-installed LLVM
(`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's bundled clang) rather than
a version-pinned one — Homebrew's core `llvm` formula has no versioned sibling the way
apt.llvm.org or the official Windows installer do, so unlike the other LLVM legs this one tracks
whatever Homebrew currently ships. The gold-reference correctness gate
(`tools/checks/verify_gold_reference.sh` — see [Gold-reference correctness gate](#gold-reference-correctness-gate)
below) also passes: real SNR numbers from that CI run were 61.81/61.82 dB on macOS, against
67.84/67.82 dB on Linux and Windows for the same material - a real but modest cross-compiler
floating-point difference, comfortably clear of the 30 dB gate. `macos-llvm` now builds the GUI
too (Homebrew's `qt` formula — see [GUI on macOS](platforms/macos.md#gui-on-macos)), which adds
the same per-suite `ac3gui_qml_tests_*` entries to that same suite the same way it does on Linux:
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
occupy on their own platforms), not the no-backend stub it fell back to before. `AC3FORGE_BUILD_GUI`
still defaults off there (`macos-llvm` opts it on in CI the same way the Linux legs do — see
[GUI on macOS](platforms/macos.md#gui-on-macos)) — capture, monitor playback and IEC 61937
passthrough compile and link for real either way, and `ac3tests` exercises the backend's
device-free logic (format matching, sample conversion) directly. What CI cannot exercise is a
real device: the hosted runner enumerates
whatever HAL objects macOS itself reports and touches nothing beyond that, same as ALSA's own
"verified headless" story below — see [Linux audio](#linux-audio) for the general shape of what
that does and does not prove, and [macOS](platforms/macos.md) for the backend's own header
comments on where its research came from (three independent real-world CoreAudio passthrough
implementations, surveyed since no Mac is available to try it on directly).

The `-arm64` presets are exercised in CI on GitHub's hosted `ubuntu-24.04-arm` runner (real ARM
hardware, not QEMU) and, separately, on a real Raspberry Pi 4B — see
[Raspberry Pi](platforms/raspberry-pi.md#verified-configuration) for the on-device numbers, which
are tracked there rather than duplicated here since that page is the canonical source for
Pi-specific hardware findings (real ALSA/HDMI device names, resolved compiler versions on Raspberry
Pi OS, and so on).

## SIMD kernels and the architecture tree

The codec's hot kernels are vectorised, and the vector types they are written against come from
a directory CMake chooses — never from an `#ifdef`. `src/forge/src/internal/arch/` holds
`generic/`, `x86_64/` and `aarch64/`, each carrying one identically-pathed
`ac3/internal/arch/simd.hpp`; `src/forge/CMakeLists.txt` puts exactly one of them on
`forge_objects`'s private include path, so every `#include "ac3/internal/arch/simd.hpp"` in the
core resolves to it and no translation unit ever asks what it is being compiled for. This is the
same mechanism `src/forge/src/internal/profiling/tracy_{enabled,disabled}/` uses for the
profiling seam and `src/audio/src/backend/<backend>/` uses for the operating system, and it is what
`tools/checks/check_platform_macros.ps1` exists to keep true (no preprocessor conditional anywhere
under `src/` or `apps/`).

`AC3FORGE_SIMD` forces a directory; `auto` (the default) resolves `x86_64` on x86-64, `aarch64` on
arm64, and `generic` on everything else — 32-bit x86, WebAssembly, anything unrecognised.
`generic` is a complete scalar implementation, not a stub: it is the reference the other two are
measured against, and `-DAC3FORGE_SIMD=generic` is what a reproducibility comparison should reach
for first when two machines disagree. The resolved value is printed by the configure summary and
by `ac3cli --version`, which reads it from the compiled header rather than from a
CMake-substituted string, so a binary cannot claim a directory it was not built with.

**What is vectorised.** The kernels live once, in shared code, written against the two 128-bit
types the header defines (`f64x2`, two doubles; `i32x4`, four 32-bit integers) — the directories
carry the types, not a copy of each kernel:

| Kernel | Where | Note |
|---|---|---|
| DCT-IV pre/post twiddles | `src/forge/src/core/mdct.cpp` | The complex multiplies either side of the FFT/DCT-IV core (`fft_kernel.hpp`, see the boundary note below). The core wants its input digit-reversed and returns its output in natural order, so the pre-twiddle gathers at stride ±2 and scatters to `bitrev[m]`, and the post-twiddle reads at stride +1 and scatters to stride ±2. Every gather and scatter stays scalar; only the arithmetic between them goes two-wide. Also has a real AVX2 tier — see [Runtime AVX2 dispatch](#runtime-avx2-dispatch). |
| IMDCT twiddle stages | `src/forge/src/core/mdct.cpp` | Both inverses' pre- and post-transform complex multiplies, same gather/scatter-around-the-core shape as above for the pre-twiddle, unit stride for the post-twiddle. Also has a real AVX2 tier. |
| analysis windowing | `src/forge/src/core/mdct.cpp` | 512 independent multiplies, unit stride throughout. Also has a real AVX2 tier. |
| `dft512` normalisation | `src/forge/src/core/fft.cpp` | Multiply by the exact reciprocal of 512, which is the same correctly-rounded result as the division it replaced. SSE2/NEON only — Phase 1's own measurement (below) found no reproducible AVX2 signal, since this is a small slice of a function `fft_kernel.hpp`'s own unaccelerated core dominates. |
| §7.2.2.2 exponent to PSD | `src/forge/src/core/bitalloc.cpp` | Four bins at a time. The only loop in the bit allocator that vectorises at all: §7.2.2.4's excitation function is a serial recurrence and §7.2.2.5's masking curve is a per-band conditional over 50 elements. SSE2/NEON only, same reason as `dft512` above. |
| `to_fixed25_block` | `src/forge/src/core/exponents.cpp` | Batched form of `to_fixed25`, about 9,100 calls a frame. SSE2/NEON only, same reason as `dft512` above. |

**The FFT/DCT-IV core itself is not part of this seam.** `src/forge/src/core/fft_kernel.hpp`
(ROADMAP PF4) is a radix-4 decimation-in-time kernel with a trailing radix-2 stage where
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
`cpuid`-gated mechanism that makes a *wider* tier safe to ship without that risk — ROADMAP PF5's
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

Two gates hold it. `tests/core/test_simd_kernels.cpp` compares each seam *primitive* — `f64x2`/
`i32x4` arithmetic, `round_ties_away`, `to_fixed25_block` — against a scalar reference in the same
binary and requires bit-for-bit equality, never a tolerance; on a `generic` build most of it is a
tautology, on every other build it is the whole argument. The kernels built from those primitives
are composition, not new arithmetic, so they inherit the guarantee rather than needing their own
bit-exact unit test — their correctness end to end is instead covered by
`tests/core/test_mdct_fast.cpp`'s existing tolerance check against the direct-form oracle and by
the corpus check below. Above it,
`tools/ci/run_codec_matrix.sh` run against two builds differing only in `AC3FORGE_SIMD` must
produce byte-identical output; that one covers restructuring the unit test cannot see:

```bash
cmake -S . -B build/simd-generic -DAC3FORGE_SIMD=generic
```

```bash
./tools/ci/run_codec_matrix.sh build/config-linux-gcc/bin/ac3cli /tmp/mx-simd && ./tools/ci/run_codec_matrix.sh build/simd-generic/bin/ac3cli /tmp/mx-generic && diff <(cd /tmp/mx-simd && find . -type f | sort | xargs sha256sum) <(cd /tmp/mx-generic && find . -type f | sort | xargs sha256sum)
```

## Runtime AVX2 dispatch

`AC3FORGE_SIMD` above answers "which architecture" and is a compile-time question — SSE2, NEON and
scalar are all guaranteed present on the architecture they target, so there is nothing to ask a
running CPU. AVX2 is different: it is a real feature a deployment machine might not have, so the
build machine cannot bake in a yes/no answer safely. `AC3FORGE_AVX2` (`ON` by default, x86_64 only)
compiles a second, AVX2-flagged tier alongside the baseline one; `ac3::internal::cpu::has_avx2()`
(`src/forge/src/internal/cpu/cpu_features.hpp`) decides at runtime, once per process, whether it is
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

**`AC3FORGE_SIMD_TIER`** (environment variable, read once inside that same cached initialisation)
overrides the answer: `sse2` always forces `has_avx2()` false, `avx2` forces it true — except when
the hardware genuinely cannot run AVX2, where forcing up `std::abort()`s with a clear message
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
codegen across a translation-unit boundary into a caller `has_avx2()` never approved for it.

**Testing — compile everywhere, execute only where capable.** `forge_simd_avx2` links into
`ac3tests` on every x86_64 leg unconditionally, proving the AVX2 code is valid, compilable,
linkable C++ on MSVC, clang-cl, GCC, Clang and AppleClang alike, with zero hardware dependency.
`tests/core/test_simd_kernels.cpp`'s `[avx2]`-tagged cases go further and actually execute it —
guarded by `has_avx2()`, with a loud, explicit `SKIP()` (never a silent pass) on hardware that
genuinely lacks it. The four x86_64 CI legs resolve to self-hosted-or-GitHub-hosted dynamically per
run and self-hosted CPU features are not documented anywhere in this repo, so no leg may assume the
host it landed on qualifies. `AC3FORGE_REQUIRE_AVX2=1` turns that skip into a hard failure instead —
set on the `linux-llvm-asan-ubsan` leg (`.github/workflows/_build.yml`), the one leg pinned to a
GitHub-hosted (rather than the dynamic self-hosted/GitHub-hosted `matrix.runner`) label, so there is
always at least one leg per PR where "the AVX2 path actually ran and passed" is a guaranteed, not
aspirational, statement.

`tools/ci/run_codec_matrix.sh`'s own `AC3FORGE_CROSS_TIER_CHECK=1` mode is the corpus-level
analogue of the `AC3FORGE_SIMD=generic` cross-*build* check above, but cross-*tier* from the SAME
binary: it runs the whole matrix twice, once under `AC3FORGE_SIMD_TIER=sse2` and once under `=avx2`,
and byte-diffs the two output trees. Wired into the same `linux-llvm-asan-ubsan` leg for the same
reason. On a host that cannot execute AVX2 (`/proc/cpuinfo` has no `avx2` flag) the second pass is
skipped with an explicit message and the script still exits 0 — degrading to exactly today's
guarantee, never silently claiming a check that did not run:

```bash
AC3FORGE_CROSS_TIER_CHECK=1 ./tools/ci/run_codec_matrix.sh build/config-linux-llvm/bin/ac3cli
```

ROADMAP PF5's dynamic-dispatch follow-on proved this whole mechanism end to end against one trivial
function first (`ac3::internal::avx2::avx2_probe_matches_expected()`, no codec bit-exactness stakes
of its own) — deliberately, so the build/link/dispatch/test pipeline was proven before any kernel's
correctness depended on it — then wired two real kernels behind it: `apply_analysis_window` and the
DCT-IV/IMDCT twiddle stages (`mdct.cpp`'s `dct4_pre_twiddle`/`dct4_post_twiddle`,
`imdct512_pre_twiddle`/`imdct512_negate_copy`/`imdct512_post_twiddle`, `imdct256_post_twiddle` —
see `src/forge/src/internal/avx2/mdct_avx2.hpp`).

**Which kernel gets wired is decided by measurement, not by which ones happen to be easiest.** A
symbol-scoped `perf record` comparison (generic-vs-x86_64-tier `ac3kernelbench`, PID-attributed
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

**Architecture-specific libm `sin`/`cos` — tested directly (roadmap VX11), also ruled out.** The
standing hypothesis was aarch64's own compiled `libm` (glibc ships an architecture-specific
`sincos`/`cos`/`sin`, so "the same libm" as a source package does not mean bit-identical machine
code) producing different last-bit results in the transform twiddle tables. Two things needed
correcting before that hypothesis could even be tested precisely: `kAnalysisWindow` is not built
from `std::cos`/`std::sin` at all — it is a `consteval` construction (`ac3/core/window.hpp`) using
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
was run end to end against a real `AC3FORGE_SIMD=aarch64` cross-build (same `-ffp-contract=off`
flag, same GCC 16, the actual `aarch64-neon` kernel real CI selects) under that same emulation, and
every one of its 32 checks' SNR numbers — not just the twiddle tables — came back bit-identical to
the native x86-64 build's, including the three streams this project's own encoder produces
(channel 4: 67.80/67.82/67.76 dB, matching x86-64 to the reported precision, not the ~61.8 dB every
real arm64/macOS CI leg measures). A `generic` (scalar reference kernel) build on the same x86-64
hardware matched both, for the same reason: IEEE-754 correctly-rounded arithmetic has no room for
two conforming implementations to disagree on `+`, `-`, `*`, or a `round()` pinned bit-exact by its
own test (`tests/core/test_simd_kernels.cpp`).

So the 6.02 dB gap does **not** reproduce under any standards-conformant aarch64 execution this
project can construct without the real hardware CI already has. That rules out "the aarch64
instruction set" as an explanation in the abstract, and narrows what is left to two candidates
neither locally reproducible: the specific *natively*-packaged aarch64 compiler GitHub's hosted
arm64/macOS runners use (as opposed to a Debian **cross**-compiler package, the only kind available
without that hardware — a native package can carry different default codegen/tuning even with
identical flags and the identical GCC version), or a genuine real-silicon floating-point behaviour
`qemu-user`'s software emulation does not reproduce. Both need the real runners to test further.

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

ROADMAP VX11's mystery therefore stays open — both hypotheses proposed for it are now closed out
by direct measurement rather than by argument — but it is no longer *unwatched*: a cross-platform
bitstream-hash gate (`tools/checks/check_cross_platform_hash.py`, wired into
[the gold-reference gate](#gold-reference-correctness-gate)) pins a SHA-256 of the actual encoded
bytes per `(kernel, transform mode)` pair in `tests/golden/bitstream-hashes.json`, so this specific
divergence — resolved or not — cannot silently change size without a CI failure pointing straight
at it. `x86_64-sse2` and `generic` are pinned from the measurements above; `aarch64-neon` and the
macOS kernel are deliberately left unpinned rather than pre-filled from the qemu measurement, since
that measurement is exactly the evidence that an emulated cross-build is not equivalent to a real
CI leg — the next person with those CI logs in front of them should pin what the real hardware
actually produces.

## Gold-reference correctness gate

`tools/checks/verify_gold_reference.sh` (invoked in CI on every leg except linux-llvm-asan-ubsan,
which stays diagnostic-only) is the first real implementation of the project's original
validation-pyramid design (now [docs/verification.md](verification.md)), which had never been
wired into CI before this: encode a fixed, checked-in 5.1 WAV
(`tests/golden/audio/reference_51.wav`, synthesized once by `tools/generators/gen_gold_reference_wav.py` —
independent of this codec's own encoder/decoder, not bootstrapped from one of our own encodes),
strict-decode the result with FFmpeg (`-err_detect crccheck+bitstream+buffer+explode`, checked
via stderr content rather than exit code — confirmed locally that ffmpeg's own process exits 0
even on a CRC mismatch), decode it again with ac3cli's own decoder, and assert the two decodes
agree via `tools/checks/compare_wav.py`'s delay-compensated SNR (stdlib-only Python, no numpy — every
CI-hosted runner already ships Python 3, so this needs no new provisioning). The gate's own three
checks are perceptual/SNR-based rather than a bit-exact bitstream comparison deliberately: nothing
in this project verifies that Homebrew LLVM, GCC and MSVC round the codec's floating-point
pipeline identically, and the real numbers above show they in fact do not, by a small but
measurable margin. `tools/checks/check_cross_platform_hash.py` runs immediately after (roadmap
VX11) and does add a bit-exact comparison, but as a pinned-regression gate over each leg's own
encoded bytes rather than a cross-leg equality assertion — see
[Floating-point contraction](#floating-point-contraction) above for why the latter cannot pass
today.

This is a narrow, cross-platform *quality* check — one sample, two codecs, every OS — not a
conformance sweep. `tools/checks/check_matrix_coverage.py`, `tools/ci/quality_race.py`'s `ci` mode and the
rest of the `ffmpeg-validate` CI leg (Linux-only, see [Verified configuration](#verified-configuration)
above) cover the *correctness* question instead: does every layout, every Annex E tool token and
every metadata option actually produce a structurally valid, spec-conformant stream, across the
full option space this gate does not attempt.
