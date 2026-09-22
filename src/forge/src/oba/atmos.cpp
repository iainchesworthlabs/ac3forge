#include "ac3/oba/atmos.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/dsp/qmf.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/core/eac3_tables.hpp"  // blocks_per_syncframe
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/internal/profiling.hpp"
#include "ac3/latency.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/joc_tables.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/spatial/spatial.hpp"

namespace ac3::oba {

namespace {

constexpr int kChannels = joc::kNumChannels5X;

// AC-3 codes 3/2 as L, C, R, Ls, Rs (Table 5.8) and spatial::PanGains follows
// it. JOC indexes its downmix as L, R, C, Ls, Rs (Table 53). C and R swap.
constexpr std::array<int, kChannels> kAc3FromJoc = {0, 2, 1, 3, 4};

// Regularization for the reconstruction solve, relative to the downmix's own
// energy. Without it, objects that landed on the same bed channels make the
// covariance singular and the matrix runs away to values the quantizer cannot
// express; with it, the solve degrades into splitting their shared energy in
// proportion to their power, which is the right answer to an unanswerable
// question.
constexpr double kRelativeRegularization = 1e-3;
constexpr double kAbsoluteFloor = 1e-20;

// Gauss-Jordan with partial pivoting over a 5x5. Small, symmetric and
// positive definite once regularized, so this is never the interesting part.
[[nodiscard]] bool invert(std::array<std::array<double, kChannels>, kChannels>& m) {
    std::array<std::array<double, kChannels>, kChannels> inverse{};
    for (int i = 0; i < kChannels; ++i) {
        inverse[static_cast<std::size_t>(i)][static_cast<std::size_t>(i)] = 1.0;
    }
    for (int col = 0; col < kChannels; ++col) {
        int pivot = col;
        for (int row = col + 1; row < kChannels; ++row) {
            if (std::abs(m[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)]) >
                std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(col)])) {
                pivot = row;
            }
        }
        // pivot is only ever col itself or a row from the loop above, both
        // bounded to [0, kChannels) by their own for-loop conditions - so
        // pivot is always in range here. MSVC /analyze's C28020 doesn't
        // track that a variable's bound is inherited from the two loop
        // variables it was assigned from, and flags the std::array subscript
        // below as unproven. #pragma warning(suppress: 28020) would silence
        // /analyze too, but it is not a portable pragma - GCC/clang both
        // treat an unrecognized #pragma as -Wunknown-pragmas, and this
        // project builds with -Werror, so emitting it here would fail every
        // non-MSVC leg. The C28020 alert is dismissed separately with this
        // same justification instead.
        assert(pivot >= 0 && pivot < kChannels);
        if (std::abs(m[static_cast<std::size_t>(pivot)][static_cast<std::size_t>(col)]) <
            kAbsoluteFloor) {
            return false;
        }
        std::swap(m[static_cast<std::size_t>(pivot)], m[static_cast<std::size_t>(col)]);
        std::swap(inverse[static_cast<std::size_t>(pivot)], inverse[static_cast<std::size_t>(col)]);

        const double scale =
            1.0 / m[static_cast<std::size_t>(col)][static_cast<std::size_t>(col)];
        for (int k = 0; k < kChannels; ++k) {
            m[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)] *= scale;
            inverse[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)] *= scale;
        }
        for (int row = 0; row < kChannels; ++row) {
            if (row == col) {
                continue;
            }
            const double factor =
                m[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)];
            if (factor == 0.0) {
                continue;
            }
            for (int k = 0; k < kChannels; ++k) {
                m[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] -=
                    factor * m[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)];
                inverse[static_cast<std::size_t>(row)][static_cast<std::size_t>(k)] -=
                    factor * inverse[static_cast<std::size_t>(col)][static_cast<std::size_t>(k)];
            }
        }
    }
    m = inverse;
    return true;
}

