#include "../decode_adm.hpp"

#include <expected>
#include <string>
#include <string_view>

#include "../atmos_adm.hpp"

// Compiled only when ICLFORGE_BUILD_ADM did NOT turn iclforge::adm/iclforge::admbridge on (see
// apps/cli/CMakeLists.txt) - see ../decode_adm.hpp's own top comment for why this file, rather
// than a preprocessor conditional inside decode.cpp, is the mechanism. This translation unit
// links neither iclforge::adm nor iclforge::admbridge and includes neither of their headers - it
// cannot, since in this build neither target was ever add_subdirectory()'d at all.

namespace forge_cli {

std::expected<void, std::string> write_adm_atmos_master(std::string_view /*path*/,
                                                          const AdmMasterInput& /*input*/) {
    // Unreachable in practice: decode.cpp checks forge_cli::adm_capability() (the same capability
    // atmos_adm.hpp declares and run_atmos_adm's own dispatch gate already uses) before ever
    // calling this function - see decode.cpp's own comment. Still a real, defined function
    // rather than an abort() or an unreachable() marker, matching atmos_adm.hpp's own disabled
    // stub for load_adm_atmos_source.
    return std::unexpected(std::string(adm_capability().reason));
}

}  // namespace forge_cli
