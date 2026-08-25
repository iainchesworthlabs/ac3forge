#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac3/dsp/qmf.hpp"
#include "ac3/export.hpp"
#include "ac3/oba/joc_tables.hpp"

// Joint Object Coding - ETSI TS 103 420 clause 6. The tool that gets more
// objects out of a decoder than there are channels in the bitstream.
//
// JOC codes no audio of its own. It carries a matrix: for each output object,
// how much of each downmix channel to take, per QMF parameter band and
// interpolated across the frame. The decoder (§6.6.6) computes
//     object[obj] = sum over channels of downmix[ch] * mix[obj][ch]
// in the complex QMF domain, so the "matrix" is really a set of per-band
// gains, and the whole tool is 5 channels in, up to 16 objects out.
//
// Because the reconstruction is a linear combination of the downmix, objects
// that were mixed into the SAME downmix channels with the same gains cannot be
// pulled apart again. JOC is a parametric approximation, not a lossless
// separation, and its quality depends entirely on how well-separated the
// objects were in the downmix.

namespace ac3::joc {

// Table 47 / Table 48. This encoder only ever writes 5.X - 7.X needs Lb/Rb in
// the downmix, which costs a dependent substream - but a decoder meets all
// five, and Dolby's own DD+ JOC encoder reaches for the phase-shifted 5.X
// variant by default.
inline constexpr int kDmxConfig5X = 0;
inline constexpr int kDmxConfig7X = 1;
inline constexpr int kDmxConfig5XPlus2 = 2;
inline constexpr int kDmxConfig5XPhaseShift = 3;
inline constexpr int kDmxConfig5XPlus2PhaseShift = 4;

inline constexpr int kNumChannels5X = 5;
// Table 48's widest configuration, and so the ceiling every per-channel
// buffer here is sized to.
inline constexpr int kMaxChannels = 7;

// Table 48. 0 for the reserved indices 5..7, which is how a caller tells them
// apart from a real configuration.
[[nodiscard]] constexpr int dmx_channel_count(int dmx_config_idx) {
    constexpr std::array<int, 5> kCounts = {5, 7, 7, 5, 7};
    return (dmx_config_idx >= 0 && dmx_config_idx < 5)
               ? kCounts[static_cast<std::size_t>(dmx_config_idx)]
               : 0;
}

// §7.1: the complex QMF the reconstruction runs in is 64 subbands wide.
inline constexpr int kQmfSubbands = 64;

// §6.4: one 1 536-sample frame is 24 QMF timeslots, which is what §6.6.5's
// interpolation counts in.
inline constexpr int kQmfTimeslots = 24;

// §6.3.2.4: joc_num_objects_bits is 6 bits but capped at 15, so 16 objects.
inline constexpr int kMaxObjects = 16;

// §6.3.4.3: joc_num_dpoints_bits is one bit, so one or two data points.
inline constexpr int kMaxDataPoints = 2;

// §6.2.3/§6.2.4's per-object header. Every field here is transmitted once per
// object, so a frame can legally mix resolutions, quantizers and coding modes
// between its objects - which real streams do.
struct ObjectShape {
    // §6.3.3.4. An absent object contributes no coefficients at all.
    bool present = true;
    // Index into kNumBands (Table 50), not the band count itself.
    int num_bands_idx = 4;  // 9 bands
    // §6.3.3.7. Coarse is 96 quantization steps over the range, fine is 192.
    // Fine halves the step at roughly one extra bit per coefficient.
    bool fine_quant = false;
    // §6.3.3.6. Sparse names one channel per band and gives every other
    // channel a fixed value - and that value is joc_num_quant/2 + 2
    // (§6.6.2), not the quantizer's zero, so the channels it does not name
    // still leak about 0,4 into the object.
    bool sparse = false;
    // §6.3.4.2 Table 52: false is smooth (linear interpolation across the
    // frame), true is steep (a step at joc_offset_ts, no interpolation).
    bool steep = false;
    int data_points = 1;
    // §6.3.4.4, one per data point, in QMF timeslots; steep mode only.
    std::array<int, kMaxDataPoints> offset_ts{};

