// The AC-4 encoder at every frame rate of Part 1 Table 83 (planning/ac4.md,
// phase E5): the decoder reads each stream with the encoder's trace, decodes
// each frame to the sample count the encoder gives it, and the tones in the
// input come out at their level.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

constexpr double kRate = 48000.0;

std::vector<float> tone(double hz, double amplitude, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
    }
    return x;
}

// The power of `samples` at `hz`, Hann-windowed, over [from, to).
double tone_power(std::span<const float> samples, double hz, std::size_t from, std::size_t to) {
    const auto body = samples.subspan(from, std::min(to, samples.size()) - from);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    const double coeff = 2.0 * std::cos(w);
    const auto n = static_cast<double>(body.size());
    double s1 = 0.0;
    double s2 = 0.0;
    double window_sum = 0.0;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / n);
        const double s0 = hann * static_cast<double>(body[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
        window_sum += hann;
    }
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / (window_sum * window_sum);
}

struct Leg {
    int channels;
    int kbps;
    ac4::CodecMode expected;
    double tolerance_db;  // of each tone's level
};

}  // namespace

TEST_CASE(
    "every frame rate reads back and decodes to its sample count with the tones at their levels",
    "[ac4enc][frame-rate]") {
    // SIMPLE, ASPX with companding and without, ASPX_ACPL_2. At 120 fps a
    // frame's table of contents, presentation substream, metadata and A-SPX
    // data take much of a low rate: mono at 32 kbps, 33 bytes a frame there,
    // leaves the tone 9 dB down, and at 48 kbps 0.5 dB.
    const std::vector<Leg> legs = {
        {.channels = 2, .kbps = 192, .expected = ac4::CodecMode::kSimple, .tolerance_db = 0.1},
        {.channels = 1, .kbps = 56, .expected = ac4::CodecMode::kAspx, .tolerance_db = 0.5},
        {.channels = 2, .kbps = 128, .expected = ac4::CodecMode::kAspx, .tolerance_db = 0.3},
        {.channels = 6, .kbps = 128, .expected = ac4::CodecMode::kAspxAcpl2, .tolerance_db = 1.0},
    };
    const std::size_t count = static_cast<std::size_t>(kRate);  // a second
    for (int index = 0; index <= 12; ++index) {
        for (const Leg& leg : legs) {
            CAPTURE(index, leg.channels, leg.kbps);
            // Each channel its own tone below any crossover.
            std::vector<double> hz;
            std::vector<std::vector<float>> input;
            for (int c = 0; c < leg.channels; ++c) {
                hz.push_back(leg.channels == 6 && c == 3 ? 60.0 : 330.0 + 190.0 * c);
                input.push_back(tone(hz.back(), 0.1, count));
            }
            ac4::EncoderConfig config;
            config.channels = leg.channels;
            config.bitrate_kbps = leg.kbps;
            config.frame_rate_index = index;
            // The sinks are callable references: the lambdas outlive the
            // encoder and the decoder.
            std::vector<ac4::SyntaxRecord> written;
            const auto write_sink = [&written](const ac4::SyntaxRecord& r) {
                written.push_back(r);
            };
            config.trace = write_sink;
            auto encoder = ac4::Encoder::create(config);
            REQUIRE(encoder.has_value());
            CHECK(encoder->codec_mode() == leg.expected);
            CHECK(encoder->toc().frame_rate_index == index);
            std::vector<ac4::EncodedFrame> frames;
            // In pieces that no frame's length divides.
            for (std::size_t at = 0; at < count; at += 1000) {
                std::vector<std::span<const float>> views;
                for (const auto& channel : input) {
                    views.emplace_back(std::span<const float>(channel).subspan(
                        at, std::min<std::size_t>(1000, count - at)));
                }
                auto out = encoder->encode(views);
                REQUIRE(out.has_value());
                frames.insert(frames.end(), out->begin(), out->end());
            }
            auto rest = encoder->flush();
            REQUIRE(rest.has_value());
            frames.insert(frames.end(), rest->begin(), rest->end());

            // The decoder reads what was written, record for record, and
            // gives each frame the samples the encoder counted for it.
            std::vector<ac4::SyntaxRecord> read;
            const auto read_sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
            ac4::Decoder decoder(
                ac4::DecoderConfig{.syntax = read_sink, .output = {}, .concealment = {}});
            std::vector<std::vector<float>> decoded(static_cast<std::size_t>(leg.channels));
            std::size_t mismatched_counts = 0;
            for (const ac4::EncodedFrame& frame : frames) {
                const auto result = decoder.decode(frame.raw_ac4_frame);
                INFO(decoder.refusal_reason());
                REQUIRE(result.has_value());
                REQUIRE(result->has_value());
                const ac4::DecodedFrame& pcm = **result;
                REQUIRE(pcm.channels.size() == decoded.size());
                if (pcm.channels.front().size() != static_cast<std::size_t>(frame.samples)) {
                    ++mismatched_counts;
                }
                for (std::size_t c = 0; c < decoded.size(); ++c) {
                    decoded[c].insert(decoded[c].end(), pcm.channels[c].begin(),
                                      pcm.channels[c].end());
                }
            }
            CHECK(mismatched_counts == 0);
            REQUIRE(read.size() == written.size());
            std::size_t differing = 0;
            for (std::size_t i = 0; i < read.size(); ++i) {
                if (read[i].bits != written[i].bits || read[i].value != written[i].value ||
                    read[i].bit_offset != written[i].bit_offset ||
                    read[i].substream != written[i].substream) {
                    ++differing;
                }
            }
            CHECK(differing == 0);

            // Each tone at its level, away from the start and the end.
            const std::size_t lag = static_cast<std::size_t>(encoder->delay_samples()) + 4096;
            REQUIRE(decoded.front().size() > count + lag / 2);
            for (std::size_t c = 0; c < decoded.size(); ++c) {
                CAPTURE(c);
                const double in = tone_power(input[c], hz[c], 12000, count - 12000);
                const double out = tone_power(decoded[c], hz[c], lag + 12000, lag + count - 12000);
                CHECK(std::abs(10.0 * std::log10(out / in)) < leg.tolerance_db);
            }
        }
    }
}

