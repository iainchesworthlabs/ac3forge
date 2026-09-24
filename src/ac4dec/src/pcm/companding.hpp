#pragma once

#include <span>

#include "pcm/aspx.hpp"
#include "syntax/channel_elements.hpp"

// The companding tool: ETSI TS 103 190-1 V1.4.1 clause 5.7.5. It expands, in
// the QMF domain, what the encoder compressed: each slot of an A-SPX interval
// is scaled by g(ts) * G, g(ts) = L(ts)^((1 - alpha) / alpha) with L(ts) the
// slot's mean absolute level over the companded subbands, alpha = 0.65 and G
// = 2^(1 / alpha); or by one gain from the interval's average; or not at all.
// The gain depends on the level's scale. The QMF domain runs at the inverse
// transform's, full scale 2^15, where A-SPX's envelopes are measured; the
// levels here are measured against full scale 1.0 (src/ac4dec/ERRATA.md,
// "Companding measures against full scale 1.0").

namespace ac4::detail {

// One channel companding_control() lists, in its order (Part 1 Table 212).
struct CompandingChannel {
    // Q_low_ext, as AspxChannelIo's `ext`: slot aspx::kTsOffsetHfadj + ts
    // holds Q_low's slot ts.
    std::span<QmfValue> ext;
    int sb1 = 0;            // the channel's A-SPX crossover, sbx
    AspxInterval interval;  // the channel's A-SPX interval, in Q_low's slots
};

// Clause 5.7.5.2 over subbands [sb0, sb1) of each channel's interval. sb0 is
// acpl_qmf_band in ASPX_ACPL_1 and 0 otherwise. `full_scale` is the QMF
// domain's value of full scale, which levels are divided by.
void apply_companding(const CompandingControl& control, int sb0, double full_scale,
                      std::span<const CompandingChannel> channels);

}  // namespace ac4::detail
