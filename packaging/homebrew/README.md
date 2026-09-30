# Homebrew formula and cask

`Formula/ac3forge.rb` packages `ac3cli` — the CLI only, built from the release source
tarball. `Casks/ac3gui.rb` packages `ac3gui` — the GUI, as the prebuilt `.app` bundle from a
release's DragNDrop `.dmg`. Both live here first and are copied into the personal tap
[`iainchesworthlabs/homebrew-ac3forge`](https://github.com/iainchesworthlabs/homebrew-ac3forge),
which is public and carries both at `v0.10.0-beta.1`. Neither is submitted to `homebrew-core`.

```bash
brew install iainchesworthlabs/ac3forge/ac3forge          # ac3cli, built from source
brew install --cask iainchesworthlabs/ac3forge/ac3gui     # ac3gui.app, prebuilt
```

After a release, [`manifest-bump.yml`](../../.github/workflows/manifest-bump.yml) rewrites both
files for the new tag and, when `HOMEBREW_TAP_TOKEN` is set, opens a pull request on the tap. The
tap's `main` accepts only pull requests, and a person merges each one after validating on a macOS
machine; the tap's pull requests #1 to #3 are the bumps to `v0.8.0-beta.2`, `v0.9.0-beta.1` and
`v0.10.0-beta.1`. See [docs/releasing.md](../../docs/releasing.md#homebrew-formula-and-cask).

## Why a personal tap, not `homebrew-core`

`homebrew-core` has its own bar a submission has to clear on top of the formula being
technically correct: notability (real, sustained usage — GitHub stars/forks/watchers, not
just "it exists"), a track record of maintenance, and a formula that needs no unusual
patching to build cleanly with Homebrew's own toolchain on every supported macOS version.
ac3forge does not clear that bar yet. A personal tap (`iainchesworthlabs/ac3forge`, i.e. a
`homebrew-ac3forge` repo) has none of those requirements — anyone can `brew tap` it and
`brew install` from it immediately — and is the right home for the formula until a
`homebrew-core` submission is worth making on its own merits. See [Homebrew's Acceptable
Formulae criteria](https://docs.brew.sh/Acceptable-Formulae) for the full bar; that PR, if
and when it happens, is a separate decision from staging the formula here.

## What gets packaged

**The formula:** just `ac3cli` — `AC3FORGE_BUILD_CLI=ON`, GUI/tests/examples/fuzzers off, same
reasoning as the vcpkg port ([`packaging/vcpkg-port/iclforge/`](../vcpkg-port/ac3forge/))
staying library-only but pointed the other way: Homebrew formulae are for end-user tools, not
`find_package()`-consumed libraries, so this ships the thing vcpkg deliberately does not.

**The cask:** just `ac3gui.app`, the prebuilt bundle from a tagged release's
`ac3forge-*-Darwin.dmg` (`cmake/Packaging.cmake`). A Cask, not a Formula, is the right shape
for a bundled `.app` — Homebrew formulae build from source, and a Qt6 GUI app is idiomatically
distributed prebuilt and signed (or, here, prebuilt and *not* Apple-signed — see the cask's own
`caveats` block). `Casks/ac3gui.rb` names one release: `version` is its tag and `sha256` is the
digest of its `ac3forge-*-Darwin.dmg` (the digest GitHub reports for that asset). `v0.8.0-beta.2`
was the first tag whose `macos-llvm` CI leg builds `AC3FORGE_BUILD_GUI=ON` (see
[docs/platforms/macos.md](../../docs/platforms/macos.md#gui-on-macos)), so it was the first
`ac3forge-*-Darwin.dmg` that contains `ac3gui.app`.

Each release needs the same bump the sibling Formula gets (the Formula's `url` and `sha256`, the
cask's `version` and `sha256`). [`manifest-bump.yml`](../../.github/workflows/manifest-bump.yml)
makes it after a release publishes: it opens a PR here with the bump and, when
`HOMEBREW_TAP_TOKEN` is set, a PR on the live tap,
[`iainchesworthlabs/homebrew-ac3forge`](https://github.com/iainchesworthlabs/homebrew-ac3forge),
for a person to merge after the local validation below. See
[docs/releasing.md](../../docs/releasing.md#homebrew-formula-and-cask) for the flow.

## Validating locally

From a macOS machine with Homebrew installed:

```bash
brew install --build-from-source ./packaging/homebrew/Formula/ac3forge.rb
brew test ac3forge
brew audit --formula ./packaging/homebrew/Formula/ac3forge.rb
brew uninstall ac3forge
```

`brew audit` catches most style/metadata issues before they'd surface in a tap or a
`homebrew-core` PR review. No CI job runs `brew audit`, `brew install` or `brew test` on either
file, so this validation is manual, macOS-only, and not automated —
see [docs/releasing.md](../../docs/releasing.md#homebrew-formula-and-cask) for the per-release update
flow.

The cask points at a downloadable `.dmg`, so it can be validated the same way, from a macOS
machine with Homebrew installed:

```bash
brew audit --cask ./packaging/homebrew/Casks/ac3gui.rb
brew install --cask ./packaging/homebrew/Casks/ac3gui.rb
brew uninstall --cask ac3gui
```

No CI job runs it, same as the Formula above, so this is manual and macOS-only too. Run it
before merging the bump PR on the tap.
