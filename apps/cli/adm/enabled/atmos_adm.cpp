#include "../atmos_adm.hpp"

#include <utility>

#include "iclforge/admbridge/bridge.hpp"
#include "iclforge/adm/ac3adm.hpp"

// Compiled only when AC3FORGE_BUILD_ADM turned iclforge::adm/iclforge::admbridge on (see
// apps/cli/CMakeLists.txt) - see ../atmos_adm.hpp's own top comment for why this file, rather than
// a preprocessor conditional inside main.cpp, is the mechanism.

namespace ac3cli {

const iclforge::audio::Capability& adm_capability() {
    static constexpr iclforge::audio::Capability kAvailable{.available = true, .reason = {}};
    return kAvailable;
}

std::expected<AdmAtmosSource, std::string> load_adm_atmos_source(std::string_view path,
                                                                  std::string_view programme_id) {
    auto parsed = iclforge::adm::parse_bw64(std::string{path});
    if (!parsed) {
        return std::unexpected(std::string(iclforge::adm::describe(parsed.error())));
    }

    // Placed on the heap (not a stack local) before build() runs: BridgeResult::pcm borrows
    // spans straight out of the AdmDocument passed to build(), and AdmAtmosSource::handle has to
    // keep that exact object alive for as long as this function's caller keeps reading them -
    // building the spans against anything but their final, stable address would leave them
    // dangling the moment this function returns.
    auto document = std::make_shared<iclforge::adm::AdmDocument>(std::move(*parsed));

    auto bridged = iclforge::admbridge::build(*document, programme_id);
    if (!bridged) {
        return std::unexpected(std::string(iclforge::admbridge::describe(bridged.error())));
    }

    AdmAtmosSource out;
    out.sample_rate = bridged->sample_rate;
    out.is_bed = std::move(bridged->is_bed);
    out.paths = std::move(bridged->paths);
    out.pcm = std::move(bridged->pcm);
    out.handle = std::move(document);  // shared_ptr<AdmDocument> -> shared_ptr<void>
    return out;
}

}  // namespace ac3cli
