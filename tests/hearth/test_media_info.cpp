#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"
#include "container_input.hpp"
#include "matroska/matroska.hpp"
#include "media_info.hpp"
#include "media_inspector.hpp"
#include "mp4/mp4.hpp"
#include "mpegts/mpegts.hpp"

// ac3::hearth's media information (apps/hearth/engine/media_info.cpp and
// media_inspector.cpp): what a queue item's file says about itself, read on a
// thread of its own and written out as ac3forge.hearth.media/1.

namespace {

using ac3::hearth::LoadedItem;
using ac3::hearth::MediaCodec;
using ac3::hearth::MediaInfo;
using ac3::hearth::MediaInspector;
namespace json = ac3::sendspin::json;

std::vector<float> tone(std::size_t offset, double level = 0.3) {
    std::vector<float> out(ac3::kSamplesPerFrame);
    for (std::size_t n = 0; n < out.size(); ++n) {
        out[n] = static_cast<float>(level * std::sin(2.0 * std::numbers::pi * 440.0 *
                                                     static_cast<double>(n + offset) / 48000.0));
    }
    return out;
}

std::vector<std::byte> ac3_stream(int frames, const ac3::EncoderConfig& config) {
    ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(channels, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

std::vector<std::vector<std::byte>> eac3_frames(int frames, const ac3::eac3::FrameConfig& config) {
    ac3::eac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(channels, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::byte> joined(const std::vector<std::vector<std::byte>>& frames) {
    std::vector<std::byte> out;
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

ac3::eac3::FrameConfig surround_config() {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 384;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    return config;
}

// A file read the way the application's loader reads one.
LoadedItem load(std::span<const std::byte> file) {
    auto stream = ac3::apps::elementary_stream_from_bytes(file);
    REQUIRE(stream.error.empty());
    return LoadedItem{.bytes = std::move(stream.bytes),
                      .skip_samples = stream.trim.start,
                      .play_samples = stream.trim.length,
                      .note = std::move(stream.trim_note),
                      .container = std::move(stream.container)};
}

std::vector<std::byte> in_mp4(const std::vector<std::byte>& stream,
                              std::optional<mp4::MuxOptions::Edit> edit = std::nullopt) {
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    mp4::AudioTrack track;
    track.codec_id = std::string{mp4::kCodecEac3};
    track.sample_rate = ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    track.codec_config = ac3::io::build_codec_config_box(*scanned);
    mp4::MuxOptions options;
    options.edit = edit;
    const auto muxed = mp4::mux(
        track, std::span<const std::span<const std::byte>>(scanned->access_units), options);
    REQUIRE(muxed.has_value());
    return *muxed;
}

// A parsed JSON document, with the storage its values point into.
struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document doc;

    explicit Parsed(std::string source) : text(std::move(source)) {
        const auto result = doc.parse(text, tokens, 1U << 20);
        INFO(text);
        REQUIRE(result);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value root() const { return doc.root(); }
};

}  // namespace

TEST_CASE("media info: an AC-3 stream's bitstream information and probe", "[hearth][media-info]") {
    ac3::EncoderConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    config.dialnorm = 24;
    config.info.bsmod = ac3::meta::BitstreamMode::kMusicAndEffects;
    config.info.dsurmod = ac3::meta::SurroundMode::kDolbySurround;
    config.info.copyrightb = true;
    config.info.origbs = false;
    config.info.audprod = ac3::meta::AudioProduction{.mixlevel = 25,
                                                     .roomtyp = ac3::meta::RoomType::kSmallRoomFlat};
    const auto stream = ac3_stream(10, config);

    const MediaInfo info = describe_media("music.ac3", load(stream));
    CHECK(info.error.empty());
    REQUIRE(info.codec == MediaCodec::kAc3);
    CHECK(info.path == "music.ac3");
    CHECK(info.container.kind == ac3::apps::ContainerKind::kUnknown);
    CHECK(info.sample_rate == 48000);
    CHECK(info.stream_samples == 10 * 1536);
    CHECK(info.played_samples() == 10 * 1536);
    REQUIRE(info.programmes.size() == 1);
    CHECK(info.programmes[0].acmod == ac3::Acmod::k2_0);
    CHECK(info.programmes[0].access_units == 10);

    REQUIRE(info.probe.has_value());
    CHECK(info.probe->access_units == 10);
    CHECK(info.probe->nominal_bitrate_kbps == 192);
    CHECK(info.probe->dialnorm.constant());
    CHECK(info.probe->dialnorm.min == 24);

    REQUIRE(info.bitstream.has_value());
    REQUIRE(info.bitstream->info.has_value());
    const auto& bsi = *info.bitstream->info;
    CHECK(bsi.bsmod == ac3::meta::BitstreamMode::kMusicAndEffects);
    CHECK(bsi.dsurmod == ac3::meta::SurroundMode::kDolbySurround);
    CHECK(bsi.copyrightb);
    CHECK_FALSE(bsi.origbs);
    REQUIRE(bsi.audprod.has_value());
    CHECK(bsi.audprod->mixlevel == 25);
    CHECK(bsi.audprod->roomtyp == ac3::meta::RoomType::kSmallRoomFlat);
    // A 2/0 stream codes no mix levels, so the §7.8 defaults stand.
    CHECK_FALSE(info.bitstream->cmixlev.has_value());
    CHECK(info.bitstream->levels.loro_clev == ac3::meta::level::kMinus4_5dB);

    const Parsed parsed{media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["schema"].equals("ac3forge.hearth.media/1"));
    CHECK(root["file"].equals("music.ac3"));
    CHECK(root["codec"].equals("ac3"));
    CHECK(root["error"].is_null());
    CHECK(root["container"].is_null());
    CHECK(root["playback"]["played_samples"].as_int() == 10 * 1536);
    CHECK(root["playback"]["duration_seconds"].as_double() == Catch::Approx(0.32));
    CHECK(root["playback"]["play_samples"].is_null());
    CHECK(root["programmes"].size() == 1);
    CHECK(root["programmes"].at(0)["layout_label"].equals("2/0 stereo"));
    const auto bits = root["bitstream"];
    CHECK(bits["info"]["bsmod_label"].equals("music and effects"));
    CHECK(bits["info"]["dsurmod"].as_int() == 2);
    CHECK(bits["info"]["copyright"].as_bool() == true);
    CHECK(bits["info"]["original"].as_bool() == false);
    CHECK(bits["info"]["audio_production"]["mix_level_db_spl"].as_int() == 105);
    CHECK(bits["info"]["audio_production2"].is_null());
    CHECK(bits["cmixlev"].is_null());
    // The levels are exact quarter-powers of two, so -4.5 dB is -4.52 to two
    // places.
    CHECK(bits["fold_levels"]["loro_centre_db"].as_double() == Catch::Approx(-4.5).margin(0.05));
    CHECK(bits["fold_levels"]["lfe_db"].as_double() == Catch::Approx(10.0));
    // The probe object is the one ac3cli probe json=1 writes.
    const auto probe = root["probe"];
    CHECK(probe["schema"].equals("ac3forge.probe/1"));
    CHECK(probe["stream"]["codec"].equals("ac3"));
    CHECK(probe["stream"]["nominal_bitrate_kbps"].as_int() == 192);
    CHECK(probe["stream"]["metadata"]["dialnorm_db"]["min"].as_int() == -24);
    CHECK(probe["stream"]["bsmod_label"].equals("music and effects"));
}

TEST_CASE("media info: bsmod 7 is voice over at 1/0 and karaoke above it", "[hearth][media-info]") {
    // A/52 Table 5.7's one service that acmod names.
    ac3::EncoderConfig config;
    config.bitrate_kbps = 96;
    config.acmod = ac3::Acmod::k1_0;
    config.info.bsmod = ac3::meta::BitstreamMode::kVoiceOverOrKaraoke;
    const auto check = [](const MediaInfo& info, const char* label) {
        const Parsed parsed{media_info_json(info)};
        const auto root = parsed.root();
        CHECK(root["programmes"].at(0)["bsmod"].as_int() == 7);
        CHECK(root["programmes"].at(0)["bsmod_label"].equals(label));
        CHECK(root["bitstream"]["info"]["bsmod_label"].equals(label));
        CHECK(root["probe"]["stream"]["bsmod_label"].equals(label));
    };
    check(describe_media("voice.ac3", load(ac3_stream(2, config))), "voice over");
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    check(describe_media("karaoke.ac3", load(ac3_stream(2, config))), "karaoke");
}

TEST_CASE("media info: an AC-3 stream's coded mix levels, and Annex D's", "[hearth][media-info]") {
    ac3::EncoderConfig config;
    config.bitrate_kbps = 448;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    config.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
    config.surmixlev = ac3::meta::SurroundMixLevel::kSilent;
    SECTION("bsi's two") {
        const MediaInfo info = describe_media("bsi.ac3", load(ac3_stream(4, config)));
        REQUIRE(info.bitstream.has_value());
        CHECK(info.bitstream->cmixlev == ac3::meta::CentreMixLevel::kMinus3dB);
        CHECK(info.bitstream->surmixlev == ac3::meta::SurroundMixLevel::kSilent);
        CHECK_FALSE(info.bitstream->alternate_bsi.has_value());
        const Parsed parsed{media_info_json(info)};
        const auto bits = parsed.root()["bitstream"];
        CHECK(bits["cmixlev"]["code"].as_int() == 0);
        CHECK(bits["cmixlev"]["db"].as_double() == Catch::Approx(-3.0).margin(0.05));
        // Silence has no level in dB.
        CHECK(bits["surmixlev"]["db"].is_null());
        CHECK(bits["fold_levels"]["loro_surround_db"].is_null());
    }
    SECTION("xbsi1's, which a bsid 6 stream sends as well") {
        ac3::meta::MixMetadata mix;
        mix.dmixmod = ac3::meta::DownmixMode::kLtRt;
        mix.ltrtcmixlev = ac3::meta::MixLevel::kMinus1_5dB;
        mix.lorocmixlev = ac3::meta::MixLevel::kMinus6dB;
        config.alternate_bsi = ac3::meta::AlternateBsi{.mix = mix, .extended = std::nullopt};
        const MediaInfo info = describe_media("xbsi.ac3", load(ac3_stream(4, config)));
        REQUIRE(info.bitstream.has_value());
        REQUIRE(info.bitstream->alternate_bsi.has_value());
        CHECK(info.bitstream->levels.preferred == ac3::meta::DownmixMode::kLtRt);
        CHECK(info.bitstream->levels.loro_clev == ac3::meta::level::kMinus6dB);
        const Parsed parsed{media_info_json(info)};
        const auto bits = parsed.root()["bitstream"];
        CHECK(bits["alternate_bsi"]["xbsi1"]["dmixmod"]["label"].equals("Lt/Rt"));
        CHECK(bits["alternate_bsi"]["xbsi1"]["ltrtcmixlev"]["db"].as_double() ==
              Catch::Approx(-1.5).margin(0.05));
        CHECK(bits["alternate_bsi"]["xbsi2"].is_null());
        CHECK(bits["fold_levels"]["preferred"]["code"].as_int() == 1);
        CHECK(parsed.root()["probe"]["stream"]["bsid"].as_int() == 6);
    }
}

TEST_CASE("media info: an E-AC-3 stream in MP4, with its container and edit list",
          "[hearth][media-info]") {
    auto config = surround_config();
    ac3::meta::MixMetadata mix;
    mix.dmixmod = ac3::meta::DownmixMode::kLoRo;
    mix.lfemixlevcod = 5;
    mix.pgmscl = 51;
    config.mixing = mix;
    const auto stream = joined(eac3_frames(20, config));
    const std::uint64_t total = 20 * 1536;
    const auto file = in_mp4(stream, mp4::MuxOptions::Edit{.start_samples = 256,
                                                           .duration_samples = total - 256 - 500});

    const MediaInfo info = describe_media("movie.mp4", load(file));
    CHECK(info.error.empty());
    REQUIRE(info.codec == MediaCodec::kEac3);
    CHECK(info.stream_samples == total);
    CHECK(info.skip_samples == 256);
    CHECK(info.play_samples == total - 256 - 500);
    CHECK(info.played_samples() == total - 256 - 500);

    const auto& container = info.container;
    CHECK(container.kind == ac3::apps::ContainerKind::kMp4);
    CHECK(container.codec_id == "ec-3");
    CHECK(container.track == 1);
    CHECK(container.language == "und");
    CHECK(container.samples == 20);
    CHECK(container.sample_rate == 48000);
    CHECK(container.timescale == 48000);
    CHECK(container.edits >= 1);
    REQUIRE(container.codec_box.has_value());
    CHECK(container.codec_box->type == "dec3");
    CHECK(container.codec_box->independent_substreams == 1);
    CHECK(container.codec_box->acmod == 7);
    CHECK(container.codec_box->lfeon);
    CHECK(container.codec_box->bsid == 16);

    REQUIRE(info.bitstream.has_value());
    REQUIRE(info.bitstream->mixing.has_value());
    CHECK(info.bitstream->mixing->dmixmod == ac3::meta::DownmixMode::kLoRo);
    CHECK(info.bitstream->levels.lfe_mix_level_db == 5.0);

    const Parsed parsed{media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["codec"].equals("eac3"));
    const auto facts = root["container"];
    CHECK(facts["format"].equals("mp4"));
    CHECK(facts["codec_id"].equals("ec-3"));
    CHECK(facts["track"].as_int() == 1);
    CHECK(facts["mpegts"].is_null());
    CHECK(facts["mp4"]["timescale"].as_int() == 48000);
    CHECK(facts["mp4"]["codec_box"]["type"].equals("dec3"));
    CHECK(facts["mp4"]["codec_box"]["acmod"].as_int() == 7);
    CHECK(root["playback"]["skip_samples"].as_int() == 256);
    CHECK(root["playback"]["play_samples"].as_int() == static_cast<std::int64_t>(total - 756));
    const auto mixing = root["bitstream"]["mixing"];
    CHECK(mixing["dmixmod"]["label"].equals("Lo/Ro"));
    CHECK(mixing["lfemixlevcod"]["db"].as_double() == Catch::Approx(5.0));
    CHECK(mixing["pgmscl"]["db"].as_double() == Catch::Approx(0.0));
    CHECK(mixing["pgmscl2"].is_null());
    CHECK(root["bitstream"]["info"].is_null());
    CHECK(root["probe"]["stream"]["codec"].equals("eac3"));
    CHECK(root["probe"]["stream"]["nominal_bitrate_kbps"].is_null());
    CHECK(root["channel_map"].as_int() ==
          static_cast<std::int64_t>(ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, true)));
}

TEST_CASE("media info: Matroska and MPEG-TS name their tracks", "[hearth][media-info]") {
    const auto frames = eac3_frames(6, surround_config());
    std::vector<std::span<const std::byte>> views;
    for (const auto& frame : frames) {
        views.emplace_back(frame);
    }

    SECTION("Matroska") {
        matroska::AudioTrack track;
        track.codec_id = std::string{matroska::kCodecEac3};
        track.sample_rate = 48000;
        track.channels = 6;
        const auto file = matroska::mux(track, std::span<const std::span<const std::byte>>(views));
        REQUIRE(file.has_value());
        const MediaInfo info = describe_media("show.mkv", load(*file));
        CHECK(info.error.empty());
        CHECK(info.container.kind == ac3::apps::ContainerKind::kMatroska);
        CHECK(info.container.codec_id == "A_EAC3");
        CHECK(info.container.track == 1);
        CHECK(info.container.samples == 6);
        CHECK(info.container.channels == 6);
        const Parsed parsed{media_info_json(info)};
        CHECK(parsed.root()["container"]["format"].equals("matroska"));
        CHECK(parsed.root()["container"]["mp4"].is_null());
    }
    SECTION("MPEG-TS") {
        mpegts::AudioTrack track;
        track.channels = 6;
        mpegts::MuxOptions options;
        options.program_number = 7;
        options.audio_pid = 0x0123;
        const auto file =
            mpegts::mux(track, std::span<const std::span<const std::byte>>(views), options);
        REQUIRE(file.has_value());
        const MediaInfo info = describe_media("broadcast.ts", load(*file));
        CHECK(info.error.empty());
        CHECK(info.container.kind == ac3::apps::ContainerKind::kMpegTs);
        CHECK(info.container.track == 0x0123);
        CHECK(info.container.program_number == 7);
        CHECK(info.container.signalling == "dvb_descriptor");
        CHECK(info.container.language.empty());
        const Parsed parsed{media_info_json(info)};
        const auto facts = parsed.root()["container"];
        CHECK(facts["format"].equals("mpegts"));
        CHECK(facts["codec_id"].is_null());
        CHECK(facts["language"].is_null());
        CHECK(facts["sample_rate_hz"].is_null());
        CHECK(facts["mpegts"]["program_number"].as_int() == 7);
        CHECK(facts["mpegts"]["packet_size"].as_int() == 188);
    }
}

TEST_CASE("media info: a stream with two programmes lists both", "[hearth][media-info]") {
    auto first_config = surround_config();
    auto second_config = surround_config();
    second_config.acmod = ac3::Acmod::k2_0;
    second_config.lfe = false;
    second_config.bitrate_kbps = 192;
    second_config.substreamid = 1;
    ac3::meta::BsiInfo described;
    described.bsmod = ac3::meta::BitstreamMode::kCommentary;
    second_config.info = described;
    const auto first = eac3_frames(4, first_config);
    const auto second = eac3_frames(4, second_config);
    std::vector<std::byte> stream;
    for (std::size_t f = 0; f < first.size(); ++f) {
        stream.insert(stream.end(), first[f].begin(), first[f].end());
        stream.insert(stream.end(), second[f].begin(), second[f].end());
    }

    const MediaInfo info = describe_media("two.ec3", load(stream));
    CHECK(info.error.empty());
    REQUIRE(info.programmes.size() == 2);
    CHECK(info.programmes[0].substreamid == 0);
    CHECK(info.programmes[0].channels == 6);
    CHECK(info.programmes[1].substreamid == 1);
    CHECK(info.programmes[1].acmod == ac3::Acmod::k2_0);
    CHECK(info.programmes[1].bsmod == 5);
    CHECK(info.associated_services[0].present);
    CHECK_FALSE(info.associated_services[1].present);
    // The probe and the length are the lead programme's.
    REQUIRE(info.probe.has_value());
    CHECK(info.probe->access_units == 4);
    CHECK(info.stream_samples == 4 * 1536);

    const Parsed parsed{media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["programmes"].size() == 2);
    CHECK(root["programmes"].at(1)["bsmod_label"].equals("commentary"));
    REQUIRE(root["associated_services"].size() == 1);
    CHECK(root["associated_services"].at(0)["substream_id"].as_int() == 1);
    CHECK(root["associated_services"].at(0)["bsmod_label"].equals("commentary"));
}

TEST_CASE("media info: objects, and whether they are signed", "[hearth][media-info]") {
    ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .num_bands_idx = 4, .emit_object_metadata = true}, 1};
    const std::array<ac3::oba::ObjectPlacement, 1> placement{{{}}};
    std::vector<std::span<const float>> views(1);
    std::vector<std::byte> stream;
    for (int f = 0; f < 4; ++f) {
        const auto essence = tone(static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        views[0] = essence;
        auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }

    const MediaInfo unsigned_info = describe_media("objects.ec3", load(stream));
    REQUIRE(unsigned_info.probe.has_value());
    CHECK(unsigned_info.probe->oamd);
    CHECK(unsigned_info.probe->joc);
    REQUIRE(unsigned_info.probe->program.has_value());
    CHECK(unsigned_info.probe->authenticity_tagged_frames == 0);

    const ac3::signing::SigningKey key{std::vector<std::byte>(32, std::byte{0x5A})};
    REQUIRE(ac3::signing::sign_atmos_stream(stream, key) == 4);
    const MediaInfo signed_info = describe_media("signed.ec3", load(stream));
    REQUIRE(signed_info.probe.has_value());
    CHECK(signed_info.probe->authenticity_tagged_frames == 4);
    const Parsed parsed{media_info_json(signed_info)};
    const auto stream_json = parsed.root()["probe"]["stream"];
    CHECK(stream_json["authenticity"]["present"].as_bool() == true);
    CHECK(stream_json["objects"]["joc"].as_bool() == true);
    CHECK(stream_json["objects"]["dynamic"].as_int() == 1);
}

TEST_CASE("media info: an AC-4 stream's table of contents", "[hearth][media-info]") {
    const std::string path = AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR "/ac4-stereo-64/dee.ac4";
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    const std::vector<char> raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    REQUIRE_FALSE(bytes.empty());
    const MediaInfo info = describe_media("dee.ac4", load(bytes));
    CHECK(info.error.empty());
    REQUIRE(info.codec == MediaCodec::kAc4);
    REQUIRE(info.ac4.has_value());
    CHECK(info.ac4->sync_frames > 0);
    REQUIRE(info.ac4->first_frame.has_value());
    CHECK(info.sample_rate == 48000);
    CHECK_FALSE(info.probe.has_value());
    CHECK_FALSE(info.bitstream.has_value());

    const Parsed parsed{media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["codec"].equals("ac4"));
    CHECK(root["bitstream"].is_null());
    CHECK(root["programmes"].size() == 0);
    CHECK(root["probe"]["stream"]["codec"].equals("ac4"));
    CHECK(root["probe"]["stream"]["ac4"]["bitstream_version"].as_int() == 2);
    CHECK(root["probe"]["stream"]["ac4"]["n_presentations"].as_int() == 1);
}

TEST_CASE("media info: a legacy AC-4 presentation's substreams are valid JSON", "[hearth][media-info]") {
    // Built by hand: no stream on hand has a bitstream_version 0 or 1 table of
    // contents, whose presentations list their substreams with a role each.
    ac4::ChannelSubstreamInfo substream;
    substream.channel_mode = 1;
    substream.channel_mode_name = "Stereo";
    substream.bitrate_kbps = 96;
    ac4::PresentationInfoV0 presentation;
    presentation.presentation_version = 0;
    presentation.substreams.emplace_back("main", substream);
    ac4::RawFrame frame;
    frame.toc.bitstream_version = 1;
    frame.toc.n_presentations = 1;
    frame.toc.presentations_v0.push_back(presentation);

    MediaInfo info;
    info.path = "legacy.ac4";
    info.codec = MediaCodec::kAc4;
    info.ac4 = ac3::apps::probe_json::Ac4Summary{
        .sync_frames = 1, .bytes = 100, .crc_failures = 0, .parse_error = std::nullopt,
        .first_frame = frame};
    const Parsed parsed{media_info_json(info)};
    const auto entry =
        parsed.root()["probe"]["stream"]["ac4"]["presentations_v0"].at(0)["substreams"].at(0);
    CHECK(entry["role"].equals("main"));
    CHECK(entry["channel_mode_name"].equals("Stereo"));
    CHECK(entry["bitrate_kbps"].as_int() == 96);
}

TEST_CASE("media info: the samples an item plays", "[hearth][media-info]") {
    MediaInfo info;
    info.stream_samples = 1000;
    info.skip_samples = 100;
    CHECK(info.played_samples() == 900);
    info.play_samples = 2000;
    CHECK(info.played_samples() == 900);
    info.play_samples = 50;
    CHECK(info.played_samples() == 50);
    info.skip_samples = 2000;
    info.play_samples.reset();
    CHECK(info.played_samples() == 0);
}

TEST_CASE("media info: an item that cannot be read says why", "[hearth][media-info]") {
    std::vector<std::byte> junk(4096);
    for (std::size_t i = 0; i < junk.size(); ++i) {
        junk[i] = static_cast<std::byte>((i * 7) & 0x3F);
    }
    const MediaInfo info = describe_media("junk.ac3", LoadedItem{.bytes = junk});
    CHECK_FALSE(info.codec.has_value());
    CHECK_FALSE(info.error.empty());
    CHECK_FALSE(info.probe.has_value());
    const Parsed parsed{media_info_json(info)};
    CHECK(parsed.root()["codec"].is_null());
    CHECK(parsed.root()["error"].is_string());
    CHECK(parsed.root()["probe"].is_null());
    CHECK(parsed.root()["bitstream"].is_null());
}

namespace {

// Items in memory, with a count of loads per path. When gated, each load
// waits for a permit the test hands out, and `waiting` names the path held.
struct Shelf {
    std::map<std::string, std::vector<std::byte>> files;
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, int> loads;
    bool gated = false;
    int permits = 0;
    std::string waiting;

    [[nodiscard]] ac3::hearth::ItemLoader loader() {
        return [this](const std::string& path) -> std::expected<LoadedItem, std::string> {
            std::unique_lock lock(mutex);
            ++loads[path];
            if (gated) {
                waiting = path;
                changed.notify_all();
                changed.wait(lock, [this] { return permits > 0; });
                --permits;
                waiting.clear();
            }
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("There is no file at " + path + ".");
            }
            return LoadedItem{.bytes = found->second};
        };
    }

    // Waits until a load of `path` is held at the gate.
    [[nodiscard]] bool held(const std::string& path) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10),
                                [this, &path] { return waiting == path; });
    }