// Where a bed channel's audio is folded onto the 5-channel ring for a CBI
// programme's physical downmix - NOT bed_label_position()'s room-cuboid
// coordinates (oamd.hpp is explicit those are for a view to draw with,
// "nothing in encode or decode reads it", and they are not even on the same
// projection pan_room() uses: bed_label_position(kL) sits at 45 degrees
// through pan_room's own atan2(left, forward), not the 30 the ring actually
// puts L at). This table gives the REAL ring azimuth instead, agreeing with
// pan_azimuth's own kSpeakerAzimuthDeg for the five it names directly and
// with spatial::direction_of's Lrs/Rrs/Lw/Rw for the two further pairs
// Table 12 has that direction_of also names. A height pair folds onto its
// underlying horizontal pair's own azimuth - pan_azimuth has no elevation to
// fold with either, the same "no z" rule pan_room documents for a raised
// dynamic object. std::nullopt is the two LFEs: unpanned, fed straight into
// the bed's own LFE channel instead of through this table at all (see
// AtmosEncoder::encode_bed_frame).
[[nodiscard]] std::optional<double> bed_label_azimuth_deg(BedLabel label) {
    switch (label) {
        case BedLabel::kL:
        case BedLabel::kTfl:  return 30.0;
        case BedLabel::kC:    return 0.0;
        case BedLabel::kR:
        case BedLabel::kTfr:  return -30.0;
        case BedLabel::kLs:
        case BedLabel::kTsl:  return 110.0;
        case BedLabel::kRs:
        case BedLabel::kTsr:  return -110.0;
        case BedLabel::kLb:
        case BedLabel::kTbl:  return 150.0;
        case BedLabel::kRb:
        case BedLabel::kTbr:  return -150.0;
        case BedLabel::kLw:   return 60.0;
        case BedLabel::kRw:   return -60.0;
        case BedLabel::kLfe:
        case BedLabel::kLfe2: return std::nullopt;
    }
    return std::nullopt;
}

// Downmix headroom for a bed channel folding onto a ring position another
// bed channel already occupies at unity (a height pair shares its underlying
// pair's azimuth exactly, so e.g. L and Tfl pan identically) - an ordinary
// film-mix fold-down figure, and unlike the reconstruction below, not a
// normative one: TS 103 420 says nothing about how an encoder builds its
// physical bed (see this file's own header comment), and JOC recovers each
// channel at its OWN original level regardless of what this constant is,
// because the energy this scales is the same energy step 3 below
// reconstructs FROM (power = signal * scale^2) - only a legacy 5.1-only
// listener's fold-down balance depends on the actual value.
constexpr double kExtensionDownmixScale = 0.70710678118654752;  // -3 dB

// The base 5.1 ring channels fold in at unity - literally the same audio a
// plain 5.1 mix would carry, physically unattenuated - and only a channel
// ADDED alongside them takes kExtensionDownmixScale's headroom cut.
[[nodiscard]] constexpr bool is_base_ring_label(BedLabel label) {
    return label == BedLabel::kL || label == BedLabel::kC || label == BedLabel::kR ||
          label == BedLabel::kLs || label == BedLabel::kRs;
}

}  // namespace

// Energy of one object per JOC parameter band, over the whole frame.
//
// Nothing about this is normative. TS 103 420 specifies the bitstream and what
// a decoder does with it; how an encoder arrives at the numbers is entirely
// its own business, and §7's QMF is the DECODER's analysis, not a required
// encoder one. So this reuses the transform the encoder already runs on every
// channel: the 512-sample MDCT gives 256 bins across the same band the QMF
// splits into 64 subbands, so four bins fall in each subband exactly, and
// Table 54 groups the subbands into parameter bands from there.
//
// Given external linkage (declared in atmos.hpp) rather than staying
// anonymous-namespace-local like `invert` above it: kernel-level
// benchmarking needs to call this in isolation. It is not part of the
// object-encoding API AtmosEncoder exposes and no caller outside this
// library should need it directly.
void band_energy(std::span<const float> signal, std::span<const std::uint8_t, 64> mapping,
                 std::span<double> out, bool fast) {
    AC3_ZONE_SCOPED_N("band_energy");
    std::ranges::fill(out, 0.0);
    // The frame's own blocks, without the previous frame's overlap: this is
    // an energy estimate, not a transform that has to reconstruct. Counted
    // from the signal itself, not kBlocksPerFrame - a short syncframe
    // (§E2.3.1.4) hands this 1, 2 or 3 blocks, and walking six would read
    // past its end.
    const int blocks = static_cast<int>(signal.size()) / kSamplesPerBlock;
    for (int block = 0; block < blocks; ++block) {
        std::array<double, 512> time{};
        for (int n = 0; n < 512; ++n) {
            const int index = block * 256 + n - 256;
            time[static_cast<std::size_t>(n)] =
                index < 0 ? 0.0
                          : static_cast<double>(signal[static_cast<std::size_t>(index)]);
        }
        std::array<double, 512> windowed{};
        apply_analysis_window(time, windowed);
        std::array<double, 256> coeffs{};
        mdct512_forward(windowed, coeffs, fast);
        for (int bin = 0; bin < 256; ++bin) {
            const auto band = mapping[static_cast<std::size_t>(bin / 4)];
            out[band] += coeffs[static_cast<std::size_t>(bin)] *
                         coeffs[static_cast<std::size_t>(bin)];
        }
    }
}

