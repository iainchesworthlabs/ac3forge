#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <cstdint>
#include <optional>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/meta/loudness.hpp"

// Momentary/short-term loudness, Loudness Range and true peak - the R128
// metering roadmap item C1 adds on top of the pre-existing integrated_lkfs()
// (whose own calibration/surround-weighting/silence tests already live in
// test_drc.cpp; this file only covers the new surface). All of it needs real,
// multi-second, non-silent content: a 400 ms/3 s window, EBU Tech 3342's
// cascaded gate and an oversampled peak are all defined over real programme
// material, not frame 0 or digital silence.

using ac3::meta::LoudnessMeter;

namespace {

// Same generator shape as test_drc.cpp's calibration test: a single tone at
// a chosen peak amplitude, long enough to fill whichever window is under
// test many times over.
std::vector<float> make_tone(double seconds, double freq_hz, double amplitude,
                              double sample_rate = 48000.0) {
    const auto n = static_cast<std::size_t>(seconds * sample_rate);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * freq_hz * static_cast<double>(i) /
                                  sample_rate));
    }
    return out;
}

double dbfs(double amplitude) { return 20.0 * std::log10(amplitude); }

}  // namespace

// --- momentary / short-term -------------------------------------------------

TEST_CASE("momentary and short-term loudness are undefined before their window elapses",
          "[loudness]") {
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};

    // 200 ms: neither the 400 ms momentary window nor the 3 s short-term one
    // has elapsed yet.
    const auto tone_200ms = make_tone(0.2, 1000.0, 0.1);
    const std::array<std::span<const float>, 2> channels_200ms = {tone_200ms, tone_200ms};
    meter.push(channels_200ms);
    CHECK_FALSE(meter.momentary_lkfs().has_value());
    CHECK_FALSE(meter.short_term_lkfs().has_value());

    // Another 300 ms (500 ms total): momentary's 400 ms has now elapsed,
    // short-term's 3 s has not. If momentary read the short-term window's
    // step count (or vice versa), one of these two checks would fail.
    const auto tone_300ms = make_tone(0.3, 1000.0, 0.1);
    const std::array<std::span<const float>, 2> channels_300ms = {tone_300ms, tone_300ms};
    meter.push(channels_300ms);
    CHECK(meter.momentary_lkfs().has_value());
    CHECK_FALSE(meter.short_term_lkfs().has_value());
}

TEST_CASE("momentary and short-term loudness hit the same BS.1770 calibration point "
          "integrated loudness does, on steady content",
          "[loudness]") {
    // BS.1770/EBU Tech 3341: a 1 kHz sine at -20 dBFS reads -20.0 LKFS. On a
    // signal with no dynamics at all, the un-gated 400 ms and 3 s windows
    // should read the same thing the whole-programme gated measurement does
    // - there is nothing for gating or windowing to disagree about.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto tone = make_tone(10.0, 1000.0, 0.1);
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);

    const auto integrated = meter.integrated_lkfs();
    const auto momentary = meter.momentary_lkfs();
    const auto short_term = meter.short_term_lkfs();
    REQUIRE(integrated.has_value());
    REQUIRE(momentary.has_value());
    REQUIRE(short_term.has_value());
    CHECK(*integrated == Catch::Approx(-20.0).margin(0.1));
    CHECK(*momentary == Catch::Approx(-20.0).margin(0.1));
    CHECK(*short_term == Catch::Approx(-20.0).margin(0.1));
}

TEST_CASE("momentary loudness tracks a level step within one window, short-term lags",
          "[loudness]") {
    // Loud for 2 s, then quiet (-40 dBFS, well below the loud segment) for
    // another 2 s. Read right at the end: momentary's 400 ms window is
    // entirely inside the quiet tail, so it should read close to -40 LKFS.
    // Short-term's 3 s window still spans 1 s of the loud segment, so it
    // must sit measurably ABOVE momentary - if short_term_lkfs() were
    // accidentally wired to the same 4-step window as momentary, this
    // difference would vanish.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto loud = make_tone(2.0, 1000.0, 0.1);     // -20 dBFS
    const auto quiet = make_tone(2.0, 1000.0, 0.01);   // -40 dBFS
    const std::array<std::span<const float>, 2> loud_channels = {loud, loud};
    const std::array<std::span<const float>, 2> quiet_channels = {quiet, quiet};
    meter.push(loud_channels);
    meter.push(quiet_channels);

    const auto momentary = meter.momentary_lkfs();
    const auto short_term = meter.short_term_lkfs();
    REQUIRE(momentary.has_value());
    REQUIRE(short_term.has_value());
    CHECK(*momentary == Catch::Approx(-40.0).margin(0.5));
    CHECK(*short_term > *momentary + 3.0);
}

