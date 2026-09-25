// The downmix (src/ac4dec/src/pcm/downmix.hpp, ETSI TS 103 190-1 V1.4.1 clause
// 6.2.17): Tables 149 and 149a's gains, every matrix of the cascade - 7.X to
// 5.X by Table 219, 5.X and 3.0 to Lo/Ro, Lt/Rt and its Pro Logic II form by
// Tables 217 and 218, with the LFE and the loudness corrections, and to mono -
// against its formula with the stream's gains; the gains persisting between
// the frames that send them; and through ac4::Decoder, DEE's 5.1 tones, one
// per channel, measured in each channel of the downmix to 0.01 dB.

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "pcm/downmix.hpp"

namespace {

namespace detail = ac4::detail;
using S = ac4::Speaker;
using Row = std::vector<double>;

double db(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

constexpr std::array<S, 6> kFiveOne = {S::kLeft, S::kRight,        S::kCentre,
                                       S::kLfe,  S::kLeftSurround, S::kRightSurround};

// A stream's stereo downmix coefficients.
detail::StereoDmxCoeff coefficients(int loro_c, int loro_s, int ltrt_c, int ltrt_s,
                                    std::optional<int> lfe, int preferred) {
    detail::StereoDmxCoeff c;
    c.loro_centre_mixgain = loro_c;
    c.loro_surround_mixgain = loro_s;
    c.b_ltrt_mixinfo = true;
    c.ltrt_centre_mixgain = ltrt_c;
    c.ltrt_surround_mixgain = ltrt_s;
    c.lfe_mixgain = lfe;
    c.preferred_dmx_method = preferred;
    return c;
}

// The matrix a stage takes after one frame of `values`.
std::vector<Row> matrix_for(std::span<const S> speakers, bool add_ch_base,
                            ac4::DownmixTarget target, const detail::DownmixValues& values,
                            bool mix_lfe = true) {
    detail::DownmixStage stage;
    stage.configure(speakers, add_ch_base, target, mix_lfe);
    std::vector<std::vector<std::complex<double>>> channels(speakers.size(),
                                                            std::vector<std::complex<double>>(64));
    std::vector<std::vector<std::complex<double>>*> in;
    for (auto& channel : channels) {
        in.push_back(&channel);
    }
    std::vector<std::vector<std::complex<double>>> out;
    stage.process(values, in, out);
    return stage.matrix();
}

void check_matrix(const std::vector<Row>& got, const std::vector<Row>& expected) {
    REQUIRE(got.size() == expected.size());
    for (std::size_t o = 0; o < got.size(); ++o) {
        REQUIRE(got[o].size() == expected[o].size());
        for (std::size_t c = 0; c < got[o].size(); ++c) {
            CAPTURE(o, c, got[o][c], expected[o][c]);
            CHECK(std::abs(got[o][c] - expected[o][c]) < 1e-12);
        }
    }
}

std::vector<std::byte> read_stream(const std::string& leg) {
    const std::filesystem::path path =
        std::filesystem::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

struct Decoded {
    std::vector<S> speakers;
    std::vector<std::vector<float>> channels;
};

Decoded decode_all(std::span<const std::byte> stream, ac4::DownmixTarget target) {
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    ac4::DecoderConfig config;
    config.output.downmix = target;
    ac4::Decoder decoder(config);
    Decoded out;
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        out.speakers = pcm.speakers;
        out.channels.resize(pcm.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(),
                                   pcm.channels[c].end());
        }
    }
    return out;
}

// The amplitude of `hz` in x's middle, by a projection onto it.
double amplitude(const std::vector<float>& x, double hz) {
    const std::size_t from = 24000;
    const std::size_t to = x.size() - 24000;
    std::complex<double> sum{};
    for (std::size_t n = from; n < to; ++n) {
        sum += static_cast<double>(x[n]) *
               std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0);
    }
    return 2.0 * std::abs(sum) / static_cast<double>(to - from);
}

}  // namespace

