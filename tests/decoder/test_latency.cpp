#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/latency.hpp"
#include "ac3/oba/atmos.hpp"

// Roadmap PF6: the latency budget, MEASURED rather than asserted from the
// header's arithmetic.
//
// Every case here puts a marker into the encoder's input at a known absolute
// sample position, decodes the whole stream, and locates the marker again in
// the output. The difference is the chain's algorithmic delay, and it is what
// LatencyBudget::transform_samples claims. A comment saying "one block of
// TDAC overlap" is not evidence; this file is.
//
// Two independent locators, because each fails differently. `peak_index`
// finds the largest |sample| - direct, and exactly right for an impulse into
// silence, but a quantizer that spread the impulse unevenly could move it.
// `best_lag` maximizes cross-correlation between input and output over a
// window of candidate lags - immune to that, works on ordinary content, and
// answers the same question. When both agree the number is not an artefact of
// how it was looked for.

namespace {

constexpr int kFrames = 8;
// Frame 3, one third of the way in: far enough from the start that every
// encoder's own priming (MDCT history, the transient detector's first-block
// suppression, the SNR search's warm start) is behind it, and far enough from
// the end that the tail of the chain's delay is still inside the stream.
constexpr int kImpulseAt = 3 * ac3::kSamplesPerFrame + 512;

std::vector<float> silence(int samples) {
    return std::vector<float>(static_cast<std::size_t>(samples), 0.0f);
}

// A single full-scale sample into silence. The most localized input a
// sample-accurate delay measurement can be given.
std::vector<float> impulse(int samples, int at) {
    auto pcm = silence(samples);
    pcm[static_cast<std::size_t>(at)] = 0.9f;
    return pcm;
}

// Ordinary content for the correlation locator: a decaying tone burst, which
// unlike an impulse gives the bit allocator something it codes well and
// unlike a steady tone is not self-similar under shift (a pure sinusoid
// correlates just as well at a lag of one period as at zero, which would make
// the measurement meaningless).
std::vector<float> burst(int samples, int at, double freq, std::uint32_t sample_rate) {
    auto pcm = silence(samples);
    for (int n = 0; n < 700; ++n) {
        const int index = at + n;
        if (index >= samples) {
            break;
        }
        const double t = static_cast<double>(n) / static_cast<double>(sample_rate);
        const double envelope = std::exp(-t * 90.0);
        pcm[static_cast<std::size_t>(index)] =
            static_cast<float>(0.7 * envelope * std::sin(2.0 * std::numbers::pi * freq * t));
    }
    return pcm;
}

int peak_index(std::span<const float> pcm) {
    int best = 0;
    float best_value = 0.0f;
    for (std::size_t n = 0; n < pcm.size(); ++n) {
        const float magnitude = std::abs(pcm[n]);
        if (magnitude > best_value) {
            best_value = magnitude;
            best = static_cast<int>(n);
        }
    }
    return best;
}

// The lag (output index - input index) maximizing sum(in[n] * out[n + lag]),
// searched over [0, max_lag]. Both signals are the same length; the sum runs
// only over indices valid in both.
int best_lag(std::span<const float> in, std::span<const float> out, int max_lag) {
    int best = 0;
    double best_score = -1.0;
    for (int lag = 0; lag <= max_lag; ++lag) {
        double score = 0.0;
        const auto limit = in.size() - static_cast<std::size_t>(lag);
        for (std::size_t n = 0; n < limit; ++n) {
            score += static_cast<double>(in[n]) *
                     static_cast<double>(out[n + static_cast<std::size_t>(lag)]);
        }
        if (score > best_score) {
            best_score = score;
            best = lag;
        }
    }
    return best;
}

// Whole-stream AC-3 round trip of one mono channel.
std::vector<float> ac3_round_trip(const ac3::EncoderConfig& config, std::span<const float> pcm) {
    ac3::FrameEncoder encoder{config};
    ac3::FrameDecoder decoder;
    std::vector<float> out;
    out.reserve(pcm.size());
    for (int frame = 0; frame * ac3::kSamplesPerFrame < static_cast<int>(pcm.size()); ++frame) {
        const std::span<const float> block =
            pcm.subspan(static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame,
                        static_cast<std::size_t>(ac3::kSamplesPerFrame));
        const std::array<std::span<const float>, 1> channels{block};
        const auto encoded = encoder.encode_frame(channels);
        REQUIRE(encoded.has_value());
        const auto decoded = decoder.decode_frame(*encoded);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 1);
        out.insert(out.end(), decoded->channels[0].begin(), decoded->channels[0].end());
    }
    return out;
}

