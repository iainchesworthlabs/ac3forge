// ac4::Encoder end to end: what it writes, read back by the inspector, the
// decoder's syntax walk and the decoder's PCM path. Each stream's frames are
// the size the bit rate gives, every substream reads to its end with the trace
// the encoder recorded, and the decoded output is the input, delayed by the
// encoder's delay and the decoder's frame alignment, at unity gain, with
// each channel's tone on its own channel.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

// The decoder's delay at frame_rate_index 13: d_pcm (Part 1 Table 188), the
// QMF banks' 577 samples and six QMF slots (5.7.1).
constexpr int kDecoderDelay = 352 + 577 + 6 * 64;

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
    for (const int channels : {0, 3, 4, 7, 8, 9}) {
        CAPTURE(channels);
        config.channels = channels;
        CHECK(ac4::Encoder::create(config).error() == ac4::EncodeError::kInvalidConfig);
    }
    // A 7.X pair wants seven or eight channels.
    config.channels = 6;
    config.experimental.seven_x = ac4::AdditionalPair::kBack;
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

namespace {

// The energy of x in [lo_hz, hi_hz), over Hann-windowed blocks of 2 048
// samples from `first`, by a direct DFT of the bins there.
double band_energy(std::span<const float> x, std::size_t first, std::size_t blocks, double lo_hz, double hi_hz,
                   int rate) {
    constexpr std::size_t kBlock = 2048;
    const double bin_hz = static_cast<double>(rate) / kBlock;
    const auto lo = static_cast<std::size_t>(lo_hz / bin_hz);
    const auto hi = static_cast<std::size_t>(hi_hz / bin_hz);
    double energy = 0.0;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t at = first + b * kBlock;
        for (std::size_t k = lo; k < hi; ++k) {
            double re = 0.0;
            double im = 0.0;
            for (std::size_t n = 0; n < kBlock && at + n < x.size(); ++n) {
                const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / kBlock);
                const double phase = 2.0 * std::numbers::pi * static_cast<double>(k * n) / kBlock;
                re += w * static_cast<double>(x[at + n]) * std::cos(phase);
                im -= w * static_cast<double>(x[at + n]) * std::sin(phase);
            }
            energy += re * re + im * im;
        }
    }
    return energy;
}

std::size_t count_records(const Encoded& encoded, std::string_view name, std::uint64_t value) {
    return static_cast<std::size_t>(std::count_if(encoded.trace.begin(), encoded.trace.end(),
                                                  [&](const ac4::SyntaxRecord& r) { return r.name == name && r.value == value; }));
}

// Two tones under every crossover, noise over 14 to 16 kHz, and castanet-like
// bursts now and then.
std::vector<std::vector<float>> mixed(std::size_t count, int rate, int channels) {
    std::vector<std::vector<float>> out;
    std::uint32_t seed = 12345;
    for (int c = 0; c < channels; ++c) {
        std::vector<float> x = tone(c == 0 ? 440.0 : 660.0, 0.1, count, rate);
        const std::vector<float> high = tone(15000.0 + 100.0 * c, 0.01, count, rate);
        for (std::size_t n = 0; n < count; ++n) {
            seed = seed * 1664525U + 1013904223U;
            const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.02;
            const double burst = (n % 7000) < 300 ? noise * 10.0 * std::exp(-static_cast<double>(n % 7000) / 60.0) : 0.0;
            x[n] += high[n] + static_cast<float>(noise * 0.2 + burst);
        }
        out.push_back(std::move(x));
    }
    return out;
}

}  // namespace

