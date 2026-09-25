#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "ac3/decoder/decoder.hpp"  // split_frames, to lift the dependent out of a legacy-core unit
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"  // restamp_crc, for the non-uniform-access-unit fixture
#include "ac3/io/wav.hpp"
#include "ac4/ac4.hpp"

// apps/cli/commands/containers.cpp measured 0.0% line coverage when roadmap
// VX15 first pointed the apps/cli coverage gate at apps/ (re-measured at
// 30.4% after container readers (mkv/mp4/ts)'s container-reader/probe work landed and
// incidentally exercised some of it - see CLI container command tests). mkv/mp4/ts were
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

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_containers_" + scratch_pid_suffix());
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
    std::vector<std::vector<float>> pcm(
        6, std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame), 0.0F));
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
    // docs/forge/cli/metadata-options.md's "Programme options" section for the
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

// --------------------------------------------------------------------------
// AC-4 carriage (AC-4 bitstream inspector): the real DEE fixture through mp4 and ts, and
// back out through demux.

namespace {
fs::path ac4_fixture() {
    return fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-stereo-64" / "dee.ac4";
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    std::vector<char> chars{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(chars.size());
    std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}
}  // namespace

TEST_CASE("mp4 and ts carry a real AC-4 stream, and demux round-trips it",
          "[cli][mp4][ts][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_carriage.log";
    const auto mp4_out = dir / "dee.mp4";
    const auto ts_out = dir / "dee.ts";

    REQUIRE(run_cli("mp4 " + quoted(ac4_fixture()) + " " + quoted(mp4_out), log) == 0);
    auto report = read_log(log);
    CHECK(report.find("AC-4") != std::string::npos);
    CHECK(report.find("2048 samples/frame") != std::string::npos);
    CHECK(report.find("ac-4.02.01.00") != std::string::npos);  // Annex E.13's codecs string
    CHECK(fs::file_size(mp4_out) > 0);

    REQUIRE(run_cli("ts " + quoted(ac4_fixture()) + " " + quoted(ts_out), log) == 0);
    report = read_log(log);
    CHECK(report.find("AC-4 syncframes") != std::string::npos);
    CHECK(report.find("DVB profile") != std::string::npos);

    // A PES payload carries whole syncframes, so the TS direction is
    // byte-identical to the source elementary stream.
    const auto ts_rt = dir / "rt_ts.ac4";
    REQUIRE(run_cli("demux " + quoted(ts_out) + " " + quoted(ts_rt), log) == 0);
    const auto source_bytes = read_file(ac4_fixture());
    CHECK(read_file(ts_rt) == source_bytes);

    // An ISOBMFF sample is the raw_ac4_frame alone - the sync wrapper and
    // the fixture's CRC words are container-dropped by design - so the MP4
    // direction re-frames with the no-CRC sync word: same frame count, same
    // payload bytes, two bytes per frame shorter, and ac4::scan parses it
    // cleanly end to end.
    const auto mp4_rt = dir / "rt_mp4.ac4";
    REQUIRE(run_cli("demux " + quoted(mp4_out) + " " + quoted(mp4_rt), log) == 0);
    const auto reframed = read_file(mp4_rt);
    const auto scanned = ac4::scan(reframed);
    const auto original = ac4::scan(source_bytes);
    REQUIRE_FALSE(scanned.stopped_at.has_value());
    REQUIRE(scanned.frames.size() == original.frames.size());
    for (std::size_t i = 0; i < scanned.frames.size(); ++i) {
        CAPTURE(i);
        const auto& a = scanned.frames[i].raw_ac4_frame;
        const auto& b = original.frames[i].raw_ac4_frame;
        CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
    }
}

TEST_CASE("decode reads raw AC-4 and AC-4 in MP4 to the same PCM", "[cli][mp4][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode.log";
    // SIMPLE stereo, one tone per channel: what ac4::Decoder decodes today.
    const fs::path stream = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-tones-192" / "dee.ac4";
    const auto raw_wav = dir / "ac4_raw.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(raw_wav), log) == 0);
    const auto report = read_log(log);
    CHECK(report.find("decoded 120 AC-4 frames") != std::string::npos);
    CHECK(report.find("(L R, 48000 Hz)") != std::string::npos);
    const auto raw = ac3::io::read_wav(raw_wav.string());
    REQUIRE(raw.has_value());
    CHECK(raw->sample_rate == 48000);
    REQUIRE(raw->channels.size() == 2);
    CHECK(raw->frame_count() == 120U * 2048U);

    // The same frames in an MP4 file, whose samples decode re-frames.
    const auto mp4_out = dir / "ac4_tones.mp4";
    REQUIRE(run_cli("mp4 " + quoted(stream) + " " + quoted(mp4_out), log) == 0);
    const auto mp4_wav = dir / "ac4_mp4.wav";
    REQUIRE(run_cli("decode " + quoted(mp4_out) + " " + quoted(mp4_wav), log) == 0);
    const auto from_mp4 = ac3::io::read_wav(mp4_wav.string());
    REQUIRE(from_mp4.has_value());
    CHECK(from_mp4->channels == raw->channels);
}

TEST_CASE(
    "decode writes AC-4's ASPX mode, 5.1 and A-CPL, and 25 fps through the sample rate converter",
    "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode_aspx.log";
    const auto out = dir / "ac4_aspx.wav";
    REQUIRE(run_cli("decode " + quoted(ac4_fixture()) + " " + quoted(out), log) == 0);
    const auto decoded = ac3::io::read_wav(out.string());
    REQUIRE(decoded.has_value());
    CHECK(decoded->channels.size() == 2);
    CHECK(decoded->frame_count() % 2048 == 0);

    // 5.1 in SIMPLE mode: six channels, in WAV order.
    const auto five_one_log = dir / "ac4_decode_51.log";
    const auto five_one_wav = dir / "ac4_51.wav";
    const fs::path five_one = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-51-music-384" / "dee.ac4";
    REQUIRE(run_cli("decode " + quoted(five_one) + " " + quoted(five_one_wav), five_one_log) == 0);
    CHECK(read_log(five_one_log).find("(L R C LFE Ls Rs, 48000 Hz)") != std::string::npos);
    const auto decoded_51 = ac3::io::read_wav(five_one_wav.string());
    REQUIRE(decoded_51.has_value());
    CHECK(decoded_51->channels.size() == 6);

    // 5.1 in ASPX_ACPL_2: A-CPL makes the surrounds of the front pair.
    const auto acpl_log = dir / "ac4_decode_acpl.log";
    const auto acpl_wav = dir / "ac4_acpl.wav";
    const fs::path acpl = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-51-music-128" / "dee.ac4";
    REQUIRE(run_cli("decode " + quoted(acpl) + " " + quoted(acpl_wav), acpl_log) == 0);
    const auto decoded_acpl = ac3::io::read_wav(acpl_wav.string());
    REQUIRE(decoded_acpl.has_value());
    CHECK(decoded_acpl->channels.size() == 6);

    // 25 frames a second, through the sample rate converter: 1 920 samples a
    // frame at 48 kHz.
    const auto ims_log = dir / "ac4_decode_ims25.log";
    const auto ims_wav = dir / "ac4_ims25.wav";
    const fs::path ims = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-ims-music-128-25" / "dee.ac4";
    REQUIRE(run_cli("decode " + quoted(ims) + " " + quoted(ims_wav), ims_log) == 0);
    const auto decoded_ims = ac3::io::read_wav(ims_wav.string());
    REQUIRE(decoded_ims.has_value());
    CHECK(decoded_ims->sample_rate == 48000);
    REQUIRE(decoded_ims->channels.size() == 2);
    CHECK(decoded_ims->channels[0].size() % 1920 == 0);
}

TEST_CASE("decode takes AC-4 to output-level= and compresses it in drcmode='s mode", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode_level.log";
    const fs::path stream =
        fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-tones-192" / "dee.ac4";
    const auto rms_db = [](const std::vector<float>& x) {
        double sum = 0.0;
        for (std::size_t n = 16384; n < x.size() - 4096; ++n) {
            sum += static_cast<double>(x[n]) * static_cast<double>(x[n]);
        }
        return 10.0 * std::log10(sum / static_cast<double>(x.size() - 20480));
    };
    // Two output levels 12 dB apart, 2^(12 / 6) apart in the output whatever
    // the stream's dialnorm (ETSI TS 103 190-1 5.7.9.3.3).
    const auto low_wav = dir / "ac4_level_31.wav";
    const auto high_wav = dir / "ac4_level_19.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(low_wav) +
                        " output-level=-31 drcmode=off",
                    log) == 0);
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(high_wav) +
                        " output-level=-19 drcmode=off",
                    log) == 0);
    const auto low = ac3::io::read_wav(low_wav.string());
    const auto high = ac3::io::read_wav(high_wav.string());
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    CHECK(std::abs(rms_db(high->channels[0]) - rms_db(low->channels[0]) - 20.0 * std::log10(4.0)) <
          0.01);
    // A mode at a level decodes; without a level AC-4's DRC has nothing to
    // work to, and an output level above full scale is no level.
    const auto drc_wav = dir / "ac4_drc.wav";
    CHECK(run_cli("decode " + quoted(stream) + " " + quoted(drc_wav) +
                      " output-level=-10 drcmode=portable-headphones",
                  log) == 0);
    CHECK(run_cli("decode " + quoted(stream) + " " + quoted(drc_wav) + " drcmode=home-theatre",
                  log) == 1);
    CHECK(read_log(log).find("output-level=") != std::string::npos);
    CHECK(run_cli("decode " + quoted(stream) + " " + quoted(drc_wav) + " output-level=5", log) ==
          1);
}

