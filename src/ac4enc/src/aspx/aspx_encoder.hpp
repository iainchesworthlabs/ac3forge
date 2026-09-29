#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "aspx/aspx_syntax.hpp"
#include "aspx/frequency_tables.hpp"
#include "aspx/hf_generator.hpp"
#include "dsp/qmf.hpp"
#include "frame/timing.hpp"

// The encoder's QMF domain: ETSI TS 103 190-1 V1.4.1 clause 5.7 run from the
// other side. Each channel is analysed by the QMF bank the decoder uses, on
// the decoder's slot axis; A-SPX's envelopes, noise floors and inverse
// filtering are estimated there for the band above the crossover, and the
// band below it is compressed (companding's inverse, 5.7.5) and synthesised
// back for the audio spectral frontend to code.
//
// Alignment (frame/timing.hpp). The decoder's QMF analysis of a frame's
// output sees the encoder's delayed signal d_pcm samples later, so its QMF
// slot g covers the 64 samples of that signal from 64 g - d_pcm. A frame's
// A-SPX and companding data reach the QMF domain d_ctrl frames after the
// frame (5.7.2), behind ts_offset_hfgen slots of history (5.7.6.3.2): frame
// f's interval slot i is slot num_qmf_timeslots (f + d_ctrl) -
// ts_offset_hfgen + i, at frame_rate_index 13 32(f + 1) - 6 + i. After the
// analysis bank's group delay those slots centre on frame f's transform
// window.
//
// The compressed low band, synthesised, comes out of the synthesis bank 577
// samples after the analysis bank's input, which runs d_pcm samples ahead of
// the signal: the signal the spectral frontend codes is the synthesis output
// d_pcm + 577 samples on, at index 13 c(s) = y(s + 929). The decoder's
// analysis of c then sees the compressed slots on the same axis.

namespace ac4::detail {

using QmfSample = std::complex<double>;

// At frame_rate_index 13: num_qmf_timeslots and num_aspx_timeslots, the
// analysis bank's lead on the signal (d_pcm), and the lag of the compressed
// signal behind the synthesis output (d_pcm and the banks' 577).
inline constexpr int kQmfSlotsPerFrame = 32;
inline constexpr int kAspxTimeslots = 16;
inline constexpr int kAnalysisLead = 352;
inline constexpr int kCompandedLag = 352 + 577;
// The QMF domain's full scale: the inverse transform's 2^15.
inline constexpr double kQmfFullScale = 32768.0;

// What a stream's A-SPX is configured with: aspx_config() and the crossover,
// and the tables they give, and the frame grid it runs on.
struct AspxSetup {
    AspxConfigFields config;
    int xover_subband_offset = 0;
    bool companding = false;  // the compressor runs, b_compand_on set
    bool base_48k = true;
    FrameTiming timing{};
    // Experimental: VARVAR framing, pairs coded as sum and balance, and
    // frequency interleaved waveform coding (ac4::EncoderConfig::Experimental).
    bool varvar = false;
    bool balance = false;
    bool interleave = false;
    aspx::SubbandGroups groups;
    aspx::PatchTables patches;
    AspxCounts counts;
};

// DEE's configuration for a channel's share of the rate, which this encoder
// takes. In mono and stereo: below 32 kbps a channel the low resolution table
// from 7.5 to 17.25 kHz, below 48 the high resolution one from 10.5 to 21
// kHz, above that from 13.5 to 21 kHz; companding below 64 kbps a channel.
// With `multichannel`, the 5.X and 7.X elements, as DEE's 5.1 streams have
// it: from 38.4 kbps a channel (192 kbps in 5.X) the high resolution table
// from 12 to 21 kHz, and from 51.2 (256 kbps) to 23.25 kHz with the crossover
// a band up, at 12.75 kHz; below 38.4 the mono and stereo tables; never
// companding.
// std::nullopt for a sample rate these do not cover. `timing` is the frame
// grid, frame_rate_index 13's by default.
[[nodiscard]] std::optional<AspxSetup> aspx_setup_for(double kbps_per_channel, int sample_rate_hz,
                                                      bool multichannel = false,
                                                      const FrameTiming& timing = {});

// DEE's configuration in the 5.X element's A-CPL modes, as G0's 5.1 legs have
// it: the high resolution table from subband 32, in ASPX_ACPL_1 and 2 (128 and
// 144 kbps) to 23.25 kHz with the crossover a band up, at 12.75 kHz, and in
// ASPX_ACPL_3 (`coupling`, 96 kbps) to 18.75 kHz from 12 kHz; never
// companding. std::nullopt for a sample rate these do not cover.
[[nodiscard]] std::optional<AspxSetup> aspx_setup_for_acpl(bool coupling, int sample_rate_hz,
                                                           const FrameTiming& timing = {});

// DEE's configuration in the immersive element (ETSI TS 103 190-2 V1.3.1
// clause 6.2.4), as G1's 5.1.4 legs have it, by the rate alone: the high
// resolution table to 21 kHz (aspx_stop_freq 1), from 7.125 kHz below 24.9
// kbps a channel (224 kbps over 5.1.4's nine full-band channels), from 10.5 kHz
// below 33.8 (304 kbps), and from 12.75 kHz above, in ASPX_ACPL_2 at 192, 256 and
// 288 kbps, 320 to 448, and in ASPX_SCPL at 512; never companding.
// std::nullopt for a sample rate these do not cover.
[[nodiscard]] std::optional<AspxSetup> aspx_setup_for_immersive(double kbps_per_channel,
                                                                int sample_rate_hz,
                                                                const FrameTiming& timing = {});

// One channel's QMF domain: the analysis of its signal slot by slot, the
// compressed low band synthesised back, and A-SPX's parameters frame by
// frame, with what the decoder keeps from one frame to the next.
class AspxChannelEncoder {
   public:
    explicit AspxChannelEncoder(const AspxSetup& setup);

