#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "ac3/decoder/decoder.hpp"  // split_frames, to lift the dependent out of a legacy-core unit
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"  // restamp_crc, for the non-uniform-access-unit fixture
#include "ac3/io/wav.hpp"

// apps/cli/commands/containers.cpp measured 0.0% line coverage when roadmap
// VX15 first pointed the apps/cli coverage gate at apps/ (re-measured at
// 30.4% after roadmap IO2's container-reader/probe work landed and
// incidentally exercised some of it - see ROADMAP.md's VX22). mkv/mp4/ts were
// already reached as fixture-building helpers inside test_cli.cpp's demux
// round-trip test, but never asserted on their OWN output; three real
// branches (reject_legacy_core, the non-uniform-access-unit refusal, and
// fmp4's base DASH/HLS path with no fallback-51 companion) were never
// exercised in either direction at all. This file closes both gaps: direct
// assertions on mkv/mp4's own success path, and fixtures built to actually
// hit the three refusal branches containers.cpp's own comments describe but
// nothing had triggered.
//
// Same subprocess plumbing as tests/cli/test_cli.cpp's run_cli - see its
// comment for why the extra outer quote pair is needed on Windows and must
// not appear on POSIX. Duplicated here rather than shared, per this project's
// per-file test-helper convention (see test_cli_stream_tools.cpp).

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_containers";
    fs::create_directories(dir);
    return dir;
}

int child_exit_code(int system_status) {
#ifdef _WIN32
    return system_status;
#else
    if (system_status == -1) {
        return system_status;
    }
    return WIFEXITED(system_status) ? WEXITSTATUS(system_status)
                                    : 128 + WTERMSIG(system_status);
#endif
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return child_exit_code(std::system(wrapped.c_str()));
#else
    return child_exit_code(std::system(command.c_str()));
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<char> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

void append(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// A/52 §E2.3.1.2's legacy-core delivery, built the same way
// tests/io/test_elementary.cpp's own legacy_core_stream() is: an AC-3
// syncframe carrying the 5.1 bed, with the DEPENDENT substream of an ordinary
// E-AC-3 access unit riding immediately behind it. reject_legacy_core (see
// containers.cpp) is the only place any of the three simple writers ever
// looks at StreamKind::kAc3CoreEac3Extension, and nothing before this file
// built a stream that kind to hand it.
std::vector<std::byte> legacy_core_stream() {
    ac3::FrameEncoder core{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame, 0.0F));
    std::vector<std::span<const float>> views;
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }

    ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    config.dependents.push_back({.bitrate_kbps = 224,
                                 .acmod = ac3::Acmod::k2_2,
                                 .chanmap = ac3::eac3::chanmap::k71Rear});
    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());
    const auto frames = ac3::split_frames(unit->bytes);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 2);
    const auto dependent = (*frames)[1];

    std::vector<std::byte> stream;
    for (int f = 0; f < 2; ++f) {
        const auto frame = core.encode_frame(views);
        REQUIRE(frame.has_value());
        append(stream, *frame);
        append(stream, dependent);
    }
    return stream;
}

// A header-level fixture, same recipe as tests/io/test_elementary.cpp's "a
// stream whose access units differ in length has no uniform figure": two
// six-block E-AC-3 access units with a three-block one spliced between them.
// track_samples_per_frame (containers.cpp) refuses every stream like this -
// real per §E2.3.1.4, legal, and nothing this project's own encoders emit -
// but nothing had built one to reach the refusal before this file.
std::vector<std::byte> non_uniform_stream() {
    ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
    const auto unit = ac3::eac3::build_silent_access_unit(config);
    REQUIRE(unit.has_value());
    const auto frame_bytes = unit->bytes.size();

    std::vector<std::byte> stream;
    for (int i = 0; i < 3; ++i) {
        append(stream, unit->bytes);
    }
    // numblkscod sits at bits 34-35 (byte 4, bits 2-3 from the MSB) of an
    // E-AC-3 syncframe - see test_elementary.cpp's set_numblkscod for the
    // same bit math. Code 2 is three blocks, half of the default six.
    auto middle = std::span{stream}.subspan(frame_bytes, frame_bytes);
    auto byte4 = std::to_integer<std::uint8_t>(middle[4]);
    byte4 = static_cast<std::uint8_t>((byte4 & 0xCF) | (0x2u << 4));
    middle[4] = std::byte{byte4};
    REQUIRE(ac3::io::restamp_crc(middle).has_value());
    return stream;
}

}  // namespace