    void permit() {
        {
            const std::scoped_lock lock(mutex);
            ++permits;
        }
        changed.notify_all();
    }

    [[nodiscard]] int loads_of(const std::string& path) {
        const std::scoped_lock lock(mutex);
        return loads[path];
    }
};

// Lets every load through when it goes, however the test ends, so that an
// inspector declared before it can stop.
class OpenGate {
public:
    explicit OpenGate(Shelf& shelf) : shelf_(shelf) {}
    OpenGate(const OpenGate&) = delete;
    OpenGate& operator=(const OpenGate&) = delete;
    OpenGate(OpenGate&&) = delete;
    OpenGate& operator=(OpenGate&&) = delete;
    ~OpenGate() {
        {
            const std::scoped_lock lock(shelf_.mutex);
            shelf_.permits = 1'000'000;
        }
        shelf_.changed.notify_all();
    }

private:
    Shelf& shelf_;
};

}  // namespace

TEST_CASE("media inspector: describes what it is asked for, on its own thread",
          "[hearth][media-inspector][concurrency]") {
    ac3::EncoderConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    Shelf shelf;
    shelf.files["a.ac3"] = ac3_stream(4, config);
    // Declared before the inspector, which calls back into them until it
    // stops.
    std::mutex seen_mutex;
    std::vector<std::string> seen;
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> other_thread{true};
    MediaInspector inspector{shelf.loader()};
    inspector.on_ready([&](const MediaInfo& info) {
        if (std::this_thread::get_id() == caller) {
            other_thread = false;
        }
        const std::scoped_lock lock(seen_mutex);
        seen.push_back(info.path);
    });

    CHECK_FALSE(inspector.latest().has_value());
    inspector.request("a.ac3");
    inspector.sync();
    const auto latest = inspector.latest();
    REQUIRE(latest.has_value());
    CHECK(latest->path == "a.ac3");
    CHECK(latest->codec == MediaCodec::kAc3);
    CHECK(other_thread);
    {
        const std::scoped_lock lock(seen_mutex);
        CHECK(seen == std::vector<std::string>{"a.ac3"});
    }

    // A file that is not there is described as not there.
    inspector.request("missing.ac3");
    inspector.sync();
    REQUIRE(inspector.latest().has_value());
    CHECK(inspector.latest()->path == "missing.ac3");
    CHECK(inspector.latest()->error == "There is no file at missing.ac3.");
    CHECK_FALSE(inspector.latest()->codec.has_value());

    // Asked again, a description comes from the cache, unless a reread is
    // asked for.
    inspector.request("a.ac3");
    inspector.sync();
    CHECK(shelf.loads_of("a.ac3") == 1);
    CHECK(inspector.latest()->path == "a.ac3");
    inspector.request("a.ac3", true);
    inspector.sync();
    CHECK(shelf.loads_of("a.ac3") == 2);
}

