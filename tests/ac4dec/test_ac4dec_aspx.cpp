// The decoder's QMF-domain tools (src/ac4dec/src/pcm/aspx.cpp and
// companding.cpp) on hand-built A-SPX data, for the paths DEE's streams do not
// take: frequency and time interleaved waveform coding, a balanced pair, the
// tone generator's phase, the noise generator's index across intervals, an
// interval that runs past its frame, and companding's gains (ETSI TS 103
// 190-1 V1.4.1 clauses 5.7.5 and 5.7.6).
//
// Every case uses the configuration of DEE's 128 kbps stereo streams:
// aspx_master_freq_scale 1, aspx_start_freq 6, aspx_stop_freq 1,
// aspx_noise_sbg 3, crossover offset 0. A-SPX then recreates subbands 36 to
// 55 in eight high resolution groups (36 38 40 42 44 47 50 53 56), four low
// resolution ones and two noise groups (36 44 56).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec_units.hpp"
#include "iclforge/arithmetic/scalar_math.hpp"
#include "pcm/aspx.hpp"
#include "pcm/companding.hpp"
#include "iclforge/ac4core/tables/qmf_tables.hpp"

namespace {

using iclforge::ac4::detail::AspxChannel;
using iclforge::ac4::detail::AspxChannelIo;
using iclforge::ac4::detail::AspxChannelState;
using iclforge::ac4::detail::AspxConfig;
using iclforge::ac4::detail::AspxFrame;
using iclforge::ac4::detail::QmfValue;
using iclforge::ac4::detail::Real;
namespace aspx = iclforge::ac4::detail::aspx;
using ac4dec_units::qmf;
using ac4dec_units::qmf_real;
using ac4dec_units::qmf_units;

constexpr int kSlots = 32;                                    // num_qmf_timeslots at 2 048
constexpr int kExtSlots = aspx::kTsOffsetHfadj + 6 + kSlots;  // Q_low_ext
constexpr int kHighGroups = 8;
constexpr int kNoiseGroups = 2;

// A relative-tolerance scale, in place of a fixed 1e-9: this file's own hand
// assembly of a level times a table entry holds to a handful of ulps of Real
// (possibly float, or Fixed32's raw unit), not of double.
const double kRelativeTolerance = 1e4 * ac4dec_units::relative_epsilon();

AspxConfig dee_128k_config() {
    AspxConfig config;
    config.valid = true;
    config.quant_mode_env = 0;
    config.start_freq = 6;
    config.stop_freq = 1;
    config.master_freq_scale = 1;
    config.interpolation = true;
    config.preflat = false;
    config.limiter = true;
    config.noise_sbg = 3;
    config.freq_res_mode = 3;
    return config;
}

AspxFrame frame_for(const AspxConfig& config, bool master_reset) {
    return {.config = &config,
            .xover_subband_offset = 0,
            .balance = false,
            .master_reset = master_reset,
            .base_48k = true,
            .num_qmf_timeslots = kSlots,
            .num_ts_in_ats = 2,
            .ts_offset_hfgen = 6};
}

// One high resolution envelope over [first, last) A-SPX slots and one noise
// envelope, every signal group at qscf `sig` and every noise group at `noise`
// (Pseudocode 80's frequency deltas of 0 after the first value).
AspxChannel flat_channel(int sig, int noise, int first = 0, int last = 16) {
    AspxChannel c;
    c.framing.int_class = iclforge::ac4::detail::AspxIntClass::kFixFix;
    c.framing.num_env = 1;
    c.framing.num_noise = 1;
    c.framing.atsg_sig = {static_cast<std::int8_t>(first), static_cast<std::int8_t>(last)};
    c.framing.atsg_noise = {static_cast<std::int8_t>(first), static_cast<std::int8_t>(last)};
    c.framing.atsg_freqres[0] = 1;
    c.framing.tsg_ptr = -1;
    c.qmode_env = 0;
    c.sig[0].delta_dir = 0;
    c.sig[0].num_sbg = kHighGroups;
    c.sig[0].huff_index[0] = static_cast<std::uint16_t>(sig);
    for (int i = 1; i < kHighGroups; ++i) {
        c.sig[0].huff_index[static_cast<std::size_t>(i)] =
            70;  // ASPX_HCB_ENV_LEVEL_15_DF's cb_off: 0
    }
    c.noise[0].delta_dir = 0;
    c.noise[0].num_sbg = kNoiseGroups;
    c.noise[0].huff_index[0] = static_cast<std::uint16_t>(noise);
    c.noise[0].huff_index[1] = 29;  // ASPX_HCB_NOISE_LEVEL_DF's cb_off: 0
    return c;
}

struct Channel {
    std::vector<QmfValue> ext = std::vector<QmfValue>(static_cast<std::size_t>(kExtSlots) * 64);
    std::vector<QmfValue> out = std::vector<QmfValue>(static_cast<std::size_t>(kSlots) * 64);
    AspxChannelState state;

