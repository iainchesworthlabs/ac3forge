// The AC-4 encoder's average and variable bit rates (planning/ac4.md, phase
// E5): frames whose sizes follow their content, an average-rate stream whose
// decoder input buffer never holds more than Part 1 clause 6.2.4 sets or runs
// dry, checked frame by frame from the stream alone for a decoder that starts
// at any frame, and the rate Part 2 Annex B's br_code sequence carries.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

constexpr double kRate = 48000.0;

// Four seconds whose frames need very different sizes: near silence, a
// tone, noise, and bursts after silence.
std::vector<std::vector<float>> programme(int channels) {
    const auto second = static_cast<std::size_t>(kRate);
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels),
                                        std::vector<float>(4 * second));
    std::uint32_t seed = 2026;
    const auto noise = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return static_cast<double>(seed >> 8) / 16777216.0 - 0.5;
    };
    for (std::size_t c = 0; c < out.size(); ++c) {
        for (std::size_t n = 0; n < out[c].size(); ++n) {
            const double t = static_cast<double>(n) / kRate;
            double x = 0.0;
            switch (n / second) {
                case 0:
                    x = 1e-4 * std::sin(2.0 * std::numbers::pi * 200.0 * t);
                    break;
                case 1:
                    x = 0.2 * std::sin(2.0 * std::numbers::pi *
                                       (440.0 + 110.0 * static_cast<double>(c)) * t);
                    break;
                case 2:
                    x = 0.5 * noise();
                    break;
                default:
                    x = n % 4800 < 240 ? 0.6 * noise() : 0.0;
                    break;
            }
            out[c][n] = static_cast<float>(x);
        }
    }
    return out;
}

std::vector<ac4::EncodedFrame> encode(const ac4::EncoderConfig& config,
                                      const std::vector<std::vector<float>>& input) {
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    frames->insert(frames->end(), rest->begin(), rest->end());
    return std::move(*frames);
}

// ac4_toc()'s first fields (Part 2 clause 6.2.1.1): bitstream_version,
// sequence_counter, b_wait_frames, wait_frames and br_code.
struct TocStart {
    int wait_frames = -1;
    int br_code = -1;
};

TocStart toc_start(std::span<const std::byte> frame) {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        bits = (bits << 8U) | std::to_integer<std::uint32_t>(frame[i]);
    }
    const auto field = [&bits](int from, int width) {
        return static_cast<int>((bits >> static_cast<unsigned>(24 - from - width)) &
                                ((1U << static_cast<unsigned>(width)) - 1U));
    };
    TocStart out;
    if (field(12, 1) == 1) {
        out.wait_frames = field(13, 3);
        if (out.wait_frames > 0) {
            out.br_code = field(16, 2);
        }
    }
    return out;
}

// A decoder that starts at frame j of a stream sent at `share` bytes a frame
// period: it has frame j at its arrival, waits the frames its wait_frames
// says, and then takes a frame every period. Counted over the next `reach`
// frames from every j: frames that arrive after their output time, and times
// the buffer holds more than `buffer` shares.
struct BufferCheck {
    std::size_t late = 0;
    std::size_t over = 0;
};

