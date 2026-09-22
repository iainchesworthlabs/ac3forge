#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/admbridge/bridge.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3adm/ac3adm.hpp"

// Roadmap item IM2 ("JOC -> ADM BWF writer") - the write-direction counterpart of this
// directory's own test_adm_bridge.cpp flagship test, and driven the same real way: a real
// AtmosEncoder/Eac3Decoder round trip, not a mocked one. Where that file starts from a
// byte-level ADM fixture and ends at a decoded bitstream, this one starts from a decoded
// bitstream (exactly what apps/cli/commands/decode.cpp's own accumulate_adm lambda consumes)
// and ends at a real file on disk, read back through the identical ac3adm::parse_bw64 ->
// ac3::admbridge::build -> AtmosEncoder/Eac3Decoder chain that file's own flagship test already
// proves correct - so if THIS test's second half passes, the whole write -> read round trip
// really works, not just "write_bw64 didn't throw".

namespace fs = std::filesystem;

namespace {

using ac3::eac3::chanmap::Location;

double channel_energy(std::span<const float> samples) {
    double energy = 0.0;
    for (const auto v : samples) {
        const double sd = static_cast<double>(v);
        energy += sd * sd;
    }
    return energy;
}

// Mirrors decode.cpp's own accumulate_adm lambda, simplified for a single dynamic object (this
// test's own AtmosEncoder is always constructed with exactly one) - see
// tests/cli/test_cli_atmos_adm.cpp's own top comment for why this project's own tests duplicate
// a CLI-local helper rather than exporting one just to share it with a test.
struct AdmAccumulator {
    std::uint64_t samples_emitted = 0;
    std::vector<float> object_pcm;
    std::vector<ac3::admbridge::WriteObjectUpdate> object_updates;
    std::vector<float> lfe_pcm;

    void add(const ac3::DecodedAccessUnit& unit) {
        REQUIRE(unit.object_metadata.has_value());
        REQUIRE(unit.object_audio.size() == 1);
        object_pcm.insert(object_pcm.end(), unit.object_audio[0].begin(), unit.object_audio[0].end());

        const auto lfe_slot = unit.layout.index_of(Location::kLfe);
        REQUIRE(lfe_slot >= 0);
        const auto& lfe_channel = unit.channels[static_cast<std::size_t>(lfe_slot)];
        lfe_pcm.insert(lfe_pcm.end(), lfe_channel.begin(), lfe_channel.end());

        for (const auto& block : unit.object_metadata->blocks) {
            REQUIRE(block.objects.size() == 1);
            object_updates.push_back(
                {.sample_offset = samples_emitted + static_cast<std::uint64_t>(std::max(block.sample_offset, 0)),
                 .ramp_duration_samples = block.ramp_duration,
                 .state = block.objects[0]});
        }
        samples_emitted += unit.object_audio[0].size();
    }
};

}  // namespace

