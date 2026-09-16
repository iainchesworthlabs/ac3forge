#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/admbridge/bridge.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"
#include "ac3adm/ac3adm.hpp"

// ac3cli's 'decode ... adm_out=' path (roadmap IM2's write direction - apps/cli/commands/decode.cpp's
// accumulate_adm/run_decode_eac3). Real, subprocess-level integration test: the same "run the actual
// built binary, inspect what it wrote" shape tests/cli/test_cli_atmos_adm.cpp (IM2's read direction)
// and tests/cli/test_cli.cpp's own atmos-encode tests use, and for the same reason - see
// test_cli_atmos_adm.cpp's own top comment on why decode.cpp's own logic cannot be linked into this
// test binary and called directly. A separate file for the same two-part reason as that file's own top
// comment: this only makes sense with AC3FORGE_BUILD_ADM AND ac3cli both on - see tests/CMakeLists.txt's
// own gating comment on the block this file's source is added to.
//
// What this proves: a JOC-reconstructed object comes out of decode_access_unit
// oba::joc::reconstruction_delay(domain) samples (576 under Domain::kQmf, the DecoderConfig default)
// behind the bed it was pulled from (docs/library/decoding.md, "Atmos objects lag the bed";
// tests/decoder/test_latency.cpp measures it end to end). accumulate_adm appends each decoded unit's
// object_audio and the bed's LFE channel side by side, unit by unit, into the ADM master - carrying
// that same 576-sample gap straight into the exported file unless something delays the LFE to match.
//
// Regression test: encode a real Atmos stream with one object sent entirely to the LFE (lfe_send=1.0,
// via atmos-encode's own keyframes file - there is no lfe_send CLI flag, see run_atmos_encode's own
// default placement in apps/cli/commands/atmos.cpp), decode it with adm_out= set, read the BW64/ADM
// master back and cross-correlate its LFE channel against its one object channel. Before the fix this
// PR made, the object trails the LFE by 576 samples in the written file; fixed, they line up.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy shares (this project's
// established per-file test-helper convention - test_cli_atmos_adm.cpp's own top comment); the leaf
// name below is this file's own.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_decode_adm";
    fs::create_directories(dir);
    return dir;
}

// See tests/cli/test_cli_atmos_adm.cpp's own run_cli for the full reasoning behind the Windows
// double-quote-wrapping workaround this duplicates.
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

constexpr int kFrames = 8;
// Frame 3, as tests/decoder/test_latency.cpp and tests/render/test_object_lfe_timing.cpp (the header-
// only renderer's own regression test for this same class of bug) both place their own marker: past
// every encoder's/decoder's own priming, early enough that reconstruction_delay(kQmf)'s 576 samples
// still leave it well inside an 8-frame stream.
constexpr int kPulseAt = 3 * ac3::kSamplesPerFrame + 512;

// The object's whole-clip audio: a decaying tone burst riding a quiet noise floor. The floor is not
// decoration - JOC's reconstruction matrix is solved per frame from the object's own energy that frame
// (TS 103 420 §6.6.5 interpolates each frame's from the one before), so a pulse arriving out of true
// silence comes back through a matrix still ramping up across it, smearing exactly the peak a
// correlation is trying to locate. Flat, low-level energy in every frame keeps the matrix settled -
// tests/render/test_object_lfe_timing.cpp's own programme() makes the identical argument for the
// header-only renderer's regression test. The burst itself, not a steady tone, for the reason
// tests/decoder/test_latency.cpp's own burst() gives: a sinusoid correlates with itself a period away
// almost as well as at zero lag, which would make a lag search meaningless.
std::vector<float> object_pulse_with_floor(int samples, int at) {
    std::vector<float> pcm(static_cast<std::size_t>(samples), 0.0F);
    std::uint32_t state = 0x9E3779B9U;
    for (float& sample : pcm) {
        state = (state * 1664525U) + 1013904223U;
        const double white = (static_cast<double>(state >> 8U) / 8388608.0) - 1.0;
        sample = static_cast<float>(0.02 * white);
    }
    for (int n = 0; n < 700; ++n) {
        const int index = at + n;
        if (index >= samples) {
            break;
        }
        const double t = static_cast<double>(n) / 48000.0;
        const double envelope = std::exp(-t * 90.0);
        pcm[static_cast<std::size_t>(index)] +=
            static_cast<float>(0.6 * envelope * std::sin(2.0 * std::numbers::pi * 120.0 * t));
    }
    return pcm;
}

float peak(std::span<const float> pcm) {
    float out = 0.0F;
    for (const float sample : pcm) {
        out = std::max(out, std::abs(sample));
    }
    return out;
}

