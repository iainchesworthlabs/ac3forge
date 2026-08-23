#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/emdf/frame_layout.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/object_strip.hpp"
#include "ac3/oba/atmos.hpp"

// The claim ac3::io::strip_objects makes is narrow and checkable: the object
// layer goes, and the AUDIO does not change at all. So the assertions here
// are mostly decode-and-compare - the stripped stream's PCM against the
// original's, sample for sample - rather than assertions about which bytes
// moved where. A rewrite that shifted a mantissa by one bit would still
// produce a stream that frames and CRC-checks; only the decoded audio catches
// it.

namespace {

using Bytes = std::vector<std::byte>;

std::vector<float> tone(double hz, std::uint64_t start) {
    std::vector<float> out(static_cast<std::size_t>(ac3::kSamplesPerFrame));
    for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
    return out;
}

// A short Atmos stream, container emitted (or not) per `emit_objects` -
// the same fixture shape tests/signing/test_signing.cpp builds, for the same
// reason: it is the one stream in the tree that really carries an EMDF
// object container.
Bytes encode_atmos_stream(int frames, bool emit_objects, int objects = 2) {
    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .num_bands_idx = 4, .emit_object_metadata = emit_objects},
        objects};
    std::vector<ac3::oba::ObjectPlacement> placement(static_cast<std::size_t>(objects));
    std::vector<std::vector<float>> essence(static_cast<std::size_t>(objects));
    std::vector<std::span<const float>> views(static_cast<std::size_t>(objects));
    Bytes stream;
    for (int f = 0; f < frames; ++f) {
        for (int o = 0; o < objects; ++o) {
            essence[static_cast<std::size_t>(o)] =
                tone(440.0 * (o + 1), static_cast<std::uint64_t>(f) *
                                          static_cast<std::uint64_t>(ac3::kSamplesPerFrame));
            views[static_cast<std::size_t>(o)] = essence[static_cast<std::size_t>(o)];
        }
        auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

// Every rendered sample of a decoded stream, concatenated channel by channel
// - one value to compare rather than a nested structure.
std::vector<float> decode_all(std::span<const std::byte> stream) {
    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> rendered;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        if (rendered.empty()) {
            rendered.resize((*decoded)->channels.size());
        }
        REQUIRE((*decoded)->channels.size() == rendered.size());
        for (std::size_t ch = 0; ch < rendered.size(); ++ch) {
            rendered[ch].insert(rendered[ch].end(), (*decoded)->channels[ch].begin(),
                                (*decoded)->channels[ch].end());
        }
    }
    std::vector<float> out;
    for (const auto& channel : rendered) {
        out.insert(out.end(), channel.begin(), channel.end());
    }
    return out;
}

