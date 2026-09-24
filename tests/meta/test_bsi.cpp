#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"

// Bit stream information that changes no output sample: Annex D's alternate
// syntax (bsid 6) and the informational fields on the AC-3 side, and the rest
// of Table E1.2's mixmdate plus infomdat on the E-AC-3 side.
//
// Every case here is a ROUND TRIP through this repo's own encoder and decoder
// rather than a hand-built bit pattern. That is deliberate: the failure these
// fields invite is not a wrong value, it is a wrong LENGTH - a field written
// one bit narrow shifts everything after it, and the frame stops decoding as
// itself. A round trip catches that where a comparison against a constant
// would not, because the decoder has to arrive at exactly the offset the
// encoder left off at for any of the audio to come back. The independent
// check on the same streams is tools/references/eac3_parse.py, driven from
// tools/ci/run_codec_matrix.sh.

namespace {

// Two frames of a quiet but non-silent tone, enough for the decoder to have
// real mantissa data to land on if the header length is ever wrong. Silence
// would hide exactly the failure these tests exist for: with every SNR offset
// zero the mantissa fields vanish and a misplaced header still "decodes".
std::vector<std::vector<float>> tone(int channels) {
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels),
                                        std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
    for (std::size_t ch = 0; ch < out.size(); ++ch) {
        for (std::size_t n = 0; n < out[ch].size(); ++n) {
            const auto phase = static_cast<double>(n) * 0.05 + static_cast<double>(ch);
            out[ch][n] = static_cast<float>(0.25 * std::sin(phase));
        }
    }
    return out;
}

std::vector<std::span<const float>> views(const std::vector<std::vector<float>>& channels) {
    std::vector<std::span<const float>> out;
    out.reserve(channels.size());
    for (const auto& channel : channels) {
        out.emplace_back(channel);
    }
    return out;
}

ac3::DecodedFrame round_trip_ac3(const ac3::EncoderConfig& config) {
    ac3::FrameEncoder encoder{config};
    const auto pcm = tone(fullbw_channel_count(config.acmod) + (config.lfe ? 1 : 0));
    const auto spans = views(pcm);
    // Two frames: the first primes the MDCT overlap, the second is the one
    // whose audio a header misread would corrupt.
    auto frame = encoder.encode_frame(spans);
    REQUIRE(frame.has_value());
    frame = encoder.encode_frame(spans);
    REQUIRE(frame.has_value());

    ac3::FrameDecoder decoder;
    const auto decoded = decoder.decode_frame(*frame);
    REQUIRE(decoded.has_value());
    return *decoded;
}

ac3::DecodedSubstream round_trip_eac3(const ac3::eac3::FrameConfig& config) {
    ac3::eac3::FrameEncoder encoder{config};
    const auto pcm = tone(fullbw_channel_count(config.acmod) + (config.lfe ? 1 : 0));
    const auto spans = views(pcm);
    auto frame = encoder.encode_frame(spans);
    REQUIRE(frame.has_value());
    frame = encoder.encode_frame(spans);
    REQUIRE(frame.has_value());

    ac3::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    return **decoded;
}

// A syncframe whose dmixmod is Table D2.2's reserved '11'. `encode` returns
// one syncframe for the dmixmod it is given, with everything else fixed; its
// '01' and '10' frames are ORed together byte by byte. '01' | '10' is '11',
// every other bit is ORed with an identical copy of itself, and restamp_crc()
// then repairs the CRC words the OR spoiled. This is the one exception to the
// round-trip rule above, and it is forced: the encoder will not write '11'
// (meta::valid_downmix_mode), so a frame carrying it has to be made the way a
// third-party one would arrive. The tests check the result decodes to the same
// audio as the '01' frame, which is what shows the OR touched nothing else.
template <typename Encode>
std::vector<std::byte> reserved_dmixmod_frame(Encode encode) {
    const auto ltrt = encode(ac3::meta::DownmixMode::kLtRt);
    const auto loro = encode(ac3::meta::DownmixMode::kLoRo);
    REQUIRE(ltrt.size() == loro.size());
    std::vector<std::byte> out(ltrt.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = ltrt[i] | loro[i];
    }
    REQUIRE(ac3::io::restamp_crc(out).has_value());
    return out;
}

}  // namespace

// --- DC3: Annex D and the informational fields ------------------------------

TEST_CASE("AC-3: a config that says nothing about bsi still writes bsid 8", "[bsi]") {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k2_0;
    const auto decoded = round_trip_ac3(config);
    CHECK(decoded.bsid == 8);
    CHECK(!decoded.alternate_bsi);
    CHECK(decoded.info.bsmod == ac3::meta::BitstreamMode::kCompleteMain);
    // §5.4.2.25: an encoder producing a stream is the original of it.
    CHECK(decoded.info.origbs);
    CHECK(!decoded.info.copyrightb);
    CHECK(!decoded.info.timecod1);
    CHECK(!decoded.info.timecod2);
}

TEST_CASE("AC-3: every informational bsi field survives a round trip", "[bsi]") {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k2_0;  // 2/0 is the one layout that carries dsurmod
    config.info.bsmod = ac3::meta::BitstreamMode::kVisuallyImpaired;
    config.info.dsurmod = ac3::meta::SurroundMode::kDolbySurround;
    config.info.langcod = true;
    config.info.audprod = ac3::meta::AudioProduction{
        .mixlevel = 25, .roomtyp = ac3::meta::RoomType::kSmallRoomFlat};
    config.info.copyrightb = true;
    config.info.origbs = false;

    const auto decoded = round_trip_ac3(config);
    CHECK(decoded.bsid == 8);
    CHECK(decoded.info.bsmod == ac3::meta::BitstreamMode::kVisuallyImpaired);
    CHECK(decoded.info.dsurmod == ac3::meta::SurroundMode::kDolbySurround);
    CHECK(decoded.info.langcod);
    REQUIRE(decoded.info.audprod);
    CHECK(decoded.info.audprod->mixlevel == 25);
    CHECK(ac3::meta::mix_level_db_spl(decoded.info.audprod->mixlevel) == 105);
    CHECK(decoded.info.audprod->roomtyp == ac3::meta::RoomType::kSmallRoomFlat);
    CHECK(decoded.info.copyrightb);
    CHECK(!decoded.info.origbs);
    // §5.4.2's audprodie has no adconvtyp - only Annex E's does - so nothing
    // should have been read into it here.
    CHECK(decoded.info.audprod->adconvtyp == ac3::meta::AdConverterType::kStandard);
}

TEST_CASE("AC-3: a time code round trips through both 14-bit halves", "[bsi]") {
    ac3::EncoderConfig config;
    config.info.timecod1 =
        ac3::meta::TimeCodeCoarse{.hours = 17, .minutes = 43, .eight_seconds = 5};
    config.info.timecod2 =
        ac3::meta::TimeCodeFine{.seconds = 6, .frames = 21, .sixty_fourths = 39};

    const auto decoded = round_trip_ac3(config);
    REQUIRE(decoded.info.timecod1);
    REQUIRE(decoded.info.timecod2);
    CHECK(decoded.info.timecod1->hours == 17);
    CHECK(decoded.info.timecod1->minutes == 43);
    CHECK(decoded.info.timecod1->eight_seconds == 5);
    CHECK(decoded.info.timecod2->seconds == 6);
    CHECK(decoded.info.timecod2->frames == 21);
    CHECK(decoded.info.timecod2->sixty_fourths == 39);
    // 5 * 8 + 6 = 46 seconds, which is what the two halves together say.
    CHECK(ac3::meta::format_timecode(*decoded.info.timecod1, *decoded.info.timecod2) ==
          "17:43:46:21.39");
}

