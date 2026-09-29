// The race of the encoder's presentations against DEE's substreams
// (planning/ac4.md, phase E6). DEE writes one presentation of one substream,
// so its side of the race is phase D7's test multiplexer (ac4dec_mux.hpp) over
// the gold set's legs of the same sources: music and effects, dialogue and
// associated audio, each a DEE encode at its own rate, made into presentations
// of music and effects with dialogue (presentation_config 0), with associated
// audio as well (3), and each substream alone. The encoder's side encodes the
// same sources into the same presentations, each substream at the rate of its
// DEE leg. tools/checks/race_ac4_presentations.py runs this and scores both
// streams' presentations against the sources' mixes.
//
// Hidden ([.race]) and local: it reads phase G0's and G1's gold set, which
// never reaches CI. AC4_GOLD names the gold set's directory and AC4_E6_RACE
// the directory the streams go to.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../ac4dec/ac4dec_mux.hpp"
#include "ac3/io/wav.hpp"
#include "ac4/ac4.hpp"
#include "ac4enc/encoder.hpp"

namespace {

namespace fs = std::filesystem;

// One race: the gold legs of each role's source, and the rates they were
// encoded at.
struct Race {
    const char* name;
    const char* music_leg;
    const char* music_source;
    int music_kbps;
    const char* dialogue_leg;
    const char* dialogue_source;
    int dialogue_kbps;
    const char* associated_leg;
    const char* associated_source;
    int associated_kbps;
};

constexpr Race kRaces[] = {
    {"music128-dialogue64", "20-music-128", "music_20", 128, "20-dialogue-64", "dialogue_20", 64,
     "20-associated-64", "associated_20", 64},
    {"music192-dialogue128", "20-music-192", "music_20", 192, "20-dialogue-128", "dialogue_20", 128,
     "20-associated-128", "associated_20", 128},
    {"tones128-tone64", "20-tones-128", "tones_20", 128, "20-dlgtone-64", "dialogue_tone_20", 64,
     "20-assoctone-64", "associated_tone_20", 64},
};

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::vector<std::byte> sync_framed(std::span<const std::vector<std::byte>> frames) {
    std::vector<std::byte> out;
    for (const std::vector<std::byte>& frame : frames) {
        const std::vector<std::byte> framed = ac4::sync_frame(frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

// The presentations both sides carry: 1 music and effects with dialogue, 2
// with associated audio as well, and 10 to 12 each substream alone.
constexpr int kWithDialogue = 1;
constexpr int kWithAssociated = 2;

}  // namespace

TEST_CASE("the encoder's presentations and DEE's substreams, made for the race", "[.race]") {
    const char* gold_dir = std::getenv("AC4_GOLD");
    const char* out_dir = std::getenv("AC4_E6_RACE");
    REQUIRE(gold_dir != nullptr);
    REQUIRE(out_dir != nullptr);
    const fs::path gold{gold_dir};
    const fs::path out{out_dir};
    for (const Race& race : kRaces) {
        CAPTURE(race.name);
        // DEE's side: each leg's audio substream, multiplexed.
        std::vector<ac4dec_test::MuxSource> sources;
        std::size_t frames = SIZE_MAX;
        for (const char* leg : {race.music_leg, race.dialogue_leg, race.associated_leg}) {
            const std::vector<std::byte> file = read_file(gold / "streams" / leg / "dee.ac4");
            REQUIRE_FALSE(file.empty());
            sources.push_back(ac4dec_test::mux_source(file));
            frames = std::min(frames, sources.back().frames.size());
        }
        ac4dec_test::MuxLayout layout;
        layout.groups.push_back(ac4dec_test::MuxGroup{.source = 0,
                                                      .content_classifier = 1,
                                                      .language = {},
                                                      .dialogue = std::nullopt,
                                                      .de = std::nullopt});
        layout.groups.push_back(ac4dec_test::MuxGroup{.source = 1,
                                                      .content_classifier = 4,
                                                      .language = "en",
                                                      .dialogue = ac4::detail::DialogueMixCodes{},
                                                      .de = std::nullopt});
        layout.groups.push_back(ac4dec_test::MuxGroup{.source = 2,
                                                      .content_classifier = 2,
                                                      .language = "qad",
                                                      .dialogue = std::nullopt,
                                                      .de = std::nullopt});
        const auto mux_presentation = [](std::optional<int> config, std::vector<int> groups, int id,
                                         int md_compat) {
            ac4dec_test::MuxPresentation p;
            p.presentation_config = config;
            p.groups = std::move(groups);
            p.presentation_id = id;
            p.md_compat = md_compat;
            p.source = 0;
            return p;
        };
        layout.presentations = {mux_presentation(0, {0, 1}, kWithDialogue, 1),
                                mux_presentation(3, {0, 1, 2}, kWithAssociated, 1),
                                mux_presentation(std::nullopt, {0}, 10, 0),
                                mux_presentation(std::nullopt, {1}, 11, 0),
                                mux_presentation(std::nullopt, {2}, 12, 0)};
        const std::vector<std::vector<std::byte>> dee = ac4dec_test::multiplex(sources, layout, frames);
        write_file(out / (std::string{race.name} + ".dee.ac4"), sync_framed(dee));

        // The encoder's side: the same sources and presentations, each
        // substream at its DEE leg's rate, and the stream's rate that and
        // what its table of contents and presentation substreams take.
        ac4::EncoderConfig config;
        const int substreams_kbps = race.music_kbps + race.dialogue_kbps + race.associated_kbps;
        config.bitrate_kbps = substreams_kbps + 12;
        ac4::SubstreamConfig music;
        music.channels = 2;
        music.bitrate_kbps = race.music_kbps;
        music.content = ac4::ContentClassifier::kMusicAndEffects;
        ac4::SubstreamConfig dialogue;
        dialogue.channels = 2;
        dialogue.bitrate_kbps = race.dialogue_kbps;
        dialogue.content = ac4::ContentClassifier::kDialogue;
        dialogue.language = "en";
        ac4::SubstreamConfig associated;
        associated.channels = 2;
        associated.bitrate_kbps = race.associated_kbps;
        associated.content = ac4::ContentClassifier::kVisuallyImpaired;
        associated.language = "qad";
        config.substreams = {music, dialogue, associated};
        const auto presentation = [](std::optional<int> kind, std::vector<int> substreams, int id) {
            ac4::PresentationConfig p;
            p.config = kind;
            p.substreams = std::move(substreams);
            p.presentation_id = id;
            return p;
        };
        config.presentations = {presentation(0, {0, 1}, kWithDialogue), presentation(3, {0, 1, 2}, kWithAssociated),
                                presentation(std::nullopt, {0}, 10), presentation(std::nullopt, {1}, 11),
                                presentation(std::nullopt, {2}, 12)};
        std::vector<std::vector<float>> input;
        for (const char* source : {race.music_source, race.dialogue_source, race.associated_source}) {
            const auto wav = ac3::io::read_wav((gold / "sources" / (std::string{source} + ".wav")).string());
            REQUIRE(wav.has_value());
            REQUIRE(wav->sample_rate == 48000);
            REQUIRE(wav->channels.size() == 2);
            input.insert(input.end(), wav->channels.begin(), wav->channels.end());
        }
        auto encoder = ac4::Encoder::create(config);
        REQUIRE(encoder.has_value());
        const std::vector<std::span<const float>> views(input.begin(), input.end());
        std::vector<std::vector<std::byte>> encoded;
        auto written = encoder->encode(views);
        REQUIRE(written.has_value());
        for (ac4::EncodedFrame& frame : *written) {
            encoded.push_back(std::move(frame.raw_ac4_frame));
        }
        auto rest = encoder->flush();
        REQUIRE(rest.has_value());
        for (ac4::EncodedFrame& frame : *rest) {
            encoded.push_back(std::move(frame.raw_ac4_frame));
        }
        write_file(out / (std::string{race.name} + ".encoder.ac4"), sync_framed(encoded));
        std::ofstream(out / (std::string{race.name} + ".json"), std::ios::binary)
            << "{\"sources\": [\"" << race.music_source << "\", \"" << race.dialogue_source << "\", \""
            << race.associated_source << "\"], \"rates\": [" << race.music_kbps << ", " << race.dialogue_kbps << ", "
            << race.associated_kbps << "], \"presentations\": {\"1\": [0, 1], \"2\": [0, 1, 2], \"10\": [0], "
            << "\"11\": [1], \"12\": [2]}}\n";
    }
}
