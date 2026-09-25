// ac4::Encoder end to end: what it writes, read back by the inspector, the
// decoder's syntax walk and the decoder's PCM path. Each stream's frames are
// the size the bit rate gives, every substream reads to its end with the trace
// the encoder recorded, and the decoded output is the input, delayed by the
// encoder's delay and the decoder's frame alignment, at unity gain, with
// each channel's tone on its own channel.

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
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

constexpr int kDecoderDelay = 352;  // Part 1 Table 188 at frame_rate_index 13

struct Encoded {
    std::vector<ac4::EncodedFrame> frames;
    std::vector<ac4::SyntaxRecord> trace;
};

// Encodes planar input in pieces of `piece` samples, then flushes.
Encoded encode(const ac4::EncoderConfig& base, const std::vector<std::vector<float>>& input, std::size_t piece) {
    Encoded out;
    ac4::EncoderConfig config = base;
    const auto sink = [&out](const ac4::SyntaxRecord& r) { out.trace.push_back(r); };
    config.trace = sink;
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::size_t total = input.front().size();
    for (std::size_t at = 0; at < total; at += piece) {
        const std::size_t count = std::min(piece, total - at);
        std::vector<std::span<const float>> views;
        for (const auto& channel : input) {
            views.emplace_back(std::span<const float>(channel).subspan(at, count));
        }
        auto frames = encoder->encode(views);
        REQUIRE(frames.has_value());
        out.frames.insert(out.frames.end(), frames->begin(), frames->end());
    }
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    out.frames.insert(out.frames.end(), rest->begin(), rest->end());
    return out;
}

std::vector<std::vector<float>> decode(const std::vector<ac4::EncodedFrame>& frames) {
    ac4::Decoder decoder;
    std::vector<std::vector<float>> out;
    for (const ac4::EncodedFrame& frame : frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        out.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out[c].insert(out[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    return out;
}

std::vector<float> tone(double hz, double amplitude, std::size_t count, int rate) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate));
    }
    return x;
}

// Gain (dB) and SNR (dB) of `decoded` against `source` delayed by `lag`,
// over the middle of the overlap.
struct Score {
    double gain_db = 0.0;
    double snr_db = 0.0;
};

Score score(std::span<const float> source, std::span<const float> decoded, std::size_t lag) {
    const std::size_t skip = 4096;
    const std::size_t count = std::min(source.size(), decoded.size() - lag);
    double sd = 0.0;
    double ss = 0.0;
    for (std::size_t n = skip; n + skip < count; ++n) {
        sd += static_cast<double>(source[n]) * static_cast<double>(decoded[n + lag]);
        ss += static_cast<double>(source[n]) * static_cast<double>(source[n]);
    }
    const double gain = sd / ss;
    double error = 0.0;
    for (std::size_t n = skip; n + skip < count; ++n) {
        const double e = static_cast<double>(decoded[n + lag]) - gain * static_cast<double>(source[n]);
        error += e * e;
    }
    return {20.0 * std::log10(std::abs(gain)), 10.0 * std::log10(gain * gain * ss / error)};
}

void check_frames_read_back(const Encoded& encoded) {
    // Every substream of every frame reads to its end, and the decoder's
    // trace of the substreams is the encoder's, frame by frame.
    std::vector<ac4::SyntaxRecord> read;
    const auto sink = [&read](const ac4::SyntaxRecord& r) { read.push_back(r); };
    ac4::DecoderConfig config;
    config.syntax = sink;
    ac4::Decoder decoder(config);
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        const auto report = decoder.parse(frame.raw_ac4_frame);
        REQUIRE(report.has_value());
        for (const ac4::SubstreamReport& substream : report->substreams) {
            CAPTURE(substream.index, substream.refused_reason);
            REQUIRE_FALSE(substream.refused.has_value());
            CHECK(substream.bits_read == substream.size_bits);
        }
    }
    REQUIRE(read.size() == encoded.trace.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < read.size(); ++i) {
        const bool same = read[i].substream == encoded.trace[i].substream &&
                          read[i].bit_offset == encoded.trace[i].bit_offset &&
                          read[i].bits == encoded.trace[i].bits && read[i].value == encoded.trace[i].value;
        if (!same && mismatches++ < 5) {
            CAPTURE(i, encoded.trace[i].name, read[i].name, encoded.trace[i].bit_offset, read[i].bit_offset,
                    encoded.trace[i].value, read[i].value);
            CHECK(same);
        }
    }
    CHECK(mismatches == 0);
}

}  // namespace

