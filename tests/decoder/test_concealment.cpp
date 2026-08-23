#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// §7.10 error concealment (DecoderConfig::concealment).
//
// Every test here damages a REAL encoded frame rather than synthesising a
// broken one: the differential fuzzers only ever compare decodes that
// succeeded, so the behaviour on a frame that fails has had no coverage at
// all. What is damaged is the payload, leaving the sync word and the declared
// frame size intact, because that is the shape real transport corruption
// takes - split_frames still finds every boundary, and only the one frame's
// contents are wrong.

namespace {

std::vector<std::vector<float>> tones(std::span<const double> hz, std::uint64_t start,
                                      int samples, double amplitude = 0.35) {
    std::vector<std::vector<float>> pcm(hz.size(),
                                        std::vector<float>(static_cast<std::size_t>(samples)));
    for (std::size_t ch = 0; ch < hz.size(); ++ch) {
        for (int i = 0; i < samples; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * hz[ch] * n / 48000.0));
        }
    }
    return pcm;
}

// Several frames of real coded audio - more than three, so the MDCT overlap
// is genuine by the time anything is damaged (CONTRIBUTING.md's validation
// discipline: silence and frame 0 give false passes).
std::vector<std::vector<std::byte>> encode_ac3(int frames, ac3::Acmod acmod = ac3::Acmod::k2_0) {
    ac3::EncoderConfig config;
    config.acmod = acmod;
    config.bitrate_kbps = 192;
    ac3::FrameEncoder encoder{config};
    const std::array<double, 2> hz = {440.0, 660.0};
    std::vector<std::vector<std::byte>> out;
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        const auto pcm = tones(hz, n0, ac3::kSamplesPerFrame);
        n0 += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// Corrupt a frame's payload without touching its sync word or its declared
// size: the CRC then fails, which is what a decoder actually sees when a
// transport flips a bit.
void damage(std::vector<std::byte>& frame) {
    REQUIRE(frame.size() > 40);
    frame[frame.size() / 2] ^= std::byte{0xFF};
}

double peak(std::span<const float> samples) {
    double out = 0.0;
    for (const float sample : samples) {
        out = std::max(out, std::abs(static_cast<double>(sample)));
    }
    return out;
}

double rms(std::span<const float> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const float sample : samples) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

// The largest step between neighbouring samples - what a hard discontinuity
// looks like, and the thing concealment exists to avoid.
double largest_step(std::span<const float> samples) {
    double out = 0.0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        out = std::max(out, std::abs(static_cast<double>(samples[i]) -
                                     static_cast<double>(samples[i - 1])));
    }
    return out;
}

}  // namespace

TEST_CASE("a damaged frame still fails by default", "[decoder][concealment]") {
    // Concealment is opt-in precisely so that this stays true: a decoder used
    // as a verification tool must report a damaged frame, not paper over it.
    auto frames = encode_ac3(5);
    damage(frames[3]);
    ac3::FrameDecoder decoder;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        if (i == 3) {
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error() == ac3::DecodeError::kBadCrc);
        } else {
            REQUIRE(decoded.has_value());
            CHECK_FALSE(decoded->concealed.has_value());
        }
    }
}

TEST_CASE("repeat-and-fade conceals a damaged frame and reports that it did",
          "[decoder][concealment]") {
    auto frames = encode_ac3(6);
    damage(frames[3]);
    ac3::FrameDecoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};

    std::vector<std::vector<float>> stream(2);
    ac3::DecodedFrame concealed_frame;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        if (i == 3) {
            REQUIRE(decoded->concealed.has_value());
            CHECK(decoded->concealed->error == ac3::DecodeError::kBadCrc);
            CHECK(decoded->concealed->action == ac3::ConcealmentAction::kRepeatFade);
            // The metadata describes the last frame that DID decode - there
            // is no other honest source for it.
            CHECK(decoded->acmod == ac3::Acmod::k2_0);
            CHECK(decoded->sample_rate == ac3::SampleRate::k48000);
            concealed_frame = *decoded;
        } else {
            CHECK_FALSE(decoded->concealed.has_value());
        }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            stream[ch].insert(stream[ch].end(), decoded->channels[ch].begin(),
                              decoded->channels[ch].end());
        }
    }

    // The concealed frame carries real programme material, not silence: that
    // is the whole difference between repeat-and-fade and mute.
    CHECK(rms(concealed_frame.channels[0]) > 0.01);
    // And it decays across the frame rather than holding level, so a longer
    // dropout does not ring on at full volume.
    const auto& pcm = concealed_frame.channels[0];
    const auto head = std::span{pcm}.first(256);
    const auto tail = std::span{pcm}.last(256);
    CHECK(rms(tail) < rms(head));

    // No hard discontinuity anywhere across the join - the point of working
    // in the overlap-add domain rather than splicing finished PCM. The
    // threshold is generous on purpose: what is being ruled out is a STEP
    // (of order the signal's own amplitude), not ordinary waveform slope.
    CHECK(largest_step(stream[0]) < 0.2);
}

