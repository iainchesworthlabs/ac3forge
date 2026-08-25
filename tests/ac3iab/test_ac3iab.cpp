#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ac3iab/ac3iab.hpp"

// These tests build IAB bitstream fixtures bit-by-bit, independently of src/ac3iab's own
// implementation (a fresh, from-scratch MSB-first BitWriter below, not src/bitreader.hpp's
// reader run backwards) - the same "independent fixture" reasoning test_adm.cpp documents for
// its own BW64 fixtures. Field values are chosen at the DistanceXY/DistanceZ/gain formulas'
// own domain boundaries (§5.4/§5.5) so a sign or off-by-one error in this reader's conversion
// math would show up as a wrong Approx() rather than passing by accident.
//
// A structural cross-check was additionally run outside this suite against
// DTSProAudio/iab-validator's own real sample bitstreams (test/bitstreams/*.iab, MIT) as an
// external oracle - see iab.cpp's header comment; that corpus is not vendored into this repo
// (this project does not redistribute third-party test material, matching its spec-PDF
// convention), so it cannot be exercised from CI.

using Catch::Approx;

namespace {

// A from-scratch MSB-first bit writer (§5.1), used only to build test fixtures.
class BitWriter {
public:
    void push_bits(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            push_bit(static_cast<unsigned>((value >> (width - 1 - i)) & 0x1u));
        }
    }

    // §5.2 Plex(n) encode: escalate the field width whenever `value` cannot be told apart from
    // the all-ones escape code at the current width.
    void push_plex(std::uint64_t value, unsigned initial_width) {
        unsigned width = initial_width;
        while (true) {
            const std::uint64_t escape = (std::uint64_t{1} << width) - 1;
            if (value < escape) {
                push_bits(value, width);
                return;
            }
            push_bits(escape, width);
            width *= 2;
        }
    }

    void align_to_byte() {
        while (bit_count_ % 8 != 0) {
            push_bit(0);
        }
    }

    void push_raw_byte(unsigned char byte) {
        align_to_byte();
        bytes_.push_back(static_cast<std::byte>(byte));
        bit_count_ += 8;
    }

    [[nodiscard]] std::vector<std::byte> bytes() const { return bytes_; }

private:
    void push_bit(unsigned bit) {
        if (bit_count_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit) {
            bytes_.back() |= static_cast<std::byte>(1u << (7 - (bit_count_ % 8)));
        }
        ++bit_count_;
    }

    std::vector<std::byte> bytes_;
    std::size_t bit_count_ = 0;
};

