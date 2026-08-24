#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/decoder/decoder.hpp"  // split_frames, to lift a dependent out of an access unit
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/oba/atmos.hpp"

namespace {

std::vector<std::vector<float>> tone(int channels) {
    std::vector<std::vector<float>> pcm(
        static_cast<std::size_t>(channels),
        std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    for (auto& channel : pcm) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            channel[static_cast<std::size_t>(i)] = static_cast<float>(
                0.4 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
        }
    }
    return pcm;
}

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// numblkscod sits at bits 34-35 of an E-AC-3 syncframe whose fscod is not
// 0x3 (16 syncword + 2 strmtyp + 3 substreamid + 11 frmsiz + 2 fscod), which
// is byte 4, bits 2-3 counting from the MSB. Rewriting it and re-stamping
// crc2 is how the short-access-unit fixtures below exist at all - see their
// own comments for why a header-only fixture is the right shape there.
void set_numblkscod(std::span<std::byte> frame, int numblkscod) {
    auto byte = std::to_integer<std::uint8_t>(frame[4]);
    byte = static_cast<std::uint8_t>((byte & 0xCF) |
                                     ((static_cast<unsigned>(numblkscod) & 0x3u) << 4));
    frame[4] = std::byte{byte};
    REQUIRE(ac3::io::restamp_crc(frame).has_value());
// `frames` access units of real, multi-frame object audio - not
// build_silent_access_unit: addbsi parsing has to land on the right bit
// offset regardless of what audio rides along after it, and going through
// the real encoder is what this project's tests do whenever an actual
// encoder is available for the case (see "scan reads an AC-3 elementary
// stream" above). `emit_objects` picks AtmosConfig::emit_object_metadata,
// which decides both the EMDF container and the TS 103 420 §8.3.1 addbsi
// marker that goes with it.
std::vector<std::byte> atmos_stream(bool emit_objects, int objects, int frames) {
    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .emit_object_metadata = emit_objects}, objects};
    std::vector<std::vector<float>> sources(static_cast<std::size_t>(objects),
                                            std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views;
    for (auto& source : sources) {
        views.emplace_back(source);
    }
    std::vector<ac3::oba::ObjectPlacement> placement(
        static_cast<std::size_t>(objects),
        {.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain = 1.0});

    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        for (int obj = 0; obj < objects; ++obj) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (f * ac3::kSamplesPerFrame + n) / 48000.0;
                sources[static_cast<std::size_t>(obj)][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi *
                                                      (440.0 * static_cast<double>(obj + 1)) * t));
            }
        }
        const auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        append(stream, unit->bytes);
    }
    return stream;
}

}  // namespace

