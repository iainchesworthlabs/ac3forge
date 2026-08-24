#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/atmos.hpp"
#include "real_audio.hpp"

// Real-time throughput regression guard.
//
// AtmosEncoder::encode_frame() once measured at ~266ms per 32ms-budget frame
// on real hardware (an NVIDIA Shield's ARM SoC, building the Android
// platform backend) - traced with Tracy to the forward MDCT recomputing
// std::cos() fresh inside an O(N^2) loop on every call, instead of using a
// precomputed table the way the inverse transform right next to it already
// did (see src/forge/src/core/mdct.cpp's ForwardCosTable). Every other test in
// this suite asserts correctness, not throughput, so nothing would have
// caught a regression like that - this file exists specifically to fail
// loudly if the codec ever stops being faster than real time again.
//
// It covers both directions of both generations plus the object path: the
// AC-3 and Atmos/JOC encoders it was written for, and - since roadmap PF1 -
// the E-AC-3 encoder and all three decoders, which had no real-time gate of
// any kind. A decoder is the half of the codec that runs on the least
// capable hardware there is (a set-top box, a phone, the WASM demo in a
// browser tab), so "faster than real time" is a harder requirement there
// than it is on the encode side, not a softer one.
//
// Every case is fed real programme material (real_audio.hpp - see that
// header for why a single tone is a different workload rather than a
// cheaper one, and in particular why it never exercises the block-switched
// transform at all).
//
// Kept in a separate ac3perf binary/target, not folded into ac3tests: a
// throughput assertion is a different kind of check from the rest of the
// suite (environment-sensitive, meant to be read as a number as much as a
// pass/fail, and not something a correctness-only run should have to carry).
// See tests/performance/CMakeLists.txt.
//
// Not run at all under ASan/UBSan: instrumented code is not a throughput
// signal worth having an opinion about, at any slack factor - see this
// target's LABELS "Performance" and CMakePresets.json's
// test-linux-llvm-asan-ubsan preset, which excludes that label outright.
//
// The threshold is 2x real time, not 1x: real time is the actual functional
// requirement (a live/streaming caller - ac3cli's `live` command, or the
// Shield app's encode loop - cannot keep up otherwise), and 2x leaves
// headroom for a CI runner that is simply slower than a dev machine, without
// giving up on catching the class of regression this guards against - the
// bug it is named for was 8-28x over budget, not a marginal miss.

namespace {

constexpr int kFrames = 100;
constexpr double kSampleRate = 48000.0;
constexpr double kSlackFactor = 2.0;

// Enough frames for a decode case to have something to read, without
// spending the whole encode budget building it.
constexpr int kDecodeSourceFrames = 40;
// The object count every Atmos case here uses, encode and decode alike.
constexpr int kObjects = 4;

double real_time_budget_seconds(int frames) {
    return static_cast<double>(frames) * ac3::kSamplesPerFrame / kSampleRate;
}

const ac3::io::WavData& fixture() {
    static const ac3::io::WavData audio = perf::load_real_audio(
        perf::kReference51Wav, 6, static_cast<std::size_t>(ac3::kSamplesPerFrame));
    return audio;
}

std::vector<ac3::oba::ObjectPlacement> object_placement(int objects) {
    std::vector<ac3::oba::ObjectPlacement> placement(static_cast<std::size_t>(objects));
    for (int obj = 0; obj < objects; ++obj) {
        placement[static_cast<std::size_t>(obj)] = {
            .position = {.x = 0.2 + 0.2 * obj, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }
    return placement;
}

// One AC-3 stream to decode, built outside any timed section.
std::vector<std::byte> ac3_source_stream() {
    perf::FrameSource source{fixture(), perf::kFiveOneChannels};
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .fast_mdct = true}};
    std::vector<std::byte> stream;
    for (int frame = 0; frame < kDecodeSourceFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        REQUIRE(result.has_value());
        stream.insert(stream.end(), result->begin(), result->end());
    }
    return stream;
}

std::vector<std::byte> eac3_source_stream() {
    perf::FrameSource source{fixture(), perf::kFiveOneChannels};
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .auto_tools = true}};
    std::vector<std::byte> stream;
    for (int frame = 0; frame < kDecodeSourceFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        REQUIRE(result.has_value());
        stream.insert(stream.end(), result->begin(), result->end());
    }
    return stream;
}

