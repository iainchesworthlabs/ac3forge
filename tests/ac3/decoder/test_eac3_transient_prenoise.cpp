#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/transient_prenoise.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/verify/eac3_mirror.hpp"

// §3.7 transient pre-noise processing as Eac3Decoder applies it: where a
// correction lands (kTransientPrenoiseOrigin), one whose transient lies in a
// later frame than the one signalling it, one that reaches back into a frame
// the decoder is still holding, streams of short syncframes, and a concealed
// frame in the middle of a hold-back. The correction's own arithmetic is
// test_transient_prenoise.cpp's.

namespace {

constexpr double kSampleRate = 48000.0;

std::vector<std::byte> read_bytes(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::ranges::transform(bytes, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

void append(std::vector<std::vector<float>>& pcm, const std::vector<std::vector<float>>& channels) {
    if (pcm.empty()) {
        pcm.resize(channels.size());
    }
    REQUIRE(pcm.size() == channels.size());
    for (std::size_t ch = 0; ch < channels.size(); ++ch) {
        pcm[ch].insert(pcm[ch].end(), channels[ch].begin(), channels[ch].end());
    }
}

// One single-substream stream, frame by frame, and whatever flush() hands back
// at the end: every channel's PCM in stream order.
struct Decoded {
    std::vector<std::vector<float>> pcm;
    int held_calls = 0;  // calls that returned std::nullopt
    std::vector<int> latency;  // latency_samples() after each call
    std::vector<ac3::DecodedSubstream> flushed;
};

Decoded decode_frames(ac3::Eac3Decoder& decoder, std::span<const std::vector<std::byte>> frames) {
    Decoded out;
    for (const auto& frame : frames) {
        const auto decoded = decoder.decode_substream(frame);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            append(out.pcm, (*decoded)->channels);
        } else {
            ++out.held_calls;
        }
        out.latency.push_back(decoder.latency_samples());
    }
    out.flushed = decoder.flush();
    for (const auto& held : out.flushed) {
        append(out.pcm, held.channels);
    }
    return out;
}

double sine(double hz, double amplitude, std::int64_t n) {
    return amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kSampleRate);
}

double snr_db(std::span<const double> reference, std::span<const float> actual) {
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t n = 0; n < reference.size(); ++n) {
        const double error = reference[n] - static_cast<double>(actual[n]);
        signal += reference[n] * reference[n];
        noise += error * error;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

}  // namespace

TEST_CASE("DEE's transient pre-noise streams decode, corrected where Dolby's decoder corrects them",
          "[eac3][decoder][transient_prenoise]") {
    // The first five seconds of a Dolby Encoding Engine stream: stereo DD+ at
    // 128 kbit/s from a second of silence and then a decaying noise burst each
    // second on each channel, the right half a second after the left
    // (tools/generators/gen_dee_tpn_fixture.py). DEE turns §3.7 on for every
    // burst and places most of its transients in the frame AFTER the one that
    // signals them, which this decoder used to refuse outright.
    const std::string dir =
        std::string{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} + "/eac3-transient-stereo-128";
    const auto stream = read_bytes(dir + "/dee.ec3");
    const auto source = ac3::io::read_wav(dir + "/source.wav");
    REQUIRE(source.has_value());
    REQUIRE(source->channels.size() == 2);
    const auto split = ac3::split_frames(stream);
    REQUIRE(split.has_value());
    REQUIRE(split->size() == 156);
    std::vector<std::vector<std::byte>> frames;
    for (const auto frame : *split) {
        frames.emplace_back(frame.begin(), frame.end());
    }

    // What makes the fixture worth having, checked rather than assumed: nine
    // corrections, eight of them with their transient past the end of their
    // own frame.
    ac3::verify::Eac3AccessUnitTrace trace;
    ac3::Eac3Decoder tracing{{.eac3_trace = &trace}};
    int corrections = 0;
    int in_a_later_frame = 0;
    for (const auto& frame : frames) {
        REQUIRE(tracing.decode_substream(frame).has_value());
        const auto& syntax = trace.substreams().back();
        for (std::size_t ch = 0; ch < syntax.chintransproc.size(); ++ch) {
            if (syntax.chintransproc[ch]) {
                ++corrections;
                if (ac3::kTransientPrenoiseOrigin + syntax.transprocloc[ch] > ac3::kSamplesPerFrame) {
                    ++in_a_later_frame;
                }
            }
        }
    }
    CHECK(corrections == 9);
    CHECK(in_a_later_frame == 8);

    ac3::Eac3Decoder decoder;
    const auto decoded = decode_frames(decoder, frames);
    REQUIRE(decoded.pcm.size() == 2);
    // Every frame comes back, the last one by flush() - the final correction's
    // transient lies past the last syncframe.
    constexpr std::size_t kSamples = 156 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    REQUIRE(decoded.pcm[0].size() == kSamples);
    REQUIRE(decoded.pcm[1].size() == kSamples);

    // Scored against the source the way tools/checks/verify_gold_reference.sh
    // scores it, the decode one transform overlap behind. Noise bursts with the
    // top of the band synthesized by spectral extension keep every decoder
    // within a few dB of the source (FFmpeg's decode scores 2.46 and 2.25 dB
    // here, Dolby's own much the same); measured 2.27 and 2.03 dB, floors 1 dB.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        CAPTURE(ch);
        const auto count = kSamples - static_cast<std::size_t>(ac3::kTransformDelaySamples);
        std::vector<double> reference(count);
        for (std::size_t n = 0; n < count; ++n) {
            reference[n] = static_cast<double>(source->channels[ch][n]);
        }
        const auto actual = std::span<const float>{decoded.pcm[ch]}.subspan(
            static_cast<std::size_t>(ac3::kTransformDelaySamples), count);
        CHECK(snr_db(reference, actual) > 1.0);
    }

