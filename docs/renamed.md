# Renamed

ICL Forge was called AC3Forge up to the pre-release 0.10.0-beta.1, and its programs were `ac3cli`,
`ac3gui`, `ac3hearth` and `ac3crucible`. The family, the programs, the libraries, the packages and
the repository have new names. Nothing answers to both: there is no alias, no launcher and no
compatibility layer. This page puts each old name beside the new one, for someone who installed a
pre-release, built against one, or keeps a script that calls it.

The tree uses the new names. A package, a release file or a formula carries a new name from the
first release made after the repository was renamed; the pre-releases keep the names they were
published under. The first table says which release carries which.

## What a release carries

| | Releases up to `v0.10.0-beta.1` | The first release made after the rename, and later |
|---|---|---|
| Programs in the archives | `ac3cli`, `ac3gui` | `forge`, `forge-gui` |
| Release files | `ac3forge-<version>-<platform>.<ext>`, `ac3forge-dev-<version>-<platform>.<ext>`, `ac3gui-<version>-x86_64.AppImage`, `ac3forge-shield-v<version>.apk`, `ac3forge-signing-key.asc`, `ac3forge-<version>.spdx.json` | `iclforge-<version>-<platform>.<ext>`, `iclforge-dev-<version>-<platform>.<ext>`, `forge-gui-<version>-x86_64.AppImage`, `iclforge-shield-v<version>.apk`, `iclforge-signing-key.asc`, `iclforge-<version>.spdx.json` |
| Debian and RPM packages | `ac3forge`, `libac3forge0`, `libac3forge-dev` (RPM: `ac3forge-devel`) | `iclforge`, `libiclforge0`, `libiclforge-dev` (RPM: `iclforge-devel`); the Hearth and Crucible packages are `iclforge-hearth` and `iclforge-crucible` |
| PyPI | the project `ac3forge` (0.9.0b1 and 0.10.0b1), module `ac3forge` | the project `iclforge`, module `iclforge`; its description reads "formerly ac3forge" |
| Homebrew | the tap `iainchesworthlabs/ac3forge` (repository `homebrew-ac3forge`): formula `ac3forge`, cask `ac3gui` | the tap `iainchesworthlabs/iclforge` (repository `homebrew-iclforge`): formula and cask `iclforge`; the tap's `tap_migrations.json` maps the two old names to them |
| winget | not published: a submission under `iainchesworthlabs.ac3forge` was closed unmerged | `iainchesworthlabs.iclforge`, for a release that is submitted |
| npm, crates.io, the ESP Component Registry | not published | not published; the packages are `iclforge-wasm-decoder`, `iclforge` and `iclforge-sys` |