TEST_CASE("scan reads an AC-3 elementary stream", "[elementary]") {
    // Real audio, so the frames are full rather than mostly padding - a
    // sizing bug that only shows on a full frame would otherwise hide.
    ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    std::vector<std::byte> stream;
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    for (int f = 0; f < 4; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    CHECK(scanned->kind == ac3::io::StreamKind::kAc3);
    CHECK(scanned->sample_rate == ac3::SampleRate::k48000);
    CHECK(scanned->acmod == ac3::Acmod::k3_2);
    CHECK(scanned->lfe);
    CHECK(scanned->channels == 6);
    CHECK(scanned->access_units.size() == 4);
    // AC-3 has no substreams: one syncframe is one access unit.
    CHECK(scanned->substreams_per_unit == 1);
}

TEST_CASE("scan reads E-AC-3 substream layouts", "[elementary]") {
    using ac3::eac3::AccessUnitConfig;
    using ac3::eac3::FrameConfig;
    namespace cm = ac3::eac3::chanmap;

    const AccessUnitConfig bed{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};

    // 7.1: the dependent's Ls/Rs overwrite the bed's and Lrs/Rrs extend it, so
    // ten coded channels render as eight. Counting locations rather than
    // channels is the only way to get that right.
    AccessUnitConfig seven_one = bed;
    seven_one.dependents.push_back({.bitrate_kbps = 224,
                                    .acmod = ac3::Acmod::k2_2,
                                    .chanmap = cm::k71Rear});

    // 5.1.4: four ceiling channels, nothing replaced.
    AccessUnitConfig five_one_four = bed;
    five_one_four.dependents.push_back({.bitrate_kbps = 224,
                                        .acmod = ac3::Acmod::k2_2,
                                        .chanmap = cm::kTopQuad});

    // 7.1.4 needs two dependents - six new channels, one more than a single
    // substream can carry.
    AccessUnitConfig seven_one_four = seven_one;
    seven_one_four.dependents.push_back({.bitrate_kbps = 224,
                                         .acmod = ac3::Acmod::k2_2,
                                         .chanmap = cm::kTopQuad});

    // The DASH AudioChannelConfiguration @value each of these renders as on
    // the Dolby scheme (TS 102 366 clause I.1.2.1): four hex digits of the
    // same Table E2.5 word, left channel in the MOST significant bit. 5.1 is
    // the value the spec itself works through - "for a stream with L, C, R,
    // Ls, Rs, LFE, the value shall be 'F801'" - and the rest follow from the
    // bit positions: +Lrs/Rrs (bit 6, 0x0200) for 7.1, +Vhl/Vhr (bit 11,
    // 0x0010) and Lts/Rts (bit 13, 0x0004) for the ceiling quad.
    struct Case {
        AccessUnitConfig config;
        int channels;
        std::size_t substreams;
        std::uint16_t map;
        std::string_view dash_value;
    };
    using namespace ac3::eac3::chanmap;
    constexpr std::uint16_t k51 =
        kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit | kLfeBit;
    for (const auto& [config, channels, substreams, map, dash_value] :
         {Case{bed, 6, 1, k51, "F801"},
          Case{seven_one, 8, 2, static_cast<std::uint16_t>(k51 | cm::k71Rear), "FA01"},
          Case{five_one_four, 10, 2, static_cast<std::uint16_t>(k51 | cm::kTopQuad), "F815"},
          Case{seven_one_four, 12, 3,
               static_cast<std::uint16_t>(k51 | cm::k71Rear | cm::kTopQuad), "FA15"}}) {
        const auto unit = ac3::eac3::build_silent_access_unit(config);
        REQUIRE(unit.has_value());
        std::vector<std::byte> stream;
        for (int f = 0; f < 3; ++f) {
            append(stream, unit->bytes);
        }

        const auto scanned = ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        CHECK(scanned->kind == ac3::io::StreamKind::kEac3);
        CHECK(scanned->channels == channels);
        CHECK(scanned->substreams_per_unit == substreams);
        // The location word `channels` is the count OF - kept rather than
        // discarded, since which locations is a different question from how
        // many (ScannedStream::channel_map).
        CHECK(scanned->channel_map == map);
        CHECK(ac3::eac3::chanmap::channel_count(scanned->channel_map) == channels);
        CHECK(ac3::io::dash_channel_configuration(*scanned) == dash_value);
        // Substreams group into access units, not one frame each.
        CHECK(scanned->access_units.size() == 3);
        CHECK(scanned->access_units.front().size() == unit->bytes.size());
    }
}

TEST_CASE("scan tells AC-3 and E-AC-3 apart by bsid", "[elementary]") {
    // Both formats spend exactly 40 bits before bsid, which is what makes
    // identifying the stream possible without being told which it is.
    const auto legacy = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(legacy.has_value());
    const auto plus = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(plus.has_value());

    const auto a = ac3::io::scan(*legacy);
    REQUIRE(a.has_value());
    CHECK(a->kind == ac3::io::StreamKind::kAc3);

    const auto e = ac3::io::scan(*plus);
    REQUIRE(e.has_value());
    CHECK(e->kind == ac3::io::StreamKind::kEac3);

    // Both open on the same sync word, so the discrimination really is coming
    // from bsid and not from anything earlier in the frame.
    CHECK(std::to_integer<std::uint8_t>(legacy->at(0)) == 0x0B);
    CHECK(std::to_integer<std::uint8_t>(plus->at(0)) == 0x0B);
}

TEST_CASE("scan refuses what it cannot read", "[elementary]") {
    using ac3::io::ScanError;
    CHECK(ac3::io::scan({}).error() == ScanError::kEmpty);

    const auto frame = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());

    // A stream that does not start on a sync word is not a stream.
    std::vector<std::byte> unsynced{std::byte{0x00}, std::byte{0x00}};
    unsynced.insert(unsynced.end(), frame->begin(), frame->end());
    CHECK(ac3::io::scan(unsynced).error() == ScanError::kLostSync);

    // A frame cut short must be reported, not silently returned as a short
    // access unit - a muxer that packetised it would produce a broken file.
    const std::vector<std::byte> cut{frame->begin(), frame->end() - 64};
    CHECK(ac3::io::scan(cut).error() == ScanError::kTruncated);

    // Trailing garbage after a whole frame is a lost sync, not a silent stop.
    std::vector<std::byte> trailing{frame->begin(), frame->end()};
    trailing.insert(trailing.end(), 8, std::byte{0xAB});
    CHECK(ac3::io::scan(trailing).error() == ScanError::kLostSync);

    // A dependent substream with no independent parent ahead of it must be
    // refused rather than scanned as though it were the bed - the same
    // guard ac3::split_access_units applies on the decode side
    // (decoder.cpp), for the same reason: its channels have nothing to
    // extend, and its bsi fields (acmod, bsid, ...) describe an extension,
    // not a complete programme.
    using ac3::eac3::AccessUnitConfig;
    namespace cm = ac3::eac3::chanmap;
    const AccessUnitConfig seven_one{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 224, .acmod = ac3::Acmod::k2_2, .chanmap = cm::k71Rear}}};
    const auto unit = ac3::eac3::build_silent_access_unit(seven_one);
    REQUIRE(unit.has_value());
    REQUIRE(unit->substream_count() == 2);
    const auto dependent_bytes = unit->substream(1);
    const std::vector<std::byte> lone_dependent{dependent_bytes.begin(), dependent_bytes.end()};
    CHECK(ac3::io::scan(lone_dependent).error() == ScanError::kUnsupportedStructure);
}

