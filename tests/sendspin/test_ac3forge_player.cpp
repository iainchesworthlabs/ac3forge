#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/frames.hpp"
#include "ac3/sendspin/json.hpp"

// _ac3forge_player@v1's objects (planning/hearth-sendspin-extension.md, The role
// _ac3forge_player@v1): each writer's text byte for byte against the page's tables, each reader
// against what it must refuse and what it must let through, and the settings a sink refuses with
// the revision it names.

namespace {

namespace ac = ac3::sendspin::ac3forge;
namespace json = ac3::sendspin::json;

// A parsed object whose values stay valid while the Parsed lives.
struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;

    explicit Parsed(std::string source) : text(std::move(source)) {
        const bool parsed = static_cast<bool>(document.parse(text, tokens, 4096));
        CHECK(parsed);
    }
    // The document points into `text` and `tokens`.
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value root() const { return document.root(); }
};

template <class T, class Write>
[[nodiscard]] std::string written(const T& value, Write write) {
    std::string out;
    json::Writer w(out);
    write(w, value);
    return out;
}

[[nodiscard]] ac::Support board_support() {
    ac::Support support;
    support.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};
    support.sample_rates = {48000};
    support.outputs.count = 6;
    support.outputs.bit_depth = 32;
    support.outputs.bit_depths = {16, 24, 32};
    support.layout_grammar = 1;
    support.management.routing = true;
    support.management.trim_db = {-12.0, 12.0};
    support.management.max_delay_ms = 50.0;
    support.management.crossover_hz = {40.0, 250.0};
    support.management.identify = true;
    support.decoder_settings.assign(ac::kDecoderSettingNames.begin(), ac::kDecoderSettingNames.end());
    support.buffer_capacity = 1048576;
    return support;
}

[[nodiscard]] ac::CommandMessage full_settings() {
    ac::CommandMessage command;
    command.command = ac::Command::kSettings;
    ac::Settings& settings = command.settings;
    settings.revision = 7;
    settings.layout = "L:small,C,R:small,Ls,Rs,LFE";
    settings.routing = "0,1,2,3,4,5";
    settings.trim_db = std::vector<double>{0.0, -1.5, 0.0, -3.0, -3.0, 2.0};
    settings.delay_ms = std::vector<double>{0.0, 0.0, 0.0, 12.5, 12.5, 0.0};
    settings.crossover_hz = 80.0;
    ac::DecoderSettings& decoder = settings.decoder;
    decoder.mode = ac::DecoderMode::kRf;
    decoder.drc_cut = 0.25;
    decoder.drc_boost = 1.0;
    decoder.heavy_compression = false;
    decoder.dialnorm = true;
    decoder.downmix = ac::Downmix::kLtRt;
    decoder.ltrt_phase_shift = true;
    decoder.mix_lfe = false;
    decoder.has_programme = true;
    decoder.objects = ac::ObjectsPolicy::kNever;
    decoder.concealment = ac::Concealment::kRepeatFade;
    return command;
}

}  // namespace

TEST_CASE("ac3forge_player: names and IDs", "[sendspin][ac3forge]") {
    CHECK(ac::kRole == "_ac3forge_player@v1");
    CHECK(ac::kSupportKey == "_ac3forge_player@v1_support");
    CHECK(ac::kObjectKey == "_ac3forge_player");
    CHECK(ac3::sendspin::message_id::kAc3forgeBurst == 192);
    CHECK(ac::data_type_name(ac::DataType::kAc3) == "ac3");
    CHECK(ac::data_type_name(ac::DataType::kEac3) == "eac3");
    CHECK(ac::data_type_name(ac::DataType::kAc4) == "ac4");
    CHECK(ac::burst_data_type(ac::DataType::kAc3) == ac3::sendspin::BurstDataType::kAc3);
    CHECK(ac::burst_data_type(ac::DataType::kEac3) == ac3::sendspin::BurstDataType::kEac3);
    CHECK(ac::burst_data_type(ac::DataType::kAc4) == ac3::sendspin::BurstDataType::kAc4);
}

