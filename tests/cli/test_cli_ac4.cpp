// AC-4 in the rest of ac3cli (planning/ac4.md, phase I1), each run against the
// real binary: transcode between AC-4 and AC-3 or E-AC-3 in both directions
// with the metadata that carries, the presentation it takes and what it
// refuses; qc, levels and loudness of an AC-4 presentation; fmp4, mkv, probe
// and spdif on AC-4; and the help that names it all. The streams are short
// (half a second of tones) because the sanitizer legs run every one of these.
// record, live and monitor need a device: tests/cli/test_cli_live_alsa.cpp
// takes codec=ac4 through software ALSA devices, and
// tests/gui/test_recording_sink.cpp holds RecordingSink's AC-4 containers to
// their one-shot writers.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/io/metadata_edit.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/mixing.hpp"

namespace fs = std::filesystem;

namespace {

// Per this project's per-file test-helper convention (see
// tests/cli/test_cli_containers.cpp, whose shapes these copy).
fs::path scratch_dir() {
    auto dir =
        fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_ac4_" + ac3::test::platform::process_id());
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

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

// A scalar of the pretty-printed probe document, looked up inside the section
// that owns it (tests/cli/test_cli_probe.cpp has the reasoning).
std::string json_field(std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\": ";
    const auto at = document.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const auto start = at + needle.size();
    const auto end = document.find_first_of(",\n}]", start);
    auto value = document.substr(start, end - start);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

std::string json_section(std::string_view document, std::string_view key) {
    const std::string needle = "\"" + std::string{key} + "\": {";
    const auto at = document.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    const auto start = at + needle.size() - 1;
    int depth = 0;
    for (auto i = start; i < document.size(); ++i) {
        depth += document[i] == '{' ? 1 : 0;
        depth -= document[i] == '}' ? 1 : 0;
        if (depth == 0) {
            return std::string{document.substr(start, i - start + 1)};
        }
    }
    return {};
}

// Half a second of a tone on each of `count` channels, each at its own
// frequency, as a WAV file in the scratch directory, at `rate`.
fs::path tones_wav(const std::string& name, std::size_t count, int rate = 48000) {
    const fs::path path = scratch_dir() / name;
    if (!fs::exists(path)) {
        std::vector<std::vector<float>> channels;
        const auto length = static_cast<std::size_t>(rate / 2);
        for (std::size_t c = 0; c < count; ++c) {
            const double hz = 331.0 + 157.0 * static_cast<double>(c);
            std::vector<float> x(length);
            for (std::size_t n = 0; n < length; ++n) {
                x[n] = static_cast<float>(
                    0.2 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate));
            }
            channels.push_back(std::move(x));
        }
        REQUIRE(ac3::io::write_wav_f32(path.string(), channels, static_cast<std::uint32_t>(rate))
                    .has_value());
    }
    return path;
}

// Makes `name` in the scratch directory with one ac3cli run, `args` naming
// its input and options after the output: "<command> <input> OUT <rest>".
fs::path made(const std::string& name, const std::string& command, const fs::path& input,
              const std::string& rest) {
    const fs::path path = scratch_dir() / name;
    const auto log = scratch_dir() / (name + ".log");
    REQUIRE(run_cli(command + " " + quoted(input) + " " + quoted(path) + " " + rest, log) == 0);
    return path;
}

// A 5.1 AC-4 stream whose presentation sends every value transcode carries:
// dialnorm, the DRC profile, and each downmix value, with Lt/Rt preferred.
fs::path metadata_ac4() {
    static const fs::path path =
        made("meta_51.ac4", "ac4-encode", tones_wav("meta_51.wav", 6),
             "192 dialnorm=23.5 drc=music-light lorocmixlev=-1.5 lorosurmixlev=-4.5 ltrtcmixlev=-6 "
             "ltrtsurmixlev=-3 lfemix=-4.5 dmixmod=ltrt");
    return path;
}

fs::path constructed(const std::string& name) {
    return fs::path{AC4DEC_GOLDEN_DIR} / "constructed" / name;
}

fs::path multiplexed() {
    return fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentations-5_1.ac4";
}

// The first syncframe's metadata of an AC-3 or E-AC-3 file.
ac3::io::FrameMetadata first_metadata(const fs::path& path) {
    const auto bytes = read_bytes(path);
    const auto metadata = ac3::io::read_frame_metadata(bytes);
    REQUIRE(metadata.has_value());
    return *metadata;
}

// probe's JSON document for `path`.
std::string probe_json(const fs::path& path) {
    const auto log = scratch_dir() / (path.filename().string() + ".json");
    REQUIRE(run_cli("probe " + quoted(path) + " json=1", log) == 0);
    return read_log(log);
}

}  // namespace

// ---------------------------------------------------------------------------
// transcode
// ---------------------------------------------------------------------------

TEST_CASE("transcode carries an AC-4 presentation's metadata into E-AC-3 and AC-3",
          "[cli][ac4][transcode]") {
    const auto dir = scratch_dir();
    const auto source = metadata_ac4();

    const auto ec3 = dir / "from_ac4.ec3";
    const auto log = dir / "from_ac4.log";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(ec3), log) == 0);
    const auto status = read_log(log);
    INFO(status);
    // -23.5 dBFS to the dB; the profile Part 1 5.7.9.4 names; the downmix.
    CHECK(contains(status, "dialnorm 24 (the source's -23.5 dBFS, to the dB)"));
    CHECK(contains(status, "music-light, the source's drc_eac3_profile"));
    CHECK(contains(status, "decoded without DRC"));
    CHECK(contains(status, "layout 5.1 <- 6 decoded channels"));

    const auto document = probe_json(ec3);
    const auto metadata = json_section(json_section(document, "stream"), "metadata");
    CHECK(json_field(json_section(metadata, "dialnorm_db"), "min") == "-24");
    CHECK(json_field(json_section(metadata, "dynrng"), "present") == "true");
    const auto e = first_metadata(ec3);
    REQUIRE(e.mix.has_value());
    CHECK(e.mix->dmixmod == ac3::meta::DownmixMode::kLtRt);
    CHECK(e.mix->lorocmixlev == ac3::meta::MixLevel::kMinus1_5dB);
    CHECK(e.mix->lorosurmixlev == ac3::meta::MixLevel::kMinus4_5dB);
    CHECK(e.mix->ltrtcmixlev == ac3::meta::MixLevel::kMinus6dB);
    CHECK(e.mix->ltrtsurmixlev == ac3::meta::MixLevel::kMinus3dB);
    // AC-4's -4.5 dB goes half a dB up to E-AC-3's -4 dB: code 14.
    CHECK(e.mix->lfemixlevcod == 14);

    // AC-3's two bsi levels take the preferred pair, Lt/Rt's.
    const auto ac3_out = dir / "from_ac4.ac3";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(ac3_out) + " 384", log) == 0);
    const auto a = first_metadata(ac3_out);
    CHECK(a.dialnorm == 24);
    CHECK(a.cmixlev == ac3::meta::CentreMixLevel::kMinus6dB);
    CHECK(a.surmixlev == ac3::meta::SurroundMixLevel::kMinus3dB);

    // A presentation that sends no DRC gives the re-encode none.
    const auto plain = made("plain_20.ac4", "ac4-encode", tones_wav("plain_20.wav", 2), "96");
    const auto plain_out = dir / "plain_20.ec3";
    REQUIRE(run_cli("transcode " + quoted(plain) + " " + quoted(plain_out), log) == 0);
    CHECK(contains(read_log(log), "DRC      none: the source sends no DRC"));
    const auto plain_metadata =
        json_section(json_section(probe_json(plain_out), "stream"), "metadata");
    CHECK(json_field(json_section(plain_metadata, "dynrng"), "present") == "false");
}