    // Analyses slot slots(), from the 64 samples of the signal (full scale
    // 1.0) from 64 slots() - d_pcm.
    void push_slot(std::span<const double> samples);
    [[nodiscard]] long long slots() const noexcept { return first_slot_ + static_cast<long long>(slots_.size()); }

    // Sample s of the signal the spectral frontend codes, with companding:
    // the compressed low band, known for s below 64 slots() - d_pcm - 577.
    // Zero before the first sample kept.
    [[nodiscard]] double companded(long long s) const noexcept;

    // Frame f's A-SPX data, from the slots of its interval, which must have
    // been analysed, with the envelopes' delta coding chosen. Nothing moves
    // on until commit().
    [[nodiscard]] AspxChannelFields propose(long long frame, bool iframe);

    // What a frame whose bits hold no more sends: the last frame's envelopes
    // and inverse filtering again, which cost least to send; with `silent`,
    // or with no last frame, envelopes at F0's smallest value, 64 per QMF
    // subsample, and no noise. One envelope from where the last interval
    // stopped, or from `start` slots into the frame, where the encoder sizes
    // the frame a VARFIX interval gives (Part 1 clause 4.3.10.4).
    [[nodiscard]] AspxChannelFields fallback(bool iframe, bool silent,
                                             std::optional<int> start = std::nullopt) const;

    // Moves what the decoder keeps on to the fields sent for frame f.
    // `balance_values`: the second channel of a pair sent with aspx_balance,
    // whose values count twice (Pseudocodes 80 and 81).
    void commit(long long frame, const AspxChannelFields& sent, bool balance_values = false);

    // This channel and `right`, as proposed, coded as a sum and balance pair
    // instead (aspx_balance, Pseudocode 84): where the two share a framing
    // and inverse filtering, and that takes fewer bits.
    [[nodiscard]] std::optional<std::array<AspxChannelFields, 2>> balanced_with(
        const AspxChannelEncoder& right, const std::array<AspxChannelFields, 2>& proposed, bool iframe) const;

    // Frees the slots and companded samples frame f and later do not need.
    void drop_before_frame(long long frame);