    // The corrections themselves. The source is silent before every burst, and
    // the pre-noise the coding leaves there is what §3.7 replaces with the
    // silence before it: 0.012 to 0.17 of energy in the 448 samples ending 64
    // before each onset in FFmpeg's decode, which does not apply the tool, and
    // under 1e-4 in Dolby's and in this one. Counting transprocloc from the
    // first sample of the output frame instead of kTransientPrenoiseOrigin ends
    // each correction a block short and leaves 0.012 to 0.15 there.
    constexpr std::array<std::array<int, 2>, 8> kOnsets = {
        {{48000, 0}, {72000, 1}, {96000, 0}, {120000, 1}, {144000, 0}, {168000, 1},
         {192000, 0}, {216000, 1}}};
    for (const auto& [onset, ch] : kOnsets) {
        CAPTURE(onset, ch);
        const int decoded_onset = onset + ac3::kTransformDelaySamples;
        double energy = 0.0;
        for (int n = decoded_onset - 512; n < decoded_onset - 64; ++n) {
            const auto v = static_cast<double>(
                decoded.pcm[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]);
            energy += v * v;
        }
        CHECK(energy < 1e-3);
    }
}

TEST_CASE("a correction reaching back into a held frame is applied to it before it is released",
          "[eac3][decoder][transient_prenoise]") {
    // A steady 1 kHz tone with two bursts on it. The first, part-way into
    // frame 1, engages the hold-back; the second starts in the first block of
    // frame 5, so the encoder signals it at transprocloc 0 and its correction
    // cross-fades the last block of frame 4's output - which the decoder has
    // decoded and is still holding - towards the tone 512 samples earlier.
    ac3::eac3::FrameConfig config{.bitrate_kbps = 192, .acmod = ac3::Acmod::k1_0};
    config.transient_prenoise = true;
    ac3::eac3::FrameEncoder encoder{config};
    constexpr int kFrames = 8;
    constexpr double kToneHz = 1000.0;
    constexpr double kToneLevel = 0.05;
    const auto burst = [](std::int64_t n, std::int64_t at) {
        const auto k = n - at;
        return k >= 0 && k < 384 ? sine(5000.0, 0.8, n) * std::exp(-static_cast<double>(k) / 96.0)
                                 : 0.0;
    };
    constexpr std::int64_t kFirstBurst = 1 * ac3::kSamplesPerFrame + 900;
    constexpr std::int64_t kSecondBurst = 5 * ac3::kSamplesPerFrame + 16;
    std::vector<std::vector<std::byte>> frames;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> pcm(static_cast<std::size_t>(ac3::kSamplesPerFrame));
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::int64_t n = std::int64_t{f} * ac3::kSamplesPerFrame + i;
            pcm[static_cast<std::size_t>(i)] = static_cast<float>(
                sine(kToneHz, kToneLevel, n) + burst(n, kFirstBurst) + burst(n, kSecondBurst));
        }
        const std::array<std::span<const float>, 1> views{pcm};
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }

    // The encoder's own heuristic signals a correction at the leading edge of
    // the first block it switches. What this case needs of it: frame 5's at 0,
    // and nothing else signalled between the two bursts.
    ac3::verify::Eac3AccessUnitTrace trace;
    ac3::Eac3Decoder tracing{{.eac3_trace = &trace}};
    int engaged_at = -1;
    for (int f = 0; f < kFrames; ++f) {
        REQUIRE(tracing.decode_substream(frames[static_cast<std::size_t>(f)]).has_value());
        const auto& syntax = trace.substreams().back();
        if (syntax.transproce && engaged_at < 0) {
            engaged_at = f;
        }
        if (f >= 2 && f <= 4) {
            CHECK_FALSE(syntax.transproce);
        }
        if (f == 5) {
            REQUIRE(syntax.transproce);
            REQUIRE(syntax.chintransproc[0]);
            REQUIRE(syntax.transprocloc[0] == 0);
            REQUIRE(syntax.transproclen[0] == 0);
        }
    }
    REQUIRE(engaged_at >= 0);
    REQUIRE(engaged_at < 5);

    ac3::Eac3Decoder decoder;
    const auto decoded = decode_frames(decoder, frames);
    REQUIRE(decoded.pcm.size() == 1);
    REQUIRE(decoded.pcm[0].size() == static_cast<std::size_t>(kFrames * ac3::kSamplesPerFrame));

    // Frame 5's transient is kTransientPrenoiseOrigin into its output, pnlen a
    // block, translen 0: the correction writes [transient - 512, transient),
    // and its first block - frame 4's last - is §3.7.2's first cross-fade,
    // from the decoded audio to the synthesis buffer 512 samples earlier. Both
    // are the tone there, which the decode reproduces far closer than the
    // cross-fade moves it, so the output must be the cross-fade of the tone
    // with itself 512 samples back - and not the tone.
    const int transient = 5 * ac3::kSamplesPerFrame + ac3::kTransientPrenoiseOrigin;
    const int start = transient - 2 * ac3::kSamplesPerBlock;
    const auto tone_out = [&](int n) {
        return sine(kToneHz, kToneLevel, n - ac3::kTransformDelaySamples);
    };
    std::vector<double> blended;
    std::vector<double> untouched;
    for (int s = 64; s < ac3::kTransientPrenoiseTC1 - 16; ++s) {
        const double fade_in =
            0.5 * (1.0 - std::cos(std::numbers::pi * s / (ac3::kTransientPrenoiseTC1 - 1)));
        const int n = start + s;
        blended.push_back(tone_out(n) * (1.0 - fade_in) + tone_out(n - 512) * fade_in);
        untouched.push_back(tone_out(n));
    }
    const auto actual = std::span<const float>{decoded.pcm[0]}.subspan(
        static_cast<std::size_t>(start + 64), blended.size());
    CHECK(snr_db(blended, actual) > 30.0);
    CHECK(snr_db(untouched, actual) < 10.0);
}

