#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/io/wav.hpp"

// The measurement and carrier commands (apps/cli/commands/analysis.cpp:
// levels, loudness, qc, spdif, unspdif) at the level a user meets them: the
// real binary as a subprocess, its exit code and the report it prints. The
// numbers asserted are the ones a full-scale-referenced sine makes exact -
// a 0.5-amplitude tone peaks at -6.02 dBFS and, on 1 kHz where K-weighting
// is flat enough to round away, reads -6 LKFS - so a report that drifted
// shows up as a wrong number rather than merely a different layout.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention - including
// the Windows double-quote wrapping explained in test_cli.cpp's run_cli.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() {
    return ac3::test::platform::process_id();
}

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_analysis_" + scratch_pid_suffix());
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

std::vector<char> read_raw(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_raw(const fs::path& path, std::span<const char> bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// `channels` channels of a `hz` tone at `amplitude`, `frames` long. 1 kHz by
// default; at 48 kHz that is exactly 48 samples a cycle, so the bytes repeat
// at a fixed period - which once made apps/common/container_input's MPEG-TS
// grid test take such a WAV for a transport stream; the regression test
// below pins that down directly. The loudness tests pass 997 Hz, BS.1770's
// own reference frequency, where K-weighting is exactly 0 dB.
fs::path write_tone_wav(const fs::path& path, std::size_t channels, std::uint32_t rate,
                        std::size_t frames, double amplitude, double hz = 1000.0) {
    std::vector<std::vector<float>> data(channels, std::vector<float>(frames));
    for (auto& channel : data) {
        for (std::size_t n = 0; n < frames; ++n) {
            channel[n] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                                     static_cast<double>(rate)));
        }
    }
    REQUIRE(ac3::io::write_wav_f32(path.string(), data, rate).has_value());
    return path;
}

// Runs a command expected to fail and returns what it printed, after
// checking the exit code.
std::string run_failing(const std::string& args, const fs::path& log, int exit_code) {
    const auto rc = run_cli(args, log);
    auto text = read_log(log);
    INFO(text);
    CHECK(rc == exit_code);
    return text;
}

// The 16-bit words of an IEC 61937 carrier `spdif` wrote, without its
// 44-byte RIFF header - the "raw capture" shape unspdif also takes.
constexpr std::size_t kWavHeaderBytes = 44;
// One AC-3 burst: a 1536-sample frame period of 16-bit stereo words.
constexpr std::size_t kAc3BurstBytes = 1536 * 4;

}  // namespace

TEST_CASE("levels reports a WAV file's own rate, length and A/52 channel order",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto wav = write_tone_wav(dir / "levels_51.wav", 6, 48000, 24000, 0.5);
    const auto log = dir / "levels_51.log";
    const auto rc = run_cli("levels " + quoted(wav), log);
    const auto text = read_log(log);
    INFO(text);
    REQUIRE(rc == 0);
    CHECK(text.find(wav.string() + ": 48000 Hz, 0.50 s, shown in A/52 order as 3/2 + LFE") !=
          std::string::npos);
    CHECK(text.find("per-channel levels (3/2 + LFE):") != std::string::npos);
    // WAV order is FL FR FC LFE BL BR; the report is in A/52's L C R SL SR
    // LFE, each at the tone's -6.02 dBFS peak.
    for (const std::string_view row : {"  L       -6.02", "  C       -6.02", "  R       -6.02",
                                       "  SL      -6.02", "  SR      -6.02", "  LFE     -6.02"}) {
        CAPTURE(row);
        CHECK(text.find(row) != std::string::npos);
    }
}

TEST_CASE("levels refuses a WAV wider than 5.1 and an input it cannot read",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "levels_refused.log";
    const auto wide = write_tone_wav(dir / "levels_7ch.wav", 7, 48000, 480, 0.5);
    CHECK(run_failing("levels " + quoted(wide), log, 2)
              .find("error: levels handles 1 to 6 channels (7 given)") != std::string::npos);
    const auto missing = dir / "levels_missing.wav";
    fs::remove(missing);
    CHECK(run_failing("levels " + quoted(missing), log, 2)
              .find("error: cannot read " + missing.string()) != std::string::npos);
}

