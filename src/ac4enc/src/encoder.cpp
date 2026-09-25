#include "ac4enc/encoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

#include "asf/analysis.hpp"
#include "asf/coder.hpp"
#include "asf/layout.hpp"
#include "asf/psycho.hpp"
#include "asf/stereo.hpp"
#include "aspx/aspx_encoder.hpp"
#include "bit_writer.hpp"
#include "frame/frame_writer.hpp"

namespace ac4 {

std::string_view describe(EncodeError error) {
    switch (error) {
        case EncodeError::kInvalidConfig:
            return "the configuration is not one this encoder writes";
        case EncodeError::kInvalidInput:
            return "the input does not match the configuration, or holds a sample that is not finite";
    }
    return "unknown error";
}

namespace {

using detail::BitWriter;
using detail::FrameLayout;

constexpr int kFrameLength = 2048;  // frame_rate_index 13, at 44.1 and 48 kHz
// The silence ahead of the input: the frame's long window starts at its first
// output sample, so a frame codes input from half a frame before it to half a
// frame after it, and this delay puts the next frame's transients, which set
// the frame's last window, inside the input read before the frame is coded.
// A frame and a half, which is also what DEE's encoder gives.
constexpr int kDelay = kFrameLength * 3 / 2;
// The decoder's delay at index 13, which flush() codes enough frames to
// cover: d_pcm (Part 1 Table 188), the QMF banks' 577 samples and the six
// QMF slots the synthesis works behind (5.7.1).
constexpr int kDecoderDelay = 352 + 577 + 6 * 64;
constexpr int kQmfSlot = 64;        // samples per QMF slot
constexpr int kSubBlocks = 16;      // transient detection, a sixteenth of a frame each
constexpr int kSubBlock = kFrameLength / kSubBlocks;
constexpr double kAttackRatio = 10.0;     // 10 dB over the sub-blocks before
constexpr double kAttackFloor = 1e-7;     // per sample: nothing below -70 dBFS is an attack
// The rate loop's steps, and what one is worth: a scale factor step, 2^(1/4)
// on the quantiser's step, moves its noise by 2^(3/8). From kCapSteps on, the
// cap on each band's noise at its energy rises a step at a time as well.
constexpr int kLowestStep = -120;
constexpr int kHighestStep = 255;
constexpr int kCapSteps = 60;
constexpr double kStepDb = 1.1289;
// Under the pull, the caps on a band's noise against its energy, tightest
// first: the rate loop takes the first the budget holds. ViSQOL marks the
// holes a looser cap leaves; a tighter one than the budget holds leaves
// every band at the cap's SNR. Measured on the race's sources, 2026-09-25.
constexpr std::array<double, 4> kCaps = {0.7, 1.0, 1.4, 2.0};
// The allowance of a band the spectral frontend leaves silent: every line
// quantises to 0 at the scale factor it gives, 255.
constexpr double kSilencedAllowance = 1e200;
// Below the thresholds, how far towards the level a band's allowance is taken,
// in dB: 1 holds every band to the level, which gives the most SNR for the
// bits and leaves the quietest bands at their thresholds; 0 lowers every band
// together. Measured on the race's sources, 2026-09-25.
constexpr double kLevelWeight = 0.75;

// Below this rate a channel, CodecMode::kAuto codes in the ASPX mode, as DEE
// does from 144 kbps in stereo down.
constexpr double kAspxBelowKbps = 96.0;

// The bandwidth the SIMPLE mode codes, by bit rate per channel.
[[nodiscard]] double cutoff_hz(double kbps_per_channel) {
    if (kbps_per_channel >= 96.0) {
        return 20000.0;
    }
    if (kbps_per_channel >= 64.0) {
        return 16000.0;
    }
    if (kbps_per_channel >= 48.0) {
        return 14000.0;
    }
    return 11000.0;
}

// The first bands of a transform whose lines start below `cutoff`.
[[nodiscard]] int bands_below(int transform_length, double cutoff, int sample_rate) {
    const std::span<const std::uint16_t> offsets = detail::band_offsets(transform_length);
    const double line_hz = static_cast<double>(sample_rate) / (2.0 * transform_length);
    int bands = 0;
    while (bands + 1 < static_cast<int>(offsets.size()) &&
           static_cast<double>(offsets[static_cast<std::size_t>(bands)]) * line_hz < cutoff) {
        ++bands;
    }
    return std::min(bands, (1 << detail::max_sfb_bits(transform_length)) - 1);
}

// sequence_counter: 0 in the first frame (Part 1 Annex E.1), then 1 to 1020
// and round again from 1 (Part 1 clause 4.3.3.2.2).
[[nodiscard]] int sequence_counter(std::int64_t frame) {
    return frame == 0 ? 0 : static_cast<int>((frame - 1) % 1020) + 1;
}

}  // namespace

struct Encoder::Impl {
    EncoderConfig config{};
    detail::Analysis analysis{kFrameLength, 1};
    detail::Psychoacoustics psycho{48000, kFrameLength};
    Toc toc{};
    int fs_index = 1;
    int dialnorm_bits = 124;
    double cutoff = 20000.0;
    double bytes_per_frame = 0.0;
    double byte_carry = 0.0;

