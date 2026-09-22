#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/render/layout.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"

// ac3::hearth::Session (apps/hearth/engine/session.cpp) against a stream whose
// decoder runs a frame behind: what a handover to a new decoder has to
// release rather than drop.

namespace {

using ac3::hearth::ItemLoader;
using ac3::hearth::LoadedItem;
using ac3::hearth::Session;
using ac3::hearth::StreamDecoder;

constexpr int kFrames = 12;

// Stereo with transient pre-noise processing (§3.7): silence, a sharp onset
// late in frame 2, and a tone after it. Once the tool engages, the decoder
// holds each unit back until the next one arrives.
std::vector<std::vector<std::byte>> held_back_frames() {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    config.transient_prenoise = true;
    config.dither = false;
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> samples(ac3::kSamplesPerFrame, 0.0F);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const bool onset = f == 2 && n >= 960;
            if (onset || f > 2) {
                samples[n] = static_cast<float>(
                    0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                   static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) /
                                   48000.0));
            }
        }
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::byte> joined(const std::vector<std::vector<std::byte>>& frames) {
    std::vector<std::byte> out;
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

}  // namespace

TEST_CASE("session: a handover releases the unit the old decoder was holding, and loses nothing",
          "[hearth][session]") {
    const auto frames = held_back_frames();
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());

    // The premise, checked on its own: from some unit on, each decode hands
    // over nothing of its own and the previous unit comes out instead.
    StreamDecoder probe{*layout, 48000};
    std::vector<std::size_t> per_call;
    const auto count = [](std::span<const std::span<const float>>, std::size_t) {};
    for (const auto& frame : frames) {
        const auto got = probe.decode(frame, count);
        REQUIRE(got.has_value());
        per_call.push_back(*got);
    }
    const std::size_t released_at_end = probe.finish(count);
    REQUIRE(per_call[2] == 0);
    REQUIRE(released_at_end == ac3::kSamplesPerFrame);

    const std::vector<std::byte> stream = joined(frames);
    const ItemLoader loader = [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.bytes = stream};
    };
    auto session = Session::open("held", loader);
    REQUIRE(session.has_value());

    std::uint64_t delivered = 0;
    const StreamDecoder::BlockFn deliver = [&delivered](std::span<const std::span<const float>>,
                                                        std::size_t n) { delivered += n; };
    std::optional<StreamDecoder> decoder;
    decoder.emplace(*layout, 48000);

    // Six frames' worth: past the transient, so the decoder is a unit behind
    // and holding the last one it decoded.
    const auto first = session->render(*decoder, deliver, 6 * ac3::kSamplesPerFrame);
    REQUIRE(first.has_value());
    const std::uint64_t before_handover = delivered;

    session->hand_over(*decoder, deliver);
    // The held unit came out through the handover.
    CHECK(delivered == before_handover + ac3::kSamplesPerFrame);
    decoder.emplace(*layout, 48000);

    while (!session->finished()) {
        REQUIRE(session->render(*decoder, deliver, 4 * ac3::kSamplesPerFrame).has_value());
    }
    // Every frame of the stream, once.
    CHECK(delivered == static_cast<std::uint64_t>(kFrames) * ac3::kSamplesPerFrame);
    CHECK(session->position_samples() == session->total_samples());
}

TEST_CASE("session: each unit the item plays is reported with its frames, and no other",
          "[hearth][session]") {
    // The same stream a frame behind, with the start and the end trimmed off.
    const std::vector<std::byte> stream = joined(held_back_frames());
    constexpr std::uint64_t kTotal = static_cast<std::uint64_t>(kFrames) * ac3::kSamplesPerFrame;
    const ItemLoader loader = [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.bytes = stream, .skip_samples = 700, .play_samples = kTotal - 700 - 1000};
    };
    auto session = Session::open("trimmed", loader);
    REQUIRE(session.has_value());
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    std::uint64_t delivered = 0;
    std::vector<std::size_t> reported;
    const StreamDecoder::BlockFn deliver = [&delivered](std::span<const std::span<const float>>,
                                                        std::size_t n) { delivered += n; };
    const Session::ReportFn report = [&reported](const ac3::hearth::UnitReport&, std::size_t frames) {
        reported.push_back(frames);
    };
    const auto play_to_end = [&] {
        while (!session->finished()) {
            REQUIRE(session->render(decoder, deliver, 4 * ac3::kSamplesPerFrame, report).has_value());
        }
    };

    play_to_end();
    CHECK(delivered == session->total_samples());
    // Every unit plays some of its frames, the first and the last only part.
    REQUIRE(reported.size() == static_cast<std::size_t>(kFrames));
    CHECK(reported.front() == static_cast<std::size_t>(ac3::kSamplesPerFrame) - 700);
    CHECK(reported.back() == static_cast<std::size_t>(ac3::kSamplesPerFrame) - 1000);
    std::uint64_t sum = 0;
    for (const std::size_t frames : reported) {
        sum += frames;
    }
    CHECK(sum == delivered);

    // After a seek, the unit decoded only to prime the decoder plays nothing,
    // so it is not reported.
    session->seek(std::chrono::milliseconds{200}, decoder);
    delivered = 0;
    reported.clear();
    play_to_end();
    CHECK(reported.size() == 6);
    sum = 0;
    for (const std::size_t frames : reported) {
        CHECK(frames > 0);
        sum += frames;
    }
    CHECK(sum == delivered);
}