    [[nodiscard]] int bands() const { return kNumBands[static_cast<std::size_t>(num_bands_idx)]; }
};

// The reconstruction matrix for one frame, in the dequantized units §6.6.4
// produces - a range of roughly [-9,6; 9,5], not a normalized gain.
//
// The layout is [object][data point][channel][band], row-major, which is the
// order joc_data writes it in. `shapes` being empty is the uniform frame -
// every object present, whole-matrix, one smooth data point, sharing
// `num_bands_idx`/`fine_quant` - which is the only shape build_payload
// writes and so the only one AtmosEncoder ever constructs; the four-argument
// at() then degenerates to exactly the [object][channel][band] layout this
// struct has always had.
struct FrameParameters {
    int objects = 0;
    int channels = kNumChannels5X;
    // Index into kNumBands (Table 50), not the band count itself. The
    // frame-wide value: what an object with no entry in `shapes` uses.
    int num_bands_idx = 4;  // 9 bands
    bool fine_quant = false;
    // §6.3.3.3: a splice detector, not a timestamp. It counts frames from 1 to
    // 1023 and wraps to 1; 0 means "first frame, or first after a splice", so
    // the decoder knows joc_mix_mtx_prev is meaningless and must not
    // interpolate from it.
    int seq_count = 0;
    // §6.3.2.2 Table 47, which is also where `channels` comes from.
    int dmx_config_idx = kDmxConfig5X;
    // §6.3.3.2. Parsed and reported, never applied: no clause in TS 103 420
    // says where in the decode chain this gain belongs, and its own stated
    // range does not match the equation as printed (see parse_payload).
    double clip_gain = 1.0;
    // Per-object headers, or empty for a uniform frame - see above.
    std::vector<ObjectShape> shapes{};
    std::vector<double> matrix{};

    [[nodiscard]] int bands() const { return kNumBands[static_cast<std::size_t>(num_bands_idx)]; }

    // This object's own header, or the frame-wide uniform one.
    [[nodiscard]] ObjectShape shape(int object) const {
        if (shapes.empty()) {
            return ObjectShape{.num_bands_idx = num_bands_idx, .fine_quant = fine_quant};
        }
        return shapes[static_cast<std::size_t>(object)];
    }

    // Where this object's coefficients start in `matrix`.
    [[nodiscard]] std::size_t object_offset(int object) const {
        if (shapes.empty()) {
            return static_cast<std::size_t>(object) * static_cast<std::size_t>(channels) *
                   static_cast<std::size_t>(bands());
        }
        std::size_t offset = 0;
        for (int i = 0; i < object; ++i) {
            const auto earlier = shapes[static_cast<std::size_t>(i)];
            if (!earlier.present) {
                continue;
            }
            offset += static_cast<std::size_t>(earlier.data_points) *
                      static_cast<std::size_t>(channels) *
                      static_cast<std::size_t>(earlier.bands());
        }
        return offset;
    }

    [[nodiscard]] std::size_t coefficient_count() const {
        return object_offset(objects);
    }

    [[nodiscard]] std::size_t index_of(int object, int data_point, int channel, int band) const {
        const int object_bands = shape(object).bands();
        return object_offset(object) +
               ((static_cast<std::size_t>(data_point) * static_cast<std::size_t>(channels) +
                 static_cast<std::size_t>(channel)) *
                static_cast<std::size_t>(object_bands)) +
               static_cast<std::size_t>(band);
    }