    // The input, from sample index `base` of the delayed signal on; the first
    // kDelay samples of that signal are the silence ahead of the input.
    std::vector<std::vector<double>> signal;
    std::int64_t base = 0;
    std::int64_t input_samples = 0;
    bool flushed = false;

    std::int64_t frames_out = 0;
    std::deque<FrameLayout> layouts;  // decided, for frames_out and after
    int previous_last = kFrameLength;

    // The ASPX mode: the stream's A-SPX configuration, and each channel's
    // QMF domain. With interleaving, each channel's QMF subbands the last
    // frame's A-SPX data had the spectral frontend code.
    std::optional<detail::AspxSetup> aspx;
    std::vector<detail::AspxChannelEncoder> qmf;
    std::vector<std::vector<std::pair<int, int>>> interleaved_prev;

    // With interleaving, the bands above the crossover the spectral frontend
    // leaves silent: all but those that meet `waveform_hz`, the frequency
    // ranges it codes there.
    [[nodiscard]] std::vector<std::vector<bool>> silenced_bands(
        const detail::Grouped& grouped, const FrameLayout& layout,
        const std::vector<std::pair<double, double>>& waveform_hz) const {
        std::vector<std::vector<bool>> out(grouped.offset.size());
        const double crossover = aspx ? aspx->groups.sbx * static_cast<double>(config.sample_rate_hz) / 128.0 : 0.0;
        for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
            const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
            out[g].assign(bands, false);
            if (waveform_hz.empty()) {
                continue;
            }
            const int length = layout.group_length[g];
            const std::span<const std::uint16_t> offsets = detail::band_offsets(length);
            const double line_hz = static_cast<double>(config.sample_rate_hz) / (2.0 * length);
            for (std::size_t b = 0; b < bands; ++b) {
                const double lo = offsets[b] * line_hz;
                const double hi = offsets[b + 1] * line_hz;
                const bool coded = std::ranges::any_of(
                    waveform_hz, [&](const std::pair<double, double>& range) { return lo < range.second && hi > range.first; });
                out[g][b] = lo >= crossover && !coded;
            }
        }
        return out;
    }

    [[nodiscard]] std::int64_t signal_end() const noexcept {
        return base + static_cast<std::int64_t>(signal.front().size());
    }

    [[nodiscard]] double sample(std::size_t c, std::int64_t s) const noexcept {
        if (s < base || s >= signal_end()) {
            return 0.0;
        }
        return signal[c][static_cast<std::size_t>(s - base)];
    }

    // What the spectral frontend codes: the signal, or with companding its
    // compressed low band.
    [[nodiscard]] double coded_sample(std::size_t c, std::int64_t s) const noexcept {
        if (aspx && aspx->companding) {
            return qmf[c].companded(s);
        }
        return sample(c, s);
    }

