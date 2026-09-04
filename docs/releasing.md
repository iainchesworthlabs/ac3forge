# Releasing ac3forge

How to cut a release: what triggers `.github/workflows/release.yml`, what it produces, and how
to set up the optional GPG signing key. Modelled on an earlier project's release process, with
the parts that don't apply to ac3forge (APT/DNF repository publishing, a Docker image, a Home
Assistant add-on) removed.

## Versioning

ac3forge derives its version from git tags, the same way aqualink-automate does.
`cmake/GitVersionDerivation.cmake` runs `git describe --tags --match "v*"` **before**
`project()` in the top-level `CMakeLists.txt` and feeds the result straight into
`project(ac3forge VERSION ...)` - the tag is the single source of truth. Nothing in the tree
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
`PROJECT_VERSION_FULL` and shows up as `ac3cli --version`'s `version_full` field.

A checkout that can't see any `v*` tag (no history, or a shallow CI clone - see `_build.yml`'s
`fetch_depth` input) falls back to version `0.0.0-dev` rather than failing the build. Ordinary
CI legs stay shallow and always show that fallback; only `release.yml`'s tag-triggered or
dispatched build fetches full history (or gets the version stamped directly via
`-DDERIVED_VERSION_OVERRIDE=`) and shows the real one.

## Pre-release checklist