TEST_CASE("mute conceals through the codec's own window rather than cutting",
          "[decoder][concealment]") {
    auto frames = encode_ac3(6);
    damage(frames[3]);
    ac3::FrameDecoder decoder{{.concealment = ac3::ConcealmentPolicy::kMute}};

    ac3::DecodedFrame concealed_frame;
    std::vector<std::vector<float>> stream(2);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        if (i == 3) {
            REQUIRE(decoded->concealed.has_value());
            CHECK(decoded->concealed->action == ac3::ConcealmentAction::kMute);
            concealed_frame = *decoded;
        }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            stream[ch].insert(stream[ch].end(), decoded->channels[ch].begin(),
                              decoded->channels[ch].end());
        }
    }

    // The first block still plays out the previous frame's own window tail,
    // so the mute arrives as a fade; every block after it is exactly silent.
    const auto& pcm = concealed_frame.channels[0];
    CHECK(peak(std::span{pcm}.first(256)) > 0.0);
    CHECK(peak(std::span{pcm}.subspan(256)) == 0.0);
    CHECK(largest_step(stream[0]) < 0.2);
}

TEST_CASE("a damaged first frame is still an error", "[decoder][concealment]") {
    // Concealment reconstructs from what came before; at the head of a stream
    // there is nothing to reconstruct from, and inventing something would be
    // substituting audio rather than concealing a gap in it.
    auto frames = encode_ac3(4);
    damage(frames[0]);
    ac3::FrameDecoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};
    const auto decoded = decoder.decode_frame(frames[0]);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == ac3::DecodeError::kBadCrc);
}

TEST_CASE("consecutive losses keep decaying instead of looping a block forever",
          "[decoder][concealment]") {
    // The one concealment artefact worse than the gap: a long dropout
    // repeating the same 256 samples at full level until the stream recovers.
    auto frames = encode_ac3(8);
    for (std::size_t i = 3; i < 7; ++i) {
        damage(frames[i]);
    }
    ac3::FrameDecoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};
    std::vector<double> concealed_rms;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        if (i >= 3 && i < 7) {
            REQUIRE(decoded->concealed.has_value());
            concealed_rms.push_back(rms(decoded->channels[0]));
        }
    }
    REQUIRE(concealed_rms.size() == 4);
    for (std::size_t i = 1; i < concealed_rms.size(); ++i) {
        INFO("frame " << i << ": " << concealed_rms[i] << " vs " << concealed_rms[i - 1]);
        CHECK(concealed_rms[i] < concealed_rms[i - 1]);
    }
    // And it really is heading for silence, not merely sloping.
    CHECK(concealed_rms.back() < 0.05 * concealed_rms.front());
}

TEST_CASE("the frame after a concealed one decodes normally", "[decoder][concealment]") {
    // Concealment leaves the overlap-add and dither state coherent, so
    // recovery is a normal decode rather than a second artefact. Compared
    // against the same frame decoded from an undamaged stream: the recovered
    // frame differs only in its first block, which legitimately overlaps
    // whatever the concealment put there.
    auto clean = encode_ac3(6);
    auto damaged = clean;
    damage(damaged[3]);

    ac3::FrameDecoder reference;
    ac3::FrameDecoder concealing{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};
    std::vector<float> reference_tail;
    std::vector<float> recovered_tail;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const auto good = reference.decode_frame(clean[i]);
        REQUIRE(good.has_value());
        const auto after = concealing.decode_frame(damaged[i]);
        REQUIRE(after.has_value());
        if (i == 4) {
            // Past the first block, the two agree: the concealed frame left
            // the delay state where a real frame would have.
            reference_tail.assign(good->channels[0].begin() + 256, good->channels[0].end());
            recovered_tail.assign(after->channels[0].begin() + 256, after->channels[0].end());
        }
    }
    REQUIRE(reference_tail.size() == recovered_tail.size());
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t i = 0; i < reference_tail.size(); ++i) {
        const double x = static_cast<double>(reference_tail[i]);
        const double d = x - static_cast<double>(recovered_tail[i]);
        signal += x * x;
        error += d * d;
    }
    const double snr = 10.0 * std::log10(signal / std::max(error, 1e-30));
    INFO("recovery SNR " << snr << " dB");
    CHECK(snr > 60.0);
}

TEST_CASE("a concealed frame is folded like any other", "[decoder][concealment]") {
    // The output stage runs on concealed frames too - otherwise a stereo fold
    // would suddenly emit six channels for the one frame that was lost, which
    // no sink downstream could take.
    auto frames = encode_ac3(5, ac3::Acmod::k2_0);
    damage(frames[3]);
    ac3::FrameDecoder decoder{{.output = {.target = ac3::DownmixTarget::kMono},
                               .concealment = ac3::ConcealmentPolicy::kRepeatFade}};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels.size() == 1);
    }
}

