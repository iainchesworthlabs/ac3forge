#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/meta/drc.hpp"

// ac3::io::edit_frame_metadata rewrites bsi fields in an already-encoded
// stream and re-stamps its CRCs. Two claims are worth pinning, and they pull
// in opposite directions:
//
//   1. The metadata really changes - a decoder reads back the new value.
//   2. NOTHING else changes - the decoded PCM is bit-identical to the
//      original's, which is the whole reason to rewrite rather than
//      re-encode.
//
// Both are checked against the in-repo decoder rather than against the
// rewriter's own reader, because the decoder validates crc1 AND crc2 before
// it will produce a sample at all (decoder.cpp's own check): a decode that
// succeeds is itself the proof that the CRC re-stamp - crc1's GF(2) solve
// included - came out right. A rewriter that got crc1 wrong would fail every
// one of these tests with kBadCrc rather than quietly passing.

namespace {

std::vector<std::vector<float>> tone(std::size_t channels) {
    std::vector<std::vector<float>> pcm(channels,
                                        std::vector<float>(ac3::kSamplesPerFrame));
    for (std::size_t ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const double f = 300.0 + 190.0 * static_cast<double>(ch);
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                0.45 * std::sin(2.0 * std::numbers::pi * f * i / 48000.0));
        }
    }
    return pcm;
}

std::vector<std::span<const float>> views_of(const std::vector<std::vector<float>>& pcm) {
    std::vector<std::span<const float>> views;
    views.reserve(pcm.size());
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    return views;
}

// Three frames of real audio, never one and never silence: block 0 of a
// stream is the one frame whose exponent/allocation state is unconditional,
// so a bug that only bites once the encoder is carrying state forward hides
// completely in a single-frame fixture.
std::vector<std::byte> ac3_stream(const ac3::EncoderConfig& config, int frames = 3) {
    ac3::FrameEncoder encoder{config};
    const auto pcm = tone(static_cast<std::size_t>(
        ac3::fullbw_channel_count(config.acmod) + (config.lfe ? 1 : 0)));
    const auto views = views_of(pcm);
    std::vector<std::byte> stream;
    for (int i = 0; i < frames; ++i) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    return stream;
}

