#include "stream_playback.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"

namespace ac3::apps {

namespace {

namespace chanmap = ac3::eac3::chanmap;

[[nodiscard]] bool is_dependent(const ac3::DecodedSubstream& sub) {
    return sub.strmtyp == ac3::eac3::StreamType::kDependent;
}

// §E3.8.2 for one substream: each of its channels into the slot its Table
// E2.5 location occupies in `layout`, over whatever an earlier substream put
// there. A location the layout does not have is skipped, and a channel longer
// than the unit is cut to it - `slots` are already sized to the unit.
void lay_over(const chanmap::Layout& layout, const ac3::DecodedSubstream& sub,
              std::vector<std::vector<float>>& slots) {
    const auto locations = chanmap::expand(sub.location_map());
    const auto count = std::min(static_cast<std::size_t>(locations.count), sub.channels.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int slot = layout.index_of(locations[static_cast<int>(i)]);
        if (slot < 0) {
            continue;
        }
        const auto& src = sub.channels[i];
        auto& dst = slots[static_cast<std::size_t>(slot)];
        std::copy_n(src.begin(), std::min(src.size(), dst.size()), dst.begin());
    }
}

}  // namespace

bool reads_as_access_units(std::span<const std::byte> stream) {
    const auto bsid = ac3::stream_bsid(stream);
    return bsid.has_value() && (*bsid > 8 || ac3::has_eac3_extension_substreams(stream));
}

std::optional<ac3::DecodedAccessUnit> held_back_unit(
    std::vector<ac3::DecodedSubstream> flushed,
    const std::optional<ac3::eac3::chanmap::Layout>& programme, bool folded) {
    // decode_access_unit leads a unit with whatever substream is not a
    // dependent, §E1.3.1's convertible kind included.
    const auto lead = std::ranges::find_if(flushed, [](const ac3::DecodedSubstream& sub) {
        return !is_dependent(sub);
    });
    if (lead == flushed.end() || lead->channels.empty()) {
        return std::nullopt;
    }

    // The fields decode_access_unit fills from its lead, from this one.
    ac3::DecodedAccessUnit out;
    out.sample_rate = lead->sample_rate;
    out.acmod = lead->acmod;
    out.dialnorm = lead->dialnorm;
    out.dialnorm2 = lead->dialnorm2;
    out.compr = lead->compr;
    out.dynrng = lead->dynrng;
    out.numblkscod = lead->numblkscod;
    out.mixing = lead->mixing;
    out.bsid = lead->bsid;
    out.cmixlev = lead->cmixlev;
    out.surmixlev = lead->surmixlev;
    out.alternate_bsi = lead->alternate_bsi;
    out.info = lead->info;
    out.programme = lead->substreamid;
    out.substream_count = static_cast<int>(flushed.size());
    // The object layer and a concealment note each come from the first
    // substream that has one, bed before dependents - decode_access_unit's
    // rule, in its order rather than flush()'s.
    const auto take_from = [&out](ac3::DecodedSubstream& sub) {
        if (!out.object_metadata.has_value() && sub.object_metadata.has_value()) {
            out.object_metadata = std::move(sub.object_metadata);
            out.object_audio = std::move(sub.object_audio);
            out.object_indices = std::move(sub.object_indices);
        }
        if (!out.concealed.has_value()) {
            out.concealed = sub.concealed;
        }
    };
    take_from(*lead);
    for (auto& sub : flushed) {
        if (is_dependent(sub)) {
            take_from(sub);
        }
    }

    if (lead->acmod == ac3::Acmod::kDualMono) {
        out.channels = std::move(lead->channels);
        return out;
    }
    std::uint16_t occupied = 0;
    for (const auto& sub : flushed) {
        occupied = static_cast<std::uint16_t>(occupied | sub.location_map());
    }
    out.layout = programme.value_or(chanmap::expand(occupied));
    if (folded) {
        out.channels = std::move(lead->channels);
        return out;
    }
    out.channels.assign(static_cast<std::size_t>(out.layout.count),
                        std::vector<float>(lead->channels.front().size(), 0.0F));
    lay_over(out.layout, *lead, out.channels);
    for (const auto& sub : flushed) {
        if (is_dependent(sub)) {
            lay_over(out.layout, sub, out.channels);
        }
    }
    return out;
}

}  // namespace ac3::apps
