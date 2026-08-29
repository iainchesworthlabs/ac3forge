// Roadmap IM3 phase 1's decode -> IAMF bridge: E-AC-3 can never be an IAMF codec (IAMF's codec
// list is Opus, AAC-LC, FLAC and LPCM only), so the route to that ecosystem is decode -> rewrap,
// not a new encoder output. This encodes a synthetic 7.1.4 E-AC-3 stream (an independent 3/2+LFE
// bed plus two dependent substreams, exactly examples/encode_eac3.cpp's own encode_714()), decodes
// each access unit back with ac3::Eac3Decoder, permutes the result from Table E2.5's bit order
// into iamf::'s own L,C,R,Lss,Rss,Lrs,Rrs,Ltf,Rtf,Ltb,Rtb,LFE order (IAMF v1.1.0 §3.6.2,
// loudspeaker_layout = 7), and writes it out as an IAMF ISOBMFF file with iamf::mux().
//
// iamf::iamf itself is codec-blind (see iamf/iamf.hpp) - the permutation below is what a caller
// bridging a real decode into it looks like, kept here rather than inside the module for the same
// reason mp4::AudioTrack::codec_config's ETSI TS 102 366 payload is built by the CALLER
// (ac3::io::build_codec_config_box) rather than by mp4:: itself.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fmt/printf.h>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "iamf/iamf.hpp"

namespace {

using ac3::eac3::chanmap::Location;

void fill_tones(std::vector<std::vector<float>>& pcm, std::span<const double> tones, int frame,
                double rate) {
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const double t = (frame * ac3::kSamplesPerFrame + n) / rate;
            pcm[ch][static_cast<std::size_t>(n)] =
                static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * tones[ch] * t));
        }
    }
}

std::vector<std::span<const float>> views_of(const std::vector<std::vector<float>>& pcm) {
    std::vector<std::span<const float>> views;
    views.reserve(pcm.size());
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    return views;
}

// The 12 locations a 7.1.4 access unit renders, in the order iamf::Frame::channels declares
// (IAMF §3.6.2 loudspeaker_layout = 7's own "L/C/R/Lss/Rss/Lrs/Rrs/Ltf/Rtf/Ltb/Rtb/LFE"): Vhl/Vhr
// are Table E2.5's front-height pair (IAMF's Ltf/Rtf) and Lts/Rts its rear-height pair (Ltb/Rtb).
constexpr std::array<Location, 12> kIamf714Order{
    Location::kLeft,          Location::kCentre,  Location::kRight,
    Location::kLeftSurround,  Location::kRightSurround,
    Location::kLrs,           Location::kRrs,
    Location::kVhl,           Location::kVhr,
    Location::kLts,           Location::kRts,
    Location::kLfe,
};

iamf::Frame to_iamf_frame(const ac3::DecodedAccessUnit& decoded) {
    iamf::Frame frame;
    for (std::size_t i = 0; i < kIamf714Order.size(); ++i) {
        const int index = decoded.layout.index_of(kIamf714Order[i]);
        frame.channels[i] = decoded.channels[static_cast<std::size_t>(index)];
    }
    return frame;
}

}  // namespace

int main() {
    // Same 7.1.4 access-unit shape as examples/encode_eac3.cpp's own encode_714(): a
    // self-sufficient 3/2+LFE bed plus two dependents (§E3.8.2's chanmap collisions with the bed
    // extend it to a full 7.1.4 render rather than replacing it outright).
    ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 384, .acmod = ac3::Acmod::k3_2, .lfe = true};
    config.dependents.push_back(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_2, .chanmap = ac3::eac3::chanmap::k71Rear});
    config.dependents.push_back(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_2, .chanmap = ac3::eac3::chanmap::kTopQuad});
    ac3::eac3::AccessUnitEncoder encoder{config};

    const auto channel_count = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<float>> pcm(channel_count, std::vector<float>(ac3::kSamplesPerFrame));
    const auto views = views_of(pcm);
    const std::vector<double> tones{1000.0, 800.0,  1200.0, 600.0,  1400.0, 60.0,
                                    500.0,  1600.0, 400.0,  1800.0, 2000.0, 2400.0,
                                    2800.0, 3200.0};

    ac3::Eac3Decoder decoder;
    iamf::AudioTrack track{.samples_per_frame = static_cast<std::uint32_t>(ac3::kSamplesPerFrame)};
    std::vector<iamf::Frame> frames;

    constexpr int kFrameCount = 8;  // several frames of real content, not silence - see
                                    // CONTRIBUTING.md's "test with real audio" section
    for (int frame = 0; frame < kFrameCount; ++frame) {
        fill_tones(pcm, tones, frame, 48000.0);
        const auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            fmt::printf("7.1.4 encode failed: %d\n", std::to_underlying(unit.error()));
            return 1;
        }
        const auto decoded = decoder.decode_access_unit(unit->bytes);
        if (!decoded) {
            fmt::printf("decode failed: %d\n", std::to_underlying(decoded.error()));
            return 1;
        }
        if (!decoded->has_value()) {
            fmt::printf("decode held back frame %d unexpectedly\n", frame);
            return 1;
        }
        frames.push_back(to_iamf_frame(**decoded));
    }

    const auto file = iamf::mux(track, frames);
    if (!file) {
        fmt::printf("iamf::mux failed: %.*s\n", static_cast<int>(iamf::describe(file.error()).size()),
                    iamf::describe(file.error()).data());
        return 1;
    }
    fmt::printf("iamf: %d frames, %zu channels, %zu bytes\n", kFrameCount, channel_count,
                file->size());
    return 0;
}
