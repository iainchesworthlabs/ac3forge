#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "ac3/admbridge/coordinates.hpp"
#include "ac3/admbridge/iab_bridge.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3iab/ac3iab.hpp"
#include "ac3iab/model.hpp"

// ac3::admbridge::build_iab - roadmap item IM1 phase 3 ("IAB (SMPTE ST 2098-2) reader", see
// ROADMAP.md). Most cases here construct ac3iab::IABitstreamFrame/IaFrame/BedDefinition/
// ObjectDefinition values directly (plain aggregates, per ac3iab/model.hpp's own design - no
// parser needed to build one), the same "construct the model directly, no byte-level round trip"
// approach test_adm_bridge.cpp already uses for its own non-flagship cases. The one flagship test
// at the bottom goes through a REAL byte-level IABitstream fixture and ac3iab::parse_iabitstream()
// end to end, then through a real ac3::oba::AtmosEncoder/ac3::Eac3Decoder round trip, per this
// project's own standard for codec-adjacent behaviour.

namespace {

// §10.2.4 Table 17 / Table 23: 24 fps -> 8 pan sub blocks - used by every non-flagship fixture
// below except the one exercising sub-block timing directly, which picks its own smaller count
// for a simpler fixture (the flagship test likewise uses its own frame rate).
constexpr std::uint8_t kFrameRateCode = 0x0;

ac3iab::IABitstreamFrame make_frame(std::vector<ac3iab::BedDefinition> beds = {},
                                    std::vector<ac3iab::ObjectDefinition> objects = {},
                                    std::uint32_t sample_rate = 48000) {
    ac3iab::IABitstreamFrame entry;
    entry.frame.version = 1;
    entry.frame.sample_rate = sample_rate;
    entry.frame.bit_depth = 16;
    entry.frame.frame_rate_code = kFrameRateCode;
    entry.frame.beds = std::move(beds);
    entry.frame.objects = std::move(objects);
    return entry;
}

ac3iab::BedChannel make_bed_channel(std::uint32_t channel_id, std::uint32_t audio_data_id = 0,
                                    double gain = 1.0) {
    ac3iab::BedChannel channel;
    channel.channel_id = channel_id;
    channel.audio_data_id = audio_data_id;
    channel.gain = gain;
    return channel;
}

ac3iab::BedDefinition make_bed(std::uint32_t meta_id, std::vector<ac3iab::BedChannel> channels,
                               bool conditional = false) {
    ac3iab::BedDefinition bed;
    bed.meta_id = meta_id;
    bed.activation.conditional = conditional;
    bed.channels = std::move(channels);
    return bed;
}

ac3iab::ObjectPanSubBlock make_sub_block(double x, double y, double z, double gain = 1.0) {
    ac3iab::ObjectPanSubBlock block;
    block.has_pan_info = true;
    block.gain = gain;
    block.position = {.x = x, .y = y, .z = z};
    return block;
}

ac3iab::ObjectDefinition make_object(std::uint32_t meta_id,
                                     std::vector<ac3iab::ObjectPanSubBlock> sub_blocks,
                                     std::uint32_t audio_data_id = 0, bool conditional = false) {
    ac3iab::ObjectDefinition object;
    object.meta_id = meta_id;
    object.audio_data_id = audio_data_id;
    object.activation.conditional = conditional;
    object.sub_blocks = std::move(sub_blocks);
    return object;
}

ac3iab::AudioDataPcm make_pcm(std::uint32_t audio_data_id, std::size_t count, float value = 0.0f) {
    ac3iab::AudioDataPcm pcm;
    pcm.audio_data_id = audio_data_id;
    pcm.samples.assign(count, value);
    return pcm;
}

}  // namespace

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

