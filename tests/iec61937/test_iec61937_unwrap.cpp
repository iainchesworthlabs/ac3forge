#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/iec61937/iec61937.hpp"

// The de-framing side of ac3::iec61937 (roadmap IO3). Its whole reason to
// exist is that nothing read a burst back before, so the wrap side - byte-
// exact against FFmpeg's spdif muxer though it is - had no round trip of its
// own. Every test below that says "round trip" is checking exactly that: the
// bytes that came out of the packer, put through the reader, are the bytes
// that went in.
//
// The adversarial half matters as much. A burst carrier arrives off a wire or
// out of a capture device, so a Pd nobody sanity-checks is an attacker-chosen
// allocation size, and a preamble pattern that turns up inside payload or
// stuffing is a routine event rather than a corner case.

namespace {

using ac3::iec61937::BurstDataType;
using ac3::iec61937::BurstReader;
using ac3::iec61937::UnwrapError;
using ac3::iec61937::WordOrder;

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

std::vector<std::vector<float>> tone_frame(int channels, std::uint64_t start) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (auto& channel : pcm) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            channel[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0));
        }
    }
    return pcm;
}

// A handful of real AC-3 frames of a tone - not silence, whose all-zero
// mantissas would round-trip through almost any bug in the word swap.
std::vector<std::vector<std::byte>> encode_ac3(int count) {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    std::vector<std::vector<std::byte>> frames;
    std::uint64_t n = 0;
    for (int f = 0; f < count; ++f) {
        auto pcm = tone_frame(2, n);
        n += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }
    return frames;
}

std::vector<std::vector<std::byte>> encode_eac3(int count) {
    ac3::eac3::AccessUnitEncoder encoder{{.independent = {.bitrate_kbps = 192}}};
    std::vector<std::vector<std::byte>> units;
    std::uint64_t n = 0;
    for (int f = 0; f < count; ++f) {
        auto pcm = tone_frame(2, n);
        n += ac3::kSamplesPerFrame;
        const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
        auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        units.push_back(std::move(unit->bytes));
    }
    return units;
}

std::vector<std::span<const std::byte>> views_of(
    const std::vector<std::vector<std::byte>>& owned) {
    std::vector<std::span<const std::byte>> views;
    views.reserve(owned.size());
    for (const auto& unit : owned) {
        views.emplace_back(unit);
    }
    return views;
}

std::vector<std::byte> concat(const std::vector<std::vector<std::byte>>& owned) {
    std::vector<std::byte> all;
    for (const auto& unit : owned) {
        all.insert(all.end(), unit.begin(), unit.end());
    }
    return all;
}

// Re-order every 16-bit word's two bytes: turns the little-endian carrier the
// packers emit into the big-endian one a wire capture or a big-endian PCM
// device would hand over. Applied to a whole carrier, preamble included,
// exactly as swapping the container's word endianness would.
std::vector<std::byte> to_big_endian_carrier(std::span<const std::byte> little) {
    std::vector<std::byte> out(little.size());
    for (std::size_t i = 0; i + 1 < little.size(); i += 2) {
        out[i] = little[i + 1];
        out[i + 1] = little[i];
    }
    return out;
}

// Interleaved stereo floats as a capture backend would deliver them, from a
// carrier's PCM16 words - the exact inverse of PassthroughDetector's own
// conversion, so a detection failure is the detector's and not the fixture's.
std::vector<float> as_capture_floats(std::span<const std::byte> carrier) {
    std::vector<float> out;
    out.reserve(carrier.size() / 2);
    for (std::size_t i = 0; i + 1 < carrier.size(); i += 2) {
        const auto word = static_cast<std::uint16_t>(u8(carrier, i) | (u8(carrier, i + 1) << 8));
        out.push_back(static_cast<float>(static_cast<std::int16_t>(word)) / 32768.0f);
    }
    return out;
}

}  // namespace

// --- Round trip: the test the wrap side never had -----------------------------

