#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac4/ac4.hpp"
#include "ac4_encode_core.hpp"
#include "ac4_encode_settings.hpp"
#include "ac4_presentations.hpp"
#include "iclforge/ac4enc/encoder.hpp"

// ac3gui's AC-4 page (apps/gui/ac4_encode_settings.hpp): each choice echoes
// the `ac3cli ac4-encode` token ac3cli's parser reads for it, and builds the
// configuration ac4-encode builds from that token. The Qt Quick suite
// (tst_e2e_ac4.qml) runs the echoed line through ac3cli and compares the
// bytes; this holds the two halves apart so a failure there says which one
// moved.

using ac3gui::Ac4EncodeSettings;

namespace {

std::string joined(const std::vector<std::string>& tokens) {
    std::string out;
    for (const std::string& token : tokens) {
        out += out.empty() ? "" : " ";
        out += token;
    }
    return out;
}

}  // namespace

TEST_CASE("AC-4 page settings at their defaults echo no token", "[gui]") {
    const Ac4EncodeSettings settings;
    CHECK(ac3gui::ac4_cli_tokens(settings, false).empty());
    CHECK(ac3gui::ac4_cli_tokens(settings, true).empty());
    const ac4::EncoderConfig config = ac3gui::ac4_encoder_config(settings, 2, 48000, 192);
    const ac4::EncoderConfig plain{};
    CHECK(config.frame_rate_index == plain.frame_rate_index);
    CHECK(config.rate_mode == plain.rate_mode);
    CHECK(config.codec_mode == plain.codec_mode);
    CHECK(config.iframe_interval == plain.iframe_interval);
    CHECK(config.dialnorm_db == plain.dialnorm_db);
    CHECK_FALSE(config.drc.has_value());
    CHECK_FALSE(config.downmix.has_value());
    CHECK_FALSE(config.dialogue.has_value());
    CHECK_FALSE(config.loudness.has_value());
}

TEST_CASE("each AC-4 page option echoes the token ac4-encode parses", "[gui]") {
    Ac4EncodeSettings s;
    s.frame_rate = 2;  // 25 fps
    s.rate_mode = 1;
    s.codec_mode = 2;
    s.dialnorm_db = 27.25;
    s.loudness = 0;
    s.drc = 1;
    s.centre_level = 5;
    s.surround_level = 5;
    s.preferred_downmix = 2;
    s.dialogue_left = true;
    s.dialogue_centre = true;
    s.dialogue_mid = true;
    s.dialogue_max_gain = 3;
    s.iframe_interval = 12;
    s.crc = false;
    CHECK(joined(ac3gui::ac4_cli_tokens(s, false)) ==
          "frame-rate=25 rate-mode=average codec-mode=aspx dialnorm=27.25 loudness=ebu-r128 "
          "drc=film-light cmixlev=-4.5 surmixlev=off dmixmod=pl2 dialogue-channels=l,c "
          "dialogue-method=mid dialogue-max-gain=12 iframe-interval=12 crc=off");
    // An MP4 sample has no CRC to turn off, and ac4-encode refuses crc= there.
    CHECK(joined(ac3gui::ac4_cli_tokens(s, true)).find("crc=") == std::string::npos);

    Ac4EncodeSettings measured;
    measured.measure_dialnorm = true;
    CHECK(joined(ac3gui::ac4_cli_tokens(measured, false)) == "dialnorm=auto");
    // loudness= measures dialnorm too unless dialnorm= names it, so the page
    // names it whenever loudness= is on.
    Ac4EncodeSettings practice;
    practice.loudness = 6;
    CHECK(joined(ac3gui::ac4_cli_tokens(practice, false)) == "dialnorm=31 loudness=not-indicated");
}