TEST_CASE("short syncframes hold 1536 samples back, and flush() hands them back as one substream",
          "[eac3][decoder][transient_prenoise]") {
    // A correction reaches 1528 samples back from its transient whatever the
    // syncframe length, so a one-block stream is held back six syncframes,
    // not one. Before the hold-back was counted in samples, a one-block stream
    // using the tool made the decoder copy a six-block frame's worth of
    // samples into a one-block frame's buffers.
    constexpr int kSyncframes = 48;
    constexpr int kImpulseAt = 20 * ac3::kSamplesPerBlock + 100;
    ac3::eac3::FrameConfig config{.bitrate_kbps = 192, .acmod = ac3::Acmod::k1_0, .numblkscod = 0};
    config.transient_prenoise = true;
    ac3::eac3::FrameEncoder encoder{config};
    REQUIRE(encoder.samples_per_frame() == ac3::kSamplesPerBlock);
    std::vector<std::vector<std::byte>> frames;
    for (int f = 0; f < kSyncframes; ++f) {
        std::vector<float> pcm(static_cast<std::size_t>(ac3::kSamplesPerBlock), 0.0F);
        const int at = kImpulseAt - f * ac3::kSamplesPerBlock;
        if (at >= 0 && at < ac3::kSamplesPerBlock) {
            pcm[static_cast<std::size_t>(at)] = 0.9F;
        }
        const std::array<std::span<const float>, 1> views{pcm};
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }

    ac3::Eac3Decoder decoder;
    const auto decoded = decode_frames(decoder, frames);
    // Six calls return nothing while the first 1536 samples build up; every
    // one after returns a syncframe; flush() returns the last six as one.
    CHECK(decoded.held_calls == 6);
    CHECK(decoded.latency.back() == ac3::kSamplesPerFrame);
    REQUIRE(decoded.flushed.size() == 1);
    CHECK(decoded.flushed.front().numblkscod == 3);
    CHECK(decoded.flushed.front().channels.front().size() ==
          static_cast<std::size_t>(ac3::kSamplesPerFrame));
    // A release delay, not a shift: the stream is its full length and the
    // impulse is where the transform overlap alone puts it.
    REQUIRE(decoded.pcm.size() == 1);
    REQUIRE(decoded.pcm[0].size() == static_cast<std::size_t>(kSyncframes * ac3::kSamplesPerBlock));
    const auto peak = std::ranges::max_element(decoded.pcm[0], {}, [](float v) { return std::abs(v); });
    CHECK(std::distance(decoded.pcm[0].begin(), peak) == kImpulseAt + ac3::kTransformDelaySamples);
}

