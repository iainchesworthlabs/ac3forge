// The decoder on streams of the channel elements DEE's streams do not reach
// (ac4dec_constructed.hpp): the 3.0 element, the 5.X element's
// coding_configs, 2ch_modes and matrices, and the 7.X element in its three
// channel modes, in the SIMPLE and ASPX codec modes. Each stream reads with
// the writer's trace, record for record; each channel's tone comes back on
// its own channel; A-SPX fills the channels of the aspx_data element that
// asks for it; companding changes the channel companding_control() names.
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
Decoded decode_checked(const BuiltStream& stream) {
    std::vector<ac4::SyntaxRecord> read;
    const auto keep = [&read](const ac4::SyntaxRecord& record) { read.push_back(record); };
    ac4::DecoderConfig config;
    config.syntax = keep;
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
// other channel's tone at least 60 dB under it there.
void check_routing(const BuiltStream& stream, const Decoded& decoded) {
    REQUIRE(decoded.speakers == stream.speakers);
    for (std::size_t c = 0; c < decoded.channels.size(); ++c) {
        CAPTURE(c, ac4::describe(decoded.speakers[c]));
        const double own = tone_amplitude(steady(decoded, c), stream.tone_hz[c]);
        CHECK(std::abs(20.0 * std::log10(own / kAmplitude)) < 0.3);
        for (std::size_t other = 0; other < decoded.channels.size(); ++other) {
            if (other != c) {
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

void check_case(const ElementCase& c) {
    INFO(name_of(c) << (c.aspx ? " ASPX" : " SIMPLE") << ", chel_matsel " << c.chel_matsel << ", sap_mode "
                    << c.sap_mode << ", 2ch_mode " << c.two_ch_mode << ", stereo processing " << c.stereo_proc
                    << ", b_use_sap_add_ch " << c.use_sap_add_ch);
    const BuiltStream stream = ac4dec_test::build_stream(c, kFrames);
    check_routing(stream, decode_checked(stream));
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
