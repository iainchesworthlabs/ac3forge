# macOS (Apple Silicon and Intel, Homebrew LLVM)

!!! note "Verified in CI only — no Mac host is available to this project"
    There is no macOS host available to this project locally; everything on this page has been
    exercised exclusively by two required CI legs: `macos-llvm`, on GitHub's `macos-latest` (Apple
    Silicon) runners, configuring the `config-macos-llvm` / `config-macos-llvm-debug` preset pair,
    and `macos-llvm-x64`, on GitHub's `macos-15-intel` runners (real native Intel hardware, not
    Rosetta emulation — confirmed against
    [docs.github.com's hosted-runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)),
    configuring `config-macos-llvm-x64` / `config-macos-llvm-x64-debug`. Neither is experimental any
    more. `macos-llvm`'s first-ever run surfaced one genuine, fully-understood issue (Homebrew's
    unpinned `llvm` formula flagging Catch2's `__COUNTER__` usage under `-Wc2y-extensions` — see
    `cmake/CompilerWarnings.cmake`), fixed in one commit, followed by two consecutive clean runs.
    `macos-llvm-x64` (DR8's new leg, on a brand-new `macos-15-intel` runner label never exercised
    before this project used it) went three consecutive clean runs — two `release.yml` dry runs and
    this feature branch's own required PR CI — at real gold-reference SNR numbers
    (67.80/67.82/67.76 dB, matching the x86 baseline every other non-arm64 leg reports) before its
    own `continue-on-error` escape hatch came off the same way
    (see [`.github/workflows/_build.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/_build.yml)),
    so a failure on either leg blocks like every other required leg now.

## Toolchain

Homebrew-installed LLVM (`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's
bundled clang), on the `arm64-macos-llvm` (Apple Silicon) or `x64-macos-llvm` (Intel) vcpkg
triplet — the toolchain file itself needed no change to support the second architecture; it
already resolved its target from `VCPKG_TARGET_ARCHITECTURE`, falling back to `uname -m` outside a
vcpkg port-build context. Unlike the Linux/Windows LLVM legs, neither is pinned to an exact
version: Homebrew's core `llvm` formula has no versioned sibling to pin against the way
`apt.llvm.org` or the official Windows installer do, so CI installs and reports whatever Homebrew
currently ships rather than asserting a specific one.

## Universal binaries (DR8)

A release's macOS package is a single **universal (arm64 + x86_64) `.dmg`**, not two per-arch
ones. `macos-llvm` and `macos-llvm-x64` each build and `cmake --install` their own single-arch
tree; a separate `package-macos-universal` job (also CI-only — it runs on `macos-latest`, any
macOS label works since `lipo`/`hdiutil` are the only tools it needs) `lipo -create`s every
Mach-O file the two trees have in common — `ac3cli`, `ac3gui`, and every dylib/framework binary
`qt_generate_deploy_qml_app_script` copies into `ac3gui.app/Contents/Frameworks/` — and packages
the merged tree with `hdiutil` directly, the same call CPack's own DragNDrop generator makes under
the hood. Proven for real in CI: `lipo -info` on the merged `ac3cli`/`ac3gui` binaries and at
least one bundled Qt framework binary reports both `x86_64` and `arm64` present in the same file —
see that job's own log, not just its exit code. This was a deliberate reversal of the original
roadmap text, which called a macOS universal binary "a separate decision, not a given" on the
assumption that Intel demand was doubtful and no Intel hosted runner existed; `macos-15-intel`
turned out to already exist, be free for public repos, and be real native hardware rather than
Rosetta, which removed the actual blocker (needing to cross-compile x86_64 from Apple Silicon, or
pay for a self-hosted Intel Mac) entirely.

Each leg's own single-arch `.dmg` still exists as a fast per-push packaging smoke test
(`packageable: true` never went away), it just isn't what a release publishes any more — see
[Packaging](#packaging) below and
[docs/releasing.md](../releasing.md#what-gets-published).

## Audio backend: CoreAudio

`src/audio/CMakeLists.txt` selects a real CoreAudio backend on macOS, `src/audio/src/backend/macos/`
— capture, monitor playback and IEC 61937 passthrough are built on the Audio HAL
(`AudioObjectID`/`AudioDeviceIOProc`), the same layer WASAPI and ALSA occupy on their own
platforms, rather than the no-backend stub that used to fall back to here. Its passthrough
mechanism is genuinely different from both: CoreAudio has no per-open bitstream flag the way
WASAPI's exclusive-mode subformat or ALSA's channel-status device name are, so bitstreaming means
taking hog mode on a digital output and retuning its *physical* stream format
(`kAudioStreamPropertyPhysicalFormat`) to `kAudioFormat60958AC3` for AC-3 — see
`src/audio/src/backend/macos/passthrough.cpp`'s own header for the full mechanism, cross-checked
against three independent real-world implementations of the same thing (MythTV, mpv, VLC) while
writing it, since there was no Mac available locally to try it on directly. For E-AC-3, the same
walk additionally probes a stream's available physical formats for `kAudioFormatEnhancedAC3`:
Apple's own documentation confirms Dolby Digital Plus/Atmos HDMI passthrough exists on Apple
Silicon Macs without documenting the HAL mechanism behind it, so where a driver doesn't publish
that format (older hardware, a non-HDMI output, an Intel Mac) the backend simply reports E-AC-3
passthrough unavailable rather than claiming it everywhere — see `passthrough.cpp`'s own "AC-3
and E-AC-3" section.

Passthrough **capture** — an input carrying somebody else's bitstream — needs none of that
machinery, on macOS or anywhere else: IEC 61937 bursts arrive as ordinary PCM samples, and
recognising them is `ac3::iec61937::PassthroughDetector`, which works off whatever interleaved
floats the backend delivers rather than off any HAL property. `ac3cli record` uses it to write
the elementary stream instead of encoding the bursts as audio, `ac3cli live` to stop rather than
encode a session of noise, and `ac3cli unspdif` does the same job on a capture already saved to
disk. That part is platform-independent and shares the verification the framing has — see
[Windows](windows.md#passthrough-capture) for what is and is not confirmed.

The backend is CI-verified only: the parts that need no live device — enumeration on a machine
with none, format matching, sample conversion — run under `ac3tests` on the hosted runner, same
as everywhere else without real hardware, but no real Mac has ever run this code against an
actual digital output, and no receiver has been asked to lock onto its output.

## Building

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-macos-llvm-debug
cmake --build --preset build-macos-llvm-debug
ctest --preset test-macos-llvm-debug
```

Swap `macos-llvm` for `macos-llvm-x64` throughout on Intel; drop `-debug` from all three preset
names for a Release build either way. The `ci-macos-llvm`/`ci-macos-llvm-x64` workflow presets
each chain the same three steps in one command. Homebrew's `llvm` formula must be installed (CI
runs `brew install llvm`), and `VCPKG_ROOT` must point at a vcpkg checkout — it supplies Catch2,
plus Boost and Tracy only if you opt into the `adm`/`profiling` features (see
[building.md](../building.md)). `AC3FORGE_BUILD_GUI` defaults **OFF** on both presets, as on Linux
— see [GUI on macOS](#gui-on-macos) below to opt in.

## GUI on macOS

`config-macos-llvm`/`config-macos-llvm-x64` both default `AC3FORGE_BUILD_GUI` to `OFF` for the
same reason the Linux presets do (see [GUI on Linux](../building.md#gui-on-linux)): a Qt kit
isn't assumed present on every Mac, not because `ac3gui` cannot be built here. `cmake/FindQt6.cmake`
already searches both Homebrew prefixes (`/opt/homebrew/opt/qt`/`/opt/homebrew/opt/qt6` on Apple
Silicon, `/usr/local/opt/qt`/`/usr/local/opt/qt6` on Intel), and `apps/gui/CMakeLists.txt`'s
`APPLE` branch — `MACOSX_BUNDLE`, the `.icns` bundle icon, and `qt_generate_deploy_qml_app_script()`
for packaging — was written for this from the start; it was simply never exercised until the
`macos-llvm` CI leg turned the option on. Opt in explicitly once Qt is installed:

```bash
brew install qt
cmake --preset config-macos-llvm-debug -DAC3FORGE_BUILD_GUI=ON
```

Homebrew's `qt` formula is the umbrella Qt6 package — one install pulls in QtDeclarative/QtQuick
and their build-time tooling (`qmlcachegen`) alongside QtCore/QtGui, unlike apt's split
`qt6-base-dev`/`qt6-declarative-dev` packages. The built app is a bundle,
`build/config-macos-llvm/bin/ac3gui.app` (or `build/config-macos-llvm-x64/...` on Intel);
`ac3gui --smoke` (the same headless check the other platforms run — see
[Verified configuration](../building.md#verified-configuration)) lives at
`ac3gui.app/Contents/MacOS/ac3gui`, not directly under `bin/`, because `MACOSX_BUNDLE` relocates
the executable there — the same property Windows' `WIN32_EXECUTABLE` sits beside but which only
takes effect on `APPLE`.

## Packaging

```bash
cpack --preset pack-macos-llvm       # or pack-macos-llvm-x64 on Intel
```

Produces a DragNDrop image on top of a plain ZIP when the packaging tool is found, the same way
NSIS is on Windows and DEB/RPM are on Linux — each leg's own single-arch `.dmg`/`.zip`, useful as
a fast per-push packaging smoke test of that architecture alone. Neither `macos-llvm` nor
`macos-llvm-x64` carries `release_package` any more, though: a real tagged release
(`release.yml`, `do_package: true`) instead runs a separate `package-macos-universal` job that
`lipo`-merges both legs' install trees into one universal `.dmg` and ships that as the canonical
macOS package — see [Universal binaries (DR8)](#universal-binaries-dr8) above and
[docs/releasing.md](../releasing.md#what-gets-published). That path has been exercised for real on
the arm64 half: nine beta releases, v0.2.0-beta.1 through v0.9.0-beta.1, shipped a macOS package
through the tag-triggered workflow before the universal merge existed. `cmake/Packaging.cmake`
needed no change for `ac3gui` to join either leg's own `.dmg`: which targets end up in a package
is decided entirely by which `install()` rules ran, and `ac3gui`'s already runs whenever
`AC3FORGE_BUILD_GUI` is `ON` — the DragNDrop generator itself is unconditional on `APPLE`, GUI or
not. No stable (non-beta) release has been tagged yet. See [Packaging](../building.md#packaging).

The `.app` bundle also declares `CFBundleDocumentTypes`/`UTExportedTypeDeclarations` for `.ac3`
and `.ec3` — a custom `Info.plist.in` rather than CMake's default template, since neither
extension is a system-known UTI and each needs its own `UTTypeConformsTo: public.audio`
declaration tying it to `audio/ac3`/`audio/eac3`. Configure/build-verified only, like the rest of
this file's GUI coverage below — nobody has opened a real `.ac3` file from Finder on real hardware
yet.

## CI: what has and has not been verified

Build, `ctest` (see [Verified configuration](../building.md#verified-configuration) for how the
suite's composition differs from Windows/Linux) and the [gold-reference correctness
gate](../building.md#gold-reference-correctness-gate) all pass on real GitHub Actions runners —
not a simulation or a local guess, on `macos-llvm`. `ac3gui_qmltests` registers and passes there
too: 582 ctest entries total, 100% passing, that one entry in 39.74s of a 56.81s total run - the
leg's first-ever GUI run, confirmed clean on a second push after two real fixes
(`QSG_RENDER_LOOP=basic` for a Qt Quick render-loop deadlock, and forcing the `Fusion` style in
the test binary for a native-`ComboBox`-under-offscreen hang - see [GUI on macOS](#gui-on-macos)
above and `apps/gui/tests/CMakeLists.txt`/`qml_test_main.cpp` for the full detail). Real SNR
numbers from the CI run that first proved the gate on macOS: 61.81/61.82 dB, against 67.84/67.82
dB on Linux and Windows for the same material, comfortably clear of the gate's 30 dB floor. That
gap is **not** a Homebrew-LLVM-libm-vs-glibc/MSVC difference, despite what this page and
`ci.yml` used to say — roadmap VX11 (see `docs/building.md`'s "Floating-point contraction"
section) traced it to every real arm64/aarch64 CI leg, `macos-llvm` included, landing on the
same ~6.0 dB offset from every x86 leg regardless of OS or C library, which rules out a
macOS-specific explanation; the gap tracks CPU architecture, and the actual mechanism is still
open pending real hardware access neither this project nor `qemu-user` emulation can substitute
for. `macos-llvm-x64` turned out to be new, confirmed data for exactly this question: same OS,
same Homebrew Qt/LLVM stack as `macos-llvm`, x86_64 instead of arm64, and its real gold-reference
numbers land with the other x86 legs — 67.80/67.82/67.76 dB across three separate real runs — not
with the ~61.8 dB every arm64/aarch64 leg (`macos-llvm` included) reports. That is further
evidence the offset is architecture-bound rather than OS-bound: two macOS legs on the same
toolchain now sit on opposite sides of the split, purely by CPU architecture. See `ci.yml`'s VX11
comment and [ROADMAP.md](../roadmap.md)'s VX11 entry for the fuller record.

---

If you get a Mac, that's still useful information for this project — running these instructions
on real local hardware, or actually launching `ac3gui.app` and using it (CI's `--smoke` run
proves it starts, loads its QML and drives a real encode headlessly, not that the interactive
experience is right), would be genuinely new. Consider filing an issue with what you found.