TEST_CASE("every frame rate frames attacks within its block and A-SPX limits",
          "[ac4enc][frame-rate]") {
    // Bursts of noise after near silence, which split the transform (a
    // frame's shortest blocks below 1 536 samples) and give A-SPX its
    // attacks, whose borders at 8 A-SPX slots a frame or fewer take one
    // relative border a side (Part 1 Table 53); and the 5.X modes.
    struct Burst {
        int channels;
        int kbps;
        ac4::CodecMode expected;
    };
    const std::vector<Burst> legs = {
        {.channels = 2, .kbps = 64, .expected = ac4::CodecMode::kAspx},
        {.channels = 2, .kbps = 192, .expected = ac4::CodecMode::kSimple},
        {.channels = 6, .kbps = 96, .expected = ac4::CodecMode::kAspxAcpl3},
        {.channels = 6, .kbps = 256, .expected = ac4::CodecMode::kAspx},
    };
    const std::size_t count = static_cast<std::size_t>(kRate);
    std::uint32_t seed = 12345;
    const auto noise = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<double>(seed >> 8) / 16777216.0 - 0.5);
    };
    for (int index = 0; index <= 12; ++index) {
        for (const Burst& leg : legs) {
            CAPTURE(index, leg.channels, leg.kbps);
            std::vector<std::vector<float>> input(static_cast<std::size_t>(leg.channels),
                                                  std::vector<float>(count));
            for (std::size_t c = 0; c < input.size(); ++c) {
                for (std::size_t n = 0; n < count; ++n) {
                    // A burst of 3 ms every 97 ms, each channel at its own
                    // phase, over a quiet tone.
                    const bool burst = (n + 1231 * c) % 4656 < 144;
                    input[c][n] =
                        (burst ? 0.5F * noise() : 0.0F) +
                        0.002F * static_cast<float>(std::sin(0.05 * static_cast<double>(n)));
                }
            }
            ac4::EncoderConfig config;
            config.channels = leg.channels;
            config.bitrate_kbps = leg.kbps;
            config.frame_rate_index = index;
            std::vector<ac4::SyntaxRecord> written;
            const auto write_sink = [&written](const ac4::SyntaxRecord& r) {
                written.push_back(r);
            };
            config.trace = write_sink;
            auto encoder = ac4::Encoder::create(config);
            REQUIRE(encoder.has_value());
            CHECK(encoder->codec_mode() == leg.expected);
            std::vector<std::span<const float>> views(input.begin(), input.end());
            auto frames = encoder->encode(views);
            REQUIRE(frames.has_value());
            auto rest = encoder->flush();
            REQUIRE(rest.has_value());
            frames->insert(frames->end(), rest->begin(), rest->end());

            std::vector<ac4::SyntaxRecord> read;
            const auto read_sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
            ac4::Decoder decoder(
                ac4::DecoderConfig{.syntax = read_sink, .output = {}, .concealment = {}});
            std::size_t total = 0;
            std::size_t expected = 0;
            for (const ac4::EncodedFrame& frame : *frames) {
                const auto result = decoder.decode(frame.raw_ac4_frame);
                INFO(decoder.refusal_reason());
                REQUIRE(result.has_value());
                REQUIRE(result->has_value());
                total += (**result).channels.front().size();
                expected += static_cast<std::size_t>(frame.samples);
            }
            CHECK(total == expected);
            REQUIRE(read.size() == written.size());
            std::size_t differing = 0;
            for (std::size_t i = 0; i < read.size(); ++i) {
                if (read[i].bits != written[i].bits || read[i].value != written[i].value ||
                    read[i].bit_offset != written[i].bit_offset ||
                    read[i].substream != written[i].substream) {
                    ++differing;
                }
            }
            CHECK(differing == 0);
            // The bursts split some frames' transforms.
            const auto split = std::ranges::count_if(written, [](const ac4::SyntaxRecord& r) {
                return (r.name == "b_long_frame" && r.value == 0) ||
                       (r.name == "transf_length" && r.value == 0);
            });
            CHECK(split > 0);
        }
    }
}