TEST_CASE("ASPX streams read back with the encoder's trace at every rate, channel count and tool",
          "[ac4enc][encoder][aspx]") {
    struct Config {
        int channels;
        int rate;
        int kbps;
        bool balance;
        bool varvar;
        bool interleave;
    };
    for (const Config c : {Config{2, 48000, 48, false, false, false}, Config{2, 48000, 64, false, false, false},
                           Config{2, 48000, 96, true, true, true}, Config{2, 48000, 144, false, false, false},
                           Config{1, 48000, 24, false, true, false}, Config{1, 48000, 32, false, false, true},
                           Config{2, 44100, 64, true, false, false}, Config{1, 44100, 48, false, false, false},
                           Config{2, 48000, 16, true, true, true}}) {
        CAPTURE(c.channels, c.rate, c.kbps, c.balance, c.varvar, c.interleave);
        const std::size_t count = static_cast<std::size_t>(c.rate) * 2;
        ac4::EncoderConfig config;
        config.channels = c.channels;
        config.sample_rate_hz = c.rate;
        config.bitrate_kbps = c.kbps;
        config.iframe_interval = 7;
        config.experimental.aspx_balance = c.balance;
        config.experimental.aspx_varvar = c.varvar;
        config.experimental.aspx_interleave = c.interleave;
        const Encoded encoded = encode(config, mixed(count, c.rate, c.channels), 3001);
        CHECK(count_records(encoded, c.channels == 2 ? "stereo_codec_mode" : "mono_codec_mode", 1) ==
              encoded.frames.size());
        check_frames_read_back(encoded);
        const auto decoded = decode(encoded.frames);
        REQUIRE(decoded.size() == static_cast<std::size_t>(c.channels));
        // The tone below the crossover comes through at unity gain, from 16
        // kbps a channel; below that the rate is past what DEE writes.
        if (c.kbps / c.channels >= 16) {
            const Score s = score(tone(440.0, 0.1, count, c.rate), decoded[0], 3072 + kDecoderDelay);
            CHECK(std::abs(s.gain_db) < 0.5);
        }
    }
}

TEST_CASE("ASPX recreates the band above the crossover at its energy", "[ac4enc][encoder][aspx]") {
    // Noise over 11 to 20 kHz on a tone: the crossover is 7.5 kHz at 48 kbps
    // and 13.5 kHz at 96, and A-SPX recreates the band over it at the
    // source's energy, give or take its envelopes' steps and the limiter.
    const std::size_t count = 48000 * 2;
    std::vector<float> x = tone(1000.0, 0.1, count, 48000);
    std::uint32_t seed = 99;
    std::vector<double> white(count);
    for (double& w : white) {
        seed = seed * 1664525U + 1013904223U;
        w = static_cast<double>(seed >> 8) / 16777216.0 - 0.5;
    }
    // A crude band-pass: white noise less its smoothed self keeps the top.
    for (std::size_t n = 8; n < count; ++n) {
        double sum = 0.0;
        for (std::size_t k = 0; k < 8; ++k) {
            sum += white[n - k];
        }
        x[n] += static_cast<float>(0.2 * (white[n] - sum / 8.0));
    }
    for (const int kbps : {48, 96}) {
        CAPTURE(kbps);
        ac4::EncoderConfig config;
        config.bitrate_kbps = kbps;
        const Encoded encoded = encode(config, {x, x}, 4096);
        const auto decoded = decode(encoded.frames);
        const std::size_t lag = 3072 + kDecoderDelay;
        // Under 17.25 kHz, the top of the A-SPX range at 48 kbps.
        const double source = band_energy(x, 8192, 8, 14000.0, 17000.0, 48000);
        const double output = band_energy(decoded[0], 8192 + lag, 8, 14000.0, 17000.0, 48000);
        CHECK(std::abs(10.0 * std::log10(output / source)) < 3.0);
        const Score s = score(tone(1000.0, 0.1, count, 48000), decoded[0], lag);
        CHECK(std::abs(s.gain_db) < 0.5);
    }
}

