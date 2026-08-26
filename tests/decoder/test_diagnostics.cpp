#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/diagnostics.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"

// DecoderConfig::diagnostics (roadmap AP11): a callback for the recoverable,
// informational events a caller otherwise has no way to hear about. Every
// test damages a REAL encoded frame or injects a REAL (if synthetic) EMDF
// payload rather than asserting on the callback mechanism in isolation - the
// whole point of this facility is what it reports about an actual decode.

namespace {

void record(const ac3::Diagnostic& diagnostic, void* context) {
    static_cast<std::vector<ac3::Diagnostic>*>(context)->push_back(diagnostic);
}

std::vector<std::vector<float>> tones(std::span<const double> hz, int samples,
                                      double amplitude = 0.35) {
    std::vector<std::vector<float>> pcm(hz.size(),
                                        std::vector<float>(static_cast<std::size_t>(samples)));
    for (std::size_t ch = 0; ch < hz.size(); ++ch) {
        for (int i = 0; i < samples; ++i) {
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * hz[ch] * i / 48000.0));
        }
    }
    return pcm;
}

// Several frames of real coded AC-3 audio (CONTRIBUTING.md: silence and frame
// 0 give false passes), the third of which gets its payload flipped without
// touching its sync word or declared size - the shape real transport
// corruption takes, and what split_frames still finds the boundary of.
std::vector<std::vector<std::byte>> encode_damaged_ac3() {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k2_0;
    config.bitrate_kbps = 192;
    ac3::FrameEncoder encoder{config};
    const std::array<double, 2> hz = {440.0, 660.0};
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < 5; ++f) {
        const auto pcm = tones(hz, ac3::kSamplesPerFrame);
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    REQUIRE(out[2].size() > 40);
    out[2][out[2].size() / 2] ^= std::byte{0xFF};
    return out;
}

}  // namespace

TEST_CASE("a CRC mismatch reaches the diagnostic sink", "[decoder][diagnostics]") {
    auto frames = encode_damaged_ac3();
    std::vector<ac3::Diagnostic> events;
    ac3::FrameDecoder decoder{{.diagnostics = &record, .diagnostics_context = &events}};

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        if (i == 2) {
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error() == ac3::DecodeError::kBadCrc);
        } else {
            REQUIRE(decoded.has_value());
        }
    }

    REQUIRE(events.size() == 1);
    CHECK(events[0].event == ac3::DiagnosticEvent::kCrcMismatch);
}

TEST_CASE("a CRC mismatch reaches the sink even when concealment hides the error return",
          "[decoder][diagnostics][concealment]") {
    // This is the gap AP11 closes: with concealment on, decode_frame's
    // return value alone no longer says a CRC failure happened at all - see
    // DecodedFrame::concealed, which a caller must poll every frame to find.
    auto frames = encode_damaged_ac3();
    std::vector<ac3::Diagnostic> events;
    ac3::FrameDecoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade,
                              .diagnostics = &record,
                              .diagnostics_context = &events}};

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        CHECK(decoded->concealed.has_value() == (i == 2));
    }

    REQUIRE(events.size() == 1);
    CHECK(events[0].event == ac3::DiagnosticEvent::kCrcMismatch);
}

TEST_CASE("a null diagnostics sink changes nothing", "[decoder][diagnostics]") {
    // The default DecoderConfig - proving the facility is genuinely opt-in,
    // the same guarantee trace/syntax/concealment already give.
    auto frames = encode_damaged_ac3();
    ac3::FrameDecoder decoder;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        CHECK(decoded.has_value() == (i != 2));
    }
}

TEST_CASE("an E-AC-3 CRC mismatch reaches the diagnostic sink", "[eac3][decoder][diagnostics]") {
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(nchans, silence);
    auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());
    REQUIRE(frame->size() > 40);
    (*frame)[frame->size() / 2] ^= std::byte{0xFF};

    std::vector<ac3::Diagnostic> events;
    ac3::Eac3Decoder decoder{{.diagnostics = &record, .diagnostics_context = &events}};
    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == ac3::DecodeError::kBadCrc);

    REQUIRE(events.size() == 1);
    CHECK(events[0].event == ac3::DiagnosticEvent::kCrcMismatch);
}

TEST_CASE("an unrecognised EMDF payload id reaches the diagnostic sink",
          "[eac3][decoder][diagnostics]") {
    // A real EMDF container (ac3::emdf::build_container), carried in the
    // frame's skip field exactly as AtmosEncoder's OAMD/JOC pair are - see
    // AuxPayload's own doc comment - but with a payload id (5) this decoder
    // does not interpret at all. §H.2.2's own design means this never fails
    // the frame; before AP11 nothing anywhere reported it either.
    const std::vector<std::byte> payload_bytes = {std::byte{0xAB}, std::byte{0xCD}};
    const std::vector<ac3::emdf::Payload> payloads = {{.id = 5, .bytes = payload_bytes}};
    const auto container = ac3::emdf::build_container(payloads);

    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(nchans, silence);
    auto frame = encoder.encode_frame(views, container);
    REQUIRE(frame.has_value());

    std::vector<ac3::Diagnostic> events;
    ac3::Eac3Decoder decoder{{.diagnostics = &record, .diagnostics_context = &events}};
    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE(decoded.has_value());  // EMDF never fails the surrounding frame

    REQUIRE(events.size() == 1);
    CHECK(events[0].event == ac3::DiagnosticEvent::kUnknownEmdfPayload);
    CHECK(events[0].emdf_payload_id == 5);
}

TEST_CASE("a recognised EMDF payload id (OAMD/JOC) does not reach the sink",
          "[eac3][decoder][diagnostics]") {
    const std::vector<std::byte> payload_bytes = {std::byte{0x00}, std::byte{0x00}};
    const std::vector<ac3::emdf::Payload> payloads = {
        {.id = ac3::emdf::kPayloadIdOamd, .bytes = payload_bytes}};
    const auto container = ac3::emdf::build_container(payloads);

    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0f);
    const std::vector<std::span<const float>> views(nchans, silence);
    auto frame = encoder.encode_frame(views, container);
    REQUIRE(frame.has_value());

    std::vector<ac3::Diagnostic> events;
    ac3::Eac3Decoder decoder{{.diagnostics = &record, .diagnostics_context = &events}};
    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE(decoded.has_value());

    CHECK(events.empty());
}

TEST_CASE("describe() names every diagnostic event distinctly", "[decoder][diagnostics]") {
    const auto crc = ac3::describe(ac3::DiagnosticEvent::kCrcMismatch);
    const auto emdf = ac3::describe(ac3::DiagnosticEvent::kUnknownEmdfPayload);
    CHECK_FALSE(crc.empty());
    CHECK_FALSE(emdf.empty());
    CHECK(crc != emdf);
}