TEST_CASE("decode takes AC-4's presentation by presentation-id= and language=, mixed at dialogue-gain=",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode_presentation.log";
    // tests/golden/ac4dec/presentations/presentations-5_1.ac4: presentation_id
    // 1 is 5.1 music and effects with English dialogue, 2 the same with
    // German, 21 the English dialogue alone (tests/ac4dec/
    // test_ac4dec_presentations.cpp).
    const fs::path stream = fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentations-5_1.ac4";
    const auto mixed_wav = dir / "ac4_mixed.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(mixed_wav) + " presentation-id=1", log) == 0);
    CHECK(read_log(log).find("presentation 0 (presentation_id 1)") != std::string::npos);
    const auto mixed = ac3::io::read_wav(mixed_wav.string());
    REQUIRE(mixed.has_value());
    CHECK(mixed->channels.size() == 6);
    const auto quiet_wav = dir / "ac4_mixed_quiet.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(quiet_wav) + " presentation-id=1 dialogue-gain=-130",
                    log) == 0);
    CHECK(read_log(log).find("dialogue substreams at -130 dB") != std::string::npos);
    const auto quiet = ac3::io::read_wav(quiet_wav.string());
    REQUIRE(quiet.has_value());
    // The dialogue goes to L at 330 degrees: silenced, L loses energy; the
    // other channels are the music and effects alone either way.
    const auto energy = [](const std::vector<float>& x) {
        double sum = 0.0;
        for (const float v : x) {
            sum += static_cast<double>(v) * static_cast<double>(v);
        }
        return sum;
    };
    CHECK(energy(quiet->channels[0]) < energy(mixed->channels[0]));
    CHECK(energy(quiet->channels[1]) == energy(mixed->channels[1]));
    const auto german_wav = dir / "ac4_german.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(german_wav) + " language=de", log) == 0);
    CHECK(read_log(log).find("presentation 1 (presentation_id 2)") != std::string::npos);
    const auto alone_wav = dir / "ac4_dialogue_alone.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(alone_wav) + " presentation=11", log) == 0);
    const auto alone = ac3::io::read_wav(alone_wav.string());
    REQUIRE(alone.has_value());
    CHECK(alone->channels.size() == 1);
}

