# Homebrew Cask for ac3gui, ac3forge's Qt6 GUI front end.
#
# A Cask, not a Formula, is the right shape here: ac3gui ships as a prebuilt
# .app bundle inside each release's DragNDrop .dmg (cmake/Packaging.cmake),
# not as something a user builds from source - the same reasoning
# Formula/ac3forge.rb's own header comment and packaging/homebrew/README.md
# already gave for deferring this file. Formula/ac3forge.rb stays the
# CLI-only, build-from-source package (AC3FORGE_BUILD_CLI=ON); this Cask is
# the GUI-only, prebuilt-binary package - two independent installs, matching
# Homebrew's own Formula-vs-Cask split (build-from-source end-user tool vs.
# bundled .app), not a replacement for the Formula.
#
# Staged here (packaging/homebrew/Casks/ac3gui.rb), the same way the Formula
# was, for validation against a real release, and copied into the live
# personal tap (iainchesworthlabs/homebrew-ac3forge) as Casks/ac3gui.rb after
# each bump - see packaging/homebrew/README.md.
#
# v0.8.0-beta.2 was the first tagged release whose macos-llvm leg builds
# AC3FORGE_BUILD_GUI=ON (see docs/platforms/macos.md#gui-on-macos), so it was
# the first release whose ac3forge-*-Darwin.dmg actually contained
# ac3gui.app. **Every release tag** needs the same follow-up update "Every
# release tag" in docs/releasing.md#homebrew-formula-and-cask already
# documents for the sibling Formula: bump version, recompute sha256 from
# that release's own ac3forge-*-Darwin.dmg, validate locally, then copy into
# the tap.
#
# DR8: the .dmg this Cask installs is universal (arm64 + x86_64) as of the
# release that added .github/workflows/_build.yml's package-macos-universal
# job - arm64-macos-llvm was the only vcpkg triplet macOS CI built against
# before that; x64-macos-llvm is now its sibling, built for real on GitHub's
# macos-15-intel runner (native Intel hardware, not Rosetta) and lipo-merged
# with the arm64 build's own install tree rather than either one shipping
# alone. No `depends_on arch:` line any more for exactly that reason - the
# same .dmg installs on both architectures.
cask "ac3gui" do
  version "0.9.0-beta.1"
  # Pinned from v0.9.0-beta.1's actual release asset (GitHub's own reported
  # digest for ac3forge-0.9.0-Darwin.dmg - the same CPACK_PACKAGE_CHECKSUM
  # SHA512 cmake/Packaging.cmake also computes and publishes alongside it,
  # just a different digest algorithm; Homebrew Casks pin sha256). If
  # `brew install` reports a mismatch, trust brew's reported hash over this
  # one and update it here.
  sha256 "fa79d65a560d2c662698a73e0bb88f09bf91970cabfc937c406f783482f3a596"

  # CPack's dmg filename carries only MAJOR.MINOR.PATCH
  # (cmake/Packaging.cmake's CPACK_PACKAGE_FILE_NAME), dropping any
  # "-beta.N" pre-release suffix the git tag itself carries - the same split
  # Formula/ac3forge.rb's install block works around for the source tarball
  # (there via DERIVED_VERSION_OVERRIDE=v#{version}), just read the other
  # way here since the Cask consumes a prebuilt filename instead of naming
  # its own.
  dmg_version = version.major_minor_patch

  url "https://github.com/iainchesworthlabs/ac3forge/releases/download/v#{version}/ac3forge-#{dmg_version}-Darwin.dmg"
  name "ac3gui"
  desc "Qt6 GUI for ac3forge, a clean-room AC-3/E-AC-3 encoder, decoder and Atmos object-layer toolkit"
  homepage "https://github.com/iainchesworthlabs/ac3forge"

  # No `depends_on arch:` restriction (DR8): the .dmg is a universal binary,
  # lipo-merged from a real arm64 build (macos-llvm, Apple Silicon) and a
  # real x86_64 build (macos-llvm-x64, GitHub's native-Intel macos-15-intel
  # runner) by .github/workflows/_build.yml's package-macos-universal job -
  # see that job's own header for how, and docs/platforms/macos.md for the
  # verified-in-CI-only caveat that still applies to both architectures now.
  #
  # cmake/toolchains/macos.llvm.toolchain.cmake pins CMAKE_OSX_DEPLOYMENT_TARGET
  # to 13.3 (Ventura) for C++23 libc++ feature availability - see that
  # file's own header comment.
  depends_on macos: ">= :ventura"

  app "ac3gui.app"

  caveats <<~EOS
    ac3gui is not Apple-notarized or code-signed. release.yml
    (.github/workflows/release.yml) signs release artifacts with GPG (a detached
    .asc) and attests build provenance via Sigstore/OIDC - neither is Apple code
    signing. macOS Gatekeeper will very likely refuse to open the app on first
    launch ("cannot be opened because the developer cannot be verified") until you
    right-click ac3gui.app in Finder and choose Open, or run:
      xattr -dr com.apple.quarantine "#{appdir}/ac3gui.app"
    This Cask itself is unverified on real hardware, same as every other macOS
    claim in this project - see docs/platforms/macos.md, which documents each one
    as CI-only until someone with a Mac checks it by hand.
  EOS
end
