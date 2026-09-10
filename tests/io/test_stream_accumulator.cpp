// ac3::io::AccessUnitAccumulator - the incremental form of split_access_units.
//
// Every case here tests one property: feeding a stream through the accumulator
// in arbitrarily small pieces must produce exactly the units
// split_access_units() produces from the whole buffer at once. That is the
// contract, and it is what makes the accumulator safe to substitute for the
// span-at-once API on a target that cannot hold the span.
//
// The chunk sizes are the point of most of these. One byte at a time exercises
// every "need more input" path; a size that is not a divisor of the frame size
// lands the boundary in a different place each time round; the whole stream at
// once is the degenerate case that must still work.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/stream_accumulator.hpp"

namespace {

using ac3::io::AccessUnitAccumulator;
using Status = AccessUnitAccumulator::Status;

std::vector<std::vector<float>> tone(int channels) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                0.4 * std::sin(2.0 * std::numbers::pi * (500.0 * static_cast<double>(ch + 1)) *
                               i / 48000.0));
        }
    }
    return pcm;
}

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<std::byte> ac3_stream(int frames) {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }
    return stream;
}

std::vector<std::byte> eac3_stream(int frames) {
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 384, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }
    return stream;
}

// Access units of an independent substream PLUS a dependent that extends it -
// the shape the accumulator exists for, and the one a syncframe-at-a-time
// reader would break by handing the decoder a dependent with nothing to extend.
//
// Built the way tests/decoder/test_eac3_decoder.cpp builds its 7.1 case: a 5.1
// independent substream followed by a 2/2 dependent whose chanmap adds the rear
// pair. Each access unit is therefore two syncframes, which is what makes this
// different from every other stream in this file.
std::vector<std::byte> multi_substream_stream(int frames) {
    namespace cm = ac3::eac3::chanmap;
    ac3::eac3::FrameEncoder bed{{.bitrate_kbps = 384, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    ac3::eac3::FrameEncoder rear{{.bitrate_kbps = 192,
                                  .acmod = ac3::Acmod::k2_2,
                                  .strmtyp = ac3::eac3::StreamType::kDependent,
                                  .substreamid = 0,
                                  .chanmap = cm::k71Rear,
                                  .last_dependent = true}};
    auto bed_pcm = tone(6);
    auto rear_pcm = tone(4);
    std::vector<std::span<const float>> bed_views;
    for (const auto& channel : bed_pcm) {
        bed_views.emplace_back(channel);
    }
    std::vector<std::span<const float>> rear_views;
    for (const auto& channel : rear_pcm) {
        rear_views.emplace_back(channel);
    }

    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        const auto independent = bed.encode_frame(bed_views);
        REQUIRE(independent.has_value());
        append(stream, *independent);
        const auto dependent = rear.encode_frame(rear_views);
        REQUIRE(dependent.has_value());
        append(stream, *dependent);
    }
    return stream;
}

// Drives the accumulator over `stream`, handing it at most `chunk` bytes at a
// time, and returns the units it produced.
std::vector<std::vector<std::byte>> collect(
    std::span<const std::byte> stream, std::size_t chunk,
    std::size_t buffer_bytes = ac3::io::kRecommendedBuffer) {
    std::vector<std::byte> storage(buffer_bytes);
    AccessUnitAccumulator acc{storage};
    std::vector<std::vector<std::byte>> units;
    std::size_t offset = 0;

    for (int guard = 0; guard < 1000000; ++guard) {
        const auto r = acc.next();
        if (r.status == Status::kUnit) {
            units.emplace_back(r.bytes.begin(), r.bytes.end());
            continue;
        }
        if (r.status == Status::kNeedMoreInput) {
            if (offset >= stream.size()) {
                acc.finish();
                continue;
            }
            auto dst = acc.writable();
            const auto n = std::min({chunk, dst.size(), stream.size() - offset});
            REQUIRE(n > 0);
            std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(offset), n, dst.begin());
            acc.commit(n);
            offset += n;
            continue;
        }
        REQUIRE(r.status == Status::kEndOfStream);
        break;
    }
    return units;
}

std::vector<std::vector<std::byte>> reference(std::span<const std::byte> stream) {
    const auto split = ac3::split_access_units(stream);
    REQUIRE(split.has_value());
    std::vector<std::vector<std::byte>> out;
    for (const auto unit : *split) {
        out.emplace_back(unit.begin(), unit.end());
    }
    return out;
}

}  // namespace

TEST_CASE("AccessUnitAccumulator agrees with split_access_units on AC-3", "[io][stream]") {
    const auto stream = ac3_stream(6);
    const auto expected = reference(stream);
    REQUIRE(expected.size() == 6);
    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{7}, std::size_t{1000}, std::size_t{1792}, stream.size()}) {
        CAPTURE(chunk);
        REQUIRE(collect(stream, chunk) == expected);
    }
}

TEST_CASE("AccessUnitAccumulator agrees with split_access_units on E-AC-3", "[io][stream]") {
    const auto stream = eac3_stream(6);
    const auto expected = reference(stream);
    REQUIRE(expected.size() == 6);
    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{13}, std::size_t{1536}, stream.size()}) {
        CAPTURE(chunk);
        REQUIRE(collect(stream, chunk) == expected);
    }
}

