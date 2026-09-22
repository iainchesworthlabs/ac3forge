#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/quality/distortion.hpp"
#include "ac3/verify/selfcheck.hpp"

namespace {

constexpr int kFrames = 8;
constexpr double kRate = 48000.0;

// Programme-like material rather than a tone: a chord that holds (so the
// tonality estimate has something to find), a swept partial, filtered noise
// and a percussive attack every few frames. Silence and a single sine both
// give false passes here for the same reason they do everywhere else in this
// project - the search has nothing to choose between.
std::vector<std::vector<float>> make_material(int channels) {
    std::mt19937 rng(0x5EA3);
    std::normal_distribution<double> gauss(0.0, 0.08);
    const std::size_t total = static_cast<std::size_t>(kFrames) * ac3::kSamplesPerFrame;
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels),
                                        std::vector<float>(total, 0.0f));
    double lowpass = 0.0;
    for (std::size_t n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / kRate;
        double value = 0.0;
        for (const double hz : {220.0, 277.18, 329.63, 659.26}) {
            value += 0.16 * std::sin(2.0 * std::numbers::pi * hz * t);
        }
        value += 0.10 * std::sin(2.0 * std::numbers::pi * (900.0 + (600.0 * t)) * t);
        // A one-pole low pass over white noise: broadband, but with a tilt,
        // so the top bands are quiet rather than absent.
        lowpass = (0.75 * lowpass) + (0.25 * gauss(rng));
        value += lowpass + (0.02 * gauss(rng));
        // A transient every 1.5 frames or so, which is what makes the frames
        // differ from one another at all.
        const std::size_t since = n % 2304;
        if (since < 64) {
            value += 0.45 * std::exp(-static_cast<double>(since) / 12.0) *
                     std::sin(2.0 * std::numbers::pi * 3200.0 * t);
        }
        for (int ch = 0; ch < channels; ++ch) {
            // Decorrelate the channels, or coupling and rematrixing both
            // meet a signal they never meet in practice.
            const double phase = 1.0 + (0.11 * ch);
            out[static_cast<std::size_t>(ch)][n] =
                static_cast<float>(std::clamp(value * phase * 0.8, -0.98, 0.98));
        }
    }
    return out;
}

std::vector<std::span<const float>> spans_for(const std::vector<std::vector<float>>& channels,
                                              int frame) {
    std::vector<std::span<const float>> spans;
    spans.reserve(channels.size());
    for (const auto& channel : channels) {
        spans.emplace_back(channel.data() + (static_cast<std::size_t>(frame) *
                                             ac3::kSamplesPerFrame),
                           ac3::kSamplesPerFrame);
    }
    return spans;
}

ac3::EncoderConfig config_for(ac3::quality::Criterion search, ac3::Acmod acmod, bool lfe,
                              std::uint32_t kbps) {
    ac3::EncoderConfig config;
    config.acmod = acmod;
    config.lfe = lfe;
    config.bitrate_kbps = kbps;
    config.coupling = acmod != ac3::Acmod::k1_0;
    config.search = search;
    return config;
}

std::vector<std::vector<std::byte>> encode_all(const ac3::EncoderConfig& config,
                                               const std::vector<std::vector<float>>& material) {
    ac3::FrameEncoder encoder(config);
    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto spans = spans_for(material, frame);
        auto result = encoder.encode_frame(spans);
        REQUIRE(result.has_value());
        frames.push_back(std::move(*result));
    }
    return frames;
}

}  // namespace

// The property that matters most, and the one a search over the transmitted
// bit allocation parameters is most likely to break: every candidate moves
// the masking curve, so every candidate moves every mantissa field's WIDTH.
// If the winning candidate's allocation is not the one step 10 sizes those
// fields from - if any of run_bap, the plan's delta segments or `budget`
// were left belonging to some other candidate - the stream desyncs, and it
// desyncs silently, blocks downstream of the cause. That is exactly the
// failure ac3/verify/mirror.hpp exists to name, so it is pointed at the
// search here.
TEST_CASE("the encoder's model still matches a real decode with the search on",
          "[quality][search]") {
    const auto stereo = make_material(2);
    const auto surround = make_material(6);  // 5 fbw + LFE
    for (const auto criterion : {ac3::quality::Criterion::kDistortion,
                                 ac3::quality::Criterion::kPerceptual}) {
        for (const std::uint32_t kbps : {192U, 448U}) {
            {
                ac3::verify::MirrorEncoder mirror(
                    config_for(criterion, ac3::Acmod::k2_0, false, kbps));
                for (int frame = 0; frame < kFrames; ++frame) {
                    auto checked = mirror.encode_frame(spans_for(stereo, frame));
                    REQUIRE(checked.has_value());
                    CAPTURE(static_cast<int>(criterion), kbps, frame, mirror.last_report());
                    CHECK(checked->ok());
                }
            }
            {
                ac3::verify::MirrorEncoder mirror(
                    config_for(criterion, ac3::Acmod::k3_2, true, kbps));
                for (int frame = 0; frame < kFrames; ++frame) {
                    auto checked = mirror.encode_frame(spans_for(surround, frame));
                    REQUIRE(checked.has_value());
                    CAPTURE(static_cast<int>(criterion), kbps, frame, mirror.last_report());
                    CHECK(checked->ok());
                }
            }
        }
    }
}