TEST_CASE("AC-4 page configuration follows its tokens", "[gui]") {
    Ac4EncodeSettings s;
    s.frame_rate = 2;
    s.rate_mode = 2;
    s.codec_mode = 5;
    s.dialnorm_db = 20.5;
    s.drc = 4;
    s.centre_level = 0;
    s.surround_level = 5;
    s.preferred_downmix = 1;
    s.dialogue_right = true;
    s.dialogue_max_gain = 0;
    s.iframe_interval = 48;
    const ac4::EncoderConfig c = ac3gui::ac4_encoder_config(s, 6, 48000, 256);
    CHECK(c.channels == 6);
    CHECK(c.sample_rate_hz == 48000);
    CHECK(c.bitrate_kbps == 256);
    CHECK(c.frame_rate_index == 2);
    CHECK(c.rate_mode == ac4::RateMode::kVariable);
    CHECK(c.codec_mode == ac4::CodecMode::kAspxAcpl3);
    CHECK(c.iframe_interval == 48);
    CHECK(c.dialnorm_db == -20.5);
    REQUIRE(c.drc.has_value());
    CHECK(c.drc->profile == ac4::DrcProfile::kSpeech);
    CHECK(c.drc->modes.size() == 4);
    REQUIRE(c.downmix.has_value());
    CHECK(c.downmix->loro_centre_db == 3.0);
    CHECK(std::isinf(c.downmix->loro_surround_db));
    CHECK(c.downmix->preferred == ac4::PreferredDownmix::kLtRt);
    CHECK_FALSE(c.downmix->ltrt_centre_db.has_value());
    REQUIRE(c.dialogue.has_value());
    CHECK_FALSE(c.dialogue->left);
    CHECK(c.dialogue->right);
    CHECK_FALSE(c.dialogue->centre);
    CHECK(c.dialogue->max_gain_db == 3);
    CHECK(c.dialogue->method == ac4::DialogueMethod::kChannelIndependent);
    CHECK(ac3gui::ac4_loudness_practice(s) == std::nullopt);
}

TEST_CASE("AC-4 page refuses what ac4-encode refuses before reading audio", "[gui]") {
    Ac4EncodeSettings s;
    CHECK_FALSE(ac3gui::ac4_settings_refusal(s, 2, 48000).has_value());
    s.frame_rate = 4;
    CHECK_FALSE(ac3gui::ac4_settings_refusal(s, 2, 48000).has_value());
    REQUIRE(ac3gui::ac4_settings_refusal(s, 2, 44100).has_value());
    CHECK(ac3gui::ac4_settings_refusal(s, 2, 44100)->find("44.1 kHz") != std::string::npos);
    s.frame_rate = ac3gui::kAc4NativeFrameRate;
    s.preferred_downmix = 0;
    REQUIRE(ac3gui::ac4_settings_refusal(s, 2, 48000).has_value());
    CHECK(ac3gui::ac4_settings_refusal(s, 2, 48000)->find("the source is stereo") !=
          std::string::npos);
    CHECK_FALSE(ac3gui::ac4_settings_refusal(s, 6, 48000).has_value());
}

TEST_CASE("AC-4 presentation labels name the position and the channels", "[gui]") {
    constexpr std::size_t kSamples = 2048 * 6;
    std::vector<float> left(kSamples);
    std::vector<float> right(kSamples);
    for (std::size_t i = 0; i < kSamples; ++i) {
        left[i] = 0.25F * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
        right[i] = left[i];
    }
    const std::vector<std::span<const float>> views{left, right};
    auto encoder = ac4::Encoder::create(ac3gui::ac4_encoder_config({}, 2, 48000, 128));
    REQUIRE(encoder.has_value());
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    const auto packaged = ac3::apps::package_ac4(*frames, encoder->toc(), false, true);
    REQUIRE(packaged.has_value());
    std::vector<std::byte> stream;
    for (const auto& chunk : packaged->chunks) {
        stream.insert(stream.end(), chunk.begin(), chunk.end());
    }
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    const auto rows = ac3gui::ac4_presentation_rows(scan.frames);
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().index == 0);
    CHECK(rows.front().label.rfind("0: L R", 0) == 0);
    CHECK(rows.front().decodable);
}

