// The decoder on streams of the channel elements DEE's streams do not reach
// (ac4dec_constructed.hpp): the 3.0 element, the 5.X element's
// coding_configs, 2ch_modes and matrices, and the 7.X element in its three
// channel modes, in the SIMPLE and ASPX codec modes; and the channel pair,
// 5.X and 7.X elements in the A-CPL modes. Each stream reads with the
// writer's trace, record for record; each channel's tone comes back on its
// own channel, and the channels A-CPL leaves silent stay so; A-SPX fills the
// channels of the aspx_data element that asks for it; companding changes the
// channel companding_control() names.
//
// The streams under tests/golden/ac4dec/constructed/ are the committed cases,
// byte for byte, and tests/golden/ac4dec/ holds
// tools/references/ac4_syntax.py's digests of them, which
// test_ac4dec_syntax.cpp holds the decoder to. With AC4DEC_WRITE_CONSTRUCTED
// set to a directory, this writes the committed cases there instead of
// comparing them, to commit after a change to the builder.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"
#include "ac4dec_constructed.hpp"
#include "dsp/qmf.hpp"

namespace {

namespace fs = std::filesystem;
using ac4::Speaker;
using ac4dec_test::BuiltStream;
using ac4dec_test::ElementCase;

constexpr int kFrames = 16;
// The first frames hold the decoder's delay and the transform's start.
constexpr std::size_t kSkippedFrames = 4;
constexpr double kAmplitude = 0.1;

struct Decoded {
    std::vector<Speaker> speakers;
    std::vector<std::vector<float>> channels;
};

// Every frame decoded, with the decoder's records the writer's: the same
// substreams, offsets, widths and values, in the same order.
Decoded decode_checked(const BuiltStream& stream,
                       ac4::DecodingMode decoding = ac4::DecodingMode::kFull) {
    std::vector<ac4::SyntaxRecord> read;
    const auto keep = [&read](const ac4::SyntaxRecord& record) { read.push_back(record); };
    ac4::DecoderConfig config;
    config.syntax = keep;
    config.decoding = decoding;
    ac4::Decoder decoder(config);
    Decoded out;
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        read.clear();
        const auto decoded = decoder.decode(stream.frames[f]);
        INFO("frame " << f << ": " << decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const std::vector<ac4::SyntaxRecord>& written = stream.traces[f];
        REQUIRE(read.size() == written.size());
        for (std::size_t i = 0; i < read.size(); ++i) {
            const bool same = read[i].substream == written[i].substream &&
                              read[i].bit_offset == written[i].bit_offset && read[i].bits == written[i].bits &&
                              read[i].value == written[i].value;
            if (!same) {
                CAPTURE(i, written[i].name, read[i].name, written[i].bit_offset, read[i].bit_offset,
                        written[i].value, read[i].value);
                REQUIRE(same);
            }
        }
        const ac4::DecodedFrame& pcm = **decoded;
        if (out.channels.empty()) {
            out.speakers = pcm.speakers;
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
    }
    return out;
}

// The amplitude of `samples`' component at `hz`, through a Hann window, whose
// sidelobes leave another tone 126 Hz away far below 60 dB.
double tone_amplitude(std::span<const float> samples, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const auto n = static_cast<double>(samples.size());
    std::complex<double> sum{};
    double weights = 0.0;
    for (std::size_t k = 0; k < samples.size(); ++k) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) / (n - 1.0));
        sum += window * static_cast<double>(samples[k]) * std::polar(1.0, -w * static_cast<double>(k));
        weights += window;
    }
    return 2.0 * std::abs(sum) / weights;
}

std::span<const float> steady(const Decoded& decoded, std::size_t c) {
    const std::size_t skip = kSkippedFrames * 2048;
    return std::span<const float>(decoded.channels[c]).subspan(skip, decoded.channels[c].size() - skip);
}