    // The QMF subbands [first, last) of each high resolution group a
    // proposal marks aspx_fic_used_in_sfb: what the spectral frontend codes
    // above the crossover for the frame.
    [[nodiscard]] std::vector<std::pair<int, int>> interleaved_subbands(const AspxChannelFields& fields) const;

   private:
    using Slot = std::array<QmfSample, dsp::kQmfSubbands>;

    [[nodiscard]] const Slot& slot(long long g) const noexcept;
    // Q_low_ext for frame f's interval, from the input's own slots, with
    // `lead` more slots ahead of it.
    void gather(long long frame, std::vector<QmfSample>& ext, int lead = 0) const;
    // Runs the high frequency generator over `ext` from `state`, as over an
    // interval of the frame's slots and `lead` more ahead of them.
    void generate(std::span<const QmfSample> ext, std::span<const std::uint8_t> tna_mode,
                  aspx::HfGeneratorState<double>& state, std::vector<QmfSample>& q_high,
                  int lead = 0) const;
    // Tonality is measured over at least kToneWindow slots of Q_low, ending
    // with the interval's own: below 1 536 samples a frame, over slots of the
    // frames before it as well (tone_lead() of them).
    [[nodiscard]] int tone_lead() const noexcept;
    [[nodiscard]] int tone_samples() const noexcept;
    // The interval's class, borders and transient envelope, from where the
    // last interval stopped and the attacks in the A-SPX band.
    [[nodiscard]] AspxFramingFields choose_framing(std::span<const QmfSample> ext) const;
    // The A-SPX band's energy in A-SPX slot t of Q_low.
    [[nodiscard]] double band_energy(std::span<const QmfSample> ext, int t) const;
    [[nodiscard]] std::vector<int> choose_inverse_filtering(std::span<const QmfSample> ext,
                                                            AspxChannelFields& fields) const;
    void choose_sinusoids(std::span<const QmfSample> ext, AspxChannelFields& fields) const;
    // aspx_fic_used_in_sfb, with interleaving on: the high resolution
    // groups that hold a steady tone the patch does not make, which the
    // spectral frontend then codes; sinusoids take the groups left.
    void choose_interleaving(std::span<const QmfSample> ext, AspxChannelFields& fields) const;
    [[nodiscard]] std::array<bool, dsp::kQmfSubbands> sine_subbands(const AspxChannelFields& fields, int env) const;
    // One envelope's quantised values: its groups' mean energy per QMF
    // subsample over A-SPX slots [first, last), or where a group holds one of
    // `sines`, that subband's; a group of `waveform` subbands, which the
    // spectral frontend codes, none.
    [[nodiscard]] std::vector<int> signal_envelope(std::span<const QmfSample> ext, int first, int last,
                                                   bool high_res, int quant_mode,
                                                   const std::array<bool, dsp::kQmfSubbands>& sines,
                                                   const std::array<bool, dsp::kQmfSubbands>& waveform) const;
    // The last envelope's values put on another envelope's groups, as
    // Pseudocode 80 does for delta coding along time.
    [[nodiscard]] std::vector<int> map_resolution(std::span<const int> previous, bool previous_high,
                                                  bool high) const;
    // Codes quantised values along frequency, or along time from `previous`
    // where `along_time` allows it and that is shorter; with `balance`, in
    // the balance codebooks.
    [[nodiscard]] static AspxEnvelopeFields delta_code(std::span<const int> q, std::span<const int> previous,
                                                       bool along_time, bool signal, int quant_mode,
                                                       bool balance = false);
    // The quantised values of each signal and noise envelope a proposal's
    // fields stand for, from this channel's last values.
    [[nodiscard]] std::vector<std::vector<int>> signal_values(const AspxChannelFields& fields) const;
    [[nodiscard]] std::vector<std::vector<int>> noise_values(const AspxChannelFields& fields) const;
    // The framing, quantisation mode and envelope resolutions of `framing`,
    // for an interval starting where the last stopped.
    [[nodiscard]] AspxChannelFields framed(const AspxFramingFields& framing) const;
    // Signal envelopes of the given values, one per envelope, and noise
    // envelopes of `noise`, each delta coded from the one before.
    void code_envelopes(AspxChannelFields& fields, const std::vector<std::vector<int>>& signal,
                        std::span<const int> noise, bool iframe) const;