TEST_CASE("unwrap_stream: AC-3 bursts round-trip back to the exact frames", "[iec61937][unwrap]") {
    const auto frames = encode_ac3(5);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());

    const auto recovered = ac3::iec61937::unwrap_stream(*carrier);
    REQUIRE(recovered.has_value());
    const auto expected = concat(frames);
    CHECK(recovered->size() == expected.size());
    CHECK(std::equal(recovered->begin(), recovered->end(), expected.begin(), expected.end()));

    // And the recovered bytes are a stream in their own right, not just a
    // byte match: split_frames finds every frame back.
    const auto split = ac3::split_frames(*recovered);
    REQUIRE(split.has_value());
    CHECK(split->size() == frames.size());
}

TEST_CASE("unwrap_stream: E-AC-3 bursts round-trip back to the exact access units",
          "[iec61937][unwrap]") {
    const auto units = encode_eac3(4);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(units), /*eac3=*/true);
    REQUIRE(carrier.has_value());

    const auto recovered = ac3::iec61937::unwrap_stream(*carrier);
    REQUIRE(recovered.has_value());
    const auto expected = concat(units);
    CHECK(std::equal(recovered->begin(), recovered->end(), expected.begin(), expected.end()));

    // Decodes as real audio, which no amount of byte-shuffling would survive.
    ac3::Eac3Decoder decoder;
    const auto split = ac3::split_access_units(*recovered);
    REQUIRE(split.has_value());
    REQUIRE(split->size() == units.size());
    const auto decoded = decoder.decode_access_unit((*split)[0]);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    CHECK((*decoded)->channels.size() == 2);
}

TEST_CASE("unwrap_stream: a burst carrying six one-block syncframes comes back whole",
          "[iec61937][unwrap]") {
    // The Annex E case this project's own encoder never produces: numblkscod
    // 0 (Table E2.4), one block per syncframe, so Eac3BurstPacker has to
    // accumulate six access units before a burst is complete. Hand-built for
    // the same reason test_iec61937.cpp hand-builds them - the packer reads
    // only bsid/fscod/numblkscod from the header and never looks at the body.
    // The reader's side of it is that Pd covers the whole concatenation, not
    // one syncframe, which is exactly what a stream that came back one-sixth
    // of its length would prove wrong.
    const auto one_block = [](std::size_t payload_words, std::byte fill) {
        std::vector<std::byte> frame(6 + payload_words * 2, fill);
        frame[0] = std::byte{0x0B};
        frame[1] = std::byte{0x77};
        frame[2] = std::byte{0x00};
        frame[3] = std::byte{0x00};
        // fscod 0 | numblkscod 0 | acmod 2 | lfeon 0
        frame[4] = static_cast<std::byte>(2 << 1);
        frame[5] = static_cast<std::byte>((16 << 3) | 0x7);  // bsid 16 | dialnorm bits
        return frame;
    };
    std::vector<std::vector<std::byte>> units;
    std::vector<std::byte> expected;
    ac3::iec61937::Eac3BurstPacker packer;
    std::vector<std::byte> carrier;
    for (int i = 0; i < 6; ++i) {
        auto unit = one_block(8 + static_cast<std::size_t>(i),
                              static_cast<std::byte>(0xA0 + i));
        expected.insert(expected.end(), unit.begin(), unit.end());
        const auto burst = packer.push(unit);
        REQUIRE(burst.has_value());
        // Only the sixth completes a burst: five blocks is not a burst period.
        CHECK(burst->has_value() == (i == 5));
        if (*burst) {
            carrier.insert(carrier.end(), (**burst).begin(), (**burst).end());
        }
        units.push_back(std::move(unit));
    }
    REQUIRE(carrier.size() == ac3::iec61937::kEac3BurstBytes);

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(carrier, out).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.bursts() == 1);
    CHECK(reader.data_type() == BurstDataType::kEac3);
    REQUIRE(reader.last_header().has_value());
    CHECK(reader.last_header()->payload_bytes == expected.size());
    CHECK(out.size() == expected.size());
    CHECK(std::equal(out.begin(), out.end(), expected.begin(), expected.end()));
}