void qmf_band_energy(std::span<const float> signal, std::span<const std::uint8_t, 64> mapping,
                     std::span<double> out, dsp::QmfAnalysis& analysis) {
    AC3_ZONE_SCOPED_N("qmf_band_energy");
    std::ranges::fill(out, 0.0);
    std::array<double, dsp::kQmfSubbands> real{};
    std::array<double, dsp::kQmfSubbands> imag{};
    // The signal's own length, not a fixed kQmfSlotsPerFrame: a short
    // syncframe (§E2.3.1.4) hands this 4, 8 or 12 hops instead of 24, and
    // walking the full 24 would read past its end.
    const int slots = static_cast<int>(signal.size()) / dsp::kQmfHop;
    for (int slot = 0; slot < slots; ++slot) {
        const std::span<const float, dsp::kQmfHop> hop{
            signal.data() + slot * dsp::kQmfHop, static_cast<std::size_t>(dsp::kQmfHop)};
        analysis.push(hop, real, imag);
        for (int k = 0; k < dsp::kQmfSubbands; ++k) {
            // Complex magnitude squared: the subband is oversampled, so this
            // is the band's short-time power directly, with none of the
            // MDCT's sign-and-phase dependence on where the block boundary
            // happened to fall.
            out[mapping[static_cast<std::size_t>(k)]] +=
                real[static_cast<std::size_t>(k)] * real[static_cast<std::size_t>(k)] +
                imag[static_cast<std::size_t>(k)] * imag[static_cast<std::size_t>(k)];
        }
    }
}

// Every private data member, following the same pimpl pattern as
// ac3::io::WavStreamReader/Writer and ac3::FrameEncoder.
struct AtmosEncoder::Impl {
    AtmosConfig config_;
    int objects_ = 0;
    // How many pan-and-reconstruct essences one encode call feeds: objects_
    // itself for a dynamic-object programme, or the bed's own non-LFE
    // channel count for a CBI one (the LFE never goes through this pipeline
    // either way - see joc_object_count's own §6.3.2.2 bypass). Always equal
    // to joc_object_count(program_).
    int essences_ = 0;
    Program program_{};
    eac3::AccessUnitEncoder encoder_;
    joc::FrameParameters params_{};

    // Per essence, its bed gains in JOC channel order plus its LFE send. Kept
    // between frames so the bed can ramp from where the last frame left off.
    // A CBI programme's own target never changes frame to frame (see
    // bed_pan_ below), so this settles after frame 1's priming and every
    // later ramp is a no-op "already there" - the same machinery, not a
    // special case.
    std::vector<std::array<double, joc::kNumChannels5X>> gains_;
    std::vector<double> lfe_gains_;
    bool primed_ = false;

    // 256 * blocks_per_syncframe(config_.numblkscod): every per-frame buffer
    // and ramp below runs to this, not to kSamplesPerFrame. Declared before
    // bed_ so the constructor's init list can size bed_ from it.
    int frame_samples_ = kSamplesPerFrame;
    std::vector<std::vector<float>> bed_;
    // One analysis filterbank per essence, for joc::Domain::kQmf's band
    // energies. Left empty - and so free - under kMdctBand.
    std::vector<dsp::QmfAnalysis> object_qmf_;
    std::uint64_t frames_ = 0;

    // --- CBI bed mode only (BedProgram constructor) --------------------
    // bed_labels(program_.bed) index that is the LFE, if the bed has one -
    // encode_bed_frame feeds it straight into bed_[5], never through
    // bed_pan_/the JOC pipeline (§6.3.2.2 bypasses it, same as a dynamic
    // programme's LFE). Every OTHER bed_labels() index maps, in order, onto
    // bed_pan_/bed_scale_ below.
    std::optional<std::size_t> bed_lfe_index_{};
    // Fixed pan (JOC channel order, see kAc3FromJoc) and downmix scale per
    // essence, resolved once here from each bed channel's own speaker label
    // (bed_label_azimuth_deg) rather than supplied per frame the way
    // encode_frame's ObjectPlacement is - a bed channel's position comes
    // from its label, never from an argument (TS 103 420 §5.5.9).
    std::vector<std::array<double, joc::kNumChannels5X>> bed_pan_;
    std::vector<double> bed_scale_;

