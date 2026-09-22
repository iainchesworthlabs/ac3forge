# Forge — the CLI and the GUI

Forge contains two applications built on the [ac3forge library](../library/index.md):

- `ac3cli` covers synthesis, file encoding and decoding, container wrapping, inspection, QC, and
  live capture and playback. See the [CLI reference](cli/index.md).
- `ac3gui` is a two-pane workbench over the same work: loading a source, choosing format and
  channels, placing and moving objects in a plan view, live capture, metadata, QC, and
  channel-level metering. It shows the equivalent `ac3cli` command at the bottom of the window.
  See the [GUI guide](gui/index.md).

The CLI and GUI ship together where both are available.

## Installing

Choose an installation method:

- **A prebuilt archive** — every
  [release](https://github.com/iainchesworthlabs/ac3forge/releases) publishes a
  `.zip`/`.tar.gz`/`.dmg` per platform with `ac3cli` (and `ac3gui` where the leg builds it)
  inside. From the next release tag on, Windows
  also carries an NSIS `ac3forge-<version>-win64.exe` installer; `0.9.0-beta.1` and earlier
  ship the `.zip` only.
- **Homebrew** (macOS/Linux) — the formula and cask are published to
  [`iainchesworthlabs/homebrew-ac3forge`](https://github.com/iainchesworthlabs/homebrew-ac3forge).
  `brew install iainchesworthlabs/ac3forge/ac3forge` builds and installs `ac3cli`;
  `brew install --cask iainchesworthlabs/ac3forge/ac3gui` installs the prebuilt `ac3gui.app`
  from the release `.dmg`. Both paths are validated manually. The cask has not been installed
  end to end on a Mac. See
  [Homebrew formula and cask](../releasing.md#homebrew-formula-and-cask).
- **winget** (Windows) — the manifest is staged in-tree at
  [`packaging/winget/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/winget).
  It has not been submitted to `microsoft/winget-pkgs`, so
  `winget install iainchesworthlabs.ac3forge` does not resolve. From a clone,
  `winget install --manifest packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<version>`
  installs `ac3cli` and `ac3gui`. See
  [winget manifest](../releasing.md#winget-manifest).

Building from source is covered by [Quick start](../quickstart.md). Publication status for each
package is recorded under [Releasing](../releasing.md#what-gets-published).

The library ships separately, as the `ac3forge-dev-*` archives and, on Linux, the
`libac3forge0` runtime package with `libac3forge-dev` (DEB) or `ac3forge-devel` (RPM)
beside it; [What it is](../library/index.md) covers consuming it.

## Where to go next

- [CLI reference](cli/index.md) — `ac3cli`'s commands, option grammars and JSON contracts.
- [GUI guide](gui/index.md) — `ac3gui` screen by screen, with screenshots.
- [Capabilities](../library/capabilities.md) — what the codec underneath both of them does.
- [Crucible](../crucible/index.md) — capture and position applications in an Atmos scene.
- [Hearth](../hearth/index.md) — play streams on a desktop or an ESP32 sink.
