#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/io/elementary.hpp"

// Two things a player that is handed whole access units needs before it
// decodes them, as the Sendspin burst player is (burst_player.hpp): where one
// unit ends and the next begins inside a burst, and which channels a unit will
// decode to.
//
// The same arithmetic the test sink's burst output uses on a computer
// (apps/hearth/testsink/burst_output.cpp), for the same reasons, so that a
// board and a test sink place a stream's channels alike.

namespace ac3forge {

// The layout an access unit decodes to, from its headers alone: each
// syncframe's acmod and lfeon, and a dependent's chanmap where it carries one,
// unioned in Table E2.5 order as the decoder assembles it (§E3.8.2). Needed
// before the decode, since the block form hands the samples over during the
// call and the layout only after it.
[[nodiscard]] inline std::optional<ac3::eac3::chanmap::Layout> peek_unit_layout(std::span<const std::byte> unit) {
    std::uint16_t map = 0;
    std::size_t offset = 0;
    while (offset < unit.size()) {
        const auto header = ac3::io::read_frame_header(unit.subspan(offset));
        if (!header || header->bytes == 0) {
            return std::nullopt;
        }
        const std::uint16_t own = ac3::eac3::chanmap::acmod_map(header->acmod, header->lfe);
        if (header->kind == ac3::io::StreamKind::kEac3 && header->strmtyp == ac3::eac3::StreamType::kDependent) {
            map = static_cast<std::uint16_t>(map | header->chanmap.value_or(own));
        } else {
            map = static_cast<std::uint16_t>(map | own);
        }
        offset += header->bytes;
    }
    if (map == 0) {
        return std::nullopt;
    }
    return ac3::eac3::chanmap::expand(map);
}

// Calls `unit` with each whole access unit of `payload`, in order: each starts
// at an independent substream or an AC-3 syncframe and takes the dependents
// after it. False, having called nothing, when a syncframe does not parse or
// runs past the payload's end.
template <class Unit>
[[nodiscard]] bool for_each_access_unit(std::span<const std::byte> payload, Unit&& unit) {
    // Checked whole before anything is handed over, so a damaged burst is
    // refused rather than half decoded.
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto header = ac3::io::read_frame_header(payload.subspan(offset));
        if (!header || header->bytes == 0 || header->bytes > payload.size() - offset) {
            return false;
        }
        offset += header->bytes;
    }
    std::size_t start = 0;
    offset = 0;
    while (offset < payload.size()) {
        const auto header = ac3::io::read_frame_header(payload.subspan(offset));
        const bool dependent =
            header->kind == ac3::io::StreamKind::kEac3 && header->strmtyp == ac3::eac3::StreamType::kDependent;
        if (!dependent && offset > start) {
            unit(payload.subspan(start, offset - start));
            start = offset;
        }
        offset += header->bytes;
    }
    if (offset > start) {
        unit(payload.subspan(start, offset - start));
    }
    return true;
}

}  // namespace ac3forge