    // Analyses the QMF slots frame f needs: its interval's and the six after
    // it that a variable border can reach, and with companding those whose
    // synthesis reaches the end of its transform window.
    void analyse_qmf(std::int64_t frame) {
        std::int64_t end = detail::kQmfSlotsPerFrame * (frame + 2);
        if (aspx->companding) {
            const std::int64_t window_last = (frame + 2) * kFrameLength - 1;
            end = std::max(end, (window_last + detail::kCompandedLag) / kQmfSlot + 1);
        }
        std::array<double, kQmfSlot> chunk{};
        for (std::size_t c = 0; c < qmf.size(); ++c) {
            while (qmf[c].slots() < end) {
                const std::int64_t from = kQmfSlot * qmf[c].slots() - detail::kAnalysisLead;
                for (std::size_t i = 0; i < chunk.size(); ++i) {
                    chunk[i] = sample(c, from + static_cast<std::int64_t>(i));
                }
                qmf[c].push_slot(chunk);
            }
        }
    }

    [[nodiscard]] std::size_t aspx_bits(bool iframe, const detail::AspxElement& element) const {
        BitWriter w = BitWriter::buffered();
        detail::write_aspx_head(w, iframe, *aspx, element);
        detail::write_aspx_tail(w, iframe, *aspx, element);
        return w.bit_position();
    }

    [[nodiscard]] detail::AspxElement aspx_element() const {
        detail::AspxElement element;
        element.companding.num_chan = config.channels;
        for (int c = 0; c < config.channels; ++c) {
            element.companding.compand_on[static_cast<std::size_t>(c)] = aspx->companding;
        }
        return element;
    }

    // Transient detection over the frame's centre, where its blocks are:
    // the first difference's energy per sub-block against the four before.
    [[nodiscard]] FrameLayout decide(std::int64_t frame) const {
        const std::int64_t centre = frame * kFrameLength + kFrameLength / 2;
        std::array<double, kSubBlocks + 4> energy{};
        for (int k = -4; k < kSubBlocks; ++k) {
            double e = 0.0;
            for (std::size_t c = 0; c < signal.size(); ++c) {
                const std::int64_t start = centre + static_cast<std::int64_t>(k) * kSubBlock;
                for (std::int64_t s = start; s < start + kSubBlock; ++s) {
                    const double d = sample(c, s) - sample(c, s - 1);
                    e += d * d;
                }
            }
            energy[static_cast<std::size_t>(k + 4)] = e;
        }
        std::array<int, 2> attack{-1, -1};
        const double quietest = kAttackFloor * kSubBlock * static_cast<double>(signal.size());
        for (int k = 0; k < kSubBlocks; ++k) {
            const auto i = static_cast<std::size_t>(k + 4);
            const double before = (energy[i - 1] + energy[i - 2] + energy[i - 3] + energy[i - 4]) / 4.0;
            if (energy[i] > quietest && energy[i] > kAttackRatio * std::max(before, quietest)) {
                const auto half = static_cast<std::size_t>(k / (kSubBlocks / 2));
                if (attack[half] < 0) {
                    attack[half] = k % (kSubBlocks / 2);
                }
            }
        }
        if (attack[0] < 0 && attack[1] < 0) {
            return detail::long_layout(kFrameLength);
        }
        // An attack's half splits into eight blocks; the other half stays one
        // block of half the frame.
        const std::array<int, 2> transf_length{attack[0] >= 0 ? 0 : 3, attack[1] >= 0 ? 0 : 3};
        return detail::split_layout(kFrameLength, transf_length, attack);
    }

    [[nodiscard]] std::array<int, 2> max_sfb_for(const FrameLayout& layout) const {
        const int first = layout.window_length.front();
        const int last = layout.window_length.back();
        return {bands_below(first, cutoff, config.sample_rate_hz), bands_below(last, cutoff, config.sample_rate_hz)};
    }

    [[nodiscard]] detail::FrameFields fields_for(std::int64_t frame) const {
        detail::FrameFields fields;
        fields.sequence_counter = sequence_counter(frame);
        fields.iframe = frame % config.iframe_interval == 0;
        fields.fs_index = fs_index;
        fields.frame_rate_index = 13;
        fields.stereo = config.channels == 2;
        fields.dialnorm_bits = dialnorm_bits;
        return fields;
    }