TEST_CASE("decode raises AC-4's dialogue by dialogue-enhancement=", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode_de.log";
    // DEE's speech stream sends dialogue enhancement parameters for L and R,
    // capped at 9 dB.
    const fs::path stream =
        fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-speech-128" / "dee.ac4";
    const auto plain_wav = dir / "ac4_de_plain.wav";
    const auto raised_wav = dir / "ac4_de_raised.wav";
    REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(plain_wav), log) == 0);
    REQUIRE(
        run_cli("decode " + quoted(stream) + " " + quoted(raised_wav) + " dialogue-enhancement=9",
                log) == 0);
    const auto plain = ac3::io::read_wav(plain_wav.string());
    const auto raised = ac3::io::read_wav(raised_wav.string());
    REQUIRE(plain.has_value());
    REQUIRE(raised.has_value());
    const auto energy = [](const std::vector<float>& x) {
        double sum = 0.0;
        for (const float v : x) {
            sum += static_cast<double>(v) * static_cast<double>(v);
        }
        return sum;
    };
    CHECK(10.0 * std::log10(energy(raised->channels[0]) / energy(plain->channels[0])) > 1.0);
}

TEST_CASE("decode folds AC-4 5.1 to stereo and mono with channels= and downmix=", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode_downmix.log";
    const fs::path stream =
        fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-51-tones-384" / "dee.ac4";
    struct Case {
        const char* options;
        std::size_t channels;
        const char* layout;
    };
    constexpr std::array<Case, 3> kCases{{
        {"downmix=loro", 2, "(L R, 48000 Hz)"},
        {"channels=2", 2, "(L R, 48000 Hz)"},
        {"channels=1", 1, "(C, 48000 Hz)"},
    }};
    for (const Case& c : kCases) {
        CAPTURE(c.options);
        const auto wav = dir / "ac4_downmix.wav";
        REQUIRE(run_cli("decode " + quoted(stream) + " " + quoted(wav) + " " + c.options, log) ==
                0);
        CHECK(read_log(log).find(c.layout) != std::string::npos);
        const auto decoded = ac3::io::read_wav(wav.string());
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels.size() == c.channels);
    }
}

TEST_CASE("decode stops on a damaged AC-4 frame and conceal= carries on through it", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_decode_conceal.log";
    // DEE's stereo tones with the eleventh frame's audio_size_value set past its
    // audio substream: the table of contents still reads, the substream does not.
    std::vector<std::byte> bytes =
        read_file(fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / "ac4-20-tones-192" / "dee.ac4");
    const ac4::ScanResult scan = ac4::scan(bytes);
    REQUIRE(scan.frames.size() == 120);
    const std::span<const std::byte> raw = scan.frames[10].raw_ac4_frame;
    const auto parsed = ac4::parse_raw_frame(raw);
    REQUIRE(parsed.has_value());
    const auto audio = std::ranges::find_if(parsed->substreams,
                                            [](const ac4::Substream& s) { return s.is_audio; });
    REQUIRE(audio != parsed->substreams.end());
    const auto at = static_cast<std::size_t>(raw.data() - bytes.data()) + audio->offset;
    bytes[at] = std::byte{0xFF};
    bytes[at + 1] = std::byte{0xFE};
    const auto damaged = dir / "ac4_damaged.ac4";
    write_bytes(damaged, bytes);

    const auto wav = dir / "ac4_conceal.wav";
    CHECK(run_cli("decode " + quoted(damaged) + " " + quoted(wav), log) == 2);
    CHECK(read_log(log).find("frame 11") != std::string::npos);
    for (const char* policy : {"repeat", "mute"}) {
        CAPTURE(policy);
        REQUIRE(run_cli("decode " + quoted(damaged) + " " + quoted(wav) + " conceal=" + policy,
                        log) == 0);
        CHECK(read_log(log).find("1 of them concealed") != std::string::npos);
        const auto decoded = ac3::io::read_wav(wav.string());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 2);
        CHECK(decoded->channels[0].size() == 120 * 2048);
    }
}

TEST_CASE("ac4-encode writes raw AC-4 and AC-4 in MP4 that decode reads back", "[cli][mp4][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode.log";
    // Two seconds of a tone per channel, 20 dB under full scale.
    constexpr std::size_t kLength = 96000;
    std::vector<std::vector<float>> channels(2, std::vector<float>(kLength));
    for (std::size_t i = 0; i < kLength; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        channels[0][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * t));
        channels[1][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 3000.0 * t));
    }
    const auto wav_in = dir / "ac4_tones_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_in.string(), channels, 48000).has_value());

    const auto raw_out = dir / "ac4_encoded.ac4";
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(raw_out) + " 192", log) == 0);
    CHECK(read_log(log).find("raw with CRC") != std::string::npos);
    const auto raw_bytes = read_file(raw_out);
    const auto scanned = ac4::scan(raw_bytes);
    REQUIRE_FALSE(scanned.frames.empty());
    CHECK_FALSE(scanned.stopped_at.has_value());
    for (const ac4::SyncFrame& frame : scanned.frames) {
        CHECK(frame.sync_word == 0xAC41);
        CHECK(frame.crc_ok.value_or(false));
        CHECK(frame.raw_ac4_frame.size() == 1024U);  // 192 kbps at 2 048 samples a frame
    }

    // Our decoder reads it back: the input, 4 385 samples later (the
    // encoder's 3 072, and the decoder's 1 313: Table 188's 352, the QMF
    // banks' 577 and six QMF slots), well above the coding noise.
    const auto raw_wav = dir / "ac4_encoded_raw.wav";
    REQUIRE(run_cli("decode " + quoted(raw_out) + " " + quoted(raw_wav), log) == 0);
    const auto decoded = ac3::io::read_wav(raw_wav.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 2);
    REQUIRE(decoded->frame_count() >= kLength + 4385);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        double signal = 0.0;
        double noise = 0.0;
        for (std::size_t i = 0; i < kLength; ++i) {
            const double error =
                static_cast<double>(decoded->channels[ch][i + 4385]) - static_cast<double>(channels[ch][i]);
            signal += static_cast<double>(channels[ch][i]) * static_cast<double>(channels[ch][i]);
            noise += error * error;
        }
        CAPTURE(ch, signal, noise);
        CHECK(10.0 * std::log10(signal / noise) > 30.0);
    }

    // The same encode into an MP4 file decodes to the same PCM.
    const auto mp4_out = dir / "ac4_encoded.mp4";
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(mp4_out) + " 192", log) == 0);
    CHECK(read_log(log).find("codecs ac-4.02.01.") != std::string::npos);
    const auto mp4_wav = dir / "ac4_encoded_mp4.wav";
    REQUIRE(run_cli("decode " + quoted(mp4_out) + " " + quoted(mp4_wav), log) == 0);
    const auto from_mp4 = ac3::io::read_wav(mp4_wav.string());
    REQUIRE(from_mp4.has_value());
    CHECK(from_mp4->channels == decoded->channels);
}