TEST_CASE("AC-4 object mode echoes the coding, a dialnorm off 31 and a raw stream's CRC", "[gui]") {
    Ac4EncodeSettings s;
    // At every default the objects' command is plain, whatever the channels' settings hold.
    CHECK(ac3gui::ac4_object_cli_tokens(s, false).empty());
    s.frame_rate = 2;
    s.rate_mode = 1;
    s.loudness = 0;
    s.drc = 1;
    s.dialogue_left = true;
    CHECK(ac3gui::ac4_object_cli_tokens(s, false).empty());
    CHECK_FALSE(ac3gui::ac4_cli_tokens(s, false).empty());

    s = {};
    s.object_coding = 1;
    s.dialnorm_db = 27;
    s.crc = false;
    CHECK(joined(ac3gui::ac4_object_cli_tokens(s, false)) == "coding=direct dialnorm=27 crc=off");
    // An MP4 sample has no CRC to turn off, and atmos-encode refuses crc= there.
    CHECK(joined(ac3gui::ac4_object_cli_tokens(s, true)) == "coding=direct dialnorm=27");
    // Out of range or off the grid, the dialnorm is refused, not echoed.
    s.dialnorm_db = 27.25;
    CHECK(joined(ac3gui::ac4_object_cli_tokens(s, false)) == "coding=direct crc=off");
}

TEST_CASE("AC-4 object mode takes a dialnorm in whole dB, 1 to 31, and no measurement", "[gui]") {
    Ac4EncodeSettings s;
    CHECK(ac3gui::ac4_object_dialnorm(s) == 31);
    CHECK_FALSE(ac3gui::ac4_object_settings_refusal(s).has_value());
    s.dialnorm_db = 1;
    CHECK(ac3gui::ac4_object_dialnorm(s) == 1);
    for (const double db : {0.0, 31.25, 27.5, 32.0}) {
        s.dialnorm_db = db;
        CAPTURE(db);
        CHECK_FALSE(ac3gui::ac4_object_dialnorm(s).has_value());
        const auto refusal = ac3gui::ac4_object_settings_refusal(s);
        REQUIRE(refusal.has_value());
        CHECK(refusal->find("whole dB, 1 to 31") != std::string::npos);
    }
    s.dialnorm_db = 31;
    s.measure_dialnorm = true;
    CHECK_FALSE(ac3gui::ac4_object_dialnorm(s).has_value());
    const auto measured = ac3gui::ac4_object_settings_refusal(s);
    REQUIRE(measured.has_value());
    CHECK(measured->find("no bed to measure") != std::string::npos);
}

TEST_CASE("AC-4 object parameters follow the settings", "[gui]") {
    Ac4EncodeSettings s;
    s.object_coding = 1;
    s.dialnorm_db = 24;
    const auto params = ac3gui::ac4_objects_params(s, 44100, 320);
    CHECK(params.sample_rate_hz == 44100);
    CHECK(params.bitrate_kbps == 320);
    CHECK(params.dialnorm_db == 24.0);
    CHECK(params.coding == ac4::ObjectCoding::kDirect);
    s.object_coding = 0;
    CHECK(ac3gui::ac4_objects_params(s, 48000, 192).coding == ac4::ObjectCoding::kAjoc);
    // An index off the table is the first, as every choice here reads one.
    s.object_coding = 9;
    CHECK(ac3gui::ac4_objects_params(s, 48000, 192).coding == ac4::ObjectCoding::kAjoc);
    CHECK(joined(ac3gui::ac4_object_cli_tokens(s, false)) == "dialnorm=24");
}

TEST_CASE("an output path names an MP4 file by its suffix, as remux does", "[gui]") {
    for (const char* name : {"out.mp4", "dir/take.m4a", "clip.mov", "a b.mp4"}) {
        CAPTURE(name);
        CHECK(ac3::apps::ac4_output_names_mp4(name));
    }
    for (const char* name : {"out.ac4", "out.mp4.ac4", "mp4", "out.MP4", "", "-"}) {
        CAPTURE(name);
        CHECK_FALSE(ac3::apps::ac4_output_names_mp4(name));
    }
}