TEST_CASE("transcode sends an AC-3 or E-AC-3 source's metadata to AC-4 and drc= as its profile",
          "[cli][ac4][transcode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "to_ac4.log";
    const auto wav = tones_wav("to_ac4_51.wav", 6);

    const auto ac3_in =
        made("to_ac4.ac3", "encode", wav, "384 51 dialnorm=20 cmixlev=-3 surmixlev=-6");
    const auto from_ac3 = dir / "from_ac3.ac4";
    REQUIRE(run_cli("transcode " + quoted(ac3_in) + " " + quoted(from_ac3) + " drc=film-standard",
                    log) == 0);
    const auto status = read_log(log);
    INFO(status);
    CHECK(contains(status, "AC-4 frames (192 kbps, 48000 Hz)"));
    CHECK(contains(status, "dialnorm -20 dBFS (carried from the source)"));
    CHECK(contains(status, "film-standard (from drc=), as drc_eac3_profile"));
    const auto ac4 = json_section(json_section(probe_json(from_ac3), "stream"), "ac4");
    const auto metadata = json_section(ac4, "metadata");
    CHECK(json_field(json_section(metadata, "loudness"), "dialnorm_dbfs") == "-20.00");
    CHECK(json_field(json_section(metadata, "drc"), "eac3_profile") == "1");
    const auto downmix = json_section(metadata, "downmix");
    CHECK(json_field(downmix, "loro_centre_db") == "-3.0");
    CHECK(json_field(downmix, "loro_surround_db") == "-6.0");
    CHECK(json_field(downmix, "preferred") == "\"not_indicated\"");

    // E-AC-3's mixing metadata: every level, the preference and the LFE,
    // +5 dB going half a dB down to AC-4's +4.5.
    const auto eac3_in = made("to_ac4.ec3", "eac3-encode", wav,
                              "384 none 51 off mixmeta dmixmod=loro lorocmixlev=-1.5 "
                              "lorosurmixlev=-4.5 lfemix=5");
    const auto from_eac3 = dir / "from_eac3.ac4";
    REQUIRE(run_cli("transcode " + quoted(eac3_in) + " " + quoted(from_eac3) + " 256", log) == 0);
    INFO(read_log(log));
    CHECK(contains(read_log(log), "DRC      none"));
    const auto eac3_metadata = json_section(
        json_section(json_section(probe_json(from_eac3), "stream"), "ac4"), "metadata");
    CHECK(json_field(eac3_metadata, "drc") == "null");
    const auto eac3_downmix = json_section(eac3_metadata, "downmix");
    CHECK(json_field(eac3_downmix, "loro_centre_db") == "-1.5");
    CHECK(json_field(eac3_downmix, "loro_surround_db") == "-4.5");
    CHECK(json_field(eac3_downmix, "lfe_db") == "4.5");
    CHECK(json_field(eac3_downmix, "preferred") == "\"lo_ro\"");

    // 3/0 without an LFE becomes AC-4's 5.0 rather than a 5.1 with a
    // silent LFE.
    const auto three = made("to_ac4_30.ac3", "encode", tones_wav("to_ac4_30.wav", 3), "256 L,C,R");
    const auto from_three = dir / "from_30.ac4";
    REQUIRE(run_cli("transcode " + quoted(three) + " " + quoted(from_three) + " 128", log) == 0);
    CHECK(contains(read_log(log), "layout 5.0 <- 3 source channels"));
}