TEST_CASE("ac4-encode codes the ASPX mode below 96 kbps a channel, or as codec-mode= says", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode_aspx.log";
    // A second of a 1 kHz tone per channel, under every crossover.
    constexpr std::size_t kLength = 48000;
    std::vector<std::vector<float>> channels(2, std::vector<float>(kLength));
    for (std::size_t i = 0; i < kLength; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        channels[0][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * t));
        channels[1][i] = channels[0][i];
    }
    const auto wav_in = dir / "ac4_aspx_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_in.string(), channels, 48000).has_value());
    const auto out = dir / "ac4_aspx.ac4";
    struct Run {
        const char* args;
        const char* mode;
    };
    for (const Run run : {Run{" 64", "ASPX mode"}, Run{" 64 codec-mode=simple", "SIMPLE mode"},
                          Run{" 192", "SIMPLE mode"}, Run{" 192 codec-mode=aspx", "ASPX mode"}}) {
        CAPTURE(run.args);
        REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(out) + run.args, log) == 0);
        CHECK(read_log(log).find(run.mode) != std::string::npos);
        const auto wav_out = dir / "ac4_aspx_out.wav";
        REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(wav_out), log) == 0);
        const auto decoded = ac3::io::read_wav(wav_out.string());
        REQUIRE(decoded.has_value());
        double signal = 0.0;
        double noise = 0.0;
        for (std::size_t i = 4800; i + 4800 < kLength; ++i) {
            const double error = static_cast<double>(decoded->channels[0][i + 4385]) - static_cast<double>(channels[0][i]);
            signal += static_cast<double>(channels[0][i]) * static_cast<double>(channels[0][i]);
            noise += error * error;
        }
        CHECK(10.0 * std::log10(signal / noise) > 25.0);
    }
    CHECK(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(out) + " 64 codec-mode=acpl", log) != 0);
    CHECK(read_log(log).find("codec-mode") != std::string::npos);
}

TEST_CASE("ac4-encode codes 5.1 in the A-CPL modes at DEE's rates, and the experimental ones when asked",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode_acpl.log";
    constexpr std::size_t kLength = 48000;
    const auto write_tones = [&](std::size_t count, const fs::path& path) {
        std::vector<std::vector<float>> channels(count, std::vector<float>(kLength));
        for (std::size_t c = 0; c < count; ++c) {
            for (std::size_t i = 0; i < kLength; ++i) {
                channels[c][i] = static_cast<float>(
                    0.1 * std::sin(2.0 * std::numbers::pi * (500.0 + 350.0 * static_cast<double>(c)) *
                                   static_cast<double>(i) / 48000.0));
            }
        }
        REQUIRE(ac3::io::write_wav_f32(path.string(), channels, 48000).has_value());
    };
    const auto five_one = dir / "ac4_acpl_51.wav";
    const auto stereo = dir / "ac4_acpl_20.wav";
    write_tones(6, five_one);
    write_tones(2, stereo);
    const auto out = dir / "ac4_acpl.ac4";
    struct Run {
        const fs::path* in;
        const char* args;
        const char* mode;
    };
    for (const Run run : {Run{&five_one, " 128", "ASPX_ACPL_2 mode"}, Run{&five_one, " 96", "ASPX_ACPL_3 mode"},
                          Run{&five_one, " 160 codec-mode=aspx-acpl-1 experimental=acpl", "ASPX_ACPL_1 mode"},
                          Run{&stereo, " 48 codec-mode=aspx-acpl-2 experimental=acpl", "ASPX_ACPL_2 mode"}}) {
        CAPTURE(run.args);
        REQUIRE(run_cli("ac4-encode " + quoted(*run.in) + " " + quoted(out) + run.args, log) == 0);
        CHECK(read_log(log).find(run.mode) != std::string::npos);
        const auto wav_out = dir / "ac4_acpl_out.wav";
        REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(wav_out), log) == 0);
    }
    // Stereo A-CPL only with experimental=acpl.
    CHECK(run_cli("ac4-encode " + quoted(stereo) + " " + quoted(out) + " 48 codec-mode=aspx-acpl-2", log) != 0);
}