// A plain stereo E-AC-3 stream: a shape ac3::emdf::walk_frame does not map,
// and one that plainly has no object layer either.
Bytes stereo_eac3_stream(int frames) {
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    Bytes stream;
    for (int f = 0; f < frames; ++f) {
        const auto unit = ac3::eac3::build_silent_access_unit(config);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

Bytes stereo_ac3_stream(int frames) {
    Bytes stream;
    for (int f = 0; f < frames; ++f) {
        const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
        REQUIRE(frame.has_value());
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    return stream;
}

}  // namespace

TEST_CASE("strip_objects leaves the bed audio bit-identical", "[io][strip]") {
    const Bytes original = encode_atmos_stream(6, /*emit_objects=*/true);
    const auto scanned = ac3::io::scan(original);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->oba_complexity_index.has_value());

    const auto stripped = ac3::io::strip_objects(original);
    REQUIRE(stripped.has_value());
    CHECK(stripped->frames_total == 6);
    CHECK(stripped->frames_stripped == 6);
    CHECK(stripped->bytes_removed > 0);
    CHECK(stripped->bytes.size() + stripped->bytes_removed == original.size());

    // The whole point: same samples, exactly.
    CHECK(decode_all(stripped->bytes) == decode_all(original));
}

TEST_CASE("strip_objects removes every trace of the object layer", "[io][strip]") {
    const Bytes original = encode_atmos_stream(4, /*emit_objects=*/true);
    const auto stripped = ac3::io::strip_objects(original);
    REQUIRE(stripped.has_value());

    const auto rescanned = ac3::io::scan(stripped->bytes);
    REQUIRE(rescanned.has_value());
    // TS 103 420 §8.3.1's addbsi marker is gone, so nothing downstream - a
    // dec3 box's Atmos extension, an HLS CHANNELS="<N>/JOC" attribute -
    // signals an object layer for a stream that no longer has one.
    CHECK_FALSE(rescanned->oba_complexity_index.has_value());
    CHECK(rescanned->access_units.size() == 4);
    CHECK(rescanned->channels == 6);
    CHECK(rescanned->acmod == ac3::Acmod::k3_2);
    CHECK(rescanned->lfe);

    // And the container itself: no skip field is left to hold one, emptied or
    // otherwise (docs/concepts/atmos-joc.md's own fallback rule).
    for (const auto& unit : rescanned->access_units) {
        const auto layout = ac3::emdf::walk_frame(unit);
        REQUIRE(layout.object_signals);
        CHECK_FALSE(layout.skipflde);
        CHECK_FALSE(layout.addbsi_object_extension);
        CHECK_FALSE(layout.has_container);
    }
}

TEST_CASE("strip_objects re-derives frmsiz and re-stamps crc2", "[io][strip]") {
    const Bytes original = encode_atmos_stream(4, /*emit_objects=*/true);
    const auto stripped = ac3::io::strip_objects(original);
    REQUIRE(stripped.has_value());

    // §E2.3.1.3: frmsiz is the word count minus one, so the declared size and
    // the real one have to agree for scan() to walk the stream at all - which
    // it just did above. What is left to check is that every frame really did
    // shrink, and that crc2 checks out over the new bytes.
    const auto rescanned = ac3::io::scan(stripped->bytes);
    REQUIRE(rescanned.has_value());
    const auto before = ac3::io::scan(original);
    REQUIRE(before.has_value());
    REQUIRE(rescanned->access_units.size() == before->access_units.size());
    for (std::size_t i = 0; i < rescanned->access_units.size(); ++i) {
        CHECK(rescanned->access_units[i].size() < before->access_units[i].size());
        // A/52 §5.4.5.2: running the covered region through the CRC register
        // yields zero when crc2 is right. crc2 covers everything after the
        // syncword, itself included.
        const auto unit = rescanned->access_units[i];
        CHECK(ac3::crc16(unit.subspan(2)) == 0);
    }
}

TEST_CASE("strip_objects passes through what has no object layer", "[io][strip]") {
    SECTION("an Atmos-shaped stream with the container switched off") {
        const Bytes bed51 = encode_atmos_stream(3, /*emit_objects=*/false);
        const auto stripped = ac3::io::strip_objects(bed51);
        REQUIRE(stripped.has_value());
        CHECK(stripped->frames_total == 3);
        CHECK(stripped->frames_stripped == 0);
        CHECK(stripped->bytes_removed == 0);
        CHECK(stripped->bytes == bed51);
    }

    SECTION("a stream of a shape the frame walker does not map") {
        // A stereo E-AC-3 stream is outside ac3::emdf::walk_frame's scope,
        // but it plainly has no object layer either - so it comes back
        // untouched rather than refused. That distinction is the whole reason
        // FrameLayout::object_signals is read separately from the full map.
        const Bytes stream = stereo_eac3_stream(3);
        const auto stripped = ac3::io::strip_objects(stream);
        REQUIRE(stripped.has_value());
        CHECK(stripped->frames_total == 3);
        CHECK(stripped->frames_stripped == 0);
        CHECK(stripped->bytes == stream);
    }
}

TEST_CASE("strip_objects refuses what it cannot honestly strip", "[io][strip]") {
    SECTION("an AC-3 stream has no Annex E object layer at all") {
        const auto stripped = ac3::io::strip_objects(stereo_ac3_stream(3));
        REQUIRE_FALSE(stripped.has_value());
        CHECK(stripped.error() == ac3::io::StripError::kNotEac3);
    }

    SECTION("empty input") {
        const auto stripped = ac3::io::strip_objects({});
        REQUIRE_FALSE(stripped.has_value());
        CHECK(stripped.error() == ac3::io::StripError::kEmpty);
    }

    SECTION("a truncated final frame") {
        Bytes original = encode_atmos_stream(3, /*emit_objects=*/true);
        original.resize(original.size() - 16);
        const auto stripped = ac3::io::strip_objects(original);
        REQUIRE_FALSE(stripped.has_value());
        CHECK(stripped.error() == ac3::io::StripError::kTruncated);
    }

    SECTION("every error has a description") {
        for (const auto error :
             {ac3::io::StripError::kEmpty, ac3::io::StripError::kLostSync,
              ac3::io::StripError::kTruncated, ac3::io::StripError::kNotEac3,
              ac3::io::StripError::kUnsupportedFrame,
              ac3::io::StripError::kFrameSizeDependentField}) {
            CHECK_FALSE(ac3::io::describe(error).empty());
        }
    }
}

TEST_CASE("stripping twice is the same as stripping once", "[io][strip]") {
    const Bytes original = encode_atmos_stream(4, /*emit_objects=*/true);
    const auto once = ac3::io::strip_objects(original);
    REQUIRE(once.has_value());
    const auto twice = ac3::io::strip_objects(once->bytes);
    REQUIRE(twice.has_value());
    CHECK(twice->frames_stripped == 0);
    CHECK(twice->bytes == once->bytes);
}