    // The channel element for a frame: the bits of audio_data_chan(), for a
    // global scale factor offset.
    struct Coded {
        std::vector<detail::CodedTrack> tracks;
        detail::StereoChoice stereo;
        std::array<int, 2> max_sfb{};
    };

    // The channel element with no bands: what a frame falls back to when no
    // step of the rate loop fits it, and what create() checks the rate holds.
    [[nodiscard]] Coded silent(const FrameLayout& layout) const {
        Coded coded;
        coded.max_sfb = {0, 0};
        for (int c = 0; c < config.channels; ++c) {
            const detail::Grouped grouped = detail::regroup({}, layout, coded.max_sfb);
            coded.tracks.push_back(
                detail::code_track(grouped, std::vector<std::vector<int>>(grouped.offset.size()), 0, layout));
        }
        return coded;
    }

    // Part 1 Tables 20 and 22: single_channel_element() and
    // channel_pair_element() in the SIMPLE mode, or with `aspx_data` the ASPX
    // mode, whose aspx_config() (in an I-frame) and companding_control()
    // come before the channel data and its aspx_data element after.
    void write_element(BitWriter& w, const FrameLayout& layout, const Coded& coded, bool iframe,
                       const detail::AspxElement* aspx_data) const {
        const int mode = aspx_data != nullptr ? 1 : 0;
        if (config.channels == 2) {
            w.write(2, static_cast<std::uint64_t>(mode), "stereo_codec_mode");
            if (aspx_data != nullptr) {
                detail::write_aspx_head(w, iframe, *aspx, *aspx_data);
            }
            // Table 23, stereo_data() with one sf_info() for both tracks.
            w.write(1, 1, "b_enable_mdct_stereo_proc");
            detail::write_sf_info(w, layout, coded.max_sfb);
            detail::write_chparam_info(w, coded.stereo);
            detail::write_sf_data(w, coded.tracks[0], layout);
            detail::write_sf_data(w, coded.tracks[1], layout);
        } else {
            w.write(1, static_cast<std::uint64_t>(mode), "mono_codec_mode");
            if (aspx_data != nullptr) {
                detail::write_aspx_head(w, iframe, *aspx, *aspx_data);
            }
            // Table 21, mono_data(0) with the ASF.
            w.write(1, 0, "spec_frontend");
            detail::write_sf_info(w, layout, coded.max_sfb);
            detail::write_sf_data(w, coded.tracks[0], layout);
        }
        if (aspx_data != nullptr) {
            detail::write_aspx_tail(w, iframe, *aspx, *aspx_data);
        }
    }