// bsid/bsmod/bit_rate_code and the TS 103 420 addbsi Atmos marker exist
// purely for ac3::io::build_codec_config_box() (ac3/io/dec3.hpp) to build a
// spec-correct dac3/dec3 box - see tests/containers/test_mp4.cpp for the box byte
// layout itself. These check the values scan() reports for them.
TEST_CASE("scan reads bsid/bsmod/bit_rate_code straight off the bsi", "[elementary]") {
    // A/52 encoder.cpp always writes bsid 8; real audio, not silence, per
    // this project's own validation discipline - a bit offset error inside
    // bsi could otherwise still land on a plausible-looking bsmod by luck.
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    auto pcm = tone(2);
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};
    std::vector<std::byte> stream;
    for (int f = 0; f < 4; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    CHECK(scanned->bsid == 8);
    CHECK(scanned->bsmod == 0);  // "main audio service: complete main" - the default
    // 192 kbps is kBitratesKbps[10] (Table 5.18); frmsizecod's own top bits
    // are exactly that index.
    CHECK(scanned->bit_rate_code == 10);
    CHECK_FALSE(scanned->oba_complexity_index.has_value());
}

TEST_CASE("scan reads the addbsi Dolby Atmos marker", "[elementary]") {
    constexpr int kObjects = 2;
    const auto stream = atmos_stream(/*emit_objects=*/true, kObjects, 3);

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->oba_complexity_index.has_value());
    // §8.3.2.2: object_count is bed-first (the LFE, always present in this
    // encoder's dynamic-only program) then the dynamic objects.
    CHECK(*scanned->oba_complexity_index == kObjects + 1);
}