TEST_CASE("ac3forge_player: an AC-4 stream carries any of IEC 61937-14's four burst types",
          "[sendspin][ac3forge][ac4]") {
    using ac3::sendspin::BurstDataType;
    for (const BurstDataType burst : {BurstDataType::kAc4, BurstDataType::kAc4Hbr4,
                                      BurstDataType::kAc4Hbr16, BurstDataType::kAc4Ld}) {
        CHECK(ac::carries(ac::DataType::kAc4, burst));
        CHECK_FALSE(ac::carries(ac::DataType::kEac3, burst));
    }
    CHECK_FALSE(ac::carries(ac::DataType::kAc4, BurstDataType::kAc3));
    CHECK(ac::carries(ac::DataType::kAc3, BurstDataType::kAc3));
    CHECK_FALSE(ac::carries(ac::DataType::kAc3, BurstDataType::kEac3));
    CHECK(ac::carries(ac::DataType::kEac3, BurstDataType::kEac3));
}

TEST_CASE("ac3forge_player: the support object", "[sendspin][ac3forge]") {
    const std::string text = written(board_support(), ac::write_support);
    CHECK(text == R"({"data_types":["ac3","eac3"],"sample_rates":[48000],)"
                  R"("outputs":{"count":6,"bit_depth":32,"bit_depths":[16,24,32]},"layout_grammar":1,)"
                  R"("management":{"routing":true,"trim_db":[-12,12],"delay_ms":[0,50],"crossover_hz":[40,250],"identify":true},)"
                  R"("decoder_settings":["mode","drc_cut","drc_boost","heavy_compression","dialnorm","downmix",)"
                  R"("ltrt_phase_shift","mix_lfe","programme","objects","concealment"],"buffer_capacity":1048576})");

    const Parsed parsed(text);
    const std::optional<ac::Support> read = ac::read_support(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->data_types == board_support().data_types);
    CHECK(read->sample_rates == std::vector<std::int32_t>{48000});
    CHECK(read->outputs.count == 6);
    CHECK(read->outputs.bit_depth == 32);
    CHECK(read->outputs.bit_depths == std::vector<std::int32_t>{16, 24, 32});
    CHECK(read->management.routing);
    CHECK(read->management.trim_db[0] == -12.0);
    CHECK(read->management.max_delay_ms == 50.0);
    CHECK(read->management.crossover_hz[1] == 250.0);
    CHECK(read->decoder_settings.size() == ac::kDecoderSettingNames.size());
    CHECK(read->buffer_capacity == 1048576);

    const auto read_text = [](std::string source) { return ac::read_support(Parsed(std::move(source)).root()); };
    const std::string outputs = R"("outputs":{"count":2,"bit_depth":16,"bit_depths":[16]},"layout_grammar":1,)";
    const std::string management =
        R"("management":{"routing":false,"trim_db":[0,0],"delay_ms":[0,0],"crossover_hz":[80,80],"identify":false},)";
    const std::string tail = R"("decoder_settings":[],"buffer_capacity":65536})";
    // A sink of a later day with a data type this reader does not know still offers E-AC-3: the
    // unknown type is left out.
    const std::optional<ac::Support> later = read_text(
        R"({"data_types":["mpegh","eac3"],"sample_rates":[48000],)" + outputs + management + tail);
    REQUIRE(later.has_value());
    CHECK(later->data_types == std::vector<ac::DataType>{ac::DataType::kEac3});
    CHECK(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],"future":{},)" + outputs + management + tail));
    // AC-4 (planning/ac4.md, D11).
    const std::optional<ac::Support> with_ac4 =
        read_text(R"({"data_types":["ac3","eac3","ac4"],"sample_rates":[48000],)" + outputs +
                  management + tail);
    REQUIRE(with_ac4.has_value());
    CHECK(with_ac4->data_types ==
          std::vector<ac::DataType>{ac::DataType::kAc3, ac::DataType::kEac3, ac::DataType::kAc4});

    CHECK_FALSE(read_text(R"({"data_types":["mpegh"],"sample_rates":[48000],)" + outputs +
                          management + tail));
    CHECK_FALSE(read_text(R"({"data_types":[],"sample_rates":[48000],)" + outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3",3],"sample_rates":[48000],)" + outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[0],)" + outputs + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + management + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs + tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs +
                          R"("management":{"routing":false,"trim_db":[6,-6],"delay_ms":[0,0],"crossover_hz":[80,80],"identify":false},)" +
                          tail));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs + management +
                          R"("decoder_settings":[],"buffer_capacity":-1})"));
    CHECK_FALSE(read_text(R"({"data_types":["eac3"],"sample_rates":[48000],)" + outputs + management +
                          R"("buffer_capacity":65536})"));
    CHECK_FALSE(ac::read_support(Parsed(R"(["eac3"])").root()));
}