std::vector<std::byte> eac3_stream(const ac3::eac3::AccessUnitConfig& config, int units = 3) {
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto pcm = tone(static_cast<std::size_t>(encoder.channel_count()));
    const auto views = views_of(pcm);
    std::vector<std::byte> stream;
    for (int i = 0; i < units; ++i) {
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

// Every AC-3 frame's decoded PCM, concatenated. A decode failure (a bad CRC
// included) fails the test rather than returning short.
std::vector<float> decode_ac3(std::span<const std::byte> stream, ac3::DecodedFrame& first) {
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    ac3::FrameDecoder decoder;
    std::vector<float> out;
    bool have_first = false;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        REQUIRE(decoded.has_value());
        if (!have_first) {
            first = *decoded;
            have_first = true;
        }
        for (const auto& channel : decoded->channels) {
            out.insert(out.end(), channel.begin(), channel.end());
        }
    }
    return out;
}

std::vector<float> decode_eac3(std::span<const std::byte> stream, ac3::DecodedAccessUnit& first) {
    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    ac3::Eac3Decoder decoder;
    std::vector<float> out;
    bool have_first = false;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        if (!have_first) {
            first = **decoded;
            have_first = true;
        }
        for (const auto& channel : (*decoded)->channels) {
            out.insert(out.end(), channel.begin(), channel.end());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("read_frame_metadata reports what the encoder actually wrote", "[metadata-edit]") {
    SECTION("AC-3 3/2 + LFE with heavy compression") {
        const auto stream = ac3_stream({.bitrate_kbps = 448,
                                        .dialnorm = 27,
                                        .acmod = ac3::Acmod::k3_2,
                                        .lfe = true,
                                        .heavy = ac3::meta::HeavyConfig{}});
        const auto meta = ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->kind == ac3::io::StreamKind::kAc3);
        CHECK(meta->bsid == 8);
        CHECK(meta->acmod == ac3::Acmod::k3_2);
        CHECK(meta->lfe);
        CHECK(meta->dialnorm == 27);
        CHECK(meta->compr.has_value());
        // §5.4.2.2: AC-3 always transmits bsmod, so it is always rewritable.
        REQUIRE(meta->bsmod.has_value());
        CHECK(*meta->bsmod == 0);
        // 3/2 has three front and two surround channels, so both bsi
        // downmix levels are on the wire.
        CHECK(meta->cmixlev.has_value());
        CHECK(meta->surmixlev.has_value());
        // dsurmod is 2/0's alone.
        CHECK_FALSE(meta->dsurmod.has_value());
        CHECK_FALSE(meta->dialnorm2.has_value());
        CHECK(meta->bytes == stream.size() / 3);
    }

    SECTION("AC-3 2/0 carries dsurmod and neither mix level") {
        const auto stream =
            ac3_stream({.bitrate_kbps = 192, .dialnorm = 31, .acmod = ac3::Acmod::k2_0});
        const auto meta = ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        REQUIRE(meta->dsurmod.has_value());
        CHECK(*meta->dsurmod == 0);
        CHECK_FALSE(meta->cmixlev.has_value());
        CHECK_FALSE(meta->surmixlev.has_value());
        CHECK_FALSE(meta->compr.has_value());  // no heavy config was given
    }

    SECTION("AC-3 1+1 carries Ch2's own dialnorm") {
        const auto stream = ac3_stream({.bitrate_kbps = 192,
                                        .dialnorm = 24,
                                        .dialnorm2 = 20,
                                        .acmod = ac3::Acmod::kDualMono});
        const auto meta = ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->dialnorm == 24);
        REQUIRE(meta->dialnorm2.has_value());
        CHECK(*meta->dialnorm2 == 20);
    }

    SECTION("E-AC-3 with mixmdate reports the whole group") {
        ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 448,
                              .acmod = ac3::Acmod::k3_2,
                              .lfe = true,
                              .dialnorm = 23,
                              .mixing = ac3::meta::MixMetadata{}};
        const auto stream = eac3_stream(config);
        const auto meta = ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->kind == ac3::io::StreamKind::kEac3);
        CHECK(meta->bsid == 16);
        CHECK(meta->dialnorm == 23);
        CHECK(meta->strmtyp == 0);
        REQUIRE(meta->mix.has_value());
        // 3/2 + LFE puts every level in the group on the wire.
        CHECK(meta->mix->dmixmod.has_value());
        CHECK(meta->mix->ltrtcmixlev.has_value());
        CHECK(meta->mix->lorocmixlev.has_value());
        CHECK(meta->mix->ltrtsurmixlev.has_value());
        CHECK(meta->mix->lorosurmixlev.has_value());
        // This encoder never sets infomdate, so bsmod/dsurmod are genuinely
        // not on the wire here - the documented E-AC-3 limit.
        CHECK_FALSE(meta->bsmod.has_value());
        CHECK_FALSE(meta->dsurmod.has_value());
    }
}

TEST_CASE("editing dialnorm changes the metadata and nothing else", "[metadata-edit]") {
    auto stream = ac3_stream({.bitrate_kbps = 448,
                              .dialnorm = 27,
                              .acmod = ac3::Acmod::k3_2,
                              .lfe = true});
    const std::vector<std::byte> original = stream;

    ac3::DecodedFrame before_meta{};
    const auto before = decode_ac3(original, before_meta);
    CHECK(before_meta.dialnorm == 27);

    const auto summary = ac3::io::edit_stream_metadata(stream, {.dialnorm = 14});
    REQUIRE(summary.has_value());
    CHECK(summary->syncframes == 3);
    CHECK(summary->changed == 3);
    // The rewrite is in place: not one byte more or fewer.
    CHECK(stream.size() == original.size());

    ac3::DecodedFrame after_meta{};
    const auto after = decode_ac3(stream, after_meta);
    CHECK(after_meta.dialnorm == 14);
    // Bit-identical audio, sample for sample. This is the claim that separates
    // a metadata rewrite from a transcode.
    REQUIRE(after.size() == before.size());
    CHECK(after == before);
}

TEST_CASE("editing dialnorm to the value already there changes no bytes", "[metadata-edit]") {
    auto stream = ac3_stream({.bitrate_kbps = 192, .dialnorm = 20, .acmod = ac3::Acmod::k2_0});
    const std::vector<std::byte> original = stream;
    const auto summary = ac3::io::edit_stream_metadata(stream, {.dialnorm = 20});
    REQUIRE(summary.has_value());
    CHECK(summary->syncframes == 3);
    CHECK(summary->changed == 0);
    CHECK(stream == original);
}