    [[nodiscard]] EncodedFrame encode_frame(std::int64_t frame, const FrameLayout& layout, int next_first) {
        const std::size_t channels = signal.size();
        const std::int64_t start = frame * kFrameLength;
        const detail::FrameFields fields = fields_for(frame);
        Coded coded;
        coded.max_sfb = max_sfb_for(layout);

        // A-SPX's parameters for the frame's interval, and with companding
        // the compressed low band its transform window takes.
        std::optional<detail::AspxElement> aspx_data;
        if (aspx) {
            analyse_qmf(frame);
            aspx_data = aspx_element();
            for (std::size_t c = 0; c < channels; ++c) {
                aspx_data->channels.push_back(qmf[c].propose(frame, fields.iframe));
            }
            if (channels == 2 && aspx->balance) {
                const auto pair = qmf[0].balanced_with(qmf[1], {aspx_data->channels[0], aspx_data->channels[1]},
                                                       fields.iframe);
                if (pair) {
                    aspx_data->channels = {(*pair)[0], (*pair)[1]};
                    aspx_data->balance = true;
                }
            }
        }

        // With interleaving, the spectral frontend codes above the crossover
        // the groups this frame's A-SPX data marks and the last frame's,
        // whose slots its transform window overlaps, and nothing else there.
        std::vector<std::pair<double, double>> waveform_hz;
        std::vector<std::vector<std::pair<int, int>>> interleaved(channels);
        if (aspx && aspx->interleave) {
            const double subband_hz = static_cast<double>(config.sample_rate_hz) / 128.0;
            for (std::size_t c = 0; c < channels; ++c) {
                interleaved[c] = qmf[c].interleaved_subbands(aspx_data->channels[c]);
                for (const auto& ranges : {interleaved[c], interleaved_prev[c]}) {
                    for (const auto& [first, last] : ranges) {
                        waveform_hz.emplace_back(first * subband_hz, last * subband_hz);
                    }
                }
            }
            double top = 0.0;
            for (const auto& range : waveform_hz) {
                top = std::max(top, range.second);
            }
            const int first_length = layout.window_length.front();
            const int last_length = layout.window_length.back();
            coded.max_sfb = {std::max(coded.max_sfb[0], bands_below(first_length, top, config.sample_rate_hz)),
                             std::max(coded.max_sfb[1], bands_below(last_length, top, config.sample_rate_hz))};
        }

        std::vector<detail::Grouped> grouped(channels);
        std::vector<std::vector<std::vector<double>>> allowed(channels);
        std::vector<std::vector<std::vector<bool>>> silenced(channels);
        std::vector<double> window(2 * kFrameLength);
        std::vector<double> spectrum;
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t i = 0; i < window.size(); ++i) {
                window[i] = coded_sample(c, start + static_cast<std::int64_t>(i));
            }
            analysis.transform(window, layout, previous_last, next_first, spectrum);
            grouped[c] = detail::regroup(spectrum, layout, coded.max_sfb);
            allowed[c] = psycho.thresholds(grouped[c], layout);
            silenced[c] = silenced_bands(grouped[c], layout, waveform_hz);
            for (std::size_t g = 0; g < allowed[c].size(); ++g) {
                for (std::size_t b = 0; b < allowed[c][g].size(); ++b) {
                    if (silenced[c][g][b]) {
                        allowed[c][g][b] = kSilencedAllowance;
                    }
                }
            }
        }
        if (channels == 2) {
            coded.stereo = detail::choose_stereo(grouped[0], grouped[1], allowed[0], allowed[1]);
        }
        // The frame's size, and the bits the channel element may take.
        const double exact = byte_carry + bytes_per_frame;
        const auto frame_bytes = static_cast<std::size_t>(exact);
        const std::size_t overhead = detail::frame_overhead_bits(fields, frame_bytes);
        std::size_t element = channels == 2 ? 3 + detail::chparam_info_bits(coded.stereo) : 2;
        element += detail::sf_info_bits(layout, coded.max_sfb);
        if (aspx_data) {
            element += aspx_bits(fields.iframe, *aspx_data);
        }
        const std::size_t budget = 8 * frame_bytes > overhead + element ? 8 * frame_bytes - overhead - element : 0;

        // The rate loop: a level of noise per line, top_level * 10^(kStepDb p
        // / 10) at step p, and two laws that bring the bands' allowances to
        // it. Where the budget holds every band at its masking threshold (p =
        // 0), the bits left lower the level: each band whose allowance per
        // line is over it is brought kLevelWeight of the way to it, in dB, so
        // that the bits go first where the noise is loudest. Where it does
        // not, every band is pulled kLevelWeight of the way to the level from
        // above or below: the loudest bands keep noise under their thresholds,
        // as a waveform coder at a low rate must, and the quietest give up
        // theirs first. No band's noise is let past its energy, which would
        // leave a hole, until the last steps (kCapSteps on) relax that too.
        double top_level = 0.0;  // the highest allowance per line
        std::vector<std::vector<std::vector<double>>> energy(channels);
        for (std::size_t c = 0; c < channels; ++c) {
            energy[c].resize(allowed[c].size());
            for (std::size_t g = 0; g < allowed[c].size(); ++g) {
                energy[c][g].assign(allowed[c][g].size(), 0.0);
                for (std::size_t b = 0; b < allowed[c][g].size(); ++b) {
                    if (silenced[c][g][b]) {
                        continue;
                    }
                    const std::size_t begin = grouped[c].offset[g][b];
                    const std::size_t end = grouped[c].offset[g][b + 1];
                    top_level = std::max(top_level, allowed[c][g][b] / static_cast<double>(end - begin));
                    for (std::size_t k = begin; k < end; ++k) {
                        energy[c][g][b] += grouped[c].lines[k] * grouped[c].lines[k];
                    }
                }
            }
        }
        std::vector<std::vector<std::vector<int>>> sf(channels);
        std::vector<std::vector<double>> capped;
        double kappa = kCaps.front();
        const auto set_step = [&](bool pull, int step) {
            const double level = top_level * std::pow(10.0, kStepDb * step / 10.0);
            const double cap = kappa * (step > kCapSteps ? std::pow(10.0, kStepDb * (step - kCapSteps) / 10.0) : 1.0);
            for (std::size_t c = 0; c < channels; ++c) {
                capped = allowed[c];
                for (std::size_t g = 0; g < capped.size(); ++g) {
                    for (std::size_t b = 0; b < capped[g].size(); ++b) {
                        if (silenced[c][g][b]) {
                            continue;
                        }
                        const auto lines = static_cast<double>(grouped[c].offset[g][b + 1] - grouped[c].offset[g][b]);
                        double& allowance = capped[g][b];
                        if (!pull) {
                            if (allowance > level * lines) {
                                allowance *= std::pow(level * lines / allowance, kLevelWeight);
                            }
                            continue;
                        }
                        allowance *= std::pow(level * lines / allowance, kLevelWeight);
                        if (energy[c][g][b] > 0.0) {
                            allowance = std::min(allowance, cap * energy[c][g][b]);
                        }
                    }
                }
                sf[c] = detail::scale_factors_for(grouped[c], capped);
            }
        };
        const auto bits_at = [&](bool pull, int step) {
            set_step(pull, step);
            std::size_t total = 0;
            for (std::size_t c = 0; c < channels; ++c) {
                total += detail::code_track(grouped[c], sf[c], 0, layout).bits();
            }
            return total;
        };
        // The lowest step of a law that fits: the bits fall as it rises.
        const auto lowest_fitting = [&](bool pull, int low, int high) {
            while (low < high) {
                const int mid = low + (high - low) / 2;
                if (bits_at(pull, mid) <= budget) {
                    high = mid;
                } else {
                    low = mid + 1;
                }
            }
            return low;
        };
        const auto write = [&]() {
            BitWriter audio = BitWriter::buffered();
            write_element(audio, layout, coded, fields.iframe, aspx_data ? &*aspx_data : nullptr);
            return detail::write_frame(fields, audio, frame_bytes, config.trace);
        };
        const auto write_at = [&](bool pull, int step) {
            set_step(pull, step);
            coded.tracks.clear();
            for (std::size_t c = 0; c < channels; ++c) {
                coded.tracks.push_back(detail::code_track(grouped[c], sf[c], 0, layout));
            }
            return write();
        };
        std::optional<std::vector<std::byte>> raw;
        if (bits_at(false, 0) <= budget) {
            for (int step = lowest_fitting(false, kLowestStep, 0); step <= 0 && !raw; ++step) {
                raw = write_at(false, step);
            }
        }
        // The pull, with the tightest cap on each band's noise the budget
        // holds before the last steps relax it.
        int first = kCapSteps + 1;
        for (const double k : kCaps) {
            if (raw) {
                break;
            }
            kappa = k;
            const int step = lowest_fitting(true, kLowestStep, kCapSteps);
            if (bits_at(true, step) <= budget) {
                first = step;
                break;
            }
        }
        if (!raw) {
            int step = first;
            if (first > kCapSteps) {
                // No cap held: the last steps relax the loosest.
                kappa = kCaps.back();
                step = lowest_fitting(true, kCapSteps, kHighestStep);
            }
            for (; step <= kHighestStep && !raw; ++step) {
                raw = write_at(true, step);
            }
        }
        if (!raw) {
            // Lines so far past full scale that the coarsest step still codes
            // more than the frame holds, or A-SPX data that leave too little:
            // the frame goes out with no bands, and then with the A-SPX data
            // that cost least.
            coded = silent(layout);
            raw = write();
            for (const bool silence : {false, true}) {
                if (raw || !aspx_data) {
                    break;
                }
                for (std::size_t c = 0; c < channels; ++c) {
                    aspx_data->channels[c] = qmf[c].fallback(fields.iframe, silence);
                }
                aspx_data->balance = false;
                raw = write();
            }
        }
        if (aspx_data) {
            for (std::size_t c = 0; c < channels; ++c) {
                qmf[c].commit(frame, aspx_data->channels[c], aspx_data->balance && c == 1);
                qmf[c].drop_before_frame(frame + 1);
                interleaved_prev[c] = qmf[c].interleaved_subbands(aspx_data->channels[c]);
            }
        }
        byte_carry = exact - static_cast<double>(frame_bytes);
        previous_last = layout.window_length.back();
        EncodedFrame out;
        out.raw_ac4_frame = std::move(raw).value();
        out.samples = kFrameLength;
        out.iframe = fields.iframe;
        return out;
    }

    // The frames the input read so far lets through. A frame needs the input
    // to half a frame past its window, where the next frame's transients are.
    [[nodiscard]] std::vector<EncodedFrame> drain() {
        std::vector<EncodedFrame> frames;
        for (;;) {
            const std::int64_t frame = frames_out;
            if (flushed && frame * kFrameLength >= input_samples + kDelay + kDecoderDelay) {
                break;
            }
            const std::int64_t needed = (frame + 2) * kFrameLength + kFrameLength / 2;
            if (!flushed && signal_end() < needed) {
                break;
            }
            while (static_cast<std::int64_t>(layouts.size()) < 2) {
                layouts.push_back(decide(frame + static_cast<std::int64_t>(layouts.size())));
            }
            const FrameLayout layout = layouts.front();
            const int next_first = layouts[1].window_length.front();
            frames.push_back(encode_frame(frame, layout, next_first));
            layouts.pop_front();
            ++frames_out;
            // Nothing before the next frame's window is read again.
            const std::int64_t keep_from = (frames_out * kFrameLength) - kSubBlock * 5;
            if (keep_from > base) {
                const auto drop = static_cast<std::size_t>(std::min(keep_from - base, signal_end() - base));
                for (std::vector<double>& channel : signal) {
                    channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(drop));
                }
                base += static_cast<std::int64_t>(drop);
            }
        }
        return frames;
    }
};