    Impl(const AtmosConfig& config, int objects)
        : config_(config),
          objects_(objects),
          essences_(objects),
          program_{.dynamic_only = true, .lfe = true, .dynamic_objects = objects},
          encoder_(eac3::AccessUnitConfig{
              .independent = {.sample_rate = config.sample_rate,
                              .bitrate_kbps = config.bitrate_kbps,
                              .acmod = Acmod::k3_2,
                              .lfe = true,
                              .numblkscod = config.numblkscod,
                              .dialnorm = config.dialnorm,
                              .fast_mdct = config.fast_mdct,
                              // §8.3.1's flag_ec3_extension_type_a plus §8.3.2.2's
                              // complexity index - the object count, bed included.
                              // Only when the container is actually emitted: this
                              // marker is what a reader keys "this stream has an
                              // object layer" off (ac3::io::scan, the dec3 box's
                              // Atmos extension, HLS CHANNELS=.../JOC, FFmpeg's
                              // "Dolby Digital Plus + Dolby Atmos" profile), so
                              // writing it into a bed51 stream would advertise
                              // objects that were never encoded - the same
                              // objects-or-nothing rule encode_frame() applies to
                              // the container itself.
                              .oba_complexity_index =
                                  config.emit_object_metadata
                                      ? std::optional<int>{object_count(program_)}
                                      : std::nullopt}}),
          gains_(static_cast<std::size_t>(objects)),
          lfe_gains_(static_cast<std::size_t>(objects), 0.0),
          // A short syncframe (AtmosConfig::numblkscod 0-2) shortens the
          // whole object pipeline with it: the bed, the per-object input
          // spans, the OAMD ramp and the JOC interpolation window all cover
          // frame_samples_, not a fixed kSamplesPerFrame.
          frame_samples_(eac3::blocks_per_syncframe(config.numblkscod) * kSamplesPerBlock),
          bed_(6, std::vector<float>(static_cast<std::size_t>(frame_samples_))) {
        // §5.6.4.8 orders the program's objects bed-first, and the bed here is
        // the LFE alone - so program object 0 is the LFE and the dynamic
        // objects follow it. §6.3.2.2 bypasses the LFE rather than matrixing
        // it, so it costs no JOC output and JOC object j is program object
        // j + 1.
        params_.objects = joc_object_count(program_);
        params_.channels = kChannels;
        params_.num_bands_idx = config.num_bands_idx;
        params_.fine_quant = config.fine_quant;
        params_.matrix.assign(params_.coefficient_count(), 0.0);
        if (config.joc_domain == joc::Domain::kQmf) {
            object_qmf_.resize(static_cast<std::size_t>(objects));
        }
    }

    // CBI: dynamic_objects is always 0, and object_count(program_) - the
    // encoder_ complexity index and params_.objects below both read it via
    // the same free functions the dynamic-object constructor uses - is the
    // bed's own channel_count(bed), so this needs no field this struct does
    // not already have.
    Impl(const AtmosConfig& config, BedProgram bed)
        : config_(config),
          objects_(0),
          essences_(joc_object_count(Program{.dynamic_only = false, .bed = bed.bed})),
          program_{.dynamic_only = false, .bed = bed.bed, .dynamic_objects = 0},
          encoder_(eac3::AccessUnitConfig{
              .independent = {.sample_rate = config.sample_rate,
                              .bitrate_kbps = config.bitrate_kbps,
                              .acmod = Acmod::k3_2,
                              .lfe = true,
                              .numblkscod = config.numblkscod,
                              .dialnorm = config.dialnorm,
                              .fast_mdct = config.fast_mdct,
                              .oba_complexity_index =
                                  config.emit_object_metadata
                                      ? std::optional<int>{object_count(program_)}
                                      : std::nullopt}}),
          gains_(static_cast<std::size_t>(essences_)),
          lfe_gains_(static_cast<std::size_t>(essences_), 0.0),
          frame_samples_(eac3::blocks_per_syncframe(config.numblkscod) * kSamplesPerBlock),
          bed_(6, std::vector<float>(static_cast<std::size_t>(frame_samples_))),
          bed_pan_(static_cast<std::size_t>(essences_)),
          bed_scale_(static_cast<std::size_t>(essences_)) {
        params_.objects = joc_object_count(program_);
        params_.channels = kChannels;
        params_.num_bands_idx = config.num_bands_idx;
        params_.fine_quant = config.fine_quant;
        params_.matrix.assign(params_.coefficient_count(), 0.0);
        if (config.joc_domain == joc::Domain::kQmf) {
            object_qmf_.resize(static_cast<std::size_t>(essences_));
        }

        // Walk the bed's own channel order once, splitting it into the LFE
        // (if any) and every other label's fixed pan/scale - the same order
        // encode_bed_frame's own `channels` argument must arrive in, and the
        // same order build_payload's anchored-object loop assumes.
        std::size_t essence = 0;
        std::size_t index = 0;
        for (const auto label : bed_labels(program_.bed)) {
            const auto azimuth = bed_label_azimuth_deg(label);
            if (!azimuth.has_value()) {
                assert(!bed_lfe_index_.has_value() &&
                       "a second bed LFE has no home in a 6-channel physical bed");
                bed_lfe_index_ = index;
                ++index;
                continue;
            }
            const auto ring = spatial::pan_azimuth(*azimuth);
            const double scale = is_base_ring_label(label) ? 1.0 : kExtensionDownmixScale;
            for (int channel = 0; channel < kChannels; ++channel) {
                bed_pan_[essence][static_cast<std::size_t>(channel)] =
                    ring[static_cast<std::size_t>(kAc3FromJoc[static_cast<std::size_t>(channel)])];
            }
            bed_scale_[essence] = scale;
            ++essence;
            ++index;
        }
        assert(essence == static_cast<std::size_t>(essences_));
    }

