// ac4::Decoder::decode() and the reconstruction behind it (src/ac4dec/src/pcm):
// the noise fill's random number generator against the text's own closed
// form, and the committed DEE streams decoded to PCM - each channel's tone on
// its own channel in stereo, 5.1 and 5.1.4, the LFE's included, in full and
// core decoding, an ASPX stream's high band rebuilt, and the IMS streams'
// frame rates through the sample rate converter.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "dsp/qmf.hpp"
#include "pcm/snf_random.hpp"
#include "pcm/stereo.hpp"
#include "syntax/asf.hpp"
#include "syntax/context.hpp"
#include "tables/noise_tables.hpp"
#include "tables/sfb_tables.hpp"

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> read_stream(const std::string& leg) {
    const fs::path path = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

struct Decoded {
    std::vector<ac4::Speaker> speakers;
    std::vector<std::vector<float>> channels;
    int sample_rate_hz = 0;
    std::size_t frames = 0;
};

Decoded decode_all(const std::string& leg, ac4::DecodingMode decoding = ac4::DecodingMode::kFull,
                   ac4::DownmixTarget target = ac4::DownmixTarget::kAsCoded) {
    const std::vector<std::byte> stream = read_stream(leg);
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    ac4::DecoderConfig config;
    config.decoding = decoding;
    config.output.downmix = target;
    ac4::Decoder decoder(config);
    Decoded out;
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        if (out.frames == 0) {
            out.speakers = pcm.speakers;
            out.sample_rate_hz = pcm.sample_rate_hz;
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
        ++out.frames;
    }
    return out;
}

// The power of `samples` at `hz`, by Goertzel's recurrence, normalised so that
// a sine of amplitude A gives A^2 / 4 whatever the length.
double tone_power(std::span<const float> samples, double hz, int sample_rate_hz) {
    const double w = 2.0 * std::numbers::pi * hz / static_cast<double>(sample_rate_hz);
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float x : samples) {
        const double s0 = static_cast<double>(x) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    const auto n = static_cast<double>(samples.size());
    return power / (n * n);
}

// Each QMF subband's mean energy over `samples` (Part 1 5.7.3's analysis).
std::vector<double> subband_energy(std::span<const float> samples) {
    ac4::detail::dsp::QmfAnalysis<double> analysis;
    const std::size_t slots = samples.size() / 64;
    std::vector<double> pcm(slots * 64);
    for (std::size_t n = 0; n < pcm.size(); ++n) {
        pcm[n] = static_cast<double>(samples[n]);
    }
    std::vector<std::complex<double>> q(pcm.size());
    analysis.process(pcm, q);
    std::vector<double> energy(64, 0.0);
    for (std::size_t ts = 0; ts < slots; ++ts) {
        for (std::size_t sb = 0; sb < 64; ++sb) {
            energy[sb] += std::norm(q[ts * 64 + sb]) / static_cast<double>(slots);
        }
    }
    return energy;
}

}  // namespace

TEST_CASE("Pseudocode 24's reset lands where stepping Pseudocode 57's increments lands", "[ac4dec][pcm]") {
    // Pseudocode 55's state, stepped 255 * (sequence_counter % 256) times
    // with `x = x++` read as an increment, is Pseudocode 24's closed form for
    // every counter; ERRATA.md, "x = x++ in Pseudocode 57".
    ac4::detail::RandGenState stepped;
    std::size_t steps = 0;
    for (int counter = 0; counter < 256; ++counter) {
        CAPTURE(counter);
        const std::size_t target = 255U * static_cast<std::size_t>(counter);
        while (steps < target) {
            ac4::detail::advance(stepped);
            ++steps;
        }
        const ac4::detail::RandGenState reset = ac4::detail::reset_rand_gen_state_snf(counter);
        CHECK(reset.offset_a == stepped.offset_a);
        CHECK(reset.offset_b == stepped.offset_b);
        CHECK(reset.state_idx == stepped.state_idx);
        CHECK(reset.current_idx == stepped.current_idx);
        // sequence_counter is ten bits; the reset repeats every 256.
        const ac4::detail::RandGenState again = ac4::detail::reset_rand_gen_state_snf(counter + 256);
        CHECK(again.state_idx == reset.state_idx);
        CHECK(again.current_idx == reset.current_idx);
    }
}

