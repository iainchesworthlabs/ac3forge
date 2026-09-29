#pragma once

// The AC-4 decoder as the player uses it (planning/ac4.md, D14b): what reading
// an AC-4 stream needs that is not the player's own loop. player.cpp includes
// this only when CONFIG_AC3FORGE_AC4 is on, and with it off none of this is in
// the build.
//
// The decoder itself is src/ac4dec's ac4::Decoder, in the float scalar, built as
// a static archive with the minimum-footprint profile's compile options
// (AC3FORGE_MINIMAL_AC4, root CMakeLists.txt). It is asked for what a player
// asks of the AC-3 and E-AC-3 decoders: blocks of 256 samples a channel, handed
// to a callback as the decoder completes them (ac4::Decoder::decode_by_block),
// so the player holds one block of the audio and not a frame's worth.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/output.hpp"

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

namespace ac3forge::ac4bridge {

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
[[nodiscard]] inline ac3::eac3::chanmap::Location location(ac4::Speaker speaker) {
    using L = ac3::eac3::chanmap::Location;
    switch (speaker) {
        case ac4::Speaker::kLeft:
            return L::kLeft;
        case ac4::Speaker::kRight:
            return L::kRight;
        case ac4::Speaker::kCentre:
            return L::kCentre;
        case ac4::Speaker::kLfe:
            return L::kLfe;
        case ac4::Speaker::kLeftSurround:
            return L::kLeftSurround;
        case ac4::Speaker::kRightSurround:
            return L::kRightSurround;
        case ac4::Speaker::kLeftBack:
            return L::kLrs;
        case ac4::Speaker::kRightBack:
            return L::kRrs;
        case ac4::Speaker::kLeftWide:
            return L::kLw;
        case ac4::Speaker::kRightWide:
            return L::kRw;
        case ac4::Speaker::kTopFrontLeft:
            return L::kVhl;
        case ac4::Speaker::kTopFrontRight:
            return L::kVhr;
        case ac4::Speaker::kTopBackLeft:
        case ac4::Speaker::kTopSideLeft:
            return L::kLts;
        case ac4::Speaker::kTopBackRight:
        case ac4::Speaker::kTopSideRight:
            return L::kRts;
        case ac4::Speaker::kLfe2:
            return L::kLfe2;
    }
    return L::kCentre;
}

// The coded layout a decoded block's channels are in, as the renderer takes it:
// each channel's location, in the decoder's order. Channels past what a
// renderer's bed holds are left out.
[[nodiscard]] inline ac3::eac3::chanmap::Layout bed(std::span<const ac4::Speaker> speakers) {
    ac3::eac3::chanmap::Layout layout{};
    for (const ac4::Speaker speaker : speakers) {
        if (layout.count >= ac3::eac3::chanmap::kMaxChannels) {
            break;
        }
        layout.items[static_cast<std::size_t>(layout.count)] = location(speaker);
        ++layout.count;
    }
    return layout;
}

// The decoder's own fold for the player's serving of a stereo or mono layout
// (ac3::render::serve): the decoder folds where the player would fold, and
// hands the renderer what it hands it for AC-3, two channels or one.
[[nodiscard]] inline ac4::DownmixTarget target(std::optional<ac3::DownmixTarget> fold) noexcept {
    if (!fold.has_value()) {
        return ac4::DownmixTarget::kAsCoded;
    }
    switch (*fold) {
        case ac3::DownmixTarget::kLoRo:
            return ac4::DownmixTarget::kLoRo;
        case ac3::DownmixTarget::kLtRt:
            return ac4::DownmixTarget::kLtRt;
        case ac3::DownmixTarget::kMono:
            return ac4::DownmixTarget::kMono;
        case ac3::DownmixTarget::kAsCoded:
            break;
    }
    return ac4::DownmixTarget::kAsCoded;
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

}  // namespace ac3forge::ac4bridge