TEST_CASE("ac3forge_player: the state object", "[sendspin][ac3forge]") {
    ac::State state;
    state.volume = 70;
    state.muted = false;
    state.output_delay_ms = 20;
    state.required_lead_time_ms = 250;
    state.min_buffer_ms = 200;
    state.supported_commands = {ac::Command::kVolume, ac::Command::kMute, ac::Command::kSetOutputDelay,
                                ac::Command::kSettings, ac::Command::kIdentify};
    state.settings_revision = 3;
    state.settings_error = ac::SettingsError{.revision = 4, .why = "trim_db needs one value per output"};
    state.decoder = ac::DecoderReport{.data_type = ac::DataType::kEac3,
                                      .acmod = 7,
                                      .lfe = true,
                                      .substreams = 2,
                                      .objects = 11,
                                      .objects_placed = true,
                                      .dialnorm = -31.0};
    state.levels = std::vector<ac::Level>{
        ac::Level{.output = 0, .peak_db = -6.04, .rms_db = -20.0},
        ac::Level{.output = 1, .peak_db = -std::numeric_limits<double>::infinity(), .rms_db = -200.0},
    };
    state.counters.bursts_played = 1000;
    state.counters.underruns = 1;
    state.counters.late_chunks = 2;
    state.counters.dropped_chunks = 3;
    state.counters.invalid_chunks = 4;

    const std::string text = written(state, ac::write_state);
    CHECK(text == R"({"volume":70,"muted":false,"output_delay_ms":20,"required_lead_time_ms":250,"min_buffer_ms":200,)"
                  R"("supported_commands":["volume","mute","set_output_delay","settings","identify"],"settings_revision":3,)"
                  R"("settings_error":{"revision":4,"why":"trim_db needs one value per output"},)"
                  R"("decoder":{"data_type":"eac3","acmod":7,"lfe":true,"substreams":2,"objects":11,"objects_placed":true,"dialnorm":-31},)"
                  R"("levels":[{"output":0,"peak_db":-6,"rms_db":-20},{"output":1,"peak_db":-120,"rms_db":-120}],)"
                  R"("counters":{"bursts_played":1000,"underruns":1,"late_chunks":2,"dropped_chunks":3,"invalid_chunks":4}})");

    const Parsed parsed(text);
    const std::optional<ac::State> read = ac::read_state(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->volume == 70);
    CHECK(read->muted == false);
    CHECK(read->output_delay_ms == 20);
    CHECK(read->required_lead_time_ms == 250);
    CHECK(read->min_buffer_ms == 200);
    CHECK(read->supported_commands == state.supported_commands);
    CHECK(read->settings_revision == 3);
    REQUIRE(read->settings_error.has_value());
    CHECK(read->settings_error->revision == 4);
    REQUIRE(read->decoder.has_value());
    CHECK(read->decoder->acmod == 7);
    CHECK(read->decoder->objects == 11);
    CHECK(read->decoder->dialnorm == -31.0);
    REQUIRE(read->levels.has_value());
    REQUIRE(read->levels->size() == 2);
    CHECK((*read->levels)[1].peak_db == ac::kSilenceDb);
    CHECK(read->counters.invalid_chunks == 4);
    CHECK_FALSE(read->why.has_value());

    const auto read_text = [](const std::string& fields) {
        return ac::read_state(Parsed("{" + fields + "}").root());
    };
    const std::string counters =
        R"("counters":{"bursts_played":0,"underruns":0,"late_chunks":0,"dropped_chunks":0,"invalid_chunks":0})";
    const std::string timing = R"("output_delay_ms":0,"required_lead_time_ms":0,"min_buffer_ms":0,)";
    const std::string idle = timing + R"("supported_commands":["settings","louder"],"settings_revision":0,)";
    const std::optional<ac::State> quiet = read_text(idle + counters + R"(,"why":"The output went away.")");
    REQUIRE(quiet.has_value());
    // A command this reader does not know is left out.
    CHECK(quiet->supported_commands == std::vector<ac::Command>{ac::Command::kSettings});
    CHECK(quiet->why == "The output went away.");
    CHECK_FALSE(quiet->decoder.has_value());

    CHECK_FALSE(read_text(idle + R"("why":"No counters.")"));
    CHECK_FALSE(read_text(R"("output_delay_ms":5001,"required_lead_time_ms":0,"min_buffer_ms":0,)"
                          R"("supported_commands":[],"settings_revision":0,)" + counters));
    CHECK_FALSE(read_text(R"("volume":101,)" + idle + counters));
    CHECK_FALSE(read_text(idle + counters + R"(,"settings_error":{"revision":1})"));
    CHECK_FALSE(read_text(idle + counters + R"(,"levels":[{"output":0,"peak_db":null,"rms_db":-20}])"));
    CHECK_FALSE(read_text(idle + counters +
                          R"(,"decoder":{"data_type":"eac3","acmod":8,"lfe":true,"substreams":1,"objects":0,"objects_placed":false,"dialnorm":-31})"));
    CHECK_FALSE(read_text(timing + R"("supported_commands":"settings","settings_revision":0,)" + counters));
}