TEST_CASE("unwrap_stream: reads the burst header back, bsmod included", "[iec61937][unwrap]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    const auto burst = ac3::iec61937::wrap_frame(*frame);
    REQUIRE(burst.has_value());

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(*burst, out).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.bursts() == 1);
    CHECK(reader.data_type() == BurstDataType::kAc3);
    CHECK(reader.word_order() == WordOrder::kLittleEndian);
    REQUIRE(reader.last_header().has_value());
    // bsmod travels in Pc bits 8..10, so it survives the wrap and comes back
    // here; the frame's own bsmod is 0 for a default silent frame.
    CHECK(reader.last_header()->data_type_dependent == (u8(*frame, 5) & 0x7));
    CHECK(reader.last_header()->payload_bytes == frame->size());
    CHECK_FALSE(reader.last_header()->error_flag);
    CHECK(reader.last_header()->stream_number == 0);
}

TEST_CASE("repetition_period: 6144 for AC-3, 24576 for E-AC-3", "[iec61937][unwrap]") {
    CHECK(ac3::iec61937::repetition_period(BurstDataType::kAc3) == 6144);
    CHECK(ac3::iec61937::repetition_period(BurstDataType::kEac3) == 24576);
}

// --- Word packings -----------------------------------------------------------

TEST_CASE("BurstReader: reads a big-endian carrier as well as the little-endian one",
          "[iec61937][unwrap]") {
    const auto frames = encode_ac3(3);
    const auto little = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(little.has_value());
    const auto big = to_big_endian_carrier(*little);

    const auto from_big = ac3::iec61937::unwrap_stream(big);
    REQUIRE(from_big.has_value());
    const auto expected = concat(frames);
    CHECK(std::equal(from_big->begin(), from_big->end(), expected.begin(), expected.end()));

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(big, out).has_value());
    CHECK(reader.word_order() == WordOrder::kBigEndian);
}

TEST_CASE("BurstReader: chunk boundaries never change the result", "[iec61937][unwrap]") {
    const auto units = encode_eac3(3);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(units), /*eac3=*/true);
    REQUIRE(carrier.has_value());
    const auto expected = concat(units);

    // Sizes chosen to split the preamble itself (1, 3), to land mid-payload
    // (7, 4096) and to straddle a burst boundary (kEac3BurstBytes - 1).
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                                    std::size_t{4096}, ac3::iec61937::kEac3BurstBytes - 1}) {
        BurstReader reader;
        std::vector<std::byte> out;
        for (std::size_t at = 0; at < carrier->size(); at += chunk) {
            const auto take = std::min(chunk, carrier->size() - at);
            REQUIRE(reader.push(std::span{*carrier}.subspan(at, take), out).has_value());
        }
        REQUIRE(reader.finish().has_value());
        CHECK(reader.bursts() == units.size());
        CHECK(std::equal(out.begin(), out.end(), expected.begin(), expected.end()));
    }
}

TEST_CASE("BurstReader: leading and trailing junk around the bursts is skipped",
          "[iec61937][unwrap]") {
    const auto frames = encode_ac3(2);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());

    // A capture that started mid-programme: whatever the device was doing
    // before the bursts began, then the bursts, then it stopping again.
    std::vector<std::byte> noisy(1234, std::byte{0x5A});
    noisy.insert(noisy.end(), carrier->begin(), carrier->end());
    noisy.insert(noisy.end(), 777, std::byte{0xA5});

    const auto recovered = ac3::iec61937::unwrap_stream(noisy);
    REQUIRE(recovered.has_value());
    const auto expected = concat(frames);
    CHECK(std::equal(recovered->begin(), recovered->end(), expected.begin(), expected.end()));
}

// --- Hostile input -----------------------------------------------------------