// The lag (later[n + lag] against earlier[n]) maximising their correlation over a window around the
// pulse - tests/decoder/test_latency.cpp's own best_lag, searched both directions since this test,
// unlike that one, does not already know which of the two channels leads.
int best_lag(std::span<const float> earlier, std::span<const float> later, int min_lag, int max_lag) {
    const int from = std::max(0, kPulseAt - 2048);
    const int to = std::min(static_cast<int>(earlier.size()), kPulseAt + 4096);
    int best = min_lag;
    double best_score = -1.0;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        double score = 0.0;
        for (int n = from; n < to; ++n) {
            const int m = n + lag;
            if (m < 0 || m >= static_cast<int>(later.size())) {
                continue;
            }
            score += static_cast<double>(earlier[static_cast<std::size_t>(n)]) *
                     static_cast<double>(later[static_cast<std::size_t>(m)]);
        }
        if (score > best_score) {
            best_score = score;
            best = lag;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("decode's ADM master lines the bed's LFE up with the object it was pulled beside",
          "[cli][decode][adm]") {
    const auto dir = scratch_dir();

    const auto wav_path = dir / "decode_adm_in.wav";
    const std::vector<std::vector<float>> channels{
        object_pulse_with_floor(kFrames * ac3::kSamplesPerFrame, kPulseAt)};
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    // One static keyframe (ac3/oba/scene.hpp: "a single keyframe holds its placement everywhere")
    // sending the object entirely to the LFE - run_atmos_encode's own default placement is
    // lfe_send=0.0 (apps/cli/commands/atmos.cpp), and the only way to override it is this keyframes
    // file (main.cpp's own 'atmos-encode' doc string names no lfe_send flag).
    const auto paths_path = dir / "decode_adm_paths.txt";
    {
        std::ofstream paths{paths_path};
        REQUIRE(paths.is_open());
        paths << "0 0.0 0.5 0.5 0.0 1.0 1.0\n";
    }

    const auto ec3_path = dir / "decode_adm_in.ec3";
    const auto encode_log = dir / "decode_adm_encode.log";
    const auto encode_rc = run_cli("atmos-encode \"" + wav_path.string() + "\" \"" +
                                        ec3_path.string() + "\" 448 1 \"" + paths_path.string() + "\"",
                                    encode_log);
    INFO(read_log(encode_log));
    REQUIRE(encode_rc == 0);

    const auto out_wav = dir / "decode_adm_out.wav";
    const auto adm_out = dir / "decode_adm_master.wav";
    const auto decode_log = dir / "decode_adm_decode.log";
    // Positional 3 (objects_dir) skipped with an explicit empty argument so positional 4 (adm_out)
    // still lands correctly - tests/cli/test_cli_stream_tools.cpp's own transcode test uses the same
    // trick for a skipped middle positional.
    const auto decode_rc =
        run_cli("decode \"" + ec3_path.string() + "\" \"" + out_wav.string() + "\" \"\" \"" +
                    adm_out.string() + "\"",
                decode_log);
    INFO(read_log(decode_log));
    REQUIRE(decode_rc == 0);
    REQUIRE(fs::exists(adm_out));

    const auto parsed = ac3adm::parse_bw64(adm_out.string());
    REQUIRE(parsed.has_value());
    const auto bridged = ac3::admbridge::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 2);

    // accumulate_adm always writes the dynamic object(s) first, the bed's LFE last
    // (AdmMasterInput::channels.resize(object_audio.size() + (have_lfe ? 1 : 0)), decode.cpp) - but
    // looked up here by is_bed/is_lfe rather than trusted by position, so a change to that order fails
    // the REQUIREs below rather than silently comparing the wrong channels.
    std::size_t object_i = bridged->channel_count();
    std::size_t lfe_i = bridged->channel_count();
    for (std::size_t i = 0; i < bridged->channel_count(); ++i) {
        if (bridged->is_bed[i] && bridged->is_lfe[i]) {
            lfe_i = i;
        } else if (!bridged->is_bed[i]) {
            object_i = i;
        }
    }
    REQUIRE(object_i < bridged->channel_count());
    REQUIRE(lfe_i < bridged->channel_count());

    const auto object_pcm = bridged->pcm[object_i];
    const auto lfe_pcm = bridged->pcm[lfe_i];
    REQUIRE(object_pcm.size() == lfe_pcm.size());

    // Both channels must actually carry the burst, or the lag search below measures noise.
    REQUIRE(peak(object_pcm) > 0.05F);
    REQUIRE(peak(lfe_pcm) > 0.05F);

    const int lag =
        best_lag(lfe_pcm, object_pcm, -2 * ac3::kSamplesPerFrame, 2 * ac3::kSamplesPerFrame);
    CAPTURE(lag);
    // Before this fix: the LFE channel was written straight from the decoded bed, undelayed, while
    // the object channel is JOC-reconstructed and so already reconstruction_delay(kQmf) samples (576)
    // behind it - the object trailed the LFE by 576 in the unfixed file (measured on this exact
    // fixture while confirming the bug - see the PR description). Fixed, the two line up.
    CHECK(lag == 0);
}