namespace {

// The amplitude of a channel's component at `hz` over `count` samples from
// `first`, through a Hann window.
double tone_amplitude(const std::vector<float>& x, std::size_t first, std::size_t count, double hz) {
    double re = 0.0;
    double im = 0.0;
    double weight = 0.0;
    for (std::size_t n = 0; n < count && first + n < x.size(); ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(count));
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(first + n) / 48000.0;
        re += w * static_cast<double>(x[first + n]) * std::cos(phase);
        im -= w * static_cast<double>(x[first + n]) * std::sin(phase);
        weight += w;
    }
    return 2.0 * std::hypot(re, im) / weight;
}

}  // namespace

TEST_CASE("ac4-encode takes 5.1 and 7.1 in the WAV order decode writes them in", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode_multichannel.log";
    // A tone per channel, at gen_ac4_baseline.py's frequencies: in WAV order
    // FL FR FC LFE BL BR, and SL SR for 7.1, whose BL BR are its back pair.
    constexpr std::array<double, 8> kHz = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0, 1117.0, 1289.0};
    constexpr std::size_t kLength = 96000;
    struct Run {
        std::size_t channels;
        const char* args;
        const char* layout;
    };
    for (const Run run : {Run{6, " 384", "5.1, 384 kbps"}, Run{6, " 192", "5.1, 192 kbps"},
                          Run{8, " 640 experimental=7x-back", "7.1, 3/4/0"}}) {
        CAPTURE(run.channels, run.args);
        std::vector<std::vector<float>> channels(run.channels, std::vector<float>(kLength));
        for (std::size_t c = 0; c < run.channels; ++c) {
            for (std::size_t i = 0; i < kLength; ++i) {
                channels[c][i] = static_cast<float>(
                    0.1 * std::sin(2.0 * std::numbers::pi * kHz[c] * static_cast<double>(i) / 48000.0));
            }
        }
        const auto wav_in = dir / "ac4_multichannel_in.wav";
        REQUIRE(ac3::io::write_wav_f32(wav_in.string(), channels, 48000).has_value());
        const auto out = dir / "ac4_multichannel.ac4";
        REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(out) + run.args, log) == 0);
        CHECK(read_log(log).find(run.layout) != std::string::npos);
        const auto wav_out = dir / "ac4_multichannel_out.wav";
        REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(wav_out), log) == 0);
        const auto decoded = ac3::io::read_wav(wav_out.string());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == run.channels);
        // Each tone where it went in, 60 dB over every other there.
        for (std::size_t c = 0; c < run.channels; ++c) {
            CAPTURE(c);
            const double own = tone_amplitude(decoded->channels[c], 8192, kLength - 8192, kHz[c]);
            CHECK(std::abs(20.0 * std::log10(own / 0.1)) < 0.5);
            for (std::size_t other = 0; other < run.channels; ++other) {
                if (other != c) {
                    CAPTURE(other);
                    const double leak = tone_amplitude(decoded->channels[c], 8192, kLength - 8192, kHz[other]);
                    CHECK(20.0 * std::log10(own / std::max(leak, 1e-30)) > 60.0);
                }
            }
        }
    }
}

TEST_CASE("ac4-encode refuses what it does not write yet, naming it", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode_refused.log";
    const std::vector<std::vector<float>> four(4, std::vector<float>(4800, 0.0F));
    const auto wav_four = dir / "ac4_four.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_four.string(), four, 48000).has_value());
    const auto out = dir / "ac4_refused.ac4";
    CHECK(run_cli("ac4-encode " + quoted(wav_four) + " " + quoted(out), log) == 2);  // kExitInput
    CHECK(read_log(log).find("mono, stereo, 5.0 and 5.1") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
    // Seven or eight channels name the 7.X pair they carry.
    const std::vector<std::vector<float>> eight(8, std::vector<float>(4800, 0.0F));
    const auto wav_eight = dir / "ac4_eight.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_eight.string(), eight, 48000).has_value());
    CHECK(run_cli("ac4-encode " + quoted(wav_eight) + " " + quoted(out), log) == 2);
    CHECK(read_log(log).find("experimental=7x-back") != std::string::npos);
    CHECK_FALSE(fs::exists(out));

    const std::vector<std::vector<float>> stereo(2, std::vector<float>(4800, 0.0F));
    const auto wav_stereo = dir / "ac4_stereo_short.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_stereo.string(), stereo, 48000).has_value());
    // AC-3's and E-AC-3's own metadata, which AC-4 has nowhere to put.
    CHECK(run_cli("ac4-encode " + quoted(wav_stereo) + " " + quoted(out) + " heavy", log) == 1);
    CHECK(read_log(log).find("no AC-4 counterpart") != std::string::npos);
    // Gains with no profile to compute them from.
    CHECK(run_cli(
              "ac4-encode " + quoted(wav_stereo) + " " + quoted(out) + " experimental=drc-gains-1",
              log) == 1);
    CHECK(read_log(log).find("name one with drc=") != std::string::npos);
    // The downmix values describe a downmix stereo does not have, and the
    // LFE's gain an LFE 5.0 does not have.
    CHECK(run_cli("ac4-encode " + quoted(wav_stereo) + " " + quoted(out) + " lorocmixlev=-3",
                  log) == 1);
    CHECK(read_log(log).find("5.0, 5.1, 7.0 and 7.1") != std::string::npos);
    const std::vector<std::vector<float>> five(5, std::vector<float>(4800, 0.0F));
    const auto wav_five = dir / "ac4_five_short.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_five.string(), five, 48000).has_value());
    CHECK(run_cli("ac4-encode " + quoted(wav_five) + " " + quoted(out) + " lfemix=-4.5", log) == 1);
    CHECK(read_log(log).find("5.0 has no LFE") != std::string::npos);
    // 44.1 kHz has the 2 048-sample frame alone.
    const auto wav_44k = dir / "ac4_stereo_44k.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_44k.string(), stereo, 44100).has_value());
    CHECK(run_cli("ac4-encode " + quoted(wav_44k) + " " + quoted(out) + " frame-rate=25", log) ==
          1);
    CHECK(read_log(log).find("44.1 kHz") != std::string::npos);
    CHECK(run_cli("ac4-encode " + quoted(wav_stereo) + " " + quoted(out) + " 5000", log) == 1);  // kExitUsage
    CHECK(read_log(log).find("a rate outside 8 to 3 000 kbps") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
    // Silence has no loudness for dialnorm=auto to measure.
    CHECK(run_cli("ac4-encode " + quoted(wav_stereo) + " " + quoted(out) + " dialnorm=auto", log) == 5);
    CHECK(read_log(log).find("-70 LKFS") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
}

namespace {

std::uint32_t be32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | std::to_integer<std::uint32_t>(bytes[at + i]);
    }
    return value;
}

// The payload of the box down `path` from the top of an MP4 file, each name a
// box inside the one before it; empty where one is missing.
std::span<const std::byte> mp4_box(std::span<const std::byte> file,
                                   std::initializer_list<std::string_view> path) {
    std::span<const std::byte> level = file;
    for (const std::string_view type : path) {
        std::span<const std::byte> found;
        for (std::size_t at = 0; at + 8 <= level.size();) {
            const std::uint32_t size = be32(level, at);
            if (size < 8 || at + size > level.size()) {
                break;
            }
            if (std::string_view(reinterpret_cast<const char*>(level.data() + at + 4), 4) == type) {
                found = level.subspan(at + 8, size - 8);
                break;
            }
            at += size;
        }
        if (found.empty()) {
            return {};
        }
        level = found;
    }
    return level;
}

// syntax-trace='s records of the first frame, name and value, in order.
std::vector<std::pair<std::string, std::uint64_t>> first_frame_records(const fs::path& trace) {
    std::ifstream in{trace};
    std::vector<std::pair<std::string, std::uint64_t>> out;
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
        if (fields.size() != 6 || fields[0] != "0") {
            continue;
        }
        out.emplace_back(fields[5], std::stoull(fields[4]));
    }
    return out;
}

// The status line's dialnorm, as ac4-encode prints dB below full scale.
std::string fmt_dialnorm(double db_below) {
    std::string text = std::to_string(db_below);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.') {
        text.pop_back();
    }
    return "dialnorm -" + text + " dB";
}

