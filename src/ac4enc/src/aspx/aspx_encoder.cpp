#include "aspx/aspx_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ac4::detail {
namespace {

constexpr std::size_t kSubbands = dsp::kQmfSubbands;

// Companding (5.7.5): the expander multiplies a slot by 2^(1/alpha)
// L^((1 - alpha) / alpha), L its level against full scale, so the compressor
// that undoes it multiplies by 0.5 L^(alpha - 1).
constexpr double kAlpha = 0.65;

// Pseudocode 83: a noise floor's value q is 2^(6 - q), the energy of the
// noise over the patch's; the level F0 codebook holds q from 0 to 29.
constexpr int kNoiseFloorOffset = 6;
constexpr int kMaxNoise = 29;
// The signal level F0 codebooks: q from 0 to 70 in steps of 1.5 dB
// (aspx_qmode_env 0), and to 35 in steps of 3 dB.
constexpr int kMaxSignalFine = 70;
constexpr int kMaxSignalCoarse = 35;
// The smallest envelope a signal scale factor sends, 64 per QMF subsample
// (Pseudocode 82): a group of the input below it is silence.
constexpr double kSilence = 64.0;

constexpr int kTnaModes = 4;

// The Q_low slots whose tonality is measured: the interval's, less the four
// its first predictions look back over; every second one. Below 1 536 samples
// a frame, whose interval holds too few, the slots before it make up at
// least kToneWindow (AspxChannelEncoder::tone_lead()).
constexpr int kToneFirst = 4;
constexpr int kToneWindow = 24;

// Framing: an attack is an A-SPX slot whose band energy is ten times the mean
// of the four before; a FIXFIX interval whose halves differ by more than
// kSplitDb takes two envelopes; an attack's envelope is kTransientSlots long.
// Relative borders run 2 to 8 slots, three a side at most, and at 8 A-SPX
// slots a frame or fewer 2 or 4, one a side (Table 53).
constexpr double kAttack = 10.0;
constexpr double kSplitDb = 6.0;
constexpr int kTransientSlots = 4;

struct RelativeLimits {
    int longest = 8;
    int count = 3;
};

[[nodiscard]] RelativeLimits relative_limits(int aspx_slots) noexcept {
    return aspx_slots > 8 ? RelativeLimits{.longest = 8, .count = 3}
                          : RelativeLimits{.longest = 4, .count = 1};
}

// Added sinusoids: a subband whose tonal share is over kSineStart (kSineKeep
// for one the last interval carried), kSinePeak times its group's mean
// energy, and kSineGap more tonal than the patch there.
constexpr double kSineStart = 0.5;
constexpr double kSineKeep = 0.3;
constexpr double kSinePeak = 2.0;
constexpr double kSineGap = 0.2;
// Interleaved waveform coding, where asked for, takes a group whose tone is
// steadier still.
constexpr double kInterleaveTonal = 0.8;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Even lengths of at most `longest` that add up to `length`, which is even,
// as few and as equal as can be.
[[nodiscard]] std::vector<int> chunks(int length, int longest) {
    if (length <= 0) {
        return {};
    }
    const int count = (length + longest - 1) / longest;
    std::vector<int> out(at(count), length / count / 2 * 2);
    int rest = length - (length / count / 2 * 2) * count;
    for (std::size_t i = out.size(); rest > 0; rest -= 2) {
        out[--i] += 2;
    }
    return out;
}

// L(ts) as the decoder measures it: 0.9105 times the mean over [0, sb1) of
// max(|Re|, |Im|) + min(|Re|, |Im|) / 2.
[[nodiscard]] double slot_level(std::span<const QmfSample> slot, int sb1) noexcept {
    if (sb1 <= 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (int sb = 0; sb < sb1; ++sb) {
        const double re = std::abs(slot[at(sb)].real());
        const double im = std::abs(slot[at(sb)].imag());
        sum += std::max(re, im) + 0.5 * std::min(re, im);
    }
    return 0.9105 * sum / static_cast<double>(sb1);
}

struct Residual {
    double residual = 0.0;
    double energy = 0.0;
};

// How much of a subband's energy a second order predictor at lags of two
// and four slots leaves, the high frequency generator's own form of
// predictor (Pseudocode 86), fitted by least squares over every second slot
// of Q_low from `first` to `last`. `matrix` holds Q_low's slot ts at
// [(ts + offset) * 64].
[[nodiscard]] Residual predict(std::span<const QmfSample> matrix, int offset, int sb, int first,
                               int last) noexcept {
    const auto value = [&](int ts) { return matrix[at(ts + offset) * kSubbands + at(sb)]; };
    // y + a u + b v, u and v the values two and four slots before y, is
    // least at R [a b]^T = -r, R Hermitian.
    double ruu = 0.0;
    double rvv = 0.0;
    QmfSample ruv{};
    QmfSample ruy{};
    QmfSample rvy{};
    double energy = 0.0;
    for (int ts = first; ts < last; ts += 2) {
        const QmfSample y = value(ts);
        const QmfSample u = value(ts - 2);
        const QmfSample v = value(ts - 4);
        ruu += std::norm(u);
        rvv += std::norm(v);
        ruv += std::conj(u) * v;
        ruy += std::conj(u) * y;
        rvy += std::conj(v) * y;
        energy += std::norm(y);
    }
    const double ridge = 1e-9 * (ruu + rvv) + 1e-30;
    const double a11 = ruu + ridge;
    const double a22 = rvv + ridge;
    const double det = a11 * a22 - std::norm(ruv);
    QmfSample a{};
    QmfSample b{};
    if (det > 0.0) {
        a = -(a22 * ruy - ruv * rvy) / det;
        b = -(a11 * rvy - std::conj(ruv) * ruy) / det;
    }
    double residual = 0.0;
    for (int ts = first; ts < last; ts += 2) {
        residual += std::norm(value(ts) + a * value(ts - 2) + b * value(ts - 4));
    }
    return {.residual = std::min(residual, energy), .energy = energy};
}

// The share of a group's energy the predictor finds, less the share a fit of
// two coefficients finds in noise of the same length, `samples` values: 0 for
// noise, towards 1 for steady tones.
[[nodiscard]] double tonal_share(const Residual& r, int samples) noexcept {
    if (r.energy <= 0.0) {
        return 0.0;
    }
    const double bias = 2.0 / static_cast<double>(samples);
    const double share = 1.0 - r.residual / r.energy;
    return std::clamp((share - bias) / (1.0 - bias), 0.0, 1.0);
}

// The quantised values an envelope's fields stand for (Pseudocodes 80 and
// 81, one resolution throughout); `delta` is 2 for a balance pair's second
// channel.
[[nodiscard]] std::vector<int> decode_values(const AspxEnvelopeFields& e, std::span<const int> previous,
                                             int delta = 1) {
    std::vector<int> q(e.values.size());
    int sum = 0;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (e.delta_dir == 0) {
            sum += delta * e.values[i];
            q[i] = sum;
        } else {
            q[i] = previous[i] + delta * e.values[i];
        }
    }
    return q;
}

// PAN_OFFSET of Pseudocode 84.
constexpr int kPanOffset = 12;

}  // namespace

