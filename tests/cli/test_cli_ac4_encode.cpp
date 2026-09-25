// ac3cli ac4-encode's options (planning/ac4.md, phase E7), each run against the
// real binary and checked in what it writes: the stream's sync frames and
// table of contents, and the syntax trace (syntax-trace=) of the elements the
// option sets. The stream options first (crc=, the codec mode, the frame and
// rate modes, I-frames, the downmix, DRC, loudness and dialogue enhancement
// values the other tests leave out, and each experimental tool), then the
// substreams and presentations (substreamN=, presentationN= and their keys),
// then what the command refuses. tests/cli/test_cli_containers.cpp has the
// first tests of the command (the codec modes, 5.1 and 7.1, frame rates and
// I-frames, and the metadata a 5.1 stream sends).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/io/wav.hpp"
#include "ac4/ac4.hpp"

namespace fs = std::filesystem;

namespace {

// Per this project's per-file test-helper convention (see
// tests/cli/test_cli_containers.cpp, whose shapes these copy).
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} /
               ("cli_ac4_encode_" + ac3::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return ac3::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

constexpr int kRate = 48000;

std::vector<float> tone(double hz, std::size_t count, double amplitude = 0.1) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
    }
    return x;
}

// A WAV file of `channels`, written to `name` in the scratch directory.
fs::path wav_of(const std::string& name, const std::vector<std::vector<float>>& channels) {
    const fs::path path = scratch_dir() / name;
    REQUIRE(ac3::io::write_wav_f32(path.string(), channels, kRate).has_value());
    return path;
}

// A second of a tone on each of `count` channels, each at its own frequency.
fs::path tones_wav(const std::string& name, std::size_t count, std::size_t seconds = 1) {
    std::vector<std::vector<float>> channels;
    for (std::size_t c = 0; c < count; ++c) {
        channels.push_back(tone(331.0 + 157.0 * static_cast<double>(c), seconds * kRate));
    }
    return wav_of(name, channels);
}

// One syntax-trace= record: frame, substream, value and name.
struct Record {
    long long frame = 0;
    int substream = 0;
    std::uint64_t value = 0;
    std::string name;
};

std::vector<Record> read_trace(const fs::path& path) {
    std::ifstream in{path};
    REQUIRE(in.is_open());
    std::vector<Record> out;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        std::size_t from = 0;
        for (std::size_t tab = line.find('\t'); tab != std::string::npos;
             tab = line.find('\t', from)) {
            fields.push_back(line.substr(from, tab - from));
            from = tab + 1;
        }
        fields.push_back(line.substr(from));
        REQUIRE(fields.size() == 6);
        out.push_back(Record{.frame = std::stoll(fields[0]),
                             .substream = std::stoi(fields[1]),
                             .value = std::stoull(fields[4]),
                             .name = fields[5]});
    }
    return out;
}

// The values of `name` in frame 0, in order, of every substream or of one.
std::vector<std::uint64_t> first_frame(const std::vector<Record>& records, std::string_view name,
                                       std::optional<int> substream = std::nullopt) {
    std::vector<std::uint64_t> out;
    for (const Record& r : records) {
        if (r.frame == 0 && r.name == name && (!substream || r.substream == *substream)) {
            out.push_back(r.value);
        }
    }
    return out;
}

// How many records of `name` over the stream hold `value`.
std::size_t count_of(const std::vector<Record>& records, std::string_view name,
                     std::uint64_t value) {
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [&](const Record& r) { return r.name == name && r.value == value; }));
}

// The table of contents of a raw stream's first frame. The frames are views of
// `bytes`, which the caller keeps.
ac4::Toc first_toc(const std::vector<std::byte>& bytes) {
    const ac4::ScanResult scanned = ac4::scan(bytes);
    REQUIRE_FALSE(scanned.frames.empty());
    const auto frame = ac4::parse_raw_frame(scanned.frames.front().raw_ac4_frame);
    REQUIRE(frame.has_value());
    return frame->toc;
}

}  // namespace

