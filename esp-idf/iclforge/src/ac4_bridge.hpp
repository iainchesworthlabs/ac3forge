#pragma once

// The AC-4 decoder as the player uses it (planning/ac4.md, D14b): what reading
// an AC-4 stream needs that is not the player's own loop. player.cpp includes
// this only when CONFIG_ICLFORGE_AC4 is on, and with it off none of this is in
// the build.
//
// The decoder itself is src/ac4dec's iclforge::ac4::Decoder, in the float scalar, built as
// a static archive with the minimum-footprint profile's compile options
// (ICLFORGE_MINIMAL_AC4, root CMakeLists.txt). It is asked for what a player
// asks of the AC-3 and E-AC-3 decoders: blocks of 256 samples a channel, handed
// to a callback as the decoder completes them (iclforge::ac4::Decoder::decode_by_block),
// so the player holds one block of the audio and not a frame's worth.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/output.hpp"

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"

namespace iclforge::ac4bridge {

// Whether `bytes` opens with an AC-4 sync word, 0xAC40 or 0xAC41 (ETSI TS 103
// 190-2 Annex G.4.1) where AC-3's and E-AC-3's is 0x0B77: how every front end
// that reads a stream decides which decoder reads it (apps/common/
// ac4_sync_word.hpp, which the desktop applications use and this component
// cannot reach, since its archive carries no apps/).
[[nodiscard]] inline bool is_ac4(std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= 2 && std::to_integer<unsigned>(bytes[0]) == 0xACU &&
           (std::to_integer<unsigned>(bytes[1]) & 0xFEU) == 0x40U;
}

// Where an AC-4 speaker is among A/52's Table E2.5 locations, for the layout
// renderer that places a decoded bed on the player's output layout: the same
// reading as apps/common/ac4_channels.hpp's ac4_location(), which the desktop
// applications place a decoded presentation by. Lb and Rb are the rear
// surrounds, Lw and Rw the wides, the top front pair the vertical heights, the
// top back and top side pairs the top surrounds (Table E2.5 has one pair for
// both), and the second LFE LFE2.
[[nodiscard]] inline iclforge::ac3::eac3::chanmap::Location location(iclforge::ac4::Speaker speaker) {
    using L = iclforge::ac3::eac3::chanmap::Location;
    switch (speaker) {
        case iclforge::ac4::Speaker::kLeft:
            return L::kLeft;
        case iclforge::ac4::Speaker::kRight:
            return L::kRight;
        case iclforge::ac4::Speaker::kCentre:
            return L::kCentre;
        case iclforge::ac4::Speaker::kLfe:
            return L::kLfe;
        case iclforge::ac4::Speaker::kLeftSurround:
            return L::kLeftSurround;
        case iclforge::ac4::Speaker::kRightSurround:
            return L::kRightSurround;
        case iclforge::ac4::Speaker::kLeftBack:
            return L::kLrs;
        case iclforge::ac4::Speaker::kRightBack:
            return L::kRrs;
        case iclforge::ac4::Speaker::kLeftWide:
            return L::kLw;
        case iclforge::ac4::Speaker::kRightWide:
            return L::kRw;
        case iclforge::ac4::Speaker::kTopFrontLeft:
            return L::kVhl;
        case iclforge::ac4::Speaker::kTopFrontRight:
            return L::kVhr;
        case iclforge::ac4::Speaker::kTopBackLeft:
        case iclforge::ac4::Speaker::kTopSideLeft:
            return L::kLts;
        case iclforge::ac4::Speaker::kTopBackRight:
        case iclforge::ac4::Speaker::kTopSideRight:
            return L::kRts;
        case iclforge::ac4::Speaker::kLfe2:
            return L::kLfe2;
    }
    return L::kCentre;
}

// The coded layout a decoded block's channels are in, as the renderer takes it:
// each channel's location, in the decoder's order. Channels past what a
// renderer's bed holds are left out.
[[nodiscard]] inline iclforge::ac3::eac3::chanmap::Layout bed(
    std::span<const iclforge::ac4::Speaker> speakers) {
    iclforge::ac3::eac3::chanmap::Layout layout{};
    for (const iclforge::ac4::Speaker speaker : speakers) {
        if (layout.count >= iclforge::ac3::eac3::chanmap::kMaxChannels) {
            break;
        }
        layout.items[static_cast<std::size_t>(layout.count)] = location(speaker);
        ++layout.count;
    }
    return layout;
}

// The decoder's own fold for the player's serving of a stereo or mono layout
// (iclforge::ac3::render::serve): the decoder folds where the player would fold, and
// hands the renderer what it hands it for AC-3, two channels or one.
[[nodiscard]] inline iclforge::ac4::DownmixTarget target(
    std::optional<iclforge::ac3::DownmixTarget> fold) noexcept {
    if (!fold.has_value()) {
        return iclforge::ac4::DownmixTarget::kAsCoded;
    }
    switch (*fold) {
        case iclforge::ac3::DownmixTarget::kLoRo:
            return iclforge::ac4::DownmixTarget::kLoRo;
        case iclforge::ac3::DownmixTarget::kLtRt:
            return iclforge::ac4::DownmixTarget::kLtRt;
        case iclforge::ac3::DownmixTarget::kMono:
            return iclforge::ac4::DownmixTarget::kMono;
        case iclforge::ac3::DownmixTarget::kAsCoded:
            break;
    }
    return iclforge::ac4::DownmixTarget::kAsCoded;
}

// Every delivered sample's bit pattern, in delivery order, through FNV-1a: the
// probe's own hash (apps/baremetal/probe.cpp's PcmHash), so a value printed
// here is comparable with one printed there. Decision 26 of planning/ac4.md
// promises the float tier's output identical on the host, the Cortex-M3 leg and
// the ESP32s, and this is what says whether it is.
struct PcmHash {
    std::uint64_t state = 14695981039346656037ULL;

    void add(std::span<const float> pcm) noexcept {
        for (const float sample : pcm) {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (int shift = 0; shift < 32; shift += 8) {
                state ^= (bits >> shift) & 0xFFU;
                state *= 1099511628211ULL;
            }
        }
    }
};

}  // namespace iclforge::ac4bridge
