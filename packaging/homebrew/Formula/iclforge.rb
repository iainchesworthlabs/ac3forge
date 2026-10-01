# Homebrew formula for forge, iclforge's command-line encoder/decoder.
#
# Builds the CLI only (ICLFORGE_BUILD_CLI=ON, everything else the library
# doesn't need for that off) - same reasoning as the vcpkg port
# (packaging/vcpkg-port/iclforge/) staying library-only, just the other way
# round: Homebrew formulae are for end-user tools, so this ships the tool
# vcpkg deliberately does not, and skips find_package(iclforge) dev files
# vcpkg already covers. The Qt6 GUI (forge-gui) is not packaged here - a Homebrew
# Cask, not a Formula, is the right shape for a bundled .app. That cask
# (../Casks/iclforge.rb) installs the prebuilt forge-gui.app from each release's
# .dmg.
#
# Staged here (packaging/homebrew/Formula/iclforge.rb) for local
# `brew install --build-from-source` validation against this repo, and
# copied into the live personal tap (iainchesworthlabs/homebrew-iclforge) as
# Formula/iclforge.rb after each bump - see docs/releasing.md.
class Iclforge < Formula
  desc "Clean-room AC-3/E-AC-3/AC-4 encoder, decoder and Atmos object-layer CLI"
  homepage "https://github.com/iainchesworthlabs/iclforge"
  url "https://github.com/iainchesworthlabs/ac3forge/archive/refs/tags/v0.10.0-beta.1.tar.gz"
  # Computed directly (sha256sum) from the same release tarball the vcpkg
  # port's portfile.cmake pins by SHA512 - see that file's comment. If
  # `brew install` reports a mismatch, trust brew's reported hash over this
  # one and update it here.
  sha256 "e9a54c509f8af73d51d75ca465816d8579e3e33715fe2c02f19873a2f08f5cf5"
  license "GPL-3.0-or-later"
  head "https://github.com/iainchesworthlabs/iclforge.git", branch: "main"

  depends_on "cmake" => :build

  def install
    # DERIVED_VERSION_OVERRIDE: cmake/GitVersionDerivation.cmake derives the
    # project version via `git describe`, which finds nothing in a release
    # tarball (no .git directory) and silently falls back to "0.0.0-dev".
    # `version` here is Homebrew's own parse of the url= tag, so re-adding
    # the "v" prefix recovers the real tag - same technique
    # packaging/vcpkg-port/iclforge/portfile.cmake uses for the same reason.
    system "cmake", "-S", ".", "-B", "build",
                     "-DICLFORGE_BUILD_CLI=ON",
                     "-DICLFORGE_BUILD_GUI=OFF",
                     "-DICLFORGE_BUILD_TESTS=OFF",
                     "-DICLFORGE_BUILD_EXAMPLES=OFF",
                     "-DICLFORGE_BUILD_FUZZERS=OFF",
                     "-DICLFORGE_FETCH_CATCH2=OFF",
                     "-DDERIVED_VERSION_OVERRIDE=v#{version}",
                     *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"

    # `cmake --install` already placed the generated man page and the four
    # shell completion scripts (roadmap IO8 - see apps/cli/CMakeLists.txt,
    # which generates them by running the freshly built forge) under
    # #{prefix}/share. Homebrew links share/man/man1 and
    # share/zsh/site-functions itself, so those two need nothing here. bash
    # and fish are the two it expects a formula to place through its own
    # helpers, so those move into place. The PowerShell script stays where
    # CMake put it, under share/iclforge/completions: it has no
    # convention-driven search path to be linked into, and `forge
    # completions powershell` says so itself.
    bash_completion.install share/"bash-completion/completions/forge"
    fish_completion.install share/"fish/vendor_completions.d/forge.fish"
  end

  test do
    assert_match "iclforge #{version}", shell_output("#{bin}/forge --version")
    # The generated artefacts, checked as installed files: a formula that
    # silently stops shipping them is the failure worth catching here.
    assert_path_exists man1/"forge.1"
    assert_match "_forge", (bash_completion/"forge").read
    # Per-command help, and the documented exit-code scheme (roadmap IO8):
    # a usage error is 1, not an undifferentiated non-zero.
    assert_match "forge encode", shell_output("#{bin}/forge help encode")
    shell_output("#{bin}/forge encode 2>&1", 1)
  end
end