TEST_CASE("ac4-encode's crc= puts Annex G's CRC in each sync frame or leaves it out",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_crc.log";
    const fs::path in = tones_wav("ac4_crc_in.wav", 2);
    const fs::path out = dir / "ac4_crc.ac4";
    for (const std::string_view option : {"", " crc=on", " crc=off"}) {
        CAPTURE(option);
        REQUIRE(
            run_cli("ac4-encode " + quoted(in) + " " + quoted(out) + " 192" + std::string{option},
                    log) == 0);
        const bool crc = option != " crc=off";
        CHECK(read_log(log).find(crc ? "raw with CRC" : "raw without CRC") != std::string::npos);
        const std::vector<std::byte> bytes = read_bytes(out);
        const ac4::ScanResult scanned = ac4::scan(bytes);
        REQUIRE_FALSE(scanned.frames.empty());
        CHECK_FALSE(scanned.stopped_at.has_value());
        for (const ac4::SyncFrame& frame : scanned.frames) {
            CHECK(frame.sync_word == (crc ? 0xAC41 : 0xAC40));
            CHECK(frame.crc_ok == (crc ? std::optional<bool>{true} : std::nullopt));
            CHECK(frame.raw_ac4_frame.size() == 1024U);  // 192 kbps at 2 048 samples a frame
        }
    }
    // An MP4 sample is the raw frame alone: no sync word, no CRC to ask for.
    CHECK(run_cli("ac4-encode " + quoted(in) + " " + quoted(dir / "ac4_crc.mp4") + " 192 crc=off",
                  log) == 1);
    CHECK(read_log(log).find("an MP4 sample is the raw frame alone") != std::string::npos);
}

TEST_CASE("ac4-encode's stream options each write what they name", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_stream_options.log";
    const auto trace = dir / "ac4_stream_options.tsv";
    const fs::path out = dir / "ac4_stream_options.ac4";
    const fs::path stereo = tones_wav("ac4_options_stereo.wav", 2, 2);
    const fs::path five_one = tones_wav("ac4_options_51.wav", 6, 2);
    struct Run {
        const char* name;
        const fs::path* in;
        std::string args;
        std::string_view status;  // what the status lines say, where they say it
        std::string_view record;  // a record of the first frame, and its values
        std::vector<std::uint64_t> values;
    };
    const std::vector<Run> runs = {
        // The codec modes by name, and the frame and rate modes the default names.
        {"codec-mode=auto", &stereo, " 64 codec-mode=auto", "ASPX mode", "", {}},
        {"codec-mode=simple",
         &stereo,
         " 64 codec-mode=simple",
         "SIMPLE mode",
         "stereo_codec_mode",
         {0}},
        {"codec-mode=aspx", &stereo, " 192 codec-mode=aspx", "ASPX mode", "stereo_codec_mode", {1}},
        {"codec-mode=aspx-acpl-3",
         &five_one,
         " 96 codec-mode=aspx-acpl-3",
         "ASPX_ACPL_3 mode",
         "",
         {}},
        {"frame-rate=native", &stereo, " 192 frame-rate=native", "2 048-sample frames", "", {}},
        {"rate-mode=constant", &stereo, " 192 rate-mode=constant", "constant rate", "", {}},
        // The downmix values the metadata test leaves out: cmixlev= and
        // surmixlev= (Lo/Ro's), lorosurmixlev=, ltrtcmixlev=, ltrt-correction=,
        // lfemix=off and dmixmod='s other values.
        {"cmixlev=-4.5", &five_one, " 256 cmixlev=-4.5", "", "loro_centre_mixgain", {5}},
        {"surmixlev=-6", &five_one, " 256 surmixlev=-6", "", "loro_surround_mixgain", {6}},
        {"lorosurmixlev=off",
         &five_one,
         " 256 lorosurmixlev=off",
         "",
         "loro_surround_mixgain",
         {7}},
        {"ltrtcmixlev=+1.5", &five_one, " 256 ltrtcmixlev=+1.5", "", "ltrt_centre_mixgain", {1}},
        {"ltrt-correction=+3", &five_one, " 256 ltrt-correction=+3", "", "ltrt_dmx_loud_corr", {9}},
        {"lfemix=off", &five_one, " 256 cmixlev=-3 lfemix=off", "", "b_lfe_mixinfo", {0}},
        {"dmixmod=loro", &five_one, " 256 dmixmod=loro", "", "preferred_dmx_method", {1}},
        {"dmixmod=ltrt", &five_one, " 256 dmixmod=ltrt", "", "preferred_dmx_method", {2}},
        {"dmixmod=none", &five_one, " 256 dmixmod=none", "", "preferred_dmx_method", {0}},
        // The stream's profile named none, and DRC's first two modes on
        // profiles of their own, each sent as its curve.
        {"drc=none", &stereo, " 192 drc=none", "", "drc_eac3_profile", {0}},
        {"drc-home-theatre=speech",
         &stereo,
         " 192 drc=none drc-home-theatre=speech",
         "",
         "drc_default_profile_flag",
         {0, 1, 1, 1}},
        {"drc-flat-panel-tv=music-light",
         &stereo,
         " 192 drc=film-light drc-flat-panel-tv=music-light",
         "",
         "drc_default_profile_flag",
         {1, 0, 1, 1}},
        // A loudness practice besides EBU R 128's.
        {"loudness=atsc-a85",
         &stereo,
         " 192 loudness=atsc-a85",
         "loudness range",
         "loud_prac_type",
         {1}},
        // Dialogue enhancement by the Mid of L and R, and by each channel.
        {"dialogue-method=mid",
         &stereo,
         " 192 dialogue-channels=l,r dialogue-method=mid",
         "",
         "de_ms_proc_flag",
         {1}},
        {"dialogue-method=independent",
         &stereo,
         " 192 dialogue-channels=l,r dialogue-method=independent",
         "",
         "de_ms_proc_flag",
         {0}},
        // Transmitted DRC gains, experimental: configuration 1, a gain per
        // channel group and subframe.
        {"experimental=drc-gains-1",
         &five_one,
         " 256 drc=film-standard experimental=drc-gains-1",
         "",
         "drc_gains_config",
         {1, 1, 1, 1}},
    };
    for (const Run& run : runs) {
        CAPTURE(run.name);
        REQUIRE(run_cli("ac4-encode " + quoted(*run.in) + " " + quoted(out) + run.args +
                            " syntax-trace=" + quoted(trace),
                        log) == 0);
        if (!run.status.empty()) {
            CHECK(read_log(log).find(run.status) != std::string::npos);
        }
        if (!run.record.empty()) {
            const std::vector<Record> records = read_trace(trace);
            CHECK(first_frame(records, run.record) == run.values);
        }
    }
}

