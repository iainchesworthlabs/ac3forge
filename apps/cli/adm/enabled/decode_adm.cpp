#include "../decode_adm.hpp"

#include <utility>
#include <vector>

#include "ac3/admbridge/bridge.hpp"
#include "ac3adm/ac3adm.hpp"

// Compiled only when AC3FORGE_BUILD_ADM turned ac3adm::ac3adm/ac3::admbridge on (see
// apps/cli/CMakeLists.txt) - see ../decode_adm.hpp's own top comment for why this file, rather
// than a preprocessor conditional inside decode.cpp, is the mechanism.

namespace ac3cli {

std::expected<void, std::string> write_adm_atmos_master(std::string_view path, const AdmMasterInput& input) {
    ac3::admbridge::WriteInput bridged;
    bridged.sample_rate = input.sample_rate;
    bridged.channels.reserve(input.channels.size());

    // `updates` holds ac3::admbridge::WriteObjectUpdate by value for as long as `bridged` is
    // alive - WriteChannel::updates is only a span, so the vectors it borrows from have to
    // outlive the ac3::admbridge::write() call below.
    std::vector<std::vector<ac3::admbridge::WriteObjectUpdate>> updates_storage;
    updates_storage.reserve(input.channels.size());

    for (const auto& channel : input.channels) {
        auto& stored_updates = updates_storage.emplace_back();
        stored_updates.reserve(channel.updates.size());
        for (const auto& update : channel.updates) {
            stored_updates.push_back({.sample_offset = update.sample_offset,
                                      .ramp_duration_samples = update.ramp_duration_samples,
                                      .state = update.state});
        }
        bridged.channels.push_back({.name = channel.name,
                                    .pcm = channel.pcm,
                                    .bed_label = channel.bed_label,
                                    .updates = stored_updates});
    }

    auto document = ac3::admbridge::write(bridged);
    if (!document) {
        return std::unexpected(std::string(ac3::admbridge::describe(document.error())));
    }

    auto written = ac3adm::write_bw64(std::string{path}, *document);
    if (!written) {
        return std::unexpected(std::string(ac3adm::describe(written.error())));
    }
    return {};
}

}  // namespace ac3cli