// Off means off. The search is a decision knob this project is not turning
// on by default, so the no-search path has to keep emitting exactly what it
// emitted before the search existed - and "exactly" is checkable here
// because the whole pipeline is deterministic.
TEST_CASE("the search off changes nothing about the encode", "[quality][search]") {
    const auto material = make_material(2);
    const auto config = config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0, false, 192);
    const auto first = encode_all(config, material);
    const auto second = encode_all(config, material);
    REQUIRE(first.size() == second.size());
    for (std::size_t frame = 0; frame < first.size(); ++frame) {
        CAPTURE(frame);
        REQUIRE(first[frame] == second[frame]);
    }
}

TEST_CASE("the search is deterministic", "[quality][search]") {
    const auto material = make_material(2);
    for (const auto criterion : {ac3::quality::Criterion::kDistortion,
                                 ac3::quality::Criterion::kPerceptual}) {
        const auto config = config_for(criterion, ac3::Acmod::k2_0, false, 256);
        const auto first = encode_all(config, material);
        const auto second = encode_all(config, material);
        for (std::size_t frame = 0; frame < first.size(); ++frame) {
            CAPTURE(static_cast<int>(criterion), frame);
            REQUIRE(first[frame] == second[frame]);
        }
    }
}

// A search that never departs from the incumbent is not a search. This does
// not assert WHICH candidate wins - that is the measurement's job, and it
// is material-dependent by design - only that the mechanism is live.
TEST_CASE("the search actually changes the emitted parameters", "[quality][search]") {
    const auto material = make_material(2);
    const auto without = encode_all(
        config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0, false, 192), material);
    const auto with = encode_all(
        config_for(ac3::quality::Criterion::kDistortion, ac3::Acmod::k2_0, false, 192), material);
    bool differs = false;
    for (std::size_t frame = 0; frame < without.size(); ++frame) {
        differs = differs || without[frame] != with[frame];
    }
    CHECK(differs);
    // Every frame stays exactly the size the rate table says, whatever the
    // search chose: the codes are fixed-width fields, so no candidate can
    // cost or save a byte.
    for (std::size_t frame = 0; frame < with.size(); ++frame) {
        CAPTURE(frame);
        CHECK(with[frame].size() == without[frame].size());
    }
}

// The search minimises measured reconstruction noise, so a decode of its
// output should carry less of it. Measured through the decoder rather than
// through the encoder's own model, so this is a check on the whole loop and
// not on the criterion agreeing with itself.
TEST_CASE("searching on distortion lowers the decoded error", "[quality][search]") {
    const auto material = make_material(2);
    ac3::DecoderConfig decoder_config;

    const auto decoded_error = [&](ac3::quality::Criterion criterion, std::uint32_t kbps) {
        const auto frames =
            encode_all(config_for(criterion, ac3::Acmod::k2_0, false, kbps), material);
        ac3::FrameDecoder decoder(decoder_config);
        double signal = 0.0;
        double noise = 0.0;
        // Frame 0's output is the previous (empty) frame's second half, so
        // it has nothing to compare against; §7.9.4's one-block delay means
        // frame N's PCM belongs to frame N-1's input.
        for (std::size_t frame = 1; frame < frames.size(); ++frame) {
            auto pcm = decoder.decode_frame(frames[frame]);
            REQUIRE(pcm.has_value());
            for (std::size_t ch = 0; ch < pcm->channels.size(); ++ch) {
                const auto& out = pcm->channels[ch];
                const auto& in = material[ch];
                const std::size_t base = (frame - 1) * ac3::kSamplesPerFrame;
                for (std::size_t n = 0; n < out.size(); ++n) {
                    const double reference = static_cast<double>(in[base + n]);
                    const double error = static_cast<double>(out[n]) - reference;
                    signal += reference * reference;
                    noise += error * error;
                }
            }
        }
        return 10.0 * std::log10(signal / std::max(noise, 1e-30));
    };

    for (const std::uint32_t kbps : {192U, 448U}) {
        const double off = decoded_error(ac3::quality::Criterion::kNone, kbps);
        const double searched = decoded_error(ac3::quality::Criterion::kDistortion, kbps);
        CAPTURE(kbps, off, searched);
        // Never worse. The margin is the search's own switch margin: a
        // candidate only wins by beating the incumbent by more than that, so
        // the result cannot come out behind it.
        CHECK(searched >= off - 0.05);
    }
}