    // Shared by encode_frame (whose step 1 resolves `pan`/`target`/`scale`/
    // `target_lfe` from this call's ObjectPlacement) and encode_bed_frame
    // (whose bed_pan_/bed_scale_ above are fixed at construction instead):
    // renders `essences_` sources into the 5.1 bed_ and solves this frame's
    // JOC reconstruction matrix into params_. `audio` is one frame per
    // essence, in the same order as `pan`/`target`/`scale`/`target_lfe`.
    //
    // Does NOT advance gains_/lfe_gains_ to `target`/`target_lfe` - the
    // caller does that itself, and only once encode_access_unit has actually
    // succeeded (see encode_frame's own tail), so a failed frame does not
    // silently consume its own ramp step.
    void render_and_reconstruct(std::span<const std::span<const float>> audio,
                                std::span<const std::array<double, joc::kNumChannels5X>> pan,
                                std::span<const std::array<double, joc::kNumChannels5X>> target,
                                std::span<const double> scale,
                                std::span<const double> target_lfe);
};

AtmosEncoder::~AtmosEncoder() = default;
AtmosEncoder::AtmosEncoder(AtmosEncoder&&) noexcept = default;
AtmosEncoder& AtmosEncoder::operator=(AtmosEncoder&&) noexcept = default;

LatencyBudget AtmosEncoder::latency() const {
    LatencyBudget budget = bed_latency();
    if (impl_->config_.emit_object_metadata) {
        budget.transform_samples += joc::reconstruction_delay(impl_->config_.joc_domain);
    }
    return budget;
}
LatencyBudget AtmosEncoder::bed_latency() const { return impl_->encoder_.latency(); }
int AtmosEncoder::dynamic_object_count() const { return impl_->objects_; }
const Program& AtmosEncoder::program() const { return impl_->program_; }
std::span<const std::vector<float>> AtmosEncoder::bed() const { return impl_->bed_; }
const joc::FrameParameters& AtmosEncoder::parameters() const { return impl_->params_; }

AtmosEncoder::AtmosEncoder(const AtmosConfig& config, int objects)
    : impl_(std::make_unique<Impl>(config, objects)) {}

AtmosEncoder::AtmosEncoder(const AtmosConfig& config, BedProgram bed)
    : impl_(std::make_unique<Impl>(config, bed)) {}

