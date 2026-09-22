// When the LFE a rendered object programme plays arrives, against the objects
// it plays beside - measured end to end, on a stream this project's own
// encoder writes.
//
// JOC does not code objects: it codes a matrix that pulls them back out of the
// decoded bed, so a reconstructed object comes out
// oba::joc::reconstruction_delay() samples after the bed it was pulled from -
// 576 under Domain::kQmf, 256 under Domain::kMdctBand (docs/library/decoding.md,
// "Atmos objects lag the bed"; tests/decoder/test_latency.cpp measures both
// halves against the encoder's input). LayoutRenderer::render() places the
// objects and passes the bed's LFE through beside them, so the LFE has to wait
// for them.
//
// The stream is one object at the front centre that also sends all of itself
// to the LFE, so the same pulse enters the programme at the same instant in
// both. Rendered onto 5.1, the object lands on the centre and the LFE on the
// LFE feed, and the lag between those two feeds is what a listener hears.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/latency.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/render.hpp"
#include "ac3/render/serving.hpp"

namespace {

using ac3::oba::joc::Domain;
using ac3::render::LayoutRenderer;
using ac3::render::OutputLayout;
using Location = ac3::eac3::chanmap::Location;

constexpr std::uint16_t k51 = ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, true);
// A genuinely different bed from k51's: the LFE sits at coded index 7 rather
// than 5 (test_layout.cpp's "a channel the layout lacks is panned..." case
// has the same figures), used to tell set_bed() apart from a mere repeat.
constexpr std::uint16_t k71 =
    static_cast<std::uint16_t>(k51 | ac3::eac3::chanmap::k71Rear);

constexpr int kFrames = 8;
// Frame 3, as tests/decoder/test_latency.cpp places its marker: past every
// encoder's and decoder's priming, and early enough that the longest delay
// here still leaves the pulse well inside the stream.
constexpr int kPulseAt = 3 * ac3::kSamplesPerFrame + 512;
// The stretch the correlations run over: the pulse, and everything the chain
// can have moved it to.
constexpr int kWindowFrom = kPulseAt - 2048;
constexpr int kWindowTo = kPulseAt + 4096;

// The object's audio: a pulse the LFE channel carries whole, over a quiet
// floor that runs the length of the stream.
//
// The pulse is the first derivative of a Gaussian with sigma 1 ms, peaking
// near 160 Hz, with no DC and nothing above the LFE's seven coded bins (656 Hz
// at 48 kHz) but -57 dB. A tone burst would not do: it correlates with itself
// a period away almost as well as at no lag.
//
// The floor is there for the reconstruction matrix. The encoder solves each
// frame's matrix from the object's energy in that frame, a silent frame gets
// a zero matrix, and §6.6.5 interpolates each frame's from the one before - so
// a pulse after silence would come back through a matrix still rising from
// zero across it, reshaped. With energy in every frame the matrix holds still.
// Low-passed noise at about -40 dB: it takes the same paths as the pulse, so a
// correlation finds it at the same lag.
std::vector<float> programme(int samples, int at) {
    std::vector<float> pcm(static_cast<std::size_t>(samples), 0.0F);
    std::uint32_t state = 0x2545F491U;
    constexpr double kPole = 0.02;  // two one-pole low-passes near 150 Hz
    double stage1 = 0.0;
    double stage2 = 0.0;
    for (float& sample : pcm) {
        state = (state * 1664525U) + 1013904223U;
        const double white = (static_cast<double>(state >> 8U) / 8388608.0) - 1.0;
        stage1 += kPole * (white - stage1);
        stage2 += kPole * (stage1 - stage2);
        sample = static_cast<float>(0.25 * stage2);
    }
    constexpr int kSigma = 48;
    for (int n = -6 * kSigma; n <= 6 * kSigma; ++n) {
        const double x = static_cast<double>(n) / kSigma;
        pcm[static_cast<std::size_t>(at + n)] +=
            static_cast<float>(-0.5 * x * std::exp(0.5 - (0.5 * x * x)));
    }
    return pcm;
}

