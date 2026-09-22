#include "../atmos_iab.hpp"

#include <cstddef>
#include <expected>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/admbridge/iab_bridge.hpp"
#include "ac3iab/ac3iab.hpp"
#include "ac3iab/mxf.hpp"

// Compiled only when AC3FORGE_BUILD_ADM turned ac3iab::ac3iab/ac3::admbridge on (see
// apps/cli/CMakeLists.txt) - see ../atmos_iab.hpp's own top comment for why this file, rather than
// a preprocessor conditional inside main.cpp, is the mechanism.

namespace ac3cli {

namespace {

// Sniffs the first byte to choose which of ac3iab::ac3iab's two container readers applies,
// avoiding a fragile try-then-fallback double parse: a real MXF file's first KLV Key always opens
// with SMPTE ST 377-1 Table 4's fixed Object Identifier byte (06h - "06.0e.2b.34..."), while an
// elementary IABitstream's own Preamble segment opens with PreambleTag (01h, SMPTE ST 2098-2
// §8.1.1) - the two are unambiguous by construction, not merely by convention, since a Preamble
// segment beginning 06h or an MXF file beginning 01h would each already be malformed against its
// own governing standard.
[[nodiscard]] std::expected<std::vector<ac3iab::IABitstreamFrame>, std::string> parse_iab_source(
    std::string_view path) {
    std::ifstream probe(std::string(path), std::ios::binary);
    if (!probe) {
        return std::unexpected(std::string(ac3iab::describe(ac3iab::IabError::kCannotOpen)));
    }
    const int first_byte = probe.get();
    probe.close();

    if (first_byte == 0x06) {
        auto frames = ac3iab::parse_mxf_iab(std::string(path));
        if (!frames) {
            return std::unexpected(std::string(ac3iab::describe(frames.error())));
        }
        return std::move(*frames);
    }

    auto frames = ac3iab::parse_iabitstream(std::string(path));
    if (!frames) {
        return std::unexpected(std::string(ac3iab::describe(frames.error())));
    }
    return std::move(*frames);
}

}  // namespace

std::expected<IabAtmosSource, std::string> load_iab_atmos_source(std::string_view path) {
    auto frames = parse_iab_source(path);
    if (!frames) {
        return std::unexpected(frames.error());
    }

    // Placed on the heap (not a stack local) before the caller reads IabAtmosSource::pcm - those
    // spans borrow straight out of this IabBridgeResult's own OWNED storage (see
    // ac3/admbridge/iab_bridge.hpp's own top comment on why, unlike ADM's BridgeResult), and
    // IabAtmosSource::handle has to keep this exact object alive for as long as the caller keeps
    // reading them.
    auto bridged = std::make_shared<ac3::admbridge::IabBridgeResult>();
    auto built = ac3::admbridge::build_iab(*frames);
    if (!built) {
        return std::unexpected(std::string(ac3::admbridge::describe(built.error())));
    }
    *bridged = std::move(*built);

    IabAtmosSource out;
    out.sample_rate = bridged->sample_rate;
    out.is_bed = bridged->is_bed;
    out.paths = std::move(bridged->paths);
    out.pcm.reserve(bridged->pcm.size());
    for (const auto& channel : bridged->pcm) {
        out.pcm.emplace_back(channel);
    }
    out.handle = std::move(bridged);  // shared_ptr<IabBridgeResult> -> shared_ptr<void>
    return out;
}

}  // namespace ac3cli
