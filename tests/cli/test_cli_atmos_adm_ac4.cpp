#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/admbridge/bridge.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/objects/motion.hpp"
#include "iclforge/adm/ac3adm.hpp"

// planning/ac4.md, I5's named exit criterion: "an ADM ... master from the committed fixtures the
// E-AC-3 object tests use, encoded to AC-4 by atmos-adm and decoded with adm_out, gives objects
// whose positions, gains and timing match the master within tolerances you pin." Real,
// subprocess-level integration test, the same "run the actual built binary" shape
// tests/cli/test_cli_atmos_adm.cpp and test_cli_decode_adm.cpp use, and for the same reason (their
// own top comments: main.cpp compiles run_atmos_adm/run_decode_ac4 into one anonymous-namespace
// binary this test binary cannot link directly). Separate file, same two-part
// AC3FORGE_BUILD_ADM-and-ac3cli gate as those two files (tests/CMakeLists.txt); the fixture below
// is a byte-identical copy of test_cli_atmos_adm.cpp's own (bed L/R at +-30 degrees, one object
// held at azimuth -110 (SR) for 0.096s then jumping to dead ahead) - "the committed fixtures the
// E-AC-3 object tests use", per the exit criterion's own wording, rather than a new one.
//
// The round trip: atmos-adm codec=ac4 (A-JOC, this project's own writer) -> decode's objects_dir
// (each object's raw PCM) and adm_out (a fresh ADM BWF master, ac4::ObjectProperties bridged onto
// ac3::oba::DynamicObject - apps/cli/commands/decode.cpp's to_oba_dynamic_object). "Match the
// master" is checked by parsing BOTH the original fixture and the round-tripped adm_out back
// through the same ac3adm::parse_bw64/ac3::admbridge::build the read side already uses, evaluating
// each channel's ObjectPath at the same two times (well inside each hold, clear of the encode's own
// frame-boundary quantization around the 0.096s jump - see kBeforeJumpS/kAfterJumpS below) and
// comparing position/gain - not by asserting a specific numeric azimuth-to-room-cube mapping, which
// belongs to ac3::admbridge's own tests.

namespace fs = std::filesystem;