TEST_CASE("BurstReader: a Pd bigger than the repetition period is refused, not allocated",
          "[iec61937][unwrap]") {
    // Pa Pb, Pc = AC-3, Pd = 0xFFFF bits = 8192 bytes, well past AC-3's own
    // 6144-byte period. Nothing here may be believed enough to size a buffer.
    const std::vector<std::byte> hostile{
        std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F}, std::byte{0x4E},
        std::byte{0x01}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0x77}, std::byte{0x0B}, std::byte{0x00}, std::byte{0x00}};
    CHECK(ac3::iec61937::unwrap_stream(hostile).error() == UnwrapError::kPayloadTooLarge);
}

TEST_CASE("BurstReader: a preamble with no syncframe behind it is a false sync",
          "[iec61937][unwrap]") {
    // The preamble pattern with a plausible Pc/Pd but payload that is not
    // 0x0B77 - what a burst carrier's own payload bytes produce by accident.
    std::vector<std::byte> junk{std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F}, std::byte{0x4E},
                                std::byte{0x01}, std::byte{0x00}, std::byte{0x40}, std::byte{0x06},
                                std::byte{0xDE}, std::byte{0xAD}};
    junk.resize(600, std::byte{0});

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(junk, out).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.bursts() == 0);
    CHECK(reader.false_syncs() >= 1);
    CHECK(out.empty());
    CHECK(ac3::iec61937::unwrap_stream(junk).error() == UnwrapError::kNoSync);
}

TEST_CASE("BurstReader: a false preamble inside a payload does not derail the real bursts",
          "[iec61937][unwrap]") {
    // Plant the preamble byte string in the stuffing after burst 0. The
    // reader is in stuffing there, so it will find it, look for a syncframe,
    // not see one, and carry on to burst 1.
    const auto frames = encode_ac3(3);
    auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());
    const std::size_t plant = ac3::iec61937::kBurstBytes - 64;
    (*carrier)[plant] = std::byte{0x72};
    (*carrier)[plant + 1] = std::byte{0xF8};
    (*carrier)[plant + 2] = std::byte{0x1F};
    (*carrier)[plant + 3] = std::byte{0x4E};
    (*carrier)[plant + 4] = std::byte{0x01};  // Pc: AC-3
    (*carrier)[plant + 6] = std::byte{0x40};  // Pd: 1600 bits

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(*carrier, out).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.bursts() == frames.size());
    CHECK(reader.false_syncs() >= 1);
    const auto expected = concat(frames);
    CHECK(std::equal(out.begin(), out.end(), expected.begin(), expected.end()));
}

TEST_CASE("BurstReader: a burst cut off mid-payload is reported, not half-emitted",
          "[iec61937][unwrap]") {
    const auto frames = encode_ac3(2);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());
    // Cut inside the second burst's payload: 8 preamble bytes plus a few.
    const auto truncated =
        std::span{*carrier}.first(ac3::iec61937::kBurstBytes + 8 + 16);

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(truncated, out).has_value());
    CHECK(reader.bursts() == 1);
    CHECK(reader.finish().error() == UnwrapError::kTruncatedBurst);
    CHECK(ac3::iec61937::unwrap_stream(truncated).error() == UnwrapError::kTruncatedBurst);
}

TEST_CASE("BurstReader: a carrier of another codec's bursts yields nothing, not garbage",
          "[iec61937][unwrap]") {
    // Data type 0x0B (DTS type IV) with a 512-byte payload, twice. Neither is
    // ours, so both are stepped over and the reader reports no bursts at all
    // rather than trying to read DTS as AC-3.
    std::vector<std::byte> carrier;
    for (int i = 0; i < 2; ++i) {
        carrier.insert(carrier.end(), {std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F},
                                       std::byte{0x4E}, std::byte{0x0B}, std::byte{0x00},
                                       std::byte{0x00}, std::byte{0x10}});
        carrier.resize(carrier.size() + 512, std::byte{0x33});
    }

    BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(carrier, out).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.bursts() == 0);
    CHECK(reader.skipped_bursts() == 2);
    CHECK(out.empty());
}