TEST_CASE("Tables 149 and 149a give the downmix's mix gains", "[ac4dec][downmix]") {
    constexpr std::array<double, 7> kCentre = {3.0, 1.5, 0.0, -1.5, -3.0, -4.5, -6.0};
    for (int code = 0; code < 7; ++code) {
        CHECK(std::abs(detail::centre_mix_gain(code) -
                       db(kCentre[static_cast<std::size_t>(code)])) < 1e-12);
    }
    CHECK(detail::centre_mix_gain(7) == 0.0);
    constexpr std::array<double, 5> kSurround = {0.0, -1.5, -3.0, -4.5, -6.0};
    for (int code = 2; code < 7; ++code) {
        CHECK(std::abs(detail::surround_mix_gain(code) -
                       db(kSurround[static_cast<std::size_t>(code - 2)])) < 1e-12);
    }
    CHECK(detail::surround_mix_gain(7) == 0.0);
    // The reserved surround codes, and no code at all, are -3 dB.
    CHECK(std::abs(detail::surround_mix_gain(0) - db(-3.0)) < 1e-12);
    CHECK(std::abs(detail::surround_mix_gain(1) - db(-3.0)) < 1e-12);
    CHECK(std::abs(detail::centre_mix_gain(-1) - db(-3.0)) < 1e-12);
}

TEST_CASE("5.1's downmixes are Table 218's with the stream's gains, LFE and loudness corrections",
          "[ac4dec][downmix]") {
    // Lo/Ro: C at 0 dB and the surrounds at -6; Lt/Rt: C at -6 and the
    // surrounds at -4.5; the LFE at 5.5 - 10 = -4.5 dB; Lo/Ro's loudness
    // correction (15 - 21) / 2 = -3 dB2 and Lt/Rt's (15 - 13) / 2 = 1 dB2.
    detail::DownmixValues values;
    values.coeff = coefficients(2, 6, 6, 5, 10, 1);
    values.loro_loud_corr = 21;
    values.ltrt_loud_corr = 13;
    const double lfe = db(-4.5);
    const double loro = std::exp2(-3.0 / 6.0);
    const double ltrt = std::exp2(1.0 / 6.0);
    const auto scaled = [](Row r, double k) {
        for (double& v : r) {
            v *= k;
        }
        return r;
    };
    // Columns L R C LFE Ls Rs.
    const Row lo = scaled({1, 0, db(0), lfe, db(-6), 0}, loro);
    const Row ro = scaled({0, 1, db(0), lfe, 0, db(-6)}, loro);
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kLoRo, values), {lo, ro});
    // The stream prefers Lo/Ro, so stereo is Lo/Ro, and mono their sum.
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kStereo, values), {lo, ro});
    Row sum(6);
    for (std::size_t c = 0; c < 6; ++c) {
        sum[c] = lo[c] + ro[c];
    }
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kMono, values), {sum});
    // Lt/Rt: the surrounds' sum out of phase in Lt.
    const double s = db(-4.5);
    check_matrix(
        matrix_for(kFiveOne, false, ac4::DownmixTarget::kLtRt, values),
        {scaled({1, 0, db(-6), lfe, -s, -s}, ltrt), scaled({0, 1, db(-6), lfe, s, s}, ltrt)});
    // Where the stream prefers the Pro Logic II form: +1.8 dB near, -3.2 far.
    values.coeff->preferred_dmx_method = 3;
    const double near = s * db(1.8);
    const double far = s * db(-3.2);
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kLtRt, values),
                 {scaled({1, 0, db(-6), lfe, -near, -far}, ltrt),
                  scaled({0, 1, db(-6), lfe, far, near}, ltrt)});
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kStereo, values),
                 {scaled({1, 0, db(-6), lfe, -near, -far}, ltrt),
                  scaled({0, 1, db(-6), lfe, far, near}, ltrt)});
    // Without the LFE, and without b_ltrt_mixinfo, which gives Lt/Rt the Lo/Ro
    // gains.
    values.coeff->preferred_dmx_method = 2;
    values.coeff->b_ltrt_mixinfo = false;
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kLtRt, values, false),
                 {scaled({1, 0, db(0), 0, -db(-6), -db(-6)}, ltrt),
                  scaled({0, 1, db(0), 0, db(-6), db(-6)}, ltrt)});
    // Nothing sent: -3 dB mix gains, no LFE, no correction.
    check_matrix(matrix_for(kFiveOne, false, ac4::DownmixTarget::kLoRo, {}),
                 {{1, 0, db(-3), 0, db(-3), 0}, {0, 1, db(-3), 0, 0, db(-3)}});
}