    [[nodiscard]] double& at(int object, int data_point, int channel, int band) {
        return matrix[index_of(object, data_point, channel, band)];
    }
    [[nodiscard]] double at(int object, int data_point, int channel, int band) const {
        return matrix[index_of(object, data_point, channel, band)];
    }
    [[nodiscard]] double& at(int object, int channel, int band) {
        return matrix[index_of(object, 0, channel, band)];
    }
    [[nodiscard]] double at(int object, int channel, int band) const {
        return matrix[index_of(object, 0, channel, band)];
    }
};

// §6.6.4's quantizer, and its inverse. The step is 820/(4096*(1+fine)) and the
// origin sits at nquant/2, so code nquant/2 is exactly zero gain.
[[nodiscard]] AC3FORGE_EXPORT int quantize(double coefficient, bool fine_quant);
[[nodiscard]] AC3FORGE_EXPORT double dequantize(int code, bool fine_quant);

// One joc() payload (§6.2.1), padded to whole bytes for emdf_payload_size.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_payload(const FrameParameters& params);

// --- Decode ------------------------------------------------------------

// Decode-side inverse of build_payload(), and rather more: all five of
// Table 47's downmix configurations, any clip gain, per-object band count,
// quantizer, sparse-or-whole-matrix mode, interpolation slope and one or two
// data points - every one of which a real DD+ JOC stream from the Dolby
// Encoding Engine uses and none of which this encoder writes. `shapes` is
// always populated on the way out, so a caller never has to guess which of
// them applied. `matrix` comes back already dequantized (§6.6.4's inverse) -
// the caller never sees the wire's Huffman codes.
//
// std::nullopt is left for what genuinely cannot be read: a reserved
// joc_dmx_config_idx (Table 48 gives 5..7 no channel count), a nonzero
// joc_ext_config_idx (Table 49 reserves every value and defines no
// joc_ext_data() syntax, so there is no length to skip), a Huffman codeword
// in neither table, more objects than §6.3.2.4's own cap, and a payload that
// does not end within a byte of where its coefficients do.
[[nodiscard]] AC3FORGE_EXPORT std::optional<FrameParameters> parse_payload(
    std::span<const std::byte> payload);

// --- Audio reconstruction -----------------------------------------------

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

// §6.6.6's reconstruction runs in the 64-band complex QMF of §7.1, and
// ac3::dsp::QmfAnalysis/QmfSynthesis is that filterbank. Domain::kQmf runs
// it there, which is what every licensed decoder does and therefore the
// only domain in which a matrix this encoder estimates means the same thing
// on the other side.
//
// Domain::kMdctBand is the path that predates the filterbank: the same
// per-band linear combination applied in the 512-sample MDCT domain. It
// stays because it is cheaper and because a stream whose matrix was
// estimated in that domain reconstructs best there - but it is an
// approximation twice over. The MDCT is critically sampled and real, so its
// subbands only behave like subbands while neighbouring blocks agree on
// what was done to them; a per-band matrix that changes every frame breaks
// that time-domain alias cancellation and leaves the residue in the output.
// And its 256 bins map to §7.1's 64 subbands only four-to-one, so the
// matrix's time resolution is the 256-sample block rather than the QMF's
// 64-sample timeslot.
//
// Carried frame to frame for one program's worth of reconstruction, the same
// way Eac3Decoder's own overlap-add delay_ is: `bed_history` gives block 0 of
// each frame real pre-roll instead of zero-padding across the frame seam,
// and `object_history` is each object's own overlap-add tail. `previous_*`
// is what §6.6.5's ramp interpolates FROM; a shape mismatch against the
// frame just decoded (object or channel count) is treated exactly like
// FrameParameters::seq_count == 0 - no ramp, this frame's matrix applies to
// the whole frame outright - since there is nothing meaningful to ramp from.
//
// §6.6.5 keeps joc_mix_mtx_prev per QMF SUBBAND rather than per parameter
// band, which is what lets an object change its band count from one frame to
// the next and still ramp; `previous_matrix` follows it, sized
// objects * channels * kQmfSubbands.
//
// One state object serves either domain, and only the members that domain
// uses are ever touched; `qmf` in particular stays null until a kQmf call
// allocates it, so a decoder that never leaves the MDCT path carries none
// of the filterbank's own state.
struct ReconstructionState {
    std::array<std::array<double, 256>, kMaxChannels> bed_history{};
    std::vector<double> previous_matrix{};
    int previous_objects = 0;
    int previous_channels = 0;
    std::vector<std::array<double, 256>> object_history{};

    // reconstruct()'s own per-call scratch (PREfast C6262: stack-declaring
    // these inside the function put it at ~24 KB of stack per call). Reused
    // across every (block, channel)/(block, object) iteration of a call
    // instead, the same reasoning Eac3Decoder's own imdct_scratch_/
    // ecpl_spectrum_*_ members already use - each is fully overwritten
    // before being read, so nothing here needs to persist meaningfully
    // BETWEEN calls the way bed_history/previous_matrix/object_history do.
    std::array<std::array<double, 256>, kMaxChannels> bed_mdct_scratch{};
    std::array<double, 512> time_scratch{};
    // Four windowed blocks, not one (ROADMAP PF5 phase 4c): the bed
    // analysis batches four CHANNELS' forward transforms into one
    // ac3::mdct512_forward_batch4 call, which needs all four windowed
    // blocks to coexist. kNumChannels5X is 5, so a block runs one batch of
    // four plus one ordinary call; lane 0 doubles as the scalar path's own
    // buffer, so this costs 3 x 512 doubles over the previous single one.
    std::array<std::array<double, 512>, 4> windowed_scratch{};
    // Per-object (ROADMAP PF5's batch-axis follow-on): every present
    // object's spectrum/synthesis output now coexists, so the imdct pass
    // can batch four objects at a time (ac3::imdct512_windowed_batch4)
    // instead of running strictly one object at a time - see
    // reconstruct_mdct_band's own object loop (joc.cpp).
    std::array<std::array<double, 256>, kMaxObjects> object_mdct_scratch{};
    std::array<std::array<double, 512>, kMaxObjects> synth_scratch{};