The release files of a pre-release keep the names they were published with. A link to
`ac3forge-0.10.0-win64.exe` stays right, and the old tags, release pages and files stay where they
are. The pages of this site link the [releases page](https://github.com/iainchesworthlabs/iclforge/releases)
and describe the files by their pattern, so that they do not name a file the latest release
lacks.

The repository is `iainchesworthlabs/iclforge`, and the documentation site is
`https://iainchesworthlabs.github.io/iclforge/`. GitHub redirects the old repository address, its
clones and its release URLs. It does not redirect the old documentation address,
`iainchesworthlabs.github.io/ac3forge`, which ended with the rename.

## Programs

| Old | New |
|---|---|
| `ac3cli` | `forge` |
| `ac3gui` | `forge-gui` |
| `ac3hearth`, `ac3hearth-render`, `ac3hearth-testsink`, `ac3hearth-testserver` | `hearth`, `hearth-render`, `hearth-testsink`, `hearth-testserver` |
| `ac3crucible`, `ac3crucible-run` | `crucible`, `crucible-run` |
| `ac3tests`, `ac3probe`, `ac3perf`, `ac3bench`, `ac3membench`, `ac3kernelbench`, `ac3fuzz`, `ac3test` (built from the tree, not released) | `iclforge-tests`, `iclforge-probe`, `iclforge-perf`, `iclforge-bench`, `iclforge-membench`, `iclforge-kernelbench`, `iclforge-fuzz`, `iclforge-test` |

What each program registers follows its name: the man page (`forge.1`), the completions for four
shells (`_forge` for zsh), the desktop entries and AppStream ids, the macOS bundle identifiers
(`com.iainchesworthlabs.forge-gui`, `com.iainchesworthlabs.hearth`), the Windows file type
(`AC3Forge.Stream` is `IclForge.Stream`) and winget's command aliases. `forge --version` prints
`iclforge <version>`, the library's version line, where `ac3cli --version` printed
`ac3forge <version>`.

`forge` is also the name of Foundry's command (the Ethereum toolkit) and of Laravel Forge's. On a
computer that has either, the one that comes first on the `PATH` runs. Put ICL Forge's directory
before it, or call `forge` by its full path, when a shell answers with the wrong program.

## Libraries and headers

| Old | New |
|---|---|
| C++ namespaces `ac3::`, `ac4::`, `mp4::`, `matroska::`, `mpegts::`, `iamf::`, `ac3iab::`, `ac3adm::` | `iclforge::` (the codec's own names), `iclforge::ac4::`, `iclforge::mp4::`, `iclforge::matroska::`, `iclforge::mpegts::`, `iclforge::iamf::`, `iclforge::iab::`, `iclforge::adm::` |
| Header roots `ac3/`, `ac4/`, `mp4/`, `matroska/`, `mpegts/`, `iamf/`, `ac3iab/`, `ac3adm/`, `ac4dec/`, `ac4enc/` | `iclforge/<library>/`: `iclforge/ac3/`, `iclforge/ac4/`, `iclforge/mp4/` and so on; the [header map](library/header-map.md) lists them |
| The C API: `ac3forge_c/ac3forge.h`, `ac3forge_*`, `AC3FORGE_*`, `libac3forge_c` | `iclforge_c/iclforge.h`, `iclforge_*`, `ICLFORGE_*`, `libiclforge_c` |
| CMake package `find_package(ac3forge)` | `find_package(iclforge)` |
| CMake options and cache variables `AC3FORGE_*` (`AC3FORGE_BUILD_ADM`, `AC3FORGE_SIMD`, ...) | `ICLFORGE_*` |
| Kconfig symbols `CONFIG_AC3FORGE_*` | `CONFIG_ICLFORGE_*` |
| pkg-config `ac3forge`, `ac3signing`, `matroska`, `mp4`, `mpegts`, `iamf`, `ac3iab`, `ac3adm`, `admbridge`, `ac3forge_c`, `ac4`, `ac4dec`, `ac4enc`, `ac4core` | `iclforge-<library>`: `iclforge-ac3`, `iclforge-signing`, `iclforge-matroska`, ..., `iclforge-c`, `iclforge-ac4core` |
| Library files `libac3forge`, `libac3forge_c`, `libmp4`, ... | `libiclforge_<library>`, with `_static` for the archive |

The codec library `ac3::forge` became one library of the 22 under `src/`, and five of its parts
became libraries of their own: `base`, `dsp`, `objects`, `render` and `iec61937`. A program that
linked only `ac3::forge` links `iclforge::ac3`, which links those five; the include path of a
header that moved changed with it (`ac3/core/layout.hpp` is `iclforge/base/layout.hpp`,
`ac3/render/render.hpp` is `iclforge/render/render.hpp`). The CMake targets:

| Old | New |
|---|---|
| `ac3::forge`, `ac3::forge_static`, `ac3::forge_shared` | `iclforge::ac3`, `iclforge::ac3_static`, `iclforge::ac3_shared` |
| `ac3::forge_minimal` | `iclforge::ac3_minimal` |
| `ac3::forge_c` (`_static`, `_shared`) | `iclforge::c` (`iclforge::c_static`, `iclforge::c_shared`) |
| `ac3::audio`, `ac3::sendspin`, `ac3::signing`, `ac3::admbridge`, `ac3::arithmetic` | `iclforge::audio`, `iclforge::sendspin`, `iclforge::signing`, `iclforge::admbridge`, `iclforge::arithmetic` |
| `matroska::matroska`, `mp4::mp4`, `mpegts::mpegts`, `iamf::iamf` | `iclforge::matroska`, `iclforge::mp4`, `iclforge::mpegts`, `iclforge::iamf` |
| `ac3iab::ac3iab`, `ac3adm::ac3adm` | `iclforge::iab`, `iclforge::adm` |
| `ac4::ac4`, `ac4::decoder`, `ac4::encoder`, `ac4::core` | `iclforge::ac4`, `iclforge::ac4dec`, `iclforge::ac4enc`, `iclforge::ac4core` |

## Bindings and environment

| Old | New |
|---|---|
| Python: `import ac3forge`, the extension `_ac3forge` | `import iclforge`, `_iclforge` |
| Rust: the crates `ac3forge` and `ac3forge-sys` | `iclforge` and `iclforge-sys` |
| npm: `ac3forge-wasm-decoder`; the factories `createAc3ForgeModule`, `Ac3ForgeDecoderNode` | `iclforge-wasm-decoder`; `createIclForgeModule`, `IclForgeDecoderNode` |
| Environment variables `AC3FORGE_SIGNING_KEY`, `AC3FORGE_SIGNING_KEY_FILE`, `AC3FORGE_SIMD_TIER`, ... | `ICLFORGE_SIGNING_KEY`, `ICLFORGE_SIGNING_KEY_FILE`, `ICLFORGE_SIMD_TIER`, ... |
| A program's variables: `AC3CLI_*`, `AC3GUI_*` (and `AC3_GUI_*`), `AC3HEARTH_*`, `AC3CRUCIBLE_*` (and the desktop demo's `AC3DESK_*`) | `ICLFORGE_CLI_*`, `ICLFORGE_GUI_*`, `ICLFORGE_HEARTH_*`, `ICLFORGE_CRUCIBLE_*` |

## Strings that two sides read

| Old | New |
|---|---|
| The Sendspin extension role `_ac3forge_player@v1` | `_iclforge_player@v1` |
| The sink firmware's project name `ac3forge_hearth_sink` | `iclforge_hearth_sink` |
| `forge probe` JSON schema `ac3forge.probe/1`; Hearth's `ac3forge.hearth.media/1`; the scene file's `ac3forge_scene` | `iclforge.probe/1`, `iclforge.hearth.media/1`, `iclforge_scene` |
| The writing application of the MP4 and Matroska files `forge` writes: `ac3forge` | `iclforge` |
| The Windows silent device's driver: the hardware id `ROOT\Ac3ForgeNullSink`, the service `Ac3ForgeNullSink`, the files `Ac3ForgeNullSink.sys` and `.inf` and `ac3forgenullsink.cat` | `ROOT\IclForgeNullSink`, `IclForgeNullSink`, `IclForgeNullSink.sys` and `.inf` and `iclforgenullsink.cat` |
| The endpoint that driver shows, which Crucible finds by its name: "Speakers (Desktop Atmos)" | "Speakers (Crucible Silent Output)" |

**Every flashed ESP32 board needs one flash over USB.** A board running a firmware from before the
rename advertises `_ac3forge_player@v1` and its update check wants the project name
`ac3forge_hearth_sink`; Hearth and the update tool after the rename look for the new names, so
neither side accepts the other's and an update over the network cannot bridge the change. The
ESP32-S3, the ESP32-C6 and the ESP32-P4 all flash over USB, with the browser installer or
`esptool`; from that flash on, updates over the network work by the new names.

## Settings, and what the programs copy

`forge-gui`, Hearth and Crucible stored their settings under the organisation `ac3forge`, and
store them under `iclforge` now, with the application names `forge-gui`, `Hearth` and `Crucible`.
At its first start each of them copies what the old names stored, before anything reads a setting:
the keys of the old application store (the preferences and session, the language, Hearth's paired
sinks, pairing keys and server identity, Crucible's first-run state) and the files under the old
per-user directories (data, local data and configuration; Qt's own cache is left). The copy
happens once, when the new store holds nothing and the old one holds something, and a key of the new
store records it. The old store and the old files are never written, renamed or removed, so going
back to a pre-release finds everything where it was left. On Windows the old keys stay under
`HKCU\Software\ac3forge`, and Qt creates the empty key of a store it reads, so a machine that never
had one gets that key with no value in it; delete it whenever you like, since nothing reads it.

The Windows driver was never in a release, and the only installed copy known is the one in the
project's test guest. Crucible's silent-device filter (Settings, Advanced) is copied with the other
settings and keeps whatever was typed in it, so a filter that says "Desktop Atmos" no longer
matches the endpoint. Clearing it brings back the default, which is the platform's own name for
its silent device.

## Installed copies

**Windows.** The new installer does not replace the old files, because the programs have new names.
Uninstall the old installation by hand: until its uninstaller has run, the old file association stays
and an old `ac3hearth` Start menu entry points at a program that no longer updates. Hearth asks for a
Windows Firewall rule per executable, named from the executable (`... (ac3hearth)`,
`... (ac3hearth-testsink)`); those rules stay, harmless and unused, and
`Get-NetFirewallRule -DisplayName '*(ac3hearth*'` lists them for removal. The first start of
`hearth.exe` asks for its own rule, as the first start of `ac3hearth.exe` did.

**Android.** The Shield app's application id is `com.iclforge.shield`, so a pre-release APK of
`com.ac3forge.shield` stays installed beside the new app and is not upgraded. Uninstall the old one.

**Homebrew.** Once a release made after the rename is in the tap, `brew install ac3forge` and
`brew install --cask ac3gui` are redirected by the tap's migration map to the new formula and cask.
Both are `iclforge`: `brew install iclforge` for `forge`, `brew install --cask iclforge` for
`forge-gui.app`. GitHub redirects the renamed repository, so a tap added under the old name keeps fetching
its updates. Between the rename and that release, `brew install ac3forge` builds `v0.10.0-beta.1` from
a source tarball that GitHub now generates under the new repository name, so it fails its checksum;
the cask and the installed copies are not affected.