    // Q_low's slot ts, the delayed input at slot ts of the output.
    QmfValue& q_low(int ts, int sb) {
        return ext[static_cast<std::size_t>(ts + aspx::kTsOffsetHfadj) * 64 +
                   static_cast<std::size_t>(sb)];
    }
    [[nodiscard]] QmfValue at(int ts, int sb) const {
        return out[static_cast<std::size_t>(ts) * 64 + static_cast<std::size_t>(sb)];
    }
};

// NoiseTable's entry at `index` (Part 1 Table D.2).
QmfValue noise_entry(int index) {
    const auto& entry =
        iclforge::ac4::detail::tables::kAspxNoise[static_cast<std::size_t>(index % 512)];
    return {static_cast<iclforge::ac4::detail::Real>(entry[0]),
            static_cast<iclforge::ac4::detail::Real>(entry[1])};
}

void decode_one(const AspxFrame& frame, const AspxChannel& data, Channel& channel) {
    std::array<AspxChannelIo, 1> io{AspxChannelIo{
        .data = &data, .state = &channel.state, .ext = channel.ext, .out = channel.out}};
    REQUIRE(iclforge::ac4::detail::decode_aspx(frame, io).has_value());
}

}  // namespace

TEST_CASE(
    "frequency interleaving adds the waveform-coded groups, time interleaving replaces its slots",
    "[ac4dec][aspx]") {
    // A silent low band leaves the patches nothing, and the lowest noise floor
    // Table A.28 can send (qscf 29) leaves the extension near silence: what
    // reaches the output above the crossover is then the waveform-coded input.
    const AspxConfig config = dee_128k_config();
    AspxChannel data = flat_channel(0, 29);
    data.fic_used_in_sfb[2] = true;   // group [40, 42)
    data.tic_used_in_slot[3] = true;  // QMF slots 6 and 7
    Channel channel;
    for (int ts = 0; ts < kSlots + 6; ++ts) {
        for (int sb = 36; sb < 64; ++sb) {
            channel.q_low(ts, sb) = qmf(1000.0, -500.0);
        }
    }
    decode_one(frame_for(config, true), data, channel);
    for (int ts = 0; ts < kSlots; ++ts) {
        CAPTURE(ts);
        const bool tic = ts == 6 || ts == 7;
        for (int sb = 36; sb < 64; ++sb) {
            CAPTURE(sb);
            const QmfValue out = channel.at(ts, sb);
            if (tic) {
                CHECK(out == qmf(1000.0, -500.0));
            } else if (sb == 40 || sb == 41) {
                CHECK(abs(out - qmf(1000.0, -500.0)) < qmf_real(1.0));
            } else {
                CHECK(abs(out) < qmf_real(1.0));
            }
        }
    }
}

TEST_CASE("a sinusoid sits in its group's middle subband, a quarter turn further each slot",
          "[ac4dec][aspx]") {
    const AspxConfig config = dee_128k_config();
    AspxChannel data = flat_channel(20, 29);
    data.add_harmonic[3] = true;  // group [42, 44): (6 + 8) / 2 = 7 above sbx, subband 43
    Channel channel;
    decode_one(frame_for(config, true), data, channel);
    // Pseudocode 105 starts the first frame at index 1, and Table 196 turns by
    // a quarter per index; Pseudocode 104 negates the imaginary part in odd
    // subbands.
    const std::array<QmfValue, 4> unit = {QmfValue(Real{1}, Real{0}), QmfValue(Real{0}, Real{-1}),
                                          QmfValue(Real{-1}, Real{0}), QmfValue(Real{0}, Real{1})};
    const iclforge::ac4::detail::Real level = abs(channel.at(0, 43));
    REQUIRE(level > qmf_real(1.0));
    for (int ts = 0; ts < kSlots; ++ts) {
        CAPTURE(ts);
        CHECK(abs(channel.at(ts, 43) / level - unit[static_cast<std::size_t>((1 + ts) % 4)]) <
              Real(1e-3));
        CHECK(abs(channel.at(ts, 42)) < Real(1e-3) * level);
        CHECK(abs(channel.at(ts, 44)) < Real(1e-3) * level);
    }
    // The next interval goes on where this one stopped: index (31 + 1 + 1) % 4.
    decode_one(frame_for(config, false), data, channel);
    CHECK(abs(channel.at(0, 43) / level - unit[1]) < Real(1e-3));
}