TEST_CASE("GetRandomNoiseValue adds two table entries and then steps", "[ac4dec][pcm]") {
    ac4::detail::RandGenState state = ac4::detail::reset_rand_gen_state_snf(0);
    // Pseudocode 55's state: current 0, state 1.
    CHECK(ac4::detail::get_random_noise_value(state) ==
          ac4::detail::tables::kRandomNoiseTable[0] + ac4::detail::tables::kRandomNoiseTable[1]);
    // After one step: offset_a 1, state_idx 1 + 1 + 0 + 1 = 3, current_idx 1.
    CHECK(state.offset_a == 1);
    CHECK(state.state_idx == 3);
    CHECK(state.current_idx == 1);
    CHECK(ac4::detail::get_random_noise_value(state) ==
          ac4::detail::tables::kRandomNoiseTable[1] + ac4::detail::tables::kRandomNoiseTable[3]);
}

TEST_CASE("a pair with b_dual_maxsfb is laid out alike before its stereo processing",
          "[ac4dec][pcm]") {
    // Two windows of 1 024 lines in groups of their own; the first track
    // sends 10 and 8 bands, the second 4 and 6. Each line holds its group,
    // band and place: 1000 (track) + 100 g + the line's index in its band's
    // run.
    ac4::detail::SubstreamContext ctx;
    ac4::detail::AsfPsyInfo psy;
    psy.b_long_frame = false;
    psy.transf_length = {3, 3};
    psy.num_windows = 2;
    psy.num_window_groups = 2;
    psy.window_to_group = {0, 1};
    psy.num_win_in_group = {1, 1};
    const std::span<const std::uint16_t> offsets = ac4::detail::tables::sfb_offsets_48(1024);
    REQUIRE(offsets.size() > 11);
    ac4::detail::SfData first;
    ac4::detail::SfData second;
    first.max_sfb = {10, 8};
    second.max_sfb = {4, 6};
    const auto lines_of = [&](const ac4::detail::SfData& data, double track) {
        std::vector<double> lines;
        for (int g = 0; g < 2; ++g) {
            for (int sfb = 0; sfb < data.max_sfb[static_cast<std::size_t>(g)]; ++sfb) {
                const auto si = static_cast<std::size_t>(sfb);
                for (int k = offsets[si]; k < offsets[si + 1]; ++k) {
                    lines.push_back(1000.0 * track + 100.0 * g + (k - offsets[si]) + 0.01 * sfb);
                }
            }
        }
        return lines;
    };
    std::vector<double> track0 = lines_of(first, 1.0);
    std::vector<double> track1 = lines_of(second, 2.0);
    ac4::detail::SfData common;
    ac4::detail::align_tracks(ctx, psy, first, second, track0, track1, common);
    CHECK(common.max_sfb[0] == 10);
    CHECK(common.max_sfb[1] == 8);
    REQUIRE(track0.size() == static_cast<std::size_t>(offsets[10] + offsets[8]));
    REQUIRE(track1.size() == track0.size());
    for (int g = 0; g < 2; ++g) {
        const auto gi = static_cast<std::size_t>(g);
        for (int sfb = 0; sfb < common.max_sfb[gi]; ++sfb) {
            const auto si = static_cast<std::size_t>(sfb);
            const std::size_t at = common.sect_sfb_offset[gi][si];
            INFO("group " << g << ", band " << sfb);
            CHECK(track0[at] == 1000.0 + 100.0 * g + 0.01 * sfb);
            const bool sent = sfb < second.max_sfb[gi];
            CHECK(track1[at] == (sent ? 2000.0 + 100.0 * g + 0.01 * sfb : 0.0));
        }
    }

    // M/S over the first track's bands: in the bands the second leaves out,
    // both outputs are the first track's lines.
    ac4::detail::SfInfo info;
    info.psy = psy;
    ac4::detail::StereoParameters parameters;
    for (auto& group : parameters.abcd) {
        group.fill({1.0, 1.0, 1.0, -1.0});
    }
    ac4::detail::apply_stereo(info, common, parameters, track0, track1);
    const std::size_t sixth = common.sect_sfb_offset[0][6];
    CHECK(track0[sixth] == 1000.0 + 0.01 * 6);
    CHECK(track1[sixth] == 1000.0 + 0.01 * 6);
    const std::size_t second_band = common.sect_sfb_offset[1][2];
    CHECK(track0[second_band] == (1100.0 + 0.01 * 2) + (2100.0 + 0.01 * 2));
    CHECK(track1[second_band] == (1100.0 + 0.01 * 2) - (2100.0 + 0.01 * 2));
}