TEST_CASE("ac3forge_player: the stream/start object", "[sendspin][ac3forge]") {
    const std::string text =
        written(ac::StreamStart{.data_type = ac::DataType::kEac3, .sample_rate = 48000}, ac::write_stream_start);
    CHECK(text == R"({"data_type":"eac3","sample_rate":48000})");
    const Parsed parsed(text);
    const std::optional<ac::StreamStart> read = ac::read_stream_start(parsed.root());
    REQUIRE(read.has_value());
    CHECK(read->data_type == ac::DataType::kEac3);
    CHECK(read->sample_rate == 48000);

    CHECK(ac::read_stream_start(Parsed(R"({"data_type":"ac3","sample_rate":44100,"future":true})").root()));
    const std::optional<ac::StreamStart> ac4 =
        ac::read_stream_start(Parsed(R"({"data_type":"ac4","sample_rate":48000})").root());
    REQUIRE(ac4.has_value());
    CHECK(ac4->data_type == ac::DataType::kAc4);
    CHECK_FALSE(
        ac::read_stream_start(Parsed(R"({"data_type":"mpegh","sample_rate":48000})").root()));
    CHECK_FALSE(ac::read_stream_start(Parsed(R"({"data_type":"eac3"})").root()));
    CHECK_FALSE(ac::read_stream_start(Parsed(R"({"sample_rate":48000})").root()));
}