// --- Loudness Range ----------------------------------------------------------

TEST_CASE("Loudness Range matches EBU Tech 3342 Table 1's own minimum-requirements test #3",
          "[loudness]") {
    // Tech 3342 (2023) Table 1, test case 3: "As #1 [stereo sine wave,
    // 1000 Hz, in phase, 20 s per level], with the 2 tones at -40.0 dBFS and
    // -20.0 dBFS respectively" -> expected response LRA = 20 +/-1 LU. This is
    // the standard's own published compliance vector, not a value derived
    // from this implementation.
    //
    // Deliberately test #3 (20 dB gap) rather than #1 (10 dB gap): with only
    // a 10 dB gap, the quieter segment's short-term loudness sits close
    // enough to the absolute-gated mean (about -7 LU below it, worked out
    // from the two segments' relative power) that it clears Tech 3342's
    // real -20 LU relative gate AND a wrongly-implemented -10 LU one (i.e.
    // integrated_lkfs()'s own relative-gate threshold) equally - a #1-shaped
    // test was tried first and kept passing even with the relative gate
    // deliberately mistyped to -10 LU, which is exactly the class of false
    // pass this project's validation rule warns about. At a 20 dB gap the
    // quiet segment sits roughly -17 LU below the mean: inside the real
    // -20 LU gate (so it counts, giving the expected ~20 LU spread) but
    // outside a -10 LU one (so a mis-gated implementation drops it and
    // collapses LRA toward 0) - confirmed by reintroducing that exact typo
    // and watching this test fail before reverting it.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto quiet = make_tone(20.0, 1000.0, 0.01);          // -40.0 dBFS peak
    const auto loud = make_tone(20.0, 1000.0, 0.1);            // -20.0 dBFS peak
    const std::array<std::span<const float>, 2> quiet_channels = {quiet, quiet};
    const std::array<std::span<const float>, 2> loud_channels = {loud, loud};
    meter.push(quiet_channels);
    meter.push(loud_channels);

    const auto lra = meter.loudness_range();
    REQUIRE(lra.has_value());
    CHECK(*lra == Catch::Approx(20.0).margin(1.0));
}

TEST_CASE("Loudness Range is undefined for a programme with no short-term history yet",
          "[loudness]") {
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto tone = make_tone(1.0, 1000.0, 0.1);  // well under the 3 s short-term window
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);
    CHECK_FALSE(meter.loudness_range().has_value());
}

// --- true peak ----------------------------------------------------------------

TEST_CASE("true peak matches sample peak within a fraction of a dB for a slow, "
          "well-sampled tone",
          "[loudness][true-peak]") {
    // A 100 Hz tone at 48 kHz puts ~480 samples per cycle, so some sample
    // always lands within a small fraction of a degree of the true peak -
    // oversampling should find essentially nothing extra here. This is the
    // control for the inter-sample-peak test below: it proves the
    // oversampler does not just unconditionally read high.
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const auto tone = make_tone(1.0, 100.0, 0.5);
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);

    const auto true_peak = meter.true_peak_dbtp();
    REQUIRE(true_peak.has_value());
    CHECK(*true_peak == Catch::Approx(dbfs(0.5)).margin(0.05));
}