TEST_CASE("iab_position_to_room is a direct passthrough", "[admbridge][iab][coordinates]") {
    // §11.1 vs oamd.hpp's own §4.2.1 comment - see coordinates.hpp's own top comment on why no
    // formula is needed: x/y already share the same convention, and z's zero is the same
    // screen/ear-height reference, just never negative on the IAB side.
    SECTION("front left corner, screen height") {
        const auto room = ac3::admbridge::iab_position_to_room({.x = 0.0, .y = 0.0, .z = 0.0});
        CHECK_THAT(room.x, Catch::Matchers::WithinAbs(0.0, 1e-9));
        CHECK_THAT(room.y, Catch::Matchers::WithinAbs(0.0, 1e-9));
        CHECK_THAT(room.z, Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
    SECTION("middle of ceiling") {
        const auto room = ac3::admbridge::iab_position_to_room({.x = 0.5, .y = 0.5, .z = 1.0});
        CHECK_THAT(room.x, Catch::Matchers::WithinAbs(0.5, 1e-9));
        CHECK_THAT(room.y, Catch::Matchers::WithinAbs(0.5, 1e-9));
        CHECK_THAT(room.z, Catch::Matchers::WithinAbs(1.0, 1e-9));
    }
    SECTION("back right corner, screen height") {
        const auto room = ac3::admbridge::iab_position_to_room({.x = 1.0, .y = 1.0, .z = 0.0});
        CHECK_THAT(room.x, Catch::Matchers::WithinAbs(1.0, 1e-9));
        CHECK_THAT(room.y, Catch::Matchers::WithinAbs(1.0, 1e-9));
        CHECK_THAT(room.z, Catch::Matchers::WithinAbs(0.0, 1e-9));
    }
}

// ---------------------------------------------------------------------------
// Table 19 ChannelID -> BedLabel mapping (indirect - see iab_bridge.cpp's own bed_label_for_
// channel_id comment for the full cited mapping table; tested here through build_iab()'s public
// behaviour, the smallest fixture that isolates it, rather than exposing that lookup itself)
// ---------------------------------------------------------------------------

TEST_CASE("build_iab maps supported Table 19 ChannelIDs to the right BedLabel position",
          "[admbridge][iab]") {
    struct Case {
        std::uint32_t channel_id;
        ac3::oba::BedLabel label;
    };
    // A representative subset, not the full table - see iab_bridge.cpp's own comment for the rest.
    const auto test_case =
        GENERATE(Case{0x0, ac3::oba::BedLabel::kL}, Case{0x2, ac3::oba::BedLabel::kC},
                 Case{0x4, ac3::oba::BedLabel::kR}, Case{0x6, ac3::oba::BedLabel::kLs},
                 Case{0xA, ac3::oba::BedLabel::kRs}, Case{0x7, ac3::oba::BedLabel::kLb},
                 Case{0x8, ac3::oba::BedLabel::kRb}, Case{0x86, ac3::oba::BedLabel::kLfe},
                 Case{0x87, ac3::oba::BedLabel::kLfe2}, Case{0x88, ac3::oba::BedLabel::kLw},
                 Case{0x89, ac3::oba::BedLabel::kRw}, Case{0x80, ac3::oba::BedLabel::kTfl},
                 Case{0x84, ac3::oba::BedLabel::kTsl});
    CAPTURE(test_case.channel_id);

    auto frame = make_frame({make_bed(1, {make_bed_channel(test_case.channel_id)})});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 1);
    CHECK(result->is_bed[0]);

    const auto expected = ac3::oba::bed_label_position(test_case.label);
    const auto placement = result->paths[0].evaluate(0.0);
    CHECK_THAT(placement.position.x, Catch::Matchers::WithinAbs(expected.x, 1e-9));
    CHECK_THAT(placement.position.y, Catch::Matchers::WithinAbs(expected.y, 1e-9));
    CHECK_THAT(placement.position.z, Catch::Matchers::WithinAbs(expected.z, 1e-9));
}

TEST_CASE("build_iab refuses a Table 19 ChannelID with no BedLabel equivalent",
          "[admbridge][iab]") {
    // 0x5 "Left Side Surround": a real Table 19 code, distinct from both "Left Surround" (0x6,
    // mapped to kLs) and "Left Rear Surround" (0x7, mapped to kLb) - see iab_bridge.cpp's own
    // comment on why this third surround zone specifically has no BedLabel slot.
    const auto channel_id = GENERATE(0x5, 0x9, 0x1, 0x3, 0x10, 0x17, 0x7F);
    CAPTURE(channel_id);
    auto frame =
        make_frame({make_bed(1, {make_bed_channel(static_cast<std::uint32_t>(channel_id))})});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kUnsupportedIabChannel);
}

TEST_CASE("build_iab routes an LFE bed channel at gain 0 / lfe_send 1", "[admbridge][iab]") {
    auto frame = make_frame({make_bed(1, {make_bed_channel(0xD /* LFE */)})});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE(result.has_value());
    REQUIRE(result->is_lfe[0]);
    const auto placement = result->paths[0].evaluate(0.0);
    CHECK(placement.gain == 0.0);
    CHECK(placement.lfe_send == 1.0);
}