TEST_CASE("ac3forge_player: the player@v1 commands and identify", "[sendspin][ac3forge]") {
    const auto round_trip = [](const ac::CommandMessage& command, std::string_view expected) {
        const std::string text = written(command, ac::write_command);
        CHECK(text == expected);
        const Parsed parsed(text);
        auto read = ac::read_command(parsed.root());
        REQUIRE(read.has_value());
        CHECK(read->command == command.command);
        return *read;
    };
    ac::CommandMessage volume;
    volume.command = ac::Command::kVolume;
    volume.volume = 40;
    CHECK(round_trip(volume, R"({"command":"volume","volume":40})").volume == 40);

    ac::CommandMessage mute;
    mute.command = ac::Command::kMute;
    mute.mute = true;
    CHECK(round_trip(mute, R"({"command":"mute","mute":true})").mute);

    ac::CommandMessage delay;
    delay.command = ac::Command::kSetOutputDelay;
    delay.output_delay_ms = 120;
    CHECK(round_trip(delay, R"({"command":"set_output_delay","output_delay_ms":120})").output_delay_ms == 120);

    ac::CommandMessage identify;
    identify.command = ac::Command::kIdentify;
    identify.identify = ac::Identify{.output = 3, .level_db = -30.0};
    const ac::CommandMessage tone = round_trip(identify, R"({"command":"identify","identify":{"output":3,"level_db":-30}})");
    REQUIRE(tone.identify.has_value());
    CHECK(tone.identify->output == 3);
    CHECK(tone.identify->level_db == -30.0);

    identify.identify.reset();
    CHECK_FALSE(round_trip(identify, R"({"command":"identify","identify":null})").identify.has_value());

    const auto failure = [](std::string text) {
        const auto read = ac::read_command(Parsed(std::move(text)).root());
        REQUIRE_FALSE(read.has_value());
        return read.error().error;
    };
    CHECK(failure(R"({"command":"louder"})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"volume","volume":101})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"mute"})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"set_output_delay","output_delay_ms":5001})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify"})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify","identify":{"output":0,"level_db":-6}})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify","identify":{"output":0,"level_db":-61}})") == ac::CommandError::kMalformed);
    CHECK(failure(R"({"command":"identify","identify":{"level_db":-30}})") == ac::CommandError::kMalformed);
}

TEST_CASE("ac3forge_player: settings", "[sendspin][ac3forge]") {
    const std::string text = written(full_settings(), ac::write_command);
    CHECK(text == R"({"command":"settings","settings":{"revision":7,"layout":"L:small,C,R:small,Ls,Rs,LFE",)"
                  R"("routing":"0,1,2,3,4,5","trim_db":[0,-1.5,0,-3,-3,2],"delay_ms":[0,0,0,12.5,12.5,0],"crossover_hz":80,)"
                  R"("decoder":{"mode":"rf","drc_cut":0.25,"drc_boost":1,"heavy_compression":false,"dialnorm":true,)"
                  R"("downmix":"ltrt","ltrt_phase_shift":true,"mix_lfe":false,"programme":null,"objects":"never",)"
                  R"("concealment":"repeat_fade"}}})");

    const Parsed parsed(text);
    const auto read = ac::read_command(parsed.root());
    REQUIRE(read.has_value());
    const ac::Settings& settings = read->settings;
    CHECK(settings.revision == 7);
    CHECK(settings.layout == "L:small,C,R:small,Ls,Rs,LFE");
    CHECK(settings.routing == "0,1,2,3,4,5");
    CHECK(settings.trim_db == full_settings().settings.trim_db);
    CHECK(settings.delay_ms == full_settings().settings.delay_ms);
    CHECK(settings.crossover_hz == 80.0);
    CHECK(settings.decoder.mode == ac::DecoderMode::kRf);
    CHECK(settings.decoder.drc_cut == 0.25);
    CHECK(settings.decoder.drc_boost == 1.0);
    CHECK(settings.decoder.heavy_compression == false);
    CHECK(settings.decoder.dialnorm == true);
    CHECK(settings.decoder.downmix == ac::Downmix::kLtRt);
    CHECK(settings.decoder.ltrt_phase_shift == true);
    CHECK(settings.decoder.mix_lfe == false);
    CHECK(settings.decoder.has_programme);
    CHECK_FALSE(settings.decoder.programme.has_value());
    CHECK(settings.decoder.objects == ac::ObjectsPolicy::kNever);
    CHECK(settings.decoder.concealment == ac::Concealment::kRepeatFade);

    const auto read_settings = [](const std::string& object) {
        return ac::read_command(Parsed(R"({"command":"settings","settings":)" + object + "}").root());
    };
    // Absent keys stay absent, and unknown keys are ignored.
    const auto sparse = read_settings(R"({"revision":2,"future":"x","decoder":{"programme":1,"future":1}})");
    REQUIRE(sparse.has_value());
    CHECK_FALSE(sparse->settings.layout.has_value());
    CHECK_FALSE(sparse->settings.trim_db.has_value());
    CHECK_FALSE(sparse->settings.decoder.mode.has_value());
    CHECK(sparse->settings.decoder.has_programme);
    CHECK(sparse->settings.decoder.programme == 1);
    const auto bare = read_settings(R"({"revision":3})");
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->settings.decoder.has_programme);

    const auto refused = [&](const std::string& object) {
        const auto result = read_settings(object);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ac::CommandError::kSettingsRefused);
        CHECK_FALSE(result.error().settings.why.empty());
        return result.error().settings.revision;
    };
    CHECK(refused(R"({"revision":9,"decoder":{"drc_cut":1.5}})") == 9);
    CHECK(refused(R"({"revision":10,"decoder":{"mode":"loud"}})") == 10);
    CHECK(refused(R"({"revision":11,"decoder":{"programme":"first"}})") == 11);
    CHECK(refused(R"({"revision":12,"decoder":{"dialnorm":1}})") == 12);
    CHECK(refused(R"({"revision":13,"delay_ms":[0,-1]})") == 13);
    CHECK(refused(R"({"revision":14,"trim_db":"flat"})") == 14);
    CHECK(refused(R"({"revision":15,"crossover_hz":0})") == 15);
    CHECK(refused(R"({"revision":16,"layout":51})") == 16);
    CHECK(refused(R"({"revision":17,"decoder":[]})") == 17);
    CHECK(refused(R"({"revision":18,"decoder":{"objects":"sometimes"}})") == 18);
    // A number the writer could not write back, and a crossover that its tenth of a hertz rounds to
    // nothing (fuzz/regressions/fuzz_sendspin_messages).
    CHECK(refused(R"({"revision":19,"trim_db":[7.7e62,0]})") == 19);
    CHECK(refused(R"({"revision":20,"crossover_hz":0.04})") == 20);
    CHECK(read_settings(R"({"revision":21,"crossover_hz":0.05})").has_value());

    const auto malformed = read_settings(R"({"layout":"5.1"})");
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().error == ac::CommandError::kMalformed);
    CHECK_FALSE(ac::read_command(Parsed(R"({"command":"settings"})").root()).has_value());
}