TEST_CASE("the noise generator's index runs on from one interval into the next", "[ac4dec][aspx]") {
    // With nothing to patch and no sinusoid, the extension is noise only:
    // one level times NoiseTable at index (base + 20 ts + sb + 1) % 512,
    // base 0 after master_reset and then the previous interval's last.
    const AspxConfig config = dee_128k_config();
    const AspxChannel data = flat_channel(10, 0);
    Channel channel;
    const auto check_frame = [&](int base) {
        const QmfValue level = channel.at(0, 36) / noise_entry(base + 1);
        REQUIRE(abs(level) > qmf_real(1.0));
        for (int ts = 0; ts < kSlots; ++ts) {
            for (int sb = 0; sb < 20; ++sb) {
                const QmfValue expected = level * noise_entry(base + 20 * ts + sb + 1);
                CHECK(abs(channel.at(ts, 36 + sb) - expected) <
                      static_cast<Real>(kRelativeTolerance) * abs(level));
            }
        }
    };
    decode_one(frame_for(config, true), data, channel);
    check_frame(0);
    decode_one(frame_for(config, false), data, channel);
    check_frame((20 * 31 + 19 + 1) % 512);
    decode_one(frame_for(config, true), data, channel);  // master_reset starts it again
    check_frame(0);
}

TEST_CASE("an interval past its frame's end reaches the output in the next frame",
          "[ac4dec][aspx]") {
    const AspxConfig config = dee_128k_config();
    // FIXVAR to A-SPX slot 18 (QMF slot 36), then VARFIX from slot 2.
    AspxChannel fixvar = flat_channel(10, 0, 0, 18);
    fixvar.framing.int_class = iclforge::ac4::detail::AspxIntClass::kFixVar;
    AspxChannel varfix = flat_channel(10, 0, 2, 16);
    varfix.framing.int_class = iclforge::ac4::detail::AspxIntClass::kVarFix;
    Channel channel;
    decode_one(frame_for(config, true), fixvar, channel);
    const QmfValue level = channel.at(0, 36) / noise_entry(1);
    decode_one(frame_for(config, false), varfix, channel);
    // Slots 0 to 3 are the first interval's slots 32 to 35.
    for (int ts = 0; ts < 4; ++ts) {
        for (int sb = 0; sb < 20; ++sb) {
            CHECK(abs(channel.at(ts, 36 + sb) - level * noise_entry(20 * (ts + 32) + sb + 1)) <
                  static_cast<Real>(kRelativeTolerance) * abs(level));
        }
    }
    // The second interval starts at QMF slot 4, one index past the first's last.
    CHECK(abs(channel.at(4, 36) - level * noise_entry(20 * 35 + 19 + 1 + 1)) <
          static_cast<Real>(kRelativeTolerance) * abs(level));
}