std::vector<std::uint64_t> values_of(
    const std::vector<std::pair<std::string, std::uint64_t>>& records, std::string_view name) {
    std::vector<std::uint64_t> out;
    for (const auto& [record, value] : records) {
        if (record == name) {
            out.push_back(value);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("ac4-encode codes the frame rate and I-frames asked for and its MP4 lists the I-frames",
          "[cli][mp4][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode_rates.log";
    constexpr std::size_t kLength = 96000;
    std::vector<std::vector<float>> channels(2, std::vector<float>(kLength));
    for (std::size_t i = 0; i < kLength; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        channels[0][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
        channels[1][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 660.0 * t));
    }
    const auto wav_in = dir / "ac4_rates_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_in.string(), channels, 48000).has_value());

    // 29.97 fps, whose frames decode to 1 601 or 1 602 samples: Part 2
    // Table E.1 counts the track at 240 000 Hz, 8 008 a frame, and the
    // I-frames, every tenth and frame 3, are its sync samples.
    const auto mp4_out = dir / "ac4_rates.mp4";
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(mp4_out) +
                        " 128 frame-rate=29.97 rate-mode=average iframe-interval=10 iframes=3",
                    log) == 0);
    CHECK(read_log(log).find("29.97 fps, average rate") != std::string::npos);
    const auto file = read_file(mp4_out);
    const auto mdhd = mp4_box(file, {"moov", "trak", "mdia", "mdhd"});
    REQUIRE(mdhd.size() >= 16);
    CHECK(be32(mdhd, 12) == 240000U);
    const auto stts = mp4_box(file, {"moov", "trak", "mdia", "minf", "stbl", "stts"});
    REQUIRE(stts.size() >= 16);
    CHECK(be32(stts, 4) == 1U);
    CHECK(be32(stts, 12) == 8008U);
    const std::uint32_t frames = be32(stts, 8);
    CHECK(frames > 60U);
    const auto stss = mp4_box(file, {"moov", "trak", "mdia", "minf", "stbl", "stss"});
    REQUIRE(stss.size() >= 8);
    std::vector<std::uint32_t> sync;
    for (std::uint32_t i = 0; i < be32(stss, 4); ++i) {
        sync.push_back(be32(stss, 8 + 4 * static_cast<std::size_t>(i)));
    }
    std::vector<std::uint32_t> expected = {1, 4};
    for (std::uint32_t f = 10; f < frames; f += 10) {
        expected.push_back(f + 1);
    }
    CHECK(sync == expected);
    const auto decoded_wav = dir / "ac4_rates_out.wav";
    REQUIRE(run_cli("decode " + quoted(mp4_out) + " " + quoted(decoded_wav), log) == 0);
    const auto decoded = ac3::io::read_wav(decoded_wav.string());
    REQUIRE(decoded.has_value());
    CHECK(decoded->frame_count() > kLength);

    // The mp4 command puts the same stream, from a raw file, in the same
    // track.
    const auto raw_2997 = dir / "ac4_rates_2997.ac4";
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(raw_2997) +
                        " 128 frame-rate=29.97 rate-mode=average iframe-interval=10 iframes=3",
                    log) == 0);
    const auto remuxed = dir / "ac4_rates_remuxed.mp4";
    REQUIRE(run_cli("mp4 " + quoted(raw_2997) + " " + quoted(remuxed), log) == 0);
    CHECK(read_log(log).find("8008/240000 s a frame") != std::string::npos);
    const auto again = read_file(remuxed);
    const auto again_mdhd = mp4_box(again, {"moov", "trak", "mdia", "mdhd"});
    REQUIRE(again_mdhd.size() >= 16);
    CHECK(be32(again_mdhd, 12) == 240000U);
    const auto again_stss = mp4_box(again, {"moov", "trak", "mdia", "minf", "stbl", "stss"});
    CHECK(std::ranges::equal(again_stss, stss));

    // A raw stream at 25 fps, every frame an I-frame, at a variable rate:
    // the frames carry the rate's share between them.
    const auto raw_out = dir / "ac4_rates.ac4";
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(raw_out) +
                        " 96 frame-rate=25 rate-mode=variable iframe-interval=1",
                    log) == 0);
    CHECK(read_log(log).find("25 fps, variable rate") != std::string::npos);
    // The scan's frames are views of the bytes, which outlive it.
    const auto raw_bytes = read_file(raw_out);
    const auto scanned = ac4::scan(raw_bytes);
    REQUIRE(scanned.frames.size() > 50U);
    CHECK_FALSE(scanned.stopped_at.has_value());
    std::size_t sizes = 0;
    for (const ac4::SyncFrame& frame : scanned.frames) {
        const auto parsed = ac4::parse_raw_frame(frame.raw_ac4_frame);
        REQUIRE(parsed.has_value());
        CHECK(parsed->toc.frame_rate_index == 2);
        CHECK(parsed->toc.b_iframe_global);
        sizes += frame.raw_ac4_frame.size();
    }
    // 96 kbps at 25 fps is 480 bytes a frame, on average.
    const double average = static_cast<double>(sizes) / static_cast<double>(scanned.frames.size());
    CHECK(average < 480.0 * 1.05);
}

