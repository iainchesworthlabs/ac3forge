# Releasing ICL Forge

How to cut a release: what triggers `.github/workflows/release.yml`, what it produces, what a
release publishes and where, and how to set up the signing keys. Modelled on an earlier
project's release process, with the parts that don't apply to ICL Forge (APT/DNF repository
publishing, a Docker image, a Home Assistant add-on) removed.

ICL Forge was called AC3Forge up to the pre-release 0.10.0-beta.1. The repository, the packages and
the release files have new names, and the packages and release files carry them from the first
release made after the rename. [Renamed](renamed.md) lists which release carries which name, and the
sections below say so where it decides what a step does.

## What a release does today

A tag push, or a dispatch of `release.yml`, starts the workflows below. Each row says what the
workflow publishes and where:

| Workflow | What it does for a `v*` tag | What reaches a registry |
|---|---|---|
| `release.yml` | builds and tests everything `_build.yml` builds (`tier: all`) with packaging on, checks the package list, signs with GPG, writes an SBOM, attests build provenance, creates the GitHub Release from the CHANGELOG section, uploads the assets and redeploys the documentation site | a GitHub Release on this repository, marked a prerelease when the tag has a suffix |
| `manifest-bump.yml`, called by `release.yml` once the release is up | rewrites the four staged packaging manifests to the new tag and opens a pull request on this repository; with `HOMEBREW_TAP_TOKEN` it also opens a pull request on the Homebrew tap | the Homebrew tap, once a person merges its pull request; the pull request on this repository publishes nothing |
| `wheels.yml` | builds the Python wheels; its `publish` job uploads them to PyPI through trusted publishing | PyPI, package `iclforge`; the pre-releases went to the project `ac3forge` |
| `npm.yml` | builds and tests `js/` and packs the tarball; its `publish` job runs only from a manual dispatch on a tag | nothing |
| `esp-component.yml` | packs and verifies the ESP-IDF component; its `publish` job runs only from a manual dispatch on a tag, and needs an `esp-component` environment and token that do not exist | nothing |