TEST_CASE("the encoder refuses what it does not write", "[ac4enc][encoder]") {
    ac4::EncoderConfig config;
    config.channels = 6;
    CHECK(ac4::Encoder::create(config).error() == ac4::EncodeError::kInvalidConfig);
    config = {};
    config.sample_rate_hz = 32000;
    CHECK(ac4::Encoder::create(config).error() == ac4::EncodeError::kInvalidConfig);
    config = {};
    config.dialnorm_db = -40.0;
    CHECK(ac4::Encoder::create(config).error() == ac4::EncodeError::kInvalidConfig);
    config = {};
    config.iframe_interval = 0;
    CHECK(ac4::Encoder::create(config).error() == ac4::EncodeError::kInvalidConfig);

    auto encoder = ac4::Encoder::create(ac4::EncoderConfig{});
    REQUIRE(encoder.has_value());
    const std::vector<float> one(10, 0.0F);
    const std::vector<float> other(11, 0.0F);
    const std::vector<std::span<const float>> uneven{one, other};
    CHECK(encoder->encode(uneven).error() == ac4::EncodeError::kInvalidInput);
    std::vector<float> bad(10, 0.0F);
    bad[3] = std::numeric_limits<float>::quiet_NaN();
    const std::vector<std::span<const float>> with_nan{bad, one};
    CHECK(encoder->encode(with_nan).error() == ac4::EncodeError::kInvalidInput);
}

TEST_CASE("samples far past full scale still encode, to frames that read back and decode", "[ac4enc][encoder]") {
    // A finite float can stand 10^38 over full scale, more than the coarsest
    // step codes in a frame at a low rate: such a frame goes out with no bands.
    std::vector<float> huge(48000);
    for (std::size_t n = 0; n < huge.size(); ++n) {
        huge[n] = (n / 64) % 2 == 0 ? 1e30F : -1e30F;
    }
    for (const int kbps : {8, 192}) {
        CAPTURE(kbps);
        ac4::EncoderConfig config;
        config.bitrate_kbps = kbps;
        const Encoded encoded = encode(config, {huge, huge}, 4800);
        REQUIRE_FALSE(encoded.frames.empty());
        check_frames_read_back(encoded);
        const auto decoded = decode(encoded.frames);
        for (const std::vector<float>& channel : decoded) {
            CHECK(std::all_of(channel.begin(), channel.end(), [](float x) { return std::isfinite(x); }));
        }
    }
}

TEST_CASE("the encoder's table of contents describes one stereo substream at index 13", "[ac4enc][encoder]") {
    auto encoder = ac4::Encoder::create(ac4::EncoderConfig{});
    REQUIRE(encoder.has_value());
    const ac4::Toc& toc = encoder->toc();
    CHECK(toc.bitstream_version == 2);
    CHECK(toc.frame_rate_index == 13);
    CHECK(toc.sample_rate_hz == 48000);
    REQUIRE(toc.presentations_v1.size() == 1);
    CHECK(toc.presentations_v1[0].presentation_version == 1);
    REQUIRE(toc.substream_groups.size() == 1);
    const auto& chan = toc.substream_groups[0].substreams.at(0).chan;
    REQUIRE(chan.has_value());
    CHECK(chan->ch_mode == 1);
    CHECK(encoder->delay_samples() == 3072);
}

TEST_CASE("stereo tones encode at 192 kbps and decode on their own channels at unity gain",
          "[ac4enc][encoder]") {
    const std::size_t count = 48000 * 3;
    const std::vector<std::vector<float>> input{tone(331.0, 0.1, count, 48000), tone(457.0, 0.1, count, 48000)};
    ac4::EncoderConfig config;
    config.bitrate_kbps = 192;
    const Encoded encoded = encode(config, input, 1000);
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        CHECK(frame.raw_ac4_frame.size() == 1024);  // 192 kbps at 2 048 samples of 48 kHz
        CHECK(frame.samples == 2048);
    }
    // Every input sample reaches the output: the encoder's delay and the
    // decoder's frame alignment are both flushed.
    CHECK(encoded.frames.size() * 2048 >= count + 3072 + kDecoderDelay);
    check_frames_read_back(encoded);

    const auto decoded = decode(encoded.frames);
    REQUIRE(decoded.size() == 2);
    const std::size_t lag = 3072 + kDecoderDelay;
    for (std::size_t c = 0; c < 2; ++c) {
        CAPTURE(c);
        const Score s = score(input[c], decoded[c], lag);
        CHECK(std::abs(s.gain_db) < 0.2);
        CHECK(s.snr_db > 40.0);
        // Nothing of the other channel's tone.
        const Score leak = score(input[1 - c], decoded[c], lag);
        CHECK(leak.gain_db < -40.0);
    }
}

TEST_CASE("a panned source is predicted from M and keeps its balance", "[ac4enc][encoder]") {
    // Twelve tones across the band, the same in both channels but for a gain:
    // S is then a multiple of M in every band, which sap_mode 3 predicts whole.
    const std::size_t count = 48000 * 2;
    std::vector<float> source(count, 0.0F);
    for (int k = 0; k < 12; ++k) {
        const double hz = 210.0 * std::pow(1.4, k);
        const std::vector<float> t = tone(hz, 0.02, count, 48000);
        for (std::size_t n = 0; n < count; ++n) {
            source[n] += t[n];
        }
    }
    for (const float right_gain : {0.4F, -0.5F}) {
        CAPTURE(right_gain);
        std::vector<float> right(count);
        std::transform(source.begin(), source.end(), right.begin(), [&](float x) { return right_gain * x; });
        const std::vector<std::vector<float>> input{source, right};
        ac4::EncoderConfig config;
        config.bitrate_kbps = 128;
        const Encoded encoded = encode(config, input, 3000);
        std::size_t predicted = 0;
        std::size_t frames = 0;
        for (const ac4::SyntaxRecord& r : encoded.trace) {
            if (r.name == "sap_mode") {
                ++frames;
                predicted += r.value == 3 ? 1 : 0;
            }
        }
        CHECK(predicted * 10 >= frames * 9);
        check_frames_read_back(encoded);
        const auto decoded = decode(encoded.frames);
        REQUIRE(decoded.size() == 2);
        const std::size_t lag = 3072 + kDecoderDelay;
        const Score l = score(input[0], decoded[0], lag);
        const Score r = score(input[1], decoded[1], lag);
        CHECK(std::abs(l.gain_db) < 0.2);
        CHECK(std::abs(r.gain_db) < 0.2);
        CHECK(l.snr_db > 30.0);
        CHECK(r.snr_db > 25.0);
    }
}