TEST_CASE("true peak finds the inter-sample peak a sample-peak reading misses",
          "[loudness][true-peak]") {
    // The classic construction: a full-scale sine at exactly Fs/4 with a
    // 45-degree phase offset. At integer sample n, sin(pi*n/2 + pi/4) is
    // +-1/sqrt(2) for EVERY sample (period-4 pattern 0.7071, 0.7071,
    // -0.7071, -0.7071, ...), so the sample-peak reads a constant
    // -3.01 dBFS no matter how long the tone runs. The continuous
    // waveform's actual peaks (amplitude 1.0, i.e. 0 dBTP) fall exactly
    // halfway between samples (at n = 0.5 + 4k), landing precisely on the
    // 4x-oversampled grid's midpoint phase - the textbook case a
    // sample-peak meter cannot see at all and an oversampled true-peak
    // meter is built to catch.
    constexpr double kSampleRate = 48000.0;
    constexpr double kFreq = kSampleRate / 4.0;
    constexpr std::size_t kSamples = 4000;  // 1000 full periods of the pattern
    std::vector<float> tone(kSamples);
    double max_sample = 0.0;
    for (std::size_t n = 0; n < kSamples; ++n) {
        const double v = std::sin(2.0 * std::numbers::pi * kFreq * static_cast<double>(n) /
                                       kSampleRate +
                                   std::numbers::pi / 4.0);
        tone[n] = static_cast<float>(v);
        max_sample = std::max(max_sample, std::abs(v));
    }
    // Confirms the construction: every sample sits at 1/sqrt(2), not 1.0.
    REQUIRE(max_sample == Catch::Approx(1.0 / std::numbers::sqrt2).margin(1e-9));

    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    const std::array<std::span<const float>, 2> channels = {tone, tone};
    meter.push(channels);

    const auto true_peak = meter.true_peak_dbtp();
    REQUIRE(true_peak.has_value());
    const double sample_peak_dbtp = dbfs(max_sample);  // ~ -3.01 dBFS

    // The oversampled reading must sit clearly above what sample-peak alone
    // would report (BS.1770-4 Annex 2's whole reason to exist), and land
    // close to the true analytic answer of 0 dBTP - Annex 2's own worst-case
    // under-read table puts a 4x-oversampled reading within ~0.7 dB of the
    // true value even at Nyquist itself; Fs/4 is comfortably inside that.
    CHECK(*true_peak > sample_peak_dbtp + 1.5);
    CHECK(*true_peak == Catch::Approx(0.0).margin(1.0));
}

TEST_CASE("true peak includes the LFE channel that integrated loudness excludes",
          "[loudness][true-peak]") {
    // header comment's own design point: true peak is about physical
    // overload, so unlike every weighted-loudness measure it must not drop
    // LFE. Silence everywhere except a full-scale LFE tone reproduces
    // test_drc.cpp's "lfe_only" pattern, but checks the opposite property.
    const auto tone = make_tone(1.0, 60.0, 0.9);
    const std::vector<float> silence(tone.size(), 0.0f);
    // 3/2 + LFE coded order is L, C, R, Ls, Rs, LFE.
    const std::array<std::span<const float>, 6> lfe_only = {silence, silence, silence,
                                                              silence, silence, tone};
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k3_2, true};
    meter.push(lfe_only);

    CHECK_FALSE(meter.integrated_lkfs().has_value());  // unchanged existing behaviour
    const auto true_peak = meter.true_peak_dbtp();
    REQUIRE(true_peak.has_value());
    CHECK(*true_peak == Catch::Approx(dbfs(0.9)).margin(0.1));
}

TEST_CASE("true peak is undefined before any sample is pushed", "[loudness][true-peak]") {
    LoudnessMeter meter{ac3::SampleRate::k48000, ac3::Acmod::k2_0, false};
    CHECK_FALSE(meter.true_peak_dbtp().has_value());
}


// --- BS.1770-5 Annex 3 positional weighting (roadmap IO10) --------------------