// The lag in [min_lag, max_lag] maximising sum(earlier[n] * later[n + lag])
// over the window: how far `later` trails `earlier`, negative when it leads.
int best_lag(std::span<const float> earlier, std::span<const float> later, int min_lag,
             int max_lag) {
    const int size = static_cast<int>(std::min(earlier.size(), later.size()));
    int best = min_lag;
    double best_score = -std::numeric_limits<double>::infinity();
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        const int from = std::max({kWindowFrom, 0, -lag});
        const int to = std::min({kWindowTo, size, size - lag});
        double score = 0.0;
        for (int n = from; n < to; ++n) {
            score += static_cast<double>(earlier[static_cast<std::size_t>(n)]) *
                     static_cast<double>(later[static_cast<std::size_t>(n + lag)]);
        }
        if (score > best_score) {
            best_score = score;
            best = lag;
        }
    }
    return best;
}

float peak(std::span<const float> pcm) {
    float out = 0.0F;
    for (std::size_t n = static_cast<std::size_t>(kWindowFrom); n < pcm.size(); ++n) {
        out = std::max(out, std::abs(pcm[n]));
    }
    return out;
}

// The two feeds a listener compares, over the whole stream.
struct Rendered {
    std::vector<float> speakers;  // every full-range slot, summed
    std::vector<float> lfe;       // the LFE feed
    std::size_t units = 0;
    std::size_t units_with_objects = 0;
};

// Encodes `in` as the one object, decodes it a block at a time as the players
// do, and renders every block onto 5.1 - placing the objects when `objects`,
// the bed otherwise.
Rendered encode_decode_render(std::span<const float> in, Domain domain, bool objects) {
    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .joc_domain = domain}, 1};
    const ac3::oba::ObjectPlacement placement{
        .position = {.x = 0.5, .y = 0.0, .z = 0.0}, .gain = 1.0, .lfe_send = 1.0};

    const OutputLayout layout = *OutputLayout::named("5.1");
    const ac3::render::Serving serving = ac3::render::serve(
        layout, ac3::DownmixTarget::kLoRo,
        objects ? ac3::render::ObjectsPolicy::kAlways : ac3::render::ObjectsPolicy::kNever);
    REQUIRE_FALSE(serving.fold.has_value());
    REQUIRE(serving.reconstruct == objects);
    ac3::DecoderConfig config;
    config.joc_domain = domain;
    ac3::render::configure_decoder(serving, config);
    ac3::Eac3Decoder decoder{config};

    const ac3::eac3::chanmap::Layout bed = ac3::eac3::chanmap::expand(k51);
    LayoutRenderer renderer{layout};
    renderer.set_joc_domain(domain);
    renderer.set_bed(bed);

    const auto lfe_slot = static_cast<std::size_t>(layout.index_of(Location::kLfe));
    std::vector<std::array<float, ac3::kSamplesPerBlock>> block(layout.slots());
    std::vector<std::span<float>> spans(block.begin(), block.end());

    Rendered out;
    const auto frame = static_cast<std::size_t>(ac3::kSamplesPerFrame);
    for (std::size_t start = 0; start < in.size(); start += frame) {
        const std::array<std::span<const float>, 1> audio{in.subspan(start, frame)};
        const std::array<ac3::oba::ObjectPlacement, 1> placements{placement};
        const auto unit = encoder.encode_frame(audio, placements);
        REQUIRE(unit.has_value());
        bool carried_objects = false;
        const auto decoded =
            decoder.decode_access_unit_by_block(unit->bytes, [&](const ac3::PcmBlock& pcm) {
                if (pcm.index == 0 && serving.reconstruct) {
                    renderer.set_objects(pcm.object_metadata, pcm.objects.size());
                }
                carried_objects = carried_objects || !pcm.objects.empty();
                renderer.render(pcm, serving.reconstruct, 1.0F, spans);
                const std::size_t n = pcm.channels.front().size();
                for (std::size_t k = 0; k < n; ++k) {
                    float speakers = 0.0F;
                    for (std::size_t slot = 0; slot < block.size(); ++slot) {
                        if (slot != lfe_slot) {
                            speakers += block[slot][k];
                        }
                    }
                    out.speakers.push_back(speakers);
                    out.lfe.push_back(block[lfe_slot][k]);
                }
            });
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac3::DecodedAccessUnit& au = **decoded;
        REQUIRE(au.layout.count == bed.count);
        REQUIRE(au.layout.index_of(Location::kLfe) == bed.index_of(Location::kLfe));
        ++out.units;
        if (carried_objects) {
            ++out.units_with_objects;
        }
    }
    REQUIRE(out.speakers.size() == in.size());
    return out;
}

