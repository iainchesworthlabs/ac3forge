#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/dsp/qmf.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/export.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

// Dolby Atmos in Dolby Digital Plus: objects in, one ordinary-looking 5.1
// E-AC-3 stream out.
//
// The whole trick is that there is only one audio payload. Objects are panned
// into a 5.1 bed the way they always were, and that bed is what a legacy
// decoder plays - unchanged, at full level, complete. Alongside it ride two
// pieces of side data in an EMDF container: OAMD saying where each object is,
// and JOC saying how to pull the objects back out of the bed. A decoder that
// knows about neither is not merely tolerated, it is the design target.
//
// What JOC can and cannot do follows from that. The reconstruction is a linear
// combination of the five bed channels, so objects that landed on the same
// channels with the same gains cannot be separated again, however different
// they sounded going in. Elevation in particular costs nothing in the bed -
// the ring has no height speakers - so two objects at one azimuth and
// different heights are indistinguishable in the downmix, and the matrix can
// only split their shared energy in proportion to how loud each one is. That
// is not a defect of this encoder; it is what a parametric object coder is.

namespace ac3::oba {

struct AtmosConfig {
    SampleRate sample_rate = SampleRate::k48000;
    // The metadata competes with the mantissas for the same frame, so an
    // object stream needs headroom a plain 5.1 stream does not.
    std::uint32_t bitrate_kbps = 448;
    int dialnorm = 31;
    // Index into joc::kNumBands (Table 50). Nine bands resolve the spectrum
    // about as finely as a five-channel downmix can justify; more bands cost
    // codewords without giving the matrix anything new to say.
    int num_bands_idx = 4;
    // §6.3.3.7's finer quantizer: half the step, roughly one more bit per
    // coefficient. Worth it when objects are nearly degenerate and the matrix
    // entries are large.
    bool fine_quant = false;
    // Whether to emit the EMDF object container (OAMD + JOC) into block 0's
    // skip field. On by default: object-aware decoders get the objects, and a
    // decoder that ignores the container plays the 5.1 bed underneath it - the
    // design target described above.
    //
    // Turn it OFF to omit the container entirely. That is the only way to keep
    // the 5.1 bed playable on a decoder that VALIDATES the emdf_protection
    // field (TS 102 366 §H.2.2.4 leaves its contents "implementation dependent
    // and not defined", so a third-party encoder cannot satisfy such a check):
    // such a decoder treats the container's sync word as a commitment to object
    // decoding and refuses the whole stream if the field does not validate,
    // rather than falling back. With no container there is no sync word to find,
    // so it decodes the bed as ordinary 5.1. The choice is objects-or-nothing,
    // never both - which is why turning this off also drops TS 103 420 §8.3.1's
    // addbsi object marker (flag_ec3_extension_type_a and §8.3.2.2's complexity
    // index): that marker is what every reader keys an object layer off
    // (ac3::io::scan, the dec3 box's Atmos extension, an HLS CHANNELS=.../JOC
    // attribute, FFmpeg's "Dolby Digital Plus + Dolby Atmos" profile), and a
    // stream with no container has no object layer to advertise. The 5.1 MIX is
    // the same either way (the same float bed is encoded); the decoded samples
    // are not bit-identical across the two, because the frame's rate control
    // gives the freed skip-field and addbsi bytes back to the mantissas, so the
    // bed here is encoded at slightly higher fidelity. See encode_frame().
    bool emit_object_metadata = true;
    // §7.9.4 fast N/4-FFT forward MDCT (see mdct.hpp's mdct512_forward), on
    // by default - see eac3::FrameConfig::fast_mdct, which is what the bed's
    // own independent substream actually reads; this also drives the
    // band_energy transforms behind the JOC reconstruction-matrix solve, so
    // the whole object encode rides one transform path. false forces the
    // direct §8.2.3.2 reference form everywhere, for validation.
    bool fast_mdct = true;
    // Which domain the reconstruction matrix is ESTIMATED in - and so, in
    // practice, which domain it is correct in. §6.6.6 puts the decoder's
    // reconstruction in §7.1's 64-band complex QMF, and every licensed
    // decoder runs it there, so joc::Domain::kQmf is the only setting whose
    // matrices mean on the other side what they meant here.
    // joc::Domain::kMdctBand is what this encoder did before the filterbank
    // existed: the same solve over 256 MDCT bins, four to a subband. It is
    // cheaper, and it is what the in-repo decoder reconstructs best from
    // when it is also told kMdctBand, but the agreement is between this
    // encoder and this decoder only - a licensed decoder has no such
    // setting, and reads every matrix as a QMF one.
    //
    // Default kQmf since the evidence was measured: +5.1 dB mean per-object
    // SNR through a QMF reconstruction, for +0.26 ms/frame of encode
    // (0.55 -> 0.80 ms of a 32 ms budget, four objects).
    joc::Domain joc_domain = joc::Domain::kQmf;
};

// One object's placement for one frame. Positions are room-anchored per
// §4.2.1; the gain is linear and applies to the bed as well as being sent as
// object_gain, because the object and its contribution to the downmix have to
// agree or the reconstruction is scaled wrong.
struct ObjectPlacement {
    Position position{};
    double gain = 1.0;
    // Objects never reach the LFE by panning (there is no direction that
    // points at it), so this is the only route - and it is deliberately not
    // part of the JOC matrix, because §6.3.2.2 bypasses the LFE entirely.
    double lfe_send = 0.0;