// apps/common/container_input's own regression: sniff_container ran its
// MPEG-TS packet-grid test on a WAV, and a valid float WAV `ac3cli decode`
// wrote from a 1 kHz `sine` had five 0x47 bytes exactly 192 bytes apart, so
// levels (and every command that sniffs its input) refused it as "a
// Transport Stream this build cannot demux". This plants both grids the
// sniff knows at their tightest - 0x47 every 188 bytes and every 192 bytes,
// each starting inside the first stride where a real capture's would - into
// a WAV's sample data, on each float's low mantissa byte so the audio stays
// a -6 dBFS tone.
TEST_CASE("levels and qc do not take a WAV with a 0x47 packet grid in it for MPEG-TS",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto wav = write_tone_wav(dir / "levels_ts_grid.wav", 2, 48000, 4800, 0.5);
    auto bytes = read_raw(wav);
    const std::string_view view{bytes.data(), bytes.size()};
    const auto data_tag = view.find("data");
    REQUIRE(data_tag != std::string_view::npos);
    const std::size_t data_at = data_tag + 8;  // past the chunk id and size
    REQUIRE(data_at < 188);
    REQUIRE(data_at % 4 == 0);  // so every multiple-of-4 offset is a low byte
    constexpr int kRuns = 8;    // past the sniff's own five
    for (int i = 0; i < kRuns; ++i) {
        bytes[data_at + (188 * static_cast<std::size_t>(i))] = static_cast<char>(0x47);
        bytes[data_at + 8 + (192 * static_cast<std::size_t>(i))] = static_cast<char>(0x47);
    }
    write_raw(wav, bytes);

    const auto log = dir / "levels_ts_grid.log";
    const auto rc = run_cli("levels " + quoted(wav), log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == 0);
    CHECK(text.find("Transport Stream") == std::string::npos);
    CHECK(text.find("  L       -6.02") != std::string::npos);

    // qc measures a coded stream only, so a WAV is still refused - but as
    // what it is (no syncframe), exit 2, never as an undemuxable container.
    const auto qc_text = run_failing("qc " + quoted(wav), log, 2);
    CHECK(qc_text.find("Transport Stream") == std::string::npos);
}