namespace {

namespace chanmap = ac3::eac3::chanmap;
using Location = chanmap::Location;

// The layouts a Table 5.8 acmod cannot name on its own. Each is the 5.1 bed
// plus whichever Table E2.5 pair bits a dependent substream would carry, which
// is exactly how the encoder allocates them (chanmap::allocate).
constexpr std::uint16_t k51 = chanmap::acmod_map(ac3::Acmod::k3_2, true);
constexpr std::uint16_t k71 = k51 | chanmap::kLrsRrsBit;
constexpr std::uint16_t k512 = k51 | chanmap::kVhlVhrBit;
constexpr std::uint16_t k514 = k51 | chanmap::kTopQuad;
constexpr std::uint16_t k714 = k71 | chanmap::kTopQuad;

// What BS.1770-5 Annex 3 Table 5 tabulates for the BS.2051 loudspeaker each
// Table E2.5 location stands for - written out here independently of the
// implementation's own switch, so the two have to agree rather than one being
// read off the other. 0.0 marks an LFE-type location, which Annex 3 drops from
// the sum entirely rather than weighting.
double expected_weight(Location location) {
    switch (location) {
        case Location::kLeft:           return 1.0;   // M+030
        case Location::kCentre:         return 1.0;   // M+000
        case Location::kRight:          return 1.0;   // M-030
        case Location::kLc:             return 1.0;   // M+SC
        case Location::kRc:             return 1.0;   // M-SC
        case Location::kLeftSurround:   return 1.41;  // M+110
        case Location::kRightSurround:  return 1.41;  // M-110
        case Location::kLsd:            return 1.41;  // M+090
        case Location::kRsd:            return 1.41;  // M-090
        case Location::kLw:             return 1.41;  // M+060
        case Location::kRw:             return 1.41;  // M-060
        case Location::kLrs:            return 1.0;   // M+135
        case Location::kRrs:            return 1.0;   // M-135
        case Location::kCs:             return 1.0;   // M+180
        case Location::kVhl:            return 1.0;   // U+030
        case Location::kVhr:            return 1.0;   // U-030
        case Location::kVhc:            return 1.0;   // U+000
        case Location::kLts:            return 1.0;   // U+110
        case Location::kRts:            return 1.0;   // U-110
        case Location::kTs:             return 1.0;   // T+000
        case Location::kLfe:            return 0.0;   // excluded
        case Location::kLfe2:           return 0.0;   // excluded
    }
    return -1.0;  // unreachable; a new enumerator should fail loudly
}

// Integrated loudness of a single -20 dBFS tone in one channel of weight G.
// A lone channel carries mean square a^2/2 = 0.005, i.e. -23.01 dB, and
// BS.1770's -0.691 offset cancels the K-weighting gain at 1 kHz exactly
// (Annex 1 Note 1), so the reading is -23.01 + 10*log10(G).
double lone_channel_lkfs(double weight) {
    return 20.0 * std::log10(0.1) - 10.0 * std::log10(2.0) + 10.0 * std::log10(weight);
}

}  // namespace

TEST_CASE("position_weight reproduces BS.1770-5 Annex 3 Table 5 for every Table E2.5 location",
          "[loudness][bs1770-5]") {
    // Every location Table E2.5 can name, not just the ones today's layouts
    // use: expand(0xFFFF) is all 22 of them, so a new enumerator cannot be
    // added without this test having an opinion about it.
    const auto every = chanmap::expand(0xFFFF);
    REQUIRE(every.count == chanmap::kMaxChannels);
    for (int i = 0; i < every.count; ++i) {
        const auto location = every[i];
        CAPTURE(chanmap::name(location));
        const double expected = expected_weight(location);
        const auto actual = ac3::meta::position_weight(location);
        if (expected == 0.0) {
            // Annex 3 weights "each channel except the LFE channels" - an
            // absent term, which is not the same as a zero-weight one.
            CHECK_FALSE(actual.has_value());
        } else {
            REQUIRE(actual.has_value());
            CHECK(*actual == Catch::Approx(expected));
        }
    }
}

TEST_CASE("only the 60..120 degree sector is surround-weighted, whatever it is called",
          "[loudness][bs1770-5]") {
    // The two results most likely to be got wrong by reasoning from channel
    // NAMES instead of from Table 4's angles, stated as their own assertions.
    //
    // A 7.1 layout's rear pair is called "surround" and is not weighted:
    // M±135 is past the sector's 120-degree edge. Every height channel is
    // likewise unweighted - Table 4's |phi| < 30 row simply does not cover
    // the upper layer, so no azimuth can bring one back in.
    CHECK(*ac3::meta::position_weight(Location::kLrs) == Catch::Approx(1.0));
    CHECK(*ac3::meta::position_weight(Location::kRrs) == Catch::Approx(1.0));
    CHECK(*ac3::meta::position_weight(Location::kVhl) == Catch::Approx(1.0));
    CHECK(*ac3::meta::position_weight(Location::kLts) == Catch::Approx(1.0));
    CHECK(*ac3::meta::position_weight(Location::kTs) == Catch::Approx(1.0));
    // While the wides ARE weighted, sitting exactly on the sector's inclusive
    // 60-degree edge, and Table 5's M±060 row says 1.41 outright.
    CHECK(*ac3::meta::position_weight(Location::kLw) == Catch::Approx(1.41));
    CHECK(*ac3::meta::position_weight(Location::kRw) == Catch::Approx(1.41));
}