// For the delay line itself, sample by sample: a programme whose every channel
// is silent but the LFEs, each a ramp that names its sample (n + 1 for the LFE,
// 10000 + n + 1 for a second one), and an object that is the negated ramp. A
// delay then shows as exactly which sample came out.
struct Ramps {
    ac3::eac3::chanmap::Layout coded;
    std::vector<std::vector<float>> channels;  // coded order
    std::vector<float> object;
};

Ramps ramps(std::uint16_t map, std::size_t samples) {
    Ramps out;
    out.coded = ac3::eac3::chanmap::expand(map);
    out.channels.assign(static_cast<std::size_t>(out.coded.count),
                        std::vector<float>(samples, 0.0F));
    for (int c = 0; c < out.coded.count; ++c) {
        const Location location = out.coded[c];
        if (location != Location::kLfe && location != Location::kLfe2) {
            continue;
        }
        const float base = location == Location::kLfe ? 0.0F : 10000.0F;
        for (std::size_t n = 0; n < samples; ++n) {
            out.channels[static_cast<std::size_t>(c)][n] = base + static_cast<float>(n + 1);
        }
    }
    out.object.resize(samples);
    for (std::size_t n = 0; n < samples; ++n) {
        out.object[n] = -static_cast<float>(n + 1);
    }
    return out;
}

// What `renderer` makes of `in`, `block` samples at a time, one output per
// slot end to end. Block b carries the object when `carries(b)` says so, and
// render() is asked to place objects when `objects` says so.
template <class Carries>
std::vector<std::vector<float>> play(LayoutRenderer& renderer, const Ramps& in, std::size_t block,
                                     Carries carries, bool objects = true) {
    const std::size_t samples = in.object.size();
    std::vector<std::vector<float>> out(renderer.layout().slots(),
                                        std::vector<float>(samples, 99.0F));
    std::size_t index = 0;
    for (std::size_t start = 0; start < samples; start += block, ++index) {
        const std::size_t n = std::min(block, samples - start);
        std::vector<std::span<const float>> channels;
        for (const std::vector<float>& channel : in.channels) {
            channels.emplace_back(channel.data() + start, n);
        }
        const std::array<std::span<const float>, 1> object{
            std::span<const float>(in.object.data() + start, n)};
        std::vector<std::span<float>> spans;
        for (std::vector<float>& slot : out) {
            spans.emplace_back(slot.data() + start, n);
        }
        const bool carried = carries(index);
        const ac3::PcmBlock pcm{
            .index = 0,
            .blocks = 1,
            .channels = channels,
            .objects = carried ? std::span<const std::span<const float>>(object)
                               : std::span<const std::span<const float>>{},
            .object_indices = {},
            .object_metadata = nullptr};
        renderer.render(pcm, objects, 1.0F, spans);
    }
    return out;
}

const auto kEveryBlock = [](std::size_t) { return true; };

// `in` as it arrives, or held back by `lag` behind silence.
float ramp_at(std::size_t n, std::size_t lag, float base = 0.0F) {
    return n < lag ? 0.0F : base + static_cast<float>(n - lag + 1);
}

// The first sample at which `got` is not `want(n)`, or -1.
template <class Want>
long first_difference(const std::vector<float>& got, Want want) {
    for (std::size_t n = 0; n < got.size(); ++n) {
        if (got[n] != want(n)) {
            return static_cast<long>(n);
        }
    }
    return -1;
}

std::array<ac3::oba::DisplayObject, 1> at_the_centre() {
    ac3::oba::DisplayObject object;
    object.position = {.x = 0.5, .y = 0.0, .z = 0.0};
    return {object};
}

}  // namespace