TEST_CASE("AC-3: one half of the time code may be sent without the other", "[bsi]") {
    ac3::EncoderConfig config;
    // Table 5.13's '0','1' row: the coarse half alone.
    config.info.timecod1 = ac3::meta::TimeCodeCoarse{.hours = 2, .minutes = 0,
                                                     .eight_seconds = 7};
    const auto decoded = round_trip_ac3(config);
    REQUIRE(decoded.info.timecod1);
    CHECK(decoded.info.timecod1->hours == 2);
    CHECK(decoded.info.timecod1->eight_seconds == 7);
    CHECK(!decoded.info.timecod2);
}

TEST_CASE("AC-3: Annex D writes bsid 6 and both xbsi groups", "[bsi]") {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::meta::AlternateBsi alternate;
    alternate.mix = ac3::meta::MixMetadata{
        .dmixmod = ac3::meta::DownmixMode::kLtRt,
        .ltrtcmixlev = ac3::meta::MixLevel::kMinus1_5dB,
        .lorocmixlev = ac3::meta::MixLevel::kMinus4_5dB,
        .ltrtsurmixlev = ac3::meta::MixLevel::kMinus3dB,
        .lorosurmixlev = ac3::meta::MixLevel::kMinus6dB,
    };
    alternate.extended = ac3::meta::ExtendedBsi{
        .dsurexmod = ac3::meta::SurroundExMode::kSurroundEx,
        .dheadphonmod = ac3::meta::HeadphoneMode::kDolbyHeadphone,
        .adconvtyp = ac3::meta::AdConverterType::kHdcd,
        .encinfo = true,
    };
    config.alternate_bsi = alternate;

    const auto decoded = round_trip_ac3(config);
    CHECK(decoded.bsid == 6);
    REQUIRE(decoded.alternate_bsi);
    REQUIRE(decoded.alternate_bsi->mix);
    const auto& mix = *decoded.alternate_bsi->mix;
    CHECK(mix.dmixmod == ac3::meta::DownmixMode::kLtRt);
    // The four levels are the point of this test: Table D2.1 pairs them
    // Lt/Rt-then-Lo/Ro where Table E1.2 pairs them centre-then-surround, so
    // reading them in mixmdate's order would swap lorocmixlev and
    // ltrtsurmixlev and still decode without complaint.
    CHECK(mix.ltrtcmixlev == ac3::meta::MixLevel::kMinus1_5dB);
    CHECK(mix.ltrtsurmixlev == ac3::meta::MixLevel::kMinus3dB);
    CHECK(mix.lorocmixlev == ac3::meta::MixLevel::kMinus4_5dB);
    CHECK(mix.lorosurmixlev == ac3::meta::MixLevel::kMinus6dB);
    // Annex D has no LFE mix level field at all.
    CHECK(!mix.lfemixlevcod);
    REQUIRE(decoded.alternate_bsi->extended);
    const auto& extended = *decoded.alternate_bsi->extended;
    CHECK(extended.dsurexmod == ac3::meta::SurroundExMode::kSurroundEx);
    CHECK(extended.dheadphonmod == ac3::meta::HeadphoneMode::kDolbyHeadphone);
    CHECK(extended.adconvtyp == ac3::meta::AdConverterType::kHdcd);
    CHECK(extended.encinfo);
    // §D2.3.1.11: reserved, and encoders shall send zero.
    CHECK(extended.xbsi2 == 0);
    // A bsid-6 frame carries no time code - the same 28 bits are spent.
    CHECK(!decoded.info.timecod1);
    CHECK(!decoded.info.timecod2);
}

TEST_CASE("AC-3: Annex D's two groups are independently optional", "[bsi]") {
    ac3::EncoderConfig config;
    config.alternate_bsi = ac3::meta::AlternateBsi{};  // both flags clear
    const auto decoded = round_trip_ac3(config);
    CHECK(decoded.bsid == 6);
    REQUIRE(decoded.alternate_bsi);
    CHECK(!decoded.alternate_bsi->mix);
    CHECK(!decoded.alternate_bsi->extended);
}

TEST_CASE("AC-3: Annex D's reserved dmixmod is kept as sent and named reserved", "[bsi]") {
    // A/52:2018 Table D2.2 and TS 102 366 Table D.1.1 both reserve '11'. The
    // readers used to fold it into '00', so a report could not say it was
    // there; kReserved keeps it.
    const auto encode = [](ac3::meta::DownmixMode dmixmod) {
        ac3::EncoderConfig config;
        config.acmod = ac3::Acmod::k3_2;
        config.lfe = true;
        ac3::meta::AlternateBsi alternate;
        alternate.mix = ac3::meta::MixMetadata{
            .dmixmod = dmixmod,
            .ltrtcmixlev = ac3::meta::MixLevel::kMinus1_5dB,
            .lorocmixlev = ac3::meta::MixLevel::kMinus4_5dB,
            .ltrtsurmixlev = ac3::meta::MixLevel::kMinus3dB,
            .lorosurmixlev = ac3::meta::MixLevel::kMinus6dB,
        };
        config.alternate_bsi = alternate;
        ac3::FrameEncoder encoder{config};
        const auto pcm = tone(6);
        const auto spans = views(pcm);
        auto frame = encoder.encode_frame(spans);
        REQUIRE(frame.has_value());
        frame = encoder.encode_frame(spans);
        REQUIRE(frame.has_value());
        return *frame;
    };
    const auto reserved = reserved_dmixmod_frame(encode);

    ac3::FrameDecoder decoder;
    const auto decoded = decoder.decode_frame(reserved);
    REQUIRE(decoded.has_value());
    CHECK(decoded->bsid == 6);
    REQUIRE(decoded->alternate_bsi);
    REQUIRE(decoded->alternate_bsi->mix);
    const auto& mix = *decoded->alternate_bsi->mix;
    CHECK(mix.dmixmod == ac3::meta::DownmixMode::kReserved);
    CHECK(ac3::meta::describe(mix.dmixmod) == "reserved");
    // The four levels after it are where the '01' frame put them.
    CHECK(mix.ltrtcmixlev == ac3::meta::MixLevel::kMinus1_5dB);
    CHECK(mix.ltrtsurmixlev == ac3::meta::MixLevel::kMinus3dB);
    CHECK(mix.lorocmixlev == ac3::meta::MixLevel::kMinus4_5dB);
    CHECK(mix.lorosurmixlev == ac3::meta::MixLevel::kMinus6dB);

    // The header tier, which probe and downmix=auto read, sees the same code.
    const auto header = ac3::io::read_frame_header(reserved);
    REQUIRE(header.has_value());
    CHECK(header->dmixmod == ac3::meta::DownmixMode::kReserved);

    // And nothing else moved: the audio is the '01' frame's, bit for bit.
    ac3::FrameDecoder reference_decoder;
    const auto reference = reference_decoder.decode_frame(encode(ac3::meta::DownmixMode::kLtRt));
    REQUIRE(reference.has_value());
    const bool same_audio = decoded->channels == reference->channels;
    CHECK(same_audio);
}