TEST_CASE("I-frames fall where the caller names them and a decoder can start at each",
          "[ac4enc][frame-rate]") {
    // At 29.97 fps, whose frames decode to 1 601 or 1 602 samples: frames 5
    // and 7 named, and fragments from output samples 16 016 (frame 10's first)
    // and 20 000 (inside frame 12, so frame 13).
    ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 128;
    config.frame_rate_index = 3;
    config.iframe_interval = 1000;
    config.iframes = {7, 5};
    config.fragment_starts = {16016, 20000};
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::vector<float> left = tone(440.0, 0.1, 48000);
    const std::vector<float> right = tone(550.0, 0.1, 48000);
    const std::vector<std::span<const float>> views = {left, right};
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    frames->insert(frames->end(), rest->begin(), rest->end());
    REQUIRE(frames->size() > 14);
    std::vector<std::size_t> iframes;
    std::int64_t output = 0;
    for (std::size_t f = 0; f < frames->size(); ++f) {
        if ((*frames)[f].iframe) {
            iframes.push_back(f);
        }
        if (f == 10) {
            CHECK(output == 16016);
        }
        output += (*frames)[f].samples;
    }
    CHECK(iframes == std::vector<std::size_t>{0, 5, 7, 10, 13});

    // A decoder that starts at an I-frame decodes from there; one that
    // starts after it has nothing, the stream being ASPX, until the next.
    for (const std::size_t start : {std::size_t{5}, std::size_t{13}, std::size_t{6}}) {
        CAPTURE(start);
        ac4::Decoder decoder(ac4::DecoderConfig{});
        const auto first = decoder.decode((*frames)[start].raw_ac4_frame);
        REQUIRE(first.has_value());
        CHECK(first->has_value() == (*frames)[start].iframe);
    }
    CHECK(encoder->codec_mode() == ac4::CodecMode::kAspx);

    // No frame or fragment starts before the stream.
    config.iframes = {-1};
    CHECK_FALSE(ac4::Encoder::create(config).has_value());
    config.iframes = {};
    config.fragment_starts = {-2048};
    CHECK_FALSE(ac4::Encoder::create(config).has_value());
}