TEST_CASE("ac3forge_player: settings checked against the support object", "[sendspin][ac3forge]") {
    const ac::Support support = board_support();
    CHECK_FALSE(ac::check_settings(full_settings().settings, support).has_value());
    CHECK_FALSE(ac::check_settings(ac::Settings{}, support).has_value());

    const auto why = [&](void (*change)(ac::Settings&), ac::Support with = board_support()) {
        ac::Settings settings = full_settings().settings;
        change(settings);
        return ac::check_settings(settings, with).value_or("");
    };
    CHECK(why([](ac::Settings& s) { s.trim_db->pop_back(); }) == "trim_db needs one value per output");
    CHECK(why([](ac::Settings& s) { (*s.trim_db)[0] = 12.5; }) == "a trim_db value is outside management.trim_db");
    CHECK(why([](ac::Settings& s) { s.delay_ms->push_back(0.0); }) == "delay_ms needs one value per output");
    CHECK(why([](ac::Settings& s) { (*s.delay_ms)[5] = 50.5; }) == "a delay_ms value is outside management.delay_ms");
    CHECK(why([](ac::Settings& s) { s.crossover_hz = 30.0; }) == "crossover_hz is outside management.crossover_hz");

    ac::Support fixed = board_support();
    fixed.management.routing = false;
    CHECK(why([](ac::Settings&) {}, fixed) == "this sink does not take routing");
    CHECK(why([](ac::Settings& s) { s.routing.reset(); }, fixed).empty());

    ac::Support simple = board_support();
    simple.decoder_settings = {"mode", "drc_cut", "drc_boost", "heavy_compression", "dialnorm", "downmix",
                               "ltrt_phase_shift", "mix_lfe", "objects", "concealment"};
    CHECK(why([](ac::Settings&) {}, simple) == "decoder.programme is not a setting this sink takes");
    CHECK(why([](ac::Settings& s) { s.decoder.has_programme = false; }, simple).empty());
}