TEST_CASE("ac4-encode's I-frame options put I-frames where they say", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_iframes.log";
    const fs::path in = tones_wav("ac4_iframes_in.wav", 2, 2);
    const fs::path out = dir / "ac4_iframes.ac4";
    const auto iframes_of = [&](const std::string& options) {
        REQUIRE(run_cli("ac4-encode " + quoted(in) + " " + quoted(out) + " 192" + options, log) ==
                0);
        const std::vector<std::byte> bytes = read_bytes(out);
        const ac4::ScanResult scanned = ac4::scan(bytes);
        std::vector<std::size_t> out_frames;
        for (std::size_t i = 0; i < scanned.frames.size(); ++i) {
            const auto frame = ac4::parse_raw_frame(scanned.frames[i].raw_ac4_frame);
            REQUIRE(frame.has_value());
            if (frame->toc.b_iframe_global) {
                out_frames.push_back(i);
            }
        }
        return out_frames;
    };
    // The default interval, 24 frames, over the fifty frames two seconds make.
    CHECK(iframes_of("") == std::vector<std::size_t>{0, 24, 48});
    CHECK(iframes_of(" iframe-interval=10") == std::vector<std::size_t>{0, 10, 20, 30, 40});
    CHECK(iframes_of(" iframe-interval=100 iframes=3,7") == std::vector<std::size_t>{0, 3, 7});
    // fragment=0.5: an I-frame where each half second of the output starts,
    // about every 11.7 frames of 2 048 samples.
    const std::vector<std::size_t> fragments = iframes_of(" iframe-interval=100 fragment=0.5");
    REQUIRE(fragments.size() >= 5);
    for (std::size_t k = 1; k < fragments.size(); ++k) {
        CAPTURE(k);
        CHECK(fragments[k] - fragments[k - 1] >= 11U);
        CHECK(fragments[k] - fragments[k - 1] <= 12U);
    }
}