TEST_CASE("the bed's LFE and its other channels arrive together", "[render][latency]") {
    // The control: with the objects declined, render() places the bed as
    // coded, and everything in it shares the one transform delay. The LFE
    // channel's narrow coded band does not move where a correlation finds the
    // pulse - which is what makes the measurement below mean anything.
    const auto in = programme(kFrames * ac3::kSamplesPerFrame, kPulseAt);
    const Rendered bed = encode_decode_render(in, Domain::kQmf, false);
    REQUIRE(bed.units_with_objects == 0);
    REQUIRE(peak(bed.speakers) > 0.25F);
    REQUIRE(peak(bed.lfe) > 0.25F);

    CHECK(best_lag(in, bed.speakers, 0, 2 * ac3::kSamplesPerFrame) ==
          ac3::kTransformDelaySamples);
    CHECK(best_lag(in, bed.lfe, 0, 2 * ac3::kSamplesPerFrame) == ac3::kTransformDelaySamples);
    CHECK(best_lag(bed.lfe, bed.speakers, -ac3::kSamplesPerFrame, ac3::kSamplesPerFrame) == 0);
}

TEST_CASE("the objects' LFE arrives with the objects", "[render][latency]") {
    const Domain domain = GENERATE(Domain::kQmf, Domain::kMdctBand);
    const int object_lag =
        ac3::kTransformDelaySamples + ac3::oba::joc::reconstruction_delay(domain);
    CAPTURE(domain == Domain::kQmf ? "kQmf" : "kMdctBand", object_lag);

    const auto in = programme(kFrames * ac3::kSamplesPerFrame, kPulseAt);
    const Rendered placed = encode_decode_render(in, domain, true);
    REQUIRE(placed.units_with_objects == placed.units);
    REQUIRE(peak(placed.speakers) > 0.25F);
    REQUIRE(peak(placed.lfe) > 0.25F);

    const int to_speakers = best_lag(in, placed.speakers, 0, 2 * ac3::kSamplesPerFrame);
    const int to_lfe = best_lag(in, placed.lfe, 0, 2 * ac3::kSamplesPerFrame);
    const int lfe_to_speakers =
        best_lag(placed.lfe, placed.speakers, -ac3::kSamplesPerFrame, ac3::kSamplesPerFrame);
    CAPTURE(to_speakers, to_lfe, lfe_to_speakers);
    // The objects are where test_latency.cpp says a reconstructed object is.
    CHECK(to_speakers == object_lag);
    // And the LFE with them, not a reconstruction delay ahead.
    CHECK(to_lfe == object_lag);
    CHECK(lfe_to_speakers == 0);
}

TEST_CASE("the renderer's lag is the one the decoder's default domain has", "[render]") {
    const LayoutRenderer renderer{*OutputLayout::named("5.1")};
    CHECK(renderer.object_lag() == static_cast<std::size_t>(ac3::oba::joc::reconstruction_delay(
                                       ac3::DecoderConfig{}.joc_domain)));
}

TEST_CASE("render holds the bed's LFE back by the objects' lag, whatever the block length",
          "[render]") {
    const Domain domain = GENERATE(Domain::kQmf, Domain::kMdctBand);
    // A block's worth, one that divides neither lag, and one longer than both.
    const std::size_t block = GENERATE(std::size_t{256}, std::size_t{100}, std::size_t{2048});
    const auto lag = static_cast<std::size_t>(ac3::oba::joc::reconstruction_delay(domain));
    CAPTURE(lag, block);

    LayoutRenderer renderer{*OutputLayout::named("5.1")};  // L C R Ls Rs LFE
    renderer.set_joc_domain(domain);
    REQUIRE(renderer.object_lag() == lag);
    const Ramps in = ramps(k51, 4096);
    renderer.set_bed(in.coded);
    renderer.set_objects(at_the_centre());
    const auto out = play(renderer, in, block, kEveryBlock);

    CHECK(first_difference(out[5], [&](std::size_t n) { return ramp_at(n, lag); }) == -1);
    // The objects themselves are not moved.
    CHECK(first_difference(out[1], [&](std::size_t n) { return in.object[n]; }) == -1);
    for (const std::size_t silent : {0U, 2U, 3U, 4U}) {
        CHECK(first_difference(out[silent], [](std::size_t) { return 0.0F; }) == -1);
    }
}