// --- E-AC-3 ----------------------------------------------------------------

namespace {

std::vector<std::vector<std::byte>> encode_eac3(int frames) {
    ac3::eac3::FrameConfig config;
    config.acmod = ac3::Acmod::k2_0;
    config.bitrate_kbps = 192;
    ac3::eac3::FrameEncoder encoder{config};
    const std::array<double, 2> hz = {440.0, 660.0};
    std::vector<std::vector<std::byte>> out;
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        const auto pcm = tones(hz, n0, ac3::kSamplesPerFrame);
        n0 += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

}  // namespace

TEST_CASE("E-AC-3 conceals a damaged substream from its own identity's history",
          "[decoder][concealment][eac3]") {
    auto frames = encode_eac3(6);
    damage(frames[3]);

    ac3::Eac3Decoder plain;
    const auto refused = [&] {
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto decoded = plain.decode_substream(frames[i]);
            if (!decoded) {
                return i;
            }
        }
        return frames.size();
    }();
    REQUIRE(refused == 3);

    ac3::Eac3Decoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_substream(frames[i]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto& substream = **decoded;
        if (i == 3) {
            REQUIRE(substream.concealed.has_value());
            CHECK(substream.concealed->action == ac3::ConcealmentAction::kRepeatFade);
            CHECK(substream.channels.size() == 2);
            CHECK(rms(substream.channels[0]) > 0.01);
            // A concealed frame never carries an object layer: OAMD
            // describes the frame that did not arrive, and repeating its
            // positions would put moving objects somewhere they are not.
            CHECK_FALSE(substream.object_metadata.has_value());
            CHECK(substream.object_audio.empty());
        } else {
            CHECK_FALSE(substream.concealed.has_value());
        }
    }
}

TEST_CASE("an access unit whose dependent will not decode still renders its bed",
          "[decoder][concealment][eac3]") {
    // §E3.8.2 assembly needs every substream of a unit in the one call, so a
    // damaged dependent used to take the whole programme down. The bed is a
    // self-sufficient rendering of the same programme - narrower than the
    // stream promised, but real - so it is rendered and the narrowing is
    // reported rather than the audio being lost.
    ac3::eac3::FrameConfig bed_config;
    bed_config.acmod = ac3::Acmod::k3_2;
    bed_config.lfe = true;
    bed_config.bitrate_kbps = 448;
    ac3::eac3::FrameEncoder bed{bed_config};

    ac3::eac3::FrameConfig dep_config;
    dep_config.acmod = ac3::Acmod::k2_0;
    dep_config.bitrate_kbps = 192;
    dep_config.strmtyp = ac3::eac3::StreamType::kDependent;
    dep_config.chanmap = ac3::eac3::chanmap::kVhlVhrBit;
    ac3::eac3::FrameEncoder dependent{dep_config};

    const std::array<double, 6> bed_hz = {200.0, 300.0, 500.0, 700.0, 1100.0, 45.0};
    const std::array<double, 2> dep_hz = {2000.0, 2500.0};

    std::vector<std::vector<std::byte>> units;
    std::uint64_t n0 = 0;
    for (int f = 0; f < 5; ++f) {
        const auto bed_pcm = tones(bed_hz, n0, ac3::kSamplesPerFrame);
        const auto dep_pcm = tones(dep_hz, n0, ac3::kSamplesPerFrame);
        n0 += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> bed_views;
        for (const auto& channel : bed_pcm) {
            bed_views.emplace_back(channel);
        }
        std::vector<std::span<const float>> dep_views;
        for (const auto& channel : dep_pcm) {
            dep_views.emplace_back(channel);
        }
        auto bed_frame = bed.encode_frame(bed_views);
        REQUIRE(bed_frame.has_value());
        auto dep_frame = dependent.encode_frame(dep_views);
        REQUIRE(dep_frame.has_value());
        std::vector<std::byte> unit = std::move(*bed_frame);
        unit.insert(unit.end(), dep_frame->begin(), dep_frame->end());
        units.push_back(std::move(unit));
    }

    // Damage only the dependent half of one unit, leaving the bed intact.
    // Its own frame starts where the bed's ends, so corrupting the tail of
    // the unit corrupts the dependent and nothing else.
    auto& target = units[3];
    target[target.size() - 8] ^= std::byte{0xFF};

    ac3::Eac3Decoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};
    bool saw_bed_only = false;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto decoded = decoder.decode_access_unit(units[i]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto& unit = **decoded;
        if (unit.concealed && unit.concealed->action == ac3::ConcealmentAction::kBedOnly) {
            saw_bed_only = true;
            // The bed's own channels are real, not substituted - that is what
            // makes kBedOnly a different report from kRepeatFade.
            CHECK(unit.layout.count == 6);
            CHECK(rms(unit.channels[0]) > 0.01);
        }
    }
    CHECK(saw_bed_only);
}