TEST_CASE("transcode folds a 7.X presentation for AC-3 and keeps its pair in E-AC-3",
          "[cli][ac4][transcode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "seven.log";
    const auto source = constructed("7_1-322-simple-config2-sap.ac4");

    const auto ac3_out = dir / "seven.ac3";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(ac3_out), log) == 0);
    CHECK(contains(read_log(log), "(the 7.X element folded to 5.X, Part 1 Table 219)"));
    const auto a = first_metadata(ac3_out);
    CHECK(a.acmod == ac3::Acmod::k3_2);
    CHECK(a.lfe);

    // The top front pair at E-AC-3's vertical heights.
    const auto ec3 = dir / "seven.ec3";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(ec3), log) == 0);
    INFO(read_log(log));
    CHECK(contains(read_log(log), "layout L,C,R,Ls,Rs,Vhl,Vhr,LFE <- 8 decoded channels"));
    const auto probe_log = dir / "seven_probe.log";
    REQUIRE(run_cli("probe " + quoted(ec3), probe_log) == 0);
    CHECK(contains(read_log(probe_log), "L C R Ls Rs Vhl Vhr LFE"));

    // channels=5.1 folds it for E-AC-3 too.
    const auto folded = dir / "seven_folded.ec3";
    REQUIRE(run_cli("transcode " + quoted(source) + " " + quoted(folded) + " channels=5.1", log) ==
            0);
    CHECK(contains(read_log(log), "layout 5.1 <- 6 decoded channels"));
}

TEST_CASE("transcode takes the presentation decode's options choose", "[cli][ac4][transcode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "presentation.log";
    const auto out = dir / "presentation.ec3";
    REQUIRE(run_cli("transcode " + quoted(multiplexed()) + " " + quoted(out) + " presentation=1",
                    log) == 0);
    CHECK(contains(read_log(log), "presentation 1, presentation_id 2 of 17"));
    REQUIRE(run_cli("transcode " + quoted(multiplexed()) + " " + quoted(out) + " presentation-id=2",
                    log) == 0);
    CHECK(contains(read_log(log), "presentation 1, presentation_id 2 of 17"));
}