// ---------------------------------------------------------------------------
// MetaID cross-frame identity (§10.3.1)
// ---------------------------------------------------------------------------

TEST_CASE("build_iab tracks a channel's identity by MetaID across frames", "[admbridge][iab]") {
    // AudioDataID left at its default (0) - legitimate silence per §10.3.6, so this fixture stays
    // focused on identity tracking and does not also need a matching AudioDataPCM element.
    std::vector<ac3iab::IABitstreamFrame> frames;
    frames.push_back(make_frame({make_bed(7, {make_bed_channel(0x2)})}));
    frames.push_back(make_frame({make_bed(7, {make_bed_channel(0x2)})}));
    frames.push_back(make_frame({make_bed(7, {make_bed_channel(0x2)})}));

    const auto result = ac3::admbridge::build_iab(frames);
    REQUIRE(result.has_value());
    // Same MetaID+ChannelID every frame -> ONE channel, not three.
    CHECK(result->channel_count() == 1);
}

TEST_CASE("build_iab silence-fills a frame where a channel is absent", "[admbridge][iab]") {
    const auto samples_per_frame = *ac3iab::sample_count(kFrameRateCode, false);

    std::vector<ac3iab::IABitstreamFrame> frames;
    auto present = make_frame({make_bed(1, {make_bed_channel(0x2, 1)})});
    present.frame.audio_pcm.push_back(make_pcm(1, samples_per_frame, 0.5f));
    frames.push_back(present);
    frames.push_back(make_frame());  // the bed is entirely absent this frame

    const auto result = ac3::admbridge::build_iab(frames);
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 1);
    REQUIRE(result->pcm[0].size() == 2 * samples_per_frame);
    for (std::size_t i = 0; i < samples_per_frame; ++i) {
        CHECK(result->pcm[0][i] == 0.5f);
    }
    for (std::size_t i = samples_per_frame; i < 2 * samples_per_frame; ++i) {
        CHECK(result->pcm[0][i] == 0.0f);
    }
}

TEST_CASE("build_iab excludes a conditionally-Activated Bed from the channel set",
          "[admbridge][iab]") {
    auto frame = make_frame({make_bed(1, {make_bed_channel(0x2)}, /*conditional=*/true)});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE(result.has_value());
    CHECK(result->channel_count() == 0);
}

// ---------------------------------------------------------------------------
// Object position / gain
// ---------------------------------------------------------------------------

TEST_CASE("build_iab places an Object via iab_position_to_room and its own gain",
          "[admbridge][iab]") {
    auto frame = make_frame({}, {make_object(2, {make_sub_block(0.9, 0.5, 0.0, 0.5)})});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 1);
    CHECK_FALSE(result->is_bed[0]);

    // Sub block 0 of 8 -> keyframe at 1/8 of the frame's own duration (its own END - see
    // iab_bridge.cpp's own comment on why - not its start). Evaluated well inside that span, so a
    // KeyframePath clamped to a single keyframe (holding everywhere) still reads the same value -
    // see the dedicated timing test below for a fixture that actually distinguishes "at the
    // sub-block's end" from "at its start".
    const auto duration =
        static_cast<double>(*ac3iab::sample_count(kFrameRateCode, false)) / 48000.0;
    const auto placement = result->paths[0].evaluate(duration / 8.0);
    CHECK_THAT(placement.position.x, Catch::Matchers::WithinAbs(0.9, 1e-9));
    CHECK_THAT(placement.position.y, Catch::Matchers::WithinAbs(0.5, 1e-9));
    CHECK_THAT(placement.gain, Catch::Matchers::WithinAbs(0.5, 1e-9));
    CHECK(placement.lfe_send == 0.0);  // objects never route to the LFE by panning
}