std::expected<Encoder, EncodeError> Encoder::create(const EncoderConfig& config) {
    if (config.channels != 1 && config.channels != 2) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.sample_rate_hz != 48000 && config.sample_rate_hz != 44100) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.iframe_interval < 1 || config.bitrate_kbps < 8 || config.bitrate_kbps > 3000) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (!(config.dialnorm_db <= 0.0 && config.dialnorm_db >= -31.75)) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    if (config.codec_mode != CodecMode::kAuto && config.codec_mode != CodecMode::kSimple &&
        config.codec_mode != CodecMode::kAspx) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->psycho = detail::Psychoacoustics(config.sample_rate_hz, kFrameLength);
    impl->fs_index = config.sample_rate_hz == 48000 ? 1 : 0;
    impl->dialnorm_bits = static_cast<int>(std::lround(-config.dialnorm_db * 4.0));
    impl->bytes_per_frame = static_cast<double>(config.bitrate_kbps) * 1000.0 * kFrameLength /
                            (static_cast<double>(config.sample_rate_hz) * 8.0);
    const double kbps_per_channel = static_cast<double>(config.bitrate_kbps) / config.channels;
    impl->cutoff = cutoff_hz(kbps_per_channel);
    if (config.codec_mode == CodecMode::kAspx ||
        (config.codec_mode == CodecMode::kAuto && kbps_per_channel < kAspxBelowKbps)) {
        impl->aspx = detail::aspx_setup_for(kbps_per_channel, config.sample_rate_hz);
        if (!impl->aspx) {
            return std::unexpected(EncodeError::kInvalidConfig);
        }
        impl->aspx->varvar = config.experimental.aspx_varvar;
        impl->aspx->balance = config.experimental.aspx_balance;
        impl->aspx->interleave = config.experimental.aspx_interleave;
        impl->interleaved_prev.resize(static_cast<std::size_t>(config.channels));
        // The spectral frontend codes up to the crossover, subband sbx of 64
        // across half the sampling rate.
        impl->cutoff = static_cast<double>(impl->aspx->groups.sbx) * config.sample_rate_hz / 128.0;
        for (int c = 0; c < config.channels; ++c) {
            impl->qmf.emplace_back(*impl->aspx);
        }
    }
    impl->signal.assign(static_cast<std::size_t>(config.channels),
                        std::vector<double>(static_cast<std::size_t>(kDelay), 0.0));

    // The rate must hold a frame with no bands and no A-SPX energy, in the
    // smaller of the sizes it gives frames, whatever the frame's blocks: that
    // is what a frame falls back to. The table of contents is read back from
    // the first such frame: what every frame carries but its counter and
    // sizes.
    const detail::FrameFields fields = impl->fields_for(0);
    const auto frame_bytes = static_cast<std::size_t>(impl->bytes_per_frame);
    std::optional<detail::AspxElement> aspx_data;
    if (impl->aspx) {
        aspx_data = impl->aspx_element();
        for (const detail::AspxChannelEncoder& channel : impl->qmf) {
            aspx_data->channels.push_back(channel.fallback(true, true));
        }
    }
    std::optional<std::vector<std::byte>> raw;
    for (const FrameLayout& layout :
         {detail::long_layout(kFrameLength), detail::split_layout(kFrameLength, {0, 0}, {0, 0})}) {
        BitWriter silent = BitWriter::buffered();
        impl->write_element(silent, layout, impl->silent(layout), fields.iframe, aspx_data ? &*aspx_data : nullptr);
        auto written = detail::write_frame(fields, silent, frame_bytes, {});
        if (!written) {
            return std::unexpected(EncodeError::kInvalidConfig);  // the rate cannot hold a frame
        }
        if (!raw) {
            raw = std::move(written);
        }
    }
    const auto parsed = parse_raw_frame(*raw);
    if (!parsed) {
        return std::unexpected(EncodeError::kInvalidConfig);
    }
    impl->toc = parsed->toc;
    // What the table of contents does not carry, for build_dac4(): the
    // encoder writes no dialogue enhancement data and no immersive audio.
    for (PresentationInfoV1& presentation : impl->toc.presentations_v1) {
        presentation.de_indicator = false;
        presentation.immersive_audio_indicator = false;
    }
    return Encoder(std::move(impl));
}