namespace {

// The rest of aspx_config() as DEE sends it, and the tables the configuration
// gives; false where they do not derive.
[[nodiscard]] bool complete(AspxSetup& setup, int sample_rate_hz, const FrameTiming& timing) {
    setup.timing = timing;
    AspxConfigFields& c = setup.config;
    c.quant_mode_env = 1;
    c.interpolation = true;
    c.preflat = true;
    c.limiter = true;
    c.noise_sbg = 3;
    c.num_env_bits_fixfix = 0;
    c.freq_res_mode = 2;
    setup.base_48k = sample_rate_hz == 48000;
    const aspx::FrequencyConfig frequency{.master_freq_scale = c.master_freq_scale,
                                          .start_freq = c.start_freq,
                                          .stop_freq = c.stop_freq,
                                          .noise_sbg = c.noise_sbg,
                                          .xover_subband_offset = setup.xover_subband_offset};
    if (aspx::derive_subband_groups(frequency, setup.groups) != aspx::GroupsError::kNone ||
        !aspx::derive_patch_tables(setup.groups, c.master_freq_scale, setup.base_48k, setup.patches)) {
        return false;
    }
    setup.counts = AspxCounts{.num_sbg_sig_highres = setup.groups.num_sbg_sig_highres,
                              .num_sbg_sig_lowres = setup.groups.num_sbg_sig_lowres,
                              .num_sbg_noise = setup.groups.num_sbg_noise,
                              .num_aspx_timeslots = timing.aspx_slots};
    return true;
}

}  // namespace

std::optional<AspxSetup> aspx_setup_for_acpl(bool coupling, int sample_rate_hz,
                                             const FrameTiming& timing) {
    if (sample_rate_hz != 48000 && sample_rate_hz != 44100) {
        return std::nullopt;
    }
    AspxSetup setup;
    setup.config.master_freq_scale = 1;
    setup.config.start_freq = 5;
    setup.config.stop_freq = coupling ? 2 : 0;
    setup.xover_subband_offset = coupling ? 0 : 1;
    setup.companding = false;
    if (!complete(setup, sample_rate_hz, timing)) {
        return std::nullopt;
    }
    return setup;
}

std::optional<AspxSetup> aspx_setup_for(double kbps_per_channel, int sample_rate_hz,
                                        bool multichannel, const FrameTiming& timing) {
    if (sample_rate_hz != 48000 && sample_rate_hz != 44100) {
        return std::nullopt;
    }
    AspxSetup setup;
    AspxConfigFields& c = setup.config;
    setup.xover_subband_offset = 0;
    // DEE's aspx_config() in G0's 2.0 legs at 48, 64 and 96 to 144 kbps: the
    // low resolution table from subband 20, the high resolution one from 28,
    // and from 36 (tests/ac4core/test_ac4core_aspx.cpp works two of them
    // through Pseudocodes 67 to 74). In its 5.1 legs from 192 to 320 kbps, the
    // high resolution table from subband 32, and from 256 kbps with
    // aspx_stop_freq 0 and aspx_xover_subband_offset 1.
    if (multichannel && kbps_per_channel >= 38.4) {
        const bool upper = kbps_per_channel >= 51.2;
        c.master_freq_scale = 1;
        c.start_freq = 5;
        c.stop_freq = upper ? 0 : 1;
        setup.xover_subband_offset = upper ? 1 : 0;
    } else if (kbps_per_channel < 32.0) {
        c.master_freq_scale = 0;
        c.start_freq = 5;
        c.stop_freq = 0;
    } else if (kbps_per_channel < 48.0) {
        c.master_freq_scale = 1;
        c.start_freq = 4;
        c.stop_freq = 1;
    } else {
        c.master_freq_scale = 1;
        c.start_freq = 6;
        c.stop_freq = 1;
    }
    setup.companding = !multichannel && kbps_per_channel < 64.0;
    if (!complete(setup, sample_rate_hz, timing)) {
        return std::nullopt;
    }
    return setup;
}

AspxChannelEncoder::AspxChannelEncoder(const AspxSetup& setup)
    : setup_(&setup),
      companded_first_(-(setup.timing.alignment_delay + 577)),
      tna_prev_(at(setup.groups.num_sbg_noise), 0),
      stop_prev_(setup.timing.aspx_slots) {}

int AspxChannelEncoder::tone_lead() const noexcept {
    return std::max(0, kToneWindow - setup_->timing.qmf_slots);
}

int AspxChannelEncoder::tone_samples() const noexcept {
    return (setup_->timing.qmf_slots + tone_lead() - kToneFirst) / 2;
}

void AspxChannelEncoder::push_slot(std::span<const double> samples) {
    std::array<double, kSubbands> scaled{};
    for (std::size_t i = 0; i < kSubbands; ++i) {
        scaled[i] = samples[i] * kQmfFullScale;
    }
    Slot& analysed = slots_.emplace_back();
    analysis_.process(scaled, analysed);
    if (!setup_->companding) {
        return;
    }
    // The compressor, over the subbands below the crossover, which alone the
    // expander touches. Nothing above it is synthesised, unless interleaving
    // may code some of it, as it is.
    const int sbx = setup_->groups.sbx;
    const double level = slot_level(analysed, sbx) / kQmfFullScale;
    const double gain = level > 0.0 ? 0.5 * std::pow(level, kAlpha - 1.0) : 0.0;
    Slot compressed{};
    for (int sb = 0; sb < sbx; ++sb) {
        compressed[at(sb)] = analysed[at(sb)] * gain;
    }
    if (setup_->interleave) {
        std::copy(analysed.begin() + sbx, analysed.end(), compressed.begin() + sbx);
    }
    synthesis_.process(compressed, scaled);
    for (const double y : scaled) {
        companded_.push_back(y / kQmfFullScale);
    }
}

double AspxChannelEncoder::companded(long long s) const noexcept {
    const long long index = s - companded_first_;
    if (index < 0 || index >= static_cast<long long>(companded_.size())) {
        return 0.0;
    }
    return companded_[static_cast<std::size_t>(index)];
}

const AspxChannelEncoder::Slot& AspxChannelEncoder::slot(long long g) const noexcept {
    if (g < first_slot_ || g >= slots()) {
        return silence_;
    }
    return slots_[static_cast<std::size_t>(g - first_slot_)];
}