TEST_CASE("editing compr, bsmod and dsurmod rewrites each field in place", "[metadata-edit]") {
    SECTION("compr, where the stream carries one") {
        auto stream = ac3_stream({.bitrate_kbps = 192,
                                  .dialnorm = 27,
                                  .acmod = ac3::Acmod::k2_0,
                                  .heavy = ac3::meta::HeavyConfig{}});
        const std::vector<std::byte> original = stream;
        ac3::DecodedFrame before_meta{};
        const auto before = decode_ac3(original, before_meta);
        REQUIRE(before_meta.compr.has_value());

        // A word the encoder's own peak detector would not have chosen, so
        // "it was already that" cannot pass this by accident.
        constexpr std::uint8_t kWord = 0x93;
        REQUIRE(*before_meta.compr != kWord);
        const auto summary = ac3::io::edit_stream_metadata(stream, {.compr = kWord});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);

        ac3::DecodedFrame after_meta{};
        const auto after = decode_ac3(stream, after_meta);
        REQUIRE(after_meta.compr.has_value());
        CHECK(*after_meta.compr == kWord);
        CHECK(after == before);
    }

    SECTION("bsmod, which AC-3 always transmits") {
        auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
        const auto summary = ac3::io::edit_stream_metadata(stream, {.bsmod = 2});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);
        // Read back through scan(), which walks bsi independently of the
        // rewriter's own parse.
        const auto scanned = ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        CHECK(scanned->bsmod == 2);
    }

    SECTION("dsurmod, on the one acmod that carries it") {
        auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
        const std::vector<std::byte> original = stream;
        ac3::DecodedFrame before_meta{};
        const auto before = decode_ac3(original, before_meta);

        const auto summary = ac3::io::edit_stream_metadata(stream, {.dsurmod = 2});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);
        const auto meta = ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        REQUIRE(meta->dsurmod.has_value());
        CHECK(*meta->dsurmod == 2);

        ac3::DecodedFrame after_meta{};
        const auto after = decode_ac3(stream, after_meta);
        CHECK(after == before);
    }

    SECTION("dialnorm2, on 1+1") {
        auto stream = ac3_stream({.bitrate_kbps = 192,
                                  .dialnorm = 24,
                                  .dialnorm2 = 20,
                                  .acmod = ac3::Acmod::kDualMono});
        const auto summary =
            ac3::io::edit_stream_metadata(stream, {.dialnorm = 18, .dialnorm2 = 12});
        REQUIRE(summary.has_value());
        ac3::DecodedFrame meta{};
        const auto pcm = decode_ac3(stream, meta);
        CHECK(pcm.size() > 0);
        CHECK(meta.dialnorm == 18);
        REQUIRE(meta.dialnorm2.has_value());
        CHECK(*meta.dialnorm2 == 12);
    }
}

TEST_CASE("an E-AC-3 stream rewrites the same way, dependents included", "[metadata-edit]") {
    namespace cm = ac3::eac3::chanmap;
    ac3::eac3::AccessUnitConfig config;
    config.independent = {
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true, .dialnorm = 25};
    // A dependent substream, so the per-substream rules actually get
    // exercised: its own dialnorm is rewritten, its compre bit is left alone.
    config.dependents.push_back(
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .chanmap = cm::k512Height});
    auto stream = eac3_stream(config);
    const std::vector<std::byte> original = stream;

    ac3::DecodedAccessUnit before_meta{};
    const auto before = decode_eac3(original, before_meta);
    CHECK(before_meta.dialnorm == 25);
    CHECK(before_meta.substream_count == 2);

    const auto summary = ac3::io::edit_stream_metadata(stream, {.dialnorm = 11});
    REQUIRE(summary.has_value());
    // Six syncframes: three access units of one independent plus one
    // dependent each.
    CHECK(summary->syncframes == 6);
    CHECK(summary->changed == 6);
    CHECK(stream.size() == original.size());

    ac3::DecodedAccessUnit after_meta{};
    const auto after = decode_eac3(stream, after_meta);
    CHECK(after_meta.dialnorm == 11);
    REQUIRE(after.size() == before.size());
    CHECK(after == before);
}

TEST_CASE("an absent field is refused rather than invented", "[metadata-edit]") {
    // No heavy config, so compre is clear: there are no compr bits to
    // overwrite, and adding them would re-frame the syncframe.
    auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
    const std::vector<std::byte> original = stream;
    const auto result = ac3::io::edit_stream_metadata(stream, {.compr = 0x40});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::io::EditError::kFieldAbsent);
    // Refused BEFORE writing anything - a partly-rewritten stream would be
    // worse than a refusal.
    CHECK(stream == original);

    SECTION("dialnorm2 on a stream that is not 1+1") {
        const auto no_ch2 = ac3::io::edit_stream_metadata(stream, {.dialnorm2 = 20});
        REQUIRE_FALSE(no_ch2.has_value());
        CHECK(no_ch2.error() == ac3::io::EditError::kFieldAbsent);
    }

    SECTION("dsurmod on an acmod that has none") {
        auto wide = ac3_stream(
            {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true});
        const auto result_wide = ac3::io::edit_stream_metadata(wide, {.dsurmod = 1});
        REQUIRE_FALSE(result_wide.has_value());
        CHECK(result_wide.error() == ac3::io::EditError::kFieldAbsent);
    }

    SECTION("bsmod on an E-AC-3 stream with no infomdate") {
        ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
        auto eac3 = eac3_stream(config);
        const auto result_eac3 = ac3::io::edit_stream_metadata(eac3, {.bsmod = 3});
        REQUIRE_FALSE(result_eac3.has_value());
        CHECK(result_eac3.error() == ac3::io::EditError::kFieldAbsent);
    }
}

