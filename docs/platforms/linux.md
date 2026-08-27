# Linux

ac3forge builds and is tested on Linux today, on both GCC and Clang, CLI and GUI alike. This
page covers what is specific to Linux; for the full preset reference, options list and
troubleshooting, see [Building from source](../building.md).

## Toolchains

Built and tested with **GCC 16** and **Clang 22.1** on **Ubuntu 26.04 (WSL2)** — the versions
CI pins. Note that 26.04's own archive currently carries `gcc-16` as a pre-release/experimental
trunk snapshot in its `universe` component (`02-gcc-toolchain.sh` enables `universe` to reach
it) rather than the finished 16.1 release, which lands in a later Ubuntu series first — this is
Ubuntu's packaging timing, not a project choice, and the pin tracks whatever `apt` resolves for
`gcc-16` without further action needed here once 26.04 backports the stable release. The pin
itself is a CI reproducibility choice, not a hard floor of the code:
`cmake/toolchains/linux.{gcc,llvm}.toolchain.cmake` already `find_program` a fallback list
(`gcc-16, gcc, gcc-15, gcc-14, gcc-13` / `clang-22, clang, clang-21, clang-20`), so an older distro
compiler is picked up automatically — the [Raspberry Pi
validation](raspberry-pi.md#verified-configuration) built and passed the full suite with
GCC 14.2 and Clang 19.1.7.

`config-linux-gcc` and `config-linux-llvm` (each with a `-debug` variant) find the compiler by
that same known-name `find_program` walk, the same way the Windows presets pin MSVC/clang-cl,
rather than trusting whatever is first on `PATH`.

## Audio backend: ALSA, or PipeWire

On Linux, live capture (`ac3cli devices`/`record`), monitor playback (`ac3cli monitor`) and IEC
61937 bitstream passthrough (`ac3cli outputs`/`play`) are implemented over **ALSA** when its
headers are present, and over **PipeWire**'s native `pw_stream` API (not its ALSA-compatibility
shim) when they are not but PipeWire's are. Everything else is file I/O and needs no audio stack
at all — `ac3cli spdif` in particular reaches an AV receiver by writing a WAV, on any machine,
and `ac3cli unspdif` reads one back the same way.

