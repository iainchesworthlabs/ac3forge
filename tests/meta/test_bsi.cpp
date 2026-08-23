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
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/encoder/plan.hpp"
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
    CHECK(decoded.cmixlev == ac3::meta::CentreMixLevel::kMinus4_5dB);
    CHECK(decoded.surmixlev == ac3::meta::SurroundMixLevel::kMinus6dB);
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
    options.xbsi2.adconvtyp = ac3::meta::AdConverterType::kHdcd;
    options.xbsi2.encinfo = true;

    const auto alternate = ac3::plan::alternate_bsi(options);
    REQUIRE(alternate.mix);
    CHECK(alternate.mix->dmixmod == ac3::meta::DownmixMode::kLtRt);
    CHECK(alternate.mix->lorosurmixlev == ac3::meta::MixLevel::kSilent);
    REQUIRE(alternate.extended);
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
