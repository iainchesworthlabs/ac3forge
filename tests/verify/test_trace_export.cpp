#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"
#include "ac3/verify/trace_export.hpp"

// Roadmap AP12: research trace export. Two things are checked, separately:
//
//  - The serializer (append_trace_csv/append_trace_json_lines) reads a
//    FrameTrace/Eac3AccessUnitTrace correctly. Hand-built traces with known
//    values prove this exactly, byte for byte - no decoder involved.
//  - The DECODER actually populates StreamTrace::mask/snr_offset (roadmap
//    AP12's own addition to a trace that, before this, only ever carried
//    exponents/bap/delta) from a REAL decode, and does so without disturbing
//    the bap array the mantissas themselves depend on - proven by decoding
//    the same real frame with and without a trace attached and requiring
//    byte-identical PCM.

namespace {

ac3::verify::StreamTrace stream_with(std::vector<std::uint8_t> exponents,
                                     std::vector<std::uint8_t> bap, int snr_offset) {
    ac3::verify::StreamTrace stream;
    stream.exponents = std::move(exponents);
    stream.bap = std::move(bap);
    stream.snr_offset = snr_offset;
    for (std::size_t band = 0; band < stream.mask.size(); ++band) {
        stream.mask[band] = static_cast<int>(band) * 10;
    }
    return stream;
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
// 0 give false passes).
std::vector<std::vector<std::byte>> encode_real_ac3() {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k2_0;
    config.bitrate_kbps = 192;
    ac3::FrameEncoder encoder{config};
    const std::array<double, 2> hz = {440.0, 660.0};
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < 3; ++f) {
        const auto pcm = tones(hz, ac3::kSamplesPerFrame);
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

bool any_mask_populated(const ac3::verify::FrameTrace& trace) {
    for (const auto& block : trace.blocks) {
        if (!block.allocated) {
            continue;
        }
        for (const auto& stream : block.streams) {
            if (std::ranges::any_of(stream.mask, [](int m) { return m != 0; })) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("trace_csv_header names every column", "[verify][trace_export]") {
    CHECK(ac3::verify::trace_csv_header() == "frame,substream,block,stream,kind,index,value\n");
}

TEST_CASE("append_trace_csv writes exactly the rows a hand-built trace holds",
          "[verify][trace_export]") {
    ac3::verify::FrameTrace trace;
    trace.blocks[0].entered = true;
    trace.blocks[0].allocated = true;
    trace.blocks[0].streams.push_back(stream_with({5, 6}, {1, 2}, 42));

    std::string out;
    ac3::verify::append_trace_csv(trace, 7, out);

    std::string expected;
    expected += "7,0,0,0,exponent,0,5\n";
    expected += "7,0,0,0,exponent,1,6\n";
    expected += "7,0,0,0,bap,0,1\n";
    expected += "7,0,0,0,bap,1,2\n";
    for (int band = 0; band < 50; ++band) {
        expected += "7,0,0,0,mask," + std::to_string(band) + "," + std::to_string(band * 10) + "\n";
    }
    expected += "7,0,0,0,snr_offset,0,42\n";
    CHECK(out == expected);
}

TEST_CASE("append_trace_json_lines writes exactly the rows a hand-built trace holds",
          "[verify][trace_export]") {
    ac3::verify::FrameTrace trace;
    trace.blocks[0].entered = true;
    trace.blocks[0].allocated = true;
    trace.blocks[0].streams.push_back(stream_with({5}, {1}, 42));

    std::string out;
    ac3::verify::append_trace_json_lines(trace, 3, out);

    std::string expected;
    expected += R"({"frame":3,"substream":0,"block":0,"stream":0,"kind":"exponent","index":0,"value":5})"
               "\n";
    expected += R"({"frame":3,"substream":0,"block":0,"stream":0,"kind":"bap","index":0,"value":1})"
               "\n";
    for (int band = 0; band < 50; ++band) {
        expected += R"({"frame":3,"substream":0,"block":0,"stream":0,"kind":"mask","index":)" +
                   std::to_string(band) + R"(,"value":)" + std::to_string(band * 10) + "}\n";
    }
    expected +=
        R"({"frame":3,"substream":0,"block":0,"stream":0,"kind":"snr_offset","index":0,"value":42})"
        "\n";
    CHECK(out == expected);
}

TEST_CASE("append_trace_csv skips a block the decoder never allocated",
          "[verify][trace_export]") {
    // Default-constructed: every block's `allocated` is false, matching a
    // frame the decoder refused before running the bit allocation at all.
    const ac3::verify::FrameTrace trace;
    std::string out;
    ac3::verify::append_trace_csv(trace, 0, out);
    CHECK(out.empty());
}

TEST_CASE("append_trace_csv numbers E-AC-3 substreams by access-unit position",
          "[verify][trace_export]") {
    ac3::verify::Eac3AccessUnitTrace trace;
    trace.resize(2);
    trace.substream(0).blocks[0].entered = true;
    trace.substream(0).blocks[0].allocated = true;
    ac3::verify::Eac3StreamTrace s0;
    s0.exponents = {1};
    s0.bap = {2};
    trace.substream(0).blocks[0].streams.push_back(s0);
    trace.substream(1).blocks[0].entered = true;
    trace.substream(1).blocks[0].allocated = true;
    ac3::verify::Eac3StreamTrace s1;
    s1.exponents = {9};
    s1.bap = {8};
    trace.substream(1).blocks[0].streams.push_back(s1);

    std::string out;
    ac3::verify::append_trace_csv(trace, 0, out);
    CHECK(out.find("0,0,0,0,exponent,0,1\n") != std::string::npos);
    CHECK(out.find("0,1,0,0,exponent,0,9\n") != std::string::npos);
}

TEST_CASE("AC-3: attaching a trace does not change the decoded audio",
          "[decoder][verify][trace_export]") {
    // The traced bit-allocation path (ac3::internal::compute_bit_allocation_traced)
    // is a second entry point onto the SAME routine the plain, untraced
    // decode uses (see bitalloc_internal.hpp) - this is the black-box proof
    // that capturing the masking curve alongside it changes nothing about
    // what gets decoded.
    auto frames = encode_real_ac3();
    ac3::FrameDecoder plain;
    ac3::verify::FrameTrace trace;
    ac3::FrameDecoder traced{{.trace = &trace}};

    for (const auto& frame : frames) {
        const auto a = plain.decode_frame(frame);
        const auto b = traced.decode_frame(frame);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(a->channels == b->channels);
    }
    CHECK(any_mask_populated(trace));
}

TEST_CASE("AC-3: a real decode's trace exports one row per bin/band/scalar",
          "[decoder][verify][trace_export]") {
    auto frames = encode_real_ac3();
    ac3::verify::FrameTrace trace;
    ac3::FrameDecoder decoder{{.trace = &trace}};
    REQUIRE(decoder.decode_frame(frames[0]).has_value());

    std::size_t expected_rows = 0;
    for (const auto& block : trace.blocks) {
        if (!block.allocated) {
            continue;
        }
        for (const auto& stream : block.streams) {
            expected_rows += stream.exponents.size() + stream.bap.size() + stream.mask.size() + 1;
        }
    }
    REQUIRE(expected_rows > 0);

    std::string csv;
    ac3::verify::append_trace_csv(trace, 0, csv);
    const auto csv_rows = static_cast<std::size_t>(std::ranges::count(csv, '\n'));
    CHECK(csv_rows == expected_rows);

    std::string json;
    ac3::verify::append_trace_json_lines(trace, 0, json);
    const auto json_rows = static_cast<std::size_t>(std::ranges::count(json, '\n'));
    CHECK(json_rows == expected_rows);
}

TEST_CASE("E-AC-3: attaching a trace does not change the decoded audio",
          "[eac3][decoder][verify][trace_export]") {
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    const std::array<double, 2> hz = {440.0, 660.0};
    std::vector<std::vector<std::byte>> frames;
    for (int f = 0; f < 3; ++f) {
        const auto pcm = tones(hz, ac3::kSamplesPerFrame);
        std::vector<std::span<const float>> views(pcm.begin(), pcm.end());
        REQUIRE(views.size() == nchans);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }

    ac3::Eac3Decoder plain;
    ac3::verify::Eac3AccessUnitTrace trace;
    ac3::Eac3Decoder traced{{.eac3_trace = &trace}};

    bool mask_populated = false;
    for (const auto& frame : frames) {
        const auto a = plain.decode_substream(frame);
        const auto b = traced.decode_substream(frame);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(a->has_value());
        REQUIRE(b->has_value());
        CHECK((*a)->channels == (*b)->channels);
    }
    for (const auto& substream : trace.substreams()) {
        for (const auto& block : substream.blocks) {
            if (!block.allocated) {
                continue;
            }
            for (const auto& stream : block.streams) {
                mask_populated = mask_populated ||
                                std::ranges::any_of(stream.mask, [](int m) { return m != 0; });
            }
        }
    }
    CHECK(mask_populated);
}
