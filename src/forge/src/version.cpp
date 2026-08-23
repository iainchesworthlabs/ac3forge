#include "ac3/version.hpp"

#include <fmt/format.h>

#include <string>

namespace ac3 {

std::string version_details() {
    std::string out = fmt::format(
        "ac3forge {}\n  release: {}\n  commit:  {}\n  branch:  {}\n  target:  {}", version_full,
        git_describe, git_commit_full, git_branch, build_target);
    if (git_dirty) {
        out += "\n  state:   dirty (uncommitted changes)";
    }
    return out;
}

}  // namespace ac3