void AtmosEncoder::Impl::render_and_reconstruct(
    std::span<const std::span<const float>> audio,
    std::span<const std::array<double, joc::kNumChannels5X>> pan,
    std::span<const std::array<double, joc::kNumChannels5X>> target,
    std::span<const double> scale, std::span<const double> target_lfe) {
    const auto count = static_cast<std::size_t>(essences_);
    const int bands = params_.bands();
    const auto& mapping = joc::kSubbandToBand[static_cast<std::size_t>(config_.num_bands_idx)];
    assert(pan.size() == count && target.size() == count && scale.size() == count &&
          target_lfe.size() == count);

    if (!primed_) {
        gains_.assign(target.begin(), target.end());
        lfe_gains_.assign(target_lfe.begin(), target_lfe.end());
        primed_ = true;
    }

    // --- 2. The bed ---------------------------------------------------------
    // The ramp runs across the WHOLE frame, not per 256-sample block, because
    // both metadata layers say it does: OAMD sends one update per frame with a
    // 1 536-sample ramp_duration, and §6.6.5 interpolates the JOC matrix from
    // the previous frame's across every QMF timeslot in this one. A bed that
    // moved on a different schedule from the matrix that inverts it would
    // leave the reconstruction chasing the downmix. (A CBI caller's target
    // never moves at all - see bed_pan_'s own comment - so this ramp settles
    // to "already there" after frame 1 and costs nothing extra to share.)
    AC3_ZONE_BEGIN(zone_bed, "step2_bed_render");
    for (auto& channel : bed_) {
        std::ranges::fill(channel, 0.0f);
    }
    const int frame_samples = frame_samples_;
    for (std::size_t object = 0; object < count; ++object) {
        const auto& source = audio[object];
        assert(static_cast<int>(source.size()) == frame_samples);
        for (int channel = 0; channel < kChannels; ++channel) {
            const double from = gains_[object][static_cast<std::size_t>(channel)];
            const double to = target[object][static_cast<std::size_t>(channel)];
            if (from == 0.0 && to == 0.0) {
                continue;
            }
            auto& out = bed_[static_cast<std::size_t>(
                kAc3FromJoc[static_cast<std::size_t>(channel)])];
            for (int n = 0; n < frame_samples; ++n) {
                const double g = from + (to - from) * (n + 1) / frame_samples;
                out[static_cast<std::size_t>(n)] += static_cast<float>(
                    g * static_cast<double>(source[static_cast<std::size_t>(n)]));
            }
        }
        if (lfe_gains_[object] != 0.0 || target_lfe[object] != 0.0) {
            auto& lfe = bed_[5];
            for (int n = 0; n < frame_samples; ++n) {
                const double g = lfe_gains_[object] +
                                 (target_lfe[object] - lfe_gains_[object]) *
                                     (n + 1) / frame_samples;
                lfe[static_cast<std::size_t>(n)] += static_cast<float>(
                    g * static_cast<double>(source[static_cast<std::size_t>(n)]));
            }
        }
    }
    AC3_ZONE_END(zone_bed);

    // --- 3. Per-band object energy -----------------------------------------
    std::vector<double> power(count * static_cast<std::size_t>(bands));
    for (std::size_t object = 0; object < count; ++object) {
        const auto slot = std::span{power}.subspan(
            object * static_cast<std::size_t>(bands), static_cast<std::size_t>(bands));
        if (config_.joc_domain == joc::Domain::kQmf) {
            qmf_band_energy(audio[object], mapping, slot, object_qmf_[object]);
        } else {
            band_energy(audio[object], mapping, slot, config_.fast_mdct);
        }
        // The signal being reconstructed is the object AT ITS GAIN, so its
        // power carries the gain squared and the geometry stays in `pan`.
        const double squared = scale[object] * scale[object];
        for (auto& value : slot) {
            value *= squared;
        }
    }

    // --- 4. The reconstruction matrix ---------------------------------------
    // Minimum mean-square estimate of each object from the downmix. With
    // downmix = D s for known panning gains D and objects s of per-band power
    // p, the estimator that minimises the error is
    //     M = P D^T (D P D^T + eps I)^-1
    // which for well-separated objects is just D's left inverse - exact, not
    // approximate, because this encoder built the downmix and knows D exactly
    // rather than having to estimate it from the signals.
    AC3_ZONE_BEGIN(zone_joc_invert, "step4_joc_covariance_invert");
    for (int band = 0; band < bands; ++band) {
        std::array<std::array<double, kChannels>, kChannels> covariance{};
        for (std::size_t object = 0; object < count; ++object) {
            const double p = power[object * static_cast<std::size_t>(bands) +
                                   static_cast<std::size_t>(band)];
            if (p <= 0.0) {
                continue;
            }
            for (int a = 0; a < kChannels; ++a) {
                const double ga = pan[object][static_cast<std::size_t>(a)];
                if (ga == 0.0) {
                    continue;
                }
                for (int b = 0; b < kChannels; ++b) {
                    covariance[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] +=
                        p * ga * pan[object][static_cast<std::size_t>(b)];
                }
            }
        }
        double trace = 0.0;
        for (int c = 0; c < kChannels; ++c) {
            trace += covariance[static_cast<std::size_t>(c)][static_cast<std::size_t>(c)];
        }
        const double epsilon =
            std::max(kRelativeRegularization * trace / kChannels, kAbsoluteFloor);
        for (int c = 0; c < kChannels; ++c) {
            covariance[static_cast<std::size_t>(c)][static_cast<std::size_t>(c)] += epsilon;
        }
        const bool invertible = invert(covariance);

        for (std::size_t object = 0; object < count; ++object) {
            const double p = power[object * static_cast<std::size_t>(bands) +
                                   static_cast<std::size_t>(band)];
            for (int channel = 0; channel < kChannels; ++channel) {
                double value = 0.0;
                if (invertible && p > 0.0) {
                    for (int k = 0; k < kChannels; ++k) {
                        value += pan[object][static_cast<std::size_t>(k)] *
                                 covariance[static_cast<std::size_t>(k)]
                                           [static_cast<std::size_t>(channel)];
                    }
                    value *= p;
                }
                // The quantizer tops out at about +/-9,6 (§6.6.4). Clamping
                // here rather than letting quantize() do it silently keeps the
                // transmitted matrix and the one this encoder believes it sent
                // the same object.
                params_.at(static_cast<int>(object), channel, band) =
                    std::clamp(value, -9.5, 9.4);
            }
        }
    }
    AC3_ZONE_END(zone_joc_invert);
}