TEST_CASE("a unit without objects plays its LFE as it arrives, and the line keeps it",
          "[render]") {
    LayoutRenderer renderer{*OutputLayout::named("5.1")};
    const std::size_t lag = renderer.object_lag();
    const Ramps in = ramps(k51, 4096);
    renderer.set_bed(in.coded);
    renderer.set_objects(at_the_centre());
    // Blocks 4 to 6 - samples 1024 to 1791 - carry no objects, so the bed is
    // placed there, LFE and all, as it arrives. The objects after them then
    // find the LFE they go with, which the bed has already played: a unit
    // without objects is a jump for every slot, not a gap in the LFE alone.
    const auto gap = [](std::size_t b) { return b >= 4 && b <= 6; };
    const auto out = play(renderer, in, 256, [&](std::size_t b) { return !gap(b); });
    CHECK(first_difference(out[5], [&](std::size_t n) {
              return gap(n / 256) ? ramp_at(n, 0) : ramp_at(n, lag);
          }) == -1);
    CHECK(first_difference(out[1], [&](std::size_t n) {
              return gap(n / 256) ? 0.0F : in.object[n];
          }) == -1);
}

TEST_CASE("a renderer not asked for objects plays the LFE as it arrives", "[render]") {
    LayoutRenderer renderer{*OutputLayout::named("5.1")};
    const Ramps in = ramps(k51, 2048);
    renderer.set_bed(in.coded);
    renderer.set_objects(at_the_centre());
    const auto out = play(renderer, in, 256, kEveryBlock, false);
    CHECK(first_difference(out[5], [](std::size_t n) { return ramp_at(n, 0); }) == -1);
    CHECK(first_difference(out[1], [](std::size_t) { return 0.0F; }) == -1);  // the bed's silent C
}

TEST_CASE("each LFE has its own line", "[render]") {
    // Two coded LFEs to a room with two feeds; coded order puts LFE2 first.
    const auto map = static_cast<std::uint16_t>(k51 | ac3::eac3::chanmap::kLfe2Bit);
    LayoutRenderer renderer{*OutputLayout::named("5.2")};
    const std::size_t lag = renderer.object_lag();
    const Ramps in = ramps(map, 4096);
    renderer.set_bed(in.coded);
    renderer.set_objects(at_the_centre());
    const auto out = play(renderer, in, 256, kEveryBlock);
    const auto lfe = static_cast<std::size_t>(renderer.layout().index_of(Location::kLfe));
    const auto lfe2 = static_cast<std::size_t>(renderer.layout().index_of(Location::kLfe2));
    CHECK(first_difference(out[lfe], [&](std::size_t n) { return ramp_at(n, lag); }) == -1);
    CHECK(first_difference(out[lfe2], [&](std::size_t n) { return ramp_at(n, lag, 10000.0F); }) ==
          -1);
}

TEST_CASE("reset, a real bed change and a new domain each empty the line; a repeat set_bed does not",
          "[render]") {
    LayoutRenderer renderer{*OutputLayout::named("5.1")};
    const Ramps in = ramps(k51, 1024);
    renderer.set_bed(in.coded);
    renderer.set_objects(at_the_centre());
    const std::size_t lag = renderer.object_lag();
    (void)play(renderer, in, 256, kEveryBlock);

    SECTION("left alone, the next stream starts with this one's tail") {
        const auto out = play(renderer, in, 256, kEveryBlock);
        CHECK(out[5][0] == static_cast<float>(1024 - lag + 1));
    }
    SECTION("set_bed called again with the same bed changes nothing") {
        // tests/hearth/test_group.cpp's decode_and_render calls set_bed() on
        // every unit's first block whatever the bed is; BurstOutput::place()
        // instead guards the call with same_layout() and only calls it when
        // the bed changes. Two renderers fed the same programme through the
        // two styles of caller have to agree, so a repeat set_bed() must
        // leave the line exactly where render() left it - the regression
        // PR #722 fixed after the two disagreed under CI's Hearth leg.
        renderer.set_bed(in.coded);
        const auto out = play(renderer, in, 256, kEveryBlock);
        CHECK(out[5][0] == static_cast<float>(1024 - lag + 1));
    }
    SECTION("set_bed with a real LFE-topology change empties the line") {
        const Ramps different = ramps(k71, 1024);  // LFE moves from coded index 5 to 7
        renderer.set_bed(different.coded);
        const auto out = play(renderer, different, 256, kEveryBlock);
        CHECK(first_difference(out[5], [&](std::size_t n) { return ramp_at(n, lag); }) == -1);
    }
    SECTION("reset") {
        renderer.reset();
        const auto out = play(renderer, in, 256, kEveryBlock);
        CHECK(first_difference(out[5], [&](std::size_t n) { return ramp_at(n, lag); }) == -1);
    }
    SECTION("set_joc_domain") {
        renderer.set_joc_domain(Domain::kMdctBand);
        REQUIRE(renderer.object_lag() == 256);
        const auto out = play(renderer, in, 256, kEveryBlock);
        CHECK(first_difference(out[5], [](std::size_t n) { return ramp_at(n, 256); }) == -1);
    }
}