1. **Before tagging**: confirm `main` carries no unexplained open code-scanning alerts.

   ```bash
   gh api "repos/iainchesworthlabs/ac3forge/code-scanning/alerts?ref=refs/heads/main&state=open" -q '.[] | [.number, .rule.id, .most_recent_instance.location.path] | @tsv'
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
2. CI green on `main` for the commit you're about to tag.
3. Releases must be **cut from main** - `resolve-version` checks this with
   `git merge-base --is-ancestor` and fails otherwise (dry runs are exempt).
4. Decide the tag.
5. **Update [CHANGELOG.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md)**
   - move `## [Unreleased]`'s content down to a `## [x.y.z] - YYYY-MM-DD` section matching the
     tag from step 4, [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format, grouped by
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
The `github-release` job pushes the tag itself, at the very end, only after
build/package/sign/attest have all succeeded - so a failed dispatch run leaves nothing behind to
clean up by hand for a real release. For a **prerelease** dispatch specifically, if the tag gets
pushed but a later step still fails, `cleanup-failed-prerelease` deletes the orphaned tag
automatically; a non-prerelease tag is left alone even on failure; deleting a version someone
explicitly declared is a bigger surprise than a maintainer cleaning it up by hand.

## Dry run

Builds and packages every platform without tagging, signing, or publishing anything. Exempt
from the cut-from-main guard, so it can
run from any branch - use it to validate a packaging change before merging.

## Post-release

Most of what used to be a manual post-release checklist here is now automated (roadmap DR2):

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
3. **The four packaging manifests bump themselves.** Once `github-release` has published the
   release and uploaded every asset, the `manifest-bump` job calls
   [`.github/workflows/manifest-bump.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/manifest-bump.yml),
   which downloads the release's own source tarball and (where they exist)
   `ac3forge-*-Darwin.dmg`/`ac3forge-*-win64.zip`, computes the digests each manifest needs,
   cross-checks the two platform-asset digests against the release's published `SHA512SUMS` (the
   source tarball has none to check against - see that workflow's own comments), and opens a PR
   bumping [vcpkg port](#vcpkg-port), [Homebrew formula and cask](#homebrew-formula-and-cask),
   [winget manifest](#winget-manifest) and [Conan recipe](#conan-recipe) together. It also pushes
   the Formula/Cask straight to the live `iainchesworthlabs/homebrew-ac3forge` tap, if
   `HOMEBREW_TAP_TOKEN` is provisioned (see below) - that tap has no review gate of its own, same
   as a maintainer copying the file in and pushing by hand used to.

   Merging that PR is still a separate, reviewed step - this closes the gap between "tagged" and
   "the bump has started", not the whole gap, which is why
   `tools/checks/check_packaging_versions.sh`'s latest-tag advisory (below) stays a warning, not
   a hard gate. What is **not** automated, because it means writing to a repository this project
   does not own: each manifest's own **Every release tag** section below still lists a follow-up
   PR to `microsoft/vcpkg` (once the port is merged upstream), `conan-center-index` (once the
   recipe is merged upstream) or `microsoft/winget-pkgs`, plus Homebrew's local, macOS-only
   `brew audit`/`brew install --build-from-source`/`brew test` validation - there is no Homebrew,
   and no macOS runner, on any of this project's CI.

   **Testing this without cutting a release**: `manifest-bump.yml` is also directly
   `workflow_dispatch`-able (Actions > Manifest Bump > Run workflow), with `dry_run: true` by
   default. Point it at any already-shipped tag (e.g. the current latest) to exercise the full
   download/digest/cross-check pipeline and see the manifest diffs it would produce, with nothing
   written, committed, pushed or opened - this is release-path automation that otherwise cannot
   be exercised except by shipping a real release.

   **`HOMEBREW_TAP_TOKEN`** (optional): a fine-grained GitHub PAT scoped to `Contents: Read and
   write` on `iainchesworthlabs/homebrew-ac3forge` only. Without it, the tap push step is
   skipped (its `if:` gate simply doesn't fire, with nothing logged) - the in-tree PR still
   opens - and the PR body says so. Add it the same way as
   any other repo secret (Settings > Secrets and variables > Actions); nobody but a human with
   access to GitHub's secret store should ever generate or handle it. The in-tree PR itself needs
   no new secret - it opens with the same built-in `GITHUB_TOKEN` every other job here already
   uses - but does need "Allow GitHub Actions to create and approve pull requests" enabled under
   Settings > Actions > General > Workflow permissions, if it is not already.
4. `tools/checks/check_packaging_versions.sh -r .` also carries a latest-tag advisory now (still
   run as `ci.yml`'s `packaging-consistency` job on every push): a `::warning::`, never a failure,
   per manifest that does not yet match the latest tag - see that script's own header for why
   this stayed a warning rather than becoming a hard gate.

## vcpkg port

A vcpkg port for `ac3forge` is staged in-tree at
[`packaging/vcpkg-port/ac3forge/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/vcpkg-port/ac3forge)
(`vcpkg.json`,
`portfile.cmake`, `usage`) and is pending submission to the curated `microsoft/vcpkg` registry -
see [docs/library/index.md](library/index.md) for how a consumer uses it either
way. It installs the library only (`ac3::forge`, plus `matroska::matroska`/`mp4::mp4`/
`mpegts::mpegts` behind their own `matroska`/`mp4`/`mpegts` features - see
`cmake/InstallLibrary.cmake`'s `AC3FORGE_BUILD_MATROSKA`/`AC3FORGE_BUILD_MP4`/
`AC3FORGE_BUILD_MPEGTS`/`AC3FORGE_INSTALL_BOTH_LINKAGES` options), never the
CLI/GUI/tests/examples/fuzzers, and never `ac3::forge_c` (`AC3FORGE_BUILD_CAPI` - see the note
below). `ac3adm::ac3adm` (the ADM/BW64 reader) and `ac3::admbridge` have no vcpkg feature
either - they do install/export via `find_package(ac3forge)` now (shared-only), but embed
third-party libbw64/libadm and so deliberately carry no vcpkg/Conan feature of their own for
now - see the recipe note further down and [docs/library/index.md](library/index.md).

None of the three container-writer features are on by default: a curated-registry port's
`default-features` may only cover behaviors, not additional public APIs/targets/binaries (see
[vcpkg's maintainer guide](https://learn.microsoft.com/vcpkg/contributing/maintainer-guide#default-features-should-enable-behaviors-not-apis))
, and each of `matroska`/`mp4`/`mpegts` is exactly that. A plain `vcpkg install ac3forge`
installs the codec only; opt in explicitly with `vcpkg install ac3forge[matroska,mp4,mpegts]` or
any subset. The port also pins several build options a curated-registry review otherwise flags
as uncontrolled: `AC3FORGE_BUILD_ADM`/`AC3FORGE_ENABLE_TRACY` explicitly OFF (already the
project's own default, pinned so a future default change can't silently pull an undeclared
dependency into this port), and `AC3FORGE_WITH_ALSA`/`AC3FORGE_WITH_PIPEWIRE` explicitly OFF -
without that, this library-only build still probes the build machine's ambient ALSA/PipeWire
installs (`src/audio/` is `add_subdirectory()`'d unconditionally outside Emscripten, not gated
on `AC3FORGE_BUILD_CLI`/`AC3FORGE_BUILD_GUI`) even though `ac3::audio` is never installed or
exported. `vcpkg.json` also declares `"supports": "!(android & !arm64)"` - only `arm64-v8a`
Android is a real target (see [docs/platforms/android.md](platforms/android.md)); other Android
architectures fail to build (`matroska`'s size comparisons assume a 64-bit `size_t`).

`ac3::forge_c` (roadmap F1) is exposed as the port's `capi` feature (`vcpkg install
ac3forge[capi]`), off by default like `matroska`/`mp4`/`mpegts` above. Its `capiTargets` export
used to require `forge_static` even when `AC3FORGE_INSTALL_BOTH_LINKAGES=OFF` left that target
unexported - a real bug independent of vcpkg, fixed in `cmake/InstallLibrary.cmake` by exporting
`forge_static` alongside `forge_shared` in that branch whenever `AC3FORGE_BUILD_CAPI` is `ON`
(#227) - which is what made adding the feature itself (roadmap AP7) a scope decision rather than
a bug workaround.

Any future optional library component follows the same three-step recipe this repo's own
`AC3FORGE_BUILD_<NAME>` options already establish: add the CMake option and its
`cmake/InstallLibrary.cmake` guard first (that part isn't vcpkg-specific; also add a matching
`ac3forge_install_pkgconfig()` call there - see the "pkg-config" section of
[docs/library/index.md](library/index.md), a consumer expects one alongside every installed
component's CMake export), then add a same-named
feature to `packaging/vcpkg-port/ac3forge/vcpkg.json` and one line to `portfile.cmake`'s
`vcpkg_check_features()` call - unless the component pulls in a real third-party link dependency
of its own, the way `ac3adm`/`ac3::admbridge` do (see
[ADM / BW64 reading](library/adm.md#why-opt-in)): those still install/export (shared-only, to
stay self-contained without re-exporting the third party), but deliberately have no vcpkg/Conan
feature of their own for now.

**Every release tag, once the port has been merged upstream**, needs a follow-up PR to
`microsoft/vcpkg` - the curated registry has no mechanism to track a moving `main`, so a new
`ac3forge` release is invisible to `vcpkg install` until this happens. Step 1 below is now done
by [`manifest-bump.yml`'s PR](#post-release) (roadmap DR2) rather than by hand; steps 2-3 still
are, since they write to a repository this project does not own:

1. Bump `packaging/vcpkg-port/ac3forge/vcpkg.json`'s `version-semver` to the new tag, and
   `portfile.cmake`'s `vcpkg_from_github()` `REF`/`SHA512` to match (`sha512sum` the tag's
   release tarball, or let a first `vcpkg install` attempt report the correct hash).
2. Validate locally first (see below) before touching the upstream fork - a portfile change
   that fails vcpkg's own CI is slower to iterate on there than here.
3. Copy the updated port files into the `microsoft/vcpkg` fork's `ports/ac3forge/`, run
   `vcpkg format-manifest ports/ac3forge/vcpkg.json` (its formatting is stricter than this
   repo's own JSON style - `vcpkg x-add-version` refuses to run against an unformatted
   manifest) followed by `vcpkg x-add-version ac3forge` to regenerate
   `versions/baseline.json`/`versions/a-/ac3forge.json` (don't hand-edit these), and open the
   version-bump PR.

**Validating the port locally**, any time `packaging/vcpkg-port/ac3forge/` or the CMake options
it drives change (whether or not a release is involved):

```bash
vcpkg install ac3forge --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows
vcpkg install ac3forge --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows-static
vcpkg install ac3forge[matroska,mp4,mpegts,capi] --classic --overlay-ports=packaging/vcpkg-port --triplet x64-windows
```

`--classic` is required from inside this repo - the root `vcpkg.json` (manifest mode, for this
project's *own* build-time dependencies) would otherwise shadow the package-name argument.
Check for a clean post-build lint (no "not used"/"missing usage" warnings) and that the bare
`ac3forge` install genuinely excludes `matroska::matroska`/`mp4::mp4`/`mpegts::mpegts`/
`ac3::forge_c` - not just unlinked, no matching files anywhere in the install tree - while
`ac3forge[matroska,mp4,mpegts,capi]` installs all four.

Fetching a real tag only exercises whatever `AC3FORGE_BUILD_*` options actually existed in that
tagged source - `vcpkg_from_github()`'s `REF` always points at an already-released tag, so a
CMake option added since the last tag (as happened here: `AC3FORGE_BUILD_MP4`/
`AC3FORGE_BUILD_MPEGTS` landed in `develop` after `v0.5.0-beta.1`) can't be exercised through a
real fetch until the *next* tag contains it. To validate a port change against unreleased CMake
options, temporarily swap the `vcpkg_from_github()` block in a scratch copy of `portfile.cmake`
for `set(SOURCE_PATH "<absolute path to this checkout>")`, run the same three commands against
that scratch copy, and discard it once validated - never commit that substitution.

## Publishing to PyPI

Roadmap **F2**: Python bindings (`python/`, see
[docs/library/python-api.md](library/python-api.md)) as the `ac3forge` PyPI package, with wheels
for Windows, macOS and Linux built by `.github/workflows/wheels.yml` via `cibuildwheel`. That
workflow's `build` job runs continuously (every push/PR touching `python/**`, same "buildable is
checked continuously" reasoning as `windows-msvc`'s packaging smoke test above) and always
uploads the wheels it builds as a workflow artifact.

**Publishing to PyPI is live**: the `pypi` GitHub environment is provisioned and
[`ac3forge`](https://pypi.org/project/ac3forge/) is a real published package. `wheels.yml`'s
`publish` job is gated on both a `v*` tag push and the `pypi` environment, and uses
[PyPI trusted publishing](https://docs.pypi.org/trusted-publishers/) (OIDC) rather than a stored
API token — there is no `PYPI_API_TOKEN` secret to leak in the first place. **Nobody should ever
generate a long-lived PyPI API token and paste it into a chat with an agent or into a GitHub
secret** — trusted publishing exists specifically so that never has to happen.

The one-time setup that provisioned it, for reference (done by a maintainer directly on pypi.org
and on GitHub, and not something a future release needs to repeat):

1. On PyPI, either publish the very first `ac3forge` release by hand (`python -m build python/`
   then `twine upload`, using a temporary scoped token deleted immediately after) to create the
   project, or use PyPI's **pending publisher** mechanism (Your projects → Publishing →
   "Add a pending publisher") to pre-register the trusted publisher for a project name that does
   not exist yet — the second path needs no manual upload at all and is the one to prefer.
2. Either way, register the trusted publisher against this repository: owner
   `iainchesworthlabs`, repository `ac3forge`, workflow `wheels.yml`, environment `pypi`.
3. In the GitHub repo, create an environment named `pypi` (Settings → Environments) — no secrets
   need adding to it; its existence and name are what PyPI's trusted-publisher registration keys
   against, and `wheels.yml`'s `publish` job declares `environment: pypi` so the job has somewhere
   to request the OIDC token from. Optionally add required reviewers on the environment for a
   manual approval gate before a publish actually runs.

Pushing a `v*` tag (the same tag that triggers `release.yml`, see
[Option A](#option-a-tag-based-release-the-normal-path) above) triggers `wheels.yml`'s `publish`
job for that tag, which requests an OIDC token against the `pypi` environment and uploads the
built wheels — the `build` job (and its artifact) runs on every push regardless.

## Publishing to npm

Roadmap **UX5**: the browser decoder package (`js/`, see
[docs/platforms/wasm.md](platforms/wasm.md)) as the
`ac3forge-wasm-decoder` npm package.
Versioning mirrors the PyPI package above rather than reinventing it: `js/package.json` carries a
`0.0.0-dev` placeholder in the tree (the same untagged-build fallback CMake's own
`GitVersionDerivation.cmake` uses), and `npm.yml`'s `publish` job stamps the real,
resolved version (`npm version <version> --no-git-tag-version`) immediately before `npm publish`
— nothing to keep in sync by hand, and the tag is still the single source of truth.

**Publishing to npm is not enabled yet** — unlike PyPI above. `ac3forge-wasm-decoder` has never
been published, and two separate things hold it: the one-time setup below has not been done, and
`npm.yml`'s `publish` job is deliberately narrowed to `workflow_dispatch` so that a `v*` tag
cannot create a brand-new public package as a side effect of cutting a release. It uses (like
PyPI) [npm trusted publishing](https://docs.npmjs.com/trusted-publishers)
(OIDC) rather than a stored token — there is no `NPM_TOKEN` secret to leak in the first place.
**Nobody should ever generate a long-lived npm token and paste it into a chat with an agent or
into a GitHub secret** — trusted publishing exists specifically so that never has to happen.

The one-time setup this needs (a maintainer, directly on npmjs.com and on GitHub — not something
an agent should do, the same rule as PyPI's setup above):

1. Publish the very first `ac3forge-wasm-decoder` release by hand (`cd js && npm publish` with a
   temporary, scoped token deleted immediately after) to create the project on npmjs.com — npm's
   trusted-publishing setup, unlike PyPI's, needs the package to already exist; there is no
   "pending publisher" pre-registration mechanism for a name that doesn't exist yet.
2. On the package's npmjs.com settings page, add a trusted publisher: provider GitHub Actions,
   organization/user `iainchesworthlabs`, repository `ac3forge`, workflow filename
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

A Homebrew formula for `ac3cli` is staged in-tree at
[`packaging/homebrew/Formula/ac3forge.rb`](https://github.com/iainchesworthlabs/ac3forge/blob/main/packaging/homebrew/Formula/ac3forge.rb)
and published to the live personal tap
[`iainchesworthlabs/homebrew-ac3forge`](https://github.com/iainchesworthlabs/homebrew-ac3forge) - see
[`packaging/homebrew/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/packaging/homebrew/README.md)
for why a personal tap rather than a `homebrew-core` submission. Unlike the vcpkg port, this
packages the CLI (`ac3cli`), not the library: `AC3FORGE_BUILD_CLI=ON` with GUI/tests/examples/
fuzzers off, built from the release source tarball.

The GUI (`ac3gui`) is a separate Homebrew Cask,
[`packaging/homebrew/Casks/ac3gui.rb`](https://github.com/iainchesworthlabs/ac3forge/blob/main/packaging/homebrew/Casks/ac3gui.rb)
- a Cask, not a Formula, is the right shape for a bundled, prebuilt `.app` the way `ac3gui.app`
already ships in every platform's release archive (`cmake/Packaging.cmake`'s DragNDrop `.dmg` on
macOS). It's staged the same way the formula is. `v0.8.0-beta.2` is the first tagged release
whose macOS build actually contains `ac3gui` - `macos-llvm` only started building the GUI at all
once [GUI on macOS](platforms/macos.md#gui-on-macos) landed - so the cask's `version`/`sha256`
are now pinned from a real release rather than placeholders; see the cask file's own header
comment.

**Every release tag** needs a follow-up update to the formula, same shape as the vcpkg port's.
Steps 1 and 3 are now done by [`manifest-bump.yml`'s PR and tap push](#post-release) (roadmap
DR2) rather than by hand - step 2, local `brew` validation, still is, since there is no Homebrew
on any of this project's CI runners:

1. Bump `packaging/homebrew/Formula/ac3forge.rb`'s `url` to the new tag and `sha256` to match
   (`sha256sum` the tag's release tarball - the same tarball the vcpkg port's `SHA512` already
   points at, just a different digest algorithm).
2. Validate locally first (see below) before touching a tap - a formula change that fails
   `brew audit` is slower to iterate on there than here.
3. Copy the updated formula into the `homebrew-ac3forge` tap's `Formula/ac3forge.rb` and push.

The same three steps apply to the cask now that it tracks a real release too: bump `version` to
the new tag and `sha256` to the release's `ac3forge-*-Darwin.dmg` (`sha256sum` it, or trust
CPack's own published `.dmg.sha512` after converting digest algorithms), validate locally, then
copy `packaging/homebrew/Casks/ac3gui.rb` into the tap's `Casks/ac3gui.rb` and push - both files
ship from the same tap.

**Validating the formula locally**, from a macOS machine with Homebrew installed:

```bash
brew install --build-from-source ./packaging/homebrew/Formula/ac3forge.rb
brew test ac3forge
brew audit --formula ./packaging/homebrew/Formula/ac3forge.rb
brew uninstall ac3forge
```

**Validating the cask locally**, the same way, once you have a macOS machine with Homebrew
installed - not yet run for real, same caveat as the formula above:

```bash
brew audit --cask ./packaging/homebrew/Casks/ac3gui.rb
brew install --cask ./packaging/homebrew/Casks/ac3gui.rb
brew uninstall --cask ac3gui
```

There is no Homebrew on any of this project's CI runners or on Windows/Linux dev machines, so
this validation is manual and macOS-only - there is nothing here to automate against, unlike
the vcpkg `--overlay-ports` flow above.

## winget manifest

A winget manifest for `ac3forge` (`ac3cli` and `ac3gui` together) is staged in-tree at
[`packaging/winget/manifests/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/winget/manifests),
at the exact `manifests/<first-letter>/<publisher>/<package>/<version>/` path a
`microsoft/winget-pkgs` submission uses, so the version directory can be copied straight into a
fork of that repo.

As of DR7, `.github/workflows/_build.yml`'s `windows-msvc` leg installs `makensis` via
Chocolatey and `cpack` produces a real NSIS `.exe` installer on every push - the leg fails
outright if it doesn't (see `cmake/Packaging.cmake`'s `find_program(makensis)` gate and the
"Assert the NSIS installer was produced" step). **The next release tag onward**, bump the
manifest with `InstallerType: nullsoft` against that release's `ac3forge-X.Y.Z-win64.exe`,
dropping `NestedInstallerType`/`NestedInstallerFiles` entirely - a real installer replaces the
nested-portable-zip shape, it doesn't add to it.

Every version directory published **before** DR7 landed (`0.9.0-beta.1` and earlier) legitimately
keeps `InstallerType: zip` with `NestedInstallerType: portable` against that release's
`win64.zip`: those releases really did ship without an NSIS `.exe` (`makensis` wasn't on the
runner yet), and a staged manifest must describe what a release actually shipped, not what a
later fix made possible. Never rewrite an already-published version directory to claim an
installer that release never produced.

**Every release tag** needs a new version directory, since winget-pkgs versions each release
independently rather than tracking a moving tag the way vcpkg's `version-semver` does. Step 1
is now done by [`manifest-bump.yml`'s PR](#post-release) (roadmap DR2), which renders all three
files fresh from a template rather than copying the previous version directory - but it renders
the pre-DR7 shape (`InstallerType: zip` with `NestedInstallerType: portable`, digested against
the release's `win64.zip`; `tools/release/bump_manifests.py` never downloads the `win64.exe` at
all), so step 2's nullsoft conversion, step 3's local `winget validate`, and step 4's fork PR
all still need a human with the `winget` CLI:

1. Copy `packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<prev-version>/` to a new
   `<new-version>/` directory, updating `PackageVersion` in all three files to match.
2. Update the installer manifest to `InstallerType: nullsoft`, its `InstallerUrl` to the new
   release's `win64.exe` and `InstallerSha256` to match (`sha256sum` the `.exe` - winget wants
   SHA256, unlike the `SHA512SUMS` `release.yml` publishes for every artifact, see [What gets
   published](#what-gets-published) below), and remove `NestedInstallerType`/
   `NestedInstallerFiles`.
3. Validate locally first (see below) before touching a fork.
4. Copy the new version directory into the `microsoft/winget-pkgs` fork at the matching
   `manifests/i/iainchesworthlabs/ac3forge/<new-version>/` path and open the submission PR.

The binaries inside that `.exe` are unsigned (roadmap DR6, blocked on code-signing
certificates) - the installer building at all does not by itself resolve DR4's winget
resubmission block, which cites an unsigned-binary Defender false positive as the likely cause.
An unsigned NSIS installer may trip the same detection, or Windows SmartScreen on top of it;
resolve DR6 before assuming a DR4 resubmission will go through clean.

**Validating the manifest locally**, with the `winget` CLI (ships with Windows 10/11):

```bash
winget validate --manifest packaging/winget/manifests/i/iainchesworthlabs/ac3forge/<version>
```

## Conan recipe

A Conan (2.x) recipe for `ac3forge` is staged in-tree at
[`packaging/conan/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/conan)
(`conanfile.py`, `conandata.yml`, `test_package/`) and is pending submission to ConanCenter
(`conan-center-index`). Scoped the same as the vcpkg port - the library only (`ac3::forge`,
plus `matroska::matroska`/`mp4::mp4`/`mpegts::mpegts` behind their own default-on `matroska`/
`mp4`/`mpegts` options), never the CLI/GUI/tests/examples/fuzzers - with one Conan option per
`AC3FORGE_BUILD_<NAME>` CMake option, the same pattern the vcpkg port's `vcpkg_check_features()`
call already establishes. Rather than asking Conan's `CMakeDeps` generator to synthesise a
second CMake package config, the recipe sets `cmake_find_mode` to `"none"` and points consumers
at the config `cmake/InstallLibrary.cmake` already installs - see `conanfile.py`'s
`package_info()` comment. A consumer's `find_package(ac3forge CONFIG REQUIRED)` and
`target_link_libraries(main PRIVATE ac3::forge)` calls are identical to the vcpkg or plain
`cmake --install` case (see [docs/library/index.md](library/index.md)), Conan or not.

**Every release tag**, once the recipe has been merged upstream, needs a follow-up PR to
`conan-center-index` - ConanCenter has no mechanism to track a moving `main` either, same as
vcpkg's curated registry. Step 1 is now done by [`manifest-bump.yml`'s PR](#post-release)
(roadmap DR2) rather than by hand; steps 2-3 still are:

1. Add a new entry to `packaging/conan/conandata.yml`'s `sources` map, keyed by the new
   version, with the tag's release tarball `url` and `sha256` (same tarball the vcpkg port's
   `SHA512` and the Homebrew formula's `sha256` already point at, just fetched fresh).
2. Validate locally first (see below) before touching the upstream fork.
3. Copy the updated recipe into the `conan-center-index` fork's `recipes/ac3forge/`, add the
   new version to that recipe's own `config.yml`, and open the version-bump PR.

**Validating the recipe locally**, any time `packaging/conan/` or the CMake options it drives
change (whether or not a release is involved):

```bash
conan create packaging/conan --version <version> -s compiler.cppstd=23
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:shared=True"
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:matroska=False" -o "&:mp4=False" -o "&:mpegts=False"
conan create packaging/conan --version <version> -s compiler.cppstd=23 -o "&:capi=True"
```

`-s compiler.cppstd=23` is required - a bare default profile's `compiler.cppstd` predates
C++23 on most Conan installs, and `check_min_cppstd(self, 23)` in `conanfile.py` fails fast
rather than configuring a build that would fail deep inside compilation instead. Each command
above builds `test_package/`, which links `ac3::forge` and runs it, exercising the exact
`find_package(ac3forge)` path a real consumer uses - a passing `conan create` is a stronger
signal than a configure-only check for that reason. Fetching a real tag only exercises whatever
`AC3FORGE_BUILD_*` options actually existed in that tagged source, same caveat as the vcpkg
port's local-source-override technique above - `packaging/conan/conandata.yml` would need the
same scratch-entry treatment (a local `url` pointing at this checkout instead of a GitHub
tarball) to validate a CMake option added since the last tag.

## What gets published

One package per OS **and architecture**, not one per compiler-toolchain leg: `_build.yml`'s matrix
builds and tests both Windows toolchains (MSVC, clang-cl), both Linux toolchains (GCC, Clang) - on
both x64 and arm64 - and, since DR8, both macOS architectures (arm64 and x86_64) on every push. For
Windows and Linux, only the leg marked `release_package: true` per OS/arch actually packages for a
release - windows-msvc, linux-gcc and linux-gcc-arm64. windows-llvm, linux-llvm and
linux-llvm-arm64 still catch compiler-specific bugs in full, every push; they just don't produce a
second, redundantly canonical archive that a downloader would have no way to choose between.
`cmake/Packaging.cmake` arch-qualifies the Linux archive filename (`ac3forge-X.Y.Z-Linux-x86_64.tar.gz`
vs. `...-Linux-aarch64.tar.gz`) specifically so the two Linux architectures' TGZ/ZIP downloads never
collide; DEB/RPM already carry their arch in their own filenames.

macOS doesn't fit the "one `release_package` leg" shape at all: neither `macos-llvm` (arm64) nor
`macos-llvm-x64` (x86_64, on GitHub's native-Intel `macos-15-intel` runner - real hardware, not
Rosetta) carries `release_package`. A separate `package-macos-universal` job instead
`cmake --install`s each leg's `runtime` component, `lipo -create`s every Mach-O file the two trees
have in common (`ac3cli`, `ac3gui`, and every dylib/framework binary
`qt_generate_deploy_qml_app_script` copies into `ac3gui.app/Contents/Frameworks/`), and packages the
merged tree with `hdiutil` directly - the same call CPack's own DragNDrop generator makes under the
hood. So there is still exactly one macOS end-user package per release, just built from two legs'
output rather than one leg's own `cpack` run - which is also why it ships as a `.dmg` only, not the
`.zip` a single-arch leg's own `cpack --preset pack-macos-llvm` also produces alongside its `.dmg`:
nothing merges a second, redundant plain-archive form of the same universal binary today. The
matching `ac3forge-dev-*` library archive is attempted the same way (`library`/`libruntime`
components instead of `runtime`) but is best-effort - see `package-macos-universal`'s own comment in
`_build.yml` - so it may be missing from a given release; check that job's log if it's absent.

| Platform | Arch | Leg | End-user packages | Library (`ac3forge-dev-*`) |
|---|---|---|---|---|
| Windows | x64 | windows-msvc | `.zip`, `.exe` (NSIS) | `.zip` |
| Linux | x86_64 | linux-gcc | `.tar.gz`, `.deb`, `.rpm` | `.tar.gz`, plus real system packages: `libac3forge0`/`ac3forge-devel` (RPM) and `libac3forge0`/`libac3forge-dev` (DEB) |
| Linux | aarch64 (Raspberry Pi 4/5 and other arm64 targets) | linux-gcc-arm64 | `.tar.gz`, `.deb`, `.rpm` | same split as x86_64, above |
| macOS | arm64 + x86_64 (universal) | macos-llvm + macos-llvm-x64, merged by `package-macos-universal` | `.dmg` | `.zip`, best-effort (see above) |
| Android (Shield) | arm64 (NDK) | build-android | `.apk` | none - Shield links `ac3::forge`/`ac3::audio` in-tree, it isn't a `find_package(ac3forge)` consumer |

Windows x64 additionally ships the AC3Forge Crucible as its own
`ac3forge-crucible-*-win64.zip` (roadmap UX11,
[docs/platforms/windows-demo.md](platforms/windows-demo.md)): the `crucible` CPack component -
`ac3crucible.exe`, the `ac3crucible-run` runner, the driver's install/remove scripts and a Qt runtime of
its own - packaged by the same `windows-msvc` leg as the row above and picked up by
`release.yml`'s existing `*.zip` glob. It is a separate download rather than part of the
`runtime` component, and deliberately absent from the NSIS installer
(`cmake/CPackProjectConfig.cmake` says why): its null-sink driver is test-signed only, so the
demo needs a machine with test signing on to be useful, which is not something an `ac3cli`
download should carry. When the EV certificate lands, the installer takes over installing the
demo and its signed driver - one line in that file, and this paragraph, change together.

Linux x86_64 also ships a self-contained `ac3gui` `.AppImage` (roadmap DR8), built by its own
`linux-appimage` job rather than a `release_package: true` leg above - it isn't a CPack product
at all, so it sits outside this table's "one canonical leg per OS/arch" framing, but it runs on
every push the same continuous-packaging way `windows-msvc` does and lands in every real release
alongside the row above. See [docs/platforms/linux.md](platforms/linux.md#appimage) for why it
exists and how it's built.

The end-user packages are `ac3cli`/`ac3gui` (CPack's `runtime` component) on desktop, or the
Shield app's `.apk` on Android. The library packages are a second, independent download for a
third party consuming `ac3::forge`/`matroska::matroska` via `find_package(ac3forge)` (see
[docs/library/index.md](library/index.md)) - headers, static and shared libraries, and the
CMake package config, but neither `ac3cli`/`ac3gui` nor `ac3::audio` (live capture/monitor/
passthrough stays a CLI/GUI-internal detail, not part of what's installed here).

Archive downloads (ZIP/TGZ) bundle everything above into one `ac3forge-dev-*` file, one per
platform regardless of compiler leg, same reasoning as the end-user package above - not NSIS (a
component installer can't also produce a second standalone download), not DragNDrop (no macOS
host to build or verify it against at all).

Linux additionally gets a **real runtime/`-dev` split** as proper system packages, not just an
archive: `cmake/InstallLibrary.cmake` files the shared libraries' versioned `.so` under its own
CPack component (`libruntime`, split out via `NAMELINK_COMPONENT` - see that file's comment),
separate from headers/static-archives/CMake-config/namelink-symlink (`library`). DEB/RPM's own
component-install switches turn that into three independent packages - `ac3forge` (the CLI/GUI),
`libac3forge0` (just the `.so` a linked binary loads), and `libac3forge-dev`/`ac3forge-devel`
(everything a builder needs, version-pinned to depend on the exact matching `libac3forge0`) -
the same `libFOO`/`libFOO-dev` shape as any other Linux C library, installable with a plain
`apt install`/`dnf install` rather than a manual archive download. ZIP/TGZ still produce one
merged `ac3forge-dev-*` archive as before (`cmake/Packaging.cmake` groups `library`+`libruntime`
together for the archive generators; `cmake/CPackProjectConfig.cmake` overrides that back apart
for DEB/RPM specifically). Confirmed against real `dpkg-deb -c`/`-I` and `rpm -qlp`/`-qRp` output
in a Linux build, not just a CMake reading - the pre-split `.deb` was, on inspection, a single
package silently bundling `ac3cli` together with the *entire* SDK (headers, static archives, and
the CMake package config all thrown in beside the binary), which this split also fixes as a
side effect.

The Shield `.apk` is signed with a real release keystore when one is provisioned (see
"Provisioning the Android release keystore" below), and falls back to AGP's default debug
keystore cleanly if it isn't - either way it's fine for sideloading onto a Shield in developer
mode. A release keystore is a prerequisite before this could ever go through the Play Store,
which sideloading itself doesn't require. (Not to be confused with **object signing** - the EMDF
Atmos authenticity tag, provisioned separately via the `ATMOS_SIGNING_KEY` secret and
unrelated to APK code-signing; see "Provisioning the Android object-signing key" below.)

Alongside the packages, one artifact that is not a build of anything:
**`ac3forge-conformance-vectors-<version>.tar.gz`**, the published conformance vector set
(roadmap VX20) - 60 AC-3 / E-AC-3 / Atmos streams covering each coding tool, layout and sample
rate the encoder can emit, with the source PCM each was encoded from, the expected decode hashes
and a manifest of what each exercises. `_build.yml`'s linux-gcc leg builds it, from
`tools/generators/gen_conformance_vectors.py`; the release call additionally sets
`publish_conformance_vectors`, which regenerates the whole bundle a second time and fails the leg
if a single hash moved. It lands in `release-artifacts/` after the "at least one package was
built" check - deliberately, since it is a `.tar.gz` and would otherwise satisfy that check on
its own - and from there it is signed, checksummed, SBOM'd and attested exactly like a package.
See [Conformance vectors](conformance-vectors.md) for what is in it and how a decoder implementer
uses it.

No leg is `experimental: true` any more (see `ci.yml`'s status table), so all five package
for real rather than best-effort - a packaging failure on any of them blocks the release the
same as a build or test failure would. Every package - end-user or library - gets a `.sha512`
(`CPACK_PACKAGE_CHECKSUM` in `cmake/Packaging.cmake`), an aggregate `SHA512SUMS` manifest,
keyless Sigstore/OIDC build provenance, and an SPDX SBOM covering the whole release artifact
set - see Verifying a download below. GPG signatures are additional and only appear once a
signing key is provisioned (next section); their absence doesn't block a release.

## Provisioning the GPG signing key (optional, one-time)

GPG signing is off by default - the release workflow checks whether `REPO_GPG_PRIVATE_KEY` is
set and skips the signing steps cleanly if it isn't. The `SHA512SUMS` manifest itself is
generated either way, unconditionally, in its own step ahead of the GPG-gated one - GPG only adds
a detached signature over it and over each artifact; `manifest-bump.yml` (roadmap DR2) depends on
`SHA512SUMS` existing for every release, signed or not. **Nobody should ever paste a private key
into chat with an agent, or ask one to generate/handle key material** - do this yourself,
locally:

```bash
# 1. Generate a signing-only key (no passphrase keeps CI simplest - see the
#    tradeoff note below before deciding that's right for you).
gpg --batch --quick-generate-key "ac3forge <you@example.com>" rsa4096 sign never

# 2. Export the private key.
gpg --armor --export-secret-keys "ac3forge" > ac3forge-signing-key-private.asc
```

3. In the GitHub repo, go to Settings > Secrets and variables > Actions, and add:
   - `REPO_GPG_PRIVATE_KEY` - the full contents of `ac3forge-signing-key-private.asc`.
   - `REPO_GPG_PASSPHRASE` - only if you gave the key a passphrase in step 1.
4. Delete the local `ac3forge-signing-key-private.asc` file.

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
  -keystore ac3forge-shield-release.keystore \
  -storetype PKCS12 \
  -alias ac3forge-shield \
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
base64 -w0 ac3forge-shield-release.keystore > ac3forge-shield-release.keystore.b64
```

4. In the GitHub repo, go to Settings > Secrets and variables > Actions, and add:
   - `ANDROID_KEYSTORE_BASE64` - the full contents of `ac3forge-shield-release.keystore.b64`.
   - `ANDROID_KEYSTORE_PASSWORD` - the password from step 1.
   - `ANDROID_KEY_ALIAS` - `ac3forge-shield` (or whatever `-alias` you used).
   - `ANDROID_KEY_PASSWORD` - the same password as `ANDROID_KEYSTORE_PASSWORD` (PKCS12 doesn't
     support a different one - see step 1).
5. Delete the local `ac3forge-shield-release.keystore.b64` file. **Keep the `.keystore` file
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
# Provenance (keyless, ties the bytes to this exact repo/workflow/commit)
gh attestation verify ac3forge-0.2.0-win64.zip --repo iainchesworthlabs/ac3forge

# GPG (ties the bytes to the maintainer's key, once one is provisioned)
gpg --import ac3forge-signing-key.asc
gpg --verify SHA512SUMS.asc SHA512SUMS && sha512sum -c SHA512SUMS
gpg --verify ac3forge-0.2.0-win64.zip.asc ac3forge-0.2.0-win64.zip
```

## Troubleshooting

**"... is not on main - releases must be cut from main"** - the commit you tagged (or the ref
you dispatched from) hasn't been merged to `main` yet.

**"tag vX.Y.Z already exists"** (manual dispatch only) - either retry with a different version,
or delete the existing tag first if it was created in error:
`git push origin :refs/tags/vX.Y.Z && git tag -d vX.Y.Z`.

**No package for a platform in the release** - that leg's `build-packages` job failed for real.
No leg is `experimental: true` any more (see [What gets published](#what-gets-published) above),
so a missing package is a genuine failure to investigate, not an expected gap for a
not-yet-promoted leg - check the run's `build-packages` job.

## What's deliberately not here

A tag-triggered release publishes signed, attested, SBOM'd packages and a GitHub Release. It does
**not** publish an APT/DNF package repository, a Docker image, or anything Home Assistant-shaped -
the earlier project this process was modelled on has release and repository-publishing workflows
to copy from if any of those are ever wanted here.
