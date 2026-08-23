#include "ac3/version.hpp"

#include <fmt/format.h>

#include <string>

#include "ac3/internal/arch/simd.hpp"

namespace ac3 {

std::string version_details() {
    std::string out = fmt::format(
        "ac3forge {}\n  release: {}\n  commit:  {}\n  branch:  {}\n  target:  {}", version_full,
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
