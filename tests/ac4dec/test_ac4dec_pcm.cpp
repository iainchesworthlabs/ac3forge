// ac4::Decoder::decode() and the reconstruction behind it (src/ac4dec/src/pcm):
// the noise fill's random number generator against the text's own closed
// form, and the committed DEE streams decoded to PCM - each channel's tone on
// its own channel in stereo and 5.1, the LFE's included, an ASPX stream's high
// band rebuilt, and the IMS streams' frame rates through the sample rate
// converter.

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

Decoded decode_all(const std::string& leg) {
    const std::vector<std::byte> stream = read_stream(leg);
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    ac4::Decoder decoder;
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