// Whole-stream E-AC-3 round trip of one mono substream, honouring the §3.7
// hold-back: a call that returns nullopt contributed nothing this time round,
// and flush() collects whatever is still pending at the end. The returned PCM
// is therefore always in stream order and always the full length, whether or
// not the tool was in use - which is exactly what makes the hold-back show up
// as a RELEASE delay and not as a sample-domain shift.
struct Eac3RoundTrip {
    std::vector<float> pcm;
    int frames_held_back = 0;
    // Frame index at which decode_substream first reported a non-zero
    // hold-back, or -1 if it never did. §3.7 engages at the first frame that
    // actually sets transproce - which for this encoder means the first frame
    // that block-switches, not the first frame of the stream - so this is a
    // stronger claim than "it engaged eventually".
    int first_holdback_frame = -1;
    int latency_at_end = 0;
};

Eac3RoundTrip eac3_round_trip(const ac3::eac3::FrameConfig& config, std::span<const float> pcm) {
    ac3::eac3::FrameEncoder encoder{config};
    ac3::Eac3Decoder decoder;
    Eac3RoundTrip result;
    result.pcm.reserve(pcm.size());
    for (int frame = 0; frame * ac3::kSamplesPerFrame < static_cast<int>(pcm.size()); ++frame) {
        const std::span<const float> block =
            pcm.subspan(static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame,
                        static_cast<std::size_t>(ac3::kSamplesPerFrame));
        const std::array<std::span<const float>, 1> channels{block};
        const auto encoded = encoder.encode_frame(channels);
        REQUIRE(encoded.has_value());
        const auto decoded = decoder.decode_substream(*encoded);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            ++result.frames_held_back;
        } else {
            REQUIRE((*decoded)->channels.size() == 1);
            const auto& channel = (*decoded)->channels[0];
            result.pcm.insert(result.pcm.end(), channel.begin(), channel.end());
        }
        if (result.first_holdback_frame < 0 && decoder.latency_samples() != 0) {
            result.first_holdback_frame = frame;
        }
        result.latency_at_end = decoder.latency_samples();
    }
    for (const auto& pending : decoder.flush()) {
        REQUIRE(pending.channels.size() == 1);
        result.pcm.insert(result.pcm.end(), pending.channels[0].begin(),
                          pending.channels[0].end());
    }
    return result;
}

}  // namespace

TEST_CASE("AC-3 encode->decode delays the signal by exactly one transform overlap",
          "[latency]") {
    const int samples = kFrames * ac3::kSamplesPerFrame;
    const ac3::EncoderConfig config{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k1_0, .lfe = false};

    SECTION("impulse, located by peak") {
        const auto in = impulse(samples, kImpulseAt);
        const auto out = ac3_round_trip(config, in);
        REQUIRE(out.size() == in.size());
        REQUIRE(peak_index(out) == kImpulseAt + ac3::kTransformDelaySamples);
    }

    SECTION("tone burst, located by cross-correlation") {
        const auto in = burst(samples, kImpulseAt, 1200.0, 48000);
        const auto out = ac3_round_trip(config, in);
        REQUIRE(best_lag(in, out, 2 * ac3::kSamplesPerFrame) == ac3::kTransformDelaySamples);
    }
}

TEST_CASE("The AC-3 encoder reports the budget the round trip measures", "[latency]") {
    const ac3::FrameEncoder encoder{
        {.bitrate_kbps = 448, .acmod = ac3::Acmod::k1_0, .lfe = false}};
    const auto budget = encoder.latency();

    // The measured term, checked above.
    REQUIRE(budget.transform_samples == ac3::kTransformDelaySamples);
    // No lookahead: TransientDetector::detect() reads only the 256 new
    // samples of the block it decides, and AC-3 has no §3.7 hold-back.
    REQUIRE(budget.lookahead_samples == 0);
    REQUIRE(budget.holdback_samples == 0);
    REQUIRE(budget.frame_samples == ac3::kSamplesPerFrame);
    REQUIRE(encoder.latency_samples() == ac3::kSamplesPerFrame + ac3::kTransformDelaySamples);
    REQUIRE(ac3::FrameDecoder::latency_samples() == 0);

    // 1792 samples at 48 kHz. Spelled out so a change to any term has to
    // change a number a reader can check against docs/library/encoding-ac3.md.
    REQUIRE(encoder.latency_samples() == 1792);
    const double ms = ac3::latency_ms(budget, ac3::SampleRate::k48000);
    REQUIRE(ms > 37.3);
    REQUIRE(ms < 37.4);
}