TEST_CASE("build_iab places each active Object sub block's keyframe at its own end-time",
          "[admbridge][iab]") {
    // A frame rate with exactly 2 pan sub blocks (§10.2.4 Table 17 / Table 23: 0x8 = 120 fps),
    // both carrying distinct, real pan info - the smallest fixture that can tell "keyframe at
    // sub-block (i+1)/N * duration" apart from "at i/N * duration" (see iab_bridge.cpp's own
    // sub-block loop comment for the citation this proves).
    constexpr std::uint8_t kTwoSubBlockRate = 0x8;
    ac3iab::IABitstreamFrame frame;
    frame.frame.version = 1;
    frame.frame.sample_rate = 48000;
    frame.frame.bit_depth = 16;
    frame.frame.frame_rate_code = kTwoSubBlockRate;
    frame.frame.objects = {
        make_object(2, {make_sub_block(0.0, 0.5, 0.0), make_sub_block(1.0, 0.5, 0.0)})};

    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE(result.has_value());

    const auto duration =
        static_cast<double>(*ac3iab::sample_count(kTwoSubBlockRate, false)) / 48000.0;
    // With the correct "keyframe at (sb+1)/N * duration" formula, the two keyframes land at
    // (duration/2, x=0.0) and (duration, x=1.0). A quarter of the way through the frame is BEFORE
    // the first keyframe, so KeyframePath holds x at 0.0 there; a wrong "at sb/N * duration"
    // formula would instead put keyframes at (0, x=0.0) and (duration/2, x=1.0), which by that
    // point would already be interpolated up to x=0.5 - a clearly different, wrong value, not one
    // that could coincidentally match.
    const auto quarter = result->paths[0].evaluate(duration / 4.0);
    CHECK_THAT(quarter.position.x, Catch::Matchers::WithinAbs(0.0, 1e-9));
    // Three-quarters through: correctly halfway between the two real keyframes (x=0.5); the wrong
    // formula would already be held at its own last keyframe's value (x=1.0) by this point.
    const auto three_quarters = result->paths[0].evaluate(3.0 * duration / 4.0);
    CHECK_THAT(three_quarters.position.x, Catch::Matchers::WithinAbs(0.5, 1e-9));
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST_CASE("build_iab refuses an empty frame span", "[admbridge][iab]") {
    const auto result = ac3::admbridge::build_iab({});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kEmptyIabStream);
}

TEST_CASE("build_iab refuses a non-zero AudioDataID with no matching essence", "[admbridge][iab]") {
    auto frame = make_frame({make_bed(1, {make_bed_channel(0x2, /*audio_data_id=*/9)})});
    // No AudioDataPCM element with audio_data_id == 9 anywhere in the frame.
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kNoIabEssenceForChannel);
}

TEST_CASE("build_iab treats AudioDataID 0 as legitimate silence, not an error",
          "[admbridge][iab]") {
    auto frame = make_frame({make_bed(1, {make_bed_channel(0x2, /*audio_data_id=*/0)})});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE(result.has_value());
    REQUIRE(result->pcm[0].size() == *ac3iab::sample_count(kFrameRateCode, false));
    for (const float sample : result->pcm[0]) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("build_iab refuses more than 15 channels", "[admbridge][iab]") {
    std::vector<ac3iab::BedChannel> channels;
    // Every ChannelID in this loop maps to a distinct BedLabel (§10.3.5's own uniqueness rule -
    // "A ChannelID shall not be indicated more than once within a BedDefinition element"), so this
    // exercises the count cap itself, not the mapping table.
    constexpr std::array<std::uint32_t, 16> kIds = {0x0, 0x2, 0x4,  0x6,  0xA,  0x7,  0x8,  0xD,
                                                    0xE, 0xF, 0x82, 0x83, 0x84, 0x85, 0x88, 0x89};
    for (const auto id : kIds) {
        channels.push_back(make_bed_channel(id));
    }
    auto frame = make_frame({make_bed(1, channels)});
    const auto result = ac3::admbridge::build_iab(std::span{&frame, 1});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::admbridge::BridgeError::kTooManyChannels);
}