TEST_CASE("transcode measures an AC-4 source for dialnorm=auto", "[cli][ac4][transcode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "auto.log";
    const auto out = dir / "auto.ac3";
    REQUIRE(
        run_cli("transcode " + quoted(metadata_ac4()) + " " + quoted(out) + " 384 dialnorm=auto",
                log) == 0);
    const auto status = read_log(log);
    INFO(status);
    CHECK(contains(status, "(measured, "));
    // The tones sit well above -23.5 LKFS, so the measured value is not the
    // carried one.
    CHECK(first_metadata(out).dialnorm != 24);
}

TEST_CASE("transcode refuses what AC-4 cannot carry", "[cli][ac4][transcode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "refused.log";
    const auto out = dir / "refused.ac4";
    const auto stereo = made("refused_20.ac3", "encode", tones_wav("refused_20.wav", 2), "192");
    const auto dual = made("refused_11.ac3", "encode", tones_wav("refused_11.wav", 2), "192 1+1");
    const auto slow =
        made("refused_32k.ac3", "encode", tones_wav("refused_32k.wav", 2, 32000), "192");
    struct Row {
        std::string args;
        std::string message;
    };
    for (const Row& row : {
             Row{quoted(metadata_ac4()) + " " + quoted(out), "is AC-4 already"},
             Row{quoted(dual) + " " + quoted(out), "AC-4 has no dual mono"},
             Row{quoted(stereo) + " " + quoted(out) + " heavy", "no AC-4 counterpart"},
             Row{quoted(slow) + " " + quoted(out), "AC-4"},
             Row{quoted(stereo) + " " + quoted(out) + " 192 71", "AC-4 cannot carry 7.1"},
             Row{quoted(metadata_ac4()) + " " + quoted(dir / "refused.ec3") + " dialnorm2=20",
                 "dialnorm2= is a 1+1 stream's Ch2"},
         }) {
        CAPTURE(row.args);
        fs::remove(out);
        CHECK(run_cli("transcode " + row.args, log) == 1);
        const auto text = read_log(log);
        INFO(text);
        CHECK(contains(text, row.message));
        CHECK_FALSE(fs::exists(out));
    }
}

// ---------------------------------------------------------------------------
// qc, levels, loudness
// ---------------------------------------------------------------------------

TEST_CASE("qc levels and loudness measure an AC-4 presentation as coded", "[cli][ac4][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "measure.log";
    const auto source = metadata_ac4();

    REQUIRE(run_cli("qc " + quoted(source), log) == 0);
    auto text = read_log(log);
    INFO(text);
    CHECK(contains(text, "AC-4"));
    CHECK(contains(text, "dialnorm"));

    REQUIRE(run_cli("levels " + quoted(source), log) == 0);
    text = read_log(log);
    CHECK(contains(text, "LFE"));

    REQUIRE(run_cli("loudness " + quoted(source), log) == 0);
    text = read_log(log);
    CHECK(contains(text, "AC-4, presentation 0"));
    CHECK(contains(text, "the stream's dialnorm -23.5 dBFS"));

    // The presentation decode's options choose, here the second.
    REQUIRE(run_cli("loudness " + quoted(multiplexed()) + " presentation=1", log) == 0);
    CHECK(contains(read_log(log), "AC-4, presentation 1"));

    // loudness reads AC-3 and E-AC-3 streams too, beside their own dialnorm.
    const auto ac3_in =
        made("measure.ac3", "encode", tones_wav("measure.wav", 2), "192 dialnorm=27");
    REQUIRE(run_cli("loudness " + quoted(ac3_in), log) == 0);
    CHECK(contains(read_log(log), "AC-3"));
}

// ---------------------------------------------------------------------------
// fmp4, mkv, probe, spdif
// ---------------------------------------------------------------------------