TEST_CASE("ac4-encode's experimental tools each write their syntax", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_experimental.log";
    const auto trace = dir / "ac4_experimental.tsv";
    const fs::path out = dir / "ac4_experimental.ac4";
    const std::size_t count = 2 * kRate;
    const auto run = [&](const fs::path& in, const std::string& options) {
        REQUIRE(run_cli("ac4-encode " + quoted(in) + " " + quoted(out) + " " + options +
                            " syntax-trace=" + quoted(trace),
                        log) == 0);
        return read_trace(trace);
    };
    SECTION("aspx-balance codes equal channels as a sum and a balance") {
        const std::vector<float> x = tone(440.0, count);
        const auto records = run(wav_of("ac4_balance.wav", {x, x}), "48 experimental=aspx-balance");
        CHECK(count_of(records, "aspx_balance", 1) > 0U);
    }
    SECTION("aspx-varvar frames attacks in intervals that start where the last ran on") {
        std::vector<float> clicks(count, 0.0F);
        std::uint32_t seed = 7;
        for (std::size_t n = 6000; n + 1200 < count; n += 2600) {
            for (std::size_t k = 0; k < 1200; ++k) {
                seed = seed * 1664525U + 1013904223U;
                const double noise = static_cast<double>(seed >> 8U) / 16777216.0 - 0.5;
                clicks[n + k] =
                    static_cast<float>(0.8 * std::exp(-static_cast<double>(k) / 150.0) * noise);
            }
        }
        const auto records =
            run(wav_of("ac4_varvar.wav", {clicks, clicks}), "64 experimental=aspx-varvar");
        CHECK(count_of(records, "aspx_int_class", 0b111) > 0U);
    }
    SECTION("aspx-interleave codes a steady tone above the crossover") {
        std::vector<float> x = tone(440.0, count);
        const std::vector<float> high = tone(17100.0, count, 0.05);
        for (std::size_t n = 0; n < count; ++n) {
            x[n] += high[n];
        }
        const auto records =
            run(wav_of("ac4_interleave.wav", {x, x}), "96 experimental=aspx-interleave");
        CHECK(count_of(records, "aspx_fic_present", 1) > 0U);
    }
    SECTION("coding-configs chooses among the 5.X element's coding configurations") {
        // A second of independent tones, then one signal in L, R and C.
        std::vector<std::vector<float>> input(6, std::vector<float>(count, 0.0F));
        const std::vector<float> shared = tone(523.0, kRate, 0.2);
        for (std::size_t c = 0; c < 6; ++c) {
            const std::vector<float> t = tone(331.0 + 157.0 * static_cast<double>(c), kRate);
            std::ranges::copy(t, input[c].begin());
        }
        for (std::size_t n = 0; n < static_cast<std::size_t>(kRate); ++n) {
            input[0][kRate + n] = shared[n];
            input[1][kRate + n] = 0.8F * shared[n];
            input[2][kRate + n] = 0.6F * shared[n];
        }
        const auto records =
            run(wav_of("ac4_coding_configs.wav", input), "256 experimental=coding-configs");
        std::size_t configs = 0;
        for (const std::uint64_t value : {0U, 1U, 2U, 3U}) {
            configs += count_of(records, "coding_config", value) > 0U ? 1U : 0U;
        }
        CHECK(configs >= 2U);
    }
    SECTION("7x-wide and 7x-top-front take the 7.X element's other pairs") {
        struct Layout {
            const char* option;
            std::size_t channels;
            const char* mode;
        };
        for (const Layout layout :
             {Layout{"7x-wide", 8, "7.1: 5/2/0.1"}, Layout{"7x-top-front", 7, "7.0: 3/2/2"}}) {
            CAPTURE(layout.option);
            (void)run(tones_wav("ac4_seven.wav", layout.channels),
                      "512 experimental=" + std::string{layout.option});
            const std::vector<std::byte> bytes = read_bytes(out);
            const ac4::Toc toc = first_toc(bytes);
            REQUIRE(toc.substream_groups.size() == 1);
            CHECK(toc.substream_groups[0].substreams.at(0).chan->channel_mode_name == layout.mode);
        }
    }
}

