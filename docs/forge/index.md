# Forge — the CLI and the GUI

**Forge** is the tooling over the [ac3forge library](../library/index.md): `ac3cli`, the
command-line front end, and `ac3gui`, the Qt Quick front end. Neither carries codec logic of its
own — every coding decision they make is a call into the same public API documented under
[Library](../library/index.md), and the GUI shows the equivalent `ac3cli` invocation live at the
bottom of its window.

The two are one product in every place a user meets them. They build from one option pair
(`AC3FORGE_BUILD_CLI`, `AC3FORGE_BUILD_GUI`), install as the one CPack `runtime` component, and
ship in one archive, one Windows installer, one `.deb`, one `.rpm`, one `.dmg` and one winget
entry. Downloading Forge gets you both.

- `ac3cli` covers synthesis, file encoding and decoding, container wrapping, inspection, QC, and
  live capture and playback across thirty-nine commands — see the
  [CLI reference](../cli/index.md).
- `ac3gui` is a two-pane workbench over the same work: loading a source, choosing format and
  channels, placing and moving objects in a plan view, live capture, metadata, QC, and
  channel-level metering — see the [GUI guide](../gui/index.md).

## About the word "forge"

`ac3forge` and `ac3::forge` name the library and the family's identifiers — the CMake project,
the package name, the C++ namespace, the C symbol prefix and every published package token.
**Forge**, capitalised and standing alone, names this pair of applications. "AC3Forge" is the
family in prose, so the three members read as AC3Forge's library, Forge and
[Crucible](../crucible/index.md); "AC3Forge Forge" is never written. `ac3cli --version` keeps
printing `ac3forge <version>`, because that is the library's version line and the released
Homebrew formula tests for it.

## Installing

Building from source ([Quick start](../quickstart.md)) always works, and is the only path that
covers every platform today. Three shorter ones exist, at different stages of readiness — see
[Releasing](../releasing.md#what-gets-published) for the per-tool status:

- **A prebuilt archive** — every
  [release](https://github.com/iainchesworthlabs/ac3forge/releases) publishes a
  `.zip`/`.tar.gz`/`.dmg` per platform with `ac3cli` (and `ac3gui` where the leg builds it)
  inside, no package manager or local clone needed. From the next release tag on, Windows
  also carries an NSIS `ac3forge-<version>-win64.exe` installer; `0.9.0-beta.1` and earlier
  ship the `.zip` only.
- **Homebrew** (macOS/Linux) — the formula and the cask are published to the live personal tap
  [`iainchesworthlabs/homebrew-ac3forge`](https://github.com/iainchesworthlabs/homebrew-ac3forge).
  `brew install iainchesworthlabs/ac3forge/ac3forge` builds and installs `ac3cli`;
  `brew install --cask iainchesworthlabs/ac3forge/ac3gui` installs the prebuilt `ac3gui.app`
  from the release `.dmg`. No CI runner has Homebrew on it, so both paths are validated by
  hand, and the cask has not yet been installed end to end on a Mac — see
  [Homebrew formula and cask](../releasing.md#homebrew-formula-and-cask).
- **winget** (Windows) — the manifest is staged in-tree at
  [`packaging/winget/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/winget)
  and its submission to `microsoft/winget-pkgs` is blocked on roadmap DR4 — the CLA, and the
  unsigned-binary Defender hit DR6 covers — so `winget install iainchesworthlabs.ac3forge`
  does not resolve. From a clone,
  `winget install --manifest packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<version>`
  installs the same thing — `ac3cli` and `ac3gui` together. See
  [winget manifest](../releasing.md#winget-manifest).

The library ships separately, as the `ac3forge-dev-*` archives and, on Linux, the
`libac3forge0` runtime package with `libac3forge-dev` (DEB) or `ac3forge-devel` (RPM)
beside it; [Conventions](../library/index.md) covers consuming it.

## Where to go next

- [CLI reference](../cli/index.md) — `ac3cli`'s commands, option grammars and JSON contracts.
- [GUI guide](../gui/index.md) — `ac3gui` screen by screen, with screenshots.
- [Capabilities](../library/capabilities.md) — what the codec underneath both of them does.
- [Crucible](../crucible/index.md) — the family's third member, a desktop application rather
  than a tool.