// Each channel's tone on that channel at -20 dBFS within 0.3 dB, and every
// other channel's tone at least 60 dB under it there; a channel with no tone
// (tone_hz 0, left silent by A-CPL) has every tone 60 dB under -20 dBFS.
void check_routing(const BuiltStream& stream, const Decoded& decoded) {
    REQUIRE(decoded.speakers == stream.speakers);
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        CAPTURE(c, ac4::describe(decoded.speakers[c]));
        double own = kAmplitude;
        if (stream.tone_hz[c] > 0.0) {
            own = tone_amplitude(steady(decoded, c), stream.tone_hz[c]);
            CHECK(std::abs(20.0 * std::log10(own / kAmplitude)) < 0.3);
        }
        for (std::size_t other = 0; other < decoded.channels.size(); ++other) {
            if (other != c && stream.tone_hz[other] > 0.0) {
                CAPTURE(other);
                CHECK(tone_amplitude(steady(decoded, c), stream.tone_hz[other]) < own * 1e-3);
            }
        }
    }
}

// Mean energy of QMF subbands [first, last) over the steady frames.
double band_energy(std::span<const float> samples, std::size_t first, std::size_t last) {
    ac4::detail::dsp::QmfAnalysis<double> analysis;
    const std::size_t slots = samples.size() / 64;
    std::vector<double> pcm(slots * 64);
    for (std::size_t n = 0; n < pcm.size(); ++n) {
        pcm[n] = static_cast<double>(samples[n]);
    }
    std::vector<std::complex<double>> q(pcm.size());
    analysis.process(pcm, q);
    double sum = 0.0;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        for (std::size_t sb = first; sb < last; ++sb) {
            sum += std::norm(q[ts * 64 + sb]);
        }
    }
    return sum / static_cast<double>(slots * (last - first)) + 1e-30;
}

std::string name_of(const ElementCase& c) {
    return c.name.empty() ? "ch_mode " + std::to_string(c.ch_mode) + " config " + std::to_string(c.coding_config)
                          : c.name;
}

// Core decoding of the immersive element (Part 2 clause 4.7): each tone on the
// core channel of its pair (Ls and Lb on Ls, Tfl and Tbl on Tsl, and alike),
// L, R, C and the LFE as they are and the others 3 dB down (Tables 24 and 45),
// and every other tone 60 dB under it there.
void check_core_routing(const BuiltStream& stream, const Decoded& decoded) {
    using S = Speaker;
    const auto core_of = [](Speaker s) {
        switch (s) {
            case S::kLeftBack:
                return S::kLeftSurround;
            case S::kRightBack:
                return S::kRightSurround;
            case S::kTopFrontLeft:
            case S::kTopBackLeft:
                return S::kTopSideLeft;
            case S::kTopFrontRight:
            case S::kTopBackRight:
                return S::kTopSideRight;
            default:
                return s;
        }
    };
    std::vector<S> core_speakers;
    for (const S s : stream.speakers) {
        if (std::ranges::find(core_speakers, core_of(s)) == core_speakers.end()) {
            core_speakers.push_back(core_of(s));
        }
    }
    REQUIRE(decoded.speakers == core_speakers);
    const double down = std::sqrt(0.5);
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        CAPTURE(c, ac4::describe(decoded.speakers[c]));
        for (std::size_t s = 0; s < stream.speakers.size(); ++s) {
            if (stream.tone_hz[s] <= 0.0) {
                continue;
            }
            CAPTURE(ac4::describe(stream.speakers[s]));
            const double level = tone_amplitude(steady(decoded, c), stream.tone_hz[s]);
            if (core_of(stream.speakers[s]) != decoded.speakers[c]) {
                CHECK(level < kAmplitude * 1e-3);
                continue;
            }
            const bool coupled = stream.speakers[s] != decoded.speakers[c] ||
                                 stream.speakers[s] == S::kLeftSurround ||
                                 stream.speakers[s] == S::kRightSurround;
            CHECK(std::abs(20.0 * std::log10(level / (kAmplitude * (coupled ? down : 1.0)))) < 0.3);
        }
    }
}