TEST_CASE("a SIMPLE stereo stream decodes each tone to its own channel", "[ac4dec][pcm]") {
    const Decoded decoded = decode_all("ac4-20-tones-192");
    REQUIRE(decoded.speakers == std::vector<ac4::Speaker>{ac4::Speaker::kLeft, ac4::Speaker::kRight});
    CHECK(decoded.sample_rate_hz == 48000);
    for (const auto& channel : decoded.channels) {
        CHECK(channel.size() == decoded.frames * 2048);
    }
    // tones_20: 331 Hz on L and 457 Hz on R (gen_ac4_baseline.py's TONE_HZ).
    // Skip the first and last half second, where the encoder's and decoder's
    // delays put silence.
    const std::size_t skip = 24000;
    REQUIRE(decoded.channels[0].size() > 4 * skip);
    const auto middle = [&](std::size_t c) {
        return std::span<const float>(decoded.channels[c]).subspan(skip, decoded.channels[c].size() - 2 * skip);
    };
    const double left_331 = tone_power(middle(0), 331.0, 48000);
    const double left_457 = tone_power(middle(0), 457.0, 48000);
    const double right_331 = tone_power(middle(1), 331.0, 48000);
    const double right_457 = tone_power(middle(1), 457.0, 48000);
    CHECK(left_331 > 1e4 * left_457);
    CHECK(right_457 > 1e4 * right_331);
    // Each tone at -20 dBFS, amplitude 0.1 of full scale 1.0, within 0.2 dB:
    // ERRATA.md, "Full scale, and the overlap-add's factor of two".
    const auto level_db = [](double power) { return 20.0 * std::log10(std::sqrt(4.0 * power) / 0.1); };
    CHECK(std::abs(level_db(left_331)) < 0.2);
    CHECK(std::abs(level_db(right_457)) < 0.2);
}

TEST_CASE("a SIMPLE stereo music stream decodes every frame", "[ac4dec][pcm]") {
    const Decoded decoded = decode_all("ac4-20-music-192");
    REQUIRE(decoded.channels.size() == 2);
    float peak = 0.0F;
    for (const auto& channel : decoded.channels) {
        CHECK(channel.size() == decoded.frames * 2048);
        for (const float x : channel) {
            peak = std::max(peak, std::abs(x));
        }
    }
    // Its source, music_20, peaks at 0.21 of full scale (-13.7 dBFS).
    CHECK(peak > 0.15F);
    CHECK(peak < 0.3F);
}

TEST_CASE("an ASPX stereo stream decodes every frame with its high band rebuilt", "[ac4dec][pcm]") {
    // DEE's 2.0 speech at 128 kbps: A-SPX recreates QMF subbands 36 (13.5
    // kHz) to 55 from the waveform-coded band below. Its source speech has
    // content up to 16 kHz, 10 to 25 dB under the 7.5 to 11 kHz band.
    const Decoded decoded = decode_all("ac4-20-speech-128");
    REQUIRE(decoded.speakers == std::vector<ac4::Speaker>{ac4::Speaker::kLeft, ac4::Speaker::kRight});
    for (const auto& channel : decoded.channels) {
        REQUIRE(channel.size() == decoded.frames * 2048);
        const std::vector<double> energy = subband_energy(channel);
        const auto band = [&](std::size_t first, std::size_t last) {
            double sum = 0.0;
            for (std::size_t sb = first; sb < last; ++sb) {
                sum += energy[sb];
            }
            return 10.0 * std::log10(sum / static_cast<double>(last - first) + 1e-30);
        };
        const double waveform = band(20, 30);   // 7.5 to 11.25 kHz
        const double extension = band(36, 43);  // 13.5 to 16.1 kHz, A-SPX's
        CAPTURE(waveform, extension);
        CHECK(extension < waveform);
        CHECK(extension > waveform - 30.0);
    }
}

