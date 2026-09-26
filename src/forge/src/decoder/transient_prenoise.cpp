#include "ac3/decoder/transient_prenoise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/internal/decode_scalar.hpp"
#include "transient_prenoise_apply.hpp"

namespace ac3 {

namespace {

// The cross-fades' arithmetic: the decode path's own scalar, as output.cpp's
// per-sample products are - double in the reference build, float for the
// ESP32-S3, and the fixed-point tier's Q7.24, where a float expression would
// be the one place its PCM could differ between two machines.
using Scalar = internal::decode_scalar_t;

// A raised-cosine (Hann-shaped) cross-fade, the family the spec's own text
// names as giving good results (§3.7.2: "standard Hanning windows"). fade_in
// runs 0 -> 1, fade_out is its complement 1 -> 0, both length N - computed in
// double and narrowed once to the scalar, as output.cpp narrows its taps.
template <std::size_t N>
struct CrossFade {
    std::array<Scalar, N> fade_in{};
    std::array<Scalar, N> fade_out{};
    CrossFade() {
        constexpr double kPi = std::numbers::pi;
        for (std::size_t n = 0; n < N; ++n) {
            const double in =
                0.5 * (1.0 - std::cos(kPi * static_cast<double>(n) / static_cast<double>(N - 1)));
            fade_in[n] = static_cast<Scalar>(in);
            fade_out[n] = static_cast<Scalar>(1.0 - in);
        }
    }
};

const CrossFade<static_cast<std::size_t>(kTransientPrenoiseTC1)>& tc1_windows() {
    static const CrossFade<static_cast<std::size_t>(kTransientPrenoiseTC1)> w;
    return w;
}

const CrossFade<static_cast<std::size_t>(kTransientPrenoiseTC2)>& tc2_windows() {
    static const CrossFade<static_cast<std::size_t>(kTransientPrenoiseTC2)> w;
    return w;
}

// One cross-faded sample: the decoded sample and the synthesized one, each
// under its own gain.
float blend(float decoded, Scalar decoded_gain, float synthesized, Scalar synthesized_gain) {
    return static_cast<float>(static_cast<Scalar>(decoded) * decoded_gain +
                              static_cast<Scalar>(synthesized) * synthesized_gain);
}

// The shared arithmetic between apply_transient_prenoise and
// transient_prenoise_range - kept in one place so the two can never drift
// apart on what range a given transloc/translen actually touches.
struct Derived {
    int pnlen = 0;
    int tot_corr_len = 0;
    int start_samp = 0;
    int synth_start = 0;
    int synth_len = 0;
};

Derived derive(int transloc, int translen) {
    Derived d;
    // §3.7.1: "the leading edge of the audio coding block prior to the block
    // containing the transient" - derived from transloc, never transmitted.
    const int transient_block_start = (transloc / kSamplesPerBlock) * kSamplesPerBlock;
    const int aud_blk_samp_loc = transient_block_start - kSamplesPerBlock;
    d.pnlen = transloc - aud_blk_samp_loc;
    d.tot_corr_len = d.pnlen + translen + kTransientPrenoiseTC1;
    d.start_samp = transloc - d.tot_corr_len;
    d.synth_len = 2 * kTransientPrenoiseTC1 + d.pnlen;
    d.synth_start = transloc - 2 * kTransientPrenoiseTC1 - 2 * d.pnlen;
    return d;
}

}  // namespace

TransientPrenoiseRange transient_prenoise_range(int transloc, int translen) {
    const auto d = derive(transloc, translen);
    // The write region ends exactly at transloc (the correction stops right
    // where the transient begins). The read region - the synthesis source -
    // always starts further back than the write region: its 2*TC1 + 2*pnlen
    // exceeds the write region's pnlen + translen + TC1 by TC1 + pnlen -
    // translen, and pnlen is at least a block while translen is at most 255.
    return {.first = std::min(d.synth_start, d.start_samp), .last = transloc};
}

void apply_transient_prenoise(std::span<float> pcm, int transloc, int translen) {
    std::vector<float> synthesis(static_cast<std::size_t>(internal::kTransientPrenoiseMaxSynthesis));
    internal::apply_transient_prenoise(pcm, transloc, translen, synthesis);
}

void internal::apply_transient_prenoise(std::span<float> pcm, int transloc, int translen,
                                        std::span<float> synthesis) {
    constexpr int kTC1 = kTransientPrenoiseTC1;
    constexpr int kTC2 = kTransientPrenoiseTC2;
    const auto d = derive(transloc, translen);
    const int tot_corr_len = d.tot_corr_len;
    const int start_samp = d.start_samp;
    const int synth_start = d.synth_start;
    const int synth_len = d.synth_len;

    // The synthesis buffer: a copy of the clean, already-decoded audio that
    // precedes the pre-noise, used to overwrite it. Its first sample sits
    // 2*TC1 + 2*pnlen before the transient (§3.7.2, Figure E3.2b).
    for (int samp = 0; samp < synth_len; ++samp) {
        synthesis[static_cast<std::size_t>(samp)] = pcm[static_cast<std::size_t>(synth_start + samp)];
    }

    const auto& w1 = tc1_windows();
    const auto& w2 = tc2_windows();

    // Region 1 [0, TC1): cross-fade OUT of the original decoded pre-noise
    // and IN to the synthesis buffer.
    for (int samp = 0; samp < kTC1; ++samp) {
        const auto at = static_cast<std::size_t>(start_samp + samp);
        const auto j = static_cast<std::size_t>(samp);
        pcm[at] = blend(pcm[at], w1.fade_out[j], synthesis[j], w1.fade_in[j]);
    }
    // Region 2 [TC1, tot_corr_len - TC2): straight overwrite - this is the
    // heart of the pre-noise removal, the part neither cross-fade softens. A
    // copy, so exact in every scalar.
    for (int samp = kTC1; samp < tot_corr_len - kTC2; ++samp) {
        pcm[static_cast<std::size_t>(start_samp + samp)] =
            synthesis[static_cast<std::size_t>(samp)];
    }
    // Region 3 [tot_corr_len - TC2, tot_corr_len): cross-fade back IN to the
    // original decoded audio (now past the transient) and OUT of synthesis.
    for (int samp = tot_corr_len - kTC2; samp < tot_corr_len; ++samp) {
        const auto j = static_cast<std::size_t>(samp - (tot_corr_len - kTC2));
        const auto at = static_cast<std::size_t>(start_samp + samp);
        pcm[at] = blend(pcm[at], w2.fade_in[j], synthesis[static_cast<std::size_t>(samp)],
                        w2.fade_out[j]);
    }
}

}  // namespace ac3