TEST_CASE("ac4-encode's substream and presentation options each write what they name",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_presentations.log";
    const auto trace = dir / "ac4_presentations.tsv";
    const fs::path out = dir / "ac4_presentations.ac4";
    const fs::path music = tones_wav("ac4_me51.wav", 6);
    const fs::path english = wav_of("ac4_english.wav", {tone(1117.0, kRate)});
    const fs::path described = wav_of("ac4_described.wav", {tone(1531.0, kRate)});
    // Music and effects in 5.1 with English dialogue and an audio description:
    // the dialogue at a mix gain cap and a pan, with a payload; the
    // description coded in SIMPLE; and four presentations, of configurations
    // 0, 3 and 5 and a single group, with ids, levels, gains, the associated
    // audio's mixing values, a dialnorm of their own, payloads, and an
    // alternative, disabled and pre-virtualized one.
    REQUIRE(
        run_cli(
            "ac4-encode " + quoted(music) + " " + quoted(out) +
                " 448 substream1-content=music-and-effects"
                " substream2=" +
                quoted(english) +
                " substream2-content=dialogue substream2-language=en substream2-bitrate=64"
                " substream2-max-dialogue-gain=6 substream2-pan=330 substream2-emdf=3:09080706"
                " substream3=" +
                quoted(described) +
                " substream3-content=visually-impaired substream3-language=qad "
                "substream3-bitrate=48"
                " substream3-codec-mode=simple"
                " presentation1=1,2 presentation1-config=0 presentation1-id=1 "
                "presentation1-gains=0,-2"
                " presentation1-dialnorm=24 presentation1-md-compat=3 presentation1-emdf=2:010203"
                " presentation2=1,2,3 presentation2-config=3 presentation2-id=2 "
                "presentation2-main-gain=-6"
                " presentation2-main-centre-gain=-3 presentation2-main-front-gain=-1.5"
                " presentation2-associated-pan=30"
                " presentation3=1,3 presentation3-config=5 presentation3-id=3"
                " presentation4=1 presentation4-id=4 presentation4-name=Music "
                "presentation4-enabled=off"
                " presentation4-pre-virtualized=on"
                " syntax-trace=" +
                quoted(trace),
            log) == 0);
    INFO(read_log(log));
    CHECK(read_log(log).find("3 substreams, 4 presentations") != std::string::npos);
    const std::vector<std::byte> bytes = read_bytes(out);
    const ac4::Toc toc = first_toc(bytes);
    REQUIRE(toc.presentations_v1.size() == 4);
    const auto& p = toc.presentations_v1;
    CHECK(p[0].presentation_config == 0);
    CHECK(p[1].presentation_config == 3);
    CHECK(p[2].presentation_config == 5);
    CHECK_FALSE(p[3].presentation_config.has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(p[i].presentation_id == static_cast<int>(i + 1));
    }
    CHECK(p[0].md_compat == 3);
    CHECK(p[3].b_alternative);
    CHECK(p[3].enable_presentation == false);
    CHECK(p[3].b_pre_virtualized);
    REQUIRE(toc.substream_groups.size() == 3);
    const std::vector<int> classifiers = {1, 4, 2};
    const std::vector<std::string> languages = {"", "en", "qad"};
    for (std::size_t g = 0; g < 3; ++g) {
        CAPTURE(g);
        REQUIRE(toc.substream_groups[g].content_type.has_value());
        CHECK(toc.substream_groups[g].content_type->content_classifier == classifiers[g]);
        std::string tag;
        for (const std::byte b : toc.substream_groups[g].content_type->language_tag.value_or(
                 std::vector<std::byte>{})) {
            tag.push_back(static_cast<char>(b));
        }
        CHECK(tag == languages[g]);
    }
    const std::vector<Record> records = read_trace(trace);
    // The presentation substreams are 0 to 3, the audio 4 to 6.
    CHECK(first_frame(records, "sg_gain", 0) ==
          std::vector<std::uint64_t>{0, 8});  // -2 dB in 0.25 dB steps
    CHECK(first_frame(records, "dialnorm_bits", 0) == std::vector<std::uint64_t>{96});  // 24 dB
    CHECK(first_frame(records, "dialnorm_bits", 1) ==
          std::vector<std::uint64_t>{124});  // the stream's 31
    CHECK(first_frame(records, "scale_main", 1) ==
          std::vector<std::uint64_t>{20});  // -6 dB in 0.3 dB steps
    CHECK(first_frame(records, "scale_main_centre", 1) == std::vector<std::uint64_t>{10});
    CHECK(first_frame(records, "scale_main_front", 1) == std::vector<std::uint64_t>{5});
    CHECK(first_frame(records, "pan_associated", 1) ==
          std::vector<std::uint64_t>{20});  // 30 degrees
    CHECK(first_frame(records, "dialog_max_gain", 5) == std::vector<std::uint64_t>{1});  // 6 dB
    CHECK(first_frame(records, "pan_dialog", 5) == std::vector<std::uint64_t>{220});  // 330 degrees
    CHECK(first_frame(records, "mono_codec_mode", 5) ==
          std::vector<std::uint64_t>{1});  // ASPX by the rate
    CHECK(first_frame(records, "mono_codec_mode", 6) ==
          std::vector<std::uint64_t>{0});  // SIMPLE as asked
    const std::vector<std::uint64_t> ids = first_frame(records, "emdf_payload_id");
    CHECK(std::ranges::count(ids, 3U) == 1);
    CHECK(std::ranges::count(ids, 2U) == 1);

    // The same in an MP4 file decodes by presentation_id.
    const fs::path mp4 = dir / "ac4_presentations.mp4";
    REQUIRE(run_cli("ac4-encode " + quoted(music) + " " + quoted(mp4) +
                        " 448 substream1-content=music-and-effects substream2=" + quoted(english) +
                        " substream2-content=dialogue substream2-language=en presentation1=1,2 "
                        "presentation1-config=0"
                        " presentation2=1",
                    log) == 0);
    CHECK(read_log(log).find("2 substreams, 2 presentations, 448 kbps, dialnorm -31 dB, MP4") !=
          std::string::npos);
    const fs::path decoded = dir / "ac4_presentations.wav";
    REQUIRE(run_cli("decode " + quoted(mp4) + " " + quoted(decoded) + " presentation-id=1", log) ==
            0);
    CHECK(read_log(log).find("presentation 1 (presentation_id 1)") != std::string::npos);
}