TEST_CASE("the meter weights each channel of a wide layout by its own position",
          "[loudness][bs1770-5]") {
    // The end-to-end check that the meter actually APPLIES Table 4 rather
    // than merely being able to look it up: probe one channel at a time with
    // the same tone and read the loudness back. A channel in the +1.5 dB
    // sector must read 10*log10(1.41) = 1.49 dB above an unweighted one, and
    // an LFE-type channel must produce no reading at all.
    const std::uint16_t mask = GENERATE(k71, k512, k514, k714);
    const auto layout = chanmap::expand(mask);
    CAPTURE(mask, layout.count);

    // 2 s is seventeen 400 ms blocks - ample for a steady tone, and this
    // probe runs once per channel of four layouts, so the duration is the
    // whole cost of the test.
    const auto tone = make_tone(2.0, 1000.0, 0.1);  // -20 dBFS
    const std::vector<float> silence(tone.size(), 0.0f);

    for (int probe = 0; probe < layout.count; ++probe) {
        CAPTURE(chanmap::name(layout[probe]));
        std::vector<std::span<const float>> channels(static_cast<std::size_t>(layout.count),
                                                      silence);
        channels[static_cast<std::size_t>(probe)] = tone;

        ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, layout};
        CHECK(meter.channel_count() == layout.count);
        meter.push(channels);

        const double weight = expected_weight(layout[probe]);
        const auto integrated = meter.integrated_lkfs();
        if (weight == 0.0) {
            // A lone LFE tone contributes nothing to loudness at all, so
            // nothing clears the -70 LKFS absolute gate...
            CHECK_FALSE(integrated.has_value());
            // ...but true peak still sees it, which is the property that
            // distinguishes "excluded from the sum" from "silent".
            const auto true_peak = meter.true_peak_dbtp();
            REQUIRE(true_peak.has_value());
            CHECK(*true_peak == Catch::Approx(dbfs(0.1)).margin(0.1));
        } else {
            REQUIRE(integrated.has_value());
            CHECK(*integrated == Catch::Approx(lone_channel_lkfs(weight)).margin(0.1));
        }
    }
}

TEST_CASE("a 5.1 layout measures identically through Annex 1 and Annex 3",
          "[loudness][bs1770-5]") {
    // Annex 1's Table 3 gives Ls and Rs 1.41 by name; Annex 3's Table 4 gives
    // the same 1.41 to whatever sits at 60..120 degrees, which is where Ls
    // and Rs are (M±110). So for every Table 5.8 layout with a discrete
    // surround PAIR the two algorithms are the same function, and the
    // layout=rendered switch changes nothing for a plain 5.1 stream. If the
    // positional table ever drifted off Table 3 for these five channels, this
    // is what would catch it.
    const auto layout = chanmap::expand(k51);
    const auto tone = make_tone(4.0, 1000.0, 0.1);
    const auto quieter = make_tone(4.0, 1000.0, 0.05);
    // Deliberately not the same level in every channel: equal levels would
    // still agree even if the two constructors permuted the weights.
    const std::array<std::span<const float>, 6> channels = {tone,    quieter, tone,
                                                             quieter, tone,    quieter};

    ac3::meta::LoudnessMeter annex1{ac3::SampleRate::k48000, ac3::Acmod::k3_2, true};
    ac3::meta::LoudnessMeter annex3{ac3::SampleRate::k48000, layout};
    annex1.push(channels);
    annex3.push(channels);

    const auto a1 = annex1.integrated_lkfs();
    const auto a3 = annex3.integrated_lkfs();
    REQUIRE(a1.has_value());
    REQUIRE(a3.has_value());
    CHECK(*a3 == Catch::Approx(*a1).margin(1e-9));
}