TEST_CASE("fmp4 fragments AC-4 at its I-frames with CMAF brands and manifests",
          "[cli][ac4][fmp4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "fmp4.log";
    const auto source =
        made("fmp4_20.ac4", "ac4-encode", tones_wav("fmp4_20.wav", 2), "96 iframe-interval=4");
    const auto folder = dir / "fmp4_out";
    fs::remove_all(folder);
    REQUIRE(run_cli("fmp4 " + quoted(source) + " " + quoted(folder) + " 6", log) == 0);
    const auto status = read_log(log);
    INFO(status);
    CHECK(contains(status, "brands ca4m and ca4s"));
    const auto init = read_bytes(folder / "init.mp4");
    const std::string init_text(reinterpret_cast<const char*>(init.data()), init.size());
    CHECK(contains(init_text, "ca4m"));
    CHECK(contains(init_text, "ca4s"));
    CHECK(contains(init_text, "dac4"));
    // A fragment closes at the first I-frame once it holds six frames, every
    // eighth frame here, so the half second's frames make two segments.
    CHECK(fs::exists(folder / "segment1.m4s"));
    CHECK(fs::exists(folder / "segment2.m4s"));
    CHECK(contains(read_log(folder / "master.m3u8"), "CODECS=\"ac-4."));
    const auto mpd = read_log(folder / "manifest.mpd");
    CHECK(contains(mpd, "AudioChannelConfiguration"));
    CHECK(contains(mpd, "timescale=\"48000\""));
}

TEST_CASE("mkv refuses AC-4 for want of a Matroska codec ID", "[cli][ac4][mkv]") {
    const auto dir = scratch_dir();
    const auto log = dir / "mkv.log";
    const auto out = dir / "refused.mkv";
    fs::remove(out);
    CHECK(run_cli("mkv " + quoted(metadata_ac4()) + " " + quoted(out), log) == 1);
    CHECK(contains(read_log(log), "Matroska registers no CodecID"));
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("probe reads AC-4 inside MP4 and MPEG-TS and says what the container says",
          "[cli][ac4][probe]") {
    const auto dir = scratch_dir();
    const auto wav = tones_wav("probe_20.wav", 2);
    const auto mp4 = made("probe_20.mp4", "ac4-encode", wav, "96");
    const auto from_mp4 = probe_json(mp4);
    const auto container = json_section(from_mp4, "container");
    INFO(from_mp4);
    CHECK(json_field(container, "format") == "\"mp4\"");
    CHECK(json_field(container, "codec_id") == "\"ac-4\"");
    CHECK(contains(json_section(container, "codec_box"), "\"dac4\""));
    CHECK(json_field(json_section(from_mp4, "stream"), "codec") == "\"ac4\"");

    const auto raw = made("probe_20.ac4", "ac4-encode", wav, "96");
    const auto ts = made("probe_20.ts", "ts", raw, "");
    const auto from_ts = probe_json(ts);
    CHECK(json_field(json_section(from_ts, "container"), "format") == "\"mpegts\"");
    CHECK(json_field(json_section(from_ts, "stream"), "codec") == "\"ac4\"");
    // A bare stream's container is null, as the schema's members are never
    // omitted.
    CHECK(contains(probe_json(metadata_ac4()), "\"container\": null"));
}

TEST_CASE("spdif wraps AC-4 in IEC 61937-14 bursts that unspdif reads back unchanged",
          "[cli][ac4][spdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "spdif.log";
    const auto source = metadata_ac4();
    const auto carrier = dir / "spdif_51.wav";
    const auto back = dir / "spdif_51.ac4";
    REQUIRE(run_cli("spdif " + quoted(source) + " " + quoted(carrier), log) == 0);
    CHECK(contains(read_log(log), "IEC 61937-14"));
    REQUIRE(run_cli("unspdif " + quoted(carrier) + " " + quoted(back), log) == 0);
    CHECK(read_bytes(back) == read_bytes(source));
}

// ---------------------------------------------------------------------------
// help
// ---------------------------------------------------------------------------

TEST_CASE("help names AC-4 for every command that reads or writes it", "[cli][ac4][man]") {
    const auto dir = scratch_dir();
    const auto log = dir / "help.log";
    for (const char* command : {"qc", "levels", "loudness", "transcode", "monitor", "play"}) {
        CAPTURE(command);
        REQUIRE(run_cli(std::string{"help "} + command, log) == 0);
        CHECK(contains(read_log(log),
                       "AC-4 (decode, monitor, play, qc, levels, loudness, transcode)"));
    }
    REQUIRE(run_cli("help record", log) == 0);
    CHECK(contains(read_log(log), "record/live codec=ac4"));
    REQUIRE(run_cli("man", log) == 0);
    const auto page = read_log(log);
    CHECK(contains(page, "out.ac4"));
}