TEST_CASE("the experimental A-SPX tools do what they are for", "[ac4enc][encoder][aspx]") {
    const std::size_t count = 48000 * 2;
    SECTION("balance codes equal channels as a sum and a centred balance") {
        const std::vector<float> x = mixed(count, 48000, 1).front();
        ac4::EncoderConfig config;
        config.bitrate_kbps = 48;
        config.experimental.aspx_balance = true;
        const Encoded encoded = encode(config, {x, x}, 4096);
        CHECK(count_records(encoded, "aspx_balance", 1) * 10 >= encoded.frames.size() * 9);
        check_frames_read_back(encoded);
        const auto decoded = decode(encoded.frames);
        const Score l = score(tone(440.0, 0.1, count, 48000), decoded[0], 3072 + kDecoderDelay);
        const Score r = score(tone(440.0, 0.1, count, 48000), decoded[1], 3072 + kDecoderDelay);
        CHECK(std::abs(l.gain_db - r.gain_db) < 0.1);
    }
    SECTION("VARVAR frames an attack in an interval that starts where the last ran on") {
        std::vector<float> clicks(count, 0.0F);
        std::uint32_t seed = 7;
        for (std::size_t n = 6000; n + 1200 < count; n += 2600) {
            for (std::size_t k = 0; k < 1200; ++k) {
                seed = seed * 1664525U + 1013904223U;
                const double noise = static_cast<double>(seed >> 8) / 16777216.0 - 0.5;
                clicks[n + k] = static_cast<float>(0.8 * std::exp(-static_cast<double>(k) / 150.0) * noise);
            }
        }
        ac4::EncoderConfig config;
        config.bitrate_kbps = 64;
        config.experimental.aspx_varvar = true;
        const Encoded encoded = encode(config, {clicks, clicks}, 4096);
        CHECK(count_records(encoded, "aspx_int_class", 0b111) > 0);
        check_frames_read_back(encoded);
    }
    SECTION("interleaving codes a steady tone above the crossover where it is") {
        std::vector<float> x = tone(440.0, 0.1, count, 48000);
        const std::vector<float> high = tone(17100.0, 0.05, count, 48000);
        for (std::size_t n = 0; n < count; ++n) {
            x[n] += high[n];
        }
        for (const bool interleave : {false, true}) {
            CAPTURE(interleave);
            ac4::EncoderConfig config;
            config.bitrate_kbps = 96;
            config.experimental.aspx_interleave = interleave;
            const Encoded encoded = encode(config, {x, x}, 4096);
            check_frames_read_back(encoded);
            const auto decoded = decode(encoded.frames);
            // A sinusoid sits at its subband's edge; the spectral frontend
            // codes the tone at 17.1 kHz itself.
            const Score s = score(high, decoded[0], 3072 + kDecoderDelay);
            if (interleave) {
                CHECK(count_records(encoded, "aspx_fic_present", 1) * 10 >= encoded.frames.size() * 9);
                CHECK(std::abs(s.gain_db) < 1.0);
            } else {
                CHECK(s.gain_db < -20.0);
            }
        }
    }
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

namespace {

// gen_ac4_baseline.py's tones, L R C LFE Ls Rs, and the next two primes for
// a 7.X layout's additional pair: no tone sits on another's harmonic.
constexpr std::array<double, 8> kToneHz = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0, 1117.0, 1289.0};

// The tones of a layout's channels, in the decoder's order: L R C, the LFE if
// `lfe`, Ls Rs, and `extra` more.
std::vector<double> layout_tones(bool lfe, int extra) {
    std::vector<double> hz = {kToneHz[0], kToneHz[1], kToneHz[2]};
    if (lfe) {
        hz.push_back(kToneHz[3]);
    }
    hz.insert(hz.end(), {kToneHz[4], kToneHz[5]});
    for (int k = 0; k < extra; ++k) {
        hz.push_back(kToneHz[static_cast<std::size_t>(6 + k)]);
    }
    return hz;
}

// The amplitude of x's component at `hz` over `count` samples from `first`,
// through a Hann window: another tone's sidelobes are far below what this
// measures, which a plain projection's are not over a finite span.
double tone_amplitude(std::span<const float> x, std::size_t first, std::size_t count, double hz, int rate) {
    double re = 0.0;
    double im = 0.0;
    double weight = 0.0;
    for (std::size_t n = 0; n < count && first + n < x.size(); ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(count));
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(first + n) / rate;
        re += w * static_cast<double>(x[first + n]) * std::cos(phase);
        im -= w * static_cast<double>(x[first + n]) * std::sin(phase);
        weight += w;
    }
    return 2.0 * std::hypot(re, im) / weight;
}

// Each channel's own tone at unity gain and 40 dB of SNR, and every other
// channel's tone 60 dB under it there: planning/ac4.md, E3's exit.
void check_routing(const std::vector<double>& hz, const std::vector<std::vector<float>>& input,
                   const std::vector<std::vector<float>>& decoded) {
    REQUIRE(decoded.size() == hz.size());
    const std::size_t lag = 3072 + kDecoderDelay;
    const std::size_t first = lag + 4096;
    const std::size_t span = input.front().size() - 8192;
    for (std::size_t c = 0; c < hz.size(); ++c) {
        CAPTURE(c, hz[c]);
        const Score own = score(input[c], decoded[c], lag);
        CHECK(std::abs(own.gain_db) < 0.2);
        CHECK(own.snr_db > 40.0);
        const double level = tone_amplitude(decoded[c], first, span, hz[c], 48000);
        for (std::size_t other = 0; other < hz.size(); ++other) {
            if (other != c) {
                CAPTURE(other, hz[other]);
                const double leak = tone_amplitude(decoded[c], first, span, hz[other], 48000);
                CHECK(20.0 * std::log10(level / std::max(leak, 1e-30)) > 60.0);
            }
        }
    }
}

