#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/aspx/frequency_tables.hpp"
#include "iclforge/ac4core/aspx/hf_generator.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "syntax/aspx.hpp"
#include "syntax/context.hpp"

// A-SPX in the QMF domain: ETSI TS 103 190-1 V1.4.1 clause 5.7.6 from the
// parsed aspx_data_1ch() or aspx_data_2ch() to QoutASPX. The envelopes are
// decoded and dequantised (5.7.6.3.4, 5.7.6.3.5), the high band made by the
// core's HF generator (5.7.6.4.1), adjusted to the envelopes with the
// limiter (5.7.6.4.2), given noise and tones (5.7.6.4.3, 5.7.6.4.4),
// assembled (5.7.6.4.5), and interleaved with the waveform-coded components
// (5.7.6.5).
//
// Time runs along Q_low's slots, the QMF matrix delayed by ts_offset_hfgen
// (5.7.6.3.2): QoutASPX is that delayed matrix below the crossover and the
// assembled high band above it, and the part of an interval past the frame's
// last slot waits for the next frame. src/ac4dec/ERRATA.md records the
// readings taken, under "A-SPX".

namespace iclforge::ac4::detail {

// ac4core's own complex type (in place of std::complex<double>): the
// decoder's QMF-domain reconstruction (this file and its neighbours in
// pcm/) calls straight into src/ac4core's A-SPX, A-CPL and A-JOC kernels,
// which take this type since D14a (planning/ac4.md), and a std::complex
// buffer cannot be passed where one of those is asked for even at the same
// underlying double representation - the two are unrelated types.
//
// `Real` (ac4/detail/real.hpp) is the same CMake-selected scalar
// src/ac4core's kernels are explicitly instantiated at, resolved by
// ICLFORGE_DECODE_SCALAR (double or float): one concrete type per build,
// used directly here rather than as a template parameter, the way
// iclforge::ac3's own decode path uses iclforge::ac3::internal::decode_scalar_t (that
// header's own comment explains the distinction). pcm/'s classes and free
// functions are not C++ templates - they call the same one Real every other
// pcm/ file resolves, since a build never needs two at once - so this second
// D14a phase's retemplating is this alias's definition changing plus each
// call site that named `double` directly for a QMF-domain or spectral-domain
// value now naming `Real`, not a cascading template parameter added to every
// signature.
using QmfValue = dsp::Complex<Real>;

// One channel's QMF-domain matrix as the stages after A-SPX take it, slot after slot of 64
// subbands: a view, since a decoded channel's lives in the first slots of its Q_low_ext
// (SubstreamPcm) and the others (objects, a downmix's outputs, copies) in vectors of their own.
using QmfMatrix = std::span<QmfValue>;

// What one channel's A-SPX keeps from one interval to the next.
struct AspxChannelState {
    // Pseudocodes 80 and 81: the last envelopes' quantised scale factors and
    // the last signal envelope's frequency resolution, for delta coding
    // along time across the interval border.
    bool have_previous = false;
    std::array<int, aspx::kMaxSbgMaster> qscf_sig_prev{};
    std::array<int, aspx::kMaxSbgNoise> qscf_noise_prev{};
    int freqres_prev = 0;
    // Pseudocode 92: the previous interval's aspx_tsg_ptr, envelope count and
    // last envelope's sinusoid markers, by QMF subband.
    int tsg_ptr_prev = -1;
    int num_atsg_sig_prev = 1;
    std::array<bool, 64> sine_prev{};
    // Pseudocodes 103 and 105: the last noise and sine table indices used.
    int noise_index = 0;
    int sine_index = 0;
    bool first_frame = true;
    aspx::HfGeneratorState<Real> hf;
    // Pseudocode 106: the assembled high band of the previous interval past
    // its frame's end (Y_prev from num_qmf_timeslots on), y_prev_slots slots
    // of 64 subbands.
    std::vector<QmfValue> y_prev;
    int y_prev_slots = 0;
};

// The frame-level parameters of one aspx_data element.
struct AspxFrame {
    const AspxConfig* config = nullptr;
    int xover_subband_offset = 0;
    bool balance = false;       // aspx_data_2ch()'s aspx_balance
    bool master_reset = false;  // 5.7.6.3.1.1
    bool base_48k = true;       // fs_index 1
    int num_qmf_timeslots = 0;  // Table 189
    int num_ts_in_ats = 0;      // Table 192
    int ts_offset_hfgen = 0;    // Table 192
};

// One channel of the element: its parsed data, its state, and its QMF
// matrices. `ext` is Q_low_ext, kTsOffsetHfadj + ts_offset_hfgen +
// num_qmf_timeslots slots of 64 subbands whose last num_qmf_timeslots are
// this frame's analysis and whose slot kTsOffsetHfadj is Q_low's slot 0,
// companded already. `out` receives QoutASPX, num_qmf_timeslots slots.
struct AspxChannelIo {
    const AspxChannel* data = nullptr;
    AspxChannelState* state = nullptr;
    std::span<const QmfValue> ext;
    std::span<QmfValue> out;
};

// The QMF slots [first, last) of Q_low that `framing`'s interval covers.
struct AspxInterval {
    int first = 0;
    int last = 0;
};
[[nodiscard]] AspxInterval aspx_interval(const AspxFraming& framing, int num_ts_in_ats) noexcept;

// The subband group tables for `frame`, as Pseudocodes 67 to 74 derive them.
[[nodiscard]] ParseResult aspx_tables(const AspxFrame& frame, aspx::SubbandGroups& groups,
                                      aspx::PatchTables& patches);

// What decode_aspx() would refuse, found without decoding: tables no
// stream can use, and interval borders that do not increase or that run past
// Q_low.
[[nodiscard]] ParseResult check_aspx(const AspxFrame& frame,
                                     std::span<const AspxChannel* const> channels);

// The matrices one channel's assembly works in, per envelope and A-SPX subband:
// 16 KB at double and 8 KB at float, too much for the stack, so the caller keeps
// them (SubstreamPcm makes one when its first A-SPX frame needs it). Each channel
// takes them zeroed.
struct AspxScratch {
    using EnvelopeMatrix = std::array<std::array<dsp::Energy<Real>, 64>, kAspxMaxSignalEnvelopes>;
    using FlagMatrix = std::array<std::array<bool, 64>, kAspxMaxSignalEnvelopes>;
    EnvelopeMatrix est_sig{};
    EnvelopeMatrix scf_sig{};
    EnvelopeMatrix scf_noise{};
    FlagMatrix sine_idx{};
    FlagMatrix sine_area{};
    EnvelopeMatrix sine_lev{};
    EnvelopeMatrix noise_lev{};
    EnvelopeMatrix sig_gain{};
};

// Clause 5.7.6 for the one or two channels of an aspx_data element, in `scratch`
// or, without one, in a scratch of its own on the heap.
[[nodiscard]] ParseResult decode_aspx(const AspxFrame& frame, std::span<AspxChannelIo> channels,
                                      AspxScratch& scratch);
[[nodiscard]] ParseResult decode_aspx(const AspxFrame& frame, std::span<AspxChannelIo> channels);

}  // namespace iclforge::ac4::detail