TEST_CASE("a SIMPLE 5.1 stream decodes each tone to its own channel, the LFE's included", "[ac4dec][pcm]") {
    // tones_51: L R C LFE Ls Rs at 331, 457, 613, 47, 787 and 953 Hz, each at
    // -20 dBFS (gen_ac4_baseline.py's TONE_HZ).
    const Decoded decoded = decode_all("ac4-51-tones-384");
    using S = ac4::Speaker;
    REQUIRE(decoded.speakers ==
            std::vector<S>{S::kLeft, S::kRight, S::kCentre, S::kLfe, S::kLeftSurround, S::kRightSurround});
    constexpr std::array<double, 6> kTone = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};
    const std::size_t skip = 24000;
    REQUIRE(decoded.channels[0].size() > 4 * skip);
    for (std::size_t c = 0; c < 6; ++c) {
        CAPTURE(c);
        const auto middle =
            std::span<const float>(decoded.channels[c]).subspan(skip, decoded.channels[c].size() - 2 * skip);
        const double own = tone_power(middle, kTone[c], 48000);
        for (std::size_t other = 0; other < 6; ++other) {
            if (other != c) {
                CAPTURE(other);
                CHECK(own > 1e5 * tone_power(middle, kTone[other], 48000));
            }
        }
        // DEE low-passes the LFE before coding it, which costs the 47 Hz tone
        // 0.3 dB (tools/checks/score_ac4_decode.py, LFE_CHANNEL).
        const double level_db = 20.0 * std::log10(std::sqrt(4.0 * own) / 0.1);
        CHECK(std::abs(level_db) < (c == 3 ? 0.5 : 0.2));
    }
}

namespace {

// tones_514: L R C LFE Ls Rs Tfl Tfr Tbl Tbr, each at -20 dBFS
// (gen_ac4_baseline.py's TONE_HZ), coded by DEE as 7.1.4 with its backs
// silent (b_4_back_channels_present 0), so that its source layout, which
// decode() gives as coded, is 5.1.4.
constexpr std::array<double, 10> kTones514 = {331.0, 457.0,  613.0,  47.0,   787.0,
                                              953.0, 1117.0, 1289.0, 1453.0, 1621.0};
constexpr std::array<ac4::Speaker, 10> kTone514Speakers = {
    ac4::Speaker::kLeft,         ac4::Speaker::kRight,         ac4::Speaker::kCentre,
    ac4::Speaker::kLfe,          ac4::Speaker::kLeftSurround,  ac4::Speaker::kRightSurround,
    ac4::Speaker::kTopFrontLeft, ac4::Speaker::kTopFrontRight, ac4::Speaker::kTopBackLeft,
    ac4::Speaker::kTopBackRight};
const std::vector<ac4::Speaker> k514 = {ac4::Speaker::kLeft,         ac4::Speaker::kRight,
                                        ac4::Speaker::kCentre,       ac4::Speaker::kLfe,
                                        ac4::Speaker::kLeftSurround, ac4::Speaker::kRightSurround,
                                        ac4::Speaker::kTopFrontLeft, ac4::Speaker::kTopFrontRight,
                                        ac4::Speaker::kTopBackLeft,  ac4::Speaker::kTopBackRight};

// Each tone's level in each channel, in dB relative to -20 dBFS, past the
// first and last half second: [channel][tone].
std::vector<std::array<double, 10>> tone_levels(const Decoded& decoded) {
    const std::size_t skip = 24000;
    std::vector<std::array<double, 10>> out(decoded.channels.size());
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        REQUIRE(decoded.channels[c].size() > 4 * skip);
        const auto middle = std::span<const float>(decoded.channels[c])
                                .subspan(skip, decoded.channels[c].size() - 2 * skip);
        for (std::size_t t = 0; t < kTones514.size(); ++t) {
            const double power = tone_power(middle, kTones514[t], 48000);
            out[c][t] = 20.0 * std::log10(std::sqrt(4.0 * power + 1e-30) / 0.1);
        }
    }
    return out;
}

std::size_t channel_of(const Decoded& decoded, ac4::Speaker speaker) {
    const auto it = std::ranges::find(decoded.speakers, speaker);
    REQUIRE(it != decoded.speakers.end());
    return static_cast<std::size_t>(it - decoded.speakers.begin());
}

}  // namespace