Encoder::Encoder(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::encode(
    std::span<const std::span<const float>> channels) {
    if (impl_->flushed || channels.size() != impl_->signal.size()) {
        return std::unexpected(EncodeError::kInvalidInput);
    }
    const std::size_t count = channels.front().size();
    for (const std::span<const float> channel : channels) {
        if (channel.size() != count) {
            return std::unexpected(EncodeError::kInvalidInput);
        }
        for (const float x : channel) {
            if (!std::isfinite(x)) {
                return std::unexpected(EncodeError::kInvalidInput);
            }
        }
    }
    for (std::size_t c = 0; c < channels.size(); ++c) {
        std::vector<double>& buffer = impl_->signal[c];
        buffer.reserve(buffer.size() + count);
        for (const float x : channels[c]) {
            buffer.push_back(static_cast<double>(x));
        }
    }
    impl_->input_samples += static_cast<std::int64_t>(count);
    return impl_->drain();
}

std::expected<std::vector<EncodedFrame>, EncodeError> Encoder::flush() {
    if (impl_->flushed) {
        return std::vector<EncodedFrame>{};
    }
    impl_->flushed = true;
    return impl_->drain();
}

const Toc& Encoder::toc() const noexcept {
    return impl_->toc;
}

CodecMode Encoder::codec_mode() const noexcept {
    return impl_->aspx ? CodecMode::kAspx : CodecMode::kSimple;
}

int Encoder::delay_samples() const noexcept {
    return kDelay;
}

}  // namespace ac4