void append(std::vector<std::byte>& out, const std::vector<std::byte>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

// §9 Table 3 / §10.1: wraps a payload with its ElementID/ElementSize header.
std::vector<std::byte> wrap_element(std::uint32_t id, const std::vector<std::byte>& payload) {
    BitWriter bw;
    bw.push_plex(id, 8);
    bw.push_plex(payload.size(), 8);
    auto bytes = bw.bytes();
    append(bytes, payload);
    return bytes;
}

// §9.2 Table 6: a 2-channel Bed with one explicit (-6 dB) gain and one maximum-decorrelation
// channel, plus a Child BedRemap (§9.3 Table 7).
std::vector<std::byte> build_bed_payload(unsigned sub_block_count) {
    BitWriter bw;
    bw.push_plex(1, 8);   // MetaID
    bw.push_bits(0, 1);   // ConditionalBed = 0 (unconditional)
    bw.push_plex(2, 4);   // ChannelCount = 2

    bw.push_plex(0x2, 4);  // ChannelID = Center
    bw.push_plex(1, 8);    // AudioDataID
    bw.push_bits(2, 2);    // ChannelGainPrefix = code follows
    bw.push_bits(64, 10);  // ChannelGain = 0x40 -> spec's own -6 dB / 0.5 worked example (§5.5)
    bw.push_bits(0, 1);    // ChannelDecorInfoExists = 0

    bw.push_plex(0x0, 4);  // ChannelID = Left
    bw.push_plex(2, 8);    // AudioDataID
    bw.push_bits(0, 2);    // ChannelGainPrefix = unity
    bw.push_bits(1, 1);    // ChannelDecorInfoExists = 1
    bw.push_bits(0, 4);    // Reserved
    bw.push_bits(1, 2);    // ChannelDecorCoefPrefix = maximum

    bw.push_bits(0x180, 10);  // Reserved, set to 0x180
    bw.align_to_byte();
    bw.push_bits(0x01, 8);  // AudioDescription = not indicated
    bw.push_plex(1, 8);     // SubElementCount = 1 (the BedRemap below)

    auto bytes = bw.bytes();

    BitWriter remap;
    remap.push_plex(9, 8);     // MetaID
    remap.push_bits(0xFF, 8);  // RemapUseCase = Always Use
    remap.push_plex(2, 4);     // SourceChannels
    remap.push_plex(2, 4);     // DestinationChannels
    for (unsigned sb = 0; sb < sub_block_count; ++sb) {
        if (sb == 0) {
            remap.push_plex(0x0, 4);  // DestinationChannelID = Left
            remap.push_bits(0, 2);
            remap.push_bits(0, 2);
            remap.push_plex(0x4, 4);  // DestinationChannelID = Right
            remap.push_bits(0, 2);
            remap.push_bits(0, 2);
        } else {
            remap.push_bits(0, 1);  // RemapInfoExists = 0 (carry forward)
        }
    }
    remap.align_to_byte();
    remap.push_plex(0, 8);  // Reserved, set to 0
    append(bytes, wrap_element(0x20, remap.bytes()));
    return bytes;
}

// §9.4 Table 8: one Object sub block at the DistanceXY/DistanceZ formulas' own boundary
// codes, all-zones-set, 3D spread, plus a Child ObjectZoneDefinition19 (§9.5 Table 9). Sub
// blocks past the first carry PanInfoExists/ZoneInfoExists = 0 (no further data), matching
// §10.5.4/§10.6.1's "previous... remains valid" convention.
std::vector<std::byte> build_object_payload(unsigned sub_block_count) {
    BitWriter bw;
    bw.push_plex(5, 8);  // MetaID
    bw.push_plex(3, 8);  // AudioDataID
    bw.push_bits(0, 1);  // ConditionalObject = 0
    bw.push_bits(0, 1);  // Reserved, set to 0

    for (unsigned sb = 0; sb < sub_block_count; ++sb) {
        if (sb != 0) {
            bw.push_bits(0, 1);  // PanInfoExists = 0
            continue;
        }
        bw.push_bits(0, 2);        // ObjectGainPrefix = unity
        bw.push_bits(0b001, 3);    // Reserved
        bw.push_bits(65535, 16);   // ObjectPosX = max -> DistanceXY = 1.0
        bw.push_bits(32767, 16);   // ObjectPosY = min valid domain -> DistanceXY = 0.0
        bw.push_bits(65535, 16);   // ObjectPosZ = max -> DistanceZ = 1.0
        bw.push_bits(1, 1);        // ObjectSnap = 1
        bw.push_bits(1, 1);        // ObjectSnapTolExists = 1
        bw.push_bits(4095, 12);    // ObjectSnapTolerance = max -> DistanceZ(12) = 1.0
        bw.push_bits(0, 1);        // Res2
        bw.push_bits(1, 1);        // ObjectZoneControl = 1
        for (int n = 0; n < 9; ++n) {
            bw.push_bits(2, 2);     // ZoneGainPrefix = code follows
            bw.push_bits(511, 10);  // ZoneGain -> linear 511/1023, NOT the log2 gain formula
        }
        bw.push_bits(0x3, 2);      // ObjectSpreadMode = 3D
        bw.push_bits(4095, 12);    // SpreadX = max -> 1.0
        bw.push_bits(0, 12);       // SpreadY = 0.0
        bw.push_bits(2047, 12);    // SpreadZ -> ~0.5
        bw.push_bits(0, 4);        // Reserved
        bw.push_bits(1, 2);        // ObjectDecorCoefPrefix = maximum
    }

    bw.align_to_byte();
    bw.push_bits(0x82, 8);  // AudioDescription = Dialog (0x02) | text follows (0x80)
    for (char c : std::string_view("obj")) {
        bw.push_raw_byte(static_cast<unsigned char>(c));
    }
    bw.push_raw_byte(0x00);

    bw.push_plex(1, 8);  // SubElementCount = 1 (the ObjectZoneDefinition19 below)
    auto bytes = bw.bytes();

    BitWriter zone19;
    for (unsigned sb = 0; sb < sub_block_count; ++sb) {
        if (sb == 0) {
            for (int n = 0; n < 19; ++n) {
                zone19.push_bits(2, 2);
                zone19.push_bits(300, 10);  // linear 300/1023
            }
        } else {
            zone19.push_bits(0, 1);  // ZoneInfoExists = 0
        }
    }
    zone19.align_to_byte();
    append(bytes, wrap_element(0x80, zone19.bytes()));
    return bytes;
}

// §9.7/§10.8.1: little-endian PCM, `bytes_per_sample` wide (2 for 16-bit, 3 for 24-bit).
std::vector<std::byte> build_pcm_payload(
        std::uint32_t audio_data_id, unsigned bytes_per_sample, const std::vector<std::int32_t>& samples) {
    BitWriter bw;
    bw.push_plex(audio_data_id, 8);
    auto bytes = bw.bytes();
    for (auto sample : samples) {
        const auto raw = static_cast<std::uint32_t>(sample);
        for (unsigned b = 0; b < bytes_per_sample; ++b) {
            bytes.push_back(static_cast<std::byte>((raw >> (8 * b)) & 0xFFu));
        }
    }
    return bytes;
}

// §9.6/§10.7: phase 1 reads only AudioDataID/DLCSize and leaves the rest opaque.
std::vector<std::byte> build_dlc_payload(std::uint32_t audio_data_id, const std::vector<std::byte>& opaque) {
    BitWriter bw;
    bw.push_plex(audio_data_id, 8);
    bw.push_bits(opaque.size(), 16);  // DLCSize
    auto bytes = bw.bytes();
    append(bytes, opaque);
    return bytes;
}

std::vector<std::byte> build_authoring_tool_payload(std::string_view uri) {
    std::vector<std::byte> bytes;
    for (char c : uri) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    bytes.push_back(std::byte{0});
    return bytes;
}

std::vector<std::byte> build_user_data_payload() {
    std::vector<std::byte> bytes(16, std::byte{0xAB});
    bytes.push_back(std::byte{0xDE});
    bytes.push_back(std::byte{0xAD});
    return bytes;
}

// §9.1 Table 5: the IAFrame header plus whichever already-wrapped Children are supplied.
std::vector<std::byte> build_iaframe_payload(
        unsigned sample_rate_code, unsigned bit_depth_code, unsigned frame_rate_code,
        const std::vector<std::vector<std::byte>>& children) {
    BitWriter bw;
    bw.push_bits(1, 8);  // Version
    bw.push_bits(sample_rate_code, 2);
    bw.push_bits(bit_depth_code, 2);
    bw.push_bits(frame_rate_code, 4);
    bw.push_plex(300, 8);  // MaxRendered = 300, exercises the Plex(8) 8->16 escape
    bw.align_to_byte();
    bw.push_plex(children.size(), 8);
    auto bytes = bw.bytes();
    for (const auto& child : children) {
        append(bytes, child);
    }
    return bytes;
}

// Wraps an IAFrame payload in the §7/§8 Preamble+IAFrame segment framing a real .iab file uses.
std::vector<std::byte> build_iabitstream(const std::vector<std::byte>& iaframe_payload) {
    BitWriter bw;
    bw.push_raw_byte(0x01);  // PreambleTag
    bw.push_bits(0, 32);     // PreambleLength = 0
    bw.push_raw_byte(0x02);  // IAFrameTag
    auto element = wrap_element(0x08, iaframe_payload);
    bw.push_bits(element.size(), 32);  // IAFrameLength
    auto bytes = bw.bytes();
    append(bytes, element);
    return bytes;
}

constexpr unsigned kFrameRate48Fps = 0x3;  // Table 17: 48 fps -> Table 23: NumPanSubBlocks = 4
constexpr unsigned kSampleRate48k = 0x0;
constexpr unsigned kBitDepth24 = 0x1;

std::string to_string(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

TEST_CASE("parses the full Preamble+IAFrame segment framing", "[ac3iab]") {
    auto frame_bytes = build_iabitstream(build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {}));
    std::istringstream in(to_string(frame_bytes));

    auto result = ac3iab::parse_iabitstream(in);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK(result->front().preamble.empty());
    CHECK(result->front().frame.version == 1);
    CHECK(result->front().frame.max_rendered == 300);  // exercised the Plex(8) escape
}

TEST_CASE("parses IAFrame header fields", "[ac3iab]") {
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {});
    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    CHECK(frame->sample_rate == 48000);
    CHECK(frame->bit_depth == 24);
    CHECK(frame->frame_rate_code == kFrameRate48Fps);
    CHECK(frame->max_rendered == 300);
    CHECK(ac3iab::num_pan_sub_blocks(frame->frame_rate_code) == 4);
}