    // --- Domain::kQmf only -------------------------------------------------

    // The filterbank pair - one analysis per downmix channel, one synthesis
    // per object - plus the one timeslot of subband values in flight.
    // Behind a pointer, and built on first use, so the MDCT path does not
    // pay for it.
    struct QmfState {
        std::array<dsp::QmfAnalysis, kNumChannels5X> bed{};
        std::vector<dsp::QmfSynthesis> objects{};
        std::array<std::array<double, dsp::kQmfSubbands>, kNumChannels5X> bed_real{};
        std::array<std::array<double, dsp::kQmfSubbands>, kNumChannels5X> bed_imag{};
        std::array<double, dsp::kQmfSubbands> object_real{};
        std::array<double, dsp::kQmfSubbands> object_imag{};
    };
    std::unique_ptr<QmfState> qmf{};

    // The matrix from TWO frames back. The MDCT path never needs it: every
    // block it emits belongs to the frame being decoded, so previous_matrix
    // is always the right thing to ramp from. The QMF pair's kQmfDelay means
    // the first kQmfDelaySlots timeslots a call emits belong to the PREVIOUS
    // frame's audio and must ramp across the previous frame's own pair -
    // without this they would get a matrix one whole frame too new.
    std::vector<double> older_matrix{};
};

// Reconstructs each JOC object's time-domain audio for one frame from the
// decoded downmix and this frame's parsed JOC parameters.
//
// `bed` must be exactly `params.channels` channels of kSamplesPerFrame
// samples each, in Table 53's JOC channel order (L, R, C, Ls, Rs, and for a
// 7-channel downmix Lb, Rb) - NOT AC-3's Table 5.8 order (L, C, R, Ls, Rs);
// the caller permutes, the same permutation atmos.cpp's AtmosEncoder applies
// on the way in (see its kAc3FromJoc). Returns one waveform per object,
// `params.objects` of them, each kSamplesPerFrame samples, in the SAME order
// build_payload's own `objects`/matrix rows use - which, for a program this
// project's own AtmosEncoder produces (dynamic-object-only with a bypassed
// LFE, no bed), is exactly oba::DecodedProgram::objects' order too. An
// object whose ObjectShape says it is absent this frame comes back silent.
// Spans rather than vectors so the caller's permutation into JOC order is
// a pointer shuffle, not a channel copy.
//
// Table 47's two "90 degree phase shift" configurations are reconstructed
// like their unshifted siblings: the shift belongs to how the downmix was
// BUILT (it buys a better legacy stereo fold-down), and §6.6.6 says nothing
// about undoing it before matrixing. There is no Hilbert filterbank here to
// undo it with either.
//
// The returned audio LAGS `bed` by reconstruction_delay(domain) samples -
// 256 for kMdctBand, 576 for kQmf. Both are the algorithmic delay of the
// transform pair that domain runs, and neither can be shortened; a caller
// comparing the result against a known source has to shift by it, or it
// measures the delay instead of the reconstruction.
//
// `fast_mdct` and `fast_imdct` apply to Domain::kMdctBand only, and do
// nothing under kQmf, whose transform has only the one form. `fast_mdct`
// selects the §7.9.4 fold for the per-block forward analysis of the five
// bed channels; `fast_imdct` selects the same core for step 3 of each
// object's own §7.9.4.1 synthesis inverse - one per object per block, so 96
// of them in a 16-object frame, which is where nearly all of kMdctBand's
// time goes. Both default to the spec's own direct evaluations, the forms
// every fast-path test validates against; Eac3Decoder passes
// DecoderConfig::fast_mdct for the first and DecoderConfig::fast_imdct for
// the second.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::vector<float>> reconstruct(
    std::span<const std::span<const float>> bed, const FrameParameters& params,
    ReconstructionState& state, bool fast_mdct = false, bool fast_imdct = false,
    Domain domain = Domain::kQmf);

}  // namespace ac3::joc