void check_case(const ElementCase& c) {
    INFO(name_of(c) << (c.aspx ? " ASPX" : " SIMPLE") << ", chel_matsel " << c.chel_matsel
                    << ", sap_mode " << c.sap_mode << ", 2ch_mode " << c.two_ch_mode
                    << ", stereo processing " << c.stereo_proc << ", b_use_sap_add_ch "
                    << c.use_sap_add_ch << ", A-CPL mode " << c.acpl << ", second " << c.acpl_second
                    << ", add_ch_base " << c.add_ch_base << ", immersive mode " << c.immersive
                    << ", a' alpha_q " << c.prediction_alpha_q << ", A-JCC core mode "
                    << c.ajcc_core_mode << " route " << c.ajcc_route);
    const BuiltStream stream = ac4dec_test::build_stream(c, kFrames);
    check_routing(stream, decode_checked(stream));
    if (c.immersive >= 0) {
        check_core_routing(stream, decode_checked(stream, ac4::DecodingMode::kCore));
    }
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("the 3.0 element's two coding_configs put each tone on its channel", "[ac4dec][constructed]") {
    for (const bool aspx : {false, true}) {
        for (const bool proc : {false, true}) {
            check_case({.ch_mode = 2, .aspx = aspx, .coding_config = 0, .sap_mode = 2, .stereo_proc = proc});
        }
        for (int matsel = 0; matsel < 12; ++matsel) {
            check_case({.ch_mode = 2, .aspx = aspx, .coding_config = 1, .chel_matsel = matsel, .sap_mode = 2});
        }
    }
}

TEST_CASE("Table 180's coding_configs and 2ch_modes put each 5.X tone on its channel", "[ac4dec][constructed]") {
    for (const int ch_mode : {3, 4}) {
        for (const bool two : {false, true}) {
            for (const bool proc : {false, true}) {
                check_case({.ch_mode = ch_mode, .coding_config = 0, .two_ch_mode = two, .sap_mode = 2,
                            .stereo_proc = proc});
            }
        }
        check_case({.ch_mode = ch_mode, .coding_config = 2, .sap_mode = 2});
        check_case({.ch_mode = ch_mode, .aspx = true, .coding_config = 2, .sap_mode = 0});
    }
    for (int matsel = 0; matsel < 12; ++matsel) {
        check_case({.ch_mode = 4, .coding_config = 1, .chel_matsel = matsel, .sap_mode = 2});
        check_case({.ch_mode = 4, .aspx = matsel % 2 == 1, .coding_config = 3, .chel_matsel = matsel, .sap_mode = 2});
    }
}

TEST_CASE("Tables 182 and 183 put each 7.X tone on its channel in the three modes", "[ac4dec][constructed]") {
    for (int ch_mode = 5; ch_mode <= 10; ++ch_mode) {
        for (int config = 0; config < 4; ++config) {
            for (const bool sap : {false, true}) {
                check_case({.ch_mode = ch_mode, .aspx = config % 2 == 1, .coding_config = config,
                            .two_ch_mode = ch_mode % 2 == 0, .chel_matsel = (ch_mode + config) % 12, .sap_mode = 2,
                            .use_sap_add_ch = sap});
            }
        }
    }
}

TEST_CASE("the channel pair's A-CPL modes put each tone on its channel", "[ac4dec][constructed][acpl]") {
    // ASPX_ACPL_1: mid and side below acpl_qmf_band, with and without stereo
    // processing; ASPX_ACPL_2: one track, sent to L or to R.
    for (const bool proc : {false, true}) {
        for (const int sap : {0, 2}) {
            check_case({.ch_mode = 1, .sap_mode = sap, .stereo_proc = proc, .acpl = 2});
        }
    }
    // b_dual_maxsfb with the side in fewer bands than the mid, 16 of 22 (to
    // 984 Hz, above both tones): the two tracks' lines do not line up until
    // the decoder lays them out alike.
    check_case({.ch_mode = 1, .sap_mode = 0, .stereo_proc = true, .acpl = 2, .side_bands = 16});
    for (const bool second : {false, true}) {
        for (int id = 0; id < 4; ++id) {
            check_case({.ch_mode = 1, .acpl = 3, .acpl_second = second, .acpl_bands_id = id, .acpl_quant = id % 2});
        }
    }
}

TEST_CASE("the 5.X element's A-CPL modes put each tone on its channel", "[ac4dec][constructed][acpl]") {
    for (const int ch_mode : {3, 4}) {
        for (int config = 0; config < 2; ++config) {
            for (const int sap : {0, 2}) {
                check_case({.ch_mode = ch_mode, .coding_config = config, .chel_matsel = 5 + config, .sap_mode = 2,
                            .sap_add_mode = sap, .acpl = 2, .acpl_bands_id = config});
            }
            for (const bool second : {false, true}) {
                check_case({.ch_mode = ch_mode, .coding_config = config, .chel_matsel = 9, .sap_mode = 2, .acpl = 3,
                            .acpl_second = second, .acpl_quant = config});
            }
        }
        for (const bool second : {false, true}) {
            for (const bool proc : {false, true}) {
                check_case({.ch_mode = ch_mode, .sap_mode = 2, .stereo_proc = proc, .acpl = 4, .acpl_second = second,
                            .acpl_quant = proc ? 1 : 0});
            }
        }
    }
}

TEST_CASE("the 7.X element's A-CPL modes put each tone on its channel by Table 202", "[ac4dec][constructed][acpl]") {
    for (int ch_mode = 5; ch_mode <= 10; ++ch_mode) {
        for (int config = 0; config < 4; ++config) {
            for (const bool base : {false, true}) {
                if (base && ch_mode <= 6) {
                    continue;  // 3/4/0 sends no add_ch_base
                }
                ElementCase common;
                common.ch_mode = ch_mode;
                common.coding_config = config;
                common.two_ch_mode = ch_mode % 2 == 0;
                common.chel_matsel = (ch_mode + config) % 12;
                common.sap_mode = 2;
                common.sap_add_mode = config % 2 == 0 ? 0 : 2;
                common.add_ch_base = base;
                common.acpl_bands_id = config;
                ElementCase residual = common;
                residual.acpl = 2;
                check_case(residual);
                for (const bool second : {false, true}) {
                    ElementCase full = common;
                    full.acpl = 3;
                    full.acpl_second = second;
                    check_case(full);
                }
            }
        }
    }
}

TEST_CASE("Part 2 Table 19, step 4 and Table 20 put each 7.X.4 tone on its channel, full and core",
          "[ac4dec][constructed][immersive]") {
    // SCPL, ASPX_SCPL and ASPX_ACPL_1, which code all eleven signals: every
    // core_5ch_grouping and 2ch_mode, step 4 at identity, M/S or absent, and
    // Table 20 predicting at 0.5 or not at all.
    for (const int mode : {0, 1, 2}) {
        for (int grouping = 0; grouping < 4; ++grouping) {
            for (const bool sap : {false, true}) {
                ElementCase c;
                c.ch_mode = grouping % 2 == 0 ? 12 : 11;
                c.immersive = mode;
                c.coding_config = grouping;
                c.two_ch_mode = (grouping + mode) % 2 == 1;
                c.chel_matsel = (4 * mode + grouping) % 12;
                c.sap_mode = 2;
                c.stereo_proc = mode != 1;
                c.use_sap_add_ch = sap;
                c.sap_add_mode = grouping % 2 == 0 ? 2 : 0;
                c.prediction_alpha_q = sap ? 5 : 0;
                c.acpl_bands_id = grouping;
                c.loud_unit = mode == 1 ? grouping : -1;
                check_case(c);
            }
        }
    }
}

TEST_CASE("the immersive element's ASPX_ACPL_2 makes each coupled pair by A-CPL",
          "[ac4dec][constructed][immersive]") {
    for (const bool second : {false, true}) {
        for (const bool sap : {false, true}) {
            ElementCase c;
            c.ch_mode = second ? 11 : 12;
            c.immersive = 3;
            c.coding_config = sap ? 3 : 0;
            c.chel_matsel = 2;
            c.sap_mode = 2;
            c.use_sap_add_ch = sap;
            c.acpl_second = second;
            c.acpl_quant = second ? 1 : 0;
            c.acpl_bands_id = sap ? 1 : 3;
            check_case(c);
        }
    }
}

TEST_CASE("A-JCC makes the 7.X.4 channels of its five, full and core, by each core mode",
          "[ac4dec][constructed][immersive][ajcc]") {
    // Each route sends a module's A'' and D'' whole to two of its five
    // outputs; the other three stay silent.
    for (const int core_mode : {0, 1}) {
        for (int route = 0; route < 3; ++route) {
            ElementCase c;
            c.ch_mode = route == 1 ? 11 : 12;
            c.immersive = 4;
            c.coding_config = (core_mode + route) % 4;
            c.two_ch_mode = route == 2;
            c.chel_matsel = 6 + route;
            c.sap_mode = 2;
            c.ajcc_core_mode = core_mode;
            c.ajcc_route = route;
            c.acpl_quant = route % 2;
            c.acpl_bands_id = route;
            check_case(c);
        }
    }
}

TEST_CASE("A-SPX fills the immersive element's channels by Part 2 Table 8, full and core",
          "[ac4dec][constructed][immersive]") {
    // The loud aspx_data element's channels, and no other. A-CPL at alpha 1
    // and A-JCC's first route leave each channel A-SPX made where it was, and
    // what they make of it silent. Core decoding in ASPX_SCPL takes the first
    // channel of a coupled pair alone (Table 8's square brackets): the
    // second, Lb, Rb, Tbl or Tbr, core decoding does not have.
    using S = Speaker;
    const auto core_of = [](S s) {
        switch (s) {
            case S::kTopFrontLeft:
                return S::kTopSideLeft;
            case S::kTopFrontRight:
                return S::kTopSideRight;
            default:
                return s;
        }
    };
    for (const int mode : {1, 3, 4}) {
        const auto elements = ac4dec_test::aspx_elements(12, mode);
        for (std::size_t loud = 0; loud < elements.size(); ++loud) {
            CAPTURE(mode, loud);
            ElementCase c;
            c.ch_mode = 12;
            c.immersive = mode;
            c.sap_mode = 2;
            c.loud_unit = static_cast<int>(loud);
            const BuiltStream stream = ac4dec_test::build_stream(c, kFrames);
            for (const ac4::DecodingMode decoding :
                 {ac4::DecodingMode::kFull, ac4::DecodingMode::kCore}) {
                const bool core = decoding == ac4::DecodingMode::kCore;
                CAPTURE(core);
                const Decoded decoded = decode_checked(stream, decoding);
                std::vector<S> filled;
                for (const S s : elements[loud]) {
                    const bool second = s == S::kLeftBack || s == S::kRightBack ||
                                        s == S::kTopBackLeft || s == S::kTopBackRight;
                    if (!core || !second) {
                        filled.push_back(core ? core_of(s) : s);
                    }
                }
                double quietest_loud = 1e300;
                double loudest_other = 0.0;
                std::string levels;
                for (std::size_t ch = 0; ch < decoded.channels.size(); ++ch) {
                    const double high = band_energy(steady(decoded, ch), 32, 48);
                    levels += std::string{ac4::describe(decoded.speakers[ch])} + " " +
                              std::to_string(10.0 * std::log10(high)) + "; ";
                    if (std::ranges::find(filled, decoded.speakers[ch]) != filled.end()) {
                        quietest_loud = std::min(quietest_loud, high);
                    } else {
                        loudest_other = std::max(loudest_other, high);
                    }
                }
                CAPTURE(levels);
                CAPTURE(10.0 * std::log10(quietest_loud), 10.0 * std::log10(loudest_other + 1e-30));
                CHECK(quietest_loud > 1e3 * loudest_other);
            }
        }
    }
}

TEST_CASE("A-CPL's decorrelated part cancels in the sum of its two outputs", "[ac4dec][constructed][acpl]") {
    // The channel pair in ASPX_ACPL_2 with alpha 0 and beta_q 4 (1.4 at ibeta
    // 0, Table 204): L = x0 + 0.7 y and R = x0 - 0.7 y, with y the ducked
    // decorrelator output of 2 x0. A steady tone passes the all-pass filters
    // and the ducker whole, so (L + R) / 2 is the coded tone and (L - R) / 2
    // the tone 1.4 times over, phase-shifted.
    const BuiltStream stream = ac4dec_test::build_stream({.ch_mode = 1, .acpl = 3, .acpl_beta_q = 4}, kFrames);
    const Decoded decoded = decode_checked(stream);
    const std::span<const float> l = steady(decoded, 0);
    const std::span<const float> r = steady(decoded, 1);
    std::vector<float> sum(l.size());
    std::vector<float> difference(l.size());
    for (std::size_t n = 0; n < l.size(); ++n) {
        sum[n] = 0.5F * (l[n] + r[n]);
        difference[n] = 0.5F * (l[n] - r[n]);
    }
    const double hz = stream.tone_hz[0];
    CHECK(std::abs(20.0 * std::log10(tone_amplitude(sum, hz) / kAmplitude)) < 0.1);
    CHECK(std::abs(20.0 * std::log10(tone_amplitude(difference, hz) / (1.4 * kAmplitude))) < 0.3);
}

TEST_CASE("A-SPX fills the channels of the aspx_data element Table 213 gives them", "[ac4dec][constructed]") {
    // Crossover at QMF subband 28 (10.5 kHz): the tones are below it, and
    // only the loud element's channels have anything from 12 to 18 kHz.
    for (const int ch_mode : {2, 4, 5, 7, 9}) {
        const auto elements = ac4dec_test::aspx_elements(ch_mode);
        for (std::size_t loud = 0; loud < elements.size(); ++loud) {
            CAPTURE(ch_mode, loud);
            const BuiltStream stream = ac4dec_test::build_stream(
                {.ch_mode = ch_mode, .aspx = true, .coding_config = 0, .sap_mode = 2,
                 .loud_unit = static_cast<int>(loud)},
                kFrames);
            const Decoded decoded = decode_checked(stream);
            double quietest_loud = 1e300;
            double loudest_other = 0.0;
            for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
                const double high = band_energy(steady(decoded, c), 32, 48);
                const bool carried = std::ranges::find(elements[loud], decoded.speakers[c]) != elements[loud].end();
                if (carried) {
                    quietest_loud = std::min(quietest_loud, high);
                } else {
                    loudest_other = std::max(loudest_other, high);
                }
            }
            CAPTURE(10.0 * std::log10(quietest_loud), 10.0 * std::log10(loudest_other + 1e-30));
            CHECK(quietest_loud > 1e3 * loudest_other);
        }
    }
}

