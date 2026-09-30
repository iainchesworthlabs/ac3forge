#pragma once

#include <cstddef>
#include <span>

namespace iclforge::apps {

// Whether `bytes` opens with an AC-4 sync word, 0xAC40 or 0xAC41 (ETSI TS 103
// 190-2 Annex G), where AC-3's and E-AC-3's is 0x0B77: how every front end that
// reads a stream decides which decoder reads it, before anything reads it as
// AC-3.
[[nodiscard]] inline bool is_ac4_stream(std::span<const std::byte> bytes) {
    return bytes.size() >= 2 && std::to_integer<unsigned>(bytes[0]) == 0xACU &&
           (std::to_integer<unsigned>(bytes[1]) & 0xFEU) == 0x40U;
}

}  // namespace iclforge::apps