TEST_CASE("the two algorithms disagree only about the lone surround of 2/1 and 3/1",
          "[loudness][bs1770-5]") {
    // The one place the switch changes an answer for a layout an acmod CAN
    // name. Annex 1 has no Table 3 entry for a mono surround and this meter
    // reads it as the surround field collapsed to one channel (+1.5 dB);
    // Annex 3 sees Table E2.5's Cs, a rear centre at M+180, which Table 5
    // puts at unity. Both are faithful to their own algorithm - the point of
    // the test is that the difference is exactly the one weight, and is not
    // silently zero (which would mean layout= was not reaching the meter).
    const auto layout = chanmap::expand(chanmap::acmod_map(ac3::Acmod::k2_1, false));
    REQUIRE(layout.count == 3);
    REQUIRE(layout[2] == Location::kCs);

    const auto tone = make_tone(4.0, 1000.0, 0.1);
    const std::vector<float> silence(tone.size(), 0.0f);
    // Only the surround carries signal, so the whole reading IS that
    // channel's weighted contribution.
    const std::array<std::span<const float>, 3> channels = {silence, silence, tone};

    ac3::meta::LoudnessMeter annex1{ac3::SampleRate::k48000, ac3::Acmod::k2_1, false};
    ac3::meta::LoudnessMeter annex3{ac3::SampleRate::k48000, layout};
    annex1.push(channels);
    annex3.push(channels);

    const auto a1 = annex1.integrated_lkfs();
    const auto a3 = annex3.integrated_lkfs();
    REQUIRE(a1.has_value());
    REQUIRE(a3.has_value());
    CHECK(*a1 == Catch::Approx(lone_channel_lkfs(1.41)).margin(0.1));
    CHECK(*a3 == Catch::Approx(lone_channel_lkfs(1.0)).margin(0.1));
    CHECK(*a1 - *a3 == Catch::Approx(10.0 * std::log10(1.41)).margin(0.01));
}

TEST_CASE("widening 5.1 to 7.1.4 adds channels the meter counts but does not surround-weight",
          "[loudness][bs1770-5]") {
    // The IO10 headline, as one measurement: the same six-channel bed plus
    // six more channels carrying the same tone. Every added channel (Lrs,
    // Rrs, Vhl, Vhr, Lts, Rts) is unity-weighted, so the 7.1.4 reading must
    // sit above the 5.1 one by exactly the power those six unity terms add -
    // not by the 1.41-weighted amount a "surround" reading of their names
    // would give.
    const auto bed = chanmap::expand(k51);
    const auto wide = chanmap::expand(k714);
    REQUIRE(bed.count == 6);
    REQUIRE(wide.count == 12);

    const auto tone = make_tone(4.0, 1000.0, 0.1);
    const std::vector<float> silence(tone.size(), 0.0f);

    // Bed: L, C, R at unity and Ls, Rs at 1.41, LFE excluded.
    std::vector<std::span<const float>> bed_channels(6, tone);
    bed_channels[5] = silence;  // keep the LFE out of true peak comparisons too
    ac3::meta::LoudnessMeter bed_meter{ac3::SampleRate::k48000, bed};
    bed_meter.push(bed_channels);

    std::vector<std::span<const float>> wide_channels(12, tone);
    wide_channels[wide.index_of(Location::kLfe)] = silence;
    ac3::meta::LoudnessMeter wide_meter{ac3::SampleRate::k48000, wide};
    wide_meter.push(wide_channels);

    const auto bed_lkfs = bed_meter.integrated_lkfs();
    const auto wide_lkfs = wide_meter.integrated_lkfs();
    REQUIRE(bed_lkfs.has_value());
    REQUIRE(wide_lkfs.has_value());

    // Sum of weights: bed is 3*1.0 + 2*1.41 = 5.82; 7.1.4 adds six unity
    // channels for 11.82. The reading rises by 10*log10(11.82/5.82).
    const double expected_gain = 10.0 * std::log10(11.82 / 5.82);
    CHECK(*wide_lkfs - *bed_lkfs == Catch::Approx(expected_gain).margin(0.02));
    // Had the six new channels been surround-weighted, the rise would have
    // been 10*log10((5.82 + 6*1.41)/5.82) - a full 1.2 dB higher, far outside
    // the margin above.
    const double wrong_gain = 10.0 * std::log10((5.82 + 6.0 * 1.41) / 5.82);
    CHECK(wrong_gain - expected_gain > 1.0);
}