void AspxChannelEncoder::drop_before_frame(long long frame) {
    const FrameTiming& t = setup_->timing;
    const long long keep_slot = static_cast<long long>(t.qmf_slots) * (frame + t.control_delay) -
                                t.hfgen_slots - aspx::kTsOffsetHfadj - tone_lead();
    while (first_slot_ < keep_slot && !slots_.empty()) {
        slots_.pop_front();
        ++first_slot_;
    }
    const long long keep_sample = frame * t.frame_length;
    if (keep_sample > companded_first_) {
        const auto drop = static_cast<std::size_t>(
            std::min<long long>(keep_sample - companded_first_, static_cast<long long>(companded_.size())));
        companded_.erase(companded_.begin(), companded_.begin() + static_cast<std::ptrdiff_t>(drop));
        companded_first_ += static_cast<long long>(drop);
    }
}

void AspxChannelEncoder::gather(long long frame, std::vector<QmfSample>& ext, int lead) const {
    // From ts_offset_hfadj slots before Q_low's slot 0, which is slot
    // num_qmf_timeslots (f + d_ctrl) - ts_offset_hfgen, and `lead` before
    // that.
    const FrameTiming& t = setup_->timing;
    const long long first = static_cast<long long>(t.qmf_slots) * (frame + t.control_delay) -
                            t.hfgen_slots - aspx::kTsOffsetHfadj - lead;
    const int slots = aspx::kTsOffsetHfadj + t.hfgen_slots + t.qmf_slots + lead;
    ext.resize(at(slots) * kSubbands);
    for (int e = 0; e < slots; ++e) {
        std::ranges::copy(slot(first + e), ext.begin() + static_cast<std::ptrdiff_t>(at(e) * kSubbands));
    }
}

void AspxChannelEncoder::generate(std::span<const QmfSample> ext,
                                  std::span<const std::uint8_t> tna_mode,
                                  aspx::HfGeneratorState<double>& state,
                                  std::vector<QmfSample>& q_high, int lead) const {
    const FrameTiming& t = setup_->timing;
    const int slots = t.qmf_slots + lead;
    q_high.assign(at(t.hfgen_slots + slots) * kSubbands, QmfSample{});
    const aspx::HfGeneratorInput<double> in{
        .q_low_ext = ext,
        .num_qmf_timeslots = slots,
        .ts_offset_hfgen = t.hfgen_slots,
        .ts_begin = 0,
        .ts_end = slots,
        .preflat = setup_->config.preflat,
        .tna_mode = tna_mode,
    };
    aspx::generate_high_band<double>(setup_->groups, setup_->patches, in, state, q_high);
}

AspxChannelFields AspxChannelEncoder::propose(long long frame, bool iframe) {
    std::vector<QmfSample> ext;
    gather(frame, ext);
    // The slots tonality is measured over, with those ahead of the interval.
    std::vector<QmfSample> tone_ext;
    gather(frame, tone_ext, tone_lead());
    AspxChannelFields fields = framed(choose_framing(ext));
    const std::vector<int> noise = choose_inverse_filtering(tone_ext, fields);
    // Interleaving, where asked for, takes a steady tone first: the spectral
    // frontend codes it where it is, and a sinusoid would sit at its group's
    // middle subband.
    if (setup_->interleave) {
        choose_interleaving(tone_ext, fields);
    }
    choose_sinusoids(tone_ext, fields);
    std::array<bool, dsp::kQmfSubbands> waveform{};
    for (const auto& [first, last] : interleaved_subbands(fields)) {
        std::fill(waveform.begin() + first, waveform.begin() + last, true);
    }
    const std::vector<int> borders = interval_borders(
        fields.framing, stop_prev_ - setup_->timing.aspx_slots, setup_->timing.aspx_slots);
    std::vector<std::vector<int>> signal;
    for (int env = 0; env + 1 < static_cast<int>(borders.size()); ++env) {
        signal.push_back(signal_envelope(ext, borders[at(env)], borders[at(env + 1)],
                                         fields.envelope_freq_res[at(env)] != 0, fields.qmode_env,
                                         sine_subbands(fields, env), waveform));
    }
    code_envelopes(fields, signal, noise, iframe);
    return fields;
}

// The subbands whose sinusoid envelope `env` carries (Pseudocode 92): the
// middle of each high resolution group with aspx_add_harmonic set, from the
// transient envelope on, or from the first where the last interval ended
// with it.
std::array<bool, dsp::kQmfSubbands> AspxChannelEncoder::sine_subbands(const AspxChannelFields& fields,
                                                                      int env) const {
    std::array<bool, dsp::kQmfSubbands> sines{};
    const aspx::SubbandGroups& g = setup_->groups;
    for (int sbg = 0; sbg < static_cast<int>(fields.add_harmonic.size()); ++sbg) {
        const int mid = (g.sbg_sig_highres[at(sbg)] + g.sbg_sig_highres[at(sbg + 1)]) / 2;
        if (fields.add_harmonic[at(sbg)] && (env >= fields.framing.tsg_ptr || sine_prev_[at(mid)])) {
            sines[at(mid)] = true;
        }
    }
    return sines;
}

void AspxChannelEncoder::choose_interleaving(std::span<const QmfSample> ext, AspxChannelFields& fields) const {
    // Only where every envelope takes the high resolution groups the flags
    // name, so that no envelope's group mixes coded and recreated subbands.
    if (std::ranges::any_of(fields.envelope_freq_res, [](int high) { return high == 0; })) {
        return;
    }
    const aspx::SubbandGroups& g = setup_->groups;
    const int tone_last = setup_->timing.qmf_slots + tone_lead();
    std::vector<std::uint8_t> modes(fields.tna_mode.begin(), fields.tna_mode.end());
    aspx::HfGeneratorState<double> state = hf_;
    std::vector<QmfSample> q_high;
    generate(ext, modes, state, q_high, tone_lead());
    std::vector<bool> coded(at(g.num_sbg_sig_highres), false);
    bool any = false;
    for (int sbg = 0; sbg < g.num_sbg_sig_highres; ++sbg) {
        const int lo = g.sbg_sig_highres[at(sbg)];
        const int hi = g.sbg_sig_highres[at(sbg + 1)];
        int peak = lo;
        Residual peak_input{};
        double total = 0.0;
        for (int sb = lo; sb < hi; ++sb) {
            const Residual r = predict(ext, aspx::kTsOffsetHfadj, sb, kToneFirst, tone_last);
            total += r.energy;
            if (r.energy > peak_input.energy) {
                peak_input = r;
                peak = sb;
            }
        }
        if (peak_input.energy < kSilence * tone_samples()) {
            continue;
        }
        const double ratio = peak_input.energy / (total / static_cast<double>(hi - lo));
        const double tonal = tonal_share(peak_input, tone_samples());
        const double patched =
            tonal_share(predict(q_high, 0, peak, kToneFirst, tone_last), tone_samples());
        coded[at(sbg)] = tonal > kInterleaveTonal && (hi - lo == 1 || ratio > kSinePeak) && patched < tonal - kSineGap;
        any = any || coded[at(sbg)];
    }
    fields.fic_used_in_sfb = any ? coded : std::vector<bool>{};
}