TEST_CASE("a balanced pair shares the sum's scale factors as the balance says", "[ac4dec][aspx]") {
    // Pseudocode 84: with qmode 0, a balance of 32 (16 per step of the
    // balance codebook) puts channel 0 2^(32/2 - 12) = 16 times above channel 1.
    AspxConfig config = dee_128k_config();
    AspxChannel sum = flat_channel(40, 20);
    AspxChannel balance = flat_channel(16, 6);
    balance.stereo_mode = iclforge::ac4::detail::AspxStereoMode::kBalance;
    for (int i = 1; i < kHighGroups; ++i) {
        balance.sig[0].huff_index[static_cast<std::size_t>(i)] =
            24;  // ASPX_HCB_ENV_BALANCE_15_DF's cb_off
    }
    balance.noise[0].huff_index[1] = 12;  // ASPX_HCB_NOISE_BALANCE_DF's cb_off
    std::array<Channel, 2> channels;
    AspxFrame frame = frame_for(config, true);
    frame.balance = true;
    std::array<AspxChannelIo, 2> io{AspxChannelIo{.data = &sum,
                                                  .state = &channels[0].state,
                                                  .ext = channels[0].ext,
                                                  .out = channels[0].out},
                                    AspxChannelIo{.data = &balance,
                                                  .state = &channels[1].state,
                                                  .ext = channels[1].ext,
                                                  .out = channels[1].out}};
    REQUIRE(iclforge::ac4::detail::decode_aspx(frame, io).has_value());
    const auto energy = [&](const Channel& channel) {
        double e = 0.0;
        for (int ts = 0; ts < kSlots; ++ts) {
            for (int sb = 36; sb < 56; ++sb) {
                e += static_cast<double>(norm(channel.at(ts, sb)));
            }
        }
        return e;
    };
    CHECK(std::abs(10.0 * std::log10(energy(channels[0]) / energy(channels[1])) -
                   10.0 * std::log10(16.0)) < 0.5);
}

TEST_CASE("companding scales each slot by its level against full scale 1.0", "[ac4dec][aspx]") {
    // 5.7.5.2 with alpha 0.65: g = (L / full scale)^(0.35 / 0.65), G = 2^(1 / 0.65).
    constexpr double kFullScale = 32768.0;
    std::vector<QmfValue> ext(static_cast<std::size_t>(kExtSlots) * 64);
    const auto slot = [&](int ts, int sb) -> QmfValue& {
        return ext[static_cast<std::size_t>(ts + aspx::kTsOffsetHfadj) * 64 +
                   static_cast<std::size_t>(sb)];
    };
    for (int ts = 0; ts < kSlots + 6; ++ts) {
        for (int sb = 0; sb < 64; ++sb) {
            slot(ts, sb) = QmfValue(qmf_real(100.0 * (ts + 1)), Real{});
        }
    }
    const std::vector<QmfValue> before = ext;
    iclforge::ac4::detail::CompandingControl control;
    control.num_chan = 1;
    control.b_compand_on[0] = true;
    const std::array<iclforge::ac4::detail::CompandingChannel, 1> channels{
        iclforge::ac4::detail::CompandingChannel{
            .ext = ext, .sb1 = 36, .interval = {.first = 2, .last = 34}}};
    iclforge::ac4::detail::apply_companding(control, 0, qmf_real(kFullScale), channels);
    const double big_g = std::exp2(1.0 / 0.65);
    for (int ts = 0; ts < kSlots + 6; ++ts) {
        CAPTURE(ts);
        const double level = 0.9105 * 100.0 * (ts + 1) / kFullScale;  // E = |Re| for real values
        const double gain = ts >= 2 && ts < 34 ? std::pow(level, 0.35 / 0.65) * big_g : 1.0;
        CHECK(std::abs(qmf_units(slot(ts, 0).real()) - 100.0 * (ts + 1) * gain) <
              kRelativeTolerance * 100.0 * (ts + 1) * gain);
        CHECK(std::abs(qmf_units(slot(ts, 35).real()) - 100.0 * (ts + 1) * gain) <
              kRelativeTolerance * 100.0 * (ts + 1) * gain);
        CHECK(slot(ts, 36) ==
              before[static_cast<std::size_t>(ts + aspx::kTsOffsetHfadj) * 64 + 36]);
    }

    // b_compand_avg: one gain from the interval's mean level.
    ext = before;
    control.b_compand_on[0] = false;
    control.b_compand_avg = true;
    iclforge::ac4::detail::apply_companding(control, 0, qmf_real(kFullScale), channels);
    double mean = 0.0;
    for (int ts = 2; ts < 34; ++ts) {
        mean += 0.9105 * 100.0 * (ts + 1) / kFullScale / 32.0;
    }
    const double average = std::pow(mean, 0.35 / 0.65) * big_g;
    CHECK(std::abs(qmf_units(slot(10, 0).real()) - 1100.0 * average) <
          kRelativeTolerance * 1100.0 * average);
    CHECK(slot(35, 0) == before[static_cast<std::size_t>(35 + aspx::kTsOffsetHfadj) * 64]);
}