TEST_CASE("the immersive element's SCPL and ASPX_SCPL streams decode each tone to its own channel",
          "[ac4dec][pcm][immersive]") {
    using S = ac4::Speaker;
    for (const char* leg : {"ac4-514-tones-768", "ac4-514-tones-512"}) {
        CAPTURE(leg);
        const Decoded decoded = decode_all(leg);
        REQUIRE(decoded.speakers == k514);
        const auto levels = tone_levels(decoded);
        for (std::size_t t = 0; t < kTones514.size(); ++t) {
            CAPTURE(t);
            const std::size_t own = channel_of(decoded, kTone514Speakers[t]);
            // DEE's LFE low-pass costs the 47 Hz tone 0.3 dB, as in 5.1.
            CHECK(std::abs(levels[own][t]) < (kTone514Speakers[t] == S::kLfe ? 0.5 : 0.2));
            for (std::size_t c = 0; c < levels.size(); ++c) {
                if (c != own) {
                    CAPTURE(c);
                    CHECK(levels[c][t] < -50.0);
                }
            }
        }
        // Rendered to 7.1.4, the backs the source leaves out come out silent
        // (Table 38's 5.X.4 row), and every other channel as coded.
        const Decoded wide = decode_all(leg, ac4::DecodingMode::kFull, ac4::DownmixTarget::k7X4);
        REQUIRE(wide.speakers.size() == 12);
        for (const S back : {S::kLeftBack, S::kRightBack}) {
            for (const float x : wide.channels[channel_of(wide, back)]) {
                REQUIRE(x == 0.0F);
            }
        }
        for (std::size_t c = 0; c < decoded.speakers.size(); ++c) {
            REQUIRE(wide.channels[channel_of(wide, decoded.speakers[c])] == decoded.channels[c]);
        }
    }
}

TEST_CASE("the immersive element's ASPX_ACPL_2 stream makes its top pairs by A-CPL",
          "[ac4dec][pcm][immersive]") {
    // At 256 kbps DEE codes F'' and G'', each top pair's sum, and A-CPL makes
    // Tfl and Tbl, Tfr and Tbr of them with its parameters: each top tone
    // keeps its level across its pair and is loudest in its own channel, and
    // stays out of every other channel. The rest are coded as in SCPL.
    using S = ac4::Speaker;
    const Decoded decoded = decode_all("ac4-514-tones-256");
    REQUIRE(decoded.speakers == k514);
    const auto levels = tone_levels(decoded);
    const auto power = [](double db) { return std::pow(10.0, db / 10.0); };
    for (std::size_t t = 0; t < kTones514.size(); ++t) {
        CAPTURE(t);
        const S speaker = kTone514Speakers[t];
        const std::size_t own = channel_of(decoded, speaker);
        const bool top = t >= 6;
        if (!top) {
            CHECK(std::abs(levels[own][t]) < (speaker == S::kLfe ? 0.5 : 0.2));
        }
        const std::size_t partner =
            top ? channel_of(decoded, kTone514Speakers[t < 8 ? t + 2 : t - 2]) : own;
        if (top) {
            CHECK(levels[own][t] > levels[partner][t]);
            const double pair_db =
                10.0 * std::log10(power(levels[own][t]) + power(levels[partner][t]));
            CHECK(std::abs(pair_db) < 2.5);
        }
        for (std::size_t c = 0; c < levels.size(); ++c) {
            if (c != own && c != partner) {
                CAPTURE(c);
                CHECK(levels[c][t] < -50.0);
            }
        }
    }
}