namespace {

std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_adm_ac4_" + scratch_pid_suffix());
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

using Bytes = std::string;

void put_u16le(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32le(Bytes& out, std::uint32_t value) {
    put_u16le(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    put_u16le(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
}

void put_fourcc(Bytes& out, std::string_view cc) {
    REQUIRE(cc.size() == 4);
    out += cc;
}

void put_fixed(Bytes& out, std::string_view value, std::size_t width) {
    REQUIRE(value.size() == width);
    out += value;
}

void append_chunk(Bytes& out, std::string_view id, const Bytes& content) {
    put_fourcc(out, id);
    put_u32le(out, static_cast<std::uint32_t>(content.size()));
    out += content;
    if (content.size() % 2 != 0) {
        out.push_back('\0');
    }
}

constexpr int kFrame = ac3::kSamplesPerFrame;
constexpr int kTotalFrames = 6;  // 3 frames holding SR, 3 frames holding centre
// Well inside each hold - test_cli_decode_adm.cpp's own kPulseAt comment gives the same reasoning
// for staying clear of a boundary rather than landing on one.
constexpr double kBeforeJumpS = 0.05;
constexpr double kAfterJumpS = 0.13;
// Position: AC-4's own quantization (ac4enc/encoder.hpp: X/Y in steps of 1/62, Z in 1/15) plus this
// command's one-update-a-frame (2048 samples, 42.67 ms) sampling of the continuous ADM automation.
// Gain: AC-4's own whole-dB steps.
constexpr double kPositionTolerance = 0.06;
constexpr double kGainToleranceDb = 2.0;

Bytes build_fmt_chunk_3ch() {
    Bytes fmt;
    put_u16le(fmt, 1);
    put_u16le(fmt, 3);
    put_u32le(fmt, 48000);
    put_u32le(fmt, 48000 * 6);
    put_u16le(fmt, 6);
    put_u16le(fmt, 16);
    return fmt;
}

Bytes build_chna_chunk_3() {
    Bytes chna;
    put_u16le(chna, 3);
    put_u16le(chna, 3);
    struct Row {
        std::uint16_t track;
        std::string_view uid, track_ref, pack_ref;
    };
    const Row rows[] = {
        {1, "ATU_00000001", "AT_00019001_01", "AP_00019001"},
        {2, "ATU_00000002", "AT_00019002_01", "AP_00019001"},
        {3, "ATU_00000003", "AT_00039001_01", "AP_00039001"},
    };
    for (const auto& row : rows) {
        put_u16le(chna, row.track);
        put_fixed(chna, row.uid, 12);
        put_fixed(chna, row.track_ref, 14);
        put_fixed(chna, row.pack_ref, 11);
        chna.push_back('\0');
    }
    return chna;
}

// One real, distinct, non-silent tone a channel: 300 Hz (bed left), 500 Hz (bed right), 800 Hz
// (the moving object) - never silence/frame-0 (this project's own standing lesson: those give false
// passes).
constexpr std::array<double, 3> kToneHz = {300.0, 500.0, 800.0};

Bytes build_pcm16_3ch(int frames) {
    Bytes data;
    const double amplitude = 0.3 * 32767.0;
    for (int frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / 48000.0;
        for (const double hz : kToneHz) {
            const double v = amplitude * std::sin(2.0 * std::numbers::pi * hz * t);
            put_u16le(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
        }
    }
    return data;
}

// Byte-identical to test_cli_atmos_adm.cpp's own kAdmXml (see that file for the full field-by-field
// commentary): two DirectSpeakers bed channels at M+030/M-030, one Objects channel held at azimuth
// -110 (this project's own SR ring position) for 0.096 s then jumping (no interpolation) to dead
// ahead.
constexpr std::string_view kAdmXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<audioFormatExtended version="ITU-R_BS.2076-2">
  <audioProgramme audioProgrammeID="APR_9001" audioProgrammeName="CliBridgeTestAc4">
    <audioContentIDRef>ACO_9001</audioContentIDRef>
    <audioContentIDRef>ACO_9002</audioContentIDRef>
  </audioProgramme>
  <audioContent audioContentID="ACO_9001" audioContentName="Bed">
    <audioObjectIDRef>AO_9001</audioObjectIDRef>
  </audioContent>
  <audioContent audioContentID="ACO_9002" audioContentName="Moving">
    <audioObjectIDRef>AO_9002</audioObjectIDRef>
  </audioContent>
  <audioObject audioObjectID="AO_9001" audioObjectName="Bed" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000001</audioTrackUIDRef>
    <audioTrackUIDRef>ATU_00000002</audioTrackUIDRef>
  </audioObject>
  <audioObject audioObjectID="AO_9002" audioObjectName="Moving" start="00:00:00.00000">
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
    <audioTrackUIDRef>ATU_00000003</audioTrackUIDRef>
  </audioObject>
  <audioPackFormat audioPackFormatID="AP_00019001" audioPackFormatName="Bed" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioChannelFormatIDRef>AC_00019002</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioPackFormat audioPackFormatID="AP_00039001" audioPackFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
  </audioPackFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019001" audioChannelFormatName="BedLeft" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019001_00000001">
      <speakerLabel>M+030</speakerLabel>
      <position coordinate="azimuth">30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00019002" audioChannelFormatName="BedRight" typeLabel="0001" typeDefinition="DirectSpeakers">
    <audioBlockFormat audioBlockFormatID="AB_00019002_00000001">
      <speakerLabel>M-030</speakerLabel>
      <position coordinate="azimuth">-30.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioChannelFormat audioChannelFormatID="AC_00039001" audioChannelFormatName="Moving" typeLabel="0003" typeDefinition="Objects">
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000001" rtime="00:00:00.00000" duration="00:00:00.09600">
      <position coordinate="azimuth">-110.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
    <audioBlockFormat audioBlockFormatID="AB_00039001_00000002" rtime="00:00:00.09600">
      <position coordinate="azimuth">0.0</position>
      <position coordinate="elevation">0.0</position>
      <position coordinate="distance">1.0</position>
      <jumpPosition>1</jumpPosition>
    </audioBlockFormat>
  </audioChannelFormat>
  <audioStreamFormat audioStreamFormatID="AS_00019001" audioStreamFormatName="PCM_BedLeft" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019001_01" audioTrackFormatName="PCM_BedLeft" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000001" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00019002" audioStreamFormatName="PCM_BedRight" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00019002</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00019002_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00019002_01" audioTrackFormatName="PCM_BedRight" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00019002</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000002" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00019002_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00019001</audioPackFormatIDRef>
  </audioTrackUID>
  <audioStreamFormat audioStreamFormatID="AS_00039001" audioStreamFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioChannelFormatIDRef>AC_00039001</audioChannelFormatIDRef>
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
  </audioStreamFormat>
  <audioTrackFormat audioTrackFormatID="AT_00039001_01" audioTrackFormatName="PCM_Moving" formatLabel="0001" formatDefinition="PCM">
    <audioStreamFormatIDRef>AS_00039001</audioStreamFormatIDRef>
  </audioTrackFormat>
  <audioTrackUID UID="ATU_00000003" sampleRate="48000" bitDepth="16">
    <audioTrackFormatIDRef>AT_00039001_01</audioTrackFormatIDRef>
    <audioPackFormatIDRef>AP_00039001</audioPackFormatIDRef>
  </audioTrackUID>
</audioFormatExtended>
)";

bool write_fixture(const fs::path& path) {
    const auto fmt = build_fmt_chunk_3ch();
    const auto chna = build_chna_chunk_3();
    const Bytes axml(kAdmXml);
    const auto data = build_pcm16_3ch(kTotalFrames * kFrame);

    Bytes body;
    append_chunk(body, "fmt ", fmt);
    append_chunk(body, "chna", chna);
    append_chunk(body, "axml", axml);
    append_chunk(body, "data", data);

    Bytes file;
    put_fourcc(file, "RIFF");
    put_u32le(file, static_cast<std::uint32_t>(4 + body.size()));
    put_fourcc(file, "WAVE");
    file += body;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

// The complex amplitude of `hz` in `samples`, a plain single-bin DFT (Goertzel-equivalent) over the
// whole span - short enough here (a few thousand samples) that no windowing is needed to tell three
// well-separated tones (300/500/800 Hz) apart.
std::complex<double> component(std::span<const float> samples, double hz, std::uint32_t rate) {
    const double w = 2.0 * std::numbers::pi * hz / static_cast<double>(rate);
    std::complex<double> sum{};
    for (std::size_t n = 0; n < samples.size(); ++n) {
        sum += static_cast<double>(samples[n]) * std::polar(1.0, -w * static_cast<double>(n));
    }
    return samples.empty() ? sum : 2.0 * sum / static_cast<double>(samples.size());
}

// Which of kToneHz dominates `samples` - the CHANNEL IDENTITY check: this project's own object
// index order should survive atmos-adm's AC-4 encode and decode's re-export/re-write unchanged, but
// this looks the object up by its own tone rather than trusting that, so a future reordering fails
// with a clear "no channel carries fixture channel N's tone" rather than silently comparing the
// wrong two objects' positions.
std::optional<std::size_t> tone_index_of(std::span<const float> samples, std::uint32_t rate) {
    std::optional<std::size_t> best;
    double best_mag = 0.0;
    for (std::size_t i = 0; i < kToneHz.size(); ++i) {
        const double mag = std::abs(component(samples, kToneHz[i], rate));
        if (mag > best_mag) {
            best_mag = mag;
            best = i;
        }
    }
    // A real tone, not noise-floor energy: the fixture's own amplitude is 0.3 of full scale
    // (build_pcm16_3ch), comfortably above what any residual noise or cross-talk after AC-4's lossy
    // coding, panning and the ADM round trip could produce at an unrelated bin.
    return best_mag > 0.02 ? best : std::nullopt;
}

double gain_db(double linear) {
    return linear > 0.0 ? 20.0 * std::log10(linear) : -std::numeric_limits<double>::infinity();
}

// "  the decoder's output lags the input by <N> samples" (run_atmos_objects_to_ac4's own status
// line, apps/cli/commands/atmos.cpp) - the encoder's delay_samples() plus the decoder's
// decoder_delay_samples(), both fixed for frame_rate_index 13 (which this command's AC-4 path
// always uses - encoder.hpp's own ObjectsConfig comment). adm_out's timeline is the DECODER's own
// output sample count from its first (silent, priming) frame, so a position "at ADM time t" in the
// original fixture reads back "at ADM time t + this many samples" in the round-tripped master - the
// decoder has no way to know how much silence the encoder prepended, only ITS OWN processing delay
// (decoder.hpp's own decoder_delay_samples() comment).
std::optional<int> lag_samples_from_log(const std::string& log) {
    const std::string needle = "lags the input by ";
    const auto at = log.find(needle);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    return std::atoi(log.c_str() + at + needle.size());
}

}  // namespace

TEST_CASE(
    "ac3cli atmos-adm codec=ac4, decoded with objects_dir/adm_out, matches the ADM master's own "
    "positions, gains and timing",
    "[cli][atmos-adm][ac4]") {
    const auto dir = scratch_dir();
    const auto fixture_path = dir / "atmos_adm_ac4_fixture.wav";
    REQUIRE(write_fixture(fixture_path));

    const auto ac4_path = dir / "atmos_adm_ac4_out.ac4";
    const auto encode_log = dir / "atmos_adm_ac4_encode.log";
    const auto encode_rc = run_cli("atmos-adm \"" + fixture_path.string() + "\" \"" +
                                        ac4_path.string() + "\" 256 \"\" codec=ac4",
                                    encode_log);
    const auto encode_text = read_log(encode_log);
    INFO(encode_text);
    REQUIRE(encode_rc == 0);
    REQUIRE(fs::exists(ac4_path));
    CHECK(fs::file_size(ac4_path) > 0);
    const auto lag = lag_samples_from_log(encode_text);
    REQUIRE(lag.has_value());
    const double lag_s = static_cast<double>(*lag) / 48000.0;

    const auto decoded_wav = dir / "atmos_adm_ac4_decoded.wav";
    const auto objects_dir = dir / "atmos_adm_ac4_objects";
    const auto adm_out = dir / "atmos_adm_ac4_roundtrip.wav";
    const auto decode_log = dir / "atmos_adm_ac4_decode.log";
    const auto decode_rc =
        run_cli("decode \"" + ac4_path.string() + "\" \"" + decoded_wav.string() + "\" \"" +
                    objects_dir.string() + "\" \"" + adm_out.string() + "\"",
                decode_log);
    INFO(read_log(decode_log));
    REQUIRE(decode_rc == 0);
    REQUIRE(fs::exists(decoded_wav));
    REQUIRE(fs::exists(adm_out));

    // objects_dir: exactly one object_NN.wav per source channel, each still carrying its own
    // channel's tone undistorted (raw per-object PCM, not panned - see decode.cpp's own
    // append_ac4_objects).
    std::size_t object_files = 0;
    for (const auto& entry : fs::directory_iterator(objects_dir)) {
        if (entry.path().extension() == ".wav") {
            ++object_files;
            const auto wav = ac3::io::read_wav(entry.path().string());
            REQUIRE(wav.has_value());
            REQUIRE(wav->channels.size() == 1);
            const auto tone = tone_index_of(wav->channels.front(), wav->sample_rate);
            CAPTURE(entry.path().string());
            REQUIRE(tone.has_value());
        }
    }
    CHECK(object_files == 3);

    // "Match the master": the original fixture and the round-tripped adm_out, both read back
    // through ac3adm::parse_bw64/ac3::admbridge::build (the same pipeline load_adm_atmos_source
    // itself uses), evaluated at the same two times and compared object for object (matched by
    // tone, not by index - this function's own top comment).
    const auto before_doc = ac3adm::parse_bw64(fixture_path.string());
    REQUIRE(before_doc.has_value());
    const auto before = ac3::admbridge::build(*before_doc);
    REQUIRE(before.has_value());
    REQUIRE(before->channel_count() == 3);

    const auto after_doc = ac3adm::parse_bw64(adm_out.string());
    REQUIRE(after_doc.has_value());
    const auto after = ac3::admbridge::build(*after_doc);
    REQUIRE(after.has_value());
    REQUIRE(after->channel_count() == 3);

    for (std::size_t bi = 0; bi < before->channel_count(); ++bi) {
        const auto tone = tone_index_of(before->pcm[bi], before->sample_rate);
        CAPTURE(bi, before->channel_ids[bi]);
        REQUIRE(tone.has_value());
        std::optional<std::size_t> ai;
        for (std::size_t k = 0; k < after->channel_count(); ++k) {
            if (tone_index_of(after->pcm[k], after->sample_rate) == tone) {
                ai = k;
                break;
            }
        }
        CAPTURE(*tone);
        REQUIRE(ai.has_value());

        for (const double t : {kBeforeJumpS, kAfterJumpS}) {
            const auto want = before->paths[bi].evaluate(t);
            const auto got = after->paths[*ai].evaluate(t + lag_s);
            CAPTURE(t, want.position.x, want.position.y, want.position.z, got.position.x,
                    got.position.y, got.position.z);
            CHECK(got.position.x == Catch::Approx(want.position.x).margin(kPositionTolerance));
            CHECK(got.position.y == Catch::Approx(want.position.y).margin(kPositionTolerance));
            CHECK(got.position.z == Catch::Approx(want.position.z).margin(kPositionTolerance));
            CHECK(gain_db(got.gain) ==
                  Catch::Approx(gain_db(want.gain)).margin(kGainToleranceDb));
        }
    }

    // Timing: the moving object's own two positions (SR then centre) actually differ - proving the
    // jump survived the round trip rather than every sample above merely being loose enough to
    // pass on a flat, unmoving reading (this project's own standing lesson on false passes from
    // reused defaults).
    std::size_t moving = before->channel_count();
    for (std::size_t bi = 0; bi < before->channel_count(); ++bi) {
        if (!before->is_bed[bi]) {
            moving = bi;
            break;
        }
    }
    REQUIRE(moving < before->channel_count());
    const auto moving_tone = tone_index_of(before->pcm[moving], before->sample_rate);
    REQUIRE(moving_tone.has_value());
    std::optional<std::size_t> moving_after;
    for (std::size_t k = 0; k < after->channel_count(); ++k) {
        if (tone_index_of(after->pcm[k], after->sample_rate) == moving_tone) {
            moving_after = k;
            break;
        }
    }
    REQUIRE(moving_after.has_value());
    const auto early = after->paths[*moving_after].evaluate(kBeforeJumpS + lag_s);
    const auto late = after->paths[*moving_after].evaluate(kAfterJumpS + lag_s);
    const double moved = std::abs(early.position.x - late.position.x) +
                         std::abs(early.position.y - late.position.y) +
                         std::abs(early.position.z - late.position.z);
    CHECK(moved > 0.1);
}