std::vector<std::pair<int, int>> AspxChannelEncoder::interleaved_subbands(const AspxChannelFields& fields) const {
    std::vector<std::pair<int, int>> out;
    const aspx::SubbandGroups& g = setup_->groups;
    for (std::size_t sbg = 0; sbg < fields.fic_used_in_sfb.size(); ++sbg) {
        if (fields.fic_used_in_sfb[sbg]) {
            out.emplace_back(g.sbg_sig_highres[sbg], g.sbg_sig_highres[sbg + 1]);
        }
    }
    return out;
}

// aspx_add_harmonic: a sinusoid where the input's high band holds a steady
// one near a group's middle subband, which is where the decoder puts it, and
// the patch at the inverse filtering chosen does not. A sinusoid the last
// interval carried stays on a lower threshold.
void AspxChannelEncoder::choose_sinusoids(std::span<const QmfSample> ext, AspxChannelFields& fields) const {
    const aspx::SubbandGroups& g = setup_->groups;
    const int tone_last = setup_->timing.qmf_slots + tone_lead();
    std::vector<std::uint8_t> modes(fields.tna_mode.begin(), fields.tna_mode.end());
    aspx::HfGeneratorState<double> state = hf_;
    std::vector<QmfSample> q_high;
    generate(ext, modes, state, q_high, tone_lead());
    std::vector<bool> harmonic(at(g.num_sbg_sig_highres), false);
    bool any = false;
    for (int sbg = 0; sbg < g.num_sbg_sig_highres; ++sbg) {
        if (!fields.fic_used_in_sfb.empty() && fields.fic_used_in_sfb[at(sbg)]) {
            continue;  // the spectral frontend codes this group's tone
        }
        const int lo = g.sbg_sig_highres[at(sbg)];
        const int hi = g.sbg_sig_highres[at(sbg + 1)];
        const int mid = (lo + hi) / 2;
        int peak = lo;
        double peak_energy = 0.0;
        double total = 0.0;
        std::array<Residual, dsp::kQmfSubbands> input{};
        for (int sb = lo; sb < hi; ++sb) {
            input[at(sb)] = predict(ext, aspx::kTsOffsetHfadj, sb, kToneFirst, tone_last);
            total += input[at(sb)].energy;
            if (input[at(sb)].energy > peak_energy) {
                peak_energy = input[at(sb)].energy;
                peak = sb;
            }
        }
        if (peak_energy < kSilence * tone_samples() || std::abs(peak - mid) > 1) {
            continue;
        }
        const double ratio = peak_energy / (total / static_cast<double>(hi - lo));
        const double tonal = tonal_share(input[at(peak)], tone_samples());
        const double patched =
            tonal_share(predict(q_high, 0, peak, kToneFirst, tone_last), tone_samples());
        const double threshold = sine_prev_[at(mid)] ? kSineKeep : kSineStart;
        harmonic[at(sbg)] = tonal > threshold && (hi - lo == 1 || ratio > kSinePeak) && patched < tonal - kSineGap;
        any = any || harmonic[at(sbg)];
    }
    fields.add_harmonic = any ? harmonic : std::vector<bool>{};
}

AspxChannelFields AspxChannelEncoder::fallback(bool iframe, bool silent,
                                               std::optional<int> start) const {
    // One envelope from where the last interval stopped to the frame's end.
    AspxFramingFields framing;
    const int left = start.value_or(stop_prev_ - setup_->timing.aspx_slots);
    if (left > 0) {
        framing.int_class = AspxIntervalClass::kVarFix;
        framing.var_bord_left = left;
    }
    AspxChannelFields fields = framed(framing);
    const bool high = fields.envelope_freq_res.front() != 0;
    const int groups = high ? setup_->groups.num_sbg_sig_highres : setup_->groups.num_sbg_sig_lowres;
    const auto noise_groups = at(setup_->groups.num_sbg_noise);
    if (silent || !have_previous_) {
        // Envelopes at 64 per subsample, the cheapest to send, and no noise.
        fields.tna_mode.assign(noise_groups, 0);
        const std::vector<int> noise(noise_groups, kMaxNoise);
        code_envelopes(fields, {std::vector<int>(at(groups), 0)}, noise, true);
        return fields;
    }
    // The last values again, in this envelope's step and resolution.
    fields.tna_mode = tna_prev_;
    std::vector<int> signal = map_resolution(sig_prev_, high_prev_, high);
    const int from = qmode_prev_ == 0 ? 2 : 1;
    const int to = fields.qmode_env == 0 ? 2 : 1;
    for (int& q : signal) {
        q = q * to / from;
    }
    const std::vector<int> noise(noise_prev_.begin(), noise_prev_.begin() + static_cast<std::ptrdiff_t>(noise_groups));
    code_envelopes(fields, {signal}, noise, iframe);
    return fields;
}

void AspxChannelEncoder::commit(long long frame, const AspxChannelFields& sent, bool balance_values) {
    // Each envelope coded along time follows the one before it, the first
    // the last frame's last, put on its groups.
    const int delta = balance_values ? 2 : 1;
    std::vector<int> signal(sig_prev_.begin(), sig_prev_.end());
    bool high = high_prev_;
    for (std::size_t env = 0; env < sent.sig.size(); ++env) {
        const bool next_high = sent.envelope_freq_res[env] != 0;
        signal = decode_values(sent.sig[env], map_resolution(signal, high, next_high), delta);
        high = next_high;
    }
    std::vector<int> noise(noise_prev_.begin(), noise_prev_.end());
    for (const AspxEnvelopeFields& e : sent.noise) {
        noise = decode_values(e, noise, delta);
    }
    balance_prev_ = balance_values;
    sig_prev_.fill(0);
    std::ranges::copy(signal, sig_prev_.begin());
    high_prev_ = high;
    qmode_prev_ = sent.qmode_env;
    noise_prev_.fill(0);
    std::ranges::copy(noise, noise_prev_.begin());
    have_previous_ = true;
    tna_prev_ = sent.tna_mode;
    sine_prev_ = sine_subbands(sent, sent.framing.num_env() - 1);
    const int slots = setup_->timing.aspx_slots;
    stop_prev_ = interval_borders(sent.framing, stop_prev_ - slots, slots).back();
    // The generator's chirp factors follow the modes sent, and the attack
    // detector keeps the band's last four slots.
    std::vector<QmfSample> ext;
    gather(frame, ext);
    std::vector<std::uint8_t> modes(sent.tna_mode.begin(), sent.tna_mode.end());
    std::vector<QmfSample> q_high;
    generate(ext, modes, hf_, q_high);
    for (int t = 0; t < 4; ++t) {
        energy_prev_[at(t)] = band_energy(ext, slots - 4 + t);
    }
}