TEST_CASE("7.X folds to 5.X by Table 219 for each additional pair and add_ch_base",
          "[ac4dec][downmix]") {
    constexpr double k = 0.707;
    detail::DownmixValues values;
    // Columns L R C LFE Ls Rs and the pair.
    const std::array<S, 8> back = {S::kLeft,     S::kRight,        S::kCentre,
                                   S::kLfe,      S::kLeftSurround, S::kRightSurround,
                                   S::kLeftBack, S::kRightBack};
    check_matrix(matrix_for(back, false, ac4::DownmixTarget::k5X, values),
                 {{1, 0, 0, 0, 0, 0, 0, 0},
                  {0, 1, 0, 0, 0, 0, 0, 0},
                  {0, 0, 1, 0, 0, 0, 0, 0},
                  {0, 0, 0, 1, 0, 0, 0, 0},
                  {0, 0, 0, 0, k, 0, k, 0},
                  {0, 0, 0, 0, 0, k, 0, k}});
    for (const auto& [left, right] :
         {std::pair{S::kLeftWide, S::kRightWide}, std::pair{S::kTopFrontLeft, S::kTopFrontRight}}) {
        const std::array<S, 8> seven = {S::kLeft,         S::kRight,         S::kCentre, S::kLfe,
                                        S::kLeftSurround, S::kRightSurround, left,       right};
        // add_ch_base 0: the pair into L and R; 1: into the surrounds.
        check_matrix(matrix_for(seven, false, ac4::DownmixTarget::k5X, values),
                     {{1, 0, 0, 0, 0, 0, k, 0},
                      {0, 1, 0, 0, 0, 0, 0, k},
                      {0, 0, 1, 0, 0, 0, 0, 0},
                      {0, 0, 0, 1, 0, 0, 0, 0},
                      {0, 0, 0, 0, 1, 0, 0, 0},
                      {0, 0, 0, 0, 0, 1, 0, 0}});
        check_matrix(matrix_for(seven, true, ac4::DownmixTarget::k5X, values),
                     {{1, 0, 0, 0, 0, 0, 0, 0},
                      {0, 1, 0, 0, 0, 0, 0, 0},
                      {0, 0, 1, 0, 0, 0, 0, 0},
                      {0, 0, 0, 1, 0, 0, 0, 0},
                      {0, 0, 0, 0, k, 0, k, 0},
                      {0, 0, 0, 0, 0, k, 0, k}});
    }
    // And on to Lo/Ro through 5.X: the back pair at 0.707 x -3 dB.
    const double s = db(-3.0);
    check_matrix(matrix_for(back, false, ac4::DownmixTarget::kLoRo, values),
                 {{1, 0, s, 0, k * s, 0, k * s, 0}, {0, 1, s, 0, 0, k * s, 0, k * s}});
    // 5.X is left as it is by a 5.X target.
    CHECK(matrix_for(kFiveOne, false, ac4::DownmixTarget::k5X, values).size() == 6);
}

TEST_CASE("3.0, stereo and mono take Table 217, the sum and the 0.707 upmix", "[ac4dec][downmix]") {
    detail::DownmixValues values;
    values.coeff = coefficients(5, 4, 3, 4, std::nullopt, 1);
    const std::array<S, 3> three = {S::kLeft, S::kRight, S::kCentre};
    check_matrix(matrix_for(three, false, ac4::DownmixTarget::kLoRo, values),
                 {{1, 0, db(-4.5)}, {0, 1, db(-4.5)}});
    check_matrix(matrix_for(three, false, ac4::DownmixTarget::kLtRt, values),
                 {{1, 0, db(-1.5)}, {0, 1, db(-1.5)}});
    const std::array<S, 2> stereo = {S::kLeft, S::kRight};
    check_matrix(matrix_for(stereo, false, ac4::DownmixTarget::kMono, values), {{1, 1}});
    const std::array<S, 1> mono = {S::kCentre};
    check_matrix(matrix_for(mono, false, ac4::DownmixTarget::kStereo, values), {{0.707}, {0.707}});
}