TEST_CASE("mkv reports the access units, layout and bytes it wrote", "[cli][mkv]") {
    const auto dir = scratch_dir();
    const auto log = dir / "mkv_report.log";
    const auto source = dir / "mkv_report.ac3";
    const auto out = dir / "mkv_report.mkv";
    REQUIRE(run_cli("sine " + quoted(source) + " 1 448 440 60 51", log) == 0);

    REQUIRE(run_cli("mkv " + quoted(source) + " " + quoted(out), log) == 0);
    const auto report = read_log(log);
    CHECK(report.find("AC-3") != std::string::npos);
    CHECK(report.find("access units") != std::string::npos);
    CHECK(report.find("3/2") != std::string::npos);  // layout_name(k3_2, lfe=true)
    CHECK(fs::file_size(out) > 0);
}

TEST_CASE("mp4 names the Atmos complexity index only when one was encoded", "[cli][mp4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "mp4_report.log";

    const auto plain = dir / "mp4_plain.ec3";
    const auto plain_out = dir / "mp4_plain.mp4";
    REQUIRE(run_cli("eac3-sine " + quoted(plain) + " 1 192 440 50 stereo", log) == 0);
    REQUIRE(run_cli("mp4 " + quoted(plain) + " " + quoted(plain_out), log) == 0);
    CHECK(read_log(log).find("Atmos complexity") == std::string::npos);
    CHECK(fs::file_size(plain_out) > 0);

    const auto atmos = dir / "mp4_atmos.ec3";
    const auto atmos_out = dir / "mp4_atmos.mp4";
    REQUIRE(run_cli("atmos " + quoted(atmos) + " 1 448 2 4 objects", log) == 0);
    REQUIRE(run_cli("mp4 " + quoted(atmos) + " " + quoted(atmos_out), log) == 0);
    CHECK(read_log(log).find("Atmos complexity") != std::string::npos);
    CHECK(fs::file_size(atmos_out) > 0);
}

TEST_CASE("mkv, mp4 and ts refuse an AC-3 core with E-AC-3 extension substreams",
          "[cli][mkv][mp4][ts]") {
    const auto dir = scratch_dir();
    const auto log = dir / "legacy_core.log";
    const auto source = dir / "legacy_core.ec3";
    write_bytes(source, legacy_core_stream());

    // ac3::io::scan reads this kind off the first two syncframes regardless
    // of which command asks - one shared fixture, three refusals.
    CHECK(run_cli("mkv " + quoted(source) + " " + quoted(dir / "legacy_core.mkv"), log) == 2);
    CHECK(read_log(log).find("AC-3 core with E-AC-3 extension") != std::string::npos);

    CHECK(run_cli("mp4 " + quoted(source) + " " + quoted(dir / "legacy_core.mp4"), log) == 2);
    CHECK(read_log(log).find("AC-3 core with E-AC-3 extension") != std::string::npos);

    CHECK(run_cli("ts " + quoted(source) + " " + quoted(dir / "legacy_core.ts"), log) == 2);
    CHECK(read_log(log).find("AC-3 core with E-AC-3 extension") != std::string::npos);
}

TEST_CASE("mkv, mp4 and ts refuse a stream whose access units differ in length",
          "[cli][mkv][mp4][ts]") {
    const auto dir = scratch_dir();
    const auto log = dir / "non_uniform.log";
    const auto source = dir / "non_uniform.ec3";
    write_bytes(source, non_uniform_stream());

    CHECK(run_cli("mkv " + quoted(source) + " " + quoted(dir / "non_uniform.mkv"), log) != 0);
    CHECK(read_log(log).find("not all the same length") != std::string::npos);

    CHECK(run_cli("mp4 " + quoted(source) + " " + quoted(dir / "non_uniform.mp4"), log) != 0);
    CHECK(read_log(log).find("not all the same length") != std::string::npos);

    CHECK(run_cli("ts " + quoted(source) + " " + quoted(dir / "non_uniform.ts"), log) != 0);
    CHECK(read_log(log).find("not all the same length") != std::string::npos);
}