TEST_CASE("parses a BedDefinition with gain, decorrelation and a Child BedRemap", "[ac3iab]") {
    auto bed = wrap_element(0x10, build_bed_payload(4));
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {bed});
    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    REQUIRE(frame->beds.size() == 1);

    const auto& bed_def = frame->beds.front();
    CHECK(bed_def.meta_id == 1);
    CHECK_FALSE(bed_def.activation.conditional);
    REQUIRE(bed_def.channels.size() == 2);
    CHECK(bed_def.channels[0].channel_id == 0x2);
    CHECK(bed_def.channels[0].gain == Approx(0.5));  // spec's own -6 dB / G=0x40 example
    CHECK_FALSE(bed_def.channels[0].decorrelation.has_value());
    CHECK(bed_def.channels[1].gain == Approx(1.0));
    REQUIRE(bed_def.channels[1].decorrelation.has_value());
    CHECK(*bed_def.channels[1].decorrelation == Approx(1.0));

    REQUIRE(bed_def.remaps.size() == 1);
    CHECK(bed_def.remaps.front().source_channels == 2);
    CHECK(bed_def.remaps.front().destination_channels == 2);
    REQUIRE(bed_def.remaps.front().sub_blocks.size() == 4);
    CHECK(bed_def.remaps.front().sub_blocks[0].has_remap_info);
    CHECK_FALSE(bed_def.remaps.front().sub_blocks[1].has_remap_info);
    CHECK(bed_def.remaps.front().sub_blocks[1].gains.empty());
}

