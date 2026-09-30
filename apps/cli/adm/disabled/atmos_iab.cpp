#include "../atmos_iab.hpp"
#include <expected>
#include <string>
#include <string_view>

// Compiled only when ICLFORGE_BUILD_ADM did NOT turn iclforge::iab/iclforge::admbridge's IAB
// mapping on (see apps/cli/CMakeLists.txt) - see ../atmos_iab.hpp's own top comment for why this
// file, rather than a preprocessor conditional inside main.cpp, is the mechanism. This translation
// unit links neither iclforge::admbridge nor its IAB mapping and includes neither of their headers.

namespace forge_cli {

std::expected<IabAtmosSource, std::string> load_iab_atmos_source(std::string_view /*path*/) {
    // Unreachable in practice: main.cpp's dispatch loop checks adm_capability() (kCommands' same
    // Needs::kAdm row 'atmos-iab' reuses) before ever calling run_atmos_iab, the only caller of
    // this function - see main.cpp's own unmet()/kCommands and ../atmos_iab.hpp's own top comment
    // on why 'atmos-iab' asks the identical question 'atmos-adm' already does. Still a real,
    // defined function rather than an abort() or an unreachable() marker, in case that gate is
    // ever bypassed or this function gains another caller - the same "never silently do nothing"
    // standard this project's other refusal paths hold themselves to.
    return std::unexpected(std::string(
        "this build was not configured with -DICLFORGE_BUILD_ADM=ON "
        "(iclforge::iab's IAB mapping / iclforge::admbridge were not linked in)"));
}

}  // namespace forge_cli