// The A-SPX band's energy in A-SPX slot t of Q_low: its num_ts_in_ats QMF
// slots.
double AspxChannelEncoder::band_energy(std::span<const QmfSample> ext, int t) const {
    const aspx::SubbandGroups& g = setup_->groups;
    const int per = setup_->timing.ts_in_ats;
    double energy = 0.0;
    for (int ts = per * t; ts < per * (t + 1); ++ts) {
        const std::size_t row = at(ts + aspx::kTsOffsetHfadj) * kSubbands;
        for (int sb = g.sbx; sb < g.sbx + g.num_sb_aspx; ++sb) {
            energy += std::norm(ext[row + at(sb)]);
        }
    }
    return energy;
}

// The interval's framing. From the frame's start, where the last interval
// stopped there: no attack gives FIXFIX, one envelope or two where the
// band's level moves by more than kSplitDb between Table 194's halves; an
// attack gives FIXVAR, with an envelope of kTransientSlots at the attack and
// the interval run on to an end whose parity puts a border there, and the
// next interval then starts where this one stops. From a later start,
// VARFIX, with the attack's envelope where there is one. At 8 A-SPX slots a
// frame or fewer, where a side takes one relative border of 2 or 4 slots, an
// attack those cannot reach takes FIXFIX's two envelopes, or VARFIX's one.
AspxFramingFields AspxChannelEncoder::choose_framing(std::span<const QmfSample> ext) const {
    const aspx::SubbandGroups& g = setup_->groups;
    const int slots = setup_->timing.aspx_slots;
    const RelativeLimits limits = relative_limits(slots);
    const int start = stop_prev_ - slots;
    // The band's energy per A-SPX slot, from four slots before the frame.
    std::vector<double> energy(at(4 + slots));
    for (int t = -4; t < slots; ++t) {
        energy[at(t + 4)] = t < -2 ? energy_prev_[at(t + 4)] : band_energy(ext, t);
    }
    const double floor = kSilence * static_cast<double>(setup_->timing.ts_in_ats * g.num_sb_aspx);
    int attack = -1;
    for (int t = start; t < slots && attack < 0; ++t) {
        const double before = 0.25 * (energy[at(t)] + energy[at(t + 1)] + energy[at(t + 2)] + energy[at(t + 3)]);
        if (energy[at(t + 4)] > floor && energy[at(t + 4)] > kAttack * std::max(before, floor)) {
            attack = t;
        }
    }
    // The end of an interval run on from the frame's end so that it lies an
    // even number of slots after `from`.
    const auto end_after = [&](int from) { return slots + (slots + from) % 2; };

    AspxFramingFields f;
    const auto fixfix = [&](bool split) {
        f = AspxFramingFields{};
        f.int_class = AspxIntervalClass::kFixFix;
        f.tmp_num_env = split ? 1 : 0;
        return f;
    };
    if (start == 0 && attack < 0) {
        const int middle = fixfix_borders(slots, 2)[1];
        double first = 0.0;
        double second = 0.0;
        for (int t = 0; t < slots; ++t) {
            (t < middle ? first : second) += energy[at(t + 4)];
        }
        const bool moves = first > floor * middle && second > floor * (slots - middle) &&
                           std::abs(10.0 * std::log10(second / first)) > kSplitDb;
        return fixfix(moves);
    }
    if (start == 0) {
        // FIXVAR: borders counted back from an end of the attack's parity.
        f.int_class = AspxIntervalClass::kFixVar;
        const int end = end_after(attack);
        f.var_bord_right = end - slots;
        const int transient = std::min(kTransientSlots, end - attack);
        std::vector<int> right = chunks(end - attack - transient, limits.longest);
        std::reverse(right.begin(), right.end());  // from the end back
        if (attack > 0) {
            right.push_back(transient);
            f.tsg_ptr = 1;
        } else {
            f.tsg_ptr = 0;
        }
        if (static_cast<int>(right.size()) > limits.count) {
            return fixfix(true);
        }
        f.rel_bord_right = right;
        return f;
    }
    if (attack >= 0 && setup_->varvar) {
        // VARVAR: from the last interval's stop to the attack, then the
        // attack's envelope, then on to an end of its parity.
        const int before = (attack - start) / 2 * 2;
        const std::vector<int> left = chunks(before, limits.longest);
        const int at_attack = start + before;
        const int end = end_after(at_attack);
        const int transient = std::min(kTransientSlots, end - at_attack);
        std::vector<int> right = chunks(end - at_attack - transient, limits.longest);
        std::reverse(right.begin(), right.end());
        if (static_cast<int>(left.size()) <= limits.count &&
            static_cast<int>(right.size()) <= limits.count) {
            f.int_class = AspxIntervalClass::kVarVar;
            f.var_bord_left = start;
            f.rel_bord_left = left;
            f.var_bord_right = end - slots;
            f.rel_bord_right = right;
            f.tsg_ptr = static_cast<int>(left.size());
            return f;
        }
    }
    // VARFIX from the last interval's stop.
    f.int_class = AspxIntervalClass::kVarFix;
    f.var_bord_left = start;
    if (attack >= 0) {
        const int before = (attack - start) / 2 * 2;
        std::vector<int> left = chunks(before, limits.longest);
        if (static_cast<int>(left.size()) <= limits.count) {
            f.tsg_ptr = static_cast<int>(left.size());
            if (start + before + kTransientSlots < slots &&
                static_cast<int>(left.size()) < limits.count) {
                left.push_back(kTransientSlots);
            }
            f.rel_bord_left = left;
        }
    }
    return f;
}

AspxChannelFields AspxChannelEncoder::framed(const AspxFramingFields& framing) const {
    AspxChannelFields fields;
    fields.framing = framing;
    const int num_env = framing.num_env();
    // aspx_qmode_env: the configured step, or 1.5 dB for a FIXFIX interval
    // of one envelope (Tables 51 and 52).
    fields.qmode_env = framing.int_class == AspxIntervalClass::kFixFix && num_env == 1
                           ? 0
                           : setup_->config.quant_mode_env;
    const int slots = setup_->timing.aspx_slots;
    const std::vector<int> borders = interval_borders(framing, stop_prev_ - slots, slots);
    for (int env = 0; env < num_env; ++env) {
        fields.envelope_freq_res.push_back(
            envelope_high_res(borders, env, framing.tsg_ptr, slots) ? 1 : 0);
    }
    return fields;
}