TEST_CASE("two renderers of the same programme agree whether or not the caller repeats set_bed",
          "[render]") {
    // The exact shape of the regression PR #722's own fix caught in CI's
    // Hearth leg rather than in this file: tests/hearth/test_group.cpp's
    // decode_and_render calls set_bed() on every unit's first block whatever
    // the bed is; apps/hearth/testsink/burst_output.cpp's BurstOutput::place()
    // instead guards the call with same_layout() and skips a repeat. Both
    // decode and render the SAME programme, so their LFE output has to be the
    // same sample for sample - a set_bed() that unconditionally emptied the
    // delay line broke that: the two renderers took the reset at different
    // rates across the four units below and diverged from the first one on.
    constexpr std::size_t kUnits = 4;
    constexpr std::size_t kUnitSamples = 768;  // multiple units: a divergence compounds
    const Ramps in = ramps(k51, kUnits * kUnitSamples);

    LayoutRenderer repeats_set_bed{*OutputLayout::named("5.1")};
    LayoutRenderer guards_set_bed{*OutputLayout::named("5.1")};
    repeats_set_bed.set_bed(in.coded);
    guards_set_bed.set_bed(in.coded);
    repeats_set_bed.set_objects(at_the_centre());
    guards_set_bed.set_objects(at_the_centre());

    std::vector<std::vector<float>> from_repeats(6, std::vector<float>(in.object.size(), 99.0F));
    std::vector<std::vector<float>> from_guarded(6, std::vector<float>(in.object.size(), 99.0F));
    for (std::size_t unit = 0; unit < kUnits; ++unit) {
        const std::size_t start = unit * kUnitSamples;
        repeats_set_bed.set_bed(in.coded);  // every unit, as decode_and_render does
        // guards_set_bed never calls set_bed again: the bed never changes, so
        // BurstOutput's same_layout() guard would have skipped every repeat.
        std::vector<std::span<const float>> channels;
        for (const std::vector<float>& channel : in.channels) {
            channels.emplace_back(channel.data() + start, kUnitSamples);
        }
        const std::array<std::span<const float>, 1> object{
            std::span<const float>(in.object.data() + start, kUnitSamples)};
        const ac3::PcmBlock pcm{.index = 0,
                                .blocks = 1,
                                .channels = channels,
                                .objects = object,
                                .object_indices = {},
                                .object_metadata = nullptr};
        std::vector<std::span<float>> repeats_spans;
        std::vector<std::span<float>> guarded_spans;
        for (std::vector<float>& slot : from_repeats) {
            repeats_spans.emplace_back(slot.data() + start, kUnitSamples);
        }
        for (std::vector<float>& slot : from_guarded) {
            guarded_spans.emplace_back(slot.data() + start, kUnitSamples);
        }
        repeats_set_bed.render(pcm, true, 1.0F, repeats_spans);
        guards_set_bed.render(pcm, true, 1.0F, guarded_spans);
    }
    CHECK(from_repeats[5] == from_guarded[5]);
    CHECK(from_repeats[1] == from_guarded[1]);  // the objects too, for good measure
}