TEST_CASE("AC-3: bsi values wider than their field are refused, not truncated", "[bsi]") {
    const auto pcm = tone(2);
    const auto spans = views(pcm);

    SECTION("a mixing level above 31 needs six bits where §5.4.2.14 has five") {
        ac3::EncoderConfig config;
        config.info.audprod = ac3::meta::AudioProduction{.mixlevel = 40};
        ac3::FrameEncoder encoder{config};
        const auto frame = encoder.encode_frame(spans);
        REQUIRE(!frame.has_value());
        CHECK(frame.error() == ac3::FrameError::kInvalidBsi);
    }
    SECTION("a time code past the hours field") {
        ac3::EncoderConfig config;
        config.info.timecod1 = ac3::meta::TimeCodeCoarse{.hours = 24};
        ac3::FrameEncoder encoder{config};
        const auto frame = encoder.encode_frame(spans);
        REQUIRE(!frame.has_value());
        CHECK(frame.error() == ac3::FrameError::kInvalidBsi);
    }
    SECTION("Annex D and a time code want the same 28 bits") {
        ac3::EncoderConfig config;
        config.alternate_bsi = ac3::meta::AlternateBsi{};
        config.info.timecod2 = ac3::meta::TimeCodeFine{.frames = 3};
        ac3::FrameEncoder encoder{config};
        const auto frame = encoder.encode_frame(spans);
        REQUIRE(!frame.has_value());
        CHECK(frame.error() == ac3::FrameError::kInvalidBsi);
    }
    SECTION("a reserved surround level would not be the level applied") {
        ac3::EncoderConfig config;
        ac3::meta::AlternateBsi alternate;
        alternate.mix = ac3::meta::MixMetadata{.ltrtsurmixlev = ac3::meta::MixLevel::kUnity};
        config.alternate_bsi = alternate;
        ac3::FrameEncoder encoder{config};
        const auto frame = encoder.encode_frame(spans);
        REQUIRE(!frame.has_value());
        CHECK(frame.error() == ac3::FrameError::kInvalidBsi);
    }
    SECTION("a reserved dmixmod would state no preference a receiver can act on") {
        ac3::EncoderConfig config;
        ac3::meta::AlternateBsi alternate;
        alternate.mix = ac3::meta::MixMetadata{.dmixmod = ac3::meta::DownmixMode::kReserved};
        config.alternate_bsi = alternate;
        ac3::FrameEncoder encoder{config};
        const auto frame = encoder.encode_frame(spans);
        REQUIRE(!frame.has_value());
        CHECK(frame.error() == ac3::FrameError::kInvalidBsi);
    }
}

TEST_CASE("AC-3: cmixlev and surmixlev are reported rather than discarded", "[bsi]") {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
    config.surmixlev = ac3::meta::SurroundMixLevel::kSilent;
    const auto decoded = round_trip_ac3(config);
    CHECK(decoded.cmixlev == ac3::meta::CentreMixLevel::kMinus3dB);
    CHECK(decoded.surmixlev == ac3::meta::SurroundMixLevel::kSilent);
}

TEST_CASE("AC-3: a layout carrying neither mix level reports the 7.8 fallbacks", "[bsi]") {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k2_0;  // no centre, no surround: neither field is sent
    const auto decoded = round_trip_ac3(config);
    // Absent, not "present and says the default" - that distinction is the
    // whole reason these are std::optional (see DecodedFrame::cmixlev's own
    // comment). ac3::mix_levels() is where the §7.8 fallback actually lands.
    CHECK_FALSE(decoded.cmixlev.has_value());
    CHECK_FALSE(decoded.surmixlev.has_value());
    const auto levels = ac3::mix_levels(decoded.cmixlev, decoded.surmixlev);
    CHECK(levels.loro_clev == ac3::meta::level::kMinus4_5dB);
    CHECK(levels.loro_slev == ac3::meta::level::kMinus6dB);
}

TEST_CASE("AC-3: a decoded Annex D frame resolves to its own xbsi1 levels", "[bsi]") {
    // DecodedFrame carries everything the Annex D form of mix_levels() reads,
    // so a caller folding for itself gets the levels FrameDecoder folds with
    // (§D3.1.2). bsi says -3 dB for both levels; xbsi1 says something else for
    // all four.
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
    config.surmixlev = ac3::meta::SurroundMixLevel::kMinus3dB;
    ac3::meta::AlternateBsi alternate;
    alternate.mix = ac3::meta::MixMetadata{
        .dmixmod = ac3::meta::DownmixMode::kLoRo,
        .ltrtcmixlev = ac3::meta::MixLevel::kPlus1_5dB,
        .lorocmixlev = ac3::meta::MixLevel::kMinus4_5dB,
        .ltrtsurmixlev = ac3::meta::MixLevel::kMinus1_5dB,
        .lorosurmixlev = ac3::meta::MixLevel::kSilent,
    };
    config.alternate_bsi = alternate;
    const auto resolve = [](const ac3::DecodedFrame& frame) {
        return ac3::mix_levels(frame.acmod, frame.cmixlev, frame.surmixlev, frame.alternate_bsi);
    };

    const auto levels = resolve(round_trip_ac3(config));
    CHECK(levels.ltrt_clev == ac3::meta::level::kPlus1_5dB);
    CHECK(levels.ltrt_slev == ac3::meta::level::kMinus1_5dB);
    CHECK(levels.loro_clev == ac3::meta::level::kMinus4_5dB);
    CHECK(levels.loro_slev == ac3::meta::level::kSilent);
    CHECK(levels.preferred == ac3::meta::DownmixMode::kLoRo);

    // Table D2.2's note leaves dmixmod's meaning reserved below 3/0. At 2/0
    // the field is still reported as sent, but it states no preference.
    config.acmod = ac3::Acmod::k2_0;
    const auto stereo = round_trip_ac3(config);
    REQUIRE(stereo.alternate_bsi);
    REQUIRE(stereo.alternate_bsi->mix);
    CHECK(stereo.alternate_bsi->mix->dmixmod == ac3::meta::DownmixMode::kLoRo);
    CHECK(resolve(stereo).preferred == ac3::meta::DownmixMode::kNotIndicated);

    // With xbsi1e clear there is nothing to replace bsi's two levels with.
    config.acmod = ac3::Acmod::k3_2;
    config.alternate_bsi = ac3::meta::AlternateBsi{};
    const auto bsi = resolve(round_trip_ac3(config));
    CHECK(bsi.loro_clev == ac3::meta::level::kMinus3dB);
    CHECK(bsi.loro_slev == ac3::meta::level::kMinus3dB);
    CHECK(bsi.ltrt_clev == ac3::meta::level::kMinus3dB);
    CHECK(bsi.ltrt_slev == ac3::meta::level::kMinus3dB);
    CHECK(bsi.preferred == ac3::meta::DownmixMode::kNotIndicated);
}