TEST_CASE("ac4-encode writes the metadata its options set as its syntax trace shows",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_encode_metadata.log";
    // A tone per channel of 5.1 in WAV order, 20 dB under full scale, for
    // four seconds: the short-term loudness and the loudness range need
    // three.
    constexpr std::array<double, 6> kHz = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};
    constexpr std::size_t kLength = 192000;
    std::vector<std::vector<float>> channels(6, std::vector<float>(kLength));
    for (std::size_t c = 0; c < channels.size(); ++c) {
        for (std::size_t i = 0; i < kLength; ++i) {
            channels[c][i] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * kHz[c] * static_cast<double>(i) / 48000.0));
        }
    }
    const auto wav_in = dir / "ac4_metadata_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_in.string(), channels, 48000).has_value());
    const auto out = dir / "ac4_metadata.ac4";
    const auto trace = dir / "ac4_metadata_trace.tsv";
    REQUIRE(
        run_cli(
            "ac4-encode " + quoted(wav_in) + " " + quoted(out) +
                " 256 loudness=ebu-r128 drc=film-standard drc-portable-speakers=speech "
                "drc-portable-headphones=speech lorocmixlev=-1.5 ltrtsurmixlev=-4.5 lfemix=-4.5 "
                "dmixmod=pl2 loro-correction=-2 dialogue-channels=c dialogue-max-gain=6 "
                "syntax-trace=" +
                quoted(trace),
            log) == 0);
    const auto text = read_log(log);
    INFO(text);
    // BS.1770 over five tones near -23 LKFS each, the surrounds 1.5 dB up
    // and the LFE left out: near -16 LKFS. dialnorm is that to the quarter
    // dB, and loudrelgat to the tenth.
    const std::size_t at = text.find("measured ");
    REQUIRE(at != std::string::npos);
    const double lkfs = std::stod(text.substr(at + 9));
    CHECK(lkfs > -17.0);
    CHECK(lkfs < -15.0);
    CHECK(text.find(fmt_dialnorm(-std::round(lkfs * 4.0) / 4.0)) != std::string::npos);
    const auto records = first_frame_records(trace);
    REQUIRE_FALSE(records.empty());
    CHECK(values_of(records, "loud_prac_type") == std::vector<std::uint64_t>{2});
    for (const std::string_view flag :
         {"b_loudrelgat", "b_max_loudstrm3s", "b_max_truepk", "b_lra", "b_max_loudmntry"}) {
        CAPTURE(flag);
        CHECK(values_of(records, flag) == std::vector<std::uint64_t>{1});
    }
    // floor(10 x L + 1/2) + 1 024, L printed to two places.
    const auto loudrelgat = values_of(records, "loudrelgat");
    REQUIRE(loudrelgat.size() == 1);
    const double code = std::floor(lkfs * 10.0 + 0.5) + 1024.0;
    CHECK(std::abs(static_cast<double>(loudrelgat[0]) - code) <= 1.0);
    // Film standard for the stream; modes 0 and 1 take it, 2 speech's curve
    // and 3 repeats 2.
    CHECK(values_of(records, "drc_eac3_profile") == std::vector<std::uint64_t>{1});
    CHECK(values_of(records, "drc_decoder_mode_id") == std::vector<std::uint64_t>{0, 1, 2, 3});
    CHECK(values_of(records, "drc_repeat_profile_flag") == std::vector<std::uint64_t>{0, 0, 0, 1});
    CHECK(values_of(records, "drc_repeat_id") == std::vector<std::uint64_t>{2});
    CHECK(values_of(records, "drc_default_profile_flag") == std::vector<std::uint64_t>{1, 1, 0});
    CHECK(values_of(records, "drc_compression_curve_flag") == std::vector<std::uint64_t>{1});
    // Table 149's -1.5 dB, Table 149a's -4.5 dB, 5.5 - -4.5, Pro Logic II,
    // and 15 - 2 x -2.
    CHECK(values_of(records, "loro_centre_mixgain") == std::vector<std::uint64_t>{3});
    CHECK(values_of(records, "ltrt_surround_mixgain") == std::vector<std::uint64_t>{5});
    CHECK(values_of(records, "lfe_mixgain") == std::vector<std::uint64_t>{10});
    CHECK(values_of(records, "preferred_dmx_method") == std::vector<std::uint64_t>{3});
    CHECK(values_of(records, "loro_dmx_loud_corr") == std::vector<std::uint64_t>{19});
    // Dialogue enhancement on C, capped at 6 dB.
    CHECK(values_of(records, "de_channel_config") == std::vector<std::uint64_t>{1});
    CHECK(values_of(records, "de_max_gain") == std::vector<std::uint64_t>{1});

    // A dialogue stem with the cross-channel method: the stem is the centre
    // tone, and de_method 1 needs two channels or three.
    std::vector<std::vector<float>> dialogue(6, std::vector<float>(kLength, 0.0F));
    dialogue[2] = channels[2];
    const auto stem = dir / "ac4_metadata_stem.wav";
    REQUIRE(ac3::io::write_wav_f32(stem.string(), dialogue, 48000).has_value());
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(out) +
                        " 256 dialnorm=24.5 dialogue-stem=" + quoted(stem) +
                        " dialogue-method=cross syntax-trace=" + quoted(trace),
                    log) == 0);
    CHECK(read_log(log).find("dialnorm -24.5 dB") != std::string::npos);
    const auto stem_records = first_frame_records(trace);
    CHECK(values_of(stem_records, "de_method") == std::vector<std::uint64_t>{1});
    CHECK(values_of(stem_records, "de_channel_config") == std::vector<std::uint64_t>{7});
    const auto decoded_wav = dir / "ac4_metadata_out.wav";
    REQUIRE(run_cli("decode " + quoted(out) + " " + quoted(decoded_wav) + " dialogue-enhancement=6",
                    log) == 0);
    // A stem of another length is refused.
    for (auto& channel : dialogue) {
        channel.resize(kLength - 1);
    }
    REQUIRE(ac3::io::write_wav_f32(stem.string(), dialogue, 48000).has_value());
    CHECK(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(out) +
                      " 256 dialogue-stem=" + quoted(stem),
                  log) == 2);
    CHECK(read_log(log).find("a dialogue stem has the programme's channels, rate and length") !=
          std::string::npos);
}