TEST_CASE("companding's gains are libm's at double and the project's own functions at float",
          "[ac4dec][aspx]") {
    // The same slots as above, whose level is exact in Real (a sum of 36 equal integers), so that
    // the gain is the only inexact step. At double the gain is std::pow and G is std::exp2, as
    // they always were; at float they are 2^(e log2 L) and 2^(1 / alpha) through
    // iclforge::internal::scalar_exp2 and scalar_log2, plain float multiplies and adds, and the
    // samples that come out are pinned to the bit: the C libraries' powf and exp2f differ in
    // the last bit on some inputs, and a Cortex-M3 or an ESP32 must give the host's samples
    // (planning/ac4.md, D14a4). At Fixed32 the level and the gains are MantExp's, and its own
    // log2 and exp2 (planning/ac4.md, D14d).
    std::vector<QmfValue> ext(static_cast<std::size_t>(kExtSlots) * 64);
    const auto slot = [&](int ts, int sb) -> QmfValue& {
        return ext[static_cast<std::size_t>(ts + aspx::kTsOffsetHfadj) * 64 +
                   static_cast<std::size_t>(sb)];
    };
    for (int ts = 0; ts < kSlots + 6; ++ts) {
        for (int sb = 0; sb < 64; ++sb) {
            slot(ts, sb) = QmfValue(qmf_real(100.0 * (ts + 1)), Real{});
        }
    }
    iclforge::ac4::detail::CompandingControl control;
    control.num_chan = 1;
    control.b_compand_on[0] = true;
    const std::array<iclforge::ac4::detail::CompandingChannel, 1> channels{
        iclforge::ac4::detail::CompandingChannel{
            .ext = ext, .sb1 = 36, .interval = {.first = 2, .last = 34}}};
    const Real full_scale = qmf_real(32768.0);
    iclforge::ac4::detail::apply_companding(control, 0, full_scale, channels);

    // A generic lambda, so that each scalar's branch is compiled only at that scalar.
    const auto expected = [&]<typename R>(int ts) -> R {
        const R sample = qmf_real(100.0 * (ts + 1));
        if constexpr (iclforge::ac4::detail::dsp::kFixed<R>) {
            using iclforge::internal::MantExp;
            const MantExp alpha{0.65};
            const MantExp exponent = (MantExp{1} - alpha) / alpha;
            const MantExp level = MantExp(0.9105) * (MantExp{36} * MantExp{sample}) / MantExp{36} /
                                  MantExp{full_scale};
            const MantExp gain = scalar_exp2(exponent * scalar_log2(level));
            const MantExp big_g = scalar_exp2(MantExp{1} / alpha);
            return iclforge::ac4::detail::dsp::apply_gain<R>(gain * big_g, sample);
        } else {
            constexpr R kAlpha = R(0.65);
            constexpr R kExponent = (R{1} - kAlpha) / kAlpha;
            const R level = R(0.9105) * (R(36) * sample) / R(36) / full_scale;
            R gain{};
            R big_g{};
            if constexpr (std::is_same_v<R, double>) {
                gain = std::pow(level, kExponent);
                big_g = std::exp2(R{1} / kAlpha);
            } else {
                gain = iclforge::internal::scalar_exp2(kExponent * iclforge::internal::scalar_log2(level));
                big_g = iclforge::internal::scalar_exp2(R{1} / kAlpha);
            }
            return sample * (gain * big_g);
        }
    };
    for (int ts = 2; ts < 34; ++ts) {
        CAPTURE(ts);
        CHECK(slot(ts, 0).real() == expected.template operator()<Real>(ts));
        CHECK(slot(ts, 35).real() == expected.template operator()<Real>(ts));
    }
    if constexpr (std::is_same_v<Real, float>) {
        // Outside a template a discarded branch is still compiled: the cast keeps it valid at
        // double.
        const auto bits = [&](int ts) {
            return std::bit_cast<std::uint32_t>(static_cast<float>(slot(ts, 0).real()));
        };
        CHECK(bits(2) == 0x42845e7cU);
        CHECK(bits(9) == 0x43d2f033U);
        CHECK(bits(17) == 0x4482436dU);
        CHECK(bits(33) == 0x452d4551U);
    }
}