    const AspxSetup* setup_;
    dsp::QmfAnalysis<double> analysis_;
    dsp::QmfSynthesis<double> synthesis_;
    std::deque<Slot> slots_;
    long long first_slot_ = 0;
    Slot silence_{};
    std::vector<double> companded_;
    long long companded_first_ = -kCompandedLag;  // the sample companded_[0] is: -(d_pcm + 577)

    // What the decoder keeps: the last envelopes' quantised values for delta
    // coding along time (Pseudocodes 80 and 81), and the high frequency
    // generator's chirp factors and modes (Pseudocode 88).
    bool have_previous_ = false;
    std::array<int, aspx::kMaxSbgMaster> sig_prev_{};
    bool high_prev_ = true;  // the last signal envelope's frequency resolution
    int qmode_prev_ = 0;     // and its aspx_qmode_env
    std::array<int, aspx::kMaxSbgNoise> noise_prev_{};
    std::vector<int> tna_prev_;
    aspx::HfGeneratorState<double> hf_;
    // Pseudocode 92's sine_idx of the last envelope, by QMF subband.
    std::array<bool, dsp::kQmfSubbands> sine_prev_{};
    // Whether the last values were a balance pair's second channel's, whose
    // values are even: only then can balance values be coded along time.
    bool balance_prev_ = false;
    // Where the last interval stopped, in A-SPX slots from its frame's start:
    // num_aspx_timeslots for an interval that ends with its frame, which the
    // next starts with, and up to 3 more for one whose variable border runs
    // on.
    int stop_prev_ = kAspxTimeslots;
    // The A-SPX band's energy per A-SPX slot over the last four slots before
    // the interval, for the attack detector.
    std::array<double, 4> energy_prev_{};
};

// Pseudocode 76's signal borders of an interval, atsg_sig, in A-SPX slots
// from its frame's start, for the interval class and borders `framing` sends
// and, for a variable start, where the last interval stopped, in a frame of
// `aspx_slots` A-SPX slots; Table 194's borders for FIXFIX.
[[nodiscard]] std::vector<int> interval_borders(const AspxFramingFields& framing, int start,
                                                int aspx_slots = kAspxTimeslots);

// Pseudocode 77 with aspx_freq_res_mode 2: whether an envelope takes the high
// resolution table, from its length and where it sits against aspx_tsg_ptr.
[[nodiscard]] bool envelope_high_res(std::span<const int> borders, int envelope, int tsg_ptr,
                                     int aspx_slots = kAspxTimeslots) noexcept;

// Table 193, and Table 194 for FIXFIX: the noise envelopes' borders.
[[nodiscard]] std::vector<int> noise_borders(const AspxFramingFields& framing,
                                             std::span<const int> borders,
                                             int aspx_slots = kAspxTimeslots);

// Table 194: tab_border for `aspx_slots` A-SPX slots and 1, 2 or 4
// envelopes.
[[nodiscard]] std::vector<int> fixfix_borders(int aspx_slots, int envelopes);

// A channel element's companding_control() and aspx_data fields.
struct AspxElement {
    CompandingFields companding;
    std::vector<AspxChannelFields> channels;  // one or two
    bool balance = false;                     // aspx_balance, for two
};

// The ASPX parts of a channel element ahead of the channel data: aspx_config()
// in an I-frame and companding_control().
void write_aspx_head(BitWriter& w, bool iframe, const AspxSetup& setup, const AspxElement& element);
// The aspx_data_1ch() or aspx_data_2ch() after it.
void write_aspx_tail(BitWriter& w, bool iframe, const AspxSetup& setup, const AspxElement& element);

}  // namespace ac4::detail