TEST_CASE("a concealed frame queues behind the frames transient pre-noise processing holds",
          "[eac3][decoder][transient_prenoise]") {
    // With the hold-back engaged a decoded frame comes back a call late, so a
    // concealed frame returned straight away would overtake it.
    ac3::eac3::FrameConfig config{.bitrate_kbps = 192, .acmod = ac3::Acmod::k1_0};
    config.transient_prenoise = true;
    ac3::eac3::FrameEncoder encoder{config};
    constexpr int kFrames = 8;
    constexpr int kDamaged = 5;
    std::vector<std::vector<std::byte>> frames;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> pcm(static_cast<std::size_t>(ac3::kSamplesPerFrame));
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const std::int64_t n = std::int64_t{f} * ac3::kSamplesPerFrame + i;
            // A tone, and from frame 1 on a burst every frame so the tool is
            // in use throughout.
            const auto k = i - 700;
            pcm[static_cast<std::size_t>(i)] = static_cast<float>(
                sine(440.0, 0.1, n) +
                (f >= 1 && k >= 0 && k < 256 ? sine(6000.0, 0.8, n) : 0.0));
        }
        const std::array<std::span<const float>, 1> views{pcm};
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }
    auto& damaged = frames[static_cast<std::size_t>(kDamaged)];
    damaged[damaged.size() / 2] ^= std::byte{0xFF};

    ac3::Eac3Decoder decoder{{.concealment = ac3::ConcealmentPolicy::kMute}};
    std::vector<bool> concealed;
    for (const auto& frame : frames) {
        const auto decoded = decoder.decode_substream(frame);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            concealed.push_back((*decoded)->concealed.has_value());
        }
    }
    for (const auto& held : decoder.flush()) {
        concealed.push_back(held.concealed.has_value());
    }
    REQUIRE(concealed.size() == static_cast<std::size_t>(kFrames));
    for (int f = 0; f < kFrames; ++f) {
        CAPTURE(f);
        CHECK(concealed[static_cast<std::size_t>(f)] == (f == kDamaged));
    }
}