TEST_CASE("access-unit timing is the absolute sample position, not a running sum",
          "[elementary]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    std::vector<std::byte> stream;
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    for (int f = 0; f < 5; ++f) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
    }
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_unit_samples.size() == 5);

    // §5.3.1: every AC-3 syncframe is six blocks of 256.
    for (const auto samples : scanned->access_unit_samples) {
        CHECK(samples == 1536);
    }
    CHECK(ac3::io::stream_duration_samples(*scanned) == 5 * 1536);
    CHECK(ac3::io::stream_duration_seconds(*scanned) == Catch::Approx(5 * 1536 / 48000.0));
    CHECK(ac3::io::uniform_access_unit_samples(*scanned) == 1536);

    const auto third = ac3::io::access_unit_timing(*scanned, 2);
    REQUIRE(third.has_value());
    CHECK(third->start_sample == 2 * 1536);
    CHECK(third->duration_samples == 1536);
    CHECK(third->sample_rate == 48000);
    CHECK(third->start_seconds() == Catch::Approx(2 * 1536 / 48000.0));
    // 1536 samples at 48 kHz is exactly 2880 ticks of a 90 kHz clock, so this
    // one is checkable by hand.
    CHECK(third->start_in_timescale(90'000) == 2 * 2880);
    CHECK(third->duration_in_timescale(90'000) == 2880);

    CHECK_FALSE(ac3::io::access_unit_timing(*scanned, 5).has_value());

    // The lookup a cut uses: a position anywhere inside a unit names that
    // whole unit, never a split.
    CHECK(ac3::io::access_unit_at_sample(*scanned, 0) == 0u);
    CHECK(ac3::io::access_unit_at_sample(*scanned, 1535) == 0u);
    CHECK(ac3::io::access_unit_at_sample(*scanned, 1536) == 1u);
    CHECK(ac3::io::access_unit_at_seconds(*scanned, 0.1) == 3u);  // 4800 samples
    CHECK_FALSE(ac3::io::access_unit_at_sample(*scanned, 5 * 1536).has_value());
    CHECK_FALSE(ac3::io::access_unit_at_seconds(*scanned, -1.0).has_value());
}

TEST_CASE("a short-syncframe E-AC-3 stream reports its real access-unit length",
          "[elementary]") {
    // numblkscod 0/1/2 (§E2.3.1.4) - 1, 2 or 3 blocks instead of six - is
    // legal Annex E that this project's own encoder never emits (it writes
    // numblkscod 3 unconditionally), so the fixture is a real encoded
    // syncframe with those two header bits rewritten and its crc2 re-stamped.
    // That is enough for the question under test: scan() reads the header and
    // never decodes an audblk, and the frame's own frmsiz - which is what
    // delimits it - is untouched. Its audio would not decode, and nothing
    // here asks it to.
    using ac3::eac3::AccessUnitConfig;
    AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};

    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());
    std::vector<std::byte> stream;
    for (int i = 0; i < 4; ++i) {
        append(stream, unit->bytes);
    }
    const auto frame_bytes = unit->bytes.size();
    for (std::size_t i = 0; i < 4; ++i) {
        set_numblkscod(std::span{stream}.subspan(i * frame_bytes, frame_bytes), 2);
    }

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_unit_samples.size() == 4);
    for (const auto samples : scanned->access_unit_samples) {
        CHECK(samples == 3 * 256);
    }
    CHECK(ac3::io::uniform_access_unit_samples(*scanned) == 768u);
    CHECK(ac3::io::stream_duration_samples(*scanned) == 4 * 768);
    const auto second = ac3::io::access_unit_timing(*scanned, 1);
    REQUIRE(second.has_value());
    CHECK(second->start_sample == 768);
    CHECK(second->duration_samples == 768);
}

TEST_CASE("a stream whose access units differ in length has no uniform figure",
          "[elementary]") {
    // Same header-level fixture as above: one six-block unit, one three-block
    // unit, one six-block unit.
    using ac3::eac3::AccessUnitConfig;
    AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());
    const auto frame_bytes = unit->bytes.size();

    std::vector<std::byte> stream;
    for (int i = 0; i < 3; ++i) {
        append(stream, unit->bytes);
    }
    set_numblkscod(std::span{stream}.subspan(frame_bytes, frame_bytes), 2);

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_unit_samples.size() == 3);
    CHECK(scanned->access_unit_samples[0] == 1536);
    CHECK(scanned->access_unit_samples[1] == 768);
    CHECK(scanned->access_unit_samples[2] == 1536);
    // No single samples_per_frame describes this, and saying otherwise is
    // what would put every timestamp after the second unit in the wrong place.
    CHECK_FALSE(ac3::io::uniform_access_unit_samples(*scanned).has_value());
    CHECK(ac3::io::stream_duration_samples(*scanned) == 1536 + 768 + 1536);

    const auto third = ac3::io::access_unit_timing(*scanned, 2);
    REQUIRE(third.has_value());
    CHECK(third->start_sample == 1536 + 768);
}

TEST_CASE("access-unit timing counts a whole access unit, dependents included",
          "[elementary]") {
    using ac3::eac3::AccessUnitConfig;
    namespace cm = ac3::eac3::chanmap;
    AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true};
    config.dependents.push_back(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .chanmap = cm::k512Height});
    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());

    std::vector<std::byte> stream;
    append(stream, unit->bytes);
    append(stream, unit->bytes);

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    // Two access units of two syncframes each - the dependent does not add
    // its own 1536 samples, it codes the same ones.
    REQUIRE(scanned->access_units.size() == 2);
    REQUIRE(scanned->access_unit_samples.size() == 2);
    CHECK(ac3::io::stream_duration_samples(*scanned) == 2 * 1536);
}