    // --- extent and rendering constraints (TS 103 420 §5.6.1) --------------
    // These reach the OAMD payload and stop there. Nothing in this encoder's
    // own bed render or JOC solve reads them: §4.3 makes the renderer, not
    // the decoder, responsible for turning an extent into loudspeaker feeds,
    // and the 5.1 downmix here is a point-source VBAP pan by construction. So
    // a sized object is transmitted as sized and rendered by this encoder as
    // a point - which is the honest split, since a downmix that spread the
    // object would then be spread AGAIN by the receiving renderer.
    //
    // §5.6.1.2. A point source at 0/0/0, which is the default Table 29 gives.
    ObjectSize size{};
    // §5.6.1.5.1 b_object_snap - what ADM calls channelLock: render the
    // object to its nearest speaker rather than panning between speakers.
    bool snap = false;
    // §5.6.1.6.1 Table 20: which horizontal zones the renderer may use.
    ZoneConstraint zone = ZoneConstraint::kNone;
    // §5.6.1.6.2 Table 21: false excludes the Top-Bottom zone, holding the
    // object on the listener plane however high its z says it is.
    bool enable_elevation = true;
};

class AC3FORGE_EXPORT AtmosEncoder {
   public:
    AtmosEncoder(const AtmosConfig& config, int objects);
    // Move-only: encoder_ below is, since eac3::FrameEncoder went move-only
    // - see AccessUnitEncoder's own comment for the dllexport reason this
    // must be spelled out.
    AtmosEncoder(AtmosEncoder&&) noexcept = default;
    AtmosEncoder& operator=(AtmosEncoder&&) noexcept = default;

    // objects: one kSamplesPerFrame mono span per object, in the order the
    // encoder was constructed with. Returns one E-AC-3 access unit: a single
    // independent substream carrying the 5.1 bed, with the EMDF container in
    // its aux data.
    [[nodiscard]] std::expected<eac3::AccessUnit, FrameError> encode_frame(
        std::span<const std::span<const float>> objects,
        std::span<const ObjectPlacement> placement);

    // Dynamic objects only. The program has one more - the bed's LFE - which
    // is what the free object_count(Program) counts.
    [[nodiscard]] int dynamic_object_count() const { return objects_; }
    [[nodiscard]] const Program& program() const { return program_; }

    // The 5.1 bed the last frame encoded, in AC-3 coded order (L, C, R, Ls,
    // Rs, LFE). Exposed because it is what a legacy decoder hears, and that
    // is the thing most worth checking.
    [[nodiscard]] std::span<const std::vector<float>> bed() const { return bed_; }
    // The reconstruction matrix the last frame transmitted, before
    // quantization. Its channel axis is JOC's order (Table 53), not AC-3's.
    [[nodiscard]] const joc::FrameParameters& parameters() const { return params_; }

   private:
    AtmosConfig config_;
    int objects_ = 0;
    Program program_{};
    eac3::AccessUnitEncoder encoder_;
    joc::FrameParameters params_{};

    // Per object, its bed gains in JOC channel order plus its LFE send. Kept
    // between frames so the bed can ramp from where the last frame left off.
    std::vector<std::array<double, joc::kNumChannels5X>> gains_;
    std::vector<double> lfe_gains_;
    bool primed_ = false;

    std::vector<std::vector<float>> bed_;
    // One analysis filterbank per object, for joc::Domain::kQmf's band
    // energies. Left empty - and so free - under kMdctBand.
    std::vector<dsp::QmfAnalysis> object_qmf_;
    std::uint64_t frames_ = 0;
};

// Energy of one object per JOC parameter band, over the whole frame - the
// per-object input AtmosEncoder::encode_frame's reconstruction-matrix solve
// consumes. `mapping` is joc::kSubbandToBand's row for the active band count
// (Table 54); `out` receives one energy value per band and must outlive the
// call. `fast` selects the §7.9.4 fast forward MDCT for the internal
// transforms, the same parameter mdct512_forward itself takes and defaulted
// the same way (direct/reference); AtmosEncoder::encode_frame passes its
// AtmosConfig::fast_mdct through here, so an object stream's band energies
// ride the same transform path as its bed. Declared here purely for
// kernel-level benchmarking - it is not part of the object-encoding API
// above and no caller outside this library should need it directly.
AC3FORGE_EXPORT void band_energy(std::span<const float> signal,
                                 std::span<const std::uint8_t, 64> mapping,
                                 std::span<double> out, bool fast = false);

// The same measurement in §7.1's 64-band complex QMF - one subband per
// Table 54 entry instead of four MDCT bins standing in for one, and 24
// timeslots of it per frame instead of six blocks. `analysis` is this
// signal's own filterbank and must be the SAME object frame after frame:
// a filterbank restarted every frame would see 640 samples of ramp-in each
// time and under-read the energy at both ends.
//
// Absolute scale differs from band_energy's and does not matter: the solve
// that consumes these regularizes relative to their own trace, so a common
// factor across all objects cancels out of the matrix entirely.
//
// One approximation is left in, and it is in the timing rather than the
// domain. The filterbank emits the timeslot whose window ENDS on the
// samples just pushed, so a frame's worth of pushes yields the timeslots
// running from kQmfDelaySlots before the frame to kQmfDelaySlots before its
// end - while a decoder applies this frame's matrix to the timeslots that
// reconstruct this frame. Closing that 576-sample gap would need a frame of
// encoder lookahead; what it costs instead is that the energy average is
// taken over a window shifted 576 samples early.
AC3FORGE_EXPORT void qmf_band_energy(std::span<const float> signal,
                                     std::span<const std::uint8_t, 64> mapping,
                                     std::span<double> out, dsp::QmfAnalysis& analysis);

}  // namespace ac3::oba