TEST_CASE("fmp4 writes the base DASH/HLS rendition with no fallback-51 companion",
          "[cli][fmp4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "fmp4_base.log";
    const auto source = dir / "fmp4_base.ac3";
    const auto out_dir = dir / "fmp4_base_out";
    fs::remove_all(out_dir);
    REQUIRE(run_cli("sine " + quoted(source) + " 1 448 440 60 51", log) == 0);

    REQUIRE(run_cli("fmp4 " + quoted(source) + " " + quoted(out_dir) + " 4", log) == 0);

    CHECK(fs::exists(out_dir / "init.mp4"));
    CHECK(fs::exists(out_dir / "segment1.m4s"));
    CHECK(fs::exists(out_dir / "audio.m3u8"));
    CHECK(fs::exists(out_dir / "master.m3u8"));
    CHECK(fs::exists(out_dir / "manifest.mpd"));
    // No fallback-51 asked for, and this source carries no object layer for
    // it to strip either way - no bed51/ companion in either case.
    CHECK_FALSE(fs::exists(out_dir / "bed51"));

    std::ifstream master_in{out_dir / "master.m3u8", std::ios::binary};
    const std::string master{std::istreambuf_iterator<char>{master_in},
                             std::istreambuf_iterator<char>{}};
    CHECK(master.find("audio.m3u8") != std::string::npos);
    CHECK(master.find("bed51") == std::string::npos);
}

TEST_CASE("mkv warns and keeps only the first programme a stream carries", "[cli][mkv]") {
    const auto dir = scratch_dir();
    const auto log = dir / "multi_programme.log";

    // eac3-encode needs real WAV sources, one per programme - see
    // docs/cli/metadata-options.md's "Programme options" section for the
    // programme2=/-layout=/-bitrate= tokens this builds with.
    const auto primary_wav = dir / "programme0.wav";
    const auto commentary_wav = dir / "programme1.wav";
    const std::vector<std::vector<float>> primary(6, std::vector<float>(48000, 0.0F));
    const std::vector<float> commentary(48000, 0.0F);
    REQUIRE(ac3::io::write_wav_f32(primary_wav.string(), primary, 48000).has_value());
    REQUIRE(ac3::io::write_wav_f32(commentary_wav.string(),
                                   std::vector<std::vector<float>>{commentary}, 48000)
                .has_value());

    const auto multi = dir / "multi_programme.ec3";
    REQUIRE(run_cli("eac3-encode " + quoted(primary_wav) + " " + quoted(multi) +
                        " 448 none 51 off programme2=" + quoted(commentary_wav) +
                        " programme2-layout=mono programme2-bitrate=96",
                    log) == 0);

    const auto out = dir / "multi_programme.mkv";
    REQUIRE(run_cli("mkv " + quoted(multi) + " " + quoted(out), log) == 0);
    const auto report = read_log(log);
    CHECK(report.find("2 programmes") != std::string::npos);
    CHECK(report.find("only programme 0 is muxed") != std::string::npos);
    CHECK(fs::file_size(out) > 0);
}

TEST_CASE("demux reports what each container told it, sample rate included or not",
          "[cli][demux]") {
    const auto dir = scratch_dir();
    const auto log = dir / "demux_report.log";
    const auto source = dir / "demux_report.ac3";
    REQUIRE(run_cli("sine " + quoted(source) + " 1 192 440 60 stereo", log) == 0);

    SECTION("Matroska carries a sample rate") {
        const auto mkv = dir / "demux_report.mkv";
        REQUIRE(run_cli("mkv " + quoted(source) + " " + quoted(mkv), log) == 0);
        REQUIRE(run_cli("demux " + quoted(mkv) + " " + quoted(dir / "demux_report_mkv.back"), log) ==
                0);
        const auto report = read_log(log);
        CHECK(report.find("48000 Hz") != std::string::npos);
        CHECK(report.find("access units") != std::string::npos);
    }

    SECTION("MP4 carries a sample rate") {
        const auto mp4 = dir / "demux_report.mp4";
        REQUIRE(run_cli("mp4 " + quoted(source) + " " + quoted(mp4), log) == 0);
        REQUIRE(run_cli("demux " + quoted(mp4) + " " + quoted(dir / "demux_report_mp4.back"), log) ==
                0);
        CHECK(read_log(log).find("48000 Hz") != std::string::npos);
    }

    SECTION("MPEG-TS names the codec but not a sample rate") {
        const auto ts = dir / "demux_report.ts";
        REQUIRE(run_cli("ts " + quoted(source) + " " + quoted(ts), log) == 0);
        REQUIRE(run_cli("demux " + quoted(ts) + " " + quoted(dir / "demux_report_ts.back"), log) ==
                0);
        const auto report = read_log(log);
        CHECK(report.find("PES payloads") != std::string::npos);
        CHECK(report.find("Hz") == std::string::npos);
    }
}