TEST_CASE("an out-of-range value is refused", "[metadata-edit]") {
    auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
    const std::vector<std::byte> original = stream;
    // §5.4.2.8: dialnorm 0 is reserved ("indicates that dialnorm is not
    // used"), which this refuses rather than writes.
    for (const int bad : {0, 32, -1}) {
        const auto result = ac3::io::edit_stream_metadata(stream, {.dialnorm = bad});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac3::io::EditError::kOutOfRange);
    }
    const auto bad_bsmod = ac3::io::edit_stream_metadata(stream, {.bsmod = 8});
    REQUIRE_FALSE(bad_bsmod.has_value());
    CHECK(bad_bsmod.error() == ac3::io::EditError::kOutOfRange);
    CHECK(stream == original);
}

TEST_CASE("a rewrite really does re-stamp the CRCs", "[metadata-edit]") {
    // The negative control this whole file rests on: the decoder must REFUSE
    // a frame whose bsi was changed without the CRCs being fixed. Without
    // this, every "the decode still worked" assertion above could be passing
    // for the wrong reason - a decoder that never checked.
    auto stream = ac3_stream({.bitrate_kbps = 192, .dialnorm = 27, .acmod = ac3::Acmod::k2_0});
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    const auto frame_bytes = frames->front().size();

    auto tampered = stream;
    // dialnorm sits at bit 54 of a 2/0 AC-3 syncframe (bsi with no cmixlev,
    // no surmixlev, dsurmod present) - flip a bit of it by hand, with no
    // re-stamp.
    tampered[6] ^= std::byte{0x20};
    ac3::FrameDecoder decoder;
    const auto refused = decoder.decode_frame(std::span{tampered}.first(frame_bytes));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ac3::DecodeError::kBadCrc);

    // The same byte reachable through the rewriter instead: accepted.
    const auto summary = ac3::io::edit_stream_metadata(stream, {.dialnorm = 27 ^ 4});
    REQUIRE(summary.has_value());
    ac3::FrameDecoder ok_decoder;
    const auto accepted = ok_decoder.decode_frame(std::span{stream}.first(frame_bytes));
    REQUIRE(accepted.has_value());
    CHECK(accepted->dialnorm == (27 ^ 4));
}

TEST_CASE("a stream that is not AC-3 or E-AC-3 is refused", "[metadata-edit]") {
    std::vector<std::byte> junk(64, std::byte{0x00});
    const auto no_sync = ac3::io::read_frame_metadata(junk);
    REQUIRE_FALSE(no_sync.has_value());
    CHECK(no_sync.error() == ac3::io::EditError::kBadSyncWord);

    std::vector<std::byte> too_short{std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}};
    const auto short_frame = ac3::io::read_frame_metadata(too_short);
    REQUIRE_FALSE(short_frame.has_value());
    CHECK(short_frame.error() == ac3::io::EditError::kTruncated);

    // A real syncframe cut short of its own declared size.
    const auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}, 1);
    const auto clipped =
        ac3::io::read_frame_metadata(std::span{stream}.first(stream.size() - 1));
    REQUIRE_FALSE(clipped.has_value());
    CHECK(clipped.error() == ac3::io::EditError::kTruncated);
}

TEST_CASE("describe() gives every EditError a distinct, non-empty message", "[metadata-edit]") {
    const ac3::io::EditError all[] = {
        ac3::io::EditError::kBadSyncWord,   ac3::io::EditError::kTruncated,
        ac3::io::EditError::kUnsupportedBsid, ac3::io::EditError::kReservedValue,
        ac3::io::EditError::kFieldAbsent,   ac3::io::EditError::kOutOfRange};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        CHECK_FALSE(ac3::io::describe(all[i]).empty());
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            CHECK(ac3::io::describe(all[i]) != ac3::io::describe(all[j]));
        }
    }
}