namespace {

double channel_energy(std::span<const float> samples) {
    double energy = 0.0;
    for (const auto v : samples) {
        const double sd = static_cast<double>(v);
        energy += sd * sd;
    }
    return energy;
}

// A from-scratch MSB-first bit writer (§5.1), used only to build this test's own byte-level
// fixture - independent of src/ac3iab's own implementation, the same convention
// tests/ac3iab/test_ac3iab.cpp, examples/read_iab.cpp and examples/encode_iab.cpp already
// establish.
class BitWriter {
   public:
    void push_bits(std::uint64_t value, unsigned width) {
        for (unsigned i = 0; i < width; ++i) {
            push_bit(static_cast<unsigned>((value >> (width - 1 - i)) & 0x1u));
        }
    }

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

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

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

std::vector<std::byte> wrap_element(std::uint32_t id, const std::vector<std::byte>& payload) {
    BitWriter bw;
    bw.push_plex(id, 8);
    bw.push_plex(payload.size(), 8);
    auto bytes = bw.bytes();
    append(bytes, payload);
    return bytes;
}

constexpr std::uint8_t kFlagshipFrameRateCode = 0x8;  // 120 fps -> 2 pan sub blocks
constexpr std::uint32_t kFlagshipSamplesPerIabFrame = 400;
constexpr unsigned kFlagshipTotalFrames = 20;
// §5.4: valid ObjectPosX/Y domain is the upper half of the 16-bit range - see
// examples/encode_iab.cpp's identical comment for the citation.
constexpr std::uint16_t kPosMin = 0x8000;
constexpr std::uint16_t kPosMax = 0xFFFF;

std::vector<std::byte> build_bed_payload() {
    BitWriter bw;
    bw.push_plex(1, 8);
    bw.push_bits(0, 1);
    bw.push_plex(1, 4);
    bw.push_plex(0x2, 4);  // Center
    bw.push_plex(1, 8);
    bw.push_bits(0, 2);
    bw.push_bits(0, 1);
    bw.push_bits(0x180, 10);
    bw.align_to_byte();
    bw.push_bits(0x01, 8);
    bw.push_plex(0, 8);
    return bw.bytes();
}

std::vector<std::byte> build_object_payload(std::uint16_t pos_x, std::uint16_t pos_y) {
    BitWriter bw;
    bw.push_plex(2, 8);
    bw.push_plex(2, 8);
    bw.push_bits(0, 1);
    bw.push_bits(0, 1);

    bw.push_bits(0, 2);
    bw.push_bits(0b001, 3);
    bw.push_bits(pos_x, 16);
    bw.push_bits(pos_y, 16);
    bw.push_bits(0, 16);
    bw.push_bits(0, 1);
    bw.push_bits(0, 1);
    bw.push_bits(0x1, 2);
    bw.push_bits(0, 4);
    bw.push_bits(0, 2);

    bw.push_bits(0, 1);  // sub block 1: PanInfoExists = 0 (carried forward)

    bw.align_to_byte();
    bw.push_bits(0x01, 8);
    bw.push_plex(0, 8);
    return bw.bytes();
}

std::vector<std::byte> build_pcm_payload(std::uint32_t audio_data_id,
                                         const std::vector<std::int16_t>& samples) {
    BitWriter bw;
    bw.push_plex(audio_data_id, 8);
    for (const auto sample : samples) {
        bw.push_raw_byte(static_cast<unsigned char>(static_cast<std::uint16_t>(sample) & 0xFFu));
        bw.push_raw_byte(
            static_cast<unsigned char>((static_cast<std::uint16_t>(sample) >> 8) & 0xFFu));
    }
    return bw.bytes();
}

std::vector<std::byte> build_iaframe(unsigned index) {
    const bool right_half = index < kFlagshipTotalFrames / 2;
    const std::uint16_t pos_x = right_half ? kPosMax : kPosMin;
    constexpr std::uint16_t kPosYMid = 0xC000;

    std::vector<std::int16_t> bed_samples(kFlagshipSamplesPerIabFrame);
    std::vector<std::int16_t> object_samples(kFlagshipSamplesPerIabFrame);
    for (std::uint32_t i = 0; i < kFlagshipSamplesPerIabFrame; ++i) {
        const double t = static_cast<double>(index * kFlagshipSamplesPerIabFrame + i) / 48000.0;
        bed_samples[i] =
            static_cast<std::int16_t>(0.3 * 32767.0 * std::sin(2.0 * std::numbers::pi * 300.0 * t));
        object_samples[i] =
            static_cast<std::int16_t>(0.3 * 32767.0 * std::sin(2.0 * std::numbers::pi * 800.0 * t));
    }

    BitWriter header;
    header.push_bits(1, 8);
    header.push_bits(0, 2);
    header.push_bits(0, 2);
    header.push_bits(kFlagshipFrameRateCode, 4);
    header.push_plex(0, 8);
    header.align_to_byte();
    header.push_plex(4, 8);

    std::vector<std::byte> iaframe_payload = header.bytes();
    append(iaframe_payload, wrap_element(0x10, build_bed_payload()));
    append(iaframe_payload, wrap_element(0x40, build_object_payload(pos_x, kPosYMid)));
    append(iaframe_payload, wrap_element(0x400, build_pcm_payload(1, bed_samples)));
    append(iaframe_payload, wrap_element(0x400, build_pcm_payload(2, object_samples)));

    std::vector<std::byte> segment;
    segment.push_back(std::byte{0x01});
    for (int shift : {24, 16, 8, 0}) {
        segment.push_back(static_cast<std::byte>((0u >> shift) & 0xFFu));
    }
    segment.push_back(std::byte{0x02});
    const auto element = wrap_element(0x08, iaframe_payload);
    const auto frame_length = static_cast<std::uint32_t>(element.size());
    for (int shift : {24, 16, 8, 0}) {
        segment.push_back(static_cast<std::byte>((frame_length >> shift) & 0xFFu));
    }
    append(segment, element);
    return segment;
}

}  // namespace

TEST_CASE(
    "a real IAB fixture's bed and moving object survive admbridge into a real "
    "AtmosEncoder bitstream",
    "[admbridge][iab][atmos]") {
    std::vector<std::byte> file;
    for (unsigned i = 0; i < kFlagshipTotalFrames; ++i) {
        append(file, build_iaframe(i));
    }
    std::string view(reinterpret_cast<const char*>(file.data()), file.size());
    std::istringstream in(std::move(view), std::ios::binary);

    const auto frames = ac3iab::parse_iabitstream(in);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == kFlagshipTotalFrames);