TEST_CASE("ac4-encode's dialogue enhancement substreams stems and EMDF-only presentations",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_dialogue_substreams.log";
    const auto trace = dir / "ac4_dialogue_substreams.tsv";
    const fs::path out = dir / "ac4_dialogue_substreams.ac4";
    // A 5.1 main whose C carries dialogue alone.
    std::vector<std::vector<float>> main51;
    for (std::size_t c = 0; c < 6; ++c) {
        main51.push_back(tone(c == 2 ? 1201.0 : 331.0 + 157.0 * static_cast<double>(c), kRate));
    }
    const fs::path main = wav_of("ac4_main51.wav", main51);
    SECTION(
        "a hybrid method's waveform in a dialogue enhancement substream, substream1-... spelt") {
        REQUIRE(
            run_cli("ac4-encode " + quoted(main) + " " + quoted(out) +
                        " 384 substream1-dialogue-channels=c substream1-dialogue-hybrid=0.5"
                        " substream1-dialogue-max-gain=12 substream1-dialogue-method=independent"
                        " substream1-codec-mode=simple"
                        " substream2-enhances=1 substream2-bitrate=64"
                        " presentation1=1,2 presentation1-config=1 presentation2=1 syntax-trace=" +
                        quoted(trace),
                    log) == 0);
        INFO(read_log(log));
        CHECK(read_log(log).find("SIMPLE mode") != std::string::npos);
        const std::vector<std::byte> bytes = read_bytes(out);
        const ac4::Toc toc = first_toc(bytes);
        REQUIRE(toc.presentations_v1.size() == 2);
        CHECK(toc.presentations_v1[0].presentation_config == 1);
        const std::vector<Record> records = read_trace(trace);
        CHECK(first_frame(records, "de_method") ==
              std::vector<std::uint64_t>{2});  // Table 170: hybrid, independent
        CHECK(first_frame(records, "de_max_gain") == std::vector<std::uint64_t>{3});  // 12 dB
        CHECK(first_frame(records, "de_signal_contribution") ==
              std::vector<std::uint64_t>{16});  // 0.5 x 31
    }
    SECTION("a later substream's dialogue from a stem by the cross-channel method") {
        std::vector<std::vector<float>> stem(6, std::vector<float>(kRate, 0.0F));
        stem[2] = main51[2];
        const fs::path stem_path = wav_of("ac4_main51_stem.wav", stem);
        const fs::path stereo = tones_wav("ac4_dialogue_stereo.wav", 2);
        REQUIRE(
            run_cli("ac4-encode " + quoted(stereo) + " " + quoted(out) +
                        " 384 substream1-content=main substream2=" + quoted(main) +
                        " substream2-content=main substream2-dialogue-stem=" + quoted(stem_path) +
                        " substream2-dialogue-method=cross substream2-dialogue-max-gain=6"
                        " presentation1=1 presentation2=2 syntax-trace=" +
                        quoted(trace),
                    log) == 0);
        INFO(read_log(log));
        const std::vector<Record> records = read_trace(trace);
        CHECK(first_frame(records, "de_method") == std::vector<std::uint64_t>{1});  // cross-channel
        CHECK(first_frame(records, "de_channel_config") ==
              std::vector<std::uint64_t>{7});                                         // L, R and C
        CHECK(first_frame(records, "de_max_gain") == std::vector<std::uint64_t>{1});  // 6 dB
    }
    SECTION("EMDF payloads alone in a presentation of configuration 6") {
        REQUIRE(run_cli("ac4-encode " + quoted(tones_wav("ac4_emdf_stereo.wav", 2)) + " " +
                            quoted(out) +
                            " 128 presentation1=1 presentation2-config=6 presentation2-emdf=1:e606"
                            " presentation2-emdf=20: syntax-trace=" +
                            quoted(trace),
                        log) == 0);
        const std::vector<std::byte> bytes = read_bytes(out);
        const ac4::Toc toc = first_toc(bytes);
        REQUIRE(toc.presentations_v1.size() == 2);
        CHECK(toc.presentations_v1[1].presentation_config == 6);
        const std::vector<std::uint64_t> ids = first_frame(read_trace(trace), "emdf_payload_id");
        CHECK(std::ranges::count(ids, 1U) == 1);
        CHECK(std::ranges::count(ids, 20U) == 1);
        // An MP4 carries the presentation, and a CMAF track cannot: it has no
        // field for the presentation_id Part 2 Annex H.1.2.1 asks of each.
        CHECK(run_cli("mp4 " + quoted(out) + " " + quoted(dir / "ac4_emdf.mp4"), log) == 0);
        const fs::path fragments = dir / "ac4_emdf_fmp4";
        CHECK(run_cli("fmp4 " + quoted(out) + " " + quoted(fragments), log) == 2);
        CHECK(read_log(log).find("a CMAF track cannot carry a presentation of configuration 6") !=
              std::string::npos);
        CHECK_FALSE(fs::exists(fragments / "init.mp4"));
    }
    SECTION("3.0 dialogue beside music and effects, with experimental=three-zero") {
        const fs::path three = tones_wav("ac4_three.wav", 3);
        REQUIRE(run_cli("ac4-encode " + quoted(tones_wav("ac4_three_me.wav", 6)) + " " +
                            quoted(out) +
                            " 512 experimental=three-zero substream1-content=music-and-effects "
                            "substream2=" +
                            quoted(three) +
                            " substream2-content=dialogue presentation1=1,2 presentation1-config=0",
                        log) == 0);
        const std::vector<std::byte> bytes = read_bytes(out);
        const ac4::Toc toc = first_toc(bytes);
        REQUIRE(toc.substream_groups.size() == 2);
        CHECK(toc.substream_groups[1].substreams.at(0).chan->ch_mode == 2);
        // Without the option, three channels are refused, naming it.
        CHECK(run_cli("ac4-encode " + quoted(three) + " " + quoted(out) + " 192", log) == 2);
        CHECK(read_log(log).find("experimental=three-zero") != std::string::npos);
    }
}

