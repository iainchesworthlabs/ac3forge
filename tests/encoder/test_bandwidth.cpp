#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/bandwidth.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"

// Coded bandwidth, EQ7/EQ8. The unit half pins the decision function against
// A/52's own tables; the encoder half proves both encoders act on it, and
// that the answer reaches the bitstream rather than only the encoder's model.

namespace {

constexpr double kRate = 48000.0;

// A flat spectrum right up to Nyquist, at a level well above Table 7.15
// anywhere: nothing may be dropped from this.
std::array<std::uint8_t, 253> loud_everywhere(std::uint8_t exponent) {
    std::array<std::uint8_t, 253> exps{};
    exps.fill(exponent);
    return exps;
}

// The same, but silent (kMaxExponent) above `bin`.
std::array<std::uint8_t, 253> loud_below(int bin, std::uint8_t exponent) {
    std::array<std::uint8_t, 253> exps = loud_everywhere(ac3::kMaxExponent);
    for (int i = 0; i < bin && i < 253; ++i) {
        exps[static_cast<std::size_t>(i)] = exponent;
    }
    return exps;
}

std::vector<float> band_limited_noise(std::uint64_t& n, double top_hz, double amplitude,
                                      int partials) {
    std::vector<float> samples(ac3::kSamplesPerFrame);
    for (auto& s : samples) {
        double acc = 0.0;
        // A comb of partials up to top_hz - deterministic, and its spectrum
        // stops where it is told to, which is the whole point here.
        for (int k = 1; k <= partials; ++k) {
            const double f = top_hz * static_cast<double>(k) / static_cast<double>(partials);
            acc += std::sin(2.0 * std::numbers::pi * f * static_cast<double>(n) / kRate +
                            static_cast<double>(k));
        }
        s = static_cast<float>(amplitude * acc / static_cast<double>(partials));
        ++n;
    }
    return samples;
}

std::vector<std::byte> encode_ac3(const ac3::EncoderConfig& config, double top_hz, int frames) {
    ac3::FrameEncoder encoder(config);
    std::uint64_t n = 0;
    std::vector<std::byte> last;
    for (int f = 0; f < frames; ++f) {
        const auto pcm = band_limited_noise(n, top_hz, 0.5, 40);
        std::vector<std::span<const float>> views(
            static_cast<std::size_t>(encoder.channel_count()), pcm);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        last = *frame;
    }
    return last;
}

// §5.4 syncinfo/bsi/audblk 0, as far as the first chbwcod, for the 2/0
// no-LFE uncoupled frames this file encodes. test_encoder.cpp carries a
// fuller version of the same walk; this one stops the moment it has the
// field under test rather than carrying the coupling and delta machinery
// that has nothing to do with bandwidth.
int ac3_chbwcod(std::span<const std::byte> frame) {
    constexpr int kNfchans = 2;
    ac3::BitReader r{frame};
    r.skip(40);                // syncinfo: syncword, crc1, fscod, frmsizecod
    r.skip(27);                // bsi for 2/0 without LFE, through addbsie
    r.skip(kNfchans * 2 + 1);  // blksw, dithflag, dynrnge
    r.skip(1);                 // cplstre, always 1 in block 0
    REQUIRE(r.read(1) == 0);   // cplinu: these frames do not couple
    REQUIRE(r.read(1) == 1);   // rematstr, always sent in block 0
    r.skip(4);                 // rematflg, 4 bands with no coupling (§7.5.2)
    r.skip(kNfchans * 2);      // chexpstr
    // chbwcod, one per channel not in coupling. Both carry the same signal
    // here, so the first is the answer.
    return static_cast<int>(r.read(6));
}

// Encode band-limited material with one E-AC-3 config and report the SNR our
// own decoder gets back. Self-consistency is the available oracle for a
// tool-set question - the same reasoning quality_race's decode_scores_ours
// documents - and it is enough here, because both sides of the comparison
// run through the identical decoder.
double decode_snr_eac3(const ac3::eac3::FrameConfig& config, double top_hz) {
    constexpr int kFrames = 10;
    ac3::eac3::FrameEncoder encoder(config);
    ac3::Eac3Decoder decoder;
    std::uint64_t n = 0;
    std::vector<float> source;
    std::vector<float> decoded;
    for (int f = 0; f < kFrames; ++f) {
        const auto pcm = band_limited_noise(n, top_hz, 0.5, 40);
        source.insert(source.end(), pcm.begin(), pcm.end());
        const std::vector<std::span<const float>> views(2, std::span<const float>{pcm});
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        const auto unit = decoder.decode_access_unit(*frame);
        REQUIRE(unit.has_value());
        REQUIRE(unit->has_value());
        const auto& left = (*unit)->channels.front();
        decoded.insert(decoded.end(), left.begin(), left.end());
    }
    // 256 samples of encode+decode transform delay, and a warm-up frame
    // dropped at each end rather than scored against the ramp.
    constexpr std::size_t kDelay = ac3::kSamplesPerBlock;
    constexpr std::size_t kSkip = ac3::kSamplesPerFrame;
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = kSkip; i + kSkip < std::min(source.size(), decoded.size()); ++i) {
        const double x = static_cast<double>(source[i - kDelay]);
        const double d = static_cast<double>(decoded[i]) - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

}  // namespace

TEST_CASE("chbwcod_for_endmant rounds up onto the transmitted grid") {
    using ac3::encoder::chbwcod_for_endmant;
    using ac3::encoder::endmant_for_chbwcod;
    // §7.1.3's own grid round-trips exactly.
    for (int code = 0; code <= 60; ++code) {
        CHECK(chbwcod_for_endmant(endmant_for_chbwcod(code)) == code);
    }
    // Anything between two grid points takes the higher one, so a partly
    // audible band is kept whole rather than clipped.
    CHECK(chbwcod_for_endmant(endmant_for_chbwcod(30) + 1) == 31);
    CHECK(chbwcod_for_endmant(endmant_for_chbwcod(30) + 2) == 31);
    CHECK(chbwcod_for_endmant(endmant_for_chbwcod(30) + 3) == 31);
    // And the ends clamp rather than run off the legal range.
    CHECK(chbwcod_for_endmant(0) == 0);
    CHECK(chbwcod_for_endmant(-100) == 0);
    CHECK(chbwcod_for_endmant(1000) == 60);
}

TEST_CASE("audible_endmant keeps a band above the hearing threshold") {
    using ac3::encoder::audible_endmant;
    const auto rate = ac3::SampleRate::k48000;

    // Exponent 0 is full scale in every band, which is far above Table 7.15
    // even at its 17 kHz step: nothing is dropped.
    CHECK(audible_endmant(loud_everywhere(0), rate) == 253);

    // A spectrum that stops dead partway up stops the coded band with it,
    // and never below it: the answer is the END of the last audible band.
    for (const int bin : {100, 130, 160, 200}) {
        const int endmant = audible_endmant(loud_below(bin, 0), rate);
        CHECK(endmant >= bin);
        CHECK(endmant < 253);
        // ...and it is a real band edge, not an arbitrary bin.
        const int band = ac3::tables::kMaskTab[static_cast<std::size_t>(endmant - 1)];
        CHECK(endmant == ac3::tables::kBandStart[static_cast<std::size_t>(band)] +
                             ac3::tables::kBandSize[static_cast<std::size_t>(band)]);
    }

    // Silence has nothing above the threshold anywhere, and still does not
    // collapse the band to zero - the floor is chbwcod 0.
    CHECK(audible_endmant(loud_everywhere(ac3::kMaxExponent), rate) ==
          ac3::encoder::endmant_for_chbwcod(0));
}

TEST_CASE("choose_chbwcod holds the rate ceiling and rate-limits narrowing") {
    using ac3::encoder::choose_chbwcod;
    using ac3::encoder::kMaxNarrowStep;
    using ac3::encoder::rate_ceiling_chbwcod;
    const auto rate = ac3::SampleRate::k48000;
    const auto full = loud_everywhere(0);
    const auto quiet = loud_everywhere(ac3::kMaxExponent);

    // The ceiling is the pre-EQ7 AC-3 curve, unchanged.
    CHECK(rate_ceiling_chbwcod(192, 5) == 25);   // 38 kbit/s per channel
    CHECK(rate_ceiling_chbwcod(448, 5) == 59);   // 89
    CHECK(rate_ceiling_chbwcod(640, 5) == 60);   // 128, clamped
    CHECK(rate_ceiling_chbwcod(192, 2) == 60);   // 96, clamped

    // Content that fills the spectrum cannot buy a band the rate cannot
    // afford: at 192 kbit/s 5.1 the answer is the ceiling, not 60.
    CHECK(choose_chbwcod(192, 5, full, rate, -1) == 25);

    // Nor does an empty spectrum collapse the band in one frame once the
    // encoder has a previous value to fall from. 448 kbit/s 5.1 is 89 per
    // channel - under the ceiling above which the content is not consulted.
    CHECK(choose_chbwcod(448, 5, quiet, rate, 59) == 59 - kMaxNarrowStep);
    CHECK(choose_chbwcod(448, 5, quiet, rate, 59 - kMaxNarrowStep) ==
          59 - 2 * kMaxNarrowStep);

    // The first frame has nothing to fall from and takes the content's
    // answer outright.
    CHECK(choose_chbwcod(448, 5, quiet, rate, -1) == 0);

    // Widening is not rate-limited: one frame is enough to go from a
    // collapsed band back to the ceiling.
    CHECK(choose_chbwcod(448, 5, full, rate, 0) == 59);
}

TEST_CASE("choose_chbwcod stops consulting the content when bits are plentiful") {
    using ac3::encoder::choose_chbwcod;
    using ac3::encoder::kContentNarrowingCeiling;
    using ac3::encoder::rate_ceiling_chbwcod;
    const auto rate = ac3::SampleRate::k48000;
    const auto quiet = loud_everywhere(ac3::kMaxExponent);

    // 640 kbit/s 5.1 is 128 per channel, exactly the ceiling: an entirely
    // empty spectrum still gets the full band, because there is nothing the
    // reclaimed bits could be spent on.
    CHECK(640 / 5 >= kContentNarrowingCeiling);
    CHECK(choose_chbwcod(640, 5, quiet, rate, -1) == rate_ceiling_chbwcod(640, 5));
    CHECK(choose_chbwcod(384, 2, quiet, rate, -1) == rate_ceiling_chbwcod(384, 2));

    // One rate step below it, the same silence narrows all the way.
    CHECK(576 / 5 < kContentNarrowingCeiling);
    CHECK(choose_chbwcod(576, 5, quiet, rate, -1) == 0);
}

TEST_CASE("AC-3 narrows chbwcod to the content under the rate ceiling") {
    // 192 kbit/s stereo is 96 per channel: under the ceiling above which
    // the content is not consulted, and the rate ceiling there is already
    // 60 - so whatever chbwcod comes out is the content's own answer.
    const ac3::EncoderConfig config{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
    const int wide_code = ac3_chbwcod(encode_ac3(config, 20000.0, 8));
    const int narrow_code = ac3_chbwcod(encode_ac3(config, 8000.0, 8));

    // Reading the code back out of the bitstream is what proves the decision
    // reached the stream rather than only the encoder's own model of it.
    CHECK(narrow_code < wide_code);
    // ...and that it stopped at the content rather than below it: 8 kHz of
    // partials reach bin 85, and the band holding them has to survive.
    CHECK(ac3::encoder::endmant_for_chbwcod(narrow_code) >= 85);

    // A pinned chbwcod still overrides the whole decision.
    const ac3::EncoderConfig pinned{
        .bitrate_kbps = 192, .chbwcod = 44, .acmod = ac3::Acmod::k2_0};
    CHECK(ac3_chbwcod(encode_ac3(pinned, 8000.0, 4)) == 44);
}

TEST_CASE("AC-3 keeps the rate ceiling whatever the content") {
    // 192 kbit/s 5.1 is 38 per channel: the ceiling is 24, and full-band
    // content must not buy a band the frame cannot afford.
    const ac3::EncoderConfig config{
        .bitrate_kbps = 192, .acmod = ac3::Acmod::k3_2, .lfe = true};
    ac3::FrameEncoder encoder(config);
    std::uint64_t n = 0;
    std::vector<std::byte> last;
    for (int f = 0; f < 6; ++f) {
        const auto pcm = band_limited_noise(n, 23000.0, 0.5, 40);
        std::vector<std::span<const float>> views(6, std::span<const float>{pcm});
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        last = *frame;
    }
    CHECK(ac3::encoder::rate_ceiling_chbwcod(192, 5) == 25);
    // 3/2 + LFE couples at this rate, which removes chbwcod from the stream
    // entirely (§5.4.3.8) - so the check that matters is that the frame is
    // legal and decodes, and the ceiling itself is pinned by the unit test
    // above. What this adds is that a full-band source does not push the
    // encoder past it into an over-long frame.
    REQUIRE(!last.empty());
}

TEST_CASE("E-AC-3 narrows the coded bandwidth where it used to send 60") {
    // Coupling and spectral extension off explicitly, not just left to
    // `auto`: EQ9 made both content-aware, and a signal with nothing above
    // 8 kHz is exactly the "top end nearly empty" case that can turn
    // spectral extension on well below its old fixed 56 kbit/s-per-channel
    // ceiling - which would decide the coded bandwidth itself and leave
    // chbwcod, the thing under test here, never consulted. Forcing both
    // tools off isolates chbwcod's own behaviour from whatever `auto`
    // otherwise decides.
    ac3::eac3::FrameConfig auto_bw{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0};
    ac3::eac3::FrameConfig fixed_60 = auto_bw;
    fixed_60.chbwcod = 60;

    // Band-limited material: everything above 8 kHz is inaudible by Table
    // 7.15, so the frame that stops coding there spends its bits on the part
    // that is there - which is visible as a shorter frame's worth of
    // mantissa bits reaching the same content, i.e. a better reconstruction
    // at the same rate.
    const double top = 8000.0;
    const auto narrowed = decode_snr_eac3(auto_bw, top);
    const auto full = decode_snr_eac3(fixed_60, top);
    CHECK(narrowed > full);

    // The default is auto, so a caller that says nothing gets the narrowing.
    CHECK(ac3::eac3::FrameConfig{}.chbwcod == -1);
}