std::expected<eac3::AccessUnit, FrameError> AtmosEncoder::encode_frame(
    std::span<const std::span<const float>> objects,
    std::span<const ObjectPlacement> placement) {
    AC3_ZONE_SCOPED_N("AtmosEncoder::encode_frame");
    assert(impl_->program_.dynamic_only);
    assert(static_cast<int>(objects.size()) == impl_->objects_);
    assert(static_cast<int>(placement.size()) == impl_->objects_);

    const auto count = static_cast<std::size_t>(impl_->objects_);

    // --- 1. Where each object ends the frame ------------------------------
    // Two matrices come out of this and they are deliberately different. The
    // BED gets the panning gains times the object's gain, because that is the
    // mix. The reconstruction solve gets the panning gains alone, and the
    // object's gain is folded into its power instead - so what JOC hands back
    // is the object already at its intended level and object_gain can stay at
    // 0 dB. The alternative, reconstructing the raw essence and sending the
    // gain as metadata, would push it through Table 19's 1 dB steps for no
    // reason.
    std::vector<std::array<double, kChannels>> pan(count);
    std::vector<std::array<double, kChannels>> target(count);
    std::vector<double> scale(count);
    std::vector<double> target_lfe(count);
    for (std::size_t object = 0; object < count; ++object) {
        const auto& place = placement[object];
        const auto ring = spatial::pan_room(place.position.x, place.position.y);
        for (int channel = 0; channel < kChannels; ++channel) {
            const double g =
                ring[static_cast<std::size_t>(kAc3FromJoc[static_cast<std::size_t>(channel)])];
            pan[object][static_cast<std::size_t>(channel)] = g;
            target[object][static_cast<std::size_t>(channel)] = g * place.gain;
        }
        scale[object] = place.gain;
        target_lfe[object] = place.lfe_send * place.gain;
    }

    // --- 2-4. Bed render and JOC reconstruction matrix -----------------------
    impl_->render_and_reconstruct(objects, pan, target, scale, target_lfe);

    // --- 5. Metadata --------------------------------------------------------
    std::vector<DynamicObject> described(count);
    for (std::size_t object = 0; object < count; ++object) {
        described[object].position = placement[object].position;
        // The gain is inside the reconstructed essence (see step 1), so the
        // renderer must not apply it a second time.
        described[object].gain_db = 0.0;
        // Extent and rendering constraints pass straight through to OAMD -
        // see ObjectPlacement's own comment on why the bed render below
        // deliberately does not also act on them.
        described[object].size = placement[object].size;
        described[object].snap = placement[object].snap;
        described[object].zone = placement[object].zone;
        described[object].enable_elevation = placement[object].enable_elevation;
    }
    // §6.3.3.3: 0 marks the first frame, after which the counter runs 1..1023
    // and wraps to 1 rather than to 0 - a decoder reads 0 as a splice and
    // stops interpolating from a matrix that no longer means anything.
    impl_->params_.seq_count =
        impl_->frames_ == 0 ? 0 : static_cast<int>((impl_->frames_ - 1) % 1023 + 1);

    // The container is what carries the objects - and, on a decoder that
    // validates the emdf_protection field, it is also what commits that decoder
    // to object decoding: the moment its sync word is found in the skip field
    // and the container parses, that decoder must accept the protection field or
    // reject the whole access unit; there is no tolerant middle path that keeps
    // the bed. So a stream this encoder cannot make such a field validate for
    // either carries objects (and is refused by that decoder) or omits the
    // container and plays as the 5.1 bed - never both. impl_->config_.emit_object_metadata
    // picks which, for the TS 103 420 §8.3.1 addbsi marker in the constructor as
    // well as for the container here: a bed51 stream advertises no object layer
    // either. The float bed built below (views) is identical regardless; the
    // encoded output is not bit-identical across the two, because dropping the
    // container hands its skip-field bytes back to the mantissas.
    std::vector<std::byte> container;
    if (impl_->config_.emit_object_metadata) {
        const auto oamd = build_payload(impl_->program_, described, impl_->frame_samples_);
        const auto joc_payload = joc::build_payload(impl_->params_);
        const std::array<emdf::Payload, 2> payloads{{
            {.id = emdf::kPayloadIdOamd, .bytes = oamd},
            {.id = emdf::kPayloadIdJoc, .bytes = joc_payload},
        }};
        container = emdf::build_container(payloads);
    }

    // --- 6. The stream ------------------------------------------------------
    std::array<std::span<const float>, 6> views{};
    for (std::size_t channel = 0; channel < views.size(); ++channel) {
        views[channel] = impl_->bed_[channel];
    }
    auto unit = impl_->encoder_.encode_access_unit(views, container);
    if (!unit) {
        return std::unexpected(unit.error());
    }

    impl_->gains_ = target;
    impl_->lfe_gains_ = target_lfe;
    ++impl_->frames_;
    return unit;
}