// --- DC4: mixmdate depth and infomdat ---------------------------------------

TEST_CASE("E-AC-3: programme scale factors round trip", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::meta::MixMetadata mix;
    mix.lfemixlevcod = 4;
    mix.pgmscl = 45;      // -6 dB
    mix.extpgmscl = 63;   // +12 dB, the top of the range
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->lfemixlevcod == 4);
    REQUIRE(decoded.mixing->pgmscl);
    CHECK(*decoded.mixing->pgmscl == 45);
    CHECK(ac3::meta::pgm_scale_db(*decoded.mixing->pgmscl) == -6.0);
    REQUIRE(decoded.mixing->extpgmscl);
    CHECK(*decoded.mixing->extpgmscl == 63);
    CHECK(ac3::meta::pgm_scale_db(*decoded.mixing->extpgmscl) == 12.0);
    // §E2.3.1.12: an absent pgmscl2 is 0 dB stated in one bit, not a value.
    CHECK(!decoded.mixing->pgmscl2);
}

TEST_CASE("E-AC-3: mixdef 0x0 sends no sub-field bits at all", "[bsi]") {
    // Table E2.6: mixdef 0x0 is "no additional bits" - the 2-bit selector
    // alone. This is the struct's default, but it still needs its own round
    // trip: every other mixdef case in this file sets sub-fields that would
    // shift the rest of mixmdate if the 0x0 branch ever wrote one bit too
    // many or too few.
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    ac3::meta::MixMetadata mix;
    mix.pgmscl = 40;  // something past mixdef to prove the offset landed right
    mix.mixing.mixdef = ac3::meta::MixDefinition::kNone;
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->mixing.mixdef == ac3::meta::MixDefinition::kNone);
    CHECK(!decoded.mixing->mixing.external);
    CHECK(!decoded.mixing->mixing.speech);
    REQUIRE(decoded.mixing->pgmscl);
    CHECK(*decoded.mixing->pgmscl == 40);
}

TEST_CASE("E-AC-3: mixdef 0x1's premix compression triple round trips", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    ac3::meta::MixMetadata mix;
    mix.mixing.mixdef = ac3::meta::MixDefinition::kPremix;
    mix.mixing.premix = {.premixcmpsel = ac3::meta::PremixCompressionSource::kCompr,
                         .drcsrc = ac3::meta::DrcSource::kThisSubstream,
                         .premixcmpscl = 5};
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->mixing.mixdef == ac3::meta::MixDefinition::kPremix);
    CHECK(decoded.mixing->mixing.premix.premixcmpsel ==
          ac3::meta::PremixCompressionSource::kCompr);
    CHECK(decoded.mixing->mixing.premix.drcsrc == ac3::meta::DrcSource::kThisSubstream);
    CHECK(decoded.mixing->mixing.premix.premixcmpscl == 5);
}

TEST_CASE("E-AC-3: mixdef 0x2's twelve reserved bits are carried verbatim", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    ac3::meta::MixMetadata mix;
    mix.mixing.mixdef = ac3::meta::MixDefinition::kReserved;
    mix.mixing.reserved = 0x0A5A;
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->mixing.mixdef == ac3::meta::MixDefinition::kReserved);
    CHECK(decoded.mixing->mixing.reserved == 0x0A5A);
}

TEST_CASE("E-AC-3: mixdef 0x3 carries external scales and speech data", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::meta::MixMetadata mix;
    mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
    ac3::meta::ExternalScales external;
    external.premix = {.premixcmpsel = ac3::meta::PremixCompressionSource::kDynrng,
                       .drcsrc = ac3::meta::DrcSource::kExternal,
                       .premixcmpscl = 3};
    external.left = 0;
    external.centre = 6;
    external.right = 0;
    // §E2.3.1.31: a channel the external programme does not have keeps its
    // flag CLEAR, which is not the same as a scale factor of 0 (that is -1 dB).
    external.left_surround = std::nullopt;
    external.right_surround = std::nullopt;
    external.lfe = 15;  // Table E2.8's mute row
    external.dmixscl = 9;
    external.auxiliary = std::array<std::optional<int>, 2>{2, std::nullopt};
    mix.mixing.external = external;
    mix.mixing.speech = ac3::meta::SpeechEnhancement{
        .spchdat = 17,
        .additional = ac3::meta::SpeechEnhancement::Additional{
            .spchdat1 = 4,
            .spchan1att = 2,
            .more = ac3::meta::SpeechEnhancement::Additional::More{.spchdat2 = 30,
                                                                   .spchan2att = 6}}};
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    const auto& mixing = decoded.mixing->mixing;
    CHECK(mixing.mixdef == ac3::meta::MixDefinition::kExtended);
    REQUIRE(mixing.external);
    CHECK(mixing.external->premix.premixcmpscl == 3);
    CHECK(mixing.external->left == 0);
    CHECK(mixing.external->centre == 6);
    CHECK(mixing.external->right == 0);
    CHECK(!mixing.external->left_surround);
    CHECK(!mixing.external->right_surround);
    CHECK(mixing.external->lfe == 15);
    CHECK(ac3::meta::kExternalScaleDb[15] == 0.0);  // Table E2.8's -inf row
    CHECK(mixing.external->dmixscl == 9);
    REQUIRE(mixing.external->auxiliary);
    CHECK((*mixing.external->auxiliary)[0] == 2);
    CHECK(!(*mixing.external->auxiliary)[1]);
    REQUIRE(mixing.speech);
    CHECK(mixing.speech->spchdat == 17);
    REQUIRE(mixing.speech->additional);
    CHECK(mixing.speech->additional->spchdat1 == 4);
    CHECK(mixing.speech->additional->spchan1att == 2);
    REQUIRE(mixing.speech->additional->more);
    CHECK(mixing.speech->additional->more->spchdat2 == 30);
    CHECK(mixing.speech->additional->more->spchan2att == 6);
}

TEST_CASE("E-AC-3: mixdef 0x3's auxiliary pair round trips both set, and cleared",
          "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    ac3::meta::MixMetadata mix;
    mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
    ac3::meta::ExternalScales external;
    external.left = 3;

    SECTION("both halves set") {
        external.auxiliary = std::array<std::optional<int>, 2>{5, 9};
        mix.mixing.external = external;
        config.mixing = mix;

        const auto decoded = round_trip_eac3(config);
        REQUIRE(decoded.mixing);
        REQUIRE(decoded.mixing->mixing.external);
        REQUIRE(decoded.mixing->mixing.external->auxiliary);
        CHECK((*decoded.mixing->mixing.external->auxiliary)[0] == 5);
        CHECK((*decoded.mixing->mixing.external->auxiliary)[1] == 9);
    }

    SECTION("addche cleared outright, with other external scales still present") {
        // §E2.3.1.40: addche itself clear, not just its two scales absent -
        // the flag bit, not the values, is what this checks.
        external.auxiliary = std::nullopt;
        mix.mixing.external = external;
        config.mixing = mix;

        const auto decoded = round_trip_eac3(config);
        REQUIRE(decoded.mixing);
        REQUIRE(decoded.mixing->mixing.external);
        CHECK(decoded.mixing->mixing.external->left == 3);
        CHECK(!decoded.mixing->mixing.external->auxiliary);
    }
}