TEST_CASE("BurstReader: ordinary PCM is not mistaken for a carrier", "[iec61937][unwrap]") {
    // A loud sine as PCM16 - the thing a capture actually delivers when the
    // source is not bitstreaming. It must produce no bursts at all.
    std::vector<std::byte> pcm;
    for (int i = 0; i < 48000; ++i) {
        const auto s = static_cast<std::int16_t>(
            30000.0 * std::sin(2.0 * std::numbers::pi * 997.0 * i / 48000.0));
        const auto word = static_cast<std::uint16_t>(s);
        pcm.push_back(static_cast<std::byte>(word & 0xFF));
        pcm.push_back(static_cast<std::byte>(word >> 8));
    }
    CHECK(ac3::iec61937::unwrap_stream(pcm).error() == UnwrapError::kNoSync);
}

TEST_CASE("BurstReader: an empty or sub-preamble carrier is kNoSync, not a crash",
          "[iec61937][unwrap]") {
    CHECK(ac3::iec61937::unwrap_stream({}).error() == UnwrapError::kNoSync);
    const std::vector<std::byte> two{std::byte{0x72}, std::byte{0xF8}};
    CHECK(ac3::iec61937::unwrap_stream(two).error() == UnwrapError::kNoSync);
    // Every prefix of a real burst: no length short of the whole payload may
    // produce output, and none may trip an assertion on the way.
    const auto frames = encode_ac3(1);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());
    for (std::size_t n = 0; n < 8 + frames[0].size(); ++n) {
        BurstReader reader;
        std::vector<std::byte> out;
        REQUIRE(reader.push(std::span{*carrier}.first(n), out).has_value());
        CHECK(out.empty());
    }
}

TEST_CASE("describe: every UnwrapError has a message", "[iec61937][unwrap]") {
    for (const auto error : {UnwrapError::kNoSync, UnwrapError::kTruncatedBurst,
                             UnwrapError::kPayloadTooLarge}) {
        CHECK_FALSE(ac3::iec61937::describe(error).empty());
        CHECK(ac3::iec61937::describe(error) != "unknown error");
    }
}

// --- Capture-side recognition -------------------------------------------------

TEST_CASE("PassthroughDetector: recognises an AC-3 carrier arriving as capture floats",
          "[iec61937][unwrap][capture]") {
    const auto frames = encode_ac3(2);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());
    const auto floats = as_capture_floats(*carrier);

    ac3::iec61937::PassthroughDetector detector;
    detector.push(floats, 2);
    REQUIRE(detector.detected() == BurstDataType::kAc3);
    CHECK(detector.word_order() == WordOrder::kLittleEndian);

    // What it buffered is a carrier in its own right: unwrapping it recovers
    // the frames, which is the whole point of keeping it.
    const auto recovered = ac3::iec61937::unwrap_stream(detector.buffered());
    REQUIRE(recovered.has_value());
    const auto expected = concat(frames);
    // The detector stops buffering at its inspection limit, so it holds a
    // prefix - however much it holds must match byte for byte.
    REQUIRE_FALSE(recovered->empty());
    CHECK(std::equal(recovered->begin(), recovered->end(), expected.begin()));
}

TEST_CASE("PassthroughDetector: recognises an E-AC-3 carrier", "[iec61937][unwrap][capture]") {
    const auto units = encode_eac3(2);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(units), /*eac3=*/true);
    REQUIRE(carrier.has_value());

    ac3::iec61937::PassthroughDetector detector;
    detector.push(as_capture_floats(*carrier), 2);
    CHECK(detector.detected() == BurstDataType::kEac3);
    CHECK(detector.decided());
}

TEST_CASE("PassthroughDetector: real audio decides 'not a bitstream' and stops buffering",
          "[iec61937][unwrap][capture]") {
    ac3::iec61937::PassthroughDetector detector;
    std::vector<float> pcm(2 * 8192);
    for (std::size_t i = 0; i < pcm.size() / 2; ++i) {
        const auto s = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0));
        pcm[i * 2] = s;
        pcm[i * 2 + 1] = s;
    }
    while (!detector.decided()) {
        detector.push(pcm, 2);
    }
    CHECK_FALSE(detector.detected().has_value());
    // A capture that is not a bitstream must not go on holding carrier bytes
    // for the rest of the session.
    CHECK(detector.buffered().empty());
    CHECK(detector.inspected_bytes() >=
          ac3::iec61937::PassthroughDetector::kInspectBytes);
}