TEST_CASE("core decoding gives the immersive element's 5.X.2 core at the core gains",
          "[ac4dec][pcm][immersive]") {
    // Table 24 and clauses 4.8.3.11.2 and 4.8.3.14: L, R and C as coded; Ls,
    // Rs, Tsl and Tsr each the sum of the pair full decoding makes, over the
    // square root of 2, so every tone of those pairs 3 dB down, in every mode.
    // Then Table 45, with Table 130's gains (the streams send no custom
    // downmix data): Ls and Rs +3 dB, the source having no backs, which puts
    // their tones back at 0 dB, and Tsl and Tsr at gain_t1 + 3 dB, 0 dB.
    using S = ac4::Speaker;
    const double down = 20.0 * std::log10(std::numbers::sqrt2 / 2.0);
    for (const char* leg : {"ac4-514-tones-768", "ac4-514-tones-512", "ac4-514-tones-256"}) {
        CAPTURE(leg);
        const Decoded decoded = decode_all(leg, ac4::DecodingMode::kCore);
        REQUIRE(decoded.speakers == std::vector<S>{S::kLeft, S::kRight, S::kCentre, S::kLfe,
                                                   S::kLeftSurround, S::kRightSurround,
                                                   S::kTopSideLeft, S::kTopSideRight});
        const auto levels = tone_levels(decoded);
        const std::array<S, 10> core = {S::kLeft,        S::kRight,        S::kCentre,
                                        S::kLfe,         S::kLeftSurround, S::kRightSurround,
                                        S::kTopSideLeft, S::kTopSideRight, S::kTopSideLeft,
                                        S::kTopSideRight};
        for (std::size_t t = 0; t < kTones514.size(); ++t) {
            CAPTURE(t);
            const std::size_t own = channel_of(decoded, core[t]);
            const double expected = t < 6 ? 0.0 : down;
            CHECK(std::abs(levels[own][t] - expected) < (core[t] == S::kLfe ? 0.5 : 0.2));
            for (std::size_t c = 0; c < levels.size(); ++c) {
                if (c != own) {
                    CAPTURE(c);
                    CHECK(levels[c][t] < -50.0);
                }
            }
        }
    }
}

namespace {

// The phasor of `samples` at `hz`, (2 / N) sum x[n] e^(-j w n): for a sine of
// amplitude A over many periods, magnitude A. Linear in `samples`, so a mix of
// channels has the mix of their phasors.
std::complex<double> phasor(std::span<const float> samples, double hz, int sample_rate_hz) {
    const double w = 2.0 * std::numbers::pi * hz / static_cast<double>(sample_rate_hz);
    const std::complex<double> step = std::polar(1.0, -w);
    std::complex<double> rotor{1.0, 0.0};
    std::complex<double> sum{};
    for (std::size_t n = 0; n < samples.size(); ++n) {
        sum += static_cast<double>(samples[n]) * rotor;
        rotor *= step;
        if ((n & 1023U) == 1023U) {
            rotor = std::polar(1.0, -w * static_cast<double>(n + 1));
        }
    }
    return sum * (2.0 / static_cast<double>(samples.size()));
}

// Each tones_514 tone's phasor in each channel, past the first and last half
// second: [channel][tone].
std::vector<std::array<std::complex<double>, 10>> tone_phasors(const Decoded& decoded) {
    const std::size_t skip = 24000;
    std::vector<std::array<std::complex<double>, 10>> out(decoded.channels.size());
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        REQUIRE(decoded.channels[c].size() > 4 * skip);
        const auto middle = std::span<const float>(decoded.channels[c])
                                .subspan(skip, decoded.channels[c].size() - 2 * skip);
        for (std::size_t t = 0; t < kTones514.size(); ++t) {
            out[c][t] = phasor(middle, kTones514[t], decoded.sample_rate_hz);
        }
    }
    return out;
}

// A channel of a render as a mix of the channels decoded as coded.
struct Term {
    ac4::Speaker from;
    double weight;
};
using Mixes = std::vector<std::pair<ac4::Speaker, std::vector<Term>>>;

}  // namespace