TEST_CASE("ac4-encode refuses substreams and presentations that do not go together naming why",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_refused_presentations.log";
    const fs::path out = dir / "ac4_refused_presentations.ac4";
    const fs::path stereo = tones_wav("ac4_refused_stereo.wav", 2);
    const fs::path mono = wav_of("ac4_refused_mono.wav", {tone(1117.0, kRate)});
    struct Case {
        const char* name;
        std::string args;
        int exit;
        std::string_view says;
    };
    const std::string two = " substream2=" + quoted(mono);
    const std::vector<Case> cases = {
        {"a gap in the substreams", " substream3=" + quoted(mono), 1, "neither an input"},
        {"an input and a waveform", two + " substream2-enhances=1", 1, "takes no input of its own"},
        {"a gap in the presentations", " presentation2=1", 1, "presentation 1 is missing"},
        {"a substream the stream lacks", two + " presentation1=3", 1, "names substream 3"},
        {"a presentation of nothing", " presentation1-id=4", 1, "plays no substream"},
        {"loudness over several substreams",
         two + " presentation1=1,2 presentation1-config=0 loudness=ebu-r128", 1,
         "measure one programme"},
        {"a waveform with its own enhancement",
         " substream2-enhances=1 substream2-dialogue-channels=c", 1,
         "no dialogue enhancement of its own"},
        {"a hybrid method with no dialogue", " dialogue-hybrid=0.5", 1,
         "dialogue-hybrid= is a hybrid method"},
        {"a configuration the encoder refuses", two + " presentation1=1,2 presentation1-config=1",
         1, "a dialogue enhancement position"},
        {"an unknown substream key", " substream2-bogus=1", 1, "unknown substreamN option"},
        {"an unknown presentation key", " presentation1-bogus=1", 1,
         "unknown presentationN option"},
        {"substream 1's input", " substream1=" + quoted(mono), 1,
         "substream 1 is the positional input"},
        {"a content classifier", " substream1-content=news", 1, "a substream's content is"},
        {"a configuration past Table 53", " presentation1=1 presentation1-config=7", 1,
         "Table 53's 0 to 6"},
        {"an EMDF payload", " presentation1=1 presentation1-emdf=0:00", 1, "the id from 1"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        CHECK(run_cli("ac4-encode " + quoted(stereo) + " " + quoted(out) + " 192" + c.args, log) ==
              c.exit);
        const std::string text = read_log(log);
        CAPTURE(text);
        CHECK(text.find(c.says) != std::string::npos);
    }
    // An MP4 sample entry describes a presentation_id up to 511 alone.
    CHECK(run_cli("ac4-encode " + quoted(stereo) + " " + quoted(dir / "ac4_refused.mp4") +
                      " 192 presentation1=1 presentation1-id=600",
                  log) == 1);
    CHECK(read_log(log).find("the MP4 sample entry's dac4 cannot describe") != std::string::npos);
}

TEST_CASE("fmp4 refuses an AC-4 stream whose presentations keep CMAF's rules as not yet fragmented",
          "[cli][ac4]") {
    // One presentation with its presentation_id: what Part 2 Annex H.1.2.1
    // asks. Fragmenting AC-4 is planning/ac4.md's phase I1; the refusal for a
    // configuration 6 presentation is in the EMDF section above.
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_fmp4.log";
    const fs::path out = dir / "ac4_fmp4.ac4";
    REQUIRE(run_cli("ac4-encode " + quoted(tones_wav("ac4_fmp4_stereo.wav", 2)) + " " +
                        quoted(out) + " 128",
                    log) == 0);
    const fs::path fragments = dir / "ac4_fmp4";
    CHECK(run_cli("fmp4 " + quoted(out) + " " + quoted(fragments), log) == 2);
    CHECK(read_log(log).find("fmp4 does not fragment AC-4 yet") != std::string::npos);
    CHECK_FALSE(fs::exists(fragments / "init.mp4"));
}
