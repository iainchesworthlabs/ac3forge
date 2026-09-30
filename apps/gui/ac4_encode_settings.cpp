#include "ac4_encode_settings.hpp"

#include <cmath>
#include <fmt/format.h>
#include <limits>

#include "ac4_encode_core.hpp"

namespace ac3gui {

namespace {

// Part 1 Tables 149 and 149a in dB, in the order of kAc4CentreLevels and
// kAc4SurroundLevels; "off" is -infinity, as ac3cli reads it.
constexpr double kOff = -std::numeric_limits<double>::infinity();
constexpr std::array<double, 8> kCentreDb{3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0, kOff};
constexpr std::array<double, 6> kSurroundDb{0.0, -1.5, -3.0, -4.5, -6.0, kOff};

constexpr std::array<iclforge::ac4::RateMode, 3> kRateModes{iclforge::ac4::RateMode::kConstant,
                                                            iclforge::ac4::RateMode::kAverage,
                                                            iclforge::ac4::RateMode::kVariable};
constexpr std::array<iclforge::ac4::CodecMode, 6> kCodecModes{
    iclforge::ac4::CodecMode::kAuto,      iclforge::ac4::CodecMode::kSimple,
    iclforge::ac4::CodecMode::kAspx,      iclforge::ac4::CodecMode::kAspxAcpl1,
    iclforge::ac4::CodecMode::kAspxAcpl2, iclforge::ac4::CodecMode::kAspxAcpl3};
constexpr std::array<iclforge::ac4::LoudnessPractice, 7> kPractices{
    iclforge::ac4::LoudnessPractice::kEbuR128,
    iclforge::ac4::LoudnessPractice::kAtscA85,
    iclforge::ac4::LoudnessPractice::kAribTrB32,
    iclforge::ac4::LoudnessPractice::kFreeTvOp59,
    iclforge::ac4::LoudnessPractice::kManual,
    iclforge::ac4::LoudnessPractice::kConsumerLeveller,
    iclforge::ac4::LoudnessPractice::kNotIndicated};
constexpr std::array<iclforge::ac4::DrcProfile, 5> kProfiles{
    iclforge::ac4::DrcProfile::kFilmStandard, iclforge::ac4::DrcProfile::kFilmLight,
    iclforge::ac4::DrcProfile::kMusicStandard, iclforge::ac4::DrcProfile::kMusicLight,
    iclforge::ac4::DrcProfile::kSpeech};
constexpr std::array<iclforge::ac4::PreferredDownmix, 4> kPreferred{
    iclforge::ac4::PreferredDownmix::kLoRo, iclforge::ac4::PreferredDownmix::kLtRt,
    iclforge::ac4::PreferredDownmix::kLtRtProLogicII,
    iclforge::ac4::PreferredDownmix::kNotIndicated};

// The index `at` of a table of `size` entries, or the first where it is past
// the end.
[[nodiscard]] std::size_t within(std::size_t at, std::size_t size) {
    return at < size ? at : 0;
}

[[nodiscard]] std::string dialogue_channels(const Ac4EncodeSettings& s) {
    std::string out;
    const auto add = [&out](bool on, std::string_view name) {
        if (on) {
            out += out.empty() ? "" : ",";
            out += name;
        }
    };
    add(s.dialogue_left, "l");
    add(s.dialogue_right, "r");
    add(s.dialogue_centre, "c");
    return out;
}

}  // namespace

bool ac4_downmix_named(const Ac4EncodeSettings& settings) {
    return settings.centre_level.has_value() || settings.surround_level.has_value() ||
           settings.preferred_downmix.has_value();
}

bool ac4_dialogue_named(const Ac4EncodeSettings& settings) {
    return settings.dialogue_left || settings.dialogue_right || settings.dialogue_centre;
}

std::optional<iclforge::ac4::LoudnessPractice> ac4_loudness_practice(
    const Ac4EncodeSettings& settings) {
    if (!settings.loudness) {
        return std::nullopt;
    }
    return kPractices[within(*settings.loudness, kPractices.size())];
}

std::vector<std::string> ac4_cli_tokens(const Ac4EncodeSettings& s, bool mp4) {
    std::vector<std::string> out;
    const auto put = [&out](std::string_view key, std::string_view value) {
        out.push_back(fmt::format("{}={}", key, value));
    };
    if (const std::size_t rate = within(s.frame_rate, kAc4FrameRates.size());
        rate != kAc4NativeFrameRate) {
        put("frame-rate", kAc4FrameRates[rate].token);
    }
    if (const std::size_t mode = within(s.rate_mode, kAc4RateModes.size()); mode != 0) {
        put("rate-mode", kAc4RateModes[mode].token);
    }
    if (const std::size_t mode = within(s.codec_mode, kAc4CodecModes.size()); mode != 0) {
        put("codec-mode", kAc4CodecModes[mode].token);
    }
    // With loudness= the dialnorm is always named: without it, ac4-encode
    // measures one, and the page would not show which it sends.
    if (s.measure_dialnorm) {
        put("dialnorm", "auto");
    } else if (s.dialnorm_db != 31.0 || s.loudness) {
        put("dialnorm", fmt::format("{:g}", s.dialnorm_db));
    }
    if (s.loudness) {
        put("loudness", kAc4LoudnessPractices[within(*s.loudness, kPractices.size())].token);
    }
    if (s.drc) {
        put("drc", kAc4DrcProfiles[within(*s.drc, kProfiles.size())].token);
    }
    if (s.centre_level) {
        put("cmixlev", kAc4CentreLevels[within(*s.centre_level, kCentreDb.size())].token);
    }
    if (s.surround_level) {
        put("surmixlev", kAc4SurroundLevels[within(*s.surround_level, kSurroundDb.size())].token);
    }
    if (s.preferred_downmix) {
        put("dmixmod",
            kAc4PreferredDownmixes[within(*s.preferred_downmix, kPreferred.size())].token);
    }
    if (ac4_dialogue_named(s)) {
        put("dialogue-channels", dialogue_channels(s));
        if (s.dialogue_mid) {
            put("dialogue-method", "mid");
        }
        if (const int gain =
                kAc4DialogueMaxGains[within(s.dialogue_max_gain, kAc4DialogueMaxGains.size())];
            gain != 9) {
            put("dialogue-max-gain", fmt::format("{}", gain));
        }
    }
    if (s.iframe_interval != 24) {
        put("iframe-interval", fmt::format("{}", s.iframe_interval));
    }
    if (!s.crc && !mp4) {
        put("crc", "off");
    }
    return out;
}

std::optional<std::string> ac4_settings_refusal(const Ac4EncodeSettings& settings,
                                                std::size_t channels, int sample_rate_hz) {
    if (sample_rate_hz == 44100 && within(settings.frame_rate, kAc4FrameRates.size()) !=
                                       kAc4NativeFrameRate) {
        return std::string{
            "at 44.1 kHz AC-4 has the native frame rate alone (Part 1 Table 83); frame-rate= "
            "names another"};
    }
    if (ac4_downmix_named(settings) && channels < 5) {
        return fmt::format(
            "the downmix options describe the stereo downmix of 5.0, 5.1, 7.0, 7.1 and the "
            "immersive layouts; the source is {}",
            iclforge::apps::ac4_layout_name(channels, iclforge::ac4::AdditionalPair::kNone));
    }
    return std::nullopt;
}

std::optional<int> ac4_object_dialnorm(const Ac4EncodeSettings& s) {
    if (s.measure_dialnorm || s.dialnorm_db < 1.0 || s.dialnorm_db > 31.0 ||
        s.dialnorm_db != std::floor(s.dialnorm_db)) {
        return std::nullopt;
    }
    return static_cast<int>(s.dialnorm_db);
}

std::vector<std::string> ac4_object_cli_tokens(const Ac4EncodeSettings& s, bool mp4) {
    std::vector<std::string> out;
    if (within(s.object_coding, kAc4ObjectCodings.size()) != 0) {
        out.push_back(fmt::format(
            "coding={}",
            kAc4ObjectCodings[within(s.object_coding, kAc4ObjectCodings.size())].token));
    }
    // atmos-encode's own default is 31, as ac4-encode's.
    if (const auto dialnorm = ac4_object_dialnorm(s); dialnorm && *dialnorm != 31) {
        out.push_back(fmt::format("dialnorm={}", *dialnorm));
    }
    if (!s.crc && !mp4) {
        out.push_back("crc=off");
    }
    return out;
}

std::optional<std::string> ac4_object_settings_refusal(const Ac4EncodeSettings& s) {
    if (s.measure_dialnorm) {
        return std::string{
            "an AC-4 object stream has no bed to measure a loudness on; set the dialnorm by hand, "
            "1 to 31"};
    }
    if (!ac4_object_dialnorm(s)) {
        return fmt::format(
            "an AC-4 object stream takes its dialnorm in whole dB, 1 to 31, as atmos-encode reads "
            "it; the setting is {:g}",
            s.dialnorm_db);
    }
    return std::nullopt;
}

iclforge::apps::Ac4ObjectsParams ac4_objects_params(const Ac4EncodeSettings& s,
                                               std::uint32_t sample_rate_hz, int bitrate_kbps) {
    iclforge::apps::Ac4ObjectsParams params;
    params.sample_rate_hz = sample_rate_hz;
    params.bitrate_kbps = bitrate_kbps;
    params.dialnorm_db = ac4_object_dialnorm(s).value_or(31);
    params.coding = within(s.object_coding, kAc4ObjectCodings.size()) == 0
                        ? iclforge::ac4::ObjectCoding::kAjoc
                        : iclforge::ac4::ObjectCoding::kDirect;
    return params;
}

iclforge::ac4::EncoderConfig ac4_encoder_config(const Ac4EncodeSettings& s, int channels,
                                                int sample_rate_hz, int bitrate_kbps) {
    iclforge::ac4::EncoderConfig config;
    config.channels = channels;
    config.sample_rate_hz = sample_rate_hz;
    config.frame_rate_index = static_cast<int>(within(s.frame_rate, kAc4FrameRates.size()));
    config.bitrate_kbps = bitrate_kbps;
    config.rate_mode = kRateModes[within(s.rate_mode, kRateModes.size())];
    config.codec_mode = kCodecModes[within(s.codec_mode, kCodecModes.size())];
    config.iframe_interval = s.iframe_interval;
    config.dialnorm_db = -s.dialnorm_db;
    if (s.drc) {
        // ac4-encode's drc=: Table 161's four modes on the profile.
        iclforge::ac4::DrcConfig drc;
        drc.profile = kProfiles[within(*s.drc, kProfiles.size())];
        for (int id = 0; id < 4; ++id) {
            drc.modes.push_back(
                iclforge::ac4::DrcModeConfig{.id = id, .gains_config = std::nullopt});
        }
        config.drc = drc;
    }
    if (ac4_downmix_named(s)) {
        iclforge::ac4::DownmixConfig downmix;
        if (s.centre_level) {
            downmix.loro_centre_db = kCentreDb[within(*s.centre_level, kCentreDb.size())];
        }
        if (s.surround_level) {
            downmix.loro_surround_db = kSurroundDb[within(*s.surround_level, kSurroundDb.size())];
        }
        if (s.preferred_downmix) {
            downmix.preferred = kPreferred[within(*s.preferred_downmix, kPreferred.size())];
        }
        config.downmix = downmix;
    }
    if (ac4_dialogue_named(s)) {
        iclforge::ac4::DialogueConfig dialogue;
        dialogue.method = s.dialogue_mid ? iclforge::ac4::DialogueMethod::kMid
                                         : iclforge::ac4::DialogueMethod::kChannelIndependent;
        dialogue.source = iclforge::ac4::DialogueSource::kMarkedChannels;
        dialogue.max_gain_db =
            kAc4DialogueMaxGains[within(s.dialogue_max_gain, kAc4DialogueMaxGains.size())];
        dialogue.left = s.dialogue_left;
        dialogue.right = s.dialogue_right;
        dialogue.centre = s.dialogue_centre;
        config.dialogue = dialogue;
    }
    return config;
}

}  // namespace ac3gui