TEST_CASE("the downmix's gains hold from the frame that sends them until another does",
          "[ac4dec][downmix]") {
    detail::DownmixStage stage;
    stage.configure(kFiveOne, false, ac4::DownmixTarget::kLoRo, true);
    std::vector<std::vector<std::complex<double>>> channels(
        6, std::vector<std::complex<double>>(64, {1.0, 0.0}));
    std::vector<std::vector<std::complex<double>>*> in;
    for (auto& channel : channels) {
        in.push_back(&channel);
    }
    std::vector<std::vector<std::complex<double>>> out;
    detail::DownmixValues sent;
    sent.coeff = coefficients(6, 7, 6, 7, std::nullopt, 1);  // C at -6 dB, no surrounds
    stage.process(sent, in, out);
    const double first = out[0][0].real();
    CHECK(std::abs(first - (1.0 + db(-6.0))) < 1e-12);
    // A frame that sends nothing keeps them.
    stage.process({}, in, out);
    CHECK(out[0][0].real() == first);
    // A reset goes back to -3 dB.
    stage.reset();
    stage.process({}, in, out);
    CHECK(std::abs(out[0][0].real() - (1.0 + 2.0 * db(-3.0))) < 1e-12);
}

TEST_CASE("DEE's 5.1 tones come out of each downmix at the stream's gains, to 0.01 dB",
          "[ac4dec][downmix]") {
    // L R C LFE Ls Rs at 331, 457, 613, 47, 787 and 953 Hz; DEE's stream sends
    // -3 dB centre and surround gains and no LFE mix gain.
    constexpr std::array<double, 6> kHz = {331.0, 457.0, 613.0, 47.0, 787.0, 953.0};
    const std::vector<std::byte> stream = read_stream("ac4-51-tones-384");
    const Decoded coded = decode_all(stream, ac4::DownmixTarget::kAsCoded);
    REQUIRE(coded.speakers == std::vector<S>(kFiveOne.begin(), kFiveOne.end()));
    std::array<double, 6> own{};
    for (std::size_t c = 0; c < 6; ++c) {
        own[c] = amplitude(coded.channels[c], kHz[c]);
    }
    const double m3 = db(-3.0);
    struct Expected {
        ac4::DownmixTarget target;
        std::vector<Row> matrix;
    };
    const std::array<Expected, 3> targets = {{
        {ac4::DownmixTarget::kLoRo, {{1, 0, m3, 0, m3, 0}, {0, 1, m3, 0, 0, m3}}},
        {ac4::DownmixTarget::kLtRt, {{1, 0, m3, 0, -m3, -m3}, {0, 1, m3, 0, m3, m3}}},
        {ac4::DownmixTarget::kMono, {{1, 1, 2 * m3, 0, m3, m3}}},
    }};
    for (const Expected& expected : targets) {
        CAPTURE(ac4::describe(expected.target));
        const Decoded mixed = decode_all(stream, expected.target);
        REQUIRE(mixed.channels.size() == expected.matrix.size());
        for (std::size_t o = 0; o < mixed.channels.size(); ++o) {
            for (std::size_t c = 0; c < 6; ++c) {
                CAPTURE(o, c);
                const double got = amplitude(mixed.channels[o], kHz[c]) / own[c];
                const double want = std::abs(expected.matrix[o][c]);
                if (want == 0.0) {
                    CHECK(got < 1e-3);
                } else {
                    CHECK(std::abs(20.0 * std::log10(got / want)) < 0.01);
                }
            }
        }
    }
}