    const auto result = ac3::admbridge::build_iab(*frames);
    REQUIRE(result.has_value());
    REQUIRE(result->channel_count() == 2);
    CHECK(result->is_bed[0]);
    CHECK_FALSE(result->is_bed[1]);
    CHECK(result->sample_rate == 48000);

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448},
                                   static_cast<int>(result->channel_count())};
    ac3::Eac3Decoder decoder;
    std::vector<std::span<const float>> views(result->channel_count());

    constexpr int kFrame = ac3::kSamplesPerFrame;
    const auto total_samples = result->pcm.front().size();
    const auto total_ac3_frames = total_samples / static_cast<std::size_t>(kFrame);
    REQUIRE(total_ac3_frames >= 3);  // real content, more than one AC-3 frame - CONTRIBUTING's rule

    // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.
    constexpr int kLCh = 0;
    constexpr int kCCh = 1;
    constexpr int kRCh = 2;
    constexpr int kSLCh = 3;
    constexpr int kSRCh = 4;

    for (std::size_t f = 0; f < total_ac3_frames; ++f) {
        const auto start = f * static_cast<std::size_t>(kFrame);
        for (std::size_t ch = 0; ch < result->channel_count(); ++ch) {
            views[ch] = std::span<const float>(result->pcm[ch])
                            .subspan(start, static_cast<std::size_t>(kFrame));
        }
        const double t = static_cast<double>(start + static_cast<std::size_t>(kFrame)) / 48000.0;
        const auto placement = ac3::oba::evaluate_placements(result->paths, t);
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());

        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());

        const double energy_l = channel_energy((*decoded)->channels[kLCh]);
        const double energy_c = channel_energy((*decoded)->channels[kCCh]);
        const double energy_r = channel_energy((*decoded)->channels[kRCh]);
        const double energy_sl = channel_energy((*decoded)->channels[kSLCh]);
        const double energy_sr = channel_energy((*decoded)->channels[kSRCh]);
        CAPTURE(f, energy_l, energy_c, energy_r, energy_sl, energy_sr);

        // The bed (Center) is static throughout: C should always carry real energy.
        CHECK(energy_c > 1.0);

        // The object pans from hard right (x=1.0) to hard left (x=0.0) at the fixture's own
        // midpoint - checked on the settled half of each side, mirroring
        // test_adm_bridge.cpp's own "last frame of a hold, not mid-transition" convention.
        const bool object_on_right =
            static_cast<double>(f) < static_cast<double>(total_ac3_frames) / 2.0 - 1.0;
        const bool object_on_left =
            static_cast<double>(f) > static_cast<double>(total_ac3_frames) / 2.0 + 1.0;
        if (object_on_right) {
            CHECK(energy_r + energy_sr > energy_l + energy_sl);
        } else if (object_on_left) {
            CHECK(energy_l + energy_sl > energy_r + energy_sr);
        }
    }
}