void AspxChannelEncoder::code_envelopes(AspxChannelFields& fields, const std::vector<std::vector<int>>& signal,
                                        std::span<const int> noise, bool iframe) const {
    // Along time from the last frame's envelopes only where a decoder has
    // them: never in an I-frame.
    const bool from_last = have_previous_ && !iframe;
    std::vector<int> previous(sig_prev_.begin(), sig_prev_.end());
    bool previous_high = high_prev_;
    fields.sig.clear();
    for (std::size_t env = 0; env < signal.size(); ++env) {
        const bool high = fields.envelope_freq_res[env] != 0;
        const std::vector<int> mapped = map_resolution(previous, previous_high, high);
        fields.sig.push_back(delta_code(signal[env], mapped, env > 0 || from_last, true, fields.qmode_env));
        previous = decode_values(fields.sig.back(), mapped);
        previous_high = high;
    }
    std::vector<int> previous_noise(noise_prev_.begin(), noise_prev_.end());
    fields.noise.clear();
    for (int env = 0; env < fields.framing.num_noise(); ++env) {
        fields.noise.push_back(delta_code(noise, previous_noise, env > 0 || from_last, false, 0));
        previous_noise = decode_values(fields.noise.back(), previous_noise);
    }
}

// aspx_tna_mode per noise group, and the noise floors. The decoder's high
// frequency generator is run on the input's own low band at each of the four
// modes; each group takes the strongest whitening that leaves its patch at
// least as tonal as the input's high band there, and a noise floor that
// brings the patch's tonal share down to the input's.
std::vector<int> AspxChannelEncoder::choose_inverse_filtering(std::span<const QmfSample> ext,
                                                              AspxChannelFields& fields) const {
    const aspx::SubbandGroups& g = setup_->groups;
    const int groups = g.num_sbg_noise;
    const int tone_last = setup_->timing.qmf_slots + tone_lead();
    const auto group_of = [&](int sb) {
        int group = 0;
        while (group + 1 < groups && sb >= g.sbg_noise[at(group + 1)]) {
            ++group;
        }
        return group;
    };
    const auto sums_of = [&](std::span<const QmfSample> matrix, int offset) {
        std::array<Residual, aspx::kMaxSbgNoise> sums{};
        for (int sb = g.sbx; sb < g.sbx + g.num_sb_aspx; ++sb) {
            const Residual r = predict(matrix, offset, sb, kToneFirst, tone_last);
            Residual& sum = sums[at(group_of(sb))];
            sum.residual += r.residual;
            sum.energy += r.energy;
        }
        return sums;
    };
    const auto shares = [&](std::span<const QmfSample> matrix, int offset) {
        const std::array<Residual, aspx::kMaxSbgNoise> sums = sums_of(matrix, offset);
        std::array<double, aspx::kMaxSbgNoise> share{};
        for (int group = 0; group < groups; ++group) {
            share[at(group)] = tonal_share(sums[at(group)], tone_samples());
        }
        return share;
    };
    const std::array<Residual, aspx::kMaxSbgNoise> input_sums = sums_of(ext, aspx::kTsOffsetHfadj);
    const std::array<double, aspx::kMaxSbgNoise> input = shares(ext, aspx::kTsOffsetHfadj);

    std::array<std::array<double, aspx::kMaxSbgNoise>, kTnaModes> patched{};
    std::vector<QmfSample> q_high;
    std::vector<std::uint8_t> modes(at(groups));
    for (int mode = 0; mode < kTnaModes; ++mode) {
        std::ranges::fill(modes, static_cast<std::uint8_t>(mode));
        aspx::HfGeneratorState<double> state = hf_;
        generate(ext, modes, state, q_high, tone_lead());
        patched[at(mode)] = shares(q_high, 0);
    }

    fields.tna_mode.assign(at(groups), 0);
    std::vector<int> noise(at(groups), kMaxNoise);
    for (int group = 0; group < groups; ++group) {
        const double target = input[at(group)];
        int mode = 0;
        while (mode + 1 < kTnaModes && patched[at(mode + 1)][at(group)] >= target) {
            ++mode;
        }
        fields.tna_mode[at(group)] = mode;
        const double tonal = patched[at(mode)][at(group)];
        // Below the smallest signal envelope, 64 per QMF subsample, a group
        // is silence, to which no noise is added.
        const int width = g.sbg_noise[at(group + 1)] - g.sbg_noise[at(group)];
        const bool silent =
            input_sums[at(group)].energy < kSilence * static_cast<double>(width * tone_samples());
        int q = kMaxNoise;
        if (silent) {
            q = kMaxNoise;
        } else if (target <= 0.0) {
            q = 0;
        } else if (tonal > target) {
            q = static_cast<int>(std::lround(kNoiseFloorOffset - std::log2(tonal / target - 1.0)));
        }
        noise[at(group)] = std::clamp(q, 0, kMaxNoise);
    }
    return noise;
}

// Each group's mean energy per QMF subsample over the envelope, quantised.
// The values run below 0, 64 per subsample, as far as the delta codebooks
// reach: along frequency the first is sent as no less than 0, and
// Pseudocode 82 gives the first group the second's scale factor when that is
// below 0.
std::vector<int> AspxChannelEncoder::signal_envelope(std::span<const QmfSample> ext, int first, int last,
                                                     bool high_res, int quant_mode,
                                                     const std::array<bool, dsp::kQmfSubbands>& sines,
                                                     const std::array<bool, dsp::kQmfSubbands>& waveform) const {
    const aspx::SubbandGroups& g = setup_->groups;
    const int groups = high_res ? g.num_sbg_sig_highres : g.num_sbg_sig_lowres;
    const std::span<const std::uint8_t> table = high_res ? std::span<const std::uint8_t>(g.sbg_sig_highres)
                                                         : std::span<const std::uint8_t>(g.sbg_sig_lowres);
    const double steps = quant_mode == 0 ? 2.0 : 1.0;
    const int top = quant_mode == 0 ? kMaxSignalFine : kMaxSignalCoarse;
    const int per = setup_->timing.ts_in_ats;
    std::vector<int> q(at(groups));
    for (int sbg = 0; sbg < groups; ++sbg) {
        const int lo = table[at(sbg)];
        const int hi = table[at(sbg + 1)];
        // A group with a sinusoid gives it the scale factor whole (Pseudocode
        // 94), so its envelope is the sinusoid's subband's energy.
        std::array<double, dsp::kQmfSubbands> per_subband{};
        for (int ts = per * first; ts < per * last; ++ts) {
            const std::size_t row = at(ts + aspx::kTsOffsetHfadj) * kSubbands;
            for (int sb = lo; sb < hi; ++sb) {
                per_subband[at(sb)] += std::norm(ext[row + at(sb)]);
            }
        }
        double energy = 0.0;
        bool sine = false;
        for (int sb = lo; sb < hi; ++sb) {
            if (sines[at(sb)]) {
                energy = sine ? std::max(energy, per_subband[at(sb)]) : per_subband[at(sb)];
                sine = true;
            } else if (!sine) {
                energy += per_subband[at(sb)];
            }
        }
        energy /= static_cast<double>(per * (last - first) * (sine ? 1 : hi - lo));
        // A group the spectral frontend codes, which the decoder adds A-SPX's
        // output to (5.7.6.5.3), takes none of its own.
        if (std::all_of(waveform.begin() + lo, waveform.begin() + hi, [](bool w) { return w; })) {
            energy = 0.0;
        }
        const double value = energy > 0.0 ? steps * std::log2(energy / kSilence) : -static_cast<double>(top);
        int quantised = std::clamp(static_cast<int>(std::lround(value)), -top, top);
        // Each value within a delta codeword of the one before, as sent.
        if (sbg > 0) {
            const int before = sbg == 1 ? std::max(q[0], 0) : q[at(sbg - 1)];
            quantised = std::clamp(quantised, before - top, before + top);
        }
        q[at(sbg)] = quantised;
    }
    return q;
}