TEST_CASE("mono encodes and decodes at 64 kbps", "[ac4enc][encoder]") {
    const std::size_t count = 48000 * 2;
    const std::vector<std::vector<float>> input{tone(1000.0, 0.25, count, 48000)};
    ac4::EncoderConfig config;
    config.channels = 1;
    config.bitrate_kbps = 64;
    const Encoded encoded = encode(config, input, 4096);
    check_frames_read_back(encoded);
    const auto decoded = decode(encoded.frames);
    REQUIRE(decoded.size() == 1);
    const Score s = score(input[0], decoded[0], 3072 + kDecoderDelay);
    CHECK(std::abs(s.gain_db) < 0.2);
    CHECK(s.snr_db > 30.0);
}

TEST_CASE("transients split frames into short blocks, and still decode cleanly", "[ac4enc][encoder]") {
    const std::size_t count = 48000 * 2;
    std::vector<float> clicks(count, 0.0F);
    for (std::size_t n = 6000; n < count; n += 9000) {
        for (std::size_t k = 0; k < 400 && n + k < count; ++k) {
            clicks[n + k] = static_cast<float>(0.5 * std::exp(-static_cast<double>(k) / 60.0) *
                                               std::sin(2.0 * std::numbers::pi * 3000.0 * static_cast<double>(k) / 48000.0));
        }
    }
    const std::vector<std::vector<float>> input{clicks, clicks};
    ac4::EncoderConfig config;
    config.bitrate_kbps = 256;
    const Encoded encoded = encode(config, input, 2048);
    std::size_t short_frames = 0;
    for (const ac4::SyntaxRecord& r : encoded.trace) {
        if (r.name == "b_long_frame" && r.value == 0) {
            ++short_frames;
        }
    }
    CHECK(short_frames > 0);
    check_frames_read_back(encoded);
    const auto decoded = decode(encoded.frames);
    const Score s = score(input[0], decoded[0], 3072 + kDecoderDelay);
    CHECK(std::abs(s.gain_db) < 0.5);
    CHECK(s.snr_db > 20.0);
}

TEST_CASE("at 44.1 kHz frames alternate sizes to keep the bit rate", "[ac4enc][encoder]") {
    const std::size_t count = 44100;
    const std::vector<std::vector<float>> input{tone(440.0, 0.1, count, 44100), tone(660.0, 0.1, count, 44100)};
    ac4::EncoderConfig config;
    config.sample_rate_hz = 44100;
    config.bitrate_kbps = 192;
    const Encoded encoded = encode(config, input, 3000);
    std::size_t bytes = 0;
    for (const ac4::EncodedFrame& frame : encoded.frames) {
        bytes += frame.raw_ac4_frame.size();
        CHECK((frame.raw_ac4_frame.size() == 1114 || frame.raw_ac4_frame.size() == 1115));
    }
    const double kbps = 8.0 * static_cast<double>(bytes) * 44100.0 / (2048.0 * static_cast<double>(encoded.frames.size())) / 1000.0;
    CHECK(std::abs(kbps - 192.0) < 0.5);
    check_frames_read_back(encoded);
    const auto parsed = ac4::parse_raw_frame(encoded.frames.front().raw_ac4_frame);
    REQUIRE(parsed.has_value());
    CHECK(parsed->toc.sample_rate_hz == 44100);
}

TEST_CASE("sequence_counter starts at 0 and I-frames come at the configured interval", "[ac4enc][encoder]") {
    const std::vector<std::vector<float>> input{std::vector<float>(48000, 0.0F), std::vector<float>(48000, 0.0F)};
    ac4::EncoderConfig config;
    config.iframe_interval = 5;
    const Encoded encoded = encode(config, input, 48000);
    REQUIRE(encoded.frames.size() > 10);
    for (std::size_t f = 0; f < encoded.frames.size(); ++f) {
        const auto parsed = ac4::parse_raw_frame(encoded.frames[f].raw_ac4_frame);
        REQUIRE(parsed.has_value());
        CHECK(parsed->toc.sequence_counter == static_cast<int>(f));
        CHECK(parsed->toc.b_iframe_global == (f % 5 == 0));
        CHECK(encoded.frames[f].iframe == (f % 5 == 0));
    }
}