// Noise over 13 to 20 kHz: above every 5.X and 7.X crossover.
std::vector<float> high_noise(std::size_t count) {
    std::vector<float> x(count, 0.0F);
    std::uint32_t seed = 4242;
    std::vector<double> white(count);
    for (double& w : white) {
        seed = seed * 1664525U + 1013904223U;
        w = static_cast<double>(seed >> 8) / 16777216.0 - 0.5;
    }
    // White noise less its smoothed self keeps the top.
    for (std::size_t n = 6; n < count; ++n) {
        double sum = 0.0;
        for (std::size_t k = 0; k < 6; ++k) {
            sum += white[n - k];
        }
        x[n] = static_cast<float>(0.3 * (white[n] - sum / 6.0));
    }
    return x;
}

}  // namespace

TEST_CASE("5.0 and 5.1 put each channel's tone on its own channel in SIMPLE and ASPX, the LFE's included",
          "[ac4enc][encoder][multichannel]") {
    const std::size_t count = 48000 * 2;
    for (const bool lfe : {true, false}) {
        // ASPX below 76.8 kbps a channel, SIMPLE from there.
        for (const int kbps : {192, 384}) {
            CAPTURE(lfe, kbps);
            const std::vector<double> hz = layout_tones(lfe, 0);
            std::vector<std::vector<float>> input;
            for (const double f : hz) {
                input.push_back(tone(f, 0.1, count, 48000));
            }
            ac4::EncoderConfig config;
            config.channels = static_cast<int>(hz.size());
            config.bitrate_kbps = kbps;
            const Encoded encoded = encode(config, input, 3333);
            CHECK(count_records(encoded, "5_X_codec_mode", kbps < 384 ? 1U : 0U) == encoded.frames.size());
            // DEE's form: coding_config 0 and 2ch_mode 0 in every frame.
            CHECK(count_records(encoded, "coding_config", 0) == encoded.frames.size());
            CHECK(count_records(encoded, "2ch_mode", 0) == encoded.frames.size());
            check_frames_read_back(encoded);
            check_routing(hz, input, decode(encoded.frames));
        }
    }
}

TEST_CASE("the encoder's 5.1 table of contents and MP4 description", "[ac4enc][encoder][multichannel]") {
    ac4::EncoderConfig config;
    config.channels = 6;
    config.bitrate_kbps = 384;
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const auto& chan = encoder->toc().substream_groups.at(0).substreams.at(0).chan;
    REQUIRE(chan.has_value());
    CHECK(chan->ch_mode == 4);
    CHECK(encoder->codec_mode() == ac4::CodecMode::kSimple);
    CHECK_FALSE(ac4::build_dac4(encoder->toc()).empty());
}

TEST_CASE("each aspx_data element fills the high band of the channels Table 213 gives it",
          "[ac4enc][encoder][multichannel][aspx]") {
    // Noise above the crossover in one channel: after decoding only that
    // channel carries it, so the encoder's pairing of A-SPX channels is the
    // decoder's, element by element.
    const std::size_t count = 48000;
    struct Case {
        ac4::AdditionalPair pair;
        int channels;
        int kbps;
        std::size_t noisy;
    };
    for (const Case c : {Case{ac4::AdditionalPair::kNone, 6, 192, 2}, Case{ac4::AdditionalPair::kNone, 6, 256, 4},
                         Case{ac4::AdditionalPair::kNone, 5, 192, 1}, Case{ac4::AdditionalPair::kWide, 8, 320, 6},
                         Case{ac4::AdditionalPair::kWide, 8, 320, 4}, Case{ac4::AdditionalPair::kBack, 7, 320, 5}}) {
        CAPTURE(static_cast<int>(c.pair), c.channels, c.kbps, c.noisy);
        std::vector<std::vector<float>> input;
        for (int ch = 0; ch < c.channels; ++ch) {
            input.push_back(tone(kToneHz[static_cast<std::size_t>(ch)], 0.05, count, 48000));
        }
        const std::vector<float> noise = high_noise(count);
        for (std::size_t n = 0; n < count; ++n) {
            input[c.noisy][n] += noise[n];
        }
        ac4::EncoderConfig config;
        config.channels = c.channels;
        config.bitrate_kbps = c.kbps;
        config.experimental.seven_x = c.pair;
        const Encoded encoded = encode(config, input, 4096);
        const std::string_view mode = c.pair == ac4::AdditionalPair::kNone ? "5_X_codec_mode" : "7_X_codec_mode";
        REQUIRE(count_records(encoded, mode, 1) == encoded.frames.size());
        check_frames_read_back(encoded);
        const auto decoded = decode(encoded.frames);
        const std::size_t lag = 3072 + kDecoderDelay;
        const double source = band_energy(noise, 8192, 6, 14000.0, 19000.0, 48000);
        for (std::size_t ch = 0; ch < decoded.size(); ++ch) {
            CAPTURE(ch);
            const double output = band_energy(decoded[ch], 8192 + lag, 6, 14000.0, 19000.0, 48000);
            const double db = 10.0 * std::log10(std::max(output, 1e-30) / source);
            if (ch == c.noisy) {
                CHECK(std::abs(db) < 3.0);
            } else {
                CHECK(db < -30.0);
            }
        }
    }
}