std::vector<int> AspxChannelEncoder::map_resolution(std::span<const int> previous, bool previous_high,
                                                    bool high) const {
    const aspx::SubbandGroups& g = setup_->groups;
    const int groups = high ? g.num_sbg_sig_highres : g.num_sbg_sig_lowres;
    std::vector<int> out(at(groups));
    if (high == previous_high) {
        std::copy_n(previous.begin(), out.size(), out.begin());
        return out;
    }
    // Pseudocode 80's maps between the two tables' groups.
    std::array<int, aspx::kMaxSbgMaster> high2low{};
    std::array<int, aspx::kMaxSbgMaster + 1> low2high{};
    int low = 0;
    for (int sbg = 0; sbg < g.num_sbg_sig_highres; ++sbg) {
        if (low < g.num_sbg_sig_lowres && g.sbg_sig_lowres[at(low + 1)] == g.sbg_sig_highres[at(sbg)]) {
            ++low;
            low2high[at(low)] = sbg;
        }
        high2low[at(sbg)] = low;
    }
    for (int sbg = 0; sbg < groups; ++sbg) {
        out[at(sbg)] = previous[at(high ? high2low[at(sbg)] : low2high[at(sbg)])];
    }
    return out;
}

AspxEnvelopeFields AspxChannelEncoder::delta_code(std::span<const int> q, std::span<const int> previous,
                                                  bool along_time, bool signal, int quant_mode, bool balance) {
    // Along frequency the first value comes from F0, which holds none below 0.
    AspxEnvelopeFields f;
    f.values.resize(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        const int before = i == 1 ? std::max(q[0], 0) : (i > 1 ? q[i - 1] : 0);
        f.values[i] = i == 0 ? std::max(q[0], 0) : q[i] - before;
    }
    if (!along_time) {
        return f;
    }
    AspxEnvelopeFields t;
    t.delta_dir = 1;
    t.values.resize(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        t.values[i] = q[i] - previous[i];
    }
    if (aspx_envelope_codable(signal, quant_mode, balance, t) &&
        aspx_envelope_bits(signal, quant_mode, balance, t) < aspx_envelope_bits(signal, quant_mode, balance, f)) {
        return t;
    }
    return f;
}

std::vector<std::vector<int>> AspxChannelEncoder::signal_values(const AspxChannelFields& fields) const {
    std::vector<std::vector<int>> out;
    std::vector<int> previous(sig_prev_.begin(), sig_prev_.end());
    bool high = high_prev_;
    for (std::size_t env = 0; env < fields.sig.size(); ++env) {
        const bool next_high = fields.envelope_freq_res[env] != 0;
        previous = decode_values(fields.sig[env], map_resolution(previous, high, next_high));
        high = next_high;
        out.push_back(previous);
    }
    return out;
}

std::vector<std::vector<int>> AspxChannelEncoder::noise_values(const AspxChannelFields& fields) const {
    std::vector<std::vector<int>> out;
    std::vector<int> previous(noise_prev_.begin(), noise_prev_.end());
    for (const AspxEnvelopeFields& e : fields.noise) {
        previous = decode_values(e, previous);
        out.push_back(previous);
    }
    return out;
}