// The other half of the objects-or-nothing fallback rule (docs/concepts/
// atmos-joc.md, "The fallback rule: objects, or nothing"; AtmosConfig::
// emit_object_metadata's own comment). bed51 omits the EMDF container so the
// bed stays playable on a decoder that validates emdf_protection - and it
// must omit TS 103 420 §8.3.1's addbsi marker with it, because that marker is
// the ONLY thing a reader has to go on: ac3::io::build_codec_config_box writes
// the dec3 box's Atmos extension off it, `ac3cli fmp4` writes CHANNELS="<N>/JOC"
// off it, and FFmpeg reports "Dolby Digital Plus + Dolby Atmos" off it. Left
// in, all three would advertise an object layer that was never encoded - a
// promise as empty as an empty container would be.
TEST_CASE("scan reports no Atmos marker for a bed51 stream", "[elementary]") {
    const auto stream = atmos_stream(/*emit_objects=*/false, 2, 3);

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    // Still an ordinary 5.1 E-AC-3 stream, which is the whole point of the
    // mode - it is only the object layer that is gone.
    CHECK(scanned->kind == ac3::io::StreamKind::kEac3);
    CHECK(scanned->acmod == ac3::Acmod::k3_2);
    CHECK(scanned->lfe);
    CHECK_FALSE(scanned->oba_complexity_index.has_value());
}

// A/52 §E2.3.1.2: "If an AC-3 bit stream is present in the E-AC-3 bit stream,
// then the AC-3 bit stream shall be processed as an independent substream
// assigned substream ID 0." Built the way the real ones are - an AC-3
// syncframe carrying the 5.1 bed, with the DEPENDENT substream of an ordinary
// E-AC-3 7.1 access unit placed immediately behind it. The dependent is
// lifted out of a real access unit rather than hand-rolled so that its syntax
// is exactly what the encoder emits; only its company changes.
namespace {

std::vector<std::byte> legacy_core_stream(int access_units) {
    ac3::FrameEncoder core{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    config.dependents.push_back({.bitrate_kbps = 224,
                                 .acmod = ac3::Acmod::k2_2,
                                 .chanmap = ac3::eac3::chanmap::k71Rear});
    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());
    const auto frames = ac3::split_frames(unit->bytes);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 2);
    const auto dependent = (*frames)[1];

    std::vector<std::byte> stream;
    for (int f = 0; f < access_units; ++f) {
        const auto frame = core.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
        append(stream, dependent);
    }
    return stream;
}

}  // namespace

TEST_CASE("scan reads an AC-3 core with E-AC-3 extension substreams", "[elementary]") {
    const auto stream = legacy_core_stream(3);

    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    // Neither of the two plain kinds: the first syncframe is AC-3, so a reader
    // dispatching on bsid alone calls it AC-3 and then chokes on the
    // dependent - which is exactly the failure this kind exists to prevent.
    CHECK(scanned->kind == ac3::io::StreamKind::kAc3CoreEac3Extension);
    CHECK(scanned->sample_rate == ac3::SampleRate::k48000);
    // acmod/lfe describe the CORE, which is the independent substream.
    CHECK(scanned->acmod == ac3::Acmod::k3_2);
    CHECK(scanned->lfe);
    // §E3.8.2: the dependent's Ls/Rs overwrite the core's and its Lrs/Rrs
    // extend the layout, so a 5.1 core plus four coded channels renders 8.
    CHECK(scanned->channels == 8);
    // The core and its dependent are ONE access unit, not two frames.
    CHECK(scanned->access_units.size() == 3);
    CHECK(scanned->substreams_per_unit == 2);
    // bsid comes off the core: 6 is what real legacy-core deliveries carry and
    // 8 is what this project's own encoder writes - either way it is the AC-3
    // frame's, not the dependent's 16.
    CHECK(scanned->bsid == 8);
}

TEST_CASE("scan refuses substream arrangements it does not model", "[elementary]") {
    using ac3::io::ScanError;

    // An Annex E INDEPENDENT substream behind an AC-3 core is a second
    // programme (§E3.8.4's mixture), not an extension of the first. Folding it
    // into the core's access unit would union its channels into a layout they
    // have nothing to do with, so it is refused instead.
    ac3::FrameEncoder core{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    auto pcm = tone(6);
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    const auto frame = core.encode_frame(views);
    REQUIRE(frame.has_value());
    const auto independent = ac3::eac3::build_silent_frame({.bitrate_kbps = 192});
    REQUIRE(independent.has_value());

    std::vector<std::byte> mixture;
    append(mixture, *frame);
    append(mixture, *independent);
    CHECK(ac3::io::scan(mixture).error() == ScanError::kUnsupportedStructure);
}