TEST_CASE("E-AC-3: mixdef 0x3's speech enhancement round trips at each nesting depth",
          "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;

    SECTION("spchdat alone, addspchdate clear") {
        ac3::meta::MixMetadata mix;
        mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
        mix.mixing.speech = ac3::meta::SpeechEnhancement{.spchdat = 11};
        config.mixing = mix;

        const auto decoded = round_trip_eac3(config);
        REQUIRE(decoded.mixing);
        REQUIRE(decoded.mixing->mixing.speech);
        CHECK(decoded.mixing->mixing.speech->spchdat == 11);
        CHECK(!decoded.mixing->mixing.speech->additional);
    }

    SECTION("spchdat and additional, addspchdat1e clear") {
        ac3::meta::MixMetadata mix;
        mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
        mix.mixing.speech = ac3::meta::SpeechEnhancement{
            .spchdat = 11,
            .additional = ac3::meta::SpeechEnhancement::Additional{.spchdat1 = 19,
                                                                    .spchan1att = 1}};
        config.mixing = mix;

        const auto decoded = round_trip_eac3(config);
        REQUIRE(decoded.mixing);
        REQUIRE(decoded.mixing->mixing.speech);
        REQUIRE(decoded.mixing->mixing.speech->additional);
        CHECK(decoded.mixing->mixing.speech->additional->spchdat1 == 19);
        CHECK(decoded.mixing->mixing.speech->additional->spchan1att == 1);
        CHECK(!decoded.mixing->mixing.speech->additional->more);
    }
}

TEST_CASE("E-AC-3: mixdef 0x3's mixdeflen sizes the whole element", "[bsi]") {
    // The reader is placed from mixdeflen, not from where the field walk
    // stopped, so a nearly empty mixdata block still has to leave the decoder
    // at the right offset - §E2.3.1.22's minimum is two bytes however little
    // is in it, and §E2.3.1.52's fill makes up the difference.
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    ac3::meta::MixMetadata mix;
    mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
    // Something after the block, so a length error shows up as a wrong value
    // rather than merely a wrong offset nobody reads.
    mix.blkmixcfginfo = std::array<std::optional<int>, ac3::kBlocksPerFrame>{
        11, std::nullopt, std::nullopt, 31, std::nullopt, 0};
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->mixing.mixdef == ac3::meta::MixDefinition::kExtended);
    CHECK(!decoded.mixing->mixing.external);
    CHECK(!decoded.mixing->mixing.speech);
    REQUIRE(decoded.mixing->blkmixcfginfo);
    const auto& words = *decoded.mixing->blkmixcfginfo;
    CHECK(words[0] == 11);
    CHECK(!words[1]);
    CHECK(!words[2]);
    CHECK(words[3] == 31);
    CHECK(!words[4]);
    CHECK(words[5] == 0);
}

TEST_CASE("E-AC-3: blkmixcfginfo's one-block word is unconditional at numblkscod 0x0",
          "[bsi]") {
    // §E2.3.1.60: with one block per syncframe, the per-block flag is
    // INFERRED set and blkmixcfginfo[0] is unconditional - there is no flag
    // bit on the wire at all, unlike the six-block case the test above
    // already covers.
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k2_0;
    config.numblkscod = 0;
    ac3::meta::MixMetadata mix;
    mix.blkmixcfginfo = std::array<std::optional<int>, ac3::kBlocksPerFrame>{
        19, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    config.mixing = mix;

    ac3::eac3::FrameEncoder encoder{config};
    const auto samples = static_cast<std::size_t>(encoder.samples_per_frame());
    const auto full = tone(2);
    std::vector<std::vector<float>> shortened(full.size());
    for (std::size_t ch = 0; ch < full.size(); ++ch) {
        shortened[ch].assign(full[ch].begin(),
                             full[ch].begin() + static_cast<std::ptrdiff_t>(samples));
    }
    const auto spans = views(shortened);
    auto frame = encoder.encode_frame(spans);
    REQUIRE(frame.has_value());
    frame = encoder.encode_frame(spans);
    REQUIRE(frame.has_value());

    ac3::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(*frame);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    const auto& substream = **decoded;
    REQUIRE(substream.mixing);
    REQUIRE(substream.mixing->blkmixcfginfo);
    CHECK((*substream.mixing->blkmixcfginfo)[0] == 19);
}

TEST_CASE("E-AC-3: blkmixcfginfo carries one flag per block the syncframe holds",
          "[bsi]") {
    // §E2.3.1.60 (Table E1.2): past numblkscod 0x0 the per-block form loops
    // over number_of_blocks_per_syncframe - two flags at numblkscod 0x1,
    // three at 0x2, six at 0x3 - not over MixMetadata's six-slot array. An
    // encoder that always wrote six flags left short syncframes with stray
    // bits ahead of infomdate: the decoder refused the frame, io::scan read
    // bsmod 0 instead of 5, and emdf::walk_frame lost the addbsi marker (see
    // tests/emdf/test_emdf.cpp's frame-walker cases). Every numblkscod runs
    // here, each as an encode -> decode round trip and a scan of the same
    // bytes, so the field is checked at the width every reader expects.
    for (int numblkscod = 0; numblkscod <= 3; ++numblkscod) {
        CAPTURE(numblkscod);
        ac3::eac3::FrameConfig config;
        config.acmod = ac3::Acmod::k2_0;
        config.numblkscod = numblkscod;
        ac3::meta::MixMetadata mix;
        mix.blkmixcfginfo = std::array<std::optional<int>, ac3::kBlocksPerFrame>{
            3, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        config.mixing = mix;
        config.info = ac3::meta::BsiInfo{.bsmod = ac3::meta::BitstreamMode::kCommentary};

        ac3::eac3::FrameEncoder encoder{config};
        const auto samples = static_cast<std::size_t>(encoder.samples_per_frame());
        const auto full = tone(2);
        std::vector<std::vector<float>> shortened(full.size());
        for (std::size_t ch = 0; ch < full.size(); ++ch) {
            shortened[ch].assign(full[ch].begin(),
                                 full[ch].begin() + static_cast<std::ptrdiff_t>(samples));
        }
        const auto spans = views(shortened);
        std::vector<std::byte> stream;
        std::vector<std::byte> last;
        for (int f = 0; f < 3; ++f) {
            const auto frame = encoder.encode_frame(spans);
            REQUIRE(frame.has_value());
            stream.insert(stream.end(), frame->begin(), frame->end());
            last = *frame;
        }

        ac3::Eac3Decoder decoder;
        const auto decoded = decoder.decode_access_unit(last);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());

        ac3::Eac3Decoder substream_decoder;
        const auto substream = substream_decoder.decode_substream(last);
        REQUIRE(substream.has_value());
        REQUIRE(substream->has_value());
        REQUIRE((*substream)->mixing);
        REQUIRE((*substream)->mixing->blkmixcfginfo);
        const auto& words = *(*substream)->mixing->blkmixcfginfo;
        CHECK(words[0] == 3);
        CHECK(!words[1]);

        const auto scanned = ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        CHECK(scanned->bsmod_present);
        CHECK(scanned->bsmod == 5);
    }
}

TEST_CASE("E-AC-3: pan information round trips on a mono programme", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k1_0;  // §E2.3.1.53: acmod < 0x2 only
    ac3::meta::MixMetadata mix;
    mix.pan = ac3::meta::PanInfo{.panmean = 200, .paninfo = 41};
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    REQUIRE(decoded.mixing->pan);
    CHECK(decoded.mixing->pan->panmean == 200);
    CHECK(decoded.mixing->pan->paninfo == 41);
    CHECK(!decoded.mixing->pan2);
}

TEST_CASE("E-AC-3: 1+1 carries a second pan position and programme scale", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::kDualMono;
    config.dialnorm2 = 27;
    ac3::meta::MixMetadata mix;
    mix.pgmscl = 51;   // 0 dB, stated rather than implied
    mix.pgmscl2 = 20;  // -31 dB
    mix.pan = ac3::meta::PanInfo{.panmean = 8};
    mix.pan2 = ac3::meta::PanInfo{.panmean = 232, .paninfo = 63};
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->pgmscl == 51);
    CHECK(decoded.mixing->pgmscl2 == 20);
    REQUIRE(decoded.mixing->pan);
    CHECK(decoded.mixing->pan->panmean == 8);
    REQUIRE(decoded.mixing->pan2);
    CHECK(decoded.mixing->pan2->panmean == 232);
    CHECK(decoded.mixing->pan2->paninfo == 63);
}