// Pseudocode 84 run backwards. The sum's value makes 2^(qa / a + 1) the two
// channels' scale factors added, and the balance's, which the decoder
// doubles, makes 2^(qb / a - PAN_OFFSET) their ratio; the noise floors
// likewise, with no a. A balance value is sent from F0, which holds 0 to
// 2 * PAN_OFFSET * a / 2 for signals and 0 to PAN_OFFSET for noise.
std::optional<std::array<AspxChannelFields, 2>> AspxChannelEncoder::balanced_with(
    const AspxChannelEncoder& right, const std::array<AspxChannelFields, 2>& proposed, bool iframe) const {
    const AspxChannelFields& l = proposed[0];
    const AspxChannelFields& r = proposed[1];
    const int slots = setup_->timing.aspx_slots;
    const std::vector<int> start_l = interval_borders(l.framing, stop_prev_ - slots, slots);
    const std::vector<int> start_r = interval_borders(r.framing, right.stop_prev_ - slots, slots);
    if (start_l != start_r || l.framing.int_class != r.framing.int_class ||
        l.framing.tsg_ptr != r.framing.tsg_ptr || l.qmode_env != r.qmode_env || l.tna_mode != r.tna_mode ||
        l.envelope_freq_res != r.envelope_freq_res) {
        return std::nullopt;
    }
    const int a = l.qmode_env == 0 ? 2 : 1;
    const int top = l.qmode_env == 0 ? kMaxSignalFine : kMaxSignalCoarse;
    const std::vector<std::vector<int>> sig_l = signal_values(l);
    const std::vector<std::vector<int>> sig_r = right.signal_values(r);
    const std::vector<std::vector<int>> noise_l = noise_values(l);
    const std::vector<std::vector<int>> noise_r = right.noise_values(r);

    std::array<AspxChannelFields, 2> out{l, r};
    std::vector<std::vector<int>> sums;
    std::vector<std::vector<int>> pans;
    for (std::size_t env = 0; env < sig_l.size(); ++env) {
        std::vector<int> sum(sig_l[env].size());
        std::vector<int> pan(sig_l[env].size());
        for (std::size_t b = 0; b < sum.size(); ++b) {
            const double level_l = sig_l[env][b] / static_cast<double>(a);
            const double level_r = sig_r[env][b] / static_cast<double>(a);
            const double total = std::log2(std::exp2(level_l) + std::exp2(level_r));
            sum[b] = std::clamp(static_cast<int>(std::lround(a * (total - 1.0))), 0, top);
            const double ratio = a * (kPanOffset + level_l - level_r) / 2.0;
            pan[b] = std::clamp(static_cast<int>(std::lround(ratio)), 0, kPanOffset * a);
        }
        sums.push_back(sum);
        pans.push_back(pan);
    }
    std::vector<int> noise_sum(noise_l.front().size());
    std::vector<int> noise_pan(noise_l.front().size());
    for (std::size_t b = 0; b < noise_sum.size(); ++b) {
        // Noise floors are 2^(6 - q): the sum's q makes 2^(7 - q) their total.
        const double total = std::log2(std::exp2(-noise_l.front()[b]) + std::exp2(-noise_r.front()[b]));
        noise_sum[b] = std::clamp(static_cast<int>(std::lround(1.0 - total)), 0, kMaxNoise);
        const double ratio = (kPanOffset + noise_r.front()[b] - noise_l.front()[b]) / 2.0;
        noise_pan[b] = std::clamp(static_cast<int>(std::lround(ratio)), 0, kPanOffset);
    }

    // The sum takes the left channel's fields and delta coding; the balance
    // is coded along time only from the balance values of the last frame.
    code_envelopes(out[0], sums, noise_sum, iframe);
    const bool from_last = right.have_previous_ && right.balance_prev_ && !iframe;
    std::vector<int> previous(right.sig_prev_.begin(), right.sig_prev_.end());
    bool previous_high = right.high_prev_;
    out[1].sig.clear();
    for (std::size_t env = 0; env < pans.size(); ++env) {
        const bool high = out[1].envelope_freq_res[env] != 0;
        std::vector<int> halved = right.map_resolution(previous, previous_high, high);
        for (int& value : halved) {
            value /= 2;
        }
        out[1].sig.push_back(delta_code(pans[env], halved, env > 0 || from_last, true, out[1].qmode_env, true));
        previous = decode_values(out[1].sig.back(), halved);
        for (int& value : previous) {
            value *= 2;
        }
        previous_high = high;
    }
    std::vector<int> previous_noise(right.noise_prev_.begin(), right.noise_prev_.end());
    out[1].noise.clear();
    for (int env = 0; env < out[1].framing.num_noise(); ++env) {
        std::vector<int> halved = previous_noise;
        for (int& value : halved) {
            value /= 2;
        }
        out[1].noise.push_back(delta_code(noise_pan, halved, env > 0 || from_last, false, 0, true));
        previous_noise = decode_values(out[1].noise.back(), halved);
        for (int& value : previous_noise) {
            value *= 2;
        }
    }
    out[1].framing = out[0].framing;

    // Where the pair costs less that way.
    BitWriter apart = BitWriter::buffered();
    write_aspx_data_2ch(apart, iframe, setup_->xover_subband_offset, setup_->config, setup_->counts, false, proposed);
    BitWriter together = BitWriter::buffered();
    write_aspx_data_2ch(together, iframe, setup_->xover_subband_offset, setup_->config, setup_->counts, true, out);
    if (together.bit_position() >= apart.bit_position()) {
        return std::nullopt;
    }
    return out;
}

std::vector<int> fixfix_borders(int aspx_slots, int envelopes) {
    if (envelopes <= 1) {
        return {0, aspx_slots};
    }
    // Table 194's other rows: the halves and the quarters of 6, 8, 12, 15
    // and 16 slots, the odd ones rounded as the table has them.
    switch (aspx_slots) {
        case 6:
            return envelopes == 2 ? std::vector<int>{0, 3, 6} : std::vector<int>{0, 2, 3, 4, 6};
        case 15:
            return envelopes == 2 ? std::vector<int>{0, 8, 15} : std::vector<int>{0, 4, 8, 12, 15};
        default:
            break;
    }
    std::vector<int> out;
    for (int env = 0; env <= envelopes; ++env) {
        out.push_back(env * aspx_slots / envelopes);
    }
    return out;
}

std::vector<int> interval_borders(const AspxFramingFields& framing, int start, int aspx_slots) {
    const int num_env = framing.num_env();
    if (framing.int_class == AspxIntervalClass::kFixFix) {
        return fixfix_borders(aspx_slots, num_env);
    }
    std::vector<int> sig(at(num_env + 1));
    const bool var_start =
        framing.int_class == AspxIntervalClass::kVarFix || framing.int_class == AspxIntervalClass::kVarVar;
    const bool var_end =
        framing.int_class == AspxIntervalClass::kFixVar || framing.int_class == AspxIntervalClass::kVarVar;
    sig.front() = var_start ? start : 0;
    sig.back() = aspx_slots + (var_end ? framing.var_bord_right : 0);
    for (std::size_t i = 0; i < framing.rel_bord_left.size(); ++i) {
        sig[i + 1] = sig[i] + framing.rel_bord_left[i];
    }
    for (std::size_t i = 0; i < framing.rel_bord_right.size(); ++i) {
        sig[sig.size() - 2 - i] = sig[sig.size() - 1 - i] - framing.rel_bord_right[i];
    }
    return sig;
}

bool envelope_high_res(std::span<const int> borders, int envelope, int tsg_ptr,
                       int aspx_slots) noexcept {
    const int length = borders[at(envelope + 1)] - borders[at(envelope)];
    const bool before_ptr = envelope < tsg_ptr && aspx_slots > 8;
    // length > num_aspx_timeslots / 6 + 3.25, in integers.
    return before_ptr || 12 * length > 2 * aspx_slots + 39;
}

std::vector<int> noise_borders(const AspxFramingFields& framing, std::span<const int> borders,
                               int aspx_slots) {
    const int num_env = framing.num_env();
    if (framing.num_noise() == 1) {
        return {borders.front(), borders.back()};
    }
    if (framing.int_class == AspxIntervalClass::kFixFix) {
        return fixfix_borders(aspx_slots, 2);
    }
    int mid = 0;
    if (framing.int_class == AspxIntervalClass::kVarFix) {
        mid = framing.tsg_ptr < 0 ? 1 : num_env - 1;
    } else {
        mid = framing.tsg_ptr < 0 ? num_env - 1 : std::max(1, std::min(num_env - 1, framing.tsg_ptr));
    }
    return {borders.front(), borders[at(mid)], borders.back()};
}

void write_aspx_head(BitWriter& w, bool iframe, const AspxSetup& setup, const AspxElement& element) {
    if (iframe) {
        write_aspx_config(w, setup.config);
    }
    write_companding_control(w, element.companding);
}

void write_aspx_tail(BitWriter& w, bool iframe, const AspxSetup& setup, const AspxElement& element) {
    if (element.channels.size() == 1) {
        write_aspx_data_1ch(w, iframe, setup.xover_subband_offset, setup.config, setup.counts,
                            element.channels[0]);
        return;
    }
    write_aspx_data_2ch(w, iframe, setup.xover_subband_offset, setup.config, setup.counts, element.balance,
                        {element.channels[0], element.channels[1]});
}

}  // namespace ac4::detail
