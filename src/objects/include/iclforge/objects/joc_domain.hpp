#pragma once

#include <cstdint>

#include "iclforge/dsp/qmf.hpp"

namespace iclforge::oba::joc {

// Which domain reconstruct() applies the matrix in.
enum class Domain : std::uint8_t {
    // The 512-sample MDCT, four bins to a §7.1 subband. Cheaper, and the
    // domain this project's own encoder estimated its matrices in before
    // the filterbank existed.
    kMdctBand,
    // §7.1's 64-band complex QMF - what §6.6.6 describes and what a
    // licensed decoder runs.
    kQmf,
};

// How far the reconstruction lags the downmix it was given, in samples.
[[nodiscard]] constexpr int reconstruction_delay(Domain domain) {
    return domain == Domain::kQmf ? dsp::kQmfDelay : 256;
}

}  // namespace iclforge::oba::joc