TEST_CASE(
    "the immersive element's renders to 5.1 and 2.0 are the renderer's matrices, in both modes",
    "[ac4dec][pcm][immersive]") {
    // Rendered to 5.X.0 and to Lo/Ro, each tone's phasor in each channel is
    // the matrix's mix of its phasors decoded as coded (the source's 5.1.4 in
    // full decoding, the core's 5.1.2 in core decoding), to 0.01 dB where it
    // is heard: the renderer works in the QMF domain before the synthesis,
    // which is linear. The streams send no custom downmix data, so Table
    // 130's gains hold: full decoding's Table 43 (5.X.4 row) puts each top
    // pair into its side at gain_t2b and gain_t2e, -3 dB, and core
    // decoding's Table 46 over Table 45 puts Tsl and Tsr into their sides at
    // (gain_t2b + 3 dB) / (gain_t1 + 3 dB), 0 dB. Their stereo coefficients,
    // loro_centre_mixgain and loro_surround_mixgain 4, are -3 dB each (Tables
    // 149 and 149a), and they send no loudness correction.
    using S = ac4::Speaker;
    const double m3 = std::pow(10.0, -3.0 / 20.0);
    const auto db = [](std::complex<double> x) {
        return 20.0 * std::log10(std::abs(x) / 0.1 + 1e-30);
    };
    for (const char* leg : {"ac4-514-tones-768", "ac4-514-tones-512", "ac4-514-tones-256"}) {
        for (const ac4::DecodingMode decoding :
             {ac4::DecodingMode::kFull, ac4::DecodingMode::kCore}) {
            const bool core = decoding == ac4::DecodingMode::kCore;
            CAPTURE(leg, core);
            const Decoded coded = decode_all(leg, decoding);
            const auto in = tone_phasors(coded);
            const Mixes five = {
                {S::kLeft, {{S::kLeft, 1.0}}},
                {S::kRight, {{S::kRight, 1.0}}},
                {S::kCentre, {{S::kCentre, 1.0}}},
                {S::kLfe, {{S::kLfe, 1.0}}},
                {S::kLeftSurround,
                 core ? std::vector<Term>{{S::kLeftSurround, 1.0}, {S::kTopSideLeft, 1.0}}
                      : std::vector<Term>{{S::kLeftSurround, 1.0},
                                          {S::kTopFrontLeft, m3},
                                          {S::kTopBackLeft, m3}}},
                {S::kRightSurround,
                 core ? std::vector<Term>{{S::kRightSurround, 1.0}, {S::kTopSideRight, 1.0}}
                      : std::vector<Term>{{S::kRightSurround, 1.0},
                                          {S::kTopFrontRight, m3},
                                          {S::kTopBackRight, m3}}},
            };
            // Table 218's Lo/Ro from the 5.X.0.
            const auto side = [&](S s) {
                return std::ranges::find(five, s, &Mixes::value_type::first)->second;
            };
            const auto lo_ro = [&](S front, S surround) {
                std::vector<Term> out = {{front, 1.0}, {S::kCentre, m3}};
                for (const Term& term : side(surround)) {
                    out.push_back({term.from, m3 * term.weight});
                }
                return out;
            };
            const Mixes two = {{S::kLeft, lo_ro(S::kLeft, S::kLeftSurround)},
                               {S::kRight, lo_ro(S::kRight, S::kRightSurround)}};
            for (const auto& [target, mixes] : {std::pair{ac4::DownmixTarget::k5X, five},
                                                std::pair{ac4::DownmixTarget::kLoRo, two}}) {
                CAPTURE(ac4::describe(target));
                const Decoded rendered = decode_all(leg, decoding, target);
                REQUIRE(rendered.speakers.size() == mixes.size());
                const auto out = tone_phasors(rendered);
                for (const auto& [speaker, terms] : mixes) {
                    CAPTURE(ac4::describe(speaker));
                    const std::size_t o = channel_of(rendered, speaker);
                    for (std::size_t t = 0; t < kTones514.size(); ++t) {
                        CAPTURE(t);
                        std::complex<double> expected{};
                        for (const Term& term : terms) {
                            expected += term.weight * in[channel_of(coded, term.from)][t];
                        }
                        // Nothing but the output's rounding between them...
                        CHECK(std::abs(out[o][t] - expected) < 1e-6);
                        // ...and where the tone is heard, the same level to 0.01 dB.
                        if (db(expected) > -40.0) {
                            CHECK(std::abs(db(out[o][t]) - db(expected)) < 0.01);
                        }
                    }
                }
                if (std::string_view{leg} != "ac4-514-tones-256" &&
                    target == ac4::DownmixTarget::k5X) {
                    // Where the tops are coded channel by channel, each tone
                    // where the matrix sends it at the level it gives: its own
                    // channel at 0 dB, and each top tone in its side at -3 dB.
                    for (std::size_t t = 0; t < kTones514.size(); ++t) {
                        CAPTURE(t);
                        const S speaker = kTone514Speakers[t];
                        const bool left_top =
                            speaker == S::kTopFrontLeft || speaker == S::kTopBackLeft;
                        const bool right_top =
                            speaker == S::kTopFrontRight || speaker == S::kTopBackRight;
                        const S where = left_top    ? S::kLeftSurround
                                        : right_top ? S::kRightSurround
                                                    : speaker;
                        const double level = db(out[channel_of(rendered, where)][t]);
                        CHECK(std::abs(level - (left_top || right_top ? -3.0 : 0.0)) <
                              (speaker == S::kLfe ? 0.5 : 0.25));
                    }
                }
            }
        }
    }
}