TEST_CASE("the decoded output lags the input by the encoder's and the decoder's delays",
          "[ac4enc][frame-rate]") {
    // Noise through SIMPLE at a high rate: the lag at which the decoded
    // signal best matches the input is the sum of the two delays, to within
    // a sample, at index 13 and at frame rates with each converter ratio.
    const std::size_t count = static_cast<std::size_t>(kRate);
    std::uint32_t seed = 777;
    std::vector<float> input(count);
    for (float& x : input) {
        seed = seed * 1664525U + 1013904223U;
        x = static_cast<float>(0.2 * (static_cast<double>(seed >> 8) / 16777216.0 - 0.5));
    }
    for (const int index : {13, 0, 2, 3, 5, 10, 12}) {
        CAPTURE(index);
        ac4::EncoderConfig config;
        config.channels = 1;
        config.bitrate_kbps = 160;
        config.codec_mode = ac4::CodecMode::kSimple;
        config.frame_rate_index = index;
        auto encoder = ac4::Encoder::create(config);
        REQUIRE(encoder.has_value());
        const std::vector<std::span<const float>> views = {input};
        auto frames = encoder->encode(views);
        REQUIRE(frames.has_value());
        auto rest = encoder->flush();
        REQUIRE(rest.has_value());
        frames->insert(frames->end(), rest->begin(), rest->end());
        ac4::Decoder decoder(ac4::DecoderConfig{});
        std::vector<float> decoded;
        for (const ac4::EncodedFrame& frame : *frames) {
            const auto result = decoder.decode(frame.raw_ac4_frame);
            REQUIRE(result.has_value());
            REQUIRE(result->has_value());
            const std::vector<float>& pcm = (**result).channels.front();
            decoded.insert(decoded.end(), pcm.begin(), pcm.end());
        }
        const int predicted = encoder->delay_samples() + encoder->decoder_delay_samples();
        if (index == 13) {
            CHECK(predicted == 3072 + 1313);
        }
        REQUIRE(decoded.size() > count + static_cast<std::size_t>(predicted) + 16);
        int best = 0;
        double best_score = 0.0;
        for (int lag = predicted - 16; lag <= predicted + 16; ++lag) {
            double score = 0.0;
            for (std::size_t n = 8000; n + 8000 < count; ++n) {
                score += static_cast<double>(input[n]) *
                         static_cast<double>(decoded[n + static_cast<std::size_t>(lag)]);
            }
            if (score > best_score) {
                best_score = score;
                best = lag;
            }
        }
        CAPTURE(predicted, best);
        CHECK(std::abs(best - predicted) <= 1);
    }
}

TEST_CASE("frame rates the sample rate does not have are refused", "[ac4enc][frame-rate]") {
    ac4::EncoderConfig config;
    config.sample_rate_hz = 44100;
    for (int index = 0; index <= 15; ++index) {
        CAPTURE(index);
        config.frame_rate_index = index;
        CHECK(ac4::Encoder::create(config).has_value() == (index == 13));
    }
    config.sample_rate_hz = 48000;
    for (const int reserved : {-1, 14, 15}) {
        config.frame_rate_index = reserved;
        CHECK_FALSE(ac4::Encoder::create(config).has_value());
    }
}