TEST_CASE("parses an ObjectDefinition at the position/spread/zone formula boundaries", "[ac3iab]") {
    auto object = wrap_element(0x40, build_object_payload(4));
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {object});
    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    REQUIRE(frame->objects.size() == 1);

    const auto& obj = frame->objects.front();
    CHECK(obj.meta_id == 5);
    CHECK(obj.audio_data_id == 3);
    CHECK(obj.description.dialog);
    REQUIRE(obj.description.text.has_value());
    CHECK(*obj.description.text == "obj");

    REQUIRE(obj.sub_blocks.size() == 4);
    const auto& sb0 = obj.sub_blocks[0];
    CHECK(sb0.has_pan_info);
    CHECK(sb0.gain == Approx(1.0));
    CHECK(sb0.position.x == Approx(1.0));
    CHECK(sb0.position.y == Approx(0.0));
    CHECK(sb0.position.z == Approx(1.0));
    CHECK(sb0.snap);
    REQUIRE(sb0.snap_tolerance.has_value());
    CHECK(*sb0.snap_tolerance == Approx(1.0));
    REQUIRE(sb0.zone_gains.has_value());
    for (double gain : *sb0.zone_gains) {
        CHECK(gain == Approx(511.0 / 1023.0));
    }
    CHECK(sb0.spread.mode == ac3iab::ObjectSpreadMode::kThreeD);
    CHECK(sb0.spread.x == Approx(1.0));
    CHECK(sb0.spread.y == Approx(0.0));
    CHECK(sb0.spread.z == Approx(2047.0 / 4095.0));
    CHECK(sb0.decorrelation == Approx(1.0));

    CHECK_FALSE(obj.sub_blocks[1].has_pan_info);
    CHECK_FALSE(obj.sub_blocks[1].zone_gains.has_value());

    REQUIRE(obj.zone19.has_value());
    REQUIRE(obj.zone19->sub_blocks.size() == 4);
    CHECK(obj.zone19->sub_blocks[0].has_zone_info);
    for (double gain : obj.zone19->sub_blocks[0].zone_gains) {
        CHECK(gain == Approx(300.0 / 1023.0));
    }
    CHECK_FALSE(obj.zone19->sub_blocks[1].has_zone_info);
}

TEST_CASE("decodes little-endian AudioDataPCM at 24-bit depth", "[ac3iab]") {
    std::vector<std::int32_t> samples(1000, 0);  // Table 18: 48 fps/48 kHz -> SampleCount48 = 1000
    samples[0] = 0;
    samples[1] = 1;
    samples[2] = -1;
    samples[3] = 0x7FFFFF;         // max positive, 24-bit
    samples[4] = -0x800000;        // max negative, 24-bit (two's complement)
    auto pcm = wrap_element(0x400, build_pcm_payload(7, 3, samples));
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {pcm});

    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    REQUIRE(frame->audio_pcm.size() == 1);
    const auto& audio = frame->audio_pcm.front();
    CHECK(audio.audio_data_id == 7);
    REQUIRE(audio.samples.size() == 1000);
    CHECK(audio.samples[0] == Approx(0.0));
    CHECK(audio.samples[1] == Approx(1.0 / 8388608.0));
    CHECK(audio.samples[2] == Approx(-1.0 / 8388608.0));
    CHECK(audio.samples[3] == Approx(8388607.0 / 8388608.0));
    CHECK(audio.samples[4] == Approx(-1.0));
}