Capture in the other direction — an S/PDIF or HDMI **input** carrying somebody else's bitstream
— is ordinary PCM as far as ALSA and PipeWire are concerned, exactly as passthrough output is
(see [Why ALSA still comes first](#why-alsa-still-comes-first) for why that is the shape of the
problem on Linux). Nothing in either API says "this is Dolby Digital", so `ac3cli record`
recognises the IEC 61937 burst framing itself and writes the elementary stream rather than
encoding the bursts as audio; `ac3cli live` detects the same thing and stops. Neither is
hardware-confirmed — see [What has and has not been verified](#what-has-and-has-not-been-verified)
below — but the burst framing they rely on is verified both ways against FFmpeg's `spdif` muxer.

Both dependencies are optional, detected packages:

```bash
sudo apt-get install libasound2-dev
```

(`alsa-lib-devel` on Fedora, `alsa-lib` on Arch), or

```bash
sudo apt-get install libpipewire-0.3-dev
```

(`pipewire-devel` on Fedora), or both — ALSA wins when both are present, see [Why ALSA still
comes first](#why-alsa-still-comes-first). No PulseAudio development headers, vcpkg port, or
runtime daemon are ever needed by either. Without either set of headers, configure succeeds
anyway and the build selects a no-backend fallback whose entry points return `kNoBackend`;
`ac3cli` marks the affected commands `UNAVAILABLE HERE` in its usage rather than pretending they
exist. `AC3FORGE_WITH_ALSA` and `AC3FORGE_WITH_PIPEWIRE` both default to `AUTO` (build the one
that's found); set either to `ON` to make its own missing headers a configure error instead,
which is what a packaging build wants.

### Why ALSA still comes first

Capture and monitor playback are ordinary PCM, which every Linux audio API can do, and both
backends implement them for real. Passthrough is the discriminator, and it's what the whole
project is for. On Linux, a bitstream isn't a distinct "format" the way it is on Windows: it's
opened as plain 16-bit stereo PCM, and what tells the receiver these bytes are Dolby Digital
rather than music is the IEC 60958 **channel status** travelling beside them (the non-audio bit,
AES0 bit 1). ALSA is the layer where that bit is expressed, as arguments on the device name
(`iec958:CARD=PCH,DEV=0,AES0=0x06,…`), and it works unconditionally the moment compatible
hardware exists.

PipeWire has its own real, current, native mechanism for the same bit — `SPA_MEDIA_SUBTYPE_
iec958`, confirmed against a real shipped client (Kodi's own PipeWire passthrough support), not
assumed from memory. What it lacks is ALSA's "just works": a PipeWire sink only offers a
compressed codec once its `iec958Codecs` control has been populated by the session manager,
configuration this library cannot perform on a caller's behalf. That gap — not a capability gap
— is why ALSA keeps first precedence whenever both are found, rather than PipeWire winning by
default for being the modern norm on most current desktops. The full reasoning, and the explicit
override for a machine where PipeWire's compressed codecs genuinely are configured, is in [Why
ALSA still comes first](../building.md#why-alsa-still-comes-first).

### What has and has not been verified

!!! note "ALSA is hardware-confirmed via Raspberry Pi; PipeWire is not, anywhere"
    The development loop itself — WSL2 Ubuntu 26.04 with GCC 15.2 and Clang 22.1 — is still
    headless: ALSA with libasound present and absent and under ASan+UBSan with leak detection;
    PipeWire (libpipewire-0.3 1.6.2) with the selection forced via `-DAC3FORGE_WITH_ALSA=OFF
    -DAC3FORGE_WITH_PIPEWIRE=ON`, since WSL2's image has both sets of headers and ALSA wins by
    default. The full test suite passes in every configuration tried. ALSA's device-independent
    halves (device-name construction, channel-status derivation, negotiation, the render/capture
    threads, start/stop, error mapping) were additionally driven end to end against ALSA's
    software `null` PCM device. WSL2 has no sound devices, no kernel sound modules, and no
    PipeWire session running at all, so nothing built there has ever been bitstreamed to a real
    S/PDIF or HDMI output from that environment, and PipeWire's own enumeration has only ever
    seen "no session" (`pw_context_connect()` failing fast, not a real graph with real nodes)
    rather than a genuine node to negotiate a compressed format against.

    That gap is now closed for ALSA specifically, on real hardware elsewhere: see
    [Raspberry Pi → Live HDMI passthrough to a real
    receiver](raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver) for a Pi 4B bitstreaming
    every AC-3/E-AC-3/Atmos shape tried to a real Atmos-capable AVR over HDMI, correctly
    identified every time. PipeWire remains unconfirmed against real hardware on any platform —
    this is a real, current gap, not a minor caveat — and whether a given output accepts a
    bitstream is per-device anyway; `ac3cli outputs` probes each one and reports what it finds.

## GUI: opt-in, not on by default

Unlike Windows, where `AC3FORGE_BUILD_GUI` defaults **ON**, both Linux presets default it
**OFF** — not because the GUI can't be built on Linux (`cmake/FindQt6.cmake` resolves a Linux Qt
kit the same way it resolves a Windows one, and `ac3gui` builds clean and passes its headless
`--smoke` run under both Linux presets in CI), but because a Qt kit isn't assumed to be present
on every Linux machine that builds this project. Opt in explicitly once Qt 6.5+ is installed:

```bash
cmake --preset config-linux-gcc-debug -DAC3FORGE_BUILD_GUI=ON
```

On Debian/Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools
```

Other distros need the equivalent Qt6 base + declarative (QML/Quick) packages (Fedora:
`qt6-qtbase-devel` / `qt6-qtdeclarative-devel`). See [GUI on Linux](../building.md#gui-on-linux)
for the CMake warnings you'll see about unlinked QML plugins (harmless — a property of how
distro-packaged Qt6 is built, not a missing dependency).

## Building

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-linux-gcc-debug
cmake --build --preset build-linux-gcc-debug
ctest --preset test-linux-gcc-debug
```

Substitute `linux-llvm` for `linux-gcc` to build with Clang instead. Add
`-DAC3FORGE_BUILD_GUI=ON` to either configure line to build `ac3gui` too, once Qt is installed
(see [GUI](#gui-opt-in-not-on-by-default) above). `VCPKG_ROOT` must point at a vcpkg checkout —
it supplies Catch2, plus Boost and Tracy only if you opt into the `adm`/`profiling` features
(see [building.md](../building.md)), same as on Windows; this project's own convention keeps it
at `/opt/vcpkg`, but any path works.

## Packaging

```bash
cpack --preset pack-linux-gcc
```

(or `pack-linux-llvm` for the Clang build; append `-arm64` for the arm64 presets). Produces a
plain tarball, plus DEB/RPM on top when the corresponding packaging tool is on `PATH`. A local
run packages whatever the tree was configured with — the GUI only if you opted in. Tagged
releases run `pack-linux-gcc` and `pack-linux-gcc-arm64` for real, and CI configures those legs
with `-DAC3FORGE_BUILD_GUI=ON`, so released Linux packages include `ac3gui`; a real arm64 `.deb`
has also been produced and inspected on Raspberry Pi hardware (see
[Raspberry Pi](raspberry-pi.md#verified-configuration)). See
[Packaging](../building.md#packaging).

A GUI-enabled package also installs `ac3gui.desktop` (`Exec=ac3gui %F`, `MimeType=audio/ac3;
audio/eac3;`), an AppStream metainfo file, and a shared-mime-info fragment declaring the two media
types against `*.ac3`/`*.ec3` — `apps/gui/packaging/linux/`, wired into `install()` behind
`if(LINUX)` in `apps/gui/CMakeLists.txt`. Configure/build-verified only: nobody has installed the
resulting `.deb`/`.rpm` on a real desktop and double-clicked an `.ac3` file to confirm the
launcher fires.

## AppImage

The `.deb`/`.rpm` above are only as portable as the host distro's own Qt 6 packaging: a distro
whose Qt is too old for this project's `find_package(Qt6 6.5 REQUIRED ...)` floor, or whose
`qml6-module-*` split doesn't match what `qt6-declarative-dev`/`qt6-declarative-dev-tools` expect
(roadmap DR8), simply cannot install one of them. `ac3gui` also ships as a self-contained
AppImage that carries its own Qt 6 and QML modules, so that gap doesn't apply — the two package
kinds are complementary, not a replacement for each other.

**AppImage, not Flatpak, and this is a settled decision, not an open question.** `ac3gui`'s whole
reason to exist is `ac3::audio`'s IEC 61937 passthrough — locking a device in exclusive/hog mode
and writing a raw compressed bitstream straight to an AVR, over ALSA's `iec958:...,AES0=0x06`
device arguments or PipeWire's native `SPA_MEDIA_SUBTYPE_iec958` (see [Why ALSA still comes
first](#why-alsa-still-comes-first) above). That is exactly the kind of raw device access
Flatpak's sandbox exists to take away by default; getting it back would mean either a portal that
doesn't speak this vocabulary or blanket `--device=all`/`--filesystem=host` opt-outs that defeat
the sandbox for no real benefit here, on top of a Flathub review process and a second (KDE) Qt 6
runtime to track alongside this project's own pinned Qt version. AppImage carries no sandbox at
all, so ALSA/PipeWire device access from inside one works exactly like it does from a normally
installed binary — no portal, no opt-out flags, nothing to maintain going forward.

### Build recipe

```bash
cmake --preset config-linux-gcc -DAC3FORGE_BUILD_GUI=ON
cmake --build --preset build-linux-gcc
cmake --install build/config-linux-gcc --prefix AppDir/usr --component runtime
# plus --component library/libruntime too, only if `ldd` on the built ac3gui binary
# shows it dynamically linking a project .so (BUILD_SHARED_LIBS=ON) - see the
# linux-appimage CI job for the check.
linuxdeploy-x86_64.AppImage --appdir AppDir \
  --executable AppDir/usr/bin/ac3gui \
  --desktop-file AppDir/usr/share/applications/ac3gui.desktop \
  --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/ac3gui.png \
  --plugin qt --output appimage
```

The `apps/gui/CMakeLists.txt` `if(LINUX)` block described above already installs exactly the
XDG-shaped layout [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) expects for an
AppDir — the `--install --component runtime` line is the whole of the CMake side of this, no
extra packaging target needed.

### The actual glibc floor

CI builds the AppImage in an `ubuntu:22.04` container — deliberately *older* than the
`ubuntu:26.04` every other Linux CI leg uses — because an AppImage's whole purpose is running on
a distro older than, or differently put together from, the one that built it; compiling it
against 26.04's much newer glibc would make the result *less* portable than the `.deb`/`.rpm` it
exists to supplement. Ubuntu 22.04 ships glibc 2.35 (`ldd --version`), so that is the floor this
AppImage carries: it needs at least glibc 2.35 on the machine that runs it, and nothing older.
`.github/toolchain/02-gcc-toolchain.sh`'s existing archive-then-PPA fallback (see that script's
own header) reaches GCC 16 on 22.04's older archive with no changes needed; Qt comes from
`jurplel/install-qt-action` rather than `apt`, decoupling the Qt version entirely from the base
image's age, the same way the Windows CI leg already does.

### How this is verified

Configure/build/package-verified in CI (the `linux-appimage` job in `.github/workflows/_build.yml`,
which runs on every push, the same "standing smoke test of the packaging path" reasoning
[windows.md](windows.md#packaging) gives for why `windows-msvc` packages continuously) — and, one
step further than a plain `.deb`/`.rpm` gets, actually run: the same job then launches the built
AppImage headlessly (`ac3gui --smoke`, `QT_QPA_PLATFORM=offscreen`) inside a **second, separate
container that never had Qt or a single build tool installed** — `debian:12-slim`, reached via
Docker against the GitHub-hosted runner's own Docker daemon — and asserts it exits 0. That is the
concrete answer to "does this actually run on a distro whose own Qt packages were never
installed", not just "did packaging exit 0". No real desktop has installed the produced
`.AppImage` and double-clicked an `.ac3` file, the same caveat the `.deb`/`.rpm` packaging above
carries.

One package is still installed in that second container first: `libasound2`, ALSA's runtime
library, which `debian:12-slim` doesn't ship at all. This is deliberate, not a gap in the AppImage
— it draws the exact same boundary the `.deb`/`.rpm` already draw (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`/
`CPACK_RPM_PACKAGE_AUTOREQPROV` in `cmake/Packaging.cmake` declare `libasound2` as a runtime
package dependency rather than bundling it), because ALSA is part of the base multimedia stack on
effectively every real desktop Linux install, unlike Qt6, which is exactly the piece this AppImage
exists to stop depending on the host for. Bundling `libasound.so.2` instead would also risk a
worse failure than not bundling it: ALSA's plugin/config ecosystem
(`/usr/lib/*/alsa-lib/`, `/etc/asound.conf`) is tied to the *host* system, so a bundled `.so` with
none of that around it could load and then simply fail to enumerate any real device — the opposite
of the point made above for why AppImage was chosen over Flatpak in the first place ("ALSA/PipeWire
access works exactly like a normal installed binary").

## CI

`linux-gcc` and `linux-llvm` both run on every push and are **required**; both install a Qt6 kit
and build and smoke-test `ac3gui` in addition to the CLI. Two sanitizer legs,
`linux-llvm-asan-ubsan` (AddressSanitizer + UndefinedBehaviorSanitizer) and `linux-llvm-tsan`
(ThreadSanitizer, over the `concurrency` ctest label only — `tests/audio/` plus
`tests/cli/test_cli_live.cpp`), are also required and both stay **CLI-only on purpose**, to keep a
Qt kit out of the sanitizer legs' install time. They are separate presets because the two runtimes
are mutually exclusive: Clang refuses `-fsanitize=address,thread`.

Two more legs, `linux-gcc-arm64` and `linux-llvm-arm64`, run the same matrix on real ARM hardware
(GitHub's `ubuntu-24.04-arm` hosted runner, not QEMU emulation) — see
[Raspberry Pi](raspberry-pi.md), which is the hardware this arch target is validated
against.

A fifth Linux leg, `linux-appimage`, is separate from all of the above: an `ubuntu:22.04`
container (deliberately older than `ubuntu:26.04`) building `ac3gui`'s AppImage and then
launching it inside a second container that never had Qt installed at all — see
[AppImage](#appimage) above for what it builds and why.

The ALSA backend adds 15 tests of its own (`tests/backend/alsa/`) on top of the base suite: a
Linux build with the GUI on and `libasound2-dev` absent runs the same suite as Windows, and ALSA
adds those 15. `ctest --preset test-linux-gcc-debug` (or whichever preset matches your build)
runs the full suite. See [Verified configuration](../building.md#verified-configuration)
for the full CI matrix.
