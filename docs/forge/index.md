# Forge — the CLI and the GUI

Forge contains two applications built on the [ICL Forge library](../library/index.md):

- `forge` covers synthesis, encoding and decoding of AC-3, E-AC-3 (with its Atmos object layer)
  and AC-4, container wrapping, inspection, QC, and live capture and playback. See the
  [CLI reference](cli/index.md).
- `forge-gui` is a two-pane workbench over the same work: loading a source, choosing the codec
  (AC-3, E-AC-3 or AC-4), format and channels, placing and moving objects in a plan view, live
  capture (AC-3 and E-AC-3), metadata, QC, and channel-level metering, and it plays back and
  inspects a finished stream. It shows the equivalent `forge` command at the bottom of the
  window. See the [GUI guide](gui/index.md).

The CLI and GUI ship together where both are available.

## Installing

Choose an installation method:

- **A prebuilt archive** — every
  [release](https://github.com/iainchesworthlabs/iclforge/releases) publishes a
  `.zip`/`.tar.gz`/`.dmg` per platform with `forge` (and `forge-gui` where the leg builds it)
  inside. Windows also carries an NSIS `iclforge-<version>-win64.exe` installer from
  `0.10.0-beta.1` on; `0.9.0-beta.1` and earlier ship the `.zip` only.
- **Homebrew** (macOS/Linux) — the formula and cask are published to
  [`iainchesworthlabs/homebrew-iclforge`](https://github.com/iainchesworthlabs/homebrew-iclforge).
  `brew install iainchesworthlabs/iclforge/iclforge` builds and installs `forge`;
  `brew install --cask iainchesworthlabs/iclforge/iclforge` installs the prebuilt `forge-gui.app`
  from the release `.dmg`. Both paths are validated manually. The cask has not been installed
  end to end on a Mac. See
  [Homebrew formula and cask](../releasing.md#homebrew-formula-and-cask).
- **winget** (Windows) — the manifest is staged in-tree at
  [`packaging/winget/`](https://github.com/iainchesworthlabs/iclforge/tree/main/packaging/winget).
  It has not been submitted to `microsoft/winget-pkgs`, so
  `winget install iainchesworthlabs.iclforge` does not resolve. From a clone,
  `winget install --manifest packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<version>`
  installs `forge` and `forge-gui`. See
  [winget manifest](../releasing.md#winget-manifest).

Building from source is covered by [Quick start](../quickstart.md). Publication status for each
package is recorded under [Releasing](../releasing.md#what-gets-published).

The library ships separately, as the `iclforge-dev-*` archives and, on Linux, the
`libiclforge0` runtime package with `libiclforge-dev` (DEB) or `iclforge-devel` (RPM)
beside it; [What it is](../library/index.md) covers consuming it.

## Where to go next

- [CLI reference](cli/index.md) — `forge`'s commands, option grammars and JSON contracts.
- [GUI guide](gui/index.md) — `forge-gui` screen by screen, with screenshots.
- [Capabilities](../library/capabilities.md) — what the codec underneath both of them does.
- [Crucible](../crucible/index.md) — capture and position applications in an Atmos scene.
- [Hearth](../hearth/index.md) — play streams on a desktop or an ESP32 sink.