TEST_CASE("reads AudioDataDLC identity without decoding its coded residual", "[ac3iab]") {
    std::vector<std::byte> opaque{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    auto dlc = wrap_element(0x200, build_dlc_payload(11, opaque));
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {dlc});

    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    REQUIRE(frame->audio_dlc.size() == 1);
    CHECK(frame->audio_dlc.front().audio_data_id == 11);
    CHECK(frame->audio_dlc.front().coded == opaque);
}

TEST_CASE("parses AuthoringToolInfo and UserData", "[ac3iab]") {
    auto info = wrap_element(0x100, build_authoring_tool_payload("https://example.test/tool"));
    auto user = wrap_element(0x101, build_user_data_payload());
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {info, user});

    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    REQUIRE(frame->authoring_tool.has_value());
    CHECK(frame->authoring_tool->uri == "https://example.test/tool");
    REQUIRE(frame->user_data.size() == 1);
    CHECK(frame->user_data.front().data.size() == 2);
}

TEST_CASE("skips an ElementID this reader does not recognize", "[ac3iab]") {
    auto unknown = wrap_element(0x999, std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}});
    auto bed = wrap_element(0x10, build_bed_payload(4));
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {unknown, bed});

    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    CHECK(frame->beds.size() == 1);
}

TEST_CASE("ignores a Child element type not allowed in its Parent context", "[ac3iab]") {
    // §9 Table 4: a BedDefinition's Children are BedDefinition/BedRemap only - an
    // ObjectDefinition-tagged element inside a BedDefinition's own SubElementCount loop shall
    // be ignored.
    BitWriter bw;
    bw.push_plex(1, 8);
    bw.push_bits(0, 1);
    bw.push_plex(0, 4);  // ChannelCount = 0
    bw.push_bits(0x180, 10);
    bw.align_to_byte();
    bw.push_bits(0x01, 8);
    bw.push_plex(1, 8);  // SubElementCount = 1
    auto bed_bytes = bw.bytes();
    append(bed_bytes, wrap_element(0x40, build_object_payload(4)));  // a misplaced ObjectDefinition

    auto bed = wrap_element(0x10, bed_bytes);
    auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {bed});

    auto frame = ac3iab::parse_iaframe(payload);
    REQUIRE(frame.has_value());
    REQUIRE(frame->beds.size() == 1);
    CHECK(frame->beds.front().beds.empty());
    CHECK(frame->beds.front().remaps.empty());
}

TEST_CASE("rejects a bad PreambleTag/IAFrameTag and a truncated stream", "[ac3iab]") {
    auto good = build_iabitstream(build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {}));

    SECTION("bad PreambleTag") {
        auto bytes = good;
        bytes[0] = std::byte{0x00};
        std::istringstream in(to_string(bytes));
        auto result = ac3iab::parse_iabitstream(in);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3iab::IabError::kBadPreambleTag);
    }

    SECTION("bad IAFrameTag") {
        auto bytes = good;
        bytes[5] = std::byte{0x00};  // byte 5: right after the 5-byte Preamble header
        std::istringstream in(to_string(bytes));
        auto result = ac3iab::parse_iabitstream(in);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3iab::IabError::kBadFrameTag);
    }

    SECTION("truncated") {
        auto bytes = good;
        bytes.resize(bytes.size() - 3);
        std::istringstream in(to_string(bytes));
        auto result = ac3iab::parse_iabitstream(in);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3iab::IabError::kTruncated);
    }
}

TEST_CASE("rejects Reserved Version/SampleRate/BitDepth/FrameRate codes", "[ac3iab]") {
    SECTION("Version") {
        auto payload = build_iaframe_payload(kSampleRate48k, kBitDepth24, kFrameRate48Fps, {});
        payload[0] = std::byte{0};  // Version, the payload's first byte
        auto frame = ac3iab::parse_iaframe(payload);
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error() == ac3iab::IabError::kReservedVersion);
    }

    SECTION("SampleRate") {
        auto frame = ac3iab::parse_iaframe(build_iaframe_payload(0x2, kBitDepth24, kFrameRate48Fps, {}));
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error() == ac3iab::IabError::kReservedSampleRate);
    }

    SECTION("BitDepth") {
        auto frame = ac3iab::parse_iaframe(build_iaframe_payload(kSampleRate48k, 0x3, kFrameRate48Fps, {}));
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error() == ac3iab::IabError::kReservedBitDepth);
    }

    SECTION("FrameRate") {
        auto frame = ac3iab::parse_iaframe(build_iaframe_payload(kSampleRate48k, kBitDepth24, 0xA, {}));
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error() == ac3iab::IabError::kReservedFrameRate);
    }
}