TEST_CASE("AccessUnitAccumulator keeps a dependent substream with its independent",
          "[io][stream]") {
    // The case the class exists for. Each access unit here is an independent
    // substream plus the dependent that extends it; a reader that emitted
    // syncframes would hand the decoder a dependent with nothing to extend.
    constexpr int kFrames = 4;
    const auto stream = multi_substream_stream(kFrames);
    const auto expected = reference(stream);
    REQUIRE(expected.size() == kFrames);

    // Two syncframes per unit, not one - without this the test would pass just
    // as well against a reader that never grouped anything, which is the bug it
    // is meant to catch. Checked by splitting a unit back into syncframes
    // rather than by its size, so it says what it means.
    const auto frames_in_first = ac3::split_frames(expected.front());
    REQUIRE(frames_in_first.has_value());
    REQUIRE(frames_in_first->size() == 2);

    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{64}, std::size_t{1536}, stream.size()}) {
        CAPTURE(chunk);
        REQUIRE(collect(stream, chunk) == expected);
    }
}

TEST_CASE("AccessUnitAccumulator decodes what it produces", "[io][stream]") {
    // Agreeing with split_access_units is only interesting if the units decode.
    const auto stream = ac3_stream(6);
    std::vector<std::byte> storage(ac3::io::kRecommendedBuffer);
    AccessUnitAccumulator acc{storage};
    ac3::FrameDecoder decoder;
    std::size_t offset = 0;
    int decoded = 0;

    for (int guard = 0; guard < 1000000; ++guard) {
        const auto r = acc.next();
        if (r.status == Status::kUnit) {
            const auto frame = decoder.decode_frame(r.bytes);
            REQUIRE(frame.has_value());
            REQUIRE(frame->channels.size() == 6);
            ++decoded;
            continue;
        }
        if (r.status == Status::kNeedMoreInput) {
            if (offset >= stream.size()) {
                acc.finish();
                continue;
            }
            auto dst = acc.writable();
            const auto n = std::min({std::size_t{333}, dst.size(), stream.size() - offset});
            std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(offset), n, dst.begin());
            acc.commit(n);
            offset += n;
            continue;
        }
        break;
    }
    REQUIRE(decoded == 6);
}

TEST_CASE("AccessUnitAccumulator resynchronises past leading junk", "[io][stream]") {
    const auto real = ac3_stream(6);
    std::vector<std::byte> stream(11, std::byte{0xA5});
    stream.insert(stream.end(), real.begin(), real.end());

    const auto units = collect(stream, 5);
    REQUIRE(units.size() == 6);
    REQUIRE(units == reference(real));
}

TEST_CASE("AccessUnitAccumulator reports a buffer it cannot work in", "[io][stream]") {
    // Smaller than one syncframe, so no unit can ever be assembled. It has to
    // SAY so rather than spin on kNeedMoreInput forever: a caller that sized
    // its buffer from configuration must be able to tell "feed me" apart from
    // "you cannot feed me enough".
    const auto stream = ac3_stream(2);
    std::vector<std::byte> storage(256);
    AccessUnitAccumulator acc{storage};
    std::size_t offset = 0;
    Status last = Status::kNeedMoreInput;

    for (int guard = 0; guard < 1000; ++guard) {
        const auto r = acc.next();
        last = r.status;
        if (r.status != Status::kNeedMoreInput) {
            break;
        }
        auto dst = acc.writable();
        const auto n = std::min({std::size_t{64}, dst.size(), stream.size() - offset});
        if (n == 0) {
            break;
        }
        std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(offset), n, dst.begin());
        acc.commit(n);
        offset += n;
    }
    REQUIRE(last == Status::kBufferTooSmall);
}

TEST_CASE("AccessUnitAccumulator ends cleanly on an empty stream", "[io][stream]") {
    std::vector<std::byte> storage(ac3::io::kRecommendedBuffer);
    AccessUnitAccumulator acc{storage};
    acc.finish();
    REQUIRE(acc.next().status == Status::kEndOfStream);
}

TEST_CASE("AccessUnitAccumulator refuses a truncated tail", "[io][stream]") {
    // A stream cut mid-frame. The units before the cut are still good and must
    // come out; the fragment must not be handed over as if it were a frame.
    auto stream = ac3_stream(3);
    const auto whole = reference(stream);
    stream.resize(stream.size() - 100);

    std::vector<std::byte> storage(ac3::io::kRecommendedBuffer);
    AccessUnitAccumulator acc{storage};
    std::vector<std::vector<std::byte>> units;
    std::size_t offset = 0;
    Status last = Status::kNeedMoreInput;

    for (int guard = 0; guard < 1000000; ++guard) {
        const auto r = acc.next();
        last = r.status;
        if (r.status == Status::kUnit) {
            units.emplace_back(r.bytes.begin(), r.bytes.end());
            continue;
        }
        if (r.status == Status::kNeedMoreInput) {
            if (offset >= stream.size()) {
                acc.finish();
                continue;
            }
            auto dst = acc.writable();
            const auto n = std::min({std::size_t{512}, dst.size(), stream.size() - offset});
            std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(offset), n, dst.begin());
            acc.commit(n);
            offset += n;
            continue;
        }
        break;
    }

    REQUIRE(units.size() == 2);
    REQUIRE(units[0] == whole[0]);
    REQUIRE(units[1] == whole[1]);
    REQUIRE(last == Status::kError);
    REQUIRE(acc.error() == ac3::io::ScanError::kTruncated);
}