TEST_CASE("The frame term really is the wait for a whole frame of input", "[latency]") {
    // The claim LatencyBudget::frame_samples makes is that nothing comes out
    // of the encoder until a full frame has gone in - so N frames of input
    // produce N frames of output and never N+1, whatever the content.
    const int samples = kFrames * ac3::kSamplesPerFrame;
    const auto in = impulse(samples, kImpulseAt);
    const auto out = ac3_round_trip({.bitrate_kbps = 448, .acmod = ac3::Acmod::k1_0}, in);
    REQUIRE(static_cast<int>(out.size()) == samples);
}

TEST_CASE("E-AC-3 without transient pre-noise processing has the same budget", "[latency]") {
    const int samples = kFrames * ac3::kSamplesPerFrame;
    const ac3::eac3::FrameConfig config{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k1_0, .lfe = false};

    const auto in = impulse(samples, kImpulseAt);
    const auto trip = eac3_round_trip(config, in);
    REQUIRE(trip.frames_held_back == 0);
    REQUIRE(trip.first_holdback_frame == -1);
    REQUIRE(trip.latency_at_end == 0);
    REQUIRE(static_cast<int>(trip.pcm.size()) == samples);
    REQUIRE(peak_index(trip.pcm) == kImpulseAt + ac3::kTransformDelaySamples);

    const ac3::eac3::FrameEncoder encoder{config};
    REQUIRE(encoder.latency().holdback_samples == 0);
    REQUIRE(encoder.latency_samples() == 1792);

    const auto burst_in = burst(samples, kImpulseAt, 1200.0, 48000);
    const auto burst_trip = eac3_round_trip(config, burst_in);
    REQUIRE(best_lag(burst_in, burst_trip.pcm, 2 * ac3::kSamplesPerFrame) ==
            ac3::kTransformDelaySamples);
}

TEST_CASE("Transient pre-noise processing costs one frame of decoder hold-back", "[latency]") {
    const int samples = kFrames * ac3::kSamplesPerFrame;
    ac3::eac3::FrameConfig config{
        .bitrate_kbps = 448, .acmod = ac3::Acmod::k1_0, .lfe = false};
    config.transient_prenoise = true;

    // An impulse into silence is exactly the case that block-switches, which
    // is what makes this encoder set transproce at all (eac3_frame.cpp reuses
    // the blksw decision rather than a second detector), so the tool really
    // does engage here rather than being configured and never used.
    const auto in = impulse(samples, kImpulseAt);
    const auto trip = eac3_round_trip(config, in);

    REQUIRE(trip.frames_held_back == 1);
    // Not frame 0: three frames of silence come first, and §8.2.2 step 4's
    // silence gate keeps the detector quiet through them, so transproce is
    // clear and every one of those frames releases immediately. The tool -
    // and with it the hold-back - engages exactly at the frame the impulse
    // lands in, and stays engaged for the rest of the stream.
    REQUIRE(trip.first_holdback_frame == kImpulseAt / ac3::kSamplesPerFrame);
    REQUIRE(trip.latency_at_end == ac3::kSamplesPerFrame);
    // The hold-back is a RELEASE delay, not a sample shift: flush() puts the
    // held frame back at the end of the stream, so the whole stream is still
    // the same length and the impulse is still at the same place in it.
    REQUIRE(static_cast<int>(trip.pcm.size()) == samples);
    REQUIRE(peak_index(trip.pcm) == kImpulseAt + ac3::kTransformDelaySamples);

    const ac3::eac3::FrameEncoder encoder{config};
    REQUIRE(encoder.latency().holdback_samples == ac3::kSamplesPerFrame);
    REQUIRE(encoder.latency_samples() == 1792 + ac3::kSamplesPerFrame);
}

TEST_CASE("An access unit's budget is the worst its substreams impose", "[latency]") {
    ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 384, .acmod = ac3::Acmod::k3_2, .lfe = true};
    ac3::eac3::FrameConfig dependent{
        .bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .lfe = false};
    dependent.strmtyp = ac3::eac3::StreamType::kDependent;
    dependent.chanmap = 0x0180;
    dependent.transient_prenoise = true;
    config.dependents.push_back(dependent);

    const ac3::eac3::AccessUnitEncoder encoder{config};
    // The frame and transform terms are shared - every substream codes the
    // same 1536 samples - so only the hold-back can differ, and the unit
    // cannot be assembled until the latest substream has released.
    REQUIRE(encoder.latency().frame_samples == ac3::kSamplesPerFrame);
    REQUIRE(encoder.latency().transform_samples == ac3::kTransformDelaySamples);
    REQUIRE(encoder.latency().holdback_samples == ac3::kSamplesPerFrame);
}