What had been published on 2026-10-01, from each registry's own listing and from GitHub. The names are
the ones each artifact was published under; [Renamed](renamed.md#what-a-release-carries) lists
what the first release made after the rename calls them:

- **GitHub Releases:** ten prereleases, `v0.2.0-beta.1` through `v0.10.0-beta.1`
  (`v0.8.0-beta.2` is the second one for 0.8.0). No stable release has been tagged. What each
  carried, counting every asset including signatures and attestations:

    - `v0.2.0-beta.1`, 25 assets: the Windows zip, one Linux set (`.deb`, `.rpm`, `.tar.gz`,
      `.zip`), the macOS `.dmg` and `.zip`, the SPDX SBOM, `SHA512SUMS` and GPG signatures. No
      provenance attestations yet.
    - `v0.3.0-beta.1` and `v0.4.0-beta.1`, 52 each: the `ac3forge-dev-*` library archives, the
      Shield APK and the `.intoto.jsonl` provenance attestations are added.
    - `v0.5.0-beta.1`, 68: the Linux `.deb` and `.rpm` split into `runtime`, `library` and
      `libruntime` packages.
    - `v0.6.0-beta.1` to `v0.9.0-beta.1`, 108 each: the Linux packages, named by architecture,
      for x86_64 and aarch64.
    - `v0.10.0-beta.1`, 127: adds the Windows NSIS installer (`ac3forge-0.10.0-win64.exe`), the
      Windows ARM64 `.exe` and `.zip`, the `ac3gui` AppImage and the conformance vector bundle.
      It has no macOS runtime `.zip` beside the `.dmg`.

  No release has carried a Crucible package.
- **PyPI:** [`ac3forge`](https://pypi.org/project/ac3forge/) 0.9.0b1, uploaded on 2026-08-22, and
  0.10.0b1, uploaded on 2026-09-01. Each has fifteen wheels (CPython 3.10 to 3.14 on Windows
  x64, macOS arm64 and Linux x86-64) and no sdist. The Linux aarch64 and Intel macOS rows were
  added to `wheels.yml` on 2026-09-02, so the next release is the first to carry them. There is no
  project named `iclforge` on PyPI.
- **Homebrew:** the tap
  [`iainchesworthlabs/homebrew-ac3forge`](https://github.com/iainchesworthlabs/homebrew-ac3forge)
  is a public repository. Its `Formula/ac3forge.rb` and `Casks/ac3gui.rb` are both at
  `v0.10.0-beta.1`. The formula was added on 2026-08-18, and the bumps to `v0.8.0-beta.2`,
  `v0.9.0-beta.1` and `v0.10.0-beta.1` are the tap's merged pull requests #1 to #3.
- **vcpkg:** not in the registry. The port was submitted to `microsoft/vcpkg` as
  pull request #53470 on 2026-08-18, under the name `ac3forge`; it is a draft with changes requested
  and has not been updated since 2026-08-19.
- **winget:** not in the registry. The only submission is `microsoft/winget-pkgs` #419594, for
  `0.8.0-beta.1` as `iainchesworthlabs.ac3forge`, opened on 2026-08-18 from the
  `iainchesworthlabs/winget-pkgs` fork. A reviewer
  asked for changes on 2026-09-21, nobody replied, and a bot closed it on 2026-09-29. The
  fork still has its branch. The tree stages `0.8.0-beta.1`, `0.8.0-beta.2`, `0.9.0-beta.1` and
  `0.10.0-beta.1`; the last three have not been submitted. See [winget manifest](#winget-manifest).
- **Conan:** not in ConanCenter. No pull request on `conan-center-index` names the recipe.
- **npm:** nothing. Neither `iclforge-wasm-decoder` nor `iclforge` exists on npmjs.com, and
  `npm.yml`'s `publish` job cannot run from a tag.
- **crates.io:** nothing. The crates under `rust/` (`iclforge` and `iclforge-sys`) have no publish
  step, and neither name exists on crates.io.
- **ESP Component Registry:** nothing. The registry has no component named `iclforge` and no
  `iainchesworthlabs` namespace, and `esp-component.yml`'s `publish` job cannot run from a tag.

A release does not publish an APT or DNF repository, a Docker image or a Home Assistant add-on.
The GitHub environments the repository has are `pypi` and `github-pages`.

## Versioning

ICL Forge derives its version from git tags, the same way aqualink-automate does.
`cmake/GitVersionDerivation.cmake` runs `git describe --tags --match "v*"` **before**
`project()` in the top-level `CMakeLists.txt` and feeds the result straight into
`project(iclforge VERSION ...)` - the tag is the single source of truth. Nothing in the tree
hardcodes a version to bump by hand: not `CMakeLists.txt`, and not the root `vcpkg.json`, which
carries no `"version"` field at all - per vcpkg's own schema that field is only required for a
manifest describing a *library* (a port), and this one just declares this project's own
build-time dependencies (Catch2, optionally Boost/Tracy). The staged port's `version-semver` is
what actually tracks releases - see [vcpkg port](#vcpkg-port) below.

So the order is just:

1. Merge to `main`.
2. Tag.

No version-bump commit, no file to keep in sync, and (since the 2026-08 move to trunk-based
development) no second branch to sync the tag back into either - tagging *is* the release
decision. Before trunk-based development, `main` and `develop` were separate branches and a tag
placed only on `main` was invisible to `git describe --tags` on a `develop` build until a
sync-back PR carried it over (the v0.6.0-beta.1 promotion, #180, missed this and left `develop`
builds reporting a stale version until #192 caught up) - that whole class of gap no longer
exists because there is only one branch to tag.

Tags are strict SemVer 2.0.0: `vMAJOR.MINOR.PATCH[-(alpha|beta|rc).N]`, e.g. `v0.2.0` or
`v0.2.0-beta.1`. A tag with a prerelease suffix (or the dispatch form's `prerelease` checkbox)
marks the GitHub Release as a prerelease. The suffix also flows into the build: CMake's
`project()` `VERSION` field can only hold the bare `X.Y.Z` (that's what `PROJECT_VERSION` and
CPack's package version use), but the full tag - suffix included - is carried separately as
`PROJECT_VERSION_FULL`. It is the `iclforge::version_full` string, the headline of `forge --version`
(`iclforge 0.10.0-beta.1`, with `+N` after it for a build N commits past the tag, so a build from
`main` is not mistaken for the release), and the `generator` field of `forge probe ... json=1`.

A checkout that can't see any `v*` tag (no history, or a shallow CI clone - see `_build.yml`'s
`fetch_depth` input) falls back to version `0.0.0-dev` rather than failing the build. Ordinary
CI legs stay shallow and always show that fallback; only `release.yml`'s tag-triggered or
dispatched build fetches full history (or gets the version stamped directly via
`-DDERIVED_VERSION_OVERRIDE=`) and shows the real one.

## Pre-release checklist

1. **Before tagging**: confirm `main` carries no unexplained open code-scanning alerts.

   ```bash
   gh api "repos/iainchesworthlabs/iclforge/code-scanning/alerts?ref=refs/heads/main&state=open" -q '.[] | [.number, .rule.id, .most_recent_instance.location.path] | @tsv'
   ```

   Empty output - or every remaining line individually understood and either fixed or
   dismissed with a justification - is the bar. Under trunk-based development this is a single
   check against the branch a release is actually cut from (a scheduled run picking up updated
   query packs, or an already-dismissed finding re-minted by a file move, can still add an
   alert between releases even with no separate integration branch in the picture).
   `release.yml`'s `alert-review` job re-checks this (advisory only, default branch) as a
   backstop - it is what caught alerts #83-94 unnoticed on `main` under the old
   `develop`-\>`main` promotion flow, where alerts could accumulate on `develop` invisibly
   until a promotion merge landed them all on `main` at once.

   The analysis engines - CodeQL, MSVC PREfast, clang-tidy and SonarCloud - analyse `main`
   nightly (their crons are 02:17 to 02:35 UTC, and GitHub starts them about six and a half
   hours later, [Self-hosted CI runners](ci-self-hosted-runners.md#nightly-analysis-window)),
   not per pull request, so a release cut before the following night's run has not had that
   day's merges scanned. Either wait for the nightly or dispatch them by hand first
   (`gh workflow run codeql.yml`, `msvc-analysis.yml`, `static-analysis.yml`, `sonarcloud.yml`)
   and let them finish before running the query above. An open `nightly-analysis` issue means a run found something that has not been
   triaged yet - deal with it before tagging.

   The query above and `release.yml`'s `alert-review` job both read Security > Code
   scanning, which SonarCloud does not write to. Check its dashboard separately:
   <https://sonarcloud.io/project/overview?id=iainchesworthlabs_ac3forge>.
2. CI green on `main` for the commit you're about to tag. The `verified` branch points at the
   newest commit a green run of `ci.yml` proved, so `git log verified..main` lists what has merged
   since main was last proven ([After the merge](ci-agentic.md#after-the-merge)); tagging
   `verified` rather than the tip of `main` ties the release to a proven commit. The nightly run
   covers the legs the run after a merge leaves out (the sanitizers, coverage, the ABI gate), and a
   green one also moves `verified-nightly`, which does not exist until a nightly run has passed.
3. Releases must be **cut from main** - `resolve-version` checks this with
   `git merge-base --is-ancestor` and fails otherwise (dry runs are exempt).
4. Decide the tag.
5. **Update [CHANGELOG.md](https://github.com/iainchesworthlabs/iclforge/blob/main/CHANGELOG.md)**
   - move `## [Unreleased]`'s content down to a `## [x.y.z] - YYYY-MM-DD` section matching the
     tag from step 4 without its `v` (`## [0.10.0-beta.1] - 2026-09-01` for `v0.10.0-beta.1`),
     [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format, grouped by
     user-facing area with bold lead-in bullets. This has to happen *before* tagging, not after:
     `github-release`'s "Render release notes from CHANGELOG.md" step
     (`tools/release/render_release_notes.py`, see [Post-release](#post-release)) extracts this
     exact section for the release body the moment the tag is pushed, and hard-fails the release
     if it cannot find one.

## Option A: tag-based release (the normal path)

```bash
git checkout main
git pull origin main
git tag v0.2.0
git push origin v0.2.0
```

Prerelease: `git tag v0.2.0-beta.1 && git push origin v0.2.0-beta.1`. Watch the run under
Actions > Release.

## Option B: manual dispatch

Actions > Release > Run workflow, fill in `version` (e.g. `v0.2.0`), `prerelease`, `dry_run`.
The tag does not exist yet when the run starts; `resolve-version` fails fast if it already does.
The `github-release` job pushes the tag itself, only after build/package/sign/attest have all
succeeded and before it renders the release notes and creates the release - so a dispatch run
that fails in the build, the package check or the signing leaves no tag behind. For a
**prerelease** dispatch, if the tag gets pushed but a later step (the notes, the release, the asset
upload) still fails, `cleanup-failed-prerelease` deletes the orphaned tag automatically; a
non-prerelease tag is left alone even on failure, because deleting a version someone explicitly
declared is a bigger surprise than a maintainer cleaning it up by hand.

## Dry run

Builds and packages every platform without tagging, signing, or publishing anything. Exempt
from the cut-from-main guard, so it can
run from any branch - use it to validate a packaging change before merging.

## Post-release

Most of what used to be a manual post-release checklist here is now automated:

1. **Release notes come from CHANGELOG.md, not `--generate-notes`.** The `github-release` job's
   "Render release notes from CHANGELOG.md" step
   (`tools/release/render_release_notes.py`) extracts the `## [x.y.z] - YYYY-MM-DD` section
   matching the tag being released and uses it as the release body, followed by a
   `**Full Changelog**: …/compare/v<prev>...v<this>` line (omitted for the first release). This
   is why [updating CHANGELOG.md](#pre-release-checklist) moved into the pre-release checklist,
   as its own step: it used to be listed here, as a post-release step done any time before
   finishing the curated release notes by hand - now it is a hard prerequisite of tagging itself,
   since a missing section fails the release outright with a clear error rather than falling back
   to a commit-list draft. [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
   format, grouped by user-facing area with **bold lead-in** bullets, same as always; that
   curation now happens exactly once, in CHANGELOG.md, as part of normal development.

   Two things this deliberately does not reconstruct, left as optional manual polish via
   `gh release edit vX.Y.Z[-beta.N] --notes-file notes.md` afterward if wanted: an `## Artifacts`
   section with per-package checksums (every package already carries its own `*.sha512` file and
   an aggregate `SHA512SUMS` as separate release assets - see [What gets
   published](#what-gets-published) - so restating them in prose is a second place to keep in
   sync, not new information), and a prerelease's `> **Pre-release.**` caveat blockquote (picking
   the single biggest open gap to headline is a judgement call, not an extraction).
2. Verify the release page has all expected artifacts, and that the notes render and read well.
   `github-release` also redeploys the documentation site, which copies the release's sink
   firmware into [the browser installer](hearth/sink-installer.md). Once `docs.yml` has run, the
   installer page names the new release's version.
3. **The four packaging manifests bump themselves.** Once `github-release` has published the
   release and uploaded every asset, the `manifest-bump` job calls
   [`.github/workflows/manifest-bump.yml`](https://github.com/iainchesworthlabs/iclforge/blob/main/.github/workflows/manifest-bump.yml),
   which downloads the release's own source tarball and (where they exist)
   `iclforge-*-Darwin.dmg`/`iclforge-*-win64.zip`, computes the digests each manifest needs,
   cross-checks the two platform-asset digests against the release's published `SHA512SUMS` (the
   source tarball has none to check against - see that workflow's own comments), and opens a PR
   bumping [vcpkg port](#vcpkg-port), [Homebrew formula and cask](#homebrew-formula-and-cask),
   [winget manifest](#winget-manifest) and [Conan recipe](#conan-recipe) together. It also opens
   a pull request on the live `iainchesworthlabs/homebrew-iclforge` tap with the new Formula and
   Cask, if `HOMEBREW_TAP_TOKEN` is provisioned (see below). The tap carries a `main protection`
   ruleset with no bypass actors, so a pull request is the only way into it: an earlier version of
   this workflow pushed straight to the tap's `main` and was refused, and the bumps to
   `v0.8.0-beta.2`, `v0.9.0-beta.1` and `v0.10.0-beta.1` reached the tap as pull requests. A
   person merges the tap's pull request after `brew audit` and `brew test` on a macOS machine,
   since no job in CI runs them.

   Merging that PR is still a separate, reviewed step - this closes the gap between "tagged" and
   "the bump has started", not the whole gap, which is why
   `tools/checks/check_packaging_versions.sh`'s latest-tag advisory (below) stays a warning, not
   a hard gate. What is **not** automated, because it means writing to a repository this project
   does not own: each manifest's own **Every release tag** section below still lists a follow-up
   PR to `microsoft/vcpkg` (once the port is merged upstream), `conan-center-index` (once the
   recipe is merged upstream) or `microsoft/winget-pkgs`, plus Homebrew's local
   `brew audit`/`brew install --build-from-source`/`brew test` validation on a macOS machine. No job
   runs them on the formula or the cask; the macOS legs use Homebrew only to install their own
   build tools (`ccache`, `llvm`, `ffmpeg`, `ninja`).

   **Testing this without cutting a release**: `manifest-bump.yml` is also directly
   `workflow_dispatch`-able (Actions > Manifest Bump > Run workflow), with `dry_run: true` by
   default. Point it at an already-shipped tag to exercise the download/digest/cross-check
   pipeline and see the manifest diffs it would produce, with nothing written, committed, pushed
   or opened - this is release-path automation that otherwise cannot be exercised except by
   shipping a real release. It looks for the release files by their new names (`iclforge-*`), which
   a tag made before the rename does not carry: a dry run against one covers the source tarball and
   the vcpkg port, the formula and the Conan recipe, and its log says that the cask and the winget
   files were skipped.

   **Digests pinned before the rename.** The port, the formula and the Conan recipe pin the digest of
   a source tarball that GitHub generates, and GitHub names the top directory of that tarball after the
   repository. Renaming the repository from `ac3forge` to `iclforge` therefore changes the tarball of
   a tag made before it: the port, the formula and the recipe as staged, and the formula in the live
   tap, fail their checksum for that tag until a bump replaces the digest. The cask and the winget
   manifests name release files, whose bytes do not change.

   **`HOMEBREW_TAP_TOKEN`** (optional, and set on this repository): a fine-grained GitHub PAT
   scoped to `Contents: Read and write` and `Pull requests: Read and write` on
   `iainchesworthlabs/homebrew-iclforge` only (the first pushes the branch, the second opens the
   pull request; the built-in `GITHUB_TOKEN` cannot reach another repository). The workflow names
   the tap by that name, so the tap repository has to be renamed from `homebrew-ac3forge` before the
   first release made after the rename: until then there is nothing at that address to clone. Without
   the token, the
   tap step is skipped (its `if:` gate simply doesn't fire, with nothing logged) - the in-tree PR
   still opens - and the PR body says so. Add it the same way as
   any other repo secret (Settings > Secrets and variables > Actions); nobody but a human with
   access to GitHub's secret store should ever generate or handle it. The in-tree PR itself needs
   no new secret - it opens with the same built-in `GITHUB_TOKEN` every other job here already
   uses - but does need "Allow GitHub Actions to create and approve pull requests" enabled under
   Settings > Actions > General > Workflow permissions, if it is not already.
4. `tools/checks/check_packaging_versions.sh -r .` also carries a latest-tag advisory now (still
   run in the static checks (`_static.yml`) on every pull request and queue entry): a `::warning::`, never a failure,
   per manifest that does not yet match the latest tag - see that script's own header for why
   this stayed a warning rather than becoming a hard gate.

## vcpkg port

A vcpkg port for `iclforge` is staged in-tree at
[`packaging/vcpkg-port/iclforge/`](https://github.com/iainchesworthlabs/iclforge/tree/main/packaging/vcpkg-port/iclforge)
(`vcpkg.json`,
`portfile.cmake`, `usage`). It is not in the curated `microsoft/vcpkg` registry: it was submitted
as pull request #53470 under the name `ac3forge`, which is a draft with changes requested (last
updated 2026-08-19) - see
[docs/library/index.md](library/index.md) for how a consumer uses it either way. It installs the
library only (`iclforge::ac3`, plus `iclforge::matroska`/`iclforge::mp4`/
`iclforge::mpegts` behind their own `matroska`/`mp4`/`mpegts` features, `iclforge::c` behind
`capi` (see the note below), the AC-4 libraries behind `ac4`, `iclforge::iab` behind `iab` and
`iclforge::iamf` behind `iamf` - see `cmake/InstallLibrary.cmake`'s `ICLFORGE_BUILD_<NAME>` and
`ICLFORGE_INSTALL_BOTH_LINKAGES` options), never the CLI/GUI/Hearth/tests/examples/fuzzers.
`iclforge::adm` (the ADM/BW64 reader) and `iclforge::admbridge` have no vcpkg feature
either - they do install/export via `find_package(iclforge)` now (shared-only), but embed
third-party libbw64/libadm and so deliberately carry no vcpkg/Conan feature of their own for
now - see the recipe note further down and [docs/library/index.md](library/index.md).

None of the features is on by default: a curated-registry port's `default-features` may only
cover behaviors, not additional public APIs/targets/binaries (see
[vcpkg's maintainer guide](https://learn.microsoft.com/vcpkg/contributing/maintainer-guide#default-features-should-enable-behaviors-not-apis))
, and each of `matroska`/`mp4`/`mpegts`/`capi`/`ac4`/`iab`/`iamf` is exactly that. Upstream's
own `ICLFORGE_BUILD_<NAME>` options default ON; `vcpkg_check_features()` turns each OFF unless
its feature is asked for. A plain `vcpkg install iclforge` installs the codec only; opt in
explicitly with `vcpkg install iclforge[matroska,mp4,mpegts]`, `iclforge[ac4]` or any subset.
`tools/checks/check_packaging_versions.sh` fails a `default-features` entry, a feature missing
from the Conan recipe's options, a feature and option that switch different
`ICLFORGE_BUILD_<NAME>` options, and an `ICLFORGE_BUILD_<NAME>` option the root `CMakeLists.txt`
defaults ON that a recipe neither offers nor pins OFF. The port pins `ICLFORGE_BUILD_HEARTH` OFF:
Hearth is an application and a library nothing installs, its dependencies are not the port's, and
upstream refuses it beside `ICLFORGE_BUILD_AC4=OFF`, which is what a port without the `ac4`
feature passes. The port also pins several build options a curated-registry review otherwise flags
as uncontrolled: `ICLFORGE_BUILD_ADM`/`ICLFORGE_ENABLE_TRACY` explicitly OFF (already the
project's own default, pinned so a future default change can't silently pull an undeclared
dependency into this port), and `ICLFORGE_WITH_ALSA`/`ICLFORGE_WITH_PIPEWIRE` explicitly OFF -
without that, this library-only build still probes the build machine's ambient ALSA/PipeWire
installs (`src/audio/` is `add_subdirectory()`'d unconditionally outside Emscripten, not gated
on `ICLFORGE_BUILD_CLI`/`ICLFORGE_BUILD_GUI`) even though `iclforge::audio` is never installed or
exported. `vcpkg.json` also declares `"supports": "!(android & !arm64)"` - only `arm64-v8a`
Android is a real target (see [docs/platforms/android.md](platforms/android.md)); other Android
architectures fail to build (`matroska`'s size comparisons assume a 64-bit `size_t`).

`iclforge::c` is exposed as the port's `capi` feature (`vcpkg install
iclforge[capi]`), off by default like the others above. Its `capiTargets` export
used to require the AC-3 library's static variant even when `ICLFORGE_INSTALL_BOTH_LINKAGES=OFF`
left that target unexported - a real bug independent of vcpkg, fixed in
`cmake/InstallLibrary.cmake` by exporting the static variant alongside the shared one in that
branch whenever `ICLFORGE_BUILD_CAPI` is `ON`
(#227) - which is what made adding the feature itself a scope decision rather than
a bug workaround.

Any future optional library component follows the same three-step recipe this repo's own
`ICLFORGE_BUILD_<NAME>` options already establish: add the CMake option and its
`cmake/InstallLibrary.cmake` guard first (that part isn't vcpkg-specific; also add a matching
`iclforge_install_pkgconfig()` call there - see the "pkg-config" section of
[docs/library/index.md](library/index.md), a consumer expects one alongside every installed
component's CMake export), then add a same-named
feature to `packaging/vcpkg-port/iclforge/vcpkg.json` and one line to `portfile.cmake`'s
`vcpkg_check_features()` call, and the same-named option, off by default, to
`packaging/conan/conanfile.py` with its `tc.variables` line (the parity check above fails the
recipes until both have it), and the component to `tools/checks/check_install_consumer.sh`'s
list - unless the component pulls in a real third-party link dependency
of its own, the way `iclforge::adm`/`iclforge::admbridge` do (see
[ADM / BW64 reading](library/adm.md#why-opt-in)): those still install/export (shared-only, to
stay self-contained without re-exporting the third party), but deliberately have no vcpkg/Conan
feature of their own for now.

**Every release tag, once the port has been merged upstream** (#53470 has not been), needs a
follow-up PR to `microsoft/vcpkg` - the curated registry has no mechanism to track a moving `main`, so a new
`iclforge` release is invisible to `vcpkg install` until this happens. Step 1 below is now done
by [`manifest-bump.yml`'s PR](#post-release) rather than by hand; steps 2-3 still
are, since they write to a repository this project does not own:

1. Bump `packaging/vcpkg-port/iclforge/vcpkg.json`'s `version-semver` to the new tag, and
   `portfile.cmake`'s `vcpkg_from_github()` `REF`/`SHA512` to match (`sha512sum` the tag's
   release tarball, or let a first `vcpkg install` attempt report the correct hash).
2. Validate locally first (see below) before touching the upstream fork - a portfile change
   that fails vcpkg's own CI is slower to iterate on there than here.
3. Copy the updated port files into the `microsoft/vcpkg` fork's `ports/iclforge/`, run
   `vcpkg format-manifest ports/iclforge/vcpkg.json` (its formatting is stricter than this
   repo's own JSON style - `vcpkg x-add-version` refuses to run against an unformatted
   manifest) followed by `vcpkg x-add-version iclforge` to regenerate
   `versions/baseline.json`/`versions/a-/iclforge.json` (don't hand-edit these), and open the
   version-bump PR.

**Validating the port locally**, any time `packaging/vcpkg-port/iclforge/` or the CMake options
it drives change (whether or not a release is involved):

```bash
vcpkg install iclforge --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows
vcpkg install iclforge --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows-static
vcpkg install iclforge[matroska,mp4,mpegts,capi,ac4,iab,iamf] --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows
```

`--classic` is required from inside this repo - the root `vcpkg.json` (manifest mode, for this
project's *own* build-time dependencies) would otherwise shadow the package-name argument.
Check for a clean post-build lint (no "not used"/"missing usage" warnings) and that the bare
`iclforge` install excludes every feature's library (`iclforge::matroska`/`iclforge::mp4`/
`iclforge::mpegts`/`iclforge::c`, the AC-4 libraries, `iclforge::iab`, `iclforge::iamf`) - not just
unlinked, no matching files anywhere in the install tree - while
`iclforge[matroska,mp4,mpegts,capi,ac4,iab,iamf]` installs all seven.
`tools/checks/check_install_consumer.sh` makes the same check of any build tree it installs: a
library whose `ICLFORGE_BUILD_<NAME>` option is OFF must leave no file in the prefix.

Fetching a real tag only exercises whatever `ICLFORGE_BUILD_*` options actually existed in that
tagged source - `vcpkg_from_github()`'s `REF` always points at an already-released tag, so a
CMake option added since the last tag (as happened here: `ICLFORGE_BUILD_MP4`/
`ICLFORGE_BUILD_MPEGTS` landed in `develop` after `v0.5.0-beta.1`) can't be exercised through a
real fetch until the *next* tag contains it. To validate a port change against unreleased CMake
options, temporarily swap the `vcpkg_from_github()` block in a scratch copy of `portfile.cmake`
for `set(SOURCE_PATH "<absolute path to this checkout>")`, run the same three commands against
that scratch copy, and discard it once validated - never commit that substitution.

## Publishing to PyPI

The Python bindings (`python/`, see
[docs/library/python-api.md](library/python-api.md)) are the `iclforge` package, with wheels
for Windows (x64), macOS (arm64 and Intel) and Linux (x86_64 and aarch64) built by
`.github/workflows/wheels.yml` via `cibuildwheel`, one wheel per CPython from 3.10 to 3.14. The
releases up to `v0.10.0-beta.1` published them to PyPI as the project `ac3forge`; a release made
after the rename publishes the project `iclforge`. That
workflow's `build` job runs in `ci.yml`'s own `wheels` job, on the `python` lane ([CI lane
partitions](ci-lanes.md)): after a merge that touches `python/` or `examples/python/`, and in the
nightly run, and not on pull requests. It always uploads the wheels it builds as a workflow
artifact.

**Publishing to PyPI is live**: the `pypi` GitHub environment is provisioned and
[`ac3forge`](https://pypi.org/project/ac3forge/) is a published project, with two releases,
0.9.0b1 (2026-08-22) and 0.10.0b1 (2026-09-01). Both carry the same fifteen wheels, CPython
3.10 to 3.14 on Windows x64, macOS arm64 and Linux x86_64, and no sdist: the Linux aarch64 and
Intel macOS rows of the wheel matrix were added on 2026-09-02, after `v0.10.0-beta.1`.
`wheels.yml`'s
`publish` job is gated on both a `v*` tag push and the `pypi` environment, and uses
[PyPI trusted publishing](https://docs.pypi.org/trusted-publishers/) (OIDC) rather than a stored
API token — there is no `PYPI_API_TOKEN` secret to leak in the first place. **Nobody should ever
generate a long-lived PyPI API token and paste it into a chat with an agent or into a GitHub
secret** — trusted publishing exists specifically so that never has to happen.

The one-time setup, for reference (done by a maintainer directly on pypi.org and on GitHub). It
provisioned the project `ac3forge`. The project `iclforge` does not exist on PyPI yet, and a trusted
publisher names its project and its repository, so the first release made after the rename needs
steps 1 and 2 done again for `iclforge`; step 3 is in place already:

1. On PyPI, either publish the very first `iclforge` release by hand (`python -m build python/`
   then `twine upload`, using a temporary scoped token deleted immediately after) to create the
   project, or use PyPI's **pending publisher** mechanism (Your projects → Publishing →
   "Add a pending publisher") to pre-register the trusted publisher for a project name that does
   not exist yet — the second path needs no manual upload at all and is the one to prefer.
2. Either way, register the trusted publisher against this repository: owner
   `iainchesworthlabs`, repository `iclforge`, workflow `wheels.yml`, environment `pypi`.
3. In the GitHub repo, an environment named `pypi` (Settings → Environments) — it exists, and the
   rename keeps it — needs no secrets; its existence and name are what PyPI's trusted-publisher registration keys
   against, and `wheels.yml`'s `publish` job declares `environment: pypi` so the job has somewhere
   to request the OIDC token from. Optionally add required reviewers on the environment for a
   manual approval gate before a publish actually runs.

Pushing a `v*` tag (the same tag that triggers `release.yml`, see
[Option A](#option-a-tag-based-release-the-normal-path) above) triggers `wheels.yml`'s `publish`
job for that tag, which requests an OIDC token against the `pypi` environment and uploads the
built wheels. The upload passes `skip-existing`, so pushing a tag again (to pick up a
release-workflow fix, as `v0.10.0-beta.1` needed) does not fail on files PyPI already has.

## Publishing to npm

The browser decoder package (`js/`, see
[docs/platforms/wasm.md](platforms/wasm.md)) is meant to be the
`iclforge-wasm-decoder` npm package.
Versioning mirrors the PyPI package above rather than reinventing it: `js/package.json` carries a
`0.0.0-dev` placeholder in the tree (the same untagged-build fallback CMake's own
`GitVersionDerivation.cmake` uses), and `npm.yml`'s `publish` job stamps the real,
resolved version (`npm version <version> --no-git-tag-version`) immediately before `npm publish`
— nothing to keep in sync by hand, and the tag is still the single source of truth.

**Publishing to npm is not enabled yet** — unlike PyPI above. `iclforge-wasm-decoder` has never
been published, and two separate things hold it: the one-time setup below has not been done, and
`npm.yml`'s `publish` job is deliberately narrowed to `workflow_dispatch` so that a `v*` tag
cannot create a brand-new public package as a side effect of cutting a release. It uses (like
PyPI) [npm trusted publishing](https://docs.npmjs.com/trusted-publishers)
(OIDC) rather than a stored token — there is no `NPM_TOKEN` secret to leak in the first place.
**Nobody should ever generate a long-lived npm token and paste it into a chat with an agent or
into a GitHub secret** — trusted publishing exists specifically so that never has to happen.

The one-time setup this needs (a maintainer, directly on npmjs.com and on GitHub — not something
an agent should do, the same rule as PyPI's setup above):

1. Publish the very first `iclforge-wasm-decoder` release by hand (`cd js && npm publish` with a
   temporary, scoped token deleted immediately after) to create the project on npmjs.com — npm's
   trusted-publishing setup, unlike PyPI's, needs the package to already exist; there is no
   "pending publisher" pre-registration mechanism for a name that doesn't exist yet.
2. On the package's npmjs.com settings page, add a trusted publisher: provider GitHub Actions,
   organization/user `iainchesworthlabs`, repository `iclforge`, workflow filename
   **`npm.yml`** (the publish job lives there, not in `release.yml` — registering the wrong
   filename is an OIDC authentication failure at publish time, not a warning), environment `npm`.
3. In the GitHub repo, create an environment named `npm` (Settings → Environments) — no secrets
   need adding to it, the same reasoning as the `pypi` environment above. Optionally add required
   reviewers for a manual approval gate before a publish actually runs.
4. Requires npm CLI ≥ 11.5.1 and Node ≥ 22.14.0 for OIDC support — `npm.yml`'s job installs
   `npm@latest` explicitly rather than trusting whatever `actions/setup-node`'s chosen Node
   version happens to bundle.
5. Finally, re-arm the trigger: drop the `github.event_name == 'workflow_dispatch'` clause from
   the `publish` job's `if:` in `npm.yml`. Do this last, and only after a manual dispatch on a
   tag has been seen to publish successfully — until then the job is intentionally inert.

Once all five steps are done, pushing a `v*` tag triggers `npm.yml`'s `publish` job for that tag,
which requests an OIDC token against the `npm` environment and runs `npm publish` from `js/` — no
`--provenance` flag needed, npm attaches provenance attestations automatically for a
trusted-published package. Until step 5, a tag push builds and tests `js/` and stops there.

## Homebrew formula and cask

A Homebrew formula for `forge` is staged in-tree at
[`packaging/homebrew/Formula/iclforge.rb`](https://github.com/iainchesworthlabs/iclforge/blob/main/packaging/homebrew/Formula/iclforge.rb)
and copied to the personal tap
[`iainchesworthlabs/homebrew-iclforge`](https://github.com/iainchesworthlabs/homebrew-iclforge) - see
[`packaging/homebrew/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/packaging/homebrew/README.md)
for why a personal tap rather than a `homebrew-core` submission. Until the tap repository has been
renamed from `homebrew-ac3forge` and a release made after the rename has reached it, the tap holds
the formula `ac3forge` and the cask `ac3gui`, both at `v0.10.0-beta.1`; that release's pull request
replaces them with `iclforge` and adds the `tap_migrations.json` that answers for the old names.
Unlike the vcpkg port, this packages the CLI (`forge`), not the library: `ICLFORGE_BUILD_CLI=ON`
with GUI/tests/examples/fuzzers off, built from the release source tarball.

The GUI (`forge-gui`) is a separate Homebrew Cask,
[`packaging/homebrew/Casks/iclforge.rb`](https://github.com/iainchesworthlabs/iclforge/blob/main/packaging/homebrew/Casks/iclforge.rb)
- a Cask, not a Formula, is the right shape for a bundled, prebuilt `.app` the way `forge-gui.app`
already ships in every platform's release archive (`cmake/Packaging.cmake`'s DragNDrop `.dmg` on
macOS). It's staged the same way the formula is, and pinned to `v0.10.0-beta.1` today (its `version` and
`sha256`, which `manifest-bump.yml` rewrites for each release; see the cask file's own header
comment). Its `url` names the release file the way a release made after the rename does
(`iclforge-<version>-Darwin.dmg`), and `v0.10.0-beta.1` carries it as `ac3forge-0.10.0-Darwin.dmg`, so
the staged cask cannot download the release it names until the first bump after the rename.
`v0.8.0-beta.2` was the first tagged release whose macOS build contained the GUI, as `ac3gui.app` -
`macos-llvm` only started building the GUI at all once
[GUI on macOS](platforms/macos.md#gui-on-macos) landed.

**Every release tag** needs a follow-up update to the formula, same shape as the vcpkg port's.
Steps 1 and 3 are now done by [`manifest-bump.yml`'s PR and tap pull request](#post-release)
rather than by hand - step 2, local `brew` validation, still is, since no job runs `brew` on
the formula or the cask:

1. Bump `packaging/homebrew/Formula/iclforge.rb`'s `url` to the new tag and `sha256` to match
   (`sha256sum` the tag's release tarball - the same tarball the vcpkg port's `SHA512` already
   points at, just a different digest algorithm).
2. Validate locally first (see below) before touching a tap - a formula change that fails
   `brew audit` is slower to iterate on there than here.
3. Open a pull request on the `homebrew-iclforge` tap with the updated formula as
   `Formula/iclforge.rb`; the tap's `main` accepts nothing else.

The same three steps apply to the cask now that it tracks a real release too: bump `version` to
the new tag and `sha256` to the release's `iclforge-*-Darwin.dmg` (`sha256sum` it, or trust
CPack's own published `.dmg.sha512` after converting digest algorithms), validate locally, then
put `packaging/homebrew/Casks/iclforge.rb` into the same tap pull request, as `Casks/iclforge.rb` -
both files ship from the same tap.

**Validating the formula locally**, from a macOS machine with Homebrew installed:

```bash
brew install --build-from-source ./packaging/homebrew/Formula/iclforge.rb
brew test iclforge
brew audit --formula ./packaging/homebrew/Formula/iclforge.rb
brew uninstall iclforge
```

**Validating the cask locally**, the same way, from a macOS machine with Homebrew
installed:

```bash
brew audit --cask ./packaging/homebrew/Casks/iclforge.rb
brew install --cask ./packaging/homebrew/Casks/iclforge.rb
brew uninstall --cask iclforge
```

No job runs `brew audit`, `brew install` or `brew test` on the formula or the cask (the macOS
legs use Homebrew only for their own build tools), so this validation is manual, and nothing checks
the tap's pull request for you - unlike the vcpkg `--overlay-ports` flow above.

## winget manifest

A winget manifest for `iclforge` (`forge` and `forge-gui` together) is staged in-tree at
[`packaging/winget/manifests/`](https://github.com/iainchesworthlabs/iclforge/tree/main/packaging/winget/manifests),
at the exact `manifests/<first-letter>/<publisher>/<package>/<version>/` path a
`microsoft/winget-pkgs` submission uses, so the version directory can be copied straight into a
fork of that repo. The package identifier is `iainchesworthlabs.iclforge` for a release made after the
rename, and the four versions staged so far, up to `0.10.0-beta.1`, keep `iainchesworthlabs.ac3forge`
and the directory `ac3forge/` as they were made: a new identifier is a new package upstream, and a
staged version directory is never rewritten. It is not in the winget registry. The one submission, for
`0.8.0-beta.1` (`microsoft/winget-pkgs` #419594, opened on 2026-08-18 from the
`iainchesworthlabs/winget-pkgs` fork), raised a Windows Defender error in its first validation run, which cleared when a
moderator re-ran the validation on 2026-08-24, and then passed. On 2026-09-21 a reviewer asked
for changes: the manifest is missing dependencies it should declare, and the review attaches two
screenshots. Nobody replied, and a bot closed the pull request on 2026-09-29, three days after a
stale warning. The fork still has the branch. The tree stages `0.8.0-beta.1`, `0.8.0-beta.2`,
`0.9.0-beta.1` and `0.10.0-beta.1`, and the last three have not been submitted.

The `windows-msvc` leg of `.github/workflows/_ci-windows.yml` installs `makensis` via
Chocolatey and `cpack` produces a real NSIS `.exe` installer in every run of `ci.yml` that
builds that leg - the leg fails outright if it doesn't (see `cmake/Packaging.cmake`'s
`find_program(makensis)` gate and the "Assert the NSIS installer was produced" step). From
`v0.10.0-beta.1` on, a release carries that installer, and the manifest should use
`InstallerType: nullsoft` against that release's `iclforge-X.Y.Z-win64.exe`, dropping
`NestedInstallerType`/`NestedInstallerFiles` entirely - a real installer replaces the
nested-portable-zip shape, it doesn't add to it.

Every version directory for a release cut **before** the installer existed (`0.9.0-beta.1` and
earlier) legitimately keeps `InstallerType: zip` with `NestedInstallerType: portable` against that
release's `win64.zip`: those releases really did ship without an NSIS `.exe` (`makensis` wasn't
on the runner yet), and a staged manifest must describe what a release actually shipped, not what
a later fix made possible. Never rewrite an earlier release's version directory to claim an
installer that release never produced. `v0.10.0-beta.1` is the first release that carries the
installer (`ac3forge-0.10.0-win64.exe`), and its staged directory is still the zip shape, because
`manifest-bump.yml` rendered it that way and the conversion below has not been done for any
release.

**Every release tag** needs a new version directory, since winget-pkgs versions each release
independently rather than tracking a moving tag the way vcpkg's `version-semver` does. Step 1
is now done by [`manifest-bump.yml`'s PR](#post-release), which renders all three
files fresh from a template rather than copying the previous version directory - but it renders
the zip shape (`InstallerType: zip` with `NestedInstallerType: portable`, digested against
the release's `win64.zip`; `tools/release/bump_manifests.py` never downloads the `win64.exe` at
all), so step 2's nullsoft conversion, step 3's local `winget validate`, and step 4's fork PR
all still need a human with the `winget` CLI:

1. Make `packaging/winget/manifests/i/iainchesworthlabs/iclforge/<new-version>/` with the three files
   (`bump_manifests.py` renders them from a template). A directory made before the rename belongs to
   `ac3forge` and is not a base to copy: its package identifier is another package's.
2. Update the installer manifest to `InstallerType: nullsoft`, its `InstallerUrl` to the new
   release's `win64.exe` and `InstallerSha256` to match (`sha256sum` the `.exe` - winget wants
   SHA256, unlike the `SHA512SUMS` `release.yml` publishes for every artifact, see [What gets
   published](#what-gets-published) below), and remove `NestedInstallerType`/
   `NestedInstallerFiles`.
3. Validate locally first (see below) before touching a fork.
4. Copy the new version directory into the `microsoft/winget-pkgs` fork at the matching
   `manifests/i/iainchesworthlabs/iclforge/<new-version>/` path and open the submission PR.

The binaries inside that `.exe` are unsigned: the project has no code-signing certificate yet.
The Defender error on the `0.8.0-beta.1` submission cleared on a re-run, so signing is not what
stopped it, but an unsigned installer can still draw a SmartScreen warning from a user. What
stopped it was the reviewer's request for dependencies, and none of the staged manifests has a
`Dependencies:` block, so a resubmission starts by finding out which ones the reviewer meant.

**Validating the manifest locally**, with the `winget` CLI (ships with Windows 10/11):

```bash
winget validate --manifest packaging/winget/manifests/i/iainchesworthlabs/iclforge/<version>
```

For a version up to `0.10.0-beta.1` the directory is `ac3forge` in place of `iclforge`.

## Conan recipe

A Conan (2.x) recipe for `iclforge` is staged in-tree at
[`packaging/conan/`](https://github.com/iainchesworthlabs/iclforge/tree/main/packaging/conan)
(`conanfile.py`, `conandata.yml`, `test_package/`) and has not been submitted to ConanCenter
(`conan-center-index`), where no pull request names it. Scoped the same as the vcpkg port - the library only (`iclforge::ac3`,
plus `iclforge::matroska`/`iclforge::mp4`/`iclforge::mpegts` behind their own default-on `matroska`/
`mp4`/`mpegts` options, and `iclforge::c`, the AC-4 libraries, `iclforge::iab` and
`iclforge::iamf` behind default-off `capi`/`ac4`/`iab`/`iamf` options), never the
CLI/GUI/Hearth/tests/examples/fuzzers - with one Conan option per `ICLFORGE_BUILD_<NAME>` CMake option,
the same pattern the vcpkg port's `vcpkg_check_features()` call already establishes, and the same
options as the port's features, which `tools/checks/check_packaging_versions.sh` checks. The
two differ in one default: the three container writers are on by default here, as they have been
since the recipe was written, and off in the port since its curated-registry review. Rather than asking Conan's `CMakeDeps` generator to synthesise a
second CMake package config, the recipe sets `cmake_find_mode` to `"none"` and points consumers
at the config `cmake/InstallLibrary.cmake` already installs - see `conanfile.py`'s
`package_info()` comment. A consumer's `find_package(iclforge CONFIG REQUIRED)` and
`target_link_libraries(main PRIVATE iclforge::ac3)` calls are identical to the vcpkg or plain
`cmake --install` case (see [docs/library/index.md](library/index.md)), Conan or not.

**Every release tag**, once the recipe has been merged upstream, needs a follow-up PR to
`conan-center-index` - ConanCenter has no mechanism to track a moving `main` either, same as
vcpkg's curated registry. Step 1 is now done by [`manifest-bump.yml`'s PR](#post-release)
rather than by hand; steps 2-3 still are:

1. Add a new entry to `packaging/conan/conandata.yml`'s `sources` map, keyed by the new
   version, with the tag's release tarball `url` and `sha256` (same tarball the vcpkg port's
   `SHA512` and the Homebrew formula's `sha256` already point at, just fetched fresh).
2. Validate locally first (see below) before touching the upstream fork.
3. Copy the updated recipe into the `conan-center-index` fork's `recipes/iclforge/`, add the
   new version to that recipe's own `config.yml`, and open the version-bump PR.

**Validating the recipe locally**, any time `packaging/conan/` or the CMake options it drives
change (whether or not a release is involved):

```bash
conan create packaging/conan --version <version> -s compiler.cppstd=23
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:shared=True"
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:matroska=False" -o "&:mp4=False" -o "&:mpegts=False"
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:capi=True"
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:ac4=True" -o "&:iab=True" -o "&:iamf=True"
```

`-s compiler.cppstd=23` is required - a bare default profile's `compiler.cppstd` predates
C++23 on most Conan installs, and `check_min_cppstd(self, 23)` in `conanfile.py` fails fast
rather than configuring a build that would fail deep inside compilation instead. Each command
above builds `test_package/`, which links `iclforge::ac3` and runs it, exercising the exact
`find_package(iclforge)` path a real consumer uses - a passing `conan create` is a stronger
signal than a configure-only check for that reason. Fetching a real tag only exercises whatever
`ICLFORGE_BUILD_*` options actually existed in that tagged source, same caveat as the vcpkg
port's local-source-override technique above - `packaging/conan/conandata.yml` would need the
same scratch-entry treatment (a local `url` pointing at this checkout instead of a GitHub
tarball) to validate a CMake option added since the last tag.

## What gets published

The file names below are the ones a release made after the rename carries (`iclforge-...`, and
`forge-gui-...` for the AppImage). The releases up to `v0.10.0-beta.1` carry `ac3forge-...` and
`ac3gui-...`, as [Renamed](renamed.md#what-a-release-carries) lists.

One package per OS **and architecture**, not one per compiler-toolchain leg: `_build.yml`'s matrix
(the legs in `.github/ci/legs.jsonc`) builds and tests both Windows toolchains (MSVC, clang-cl),
both Linux toolchains (GCC, Clang) - on both x64 and arm64 - and both macOS
architectures (arm64 and x86_64), all of them in a release run. For Windows and Linux, only the
leg marked `release_package: true` per OS/arch actually packages for a release - windows-msvc,
windows-msvc-arm64, linux-gcc and linux-gcc-arm64. windows-llvm and linux-llvm still catch
compiler-specific bugs in full in every run after a merge, and linux-llvm-arm64 in the nightly run;
they just don't produce a second, redundantly canonical archive that a downloader would have no way
to choose between. `cmake/Packaging.cmake` arch-qualifies the Linux archive filename
(`iclforge-X.Y.Z-Linux-x86_64.tar.gz` vs. `...-Linux-aarch64.tar.gz`) specifically so the two
Linux architectures' TGZ/ZIP downloads never collide; DEB/RPM already carry their arch in their
own filenames. The two linux-llvm legs are the one exception to all of that, and only for
Crucible: they are the legs that build against PipeWire, which is the only backend
Crucible accepts, so they are the only legs that can package that component - one per Linux
architecture - and those packages do reach the release, on the artifact glob rather than on a
`release_package` gate (the last three rows below, and the Linux paragraph after them).

macOS doesn't fit the "one `release_package` leg" shape at all: neither `macos-llvm` (arm64) nor
`macos-llvm-x64` (x86_64, on GitHub's native-Intel `macos-15-intel` runner - real hardware, not
Rosetta) carries `release_package`. A separate `package-macos-universal` job instead
`cmake --install`s each leg's `runtime` component, `lipo -create`s every Mach-O file the two trees
have in common (`forge`, `forge-gui`, and every dylib/framework binary
`qt_generate_deploy_qml_app_script` copies into `forge-gui.app/Contents/Frameworks/`), and packages the
merged tree with `hdiutil` directly - the same call CPack's own DragNDrop generator makes under the
hood. So there is still exactly one macOS end-user package per release, just built from two legs'
output rather than one leg's own `cpack` run - which is also why it ships as a `.dmg` only, not the
`.zip` a single-arch leg's own `cpack --preset pack-macos-llvm` also produces alongside its `.dmg`:
nothing merges a second, redundant plain-archive form of the same universal binary today. The
matching `iclforge-dev-*` library archive is attempted the same way (`library`/`libruntime`
components instead of `runtime`) but is best-effort - see `package-macos-universal`'s own comment in
`_build.yml` - so it may be missing from a given release; check that job's log if it's absent.

What a release carries, and which leg builds it. The first five rows are the library and Forge's
`forge`/`forge-gui` and the sixth is the Shield app; the last three are Crucible's own
component, a separate download on both platforms that have one, and on both Linux
architectures. The `.AppImage` and the conformance-vector bundle are not CPack products and are
described after the table.

| Platform | Arch | Leg | End-user packages | Library (`iclforge-dev-*`) |
|---|---|---|---|---|
| Windows | x64 | windows-msvc | `.zip`, `.exe` (NSIS) | `.zip` |
| Windows | arm64 | windows-msvc-arm64 | `.zip`, `.exe` (NSIS) - `forge` only, and the leg is still `experimental: true`; both are explained below | `.zip` |
| Linux | x86_64 | linux-gcc | `.tar.gz`, `.zip`, `.deb`, `.rpm` | `.tar.gz` and `.zip`, plus real system packages: `libiclforge0`/`iclforge-devel` (RPM) and `libiclforge0`/`libiclforge-dev` (DEB) |
| Linux | aarch64 (Raspberry Pi 4/5 and other arm64 targets) | linux-gcc-arm64 | `.tar.gz`, `.zip`, `.deb`, `.rpm` | same split as x86_64, above |
| macOS | arm64 + x86_64 (universal) | macos-llvm + macos-llvm-x64, merged by `package-macos-universal` | `.dmg` | `.zip`, best-effort (see above) |
| Android (Shield) | arm64 (NDK) | build-android | `.apk` | none - Shield links `iclforge::ac3`/`iclforge::audio` in-tree, it isn't a `find_package(iclforge)` consumer |
| Crucible, Windows | x64 | windows-msvc | `iclforge-crucible-*-win64.zip` | none - the `crucible` component carries no headers or CMake config |
| Crucible, Linux | x86_64 | linux-llvm | `iclforge-crucible-*-Linux-x86_64.tar.gz`, `iclforge-crucible_*_amd64.deb` | none, same reason |
| Crucible, Linux | aarch64 | linux-llvm-arm64 | `iclforge-crucible-*-Linux-aarch64.tar.gz`, `iclforge-crucible_*_arm64.deb` | none, same reason |

Windows x64 additionally ships Crucible as its own
`iclforge-crucible-*-win64.zip` ([the Crucible guide](crucible/index.md)): the
`crucible` CPack component - `crucible.exe`, the `crucible-run` runner, the driver's
install/remove scripts, a Qt runtime of its own, and `NOTICES.txt` beside `LICENSE.txt` at the
archive root (the third-party notices, generated per platform from `apps/crucible/notices/` at
configure time) - packaged by the same `windows-msvc` leg as the first row, uploaded inside that
leg's own `packages-windows-msvc` artifact and attached to the release with everything else in it.
It is a separate download rather than part of the `runtime` component, and deliberately absent
from the NSIS installer (`cmake/CPackProjectConfig.cmake` says why): its null-sink driver is
test-signed only, so the application needs a machine with test signing on to be useful, which is
not something a `forge` download should carry. When the EV certificate lands, the installer
takes over installing the application and its signed driver - one line in that file, and this
paragraph, change together. `tools/ci/check_crucible_package.py` guards the archive's shape in
CI and against a local `cpack`.

Linux ships the same component as `iclforge-crucible-*-Linux-x86_64.tar.gz` and the
`iclforge-crucible` `.deb` beside it (named the way `dpkg` names things,
`iclforge-crucible_<version>_amd64.deb`), and again as the `-Linux-aarch64.tar.gz` and
`_arm64.deb` pair the arm64 leg builds. Those and no `.rpm`: the CI pass runs
`cpack -G "TGZ;DEB"`, and the RPM settings `cmake/Packaging.cmake` carries for the component are
there for a local `cpack` on a machine with `rpmbuild`. The archives hold `crucible`,
`crucible-run`, the freedesktop launcher, the AppStream record and its icons in the hicolor
theme, and under `share/doc/iclforge-crucible/` the notices (`NOTICES.txt`, once more as the
`copyright` file a `.deb` is expected to carry) with `LICENSE.txt` - and nothing else: no Qt
(the system's own loader finds it), and no driver scripts, because Linux needs no driver. The `.deb` depends on `pipewire` and a session manager
(`wireplumber | pipewire-media-session`) explicitly, since those are running services rather
than libraries shlibdeps could see; everything else it depends on is resolved from the binary.
Two things to know. The component is packaged only from a PipeWire build, which is the only
build Crucible accepts on Linux ([why](crucible/design/promotion.md#alsa-or-pipewire)), so the
Linux release legs - which build ALSA - do not produce it. The Crucible pass runs on both Linux
LLVM legs instead - x86_64 on linux-llvm, aarch64 on linux-llvm-arm64 since 2026-09-06 - and each
builds, packages and uploads its own pair as `packages-crucible-<preset>`, which is the
`packages-*` pattern `release.yml` downloads and then attaches file by file, so all four files
are release assets, checksummed, GPG-signed, SBOM'd and attested with every other package. Its
configure step passes the same `DERIVED_VERSION_OVERRIDE` every other release-bound build does,
or it would stamp the previous release's version onto them. One qualification comes with that
route: neither leg carries `release_package`, so the packages ride on the artifact glob rather
than on a release gate. On x86_64 that has a visible consequence - when the pass finds a Qt older
than 6.8 it skips the window and its package with a warning, the upload step then finds nothing
to attach, and `if-no-files-found: ignore` lets the release publish without it rather than fail,
which is why the completeness check deliberately does not require it. The arm64 leg does not
skip: a step beside the pass fails that leg when the window is missing, and reads the
architecture off the binary and the `.deb` rather than off their filenames. Any other failure in
that pass does block the release - neither linux-llvm leg is experimental, so they fail the run
like any other. No tag has been cut since that wiring landed, so this is what the workflow is
configured to do rather than a route a published release has been seen to take; a `release.yml`
dry run is what would exercise it before a tag does. And the `.deb`'s one-line synopsis is the
library's, not Crucible's: CPack's DEB generator headlines every component's package with the
project summary and offers no per-component override that takes effect, so
`apt show iclforge-crucible` opens with "Clean-room AC-3 encoder" and says what the package
actually is on the next line. The same check script reads the tarball's layout, and refuses one
that carries a PowerShell script; on both platforms it also reads `NOTICES.txt` and refuses a
notices file written for the other platform, or one whose Quick 3D section disagrees with what
the archive ships.

Linux x86_64 also ships a self-contained `forge-gui` `.AppImage`, built by its own
`linux-appimage` job rather than a `release_package: true` leg above - it isn't a CPack product
at all, so it sits outside this table's "one canonical leg per OS/arch" framing, but it is built
in the nightly run and lands in every real release alongside the row above. See [docs/platforms/linux.md](platforms/linux.md#appimage) for why it
exists and how it's built.

The end-user packages are `forge`/`forge-gui` (CPack's `runtime` component) on desktop, or the
Shield app's `.apk` on Android. The library packages are a second, independent download for a
third party consuming `iclforge::ac3`/`iclforge::matroska` via `find_package(iclforge)` (see
[docs/library/index.md](library/index.md)) - headers, static and shared libraries, and the
CMake package config, but neither `forge`/`forge-gui` nor `iclforge::audio` (live capture/monitor/
passthrough stays a CLI/GUI-internal detail, not part of what's installed here).

Archive downloads (ZIP/TGZ) bundle everything above into one `iclforge-dev-*` file, one per
platform regardless of compiler leg, same reasoning as the end-user package above - not NSIS (a
component installer can't also produce a second standalone download), not DragNDrop (no macOS
host to build or verify it against at all).

Linux additionally gets a **real runtime/`-dev` split** as proper system packages, not just an
archive: `cmake/InstallLibrary.cmake` files the shared libraries' versioned `.so` under its own
CPack component (`libruntime`, split out via `NAMELINK_COMPONENT` - see that file's comment),
separate from headers/static-archives/CMake-config/namelink-symlink (`library`). DEB/RPM's own
component-install switches turn that into three independent packages - `iclforge` (the CLI/GUI),
`libiclforge0` (just the `.so` a linked binary loads), and `libiclforge-dev`/`iclforge-devel`
(everything a builder needs, version-pinned to depend on the exact matching `libiclforge0`) -
the same `libFOO`/`libFOO-dev` shape as any other Linux C library, installable with a plain
`apt install`/`dnf install` rather than a manual archive download. ZIP/TGZ still produce one
merged `iclforge-dev-*` archive as before (`cmake/Packaging.cmake` groups `library`+`libruntime`
together for the archive generators; `cmake/CPackProjectConfig.cmake` overrides that back apart
for DEB/RPM specifically). Confirmed against real `dpkg-deb -c`/`-I` and `rpm -qlp`/`-qRp` output
in a Linux build, not just a CMake reading - the pre-split `.deb` was, on inspection, a single
package silently bundling `forge` together with the *entire* SDK (headers, static archives, and
the CMake package config all thrown in beside the binary), which this split also fixes as a
side effect.

The Shield `.apk` is signed with a real release keystore when one is provisioned (see
"Provisioning the Android release keystore" below; the four `ANDROID_KEY*` secrets are set on
this repository), and falls back to AGP's default debug keystore cleanly if it isn't - either way
it's fine for sideloading onto a Shield in developer mode. A release keystore is a prerequisite before this could ever go through the Play Store,
which sideloading itself doesn't require. (Not to be confused with **object signing** - the EMDF
Atmos authenticity tag, provisioned separately via the `ATMOS_SIGNING_KEY` secret and
unrelated to APK code-signing; see "Provisioning the Android object-signing key" below.)

Alongside the packages, one artifact that is not a build of anything:
**`iclforge-conformance-vectors-<version>.tar.gz`**, the published conformance vector set:
60 AC-3 / E-AC-3 / Atmos streams covering each coding tool, layout and sample
rate the encoder can emit, with the source PCM each was encoded from, the expected decode hashes
and a manifest of what each exercises. `_build.yml`'s linux-gcc leg builds it, from
`tools/generators/gen_conformance_vectors.py`; the release call additionally sets
`publish_conformance_vectors`, which regenerates the whole bundle a second time and fails the leg
if a single hash moved. It lands in `release-artifacts/` after the "at least one package was
built" check - deliberately, since it is a `.tar.gz` and would otherwise satisfy that check on
its own - and from there it is signed, checksummed, SBOM'd and attested exactly like a package.
See [Conformance vectors](conformance-vectors.md) for what is in it and how a decoder implementer
uses it.

And **the Hearth sink firmware** ([planning/esp32-ota.md](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-ota.md#published-images),
O8). No tag has been cut since this job landed (`v0.10.0-beta.1` has no `hearth-sink-*` assets), so
this is what the workflow is configured to publish rather than a route a published release has
been seen to take. `_build.yml`'s `package-esp32-firmware` job gathers the four board images `build-esp32s3`
and `build-esp32c3` build: `hearth-sink-esp32s3`, `hearth-sink-esp32c6` (4 MB table),
`hearth-sink-esp32c6-16mb` and `hearth-sink-esp32p4-rev1`. Each is published as four files:

- `<image>-<version>.bin`, the app image, for an update over the network;
- `-factory.bin`, the whole flash from `0x0`, for a new board (it overwrites NVS);
- `-parts.zip`, the same pieces with a `flash_args` of relative paths, which move a board already
  in use to the layout with its NVS kept;
- `-elf.zip`, for a backtrace or a core dump.

One `hearth-sink-manifest.json` describes them all, and `tools/hearth/ota.py push --release`
reads it to give each board its image. `tools/ci/check_firmware_package.py` holds each image to
its name before anything is uploaded: its chip, revisions, flash size and PSRAM, its own
SHA-256, the smallest slot of its table, its parts against its factory image, and no network
built in. On a release each image carries the tag as its version (`PROJECT_VER`). The files are
checksummed, GPG-signed and attested with everything else; the images themselves are not yet
signed with a key the boards check (O7). Every CI run keeps the same set for 14 days as the
`esp32-firmware` artifact, which `ota.py push --run <run id>` takes.

One leg is still `experimental: true`, `windows-msvc-arm64` on its own runner label
(`.github/ci/legs.jsonc` says why), and it carries `release_package: true` as well, so a
release run packages it and its files are collected with the rest. `v0.10.0-beta.1` carried its
`.exe`, `.zip` and library archive. What it packages is `forge` and not `forge-gui`:
`CMakePresets.json`'s `windows-msvc-arm64` preset leaves `ICLFORGE_BUILD_GUI` off, because when the
leg was written Qt's only Windows arm64 kit was a cross-compile kit expecting a paired x64
install for its host tools - that preset's own description says the rest. What
`experimental` costs is the leg's own failure signal: `_build.yml` runs it under
`continue-on-error`, so it can die and still report green to the reusable workflow. That is why
`release.yml`'s "Verify every documented package was built" step names each package this section
promises and fails on whatever is absent, rather than counting files - a release cannot quietly
go out missing this platform. That line is still on the leg, and comes off only once a run has
gone green. The other four package for real rather than best-effort - a packaging failure on any
of them blocks the release the same as a build or test failure would. Every package - end-user or
library - gets a `.sha512` (`CPACK_PACKAGE_CHECKSUM` in `cmake/Packaging.cmake`), and the release
carries an aggregate `SHA512SUMS` manifest, keyless Sigstore/OIDC build provenance (a
`.intoto.jsonl` beside each file, also attested through GitHub's attestation API) and an SPDX SBOM
(`iclforge-<version>.spdx.json`). With the GPG key provisioned, each file also has a detached
`.asc` signature, `SHA512SUMS.asc` signs the manifest, and the public key is attached as
`iclforge-signing-key.asc`.

## Provisioning the GPG signing key (optional, one-time)

The release workflow checks whether `REPO_GPG_PRIVATE_KEY` is set and skips the signing steps
cleanly if it isn't. Both `REPO_GPG_PRIVATE_KEY` and `REPO_GPG_PASSPHRASE` are set on this
repository, and the assets of `v0.10.0-beta.1` carry the `.asc` signatures. The `SHA512SUMS` manifest itself is
generated either way, unconditionally, in its own step ahead of the GPG-gated one - GPG only adds
a detached signature over it and over each artifact; `manifest-bump.yml` depends on
`SHA512SUMS` existing for every release, signed or not. **Nobody should ever paste a private key
into chat with an agent, or ask one to generate/handle key material** - do this yourself,
locally:

```bash
# 1. Generate a signing-only key (no passphrase keeps CI simplest - see the
#    tradeoff note below before deciding that's right for you).
gpg --batch --quick-generate-key "iclforge <you@example.com>" rsa4096 sign never

# 2. Export the private key.
gpg --armor --export-secret-keys "iclforge" > iclforge-signing-key-private.asc
```

3. In the GitHub repo, go to Settings > Secrets and variables > Actions, and add:
   - `REPO_GPG_PRIVATE_KEY` - the full contents of `iclforge-signing-key-private.asc`.
   - `REPO_GPG_PASSPHRASE` - only if you gave the key a passphrase in step 1.
4. Delete the local `iclforge-signing-key-private.asc` file.

**The no-passphrase tradeoff**: a passphrase-less key is simpler to automate (no
`REPO_GPG_PASSPHRASE` secret, no interactive unlock to script around) but weaker if GitHub's
secret store is ever compromised - decide deliberately rather than defaulting to it. There is no
key-rotation procedure documented here; if you want one, design it before you need it, not
during an incident.

## Provisioning the Android release keystore (optional, one-time)

Off by default the same way GPG signing is - `_build.yml`'s `build-android` job checks whether
`ANDROID_KEYSTORE_BASE64` and its three companion secrets are set, and falls back to the debug
keystore cleanly if they aren't (see `build.gradle.kts`'s `releaseSigningAvailable`). **Nobody
should ever paste a private key into chat with an agent, or ask one to generate/handle key
material** - do this yourself, locally:

```bash
# 1. Generate a release keystore. Requires a JDK (keytool ships with any
#    JDK - `java -version` to check; CI uses Temurin 17, matching that isn't
#    required but keeps things consistent). Leave -storepass/-keypass off so
#    it prompts interactively - keeps the passwords out of shell history and
#    the process list. The -dname prompts (name/org/etc) only populate the
#    certificate's subject line, not security-relevant - answer them however
#    you like. PKCS12 (the modern default) uses one password for both the
#    keystore and the key - there is no separate key password to set.
keytool -genkeypair -v \
  -keystore iclforge-shield-release.keystore \
  -storetype PKCS12 \
  -alias iclforge-shield \
  -keyalg RSA -keysize 4096 \
  -validity 10000

# 2. Back up the keystore file itself right now, before doing anything else -
#    e.g. into a password manager's secure file storage, or an encrypted
#    offline drive. Unlike a GPG key, there is no separate keyring backing
#    this file up - it IS the private key, with no other copy anywhere.
#    Losing it means never being able to sign an update to this app under
#    the same identity again.

# 3. Base64-encode it into one line, ready to paste into a GitHub secret
#    (GitHub Actions secrets are text; this is the standard way to carry a
#    binary keystore through one).
base64 -w0 iclforge-shield-release.keystore > iclforge-shield-release.keystore.b64
```

4. In the GitHub repo, go to Settings > Secrets and variables > Actions, and add:
   - `ANDROID_KEYSTORE_BASE64` - the full contents of `iclforge-shield-release.keystore.b64`.
   - `ANDROID_KEYSTORE_PASSWORD` - the password from step 1.
   - `ANDROID_KEY_ALIAS` - `iclforge-shield` (or whatever `-alias` you used).
   - `ANDROID_KEY_PASSWORD` - the same password as `ANDROID_KEYSTORE_PASSWORD` (PKCS12 doesn't
     support a different one - see step 1).
5. Delete the local `iclforge-shield-release.keystore.b64` file. **Keep the `.keystore` file
   itself** - see step 2.

No key-rotation procedure is documented here for the same reason as the GPG key above - design
one before an incident forces the question, not during it.

## Provisioning the Android object-signing key (optional, one-time)

Separate from the APK keystore above: this is the EMDF Atmos authenticity key that lets a
validating decoder reconstruct the objects (see [Object signing](concepts/object-signing.md)). Off
by default - `build-android` checks whether `ATMOS_SIGNING_KEY` is set and, if not, writes
no key asset, so the app ships the safe unsigned bed51 stream. **The key is yours to provision; do
not paste key material into a chat with an agent - do this yourself, locally.**

```bash
# Base64-encode your 32-byte key file into one line, ready to paste into a
# GitHub secret. CI writes this base64 verbatim into the app's bundled
# signing.key asset; the app base64-decodes it at startup (the same
# decode_signing_key() the desktop CLI uses, which also accepts a raw key).
base64 -w0 atmos.key > atmos.key.b64
```

Then, in the GitHub repo, go to Settings > Secrets and variables > Actions and add
`ATMOS_SIGNING_KEY` - the full contents of `atmos.key.b64` - and delete the local
`atmos.key.b64` afterward.

!!! danger "An APK carrying the `signing.key` asset *is* the key"
    Because the app signs on-device, any Shield build made **with the asset present** bundles the
    key and is therefore key material: it must never be distributed. In CI that is the debug APK
    only — a smoke test that never leaves the runner. `build-android` deletes the asset before any
    release step and then asserts the staged `.apk` contains no `signing.key` entry, so the
    published release asset is always the unsigned `bed51` app. Locally, a build with your own
    `signing.key` dropped in is as sensitive as the key itself: sideload it to your own Shield and
    nothing else.

    Treat `ATMOS_SIGNING_KEY` as rotatable: regenerate it and re-set the secret whenever you have
    any reason to think a build carrying the asset left a machine you control.

## Verifying a download

```bash
# Provenance (keyless, ties the bytes to this exact repo/workflow/commit), for a release made
# after the rename
gh attestation verify iclforge-<version>-win64.zip --repo iainchesworthlabs/iclforge
# The releases up to v0.10.0-beta.1 were attested while the repository was named ac3forge, so
# they are verified by owner
gh attestation verify ac3forge-0.2.0-win64.zip --owner iainchesworthlabs

# GPG (ties the bytes to the maintainer's key). The public key is the release's
# iclforge-signing-key.asc, or ac3forge-signing-key.asc up to v0.10.0-beta.1
gpg --import iclforge-signing-key.asc
gpg --verify SHA512SUMS.asc SHA512SUMS && sha512sum -c SHA512SUMS
gpg --verify iclforge-<version>-win64.zip.asc iclforge-<version>-win64.zip
```

## Troubleshooting

**"... is not on main - releases must be cut from main"** - the commit you tagged (or the ref
you dispatched from) hasn't been merged to `main` yet.

**"tag vX.Y.Z already exists"** (manual dispatch only) - either retry with a different version,
or delete the existing tag first if it was created in error:
`git push origin :refs/tags/vX.Y.Z && git tag -d vX.Y.Z`.

**"this release is missing packages it is documented to publish"** - the `github-release` job's
completeness check found that a package listed under
[What gets published](#what-gets-published) never arrived, and stopped before the release was
created. Its log names each missing one and lists everything that did arrive. Start at the
`build-packages` run: the usual cause is that leg failing, and on `windows-msvc-arm64` that
failure does not turn the job red by itself (`experimental: true`, so `continue-on-error`) - the
step exists to catch exactly that. What that leg packages is `forge` without `forge-gui` (see
[What gets published](#what-gets-published) above), so its absence is one package and not two.

Two absences are expected and deliberately not required. The macOS `iclforge-dev-*` archive is
best-effort, as above. So is the x86_64 Linux Crucible package: it comes off
`linux-llvm`, which carries no `release_package`, and that leg skips the window and its package
with a warning when the Qt it finds is older than 6.8 - so a release with no
`iclforge-crucible-*-Linux-x86_64.tar.gz` can be that rather than a failure, and the leg's log
distinguishes the two. The aarch64 pair off `linux-llvm-arm64` has no such excuse: a step beside
that leg's pass fails it when the window is missing rather than letting the skip stand, so a
release missing `iclforge-crucible-*-Linux-aarch64.tar.gz` means that leg failed.

## What's deliberately not here

A tag-triggered release publishes signed, attested, SBOM'd packages and a GitHub Release,
`wheels.yml` uploads the Python wheels to PyPI, and `manifest-bump.yml` opens the pull request that
updates the Homebrew tap ([What a release does today](#what-a-release-does-today)). It does **not** publish an APT/DNF package repository, a Docker
image, or anything Home Assistant-shaped - the earlier project this process was modelled on has
release and repository-publishing workflows to copy from if any of those are ever wanted here -
and it publishes nothing to npm, the ESP Component Registry, crates.io, vcpkg, ConanCenter or
winget.