TEST_CASE("the 7.X element's three layouts put each tone on its own channel", "[ac4enc][encoder][multichannel]") {
    const std::size_t count = 48000 * 2;
    for (const ac4::AdditionalPair pair : {ac4::AdditionalPair::kBack, ac4::AdditionalPair::kWide,
                                           ac4::AdditionalPair::kTopFront}) {
        for (const bool lfe : {true, false}) {
            // ASPX below 76.8 kbps a channel, SIMPLE from there.
            for (const int kbps : {448, 640}) {
                CAPTURE(static_cast<int>(pair), lfe, kbps);
                const std::vector<double> hz = layout_tones(lfe, 2);
                std::vector<std::vector<float>> input;
                for (const double f : hz) {
                    input.push_back(tone(f, 0.1, count, 48000));
                }
                ac4::EncoderConfig config;
                config.channels = static_cast<int>(hz.size());
                config.bitrate_kbps = kbps;
                config.experimental.seven_x = pair;
                const Encoded encoded = encode(config, input, 5000);
                CHECK(count_records(encoded, "7_X_codec_mode", kbps < 7 * 76.8 ? 1U : 0U) == encoded.frames.size());
                CHECK(count_records(encoded, "b_use_sap_add_ch", 0) == encoded.frames.size());
                check_frames_read_back(encoded);
                check_routing(hz, input, decode(encoded.frames));
            }
        }
    }
}

TEST_CASE("the experimental coding configurations choose frame by frame and decode where they should",
          "[ac4enc][encoder][multichannel]") {
    // A second of each: independent tones; one signal in L, R and C at three
    // levels; and L again in Ls and R in Rs. Frames choose among the coding
    // configurations, and every channel comes back as its input.
    const std::size_t second = 48000;
    const std::vector<float> shared = mixed(second, 48000, 1).front();
    std::vector<std::vector<float>> input(6, std::vector<float>(3 * second, 0.0F));
    for (std::size_t c = 0; c < 6; ++c) {
        const std::vector<float> t = tone(kToneHz[c], 0.1, second, 48000);
        for (std::size_t n = 0; n < second; ++n) {
            input[c][n] = t[n];
        }
    }
    const std::vector<float> left = tone(kToneHz[0], 0.1, second, 48000);
    const std::vector<float> right = tone(kToneHz[1], 0.1, second, 48000);
    for (std::size_t n = 0; n < second; ++n) {
        input[0][second + n] = shared[n];
        input[1][second + n] = 0.8F * shared[n];
        input[2][second + n] = 0.6F * shared[n];
        input[0][2 * second + n] = left[n] + shared[n];
        input[1][2 * second + n] = right[n] - shared[n];
        input[4][2 * second + n] = input[0][2 * second + n];
        input[5][2 * second + n] = input[1][2 * second + n];
    }
    for (const int kbps : {256, 448}) {
        CAPTURE(kbps);
        ac4::EncoderConfig config;
        config.channels = 6;
        config.bitrate_kbps = kbps;
        config.experimental.coding_configs = true;
        const Encoded encoded = encode(config, input, 4800);
        std::size_t configs = 0;
        for (const std::uint64_t value : {0U, 1U, 2U, 3U}) {
            configs += count_records(encoded, "coding_config", value) > 0 ? 1U : 0U;
        }
        CHECK(configs >= 2);
        check_frames_read_back(encoded);
        const auto decoded = decode(encoded.frames);
        REQUIRE(decoded.size() == 6);
        for (std::size_t c = 0; c < 6; ++c) {
            if (c == 3) {
                continue;  // the LFE is silent after the first second
            }
            CAPTURE(c);
            const Score s = score(input[c], decoded[c], 3072 + kDecoderDelay);
            CHECK(std::abs(s.gain_db) < 0.5);
            CHECK(s.snr_db > 15.0);
        }
    }
}