TEST_CASE("JOC object reconstruction costs a second transform overlap", "[latency]") {
    // The claim: an object waveform lags its input by 512 samples, not 256,
    // because joc::reconstruct runs a whole second MDCT/IMDCT round trip over
    // the bed the decoder has already reconstructed. Measured end to end
    // through the real object path.
    constexpr int kObjects = 1;
    const int samples = kFrames * ac3::kSamplesPerFrame;
    const auto in = burst(samples, kImpulseAt, 900.0, 48000);

    ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
    ac3::Eac3Decoder decoder;

    const ac3::oba::ObjectPlacement placement{
        .position = {.x = 0.5, .y = 0.9, .z = 0.0}, .gain = 1.0};
    // The bed's channels individually depend on where the object was panned,
    // and a near-silent one carries nothing to correlate against - so measure
    // the bed's delay on the SUM of its channels. Panning gains are
    // non-negative, so the sum is the input scaled by their total and delayed
    // by whatever the chain delays it, which is the only thing under test.
    std::vector<float> bed_out;
    std::vector<float> object_out;
    for (int frame = 0; frame * ac3::kSamplesPerFrame < samples; ++frame) {
        const std::span<const float> block{
            in.data() + static_cast<std::size_t>(frame) * ac3::kSamplesPerFrame,
            static_cast<std::size_t>(ac3::kSamplesPerFrame)};
        const std::array<std::span<const float>, 1> objects{block};
        const std::array<ac3::oba::ObjectPlacement, 1> placements{placement};
        const auto unit = encoder.encode_frame(objects, placements);
        REQUIRE(unit.has_value());
        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto& au = **decoded;
        REQUIRE(!au.channels.empty());
        REQUIRE(au.object_audio.size() == 1);
        const std::size_t base = bed_out.size();
        bed_out.resize(base + static_cast<std::size_t>(ac3::kSamplesPerFrame), 0.0f);
        for (const auto& channel : au.channels) {
            REQUIRE(channel.size() == static_cast<std::size_t>(ac3::kSamplesPerFrame));
            for (std::size_t n = 0; n < channel.size(); ++n) {
                bed_out[base + n] += channel[n];
            }
        }
        object_out.insert(object_out.end(), au.object_audio[0].begin(),
                          au.object_audio[0].end());
    }
    // Both signals must actually carry the burst, or a lag search over them
    // is measuring noise.
    REQUIRE(peak_index(bed_out) > 0);
    REQUIRE(peak_index(object_out) > 0);

    REQUIRE(best_lag(in, bed_out, 3 * ac3::kSamplesPerFrame) == ac3::kTransformDelaySamples);
    REQUIRE(best_lag(in, object_out, 3 * ac3::kSamplesPerFrame) ==
            2 * ac3::kTransformDelaySamples);

    REQUIRE(encoder.bed_latency().transform_samples == ac3::kTransformDelaySamples);
    REQUIRE(encoder.latency().transform_samples == 2 * ac3::kTransformDelaySamples);
    REQUIRE(encoder.latency_samples() == ac3::kSamplesPerFrame + 512);
}

TEST_CASE("An Atmos stream with no container is a plain bed and costs no second transform",
          "[latency]") {
    ac3::oba::AtmosConfig config{.bitrate_kbps = 448};
    config.emit_object_metadata = false;
    const ac3::oba::AtmosEncoder encoder{config, 2};
    REQUIRE(encoder.latency().transform_samples == ac3::kTransformDelaySamples);
    REQUIRE(encoder.latency_samples() == encoder.bed_latency().total_samples());
}

TEST_CASE("latency_ms converts at the coded rate, not a fixed 48 kHz", "[latency]") {
    const ac3::LatencyBudget budget{};
    REQUIRE(budget.total_samples() == 1792);
    // 1792 / 44100 = 40.63 ms; the eight-tenths of a millisecond between this
    // and the 48 kHz figure is exactly what a sync budget would otherwise
    // silently lose.
    const double at_44k = ac3::latency_ms(budget, ac3::SampleRate::k44100);
    REQUIRE(at_44k > 40.6);
    REQUIRE(at_44k < 40.7);
    REQUIRE(ac3::latency_ms(budget, ac3::SampleRate::k32000) > 55.9);
}