TEST_CASE("PassthroughDetector: a multichannel capture still sees the stereo carrier",
          "[iec61937][unwrap][capture]") {
    const auto frames = encode_ac3(2);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());
    const auto stereo = as_capture_floats(*carrier);

    // The same carrier delivered by a six-channel endpoint: the burst is in
    // channels 0 and 1, the rest is whatever the device pads with.
    std::vector<float> six(stereo.size() / 2 * 6, 0.25f);
    for (std::size_t frame = 0; frame * 2 + 1 < stereo.size(); ++frame) {
        six[frame * 6] = stereo[frame * 2];
        six[frame * 6 + 1] = stereo[frame * 2 + 1];
    }

    ac3::iec61937::PassthroughDetector detector;
    detector.push(six, 6);
    CHECK(detector.detected() == BurstDataType::kAc3);
}

TEST_CASE("carrier_from_capture: PCM16 words survive the trip through float exactly",
          "[iec61937][unwrap][capture]") {
    // Every int16 value, not a sample of them: the conversion is only worth
    // anything if it is exact for all 65536, and -32768/32767 are precisely
    // where a naive x * 32767 or an unclamped cast goes wrong.
    std::vector<std::byte> expected;
    std::vector<float> floats;
    for (std::int32_t v = -32768; v <= 32767; ++v) {
        const auto word = static_cast<std::uint16_t>(static_cast<std::int16_t>(v));
        expected.push_back(static_cast<std::byte>(word & 0xFF));
        expected.push_back(static_cast<std::byte>(word >> 8));
        floats.push_back(static_cast<float>(v) / 32768.0f);
    }
    // Fed as a mono capture so every value is its own sample rather than
    // half of a stereo pair.
    std::vector<std::byte> out;
    ac3::iec61937::carrier_from_capture(floats, 1, out);
    REQUIRE(out.size() == expected.size());
    CHECK(std::equal(out.begin(), out.end(), expected.begin()));

    // And the detector agrees with it, which is the invariant that matters:
    // a session that detected one carrier and then recorded a different one
    // would be worse than not detecting at all.
    const auto frames = encode_ac3(1);
    const auto carrier = ac3::iec61937::wrap_stream(views_of(frames), /*eac3=*/false);
    REQUIRE(carrier.has_value());
    const auto as_floats = as_capture_floats(*carrier);
    std::vector<std::byte> rebuilt;
    ac3::iec61937::carrier_from_capture(as_floats, 2, rebuilt);
    CHECK(std::equal(rebuilt.begin(), rebuilt.end(), carrier->begin(), carrier->end()));
}

TEST_CASE("PassthroughDetector: a big push does not make it hold a big buffer",
          "[iec61937][unwrap][capture]") {
    // A caller handing over a whole second at a time must not turn the
    // detector into a second-long buffer: the inspection budget is the bound,
    // whatever the chunk size.
    ac3::iec61937::PassthroughDetector detector;
    const std::vector<float> loud(2 * 96000, 0.75f);
    detector.push(loud, 2);
    CHECK(detector.inspected_bytes() <= ac3::iec61937::PassthroughDetector::kInspectBytes + 4);
    CHECK(detector.decided());
    CHECK(detector.buffered().empty());
}

TEST_CASE("PassthroughDetector: silence is not a bitstream", "[iec61937][unwrap][capture]") {
    ac3::iec61937::PassthroughDetector detector;
    const std::vector<float> quiet(4096, 0.0f);
    while (!detector.decided()) {
        detector.push(quiet, 2);
    }
    CHECK_FALSE(detector.detected().has_value());
}
