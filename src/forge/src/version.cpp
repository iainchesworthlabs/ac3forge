#include "ac3/version.hpp"

#include <fmt/format.h>

#include <string>

#include "ac3/internal/arch/simd.hpp"

namespace ac3 {

std::string version_details() {
    // The headline: the tag's version, plus the commits past it as build
    // metadata when there are any, so "0.10.0-beta.1+100" is not mistaken
    // for the 0.10.0-beta.1 release. version_full itself stays the tag's
    // (it names packages and the C API's version string).
    std::string headline{version_full};
    if (git_commits_since_tag > 0) {
        headline += fmt::format("+{}", git_commits_since_tag);
    }
    std::string out = fmt::format(
        "ac3forge {}\n  release: {}\n  commit:  {}\n  branch:  {}\n  target:  {}", headline,
        git_describe, git_commit_full, git_branch, build_target);
    // Which src/forge/src/internal/arch/ directory the codec's vector
    // kernels were compiled from (ROADMAP PF5). Read from the selected
    // header itself rather than from a CMake-substituted string, so the
    // binary reports what it actually contains and cannot claim a
    // directory it was not built with.
    out += fmt::format("\n  kernels: {}", internal::arch::kSimdName);
    if (git_dirty) {
        out += "\n  state:   dirty (uncommitted changes)";
    }
    return out;
}

}  // namespace ac3