TEST_CASE("E-AC-3: the infomdat group round trips", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;  // acmod >= 0x6, so dsurexmod is sent
    config.lfe = true;
    config.info = ac3::meta::BsiInfo{
        .bsmod = ac3::meta::BitstreamMode::kCommentary,
        .dsurexmod = ac3::meta::SurroundExMode::kProLogicIIz,
        .audprod = ac3::meta::AudioProduction{
            .mixlevel = 18,
            .roomtyp = ac3::meta::RoomType::kLargeRoomXCurve,
            .adconvtyp = ac3::meta::AdConverterType::kHdcd},
        .copyrightb = true,
        .sourcefscod = true,
    };

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.info);
    CHECK(decoded.info->bsmod == ac3::meta::BitstreamMode::kCommentary);
    CHECK(decoded.info->dsurexmod == ac3::meta::SurroundExMode::kProLogicIIz);
    REQUIRE(decoded.info->audprod);
    CHECK(decoded.info->audprod->mixlevel == 18);
    CHECK(decoded.info->audprod->roomtyp == ac3::meta::RoomType::kLargeRoomXCurve);
    // Unlike AC-3's audprodie, Annex E's carries adconvtyp as a third field.
    CHECK(decoded.info->audprod->adconvtyp == ac3::meta::AdConverterType::kHdcd);
    CHECK(decoded.info->copyrightb);
    CHECK(decoded.info->origbs);
    CHECK(decoded.info->sourcefscod);
    // 3/2 sends no dsurmod or dheadphonmod at all - those are 2/0's fields.
    CHECK(decoded.info->dsurmod == ac3::meta::SurroundMode::kNotIndicated);
    CHECK(decoded.info->dheadphonmod == ac3::meta::HeadphoneMode::kNotIndicated);
}

TEST_CASE("E-AC-3: 2/0's infomdat carries dsurmod and dheadphonmod", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k2_0;
    config.info = ac3::meta::BsiInfo{
        .dsurmod = ac3::meta::SurroundMode::kNotDolbySurround,
        .dheadphonmod = ac3::meta::HeadphoneMode::kDolbyHeadphone,
    };

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.info);
    CHECK(decoded.info->dsurmod == ac3::meta::SurroundMode::kNotDolbySurround);
    CHECK(decoded.info->dheadphonmod == ac3::meta::HeadphoneMode::kDolbyHeadphone);
    // A 2/0 stream is below acmod 0x6, so no dsurexmod is on the wire.
    CHECK(decoded.info->dsurexmod == ac3::meta::SurroundExMode::kNotIndicated);
}

TEST_CASE("E-AC-3: a dependent substream stops after the mixmdate levels", "[bsi]") {
    // Table E1.2 gates the programme-scaling group on strmtyp == 0x0: a
    // dependent is part of someone else's programme and has no second one to
    // mix against. Setting the fields anyway must not put them on the wire.
    ac3::eac3::FrameConfig config;
    config.strmtyp = ac3::eac3::StreamType::kDependent;
    config.acmod = ac3::Acmod::k2_2;
    ac3::meta::MixMetadata mix;
    mix.ltrtsurmixlev = ac3::meta::MixLevel::kMinus4_5dB;
    mix.lorosurmixlev = ac3::meta::MixLevel::kMinus1_5dB;
    mix.pgmscl = 30;
    mix.blkmixcfginfo = std::array<std::optional<int>, ac3::kBlocksPerFrame>{7, 7, 7, 7, 7, 7};
    config.mixing = mix;

    const auto decoded = round_trip_eac3(config);
    REQUIRE(decoded.mixing);
    CHECK(decoded.mixing->ltrtsurmixlev == ac3::meta::MixLevel::kMinus4_5dB);
    CHECK(decoded.mixing->lorosurmixlev == ac3::meta::MixLevel::kMinus1_5dB);
    CHECK(!decoded.mixing->pgmscl);
    CHECK(!decoded.mixing->blkmixcfginfo);
}

TEST_CASE("E-AC-3: a mixmdate value wider than its field is refused", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    ac3::meta::MixMetadata mix;
    mix.pgmscl = 64;  // §E2.3.1.13 stops at 63
    config.mixing = mix;

    ac3::eac3::FrameEncoder encoder{config};
    const auto pcm = tone(5);
    const auto frame = encoder.encode_frame(views(pcm));
    REQUIRE(!frame.has_value());
    CHECK(frame.error() == ac3::FrameError::kInvalidBsi);
}