TEST_CASE("media inspector: a newer request replaces one not yet started",
          "[hearth][media-inspector][concurrency]") {
    ac3::EncoderConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    Shelf shelf;
    for (const char* name : {"a.ac3", "b.ac3", "c.ac3"}) {
        shelf.files[name] = ac3_stream(2, config);
    }
    shelf.gated = true;
    // Declared before the inspector, which calls back into them until it
    // stops.
    std::mutex seen_mutex;
    std::vector<std::string> seen;
    MediaInspector inspector{shelf.loader()};
    const OpenGate open_gate{shelf};
    inspector.on_ready([&](const MediaInfo& info) {
        const std::scoped_lock lock(seen_mutex);
        seen.push_back(info.path);
    });

    // The inspector is inside a.ac3's load while b and c are asked for, and
    // c replaces b.
    inspector.request("a.ac3");
    REQUIRE(shelf.held("a.ac3"));
    inspector.request("b.ac3");
    inspector.request("c.ac3");
    CHECK_FALSE(inspector.latest().has_value());

    // a.ac3's description is handed out, but it is not the newest asked for,
    // so it is not the latest.
    shelf.permit();
    REQUIRE(shelf.held("c.ac3"));
    {
        const std::scoped_lock lock(seen_mutex);
        CHECK(seen == std::vector<std::string>{"a.ac3"});
    }
    CHECK_FALSE(inspector.latest().has_value());

    shelf.permit();
    inspector.sync();
    REQUIRE(inspector.latest().has_value());
    CHECK(inspector.latest()->path == "c.ac3");
    CHECK(shelf.loads_of("b.ac3") == 0);

    // Asking for another path puts the latest away until that path's
    // description is ready.
    inspector.request("b.ac3");
    REQUIRE(shelf.held("b.ac3"));
    CHECK_FALSE(inspector.latest().has_value());
    shelf.permit();
    inspector.sync();
    REQUIRE(inspector.latest().has_value());
    CHECK(inspector.latest()->path == "b.ac3");

    const std::scoped_lock lock(seen_mutex);
    CHECK(seen == std::vector<std::string>{"a.ac3", "c.ac3", "b.ac3"});
}

TEST_CASE("media inspector: goes away cleanly with work waiting",
          "[hearth][media-inspector][concurrency]") {
    Shelf shelf;
    std::atomic<int> ready{0};
    {
        MediaInspector inspector{shelf.loader(), 2};
        inspector.on_ready([&ready](const MediaInfo&) { ++ready; });
        for (int i = 0; i < 20; ++i) {
            inspector.request("item" + std::to_string(i));
        }
    }
    // Stopped and joined, having answered at most what was asked.
    CHECK(ready.load() <= 20);
}