BufferCheck check_buffer(const std::vector<std::size_t>& sizes, const std::vector<int>& waits,
                         double share, int buffer, int wait_step, std::size_t reach) {
    std::vector<double> through(sizes.size());  // bytes of frames 0 to i
    double total = 0.0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        total += static_cast<double>(sizes[i]);
        through[i] = total;
    }
    BufferCheck out;
    for (std::size_t j = 0; j < sizes.size(); ++j) {
        // Times in frame periods from the stream's first byte.
        const double first_output =
            through[j] / share + static_cast<double>((waits[j] - 1) * wait_step);
        for (std::size_t k = j; k < std::min(sizes.size(), j + reach); ++k) {
            const double output = first_output + static_cast<double>(k - j);
            if (through[k] / share > output + 1e-9) {
                ++out.late;
            }
            const double held = std::min(output * share, total) - (k == 0 ? 0.0 : through[k - 1]);
            if (held > static_cast<double>(buffer) * share + 1e-6) {
                ++out.over;
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("an average rate stream never needs more than the buffer it signals", "[ac4enc][rate]") {
    struct Leg {
        int frame_rate_index;
        double frames_per_second;
        int channels;
        int kbps;
    };
    const std::vector<Leg> legs = {
        {.frame_rate_index = 13, .frames_per_second = 48000.0 / 2048.0, .channels = 2, .kbps = 64},
        {.frame_rate_index = 13, .frames_per_second = 48000.0 / 2048.0, .channels = 2, .kbps = 192},
        {.frame_rate_index = 3, .frames_per_second = 30000.0 / 1001.0, .channels = 2, .kbps = 96},
        {.frame_rate_index = 12, .frames_per_second = 120.0, .channels = 1, .kbps = 96},
        {.frame_rate_index = 13, .frames_per_second = 48000.0 / 2048.0, .channels = 6, .kbps = 128},
    };
    for (const Leg& leg : legs) {
        CAPTURE(leg.frame_rate_index, leg.channels, leg.kbps);
        ac4::EncoderConfig config;
        config.channels = leg.channels;
        config.bitrate_kbps = leg.kbps;
        config.frame_rate_index = leg.frame_rate_index;
        config.rate_mode = ac4::RateMode::kAverage;
        const std::vector<ac4::EncodedFrame> frames = encode(config, programme(leg.channels));
        std::vector<std::size_t> sizes;
        std::vector<int> waits;
        std::vector<int> codes;
        ac4::Decoder decoder(ac4::DecoderConfig{});
        for (const ac4::EncodedFrame& frame : frames) {
            sizes.push_back(frame.raw_ac4_frame.size());
            const TocStart toc = toc_start(frame.raw_ac4_frame);
            waits.push_back(toc.wait_frames);
            codes.push_back(toc.br_code);
            const auto decoded = decoder.decode(frame.raw_ac4_frame);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
        }
        // Every frame signals an average rate, and the sizes follow the
        // content.
        CHECK(std::ranges::all_of(waits, [](int w) { return w >= 1 && w <= 6; }));
        const auto [shortest, longest] = std::ranges::minmax(sizes);
        CHECK(longest > 2 * shortest);

        const double share = leg.kbps * 1000.0 / 8.0 / leg.frames_per_second;
        const bool fast = leg.frame_rate_index >= 10 && leg.frame_rate_index <= 12;
        const BufferCheck check =
            check_buffer(sizes, waits, share, fast ? 12 : 6, fast ? 2 : 1, 400);
        CHECK(check.late == 0);
        CHECK(check.over == 0);
        // Over the stream the rate is the one asked for, within the buffer.
        double total = 0.0;
        for (const std::size_t size : sizes) {
            total += static_cast<double>(size);
        }
        CHECK(std::abs(total - share * static_cast<double>(sizes.size())) <=
              (fast ? 12.0 : 6.0) * share);

        // br_code: 0b11 and six base-3 digits, over and over, which Annex B
        // turns back into the rate.
        REQUIRE(codes.size() > 14);
        for (std::size_t i = 0; i < codes.size(); ++i) {
            CHECK((codes[i] == 3) == (i % 7 == 0));
        }
        double sum = 0.0;
        for (int i = 1; i <= 6; ++i) {
            sum += codes[static_cast<std::size_t>(i)] * std::pow(3.0, -i);
        }
        const double octave = std::floor(std::log2(static_cast<double>(leg.kbps)));
        const double low = std::exp2(octave + sum);
        const double high = std::exp2(octave + sum + std::pow(3.0, -6));
        CHECK(low <= leg.kbps);
        CHECK(leg.kbps < high);
    }
}

TEST_CASE("an average rate keeps each of several substreams at its least frame or above",
          "[ac4enc][rate]") {
    // Mono and two stereo substreams, the stereo ones' channels equal, which
    // A-SPX's balance coding makes cheap: at an average rate each substream
    // is sized by what it needs, and one that needs less than its least frame
    // still gets it, the substream taking what the others leave among them.
    ac4::EncoderConfig config;
    config.bitrate_kbps = 104;
    config.rate_mode = ac4::RateMode::kAverage;
    config.experimental.aspx_balance = true;
    for (const int channels : {1, 2, 2}) {
        ac4::SubstreamConfig substream;
        substream.channels = channels;
        config.substreams.push_back(substream);
        ac4::PresentationConfig presentation;
        presentation.substreams = {static_cast<int>(config.substreams.size()) - 1};
        config.presentations.push_back(presentation);
    }
    std::vector<std::vector<float>> input = programme(5);
    input[2] = input[1];
    input[4] = input[3];
    const std::vector<ac4::EncodedFrame> frames = encode(config, input);
    REQUIRE_FALSE(frames.empty());
    ac4::Decoder decoder(ac4::DecoderConfig{});
    for (const ac4::EncodedFrame& frame : frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
    }
}

TEST_CASE("a variable rate stream sends no wait and keeps its rate over seconds",
          "[ac4enc][rate]") {
    ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 96;
    config.rate_mode = ac4::RateMode::kVariable;
    const std::vector<ac4::EncodedFrame> frames = encode(config, programme(2));
    double total = 0.0;
    std::size_t longest = 0;
    std::size_t shortest = std::numeric_limits<std::size_t>::max();
    ac4::Decoder decoder(ac4::DecoderConfig{});
    for (const ac4::EncodedFrame& frame : frames) {
        CHECK(toc_start(frame.raw_ac4_frame).wait_frames == 7);
        total += static_cast<double>(frame.raw_ac4_frame.size());
        longest = std::max(longest, frame.raw_ac4_frame.size());
        shortest = std::min(shortest, frame.raw_ac4_frame.size());
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
    }
    const double share = 96000.0 / 8.0 * 2048.0 / 48000.0;
    CHECK(longest > 3 * shortest);
    // Within two seconds' shares of the rate.
    CHECK(std::abs(total - share * static_cast<double>(frames.size())) <=
          2.0 * 48000.0 / 2048.0 * share);
}

TEST_CASE("a constant rate stream sends wait_frames 0 and no br_code", "[ac4enc][rate]") {
    ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 96;
    const std::vector<ac4::EncodedFrame> frames = encode(config, programme(2));
    for (const ac4::EncodedFrame& frame : frames) {
        const TocStart toc = toc_start(frame.raw_ac4_frame);
        CHECK(toc.wait_frames == 0);
        CHECK(toc.br_code == -1);
    }
}