std::expected<eac3::AccessUnit, FrameError> AtmosEncoder::encode_bed_frame(
    std::span<const std::span<const float>> channels) {
    AC3_ZONE_SCOPED_N("AtmosEncoder::encode_bed_frame");
    assert(!impl_->program_.dynamic_only);
    assert(channels.size() == static_cast<std::size_t>(bed_channel_count(impl_->program_)));

    const auto essences = static_cast<std::size_t>(impl_->essences_);

    // Slice `channels` into the essences_ non-LFE spans render_and_reconstruct
    // expects, in the same order bed_pan_/bed_scale_ were built in - the
    // constructor's own walk of bed_labels(program_.bed), skipping the LFE.
    std::vector<std::span<const float>> audio;
    audio.reserve(essences);
    for (std::size_t index = 0; index < channels.size(); ++index) {
        if (impl_->bed_lfe_index_ == index) {
            continue;
        }
        audio.push_back(channels[index]);
    }
    assert(audio.size() == essences);

    // bed_pan_ scaled by bed_scale_ IS the target, and there is no authored
    // per-frame gain on top of it to fold in separately - recomputed fresh
    // each call rather than cached alongside bed_pan_/bed_scale_, the same
    // choice encode_frame's own `power` (step 3) makes for a per-frame
    // buffer this cheap.
    std::vector<std::array<double, kChannels>> target(essences);
    for (std::size_t essence = 0; essence < essences; ++essence) {
        for (int channel = 0; channel < kChannels; ++channel) {
            target[essence][static_cast<std::size_t>(channel)] =
                impl_->bed_pan_[essence][static_cast<std::size_t>(channel)] *
                impl_->bed_scale_[essence];
        }
    }
    // No essence sends to the LFE by panning - the bed's own LFE channel
    // feeds bed_[5] directly below instead, exactly as a plain 5.1 stream's
    // LFE would.
    const std::vector<double> target_lfe(essences, 0.0);

    impl_->render_and_reconstruct(audio, impl_->bed_pan_, target, impl_->bed_scale_, target_lfe);

    // The bed's own LFE: unpanned, unramped, unity gain - it is not
    // reconstructed by JOC either (§6.3.2.2 bypasses it for a bed programme
    // exactly as it does for a dynamic-object one), so there is no matrix or
    // ramp state for a direct passthrough to disturb.
    if (impl_->bed_lfe_index_.has_value()) {
        const auto& lfe_source = channels[*impl_->bed_lfe_index_];
        assert(static_cast<int>(lfe_source.size()) == impl_->frame_samples_);
        auto& lfe = impl_->bed_[5];
        for (int n = 0; n < impl_->frame_samples_; ++n) {
            lfe[static_cast<std::size_t>(n)] += lfe_source[static_cast<std::size_t>(n)];
        }
    }

    // --- Metadata -------------------------------------------------------
    // No DynamicObject to describe: program_.dynamic_objects is 0, so every
    // one of this programme's objects is anchored to its bed label -
    // build_payload's own anchored-object path, which an empty `objects`
    // span here selects (and which its own assert requires, matching
    // dynamic_objects == 0 exactly).
    impl_->params_.seq_count =
        impl_->frames_ == 0 ? 0 : static_cast<int>((impl_->frames_ - 1) % 1023 + 1);

    std::vector<std::byte> container;
    if (impl_->config_.emit_object_metadata) {
        const auto oamd = build_payload(impl_->program_, {}, impl_->frame_samples_);
        const auto joc_payload = joc::build_payload(impl_->params_);
        const std::array<emdf::Payload, 2> payloads{{
            {.id = emdf::kPayloadIdOamd, .bytes = oamd},
            {.id = emdf::kPayloadIdJoc, .bytes = joc_payload},
        }};
        container = emdf::build_container(payloads);
    }

    std::array<std::span<const float>, 6> views{};
    for (std::size_t channel = 0; channel < views.size(); ++channel) {
        views[channel] = impl_->bed_[channel];
    }
    auto unit = impl_->encoder_.encode_access_unit(views, container);
    if (!unit) {
        return std::unexpected(unit.error());
    }

    impl_->gains_ = target;
    impl_->lfe_gains_ = target_lfe;
    ++impl_->frames_;
    return unit;
}

}  // namespace ac3::oba