TEST_CASE("loudness turns a WAV's BS.1770 integrated loudness into the dialnorm it implies",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "loudness.log";

    const auto stereo = write_tone_wav(dir / "loudness_stereo.wav", 2, 48000, 48000, 0.5, 997.0);
    REQUIRE(run_cli("loudness " + quoted(stereo), log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("measured -6.02 LKFS (BS.1770-4, gated) -> dialnorm 6") != std::string::npos);
    CHECK(text.find(stereo.string() + ": 48000 Hz, 2/0 stereo") != std::string::npos);
    CHECK(text.find("  dialogue level -6 LKFS -> dialnorm 6") != std::string::npos);

    // 44.1 and 32 kHz are AC-3 rates too (Table 5.6), so both measure.
    for (const std::uint32_t rate : {44100u, 32000u}) {
        CAPTURE(rate);
        const auto mono =
            write_tone_wav(dir / ("loudness_" + std::to_string(rate) + ".wav"), 1, rate, rate, 0.5);
        REQUIRE(run_cli("loudness " + quoted(mono), log) == 0);
        CHECK(read_log(log).find(mono.string() + ": " + std::to_string(rate) + " Hz, 1/0 mono") !=
              std::string::npos);
    }
}

TEST_CASE("loudness refuses what AC-3 cannot carry and reports silence as undefined",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "loudness_refused.log";

    const auto wide = write_tone_wav(dir / "loudness_7ch.wav", 7, 48000, 4800, 0.5);
    CHECK(run_failing("loudness " + quoted(wide), log, 2)
              .find("error: 7 channels is not an AC-3 layout") != std::string::npos);

    const auto slow = write_tone_wav(dir / "loudness_22k.wav", 2, 22050, 2205, 0.5);
    CHECK(run_failing("loudness " + quoted(slow), log, 2)
              .find("error: sample rate 22050 is not legal for AC-3") != std::string::npos);

    const auto missing = dir / "loudness_missing.wav";
    fs::remove(missing);
    CHECK(run_failing("loudness " + quoted(missing), log, 2)
              .find("error: " + missing.string() + ": cannot open file") != std::string::npos);

    // Nothing above BS.1770's -70 LKFS absolute gate means there is no
    // loudness to turn into a dialnorm - a runtime outcome, not bad input.
    const auto silent = write_tone_wav(dir / "loudness_silent.wav", 2, 48000, 4800, 0.0);
    CHECK(run_failing("loudness " + quoted(silent), log, 5)
              .find("no audio above the -70 LKFS absolute gate: loudness undefined") !=
          std::string::npos);
}

TEST_CASE("levels and qc name a 1+1 stream's two programmes Ch1 and Ch2, AC-3 and E-AC-3 alike",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "dual_mono.log";
    const auto ac3_path = dir / "dual_mono.ac3";
    const auto ec3_path = dir / "dual_mono.ec3";
    REQUIRE(run_cli("sine " + quoted(ac3_path) + " 1 192 1000 50 1+1", log) == 0);
    REQUIRE(run_cli("eac3-sine " + quoted(ec3_path) + " 1 192 1000 50 1+1", log) == 0);

    REQUIRE(run_cli("levels " + quoted(ac3_path), log) == 0);
    auto text = read_log(log);
    INFO(text);
    CHECK(text.find("32 frames, 1+1 dual mono, 192 kbps, 48000 Hz") != std::string::npos);
    CHECK(text.find("  Ch1     -6.02") != std::string::npos);
    CHECK(text.find("  Ch2     -6.02") != std::string::npos);

    REQUIRE(run_cli("levels " + quoted(ec3_path), log) == 0);
    text = read_log(log);
    CHECK(text.find("  Ch1 ") != std::string::npos);
    CHECK(text.find("  Ch2 ") != std::string::npos);

    for (const auto& path : {ac3_path, ec3_path}) {
        CAPTURE(path.string());
        REQUIRE(run_cli("qc " + quoted(path), log) == 0);
        text = read_log(log);
        INFO(text);
        CHECK(text.find("1+1 dual mono, 48000 Hz, 32 ") != std::string::npos);
        // Each channel is its own programme with its own dialnorm (§5.4.2.16),
        // so each gets its own measurement and its own check.
        for (const std::string_view ch : {"Ch1", "Ch2"}) {
            CAPTURE(ch);
            CHECK(text.find(std::string{ch} + ": measured (BS.1770-4 gated") != std::string::npos);
            CHECK(text.find(std::string{ch} + ": embedded metadata:") != std::string::npos);
            CHECK(text.find(std::string{ch} + ": dialnorm check:") != std::string::npos);
        }
        CHECK(text.find("integrated loudness     -9.03 LKFS") != std::string::npos);
        CHECK(text.find("measurement-derived dialnorm would be 9, not 31") != std::string::npos);
    }
}

TEST_CASE("levels and qc refuse a programme the stream does not carry, and qc an unknown preset",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "qc_refused.log";
    const auto ec3_path = dir / "qc_refused.ec3";
    const auto ac3_path = dir / "qc_refused.ac3";
    REQUIRE(run_cli("eac3-sine " + quoted(ec3_path) + " 1 192", log) == 0);
    REQUIRE(run_cli("sine " + quoted(ac3_path) + " 1 192", log) == 0);

    for (const std::string_view command : {"levels", "qc"}) {
        CAPTURE(command);
        CHECK(run_failing(std::string{command} + " " + quoted(ec3_path) + " programme=3", log, 1)
                  .find("error: no programme 3 in this stream (it carries 0)") !=
              std::string::npos);
    }
    CHECK(run_failing("qc " + quoted(ec3_path) + " preset=nope", log, 1)
              .find("error: unknown qc preset 'nope' (ebu-r128-s2 | atsc-a85 | "
                    "atsc-a85-streaming | netflix | apple-music-atmos | all)") !=
          std::string::npos);
    // objects= re-renders an OAMD object layer: AC-3 cannot have one, and
    // 5.1 is not one of the advanced sound system layouts it renders onto.
    CHECK(run_failing("qc " + quoted(ac3_path) + " objects=714", log, 2)
              .find("error: objects= needs an E-AC-3 stream with OAMD, not AC-3") !=
          std::string::npos);
    CHECK(run_failing("qc " + quoted(ec3_path) + " objects=51", log, 2)
              .find("error: objects= needs an advanced sound system layout (71, 512, 514 or "
                    "714)") != std::string::npos);
    CHECK(run_failing("qc " + quoted(ec3_path) + " objects=714", log, 2)
              .find("error: programme 0 carries no dynamic-object-only OAMD") !=
          std::string::npos);
}

TEST_CASE("qc preset=all runs every gate and fails the exit code when any gate fails",
          "[cli][analysis]") {
    const auto dir = scratch_dir();
    const auto log = dir / "qc_all.log";
    const auto ec3_path = dir / "qc_all.ec3";
    REQUIRE(run_cli("eac3-sine " + quoted(ec3_path) + " 1 192", log) == 0);
    // A -6 LKFS tone is far louder than every delivery target.
    const auto text = run_failing("qc " + quoted(ec3_path) + " preset=all", log, 6);
    for (const std::string_view gate :
         {"ebu-r128-s2:", "atsc-a85:", "atsc-a85-streaming:", "netflix:", "apple-music-atmos:"}) {
        CAPTURE(gate);
        CHECK(text.find(gate) != std::string::npos);
    }
    CHECK(text.find("verdict: FAIL") != std::string::npos);
    CHECK(text.find("verdict: PASS") == std::string::npos);
}

TEST_CASE("spdif refuses an unreadable input, a non-AC-3 one and an unwritable output",
          "[cli][analysis][spdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "spdif_refused.log";
    const auto out_path = dir / "spdif_refused.wav";
    const auto missing = dir / "spdif_missing.ac3";
    fs::remove(missing);
    fs::remove(out_path);
    CHECK(run_failing("spdif " + quoted(missing) + " " + quoted(out_path), log, 2)
              .find("error: cannot read " + missing.string()) != std::string::npos);

    // Five bytes cannot hold even a syncinfo.
    const auto tiny = dir / "spdif_tiny.ac3";
    write_raw(tiny, std::string_view{"\x0B\x77\x00\x00\x00", 5});
    CHECK(run_failing("spdif " + quoted(tiny) + " " + quoted(out_path), log, 2)
              .find("error: " + tiny.string() + " is too short to hold a syncframe") !=
          std::string::npos);

    // bsid (the top five bits of byte 5) picks which wrapper judges the
    // rest, so a headerless run of zeroes is refused as whichever codec that
    // byte names - neither frames, since there is no sync word anywhere.
    struct Garbage {
        char byte5;
        std::string_view codec;
    };
    for (const auto& g : {Garbage{'\x40', "AC-3"}, Garbage{'\x80', "E-AC-3"}}) {
        CAPTURE(g.codec);
        std::vector<char> junk(4096, '\0');
        junk[5] = g.byte5;
        const auto in_path = dir / ("spdif_junk_" + std::string{g.codec} + ".bin");
        write_raw(in_path, junk);
        CHECK(run_failing("spdif " + quoted(in_path) + " " + quoted(out_path), log, 2)
                  .find("error: " + in_path.string() + " is not a valid " + std::string{g.codec} +
                        " stream") != std::string::npos);
        CHECK_FALSE(fs::exists(out_path));
    }

    const auto stream = dir / "spdif_source.ac3";
    REQUIRE(run_cli("sine " + quoted(stream) + " 1 192", log) == 0);
    const auto nowhere = dir / "no_such_directory" / "spdif.wav";
    const auto rc = run_cli("spdif " + quoted(stream) + " " + quoted(nowhere), log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == 3);  // the output's fault, not the (valid) input's
    CHECK(text.find("error: cannot open " + nowhere.string() + " for writing") !=
          std::string::npos);
}

TEST_CASE("unspdif recovers the same stream from a bare carrier, and from a RIFF with extra "
          "chunks",
          "[cli][analysis][spdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_shapes.log";
    const auto stream = dir / "unspdif_source.ac3";
    const auto carrier = dir / "unspdif_carrier.wav";
    REQUIRE(run_cli("sine " + quoted(stream) + " 1 192", log) == 0);
    REQUIRE(run_cli("spdif " + quoted(stream) + " " + quoted(carrier), log) == 0);
    const auto original = read_raw(stream);
    const auto wav = read_raw(carrier);
    REQUIRE(wav.size() > kWavHeaderBytes);

    SECTION("a bare carrier with no RIFF header at all") {
        const auto raw = dir / "unspdif_raw.bin";
        write_raw(raw, std::span{wav}.subspan(kWavHeaderBytes));
        const auto out_path = dir / "unspdif_raw.ac3";
        REQUIRE(run_cli("unspdif " + quoted(raw) + " " + quoted(out_path), log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("unwrapped 32 AC-3 bursts -> " + out_path.string() + " (24576 bytes)") !=
              std::string::npos);
        // No RIFF header, so no carrier line to report.
        CHECK(text.find("carrier:") == std::string::npos);
        CHECK(read_raw(out_path) == original);
    }

    SECTION("a RIFF whose data chunk follows a LIST chunk") {
        // An odd-sized chunk is padded to an even length (RIFF's own rule),
        // which the walk has to honour to land on the data chunk.
        std::vector<char> with_list(wav.begin(), wav.begin() + 36);
        const std::string_view list{"LIST\x05\x00\x00\x00" "abcde\x00", 14};
        with_list.insert(with_list.end(), list.begin(), list.end());
        with_list.insert(with_list.end(), wav.begin() + 36, wav.end());
        const auto riff_size = static_cast<std::uint32_t>(with_list.size() - 8);
        std::memcpy(with_list.data() + 4, &riff_size, sizeof riff_size);
        const auto listed = dir / "unspdif_list.wav";
        write_raw(listed, with_list);
        const auto out_path = dir / "unspdif_list.ac3";
        REQUIRE(run_cli("unspdif " + quoted(listed) + " " + quoted(out_path), log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("carrier: 48000 Hz, 2 ch, little-endian words") != std::string::npos);
        CHECK(read_raw(out_path) == original);
    }
}

TEST_CASE("unspdif resyncs past a false preamble and skips a burst of another data type",
          "[cli][analysis][spdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_noise.log";
    const auto stream = dir / "unspdif_noise_source.ac3";
    const auto carrier = dir / "unspdif_noise_carrier.wav";
    REQUIRE(run_cli("sine " + quoted(stream) + " 1 192", log) == 0);
    REQUIRE(run_cli("spdif " + quoted(stream) + " " + quoted(carrier), log) == 0);
    const auto original = read_raw(stream);
    const auto wav = read_raw(carrier);
    const std::span<const char> words = std::span{wav}.subspan(kWavHeaderBytes);

    SECTION("a Pa/Pb preamble with no syncframe behind it") {
        // Pa 0xF872, Pb 0x4E1F (IEC 61937-1's sync words, little-endian),
        // then a Pc claiming AC-3 - but nothing that frames as one.
        std::vector<char> noisy{'\x72', '\xF8', '\x1F', '\x4E', '\x01', '\x00', '\x00', '\x10'};
        noisy.resize(noisy.size() + 64, '\0');
        noisy.insert(noisy.end(), words.begin(), words.end());
        const auto in_path = dir / "unspdif_false_sync.bin";
        write_raw(in_path, noisy);
        const auto out_path = dir / "unspdif_false_sync.ac3";
        REQUIRE(run_cli("unspdif " + quoted(in_path) + " " + quoted(out_path), log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("resynced past 1 preamble pattern(s) with no syncframe behind them") !=
              std::string::npos);
        CHECK(read_raw(out_path) == original);
    }

    SECTION("a burst whose Pc names another data type") {
        // Pc data type 11 (DTS type I in IEC 61937's table), 2048-bit length.
        std::vector<char> mixed{'\x72', '\xF8', '\x1F', '\x4E', '\x0B', '\x00', '\x00', '\x08'};
        mixed.resize(kAc3BurstBytes, '\0');
        mixed.insert(mixed.end(), words.begin(), words.end());
        const auto in_path = dir / "unspdif_other_type.bin";
        write_raw(in_path, mixed);
        const auto out_path = dir / "unspdif_other_type.ac3";
        REQUIRE(run_cli("unspdif " + quoted(in_path) + " " + quoted(out_path), log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("skipped 1 burst(s) of another data type") != std::string::npos);
        CHECK(read_raw(out_path) == original);
    }
}

TEST_CASE("unspdif warns about a carrier cut off mid-payload but keeps the whole bursts",
          "[cli][analysis][spdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_cut.log";
    const auto stream = dir / "unspdif_cut_source.ac3";
    const auto carrier = dir / "unspdif_cut_carrier.wav";
    REQUIRE(run_cli("sine " + quoted(stream) + " 1 192", log) == 0);
    REQUIRE(run_cli("spdif " + quoted(stream) + " " + quoted(carrier), log) == 0);
    const auto wav = read_raw(carrier);
    // Three whole bursts and the first 200 bytes of the fourth's 768-byte
    // payload.
    const auto cut = dir / "unspdif_cut.bin";
    write_raw(cut, std::span{wav}.subspan(kWavHeaderBytes, 3 * kAc3BurstBytes + 200));
    const auto out_path = dir / "unspdif_cut.ac3";
    // quiet does not hide it: the take still finished, but short.
    REQUIRE(run_cli("unspdif " + quoted(cut) + " " + quoted(out_path) + " quiet", log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("warning: burst payload cut off by end of input") != std::string::npos);
    CHECK(text.find("unwrapped") == std::string::npos);
    CHECK(fs::file_size(out_path) == 3u * 768u);
}

TEST_CASE("unspdif refuses an unreadable carrier, plain PCM and an unwritable output",
          "[cli][analysis][spdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_refused.log";
    const auto out_path = dir / "unspdif_refused.ac3";
    const auto missing = dir / "unspdif_missing.wav";
    fs::remove(missing);
    fs::remove(out_path);
    CHECK(run_failing("unspdif " + quoted(missing) + " " + quoted(out_path), log, 2)
              .find("error: cannot read " + missing.string()) != std::string::npos);

    const auto pcm = write_tone_wav(dir / "unspdif_pcm.wav", 2, 48000, 4800, 0.5);
    CHECK(run_failing("unspdif " + quoted(pcm) + " " + quoted(out_path), log, 2)
              .find("error: " + pcm.string() +
                    " holds no AC-3, E-AC-3 or AC-4 bursts - is it ordinary PCM rather than an IEC "
                    "61937 carrier?") != std::string::npos);
    CHECK_FALSE(fs::exists(out_path));

    const auto stream = dir / "unspdif_refused_source.ac3";
    const auto carrier = dir / "unspdif_refused_carrier.wav";
    REQUIRE(run_cli("sine " + quoted(stream) + " 1 192", log) == 0);
    REQUIRE(run_cli("spdif " + quoted(stream) + " " + quoted(carrier), log) == 0);
    const auto nowhere = dir / "no_such_directory" / "unspdif.ac3";
    CHECK(run_failing("unspdif " + quoted(carrier) + " " + quoted(nowhere), log, 3)
              .find("error: cannot open " + nowhere.string() + " for writing") !=
          std::string::npos);
}