namespace {

// The payload of the first 'dac4' box in an MP4 file, found by its type.
std::vector<std::byte> dac4_of(std::span<const std::byte> file) {
    for (std::size_t at = 4; at + 4 <= file.size(); ++at) {
        if (std::string_view(reinterpret_cast<const char*>(file.data() + at), 4) == "dac4") {
            const std::uint32_t size = be32(file, at - 4);
            REQUIRE(size >= 8);
            REQUIRE(at - 4 + size <= file.size());
            return {file.begin() + static_cast<std::ptrdiff_t>(at + 4),
                    file.begin() + static_cast<std::ptrdiff_t>(at - 4 + size)};
        }
    }
    FAIL("no dac4 box");
    return {};
}

// Each presentation's pres_bytes in an ac4_dsi_v1() (TS 103 190-2 Annex E.6)
// with no program identifier: after 24 bits of header, b_program_id, 66 of
// ac4_bitrate_dsi() and the alignment, a presentation_version byte and
// pres_bytes, with add_pres_bytes past 254, then the body.
std::vector<std::size_t> presentation_sizes(std::span<const std::byte> dac4) {
    REQUIRE(dac4.size() >= 12);
    const auto byte = [&](std::size_t i) { return std::to_integer<std::size_t>(dac4[i]); };
    const std::size_t n_presentations = ((byte(1) & 1U) << 8U) | byte(2);
    REQUIRE((byte(3) & 0x80U) == 0U);  // b_program_id
    std::vector<std::size_t> sizes;
    std::size_t at = 12;
    for (std::size_t p = 0; p < n_presentations; ++p) {
        REQUIRE(at + 2 <= dac4.size());
        std::size_t size = byte(at + 1);
        at += 2;
        if (size == 255) {
            size += (byte(at) << 8U) | byte(at + 1);
            at += 2;
        }
        sizes.push_back(size);
        at += size;
    }
    CHECK(at == dac4.size());
    return sizes;
}

}  // namespace

TEST_CASE("mp4 describes every presentation of a stream of several in its dac4 or refuses",
          "[cli][mp4][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_presentations_mp4.log";
    const fs::path presentations =
        fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / ".." / "ac4dec" / "presentations";
    // Phase E6's broadcast stream: fifteen presentations, the eighth an
    // alternative one named Deutsch, whose name the decoder reads from its
    // presentation substream for the box.
    const auto out = dir / "ac4_broadcast.mp4";
    REQUIRE(run_cli("mp4 " + quoted(presentations / "encoder-broadcast.ac4") + " " + quoted(out), log) == 0);
    const auto file = read_file(out);
    const std::vector<std::byte> dac4 = dac4_of(file);
    const std::vector<std::size_t> sizes = presentation_sizes(dac4);
    REQUIRE(sizes.size() == 15);
    for (const std::size_t size : sizes) {
        CHECK(size > 0U);
    }
    const std::string_view text(reinterpret_cast<const char*>(dac4.data()), dac4.size());
    CHECK(text.find("Deutsch") != std::string_view::npos);
    // A bitstream_version 1 stream's presentations are Part 1's, which the box
    // does not describe: refused, and the reason named.
    const auto refused = dir / "ac4_v0.mp4";
    CHECK(run_cli("mp4 " + quoted(presentations / "presentations-v0.ac4") + " " + quoted(refused), log) == 2);
    CHECK(read_log(log).find("bitstream_version 0 or 1") != std::string::npos);
    CHECK_FALSE(fs::exists(refused));
}

TEST_CASE("ts refuses AC-4 under the atsc profile with a real reason", "[cli][ts][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_atsc.log";
    const auto out = dir / "refused.ts";
    CHECK(run_cli("ts " + quoted(ac4_fixture()) + " " + quoted(out) + " atsc", log) != 0);
    const auto report = read_log(log);
    CHECK(report.find("ATSC") != std::string::npos);
    CHECK(report.find("dvb") != std::string::npos);
}