TEST_CASE("companding changes only the channel companding_control() names", "[ac4dec][constructed]") {
    // Table 212: L, R and C for 3.0; L, R, C, Ls and Rs for 5.X. With
    // b_compand_on, the expander scales that channel's low band by its level
    // over full scale to the 0.54th power times 2^(1/0.65), which moves each
    // tone here by 0.9 dB or more, by where it falls among the subbands.
    struct Mode {
        int ch_mode;
        std::vector<Speaker> order;
    };
    const std::vector<Mode> modes = {
        {2, {Speaker::kLeft, Speaker::kRight, Speaker::kCentre}},
        {4, {Speaker::kLeft, Speaker::kRight, Speaker::kCentre, Speaker::kLeftSurround, Speaker::kRightSurround}},
    };
    for (const Mode& mode : modes) {
        const BuiltStream plain_stream =
            ac4dec_test::build_stream({.ch_mode = mode.ch_mode, .aspx = true, .coding_config = 0}, kFrames);
        const Decoded plain = decode_checked(plain_stream);
        for (std::size_t k = 0; k < mode.order.size(); ++k) {
            CAPTURE(mode.ch_mode, k);
            const BuiltStream stream = ac4dec_test::build_stream(
                {.ch_mode = mode.ch_mode, .aspx = true, .coding_config = 0, .companded = static_cast<int>(k)},
                kFrames);
            const Decoded decoded = decode_checked(stream);
            for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
                CAPTURE(ac4::describe(decoded.speakers[c]));
                const double before = tone_amplitude(steady(plain, c), stream.tone_hz[c]);
                const double after = tone_amplitude(steady(decoded, c), stream.tone_hz[c]);
                const double change = std::abs(20.0 * std::log10(after / before));
                if (decoded.speakers[c] == mode.order[k]) {
                    CHECK(change > 0.5);
                } else {
                    CHECK(change < 0.01);
                }
            }
        }
    }
}

TEST_CASE("the committed constructed streams are the builder's", "[ac4dec][constructed]") {
    const fs::path committed = fs::path{AC4DEC_GOLDEN_DIR} / "constructed";
    const char* write_to = std::getenv("AC4DEC_WRITE_CONSTRUCTED");
    for (const ElementCase& c : ac4dec_test::committed_cases()) {
        CAPTURE(c.name);
        const BuiltStream stream = ac4dec_test::build_stream(c, ac4dec_test::kCommittedFrames);
        (void)decode_checked(stream);
        const std::vector<std::byte> bytes = ac4dec_test::sync_framed(stream);
        if (write_to != nullptr) {
            fs::create_directories(write_to);
            std::ofstream out(fs::path{write_to} / (c.name + ".ac4"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(out.good());
            continue;
        }
        CHECK(read_file(committed / (c.name + ".ac4")) == bytes);
    }
}