std::vector<std::byte> atmos_source_stream() {
    perf::FrameSource source{fixture(), perf::kFourObjectChannels};
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
    const auto placement = object_placement(kObjects);
    std::vector<std::byte> stream;
    for (int frame = 0; frame < kDecodeSourceFrames; ++frame) {
        const auto result =
            encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
        REQUIRE(result.has_value());
        stream.insert(stream.end(), result->bytes.begin(), result->bytes.end());
    }
    return stream;
}

// The assertion every case below ends with, so the reported number and the
// threshold cannot drift apart between them.
void check_faster_than_real_time(const std::string& what, double elapsed_seconds, int frames) {
    const double budget = real_time_budget_seconds(frames);
    INFO(what << ": " << frames << " frames in " << elapsed_seconds << "s ("
              << (1000.0 * elapsed_seconds / frames) << "ms/frame); real-time budget is "
              << budget << "s");
    CHECK(elapsed_seconds < kSlackFactor * budget);
}

}  // namespace

TEST_CASE("the plain 5.1 encoder stays faster than real time") {
    perf::FrameSource source{fixture(), perf::kFiveOneChannels};
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};

    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        REQUIRE(result.has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_faster_than_real_time("plain 5.1 encode", elapsed.count(), kFrames);
}

// The E-AC-3 encoder is the largest source file in the codec and, until
// roadmap PF1, the only encoder with no throughput gate at all. `auto_tools`
// rather than a pinned tool set: that is what a stream from this encoder
// normally uses, and it is the setting whose cost moves when a tool's
// rate-crossover heuristic changes.
TEST_CASE("the E-AC-3 5.1 encoder stays faster than real time") {
    perf::FrameSource source{fixture(), perf::kFiveOneChannels};
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .auto_tools = true}};

    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        REQUIRE(result.has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_faster_than_real_time("E-AC-3 5.1 encode", elapsed.count(), kFrames);
}

TEST_CASE("the Atmos/JOC encoder stays faster than real time") {
    perf::FrameSource source{fixture(), perf::kFourObjectChannels};
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
    const auto placement = object_placement(kObjects);

    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result =
            encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
        REQUIRE(result.has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_faster_than_real_time("Atmos/JOC encode", elapsed.count(), kFrames);
}

TEST_CASE("the AC-3 decoder stays faster than real time") {
    const auto stream = ac3_source_stream();
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    ac3::FrameDecoder decoder{};

    const auto start = std::chrono::steady_clock::now();
    for (const auto& frame : *frames) {
        const auto result = decoder.decode_frame(frame);
        REQUIRE(result.has_value());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_faster_than_real_time("AC-3 5.1 decode", elapsed.count(),
                                static_cast<int>(frames->size()));
}

TEST_CASE("the E-AC-3 decoder stays faster than real time") {
    const auto stream = eac3_source_stream();
    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE_FALSE(units->empty());
    ac3::Eac3Decoder decoder{};

    const auto start = std::chrono::steady_clock::now();
    for (const auto& unit : *units) {
        const auto result = decoder.decode_access_unit(unit);
        REQUIRE(result.has_value());
    }
    (void)decoder.flush();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_faster_than_real_time("E-AC-3 5.1 decode", elapsed.count(),
                                static_cast<int>(units->size()));
}

// The object path costs strictly more than the bed alone: on top of the same
// E-AC-3 access unit the case above decodes, this one reads the OAMD/JOC
// object layer and reconstructs every object from it.
TEST_CASE("the Atmos/JOC decoder stays faster than real time") {
    const auto stream = atmos_source_stream();
    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE_FALSE(units->empty());
    ac3::Eac3Decoder decoder{};

    const auto start = std::chrono::steady_clock::now();
    for (const auto& unit : *units) {
        const auto result = decoder.decode_access_unit(unit);
        REQUIRE(result.has_value());
    }
    (void)decoder.flush();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    check_faster_than_real_time("Atmos/JOC 4-object decode", elapsed.count(),
                                static_cast<int>(units->size()));
}