TEST_CASE("a real decoded Atmos programme survives write_bw64 -> parse_bw64 -> build -> a fresh "
         "AtmosEncoder/Eac3Decoder round trip",
         "[admbridge][atmos][write]") {
    constexpr int kFrame = ac3::kSamplesPerFrame;
    constexpr int kTotalFrames = 6;  // 3 frames holding right, 3 frames holding left
    constexpr double kSampleRate = 48000.0;
    // The instant-jump nudge this project's own read-direction bridge uses
    // (bridge.cpp's kInstantJumpEpsilon) - see this test's own hold_end/jump_at comment for why
    // the write side needs the identical convention on its authoring side.
    constexpr double kJumpEpsilon = 1.0e-6;

    const double hold_end = static_cast<double>(3 * kFrame) / kSampleRate;  // 0.096s
    const auto path = ac3::oba::KeyframePath::create({
        {.time_s = 0.0, .position = {.x = 0.9, .y = 0.5, .z = 0.0}},          // far right
        {.time_s = hold_end, .position = {.x = 0.9, .y = 0.5, .z = 0.0}},     // still right
        {.time_s = hold_end + kJumpEpsilon, .position = {.x = 0.1, .y = 0.5, .z = 0.0}},  // jumped left
    });
    REQUIRE(path.has_value());
    const ac3::oba::ObjectPath object_path{*path};

    // --- Phase 1: encode + decode a real Atmos stream, accumulating exactly what decode.cpp's
    // own --adm output does. ---
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, 1};
    ac3::Eac3Decoder decoder;
    AdmAccumulator accumulator;

    // A real, distinct, non-silent tone - never silence/frame-0 (this project's own standing
    // lesson: those give false passes, see e.g. tests/cli/test_cli_atmos_adm.cpp's own fixture).
    std::vector<float> tone(static_cast<std::size_t>(kTotalFrames * kFrame));
    for (std::size_t i = 0; i < tone.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        tone[i] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 800.0 * t));
    }

    for (int f = 0; f < kTotalFrames; ++f) {
        const auto frame_start = static_cast<std::size_t>(f) * static_cast<std::size_t>(kFrame);
        const std::span<const float> object_signal{tone.data() + frame_start, static_cast<std::size_t>(kFrame)};
        const double t = static_cast<double>((f + 1) * kFrame) / kSampleRate;
        const auto placement = ac3::oba::evaluate_placements(std::span{&object_path, 1}, t);
        const std::array<std::span<const float>, 1> objects{object_signal};
        const auto unit = encoder.encode_frame(objects, placement);
        REQUIRE(unit.has_value());

        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        accumulator.add(**decoded);
    }

    // --- Phase 2: write a real ADM BWF master from what was decoded. ---
    ac3::admbridge::WriteInput write_input;
    write_input.sample_rate = 48000;
    write_input.channels.push_back({.name = "Object 1",
                                    .pcm = accumulator.object_pcm,
                                    .bed_label = std::nullopt,
                                    .updates = accumulator.object_updates});
    write_input.channels.push_back({.name = "LFE",
                                    .pcm = accumulator.lfe_pcm,
                                    .bed_label = ac3::oba::BedLabel::kLfe,
                                    .updates = {}});

    const auto built = ac3::admbridge::write(write_input);
    REQUIRE(built.has_value());
    CHECK(built->model.objects.size() == 2);
    CHECK(built->audio.channels.size() == 2);
    CHECK(built->audio.sample_rate == 48000);

    const auto scratch = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "admbridge_write";
    fs::create_directories(scratch);
    const auto master_path = (scratch / "write_roundtrip.wav").string();
    const auto written = ac3adm::write_bw64(master_path, *built);
    const std::string write_diag = written ? std::string{"ok"} : std::string(ac3adm::describe(written.error()));
    INFO("write_bw64: " << write_diag);
    REQUIRE(written.has_value());
    REQUIRE(fs::exists(master_path));
    CHECK(fs::file_size(master_path) > 0);

    // --- Phase 3: read it back with the exact same reader/bridge test_adm_bridge.cpp's own
    // flagship test already proves correct against a hand-authored fixture. ---
    const auto parsed = ac3adm::parse_bw64(master_path);
    const std::string parse_diag = parsed ? std::string{"ok"} : std::string(ac3adm::describe(parsed.error()));
    INFO("parse_bw64: " << parse_diag);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->audio.channels.size() == 2);
    REQUIRE(parsed->audio.frame_count() == accumulator.object_pcm.size());

    const auto bridged = ac3::admbridge::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 2);
    // Order matches model.objects' own insertion order (write()'s own doc comment): object,
    // then LFE.
    CHECK_FALSE(bridged->is_bed[0]);
    CHECK_FALSE(bridged->is_lfe[0]);
    CHECK(bridged->is_bed[1]);
    CHECK(bridged->is_lfe[1]);
    CHECK(bridged->sample_rate == 48000);

    // --- Phase 4: drive a FRESH encoder/decoder from the bridged result - the same standard
    // test_adm_bridge.cpp's own flagship test holds itself to - proving the position/audio the
    // written file carries is not just structurally present but actually reproduces the
    // original motion once re-encoded and re-decoded.
    ac3::oba::AtmosEncoder reencoder{{.bitrate_kbps = 448}, static_cast<int>(bridged->channel_count())};
    ac3::Eac3Decoder redecoder;
    std::vector<std::span<const float>> views(bridged->channel_count());

    // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.
    constexpr int kLCh = 0;
    constexpr int kRCh = 2;

    for (int f = 0; f < kTotalFrames; ++f) {
        const auto start = static_cast<std::size_t>(f) * static_cast<std::size_t>(kFrame);
        for (std::size_t i = 0; i < bridged->channel_count(); ++i) {
            views[i] = bridged->pcm[i].subspan(start, static_cast<std::size_t>(kFrame));
        }
        const double t = static_cast<double>(start + static_cast<std::size_t>(kFrame)) / kSampleRate;
        const auto placement = ac3::oba::evaluate_placements(bridged->paths, t);
        const auto unit = reencoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());

        if (f != 2 && f != 5) {
            continue;
        }
        const auto redecoded = redecoder.decode_access_unit(unit->bytes);
        REQUIRE(redecoded.has_value());
        REQUIRE(redecoded->has_value());

        const double energy_l = channel_energy((*redecoded)->channels[kLCh]);
        const double energy_r = channel_energy((*redecoded)->channels[kRCh]);
        CAPTURE(f, energy_l, energy_r);

        if (f == 2) {
            // Held at the far-right room position for [0, 0.096s) - frame 2 ends exactly at
            // 0.096s, still inside the hold (see this test's own kJumpEpsilon comment).
            CHECK(energy_r > 1.0);
            CHECK(energy_r > energy_l);
        } else {
            // Jumped to the far-left room position just after 0.096s, held afterward.
            CHECK(energy_l > 1.0);
            CHECK(energy_l > energy_r);
        }
    }
}