TEST_CASE("an ASPX 5.1 stream rebuilds the high band of every channel but the LFE", "[ac4dec][pcm]") {
    // DEE's 5.1 music at 192 kbps: A-SPX from QMF subband 32 (12 kHz) in the
    // pairs (L, R) and (Ls, Rs) and in C (Part 1 Table 213). The source music
    // is 25 to 40 dB quieter from 12 to 16 kHz than from 7.5 to 11.25 kHz; its
    // LFE is low-passed at 120 Hz.
    const Decoded decoded = decode_all("ac4-51-music-192");
    REQUIRE(decoded.channels.size() == 6);
    for (std::size_t c = 0; c < 6; ++c) {
        CAPTURE(c);
        REQUIRE(decoded.channels[c].size() == decoded.frames * 2048);
        const std::vector<double> energy = subband_energy(decoded.channels[c]);
        const auto band = [&](std::size_t first, std::size_t last) {
            double sum = 0.0;
            for (std::size_t sb = first; sb < last; ++sb) {
                sum += energy[sb];
            }
            return 10.0 * std::log10(sum / static_cast<double>(last - first) + 1e-30);
        };
        const double waveform = band(20, 30);   // 7.5 to 11.25 kHz
        const double extension = band(33, 43);  // 12.4 to 16.1 kHz, A-SPX's
        const double lowest = band(0, 1);       // below 375 Hz
        CAPTURE(waveform, extension, lowest);
        if (c == 3) {
            CHECK(extension < lowest - 100.0);
        } else {
            CHECK(extension < waveform);
            CHECK(extension > waveform - 45.0);
        }
    }
}

TEST_CASE("decode takes the IMS streams' frame rates through the sample rate converter to 48 kHz",
          "[ac4dec][pcm][src]") {
    struct Leg {
        const char* name;
        int frame_rate_index;
        std::array<std::size_t, 5> counts;  // by phi_t, sequence_counter modulo 5
    };
    // Part 1 Table 83 and Part 2 Table 47: 48 000 samples a second, a frame's
    // share at a time.
    constexpr std::array<Leg, 3> kLegs{{
        {"ac4-ims-film-96-24", 1, {2000, 2000, 2000, 2000, 2000}},
        {"ac4-ims-music-128-25", 2, {1920, 1920, 1920, 1920, 1920}},
        {"ac4-ims-music-64-2997", 3, {1601, 1602, 1601, 1602, 1602}},
    }};
    for (const Leg& leg : kLegs) {
        CAPTURE(leg.name);
        const std::vector<std::byte> stream = read_stream(leg.name);
        const ac4::ScanResult scan = ac4::scan(stream);
        REQUIRE_FALSE(scan.frames.empty());
        const auto first = ac4::parse_raw_frame(scan.frames.front().raw_ac4_frame);
        REQUIRE(first.has_value());
        REQUIRE(first->toc.frame_rate_index == leg.frame_rate_index);
        ac4::Decoder decoder;
        std::vector<float> left;
        for (const ac4::SyncFrame& frame : scan.frames) {
            const auto decoded = decoder.decode(frame.raw_ac4_frame);
            INFO(decoder.refusal_reason());
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            const ac4::DecodedFrame& pcm = **decoded;
            CHECK(pcm.sample_rate_hz == 48000);
            REQUIRE(pcm.channels.size() == 2);
            CHECK(pcm.channels[0].size() ==
                  leg.counts[static_cast<std::size_t>(pcm.sequence_counter % 5)]);
            CHECK(pcm.channels[1].size() == pcm.channels[0].size());
            left.insert(left.end(), pcm.channels[0].begin(), pcm.channels[0].end());
        }
        // Music and film come out at a level, past the decoder's delay.
        double power = 0.0;
        for (std::size_t n = 48000; n < left.size(); ++n) {
            power += static_cast<double>(left[n]) * static_cast<double>(left[n]);
        }
        CHECK(10.0 * std::log10(power / static_cast<double>(left.size() - 48000)) > -50.0);
    }
}

TEST_CASE("decode reports a table of contents it cannot read", "[ac4dec][pcm]") {
    ac4::Decoder decoder;
    const std::array<std::byte, 3> garbage{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    const auto decoded = decoder.decode(garbage);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == ac4::DecodeError::kInvalidToc);
    CHECK_FALSE(decoder.refusal_reason().empty());
}