TEST_CASE("E-AC-3: valid_mix_metadata() checks mixdef, pan and blkmixcfginfo ranges too",
          "[bsi]") {
    // The dmixmod/pgmscl cases above already exercise this function; this
    // covers the rest of what it validates: premixcmpscl's field width
    // (0..7, §E2.3.1.21), the twelve reserved bits (§E2.3.1.23), each Table
    // E2.8 external scale (0..15), panmean/paninfo (§E2.3.1.54-55) and each
    // blkmixcfginfo word (§E2.3.1.61).
    ac3::meta::MixMetadata mix;

    SECTION("mixdef 0x1's premixcmpscl over 7 is refused") {
        mix.mixing.mixdef = ac3::meta::MixDefinition::kPremix;
        mix.mixing.premix.premixcmpscl = 8;
        CHECK_FALSE(ac3::meta::valid_mix_metadata(mix));
    }

    SECTION("mixdef 0x2's reserved bits over twelve wide is refused") {
        mix.mixing.mixdef = ac3::meta::MixDefinition::kReserved;
        mix.mixing.reserved = 0x1000;  // one bit past the twelve
        CHECK_FALSE(ac3::meta::valid_mix_metadata(mix));
    }

    SECTION("mixdef 0x3's external scale over 15 is refused") {
        mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
        ac3::meta::ExternalScales external;
        external.left = 16;
        mix.mixing.external = external;
        CHECK_FALSE(ac3::meta::valid_mix_metadata(mix));
    }

    SECTION("panmean over 239 is refused") {
        mix.pan = ac3::meta::PanInfo{.panmean = 240};
        CHECK_FALSE(ac3::meta::valid_mix_metadata(mix));
    }

    SECTION("paninfo over 63 is refused") {
        mix.pan = ac3::meta::PanInfo{.panmean = 0, .paninfo = 64};
        CHECK_FALSE(ac3::meta::valid_mix_metadata(mix));
    }

    SECTION("a blkmixcfginfo word over 31 is refused") {
        mix.blkmixcfginfo = std::array<std::optional<int>, ac3::kBlocksPerFrame>{
            32, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        CHECK_FALSE(ac3::meta::valid_mix_metadata(mix));
    }

    SECTION("every field at its widest legal value is accepted") {
        mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
        ac3::meta::ExternalScales external;
        external.left = 15;
        mix.mixing.external = external;
        mix.pan = ac3::meta::PanInfo{.panmean = 239, .paninfo = 63};
        mix.blkmixcfginfo =
            std::array<std::optional<int>, ac3::kBlocksPerFrame>{31, 31, 31, 31, 31, 31};
        CHECK(ac3::meta::valid_mix_metadata(mix));
    }
}

TEST_CASE("E-AC-3: mixmdate's reserved dmixmod is kept as sent and never written",
          "[bsi][eac3]") {
    // Annex E defines no dmixmod of its own (§E2.2, TS 102 366 clause
    // E.1.2.0), so Table D2.2 applies to mixmdate unchanged and '11' is
    // reserved here exactly as it is in Annex D.
    const auto encode = [](ac3::meta::DownmixMode dmixmod) {
        ac3::eac3::FrameConfig config;
        config.acmod = ac3::Acmod::k3_2;  // acmod > 0x2, so dmixmod is sent
        config.lfe = true;
        config.mixing = ac3::meta::MixMetadata{.dmixmod = dmixmod, .lfemixlevcod = 3};
        ac3::eac3::FrameEncoder encoder{config};
        const auto pcm = tone(6);
        const auto spans = views(pcm);
        auto frame = encoder.encode_frame(spans);
        REQUIRE(frame.has_value());
        frame = encoder.encode_frame(spans);
        REQUIRE(frame.has_value());
        return *frame;
    };

    SECTION("a stream carrying it decodes, and every reader reports it") {
        const auto reserved = reserved_dmixmod_frame(encode);

        ac3::Eac3Decoder decoder;
        const auto decoded = decoder.decode_substream(reserved);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto& substream = **decoded;
        REQUIRE(substream.mixing);
        CHECK(substream.mixing->dmixmod == ac3::meta::DownmixMode::kReserved);
        CHECK(substream.mixing->lfemixlevcod == 3);
        CHECK(ac3::meta::describe(substream.mixing->dmixmod) == "reserved");

        // The output stage passes it on as sent; automatic selection reads it
        // as "not indicated" (§D2.3.1.2) and takes the plain fold.
        const auto levels = ac3::mix_levels(substream.mixing);
        CHECK(levels.preferred == ac3::meta::DownmixMode::kReserved);
        CHECK(ac3::automatic_stereo_target(substream.acmod, levels.preferred) ==
              ac3::DownmixTarget::kLoRo);

        const auto header = ac3::io::read_frame_header(reserved);
        REQUIRE(header.has_value());
        CHECK(header->dmixmod == ac3::meta::DownmixMode::kReserved);
        const auto wire = ac3::io::read_frame_metadata(reserved);
        REQUIRE(wire.has_value());
        REQUIRE(wire->mix);
        CHECK(wire->mix->dmixmod == ac3::meta::DownmixMode::kReserved);

        ac3::Eac3Decoder reference_decoder;
        const auto reference =
            reference_decoder.decode_substream(encode(ac3::meta::DownmixMode::kLtRt));
        REQUIRE(reference.has_value());
        REQUIRE(reference->has_value());
        const bool same_audio = substream.channels == (*reference)->channels;
        CHECK(same_audio);
    }

    SECTION("the encoder refuses to write it") {
        ac3::eac3::FrameConfig config;
        config.acmod = ac3::Acmod::k3_2;
        config.mixing = ac3::meta::MixMetadata{.dmixmod = ac3::meta::DownmixMode::kReserved};
        ac3::eac3::FrameEncoder encoder{config};
        const auto pcm = tone(5);
        const auto frame = encoder.encode_frame(views(pcm));
        REQUIRE(!frame.has_value());
        CHECK(frame.error() == ac3::FrameError::kInvalidMixLevel);
        CHECK_FALSE(ac3::meta::valid_mix_metadata(*config.mixing));
    }
}

TEST_CASE("E-AC-3: a stream that asks for neither group writes neither flag", "[bsi]") {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k3_2;
    const auto decoded = round_trip_eac3(config);
    CHECK(!decoded.mixing);
    CHECK(!decoded.info);
}

// --- the plan and CLI vocabularies ------------------------------------------

TEST_CASE("plan: annexd and a time code cannot share the same 28 bits", "[bsi]") {
    ac3::plan::Plan plan;
    plan.codec = ac3::plan::Codec::kAc3;
    plan.meta.annexd = true;
    plan.meta.info.timecod1 = ac3::meta::TimeCodeCoarse{.hours = 1};
    const auto error = ac3::plan::validate(plan);
    REQUIRE(error);
    CHECK(*error == ac3::plan::PlanError::kTimecodeNeedsBsid8);

    // E-AC-3 has neither an alternate syntax nor a time code field, so the
    // same plan is simply inert there rather than in conflict.
    plan.codec = ac3::plan::Codec::kEac3;
    CHECK(!ac3::plan::validate(plan));
}

TEST_CASE("plan: mix_metadata takes explicit levels over the widened ones", "[bsi]") {
    ac3::plan::Metadata options;
    options.cmixlev = ac3::meta::CentreMixLevel::kMinus6dB;
    options.surmixlev = ac3::meta::SurroundMixLevel::kMinus3dB;
    // Unset, so the derivation stands.
    CHECK(ac3::plan::mix_metadata(options).lorocmixlev == ac3::meta::MixLevel::kMinus6dB);
    CHECK(ac3::plan::mix_metadata(options).lorosurmixlev == ac3::meta::MixLevel::kMinus3dB);
    CHECK(ac3::plan::mix_metadata(options).ltrtcmixlev == ac3::meta::MixLevel::kMinus3dB);

    options.lorocmixlev = ac3::meta::MixLevel::kPlus1_5dB;
    options.ltrtcmixlev = ac3::meta::MixLevel::kUnity;
    CHECK(ac3::plan::mix_metadata(options).lorocmixlev == ac3::meta::MixLevel::kPlus1_5dB);
    CHECK(ac3::plan::mix_metadata(options).ltrtcmixlev == ac3::meta::MixLevel::kUnity);
    // The widened surround level is untouched by the centre override.
    CHECK(ac3::plan::mix_metadata(options).lorosurmixlev == ac3::meta::MixLevel::kMinus3dB);
}

TEST_CASE("plan: alternate_bsi reuses the derived levels and the xbsi2 group", "[bsi]") {
    ac3::plan::Metadata options;
    options.dmixmod = ac3::meta::DownmixMode::kLtRt;
    options.lorosurmixlev = ac3::meta::MixLevel::kSilent;
    options.info.dsurexmod = ac3::meta::SurroundExMode::kSurroundEx;
    options.adconvtyp = ac3::meta::AdConverterType::kHdcd;
    options.encinfo = true;

    const auto alternate = ac3::plan::alternate_bsi(options);
    REQUIRE(alternate.mix);
    CHECK(alternate.mix->dmixmod == ac3::meta::DownmixMode::kLtRt);
    CHECK(alternate.mix->lorosurmixlev == ac3::meta::MixLevel::kSilent);
    REQUIRE(alternate.extended);
    // dsurexmod is stated on `info` because E-AC-3's infomdat carries the same
    // field; alternate_bsi() is what puts it in xbsi2 for the AC-3 path.
    CHECK(alternate.extended->dsurexmod == ac3::meta::SurroundExMode::kSurroundEx);
    CHECK(alternate.extended->adconvtyp == ac3::meta::AdConverterType::kHdcd);
    CHECK(alternate.extended->encinfo);
}

TEST_CASE("meta: the bsi token vocabularies parse and describe", "[bsi]") {
    ac3::meta::BitstreamMode bsmod{};
    CHECK(ac3::meta::parse_bsmod("vi", bsmod));
    CHECK(bsmod == ac3::meta::BitstreamMode::kVisuallyImpaired);
    // Table 5.7's raw code is a first-class spelling, since that is what a
    // broadcast spec quotes.
    CHECK(ac3::meta::parse_bsmod("5", bsmod));
    CHECK(bsmod == ac3::meta::BitstreamMode::kCommentary);
    CHECK(!ac3::meta::parse_bsmod("8", bsmod));
    CHECK(!ac3::meta::parse_bsmod("karaoke", bsmod));

    // Code 7 means two different services and acmod is what tells them apart.
    CHECK(ac3::meta::describe(ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, ac3::Acmod::k1_0) ==
          "associated service: voice over (VO)");
    CHECK(ac3::meta::describe(ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, ac3::Acmod::k3_2) ==
          "main audio service: karaoke");

    ac3::meta::SurroundExMode surround_ex{};
    CHECK(ac3::meta::parse_surround_ex_mode("pliiz", surround_ex));
    CHECK(surround_ex == ac3::meta::SurroundExMode::kProLogicIIz);
    CHECK(!ac3::meta::parse_surround_ex_mode("on", surround_ex));

    ac3::meta::RoomType room{};
    CHECK(ac3::meta::parse_room_type("small", room));
    CHECK(room == ac3::meta::RoomType::kSmallRoomFlat);

    ac3::meta::AdConverterType converter{};
    CHECK(ac3::meta::parse_ad_converter("hdcd", converter));
    CHECK(converter == ac3::meta::AdConverterType::kHdcd);

    ac3::meta::HeadphoneMode headphone{};
    CHECK(ac3::meta::parse_headphone_mode("on", headphone));
    CHECK(headphone == ac3::meta::HeadphoneMode::kDolbyHeadphone);

    ac3::meta::SurroundMode surround{};
    CHECK(ac3::meta::parse_surround_mode("off", surround));
    CHECK(surround == ac3::meta::SurroundMode::kNotDolbySurround);
}

TEST_CASE("meta: every dmixmod code has a name, and only three are writable", "[bsi]") {
    // Table D2.2 (TS 102 366 Table D.1.1), which both codecs share.
    CHECK(ac3::meta::describe(ac3::meta::DownmixMode::kNotIndicated) == "not indicated");
    CHECK(ac3::meta::describe(ac3::meta::DownmixMode::kLtRt) == "Lt/Rt");
    CHECK(ac3::meta::describe(ac3::meta::DownmixMode::kLoRo) == "Lo/Ro");
    CHECK(ac3::meta::describe(ac3::meta::DownmixMode::kReserved) == "reserved");
    CHECK(static_cast<int>(ac3::meta::DownmixMode::kReserved) == 3);

    CHECK(ac3::meta::valid_downmix_mode(ac3::meta::DownmixMode::kNotIndicated));
    CHECK(ac3::meta::valid_downmix_mode(ac3::meta::DownmixMode::kLtRt));
    CHECK(ac3::meta::valid_downmix_mode(ac3::meta::DownmixMode::kLoRo));
    CHECK_FALSE(ac3::meta::valid_downmix_mode(ac3::meta::DownmixMode::kReserved));

    // Annex D's writer takes the same view as mixmdate's.
    ac3::meta::AlternateBsi alternate;
    alternate.mix = ac3::meta::MixMetadata{.dmixmod = ac3::meta::DownmixMode::kReserved};
    CHECK_FALSE(ac3::meta::valid_alternate_bsi(alternate));
    alternate.mix->dmixmod = ac3::meta::DownmixMode::kNotIndicated;
    CHECK(ac3::meta::valid_alternate_bsi(alternate));
}

TEST_CASE("meta: a time code splits across the two halves at eight seconds", "[bsi]") {
    ac3::meta::TimeCodeCoarse coarse;
    ac3::meta::TimeCodeFine fine;

    REQUIRE(ac3::meta::parse_timecode("01:02:03", coarse, fine));
    CHECK(coarse.hours == 1);
    CHECK(coarse.minutes == 2);
    CHECK(coarse.eight_seconds == 0);
    CHECK(fine.seconds == 3);
    CHECK(fine.frames == 0);

    REQUIRE(ac3::meta::parse_timecode("23:59:59:29.63", coarse, fine));
    CHECK(coarse.hours == 23);
    CHECK(coarse.minutes == 59);
    CHECK(coarse.eight_seconds == 7);  // 59 / 8
    CHECK(fine.seconds == 3);          // 59 % 8
    CHECK(fine.frames == 29);
    CHECK(fine.sixty_fourths == 63);
    CHECK(ac3::meta::format_timecode(coarse, fine) == "23:59:59:29.63");

    CHECK(!ac3::meta::parse_timecode("24:00:00", coarse, fine));
    CHECK(!ac3::meta::parse_timecode("00:60:00", coarse, fine));
    CHECK(!ac3::meta::parse_timecode("00:00:60", coarse, fine));
    CHECK(!ac3::meta::parse_timecode("00:00:00:30", coarse, fine));
    CHECK(!ac3::meta::parse_timecode("00:00:00:00.64", coarse, fine));
    CHECK(!ac3::meta::parse_timecode("00:00", coarse, fine));
    CHECK(!ac3::meta::parse_timecode("", coarse, fine));
}
