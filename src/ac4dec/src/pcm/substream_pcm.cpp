#include "pcm/substream_pcm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "aspx/hf_generator.hpp"
#include "pcm/asf_reconstruct.hpp"
#include "pcm/companding.hpp"
#include "pcm/immersive.hpp"
#include "pcm/multichannel.hpp"
#include "pcm/snf_random.hpp"
#include "pcm/stereo.hpp"

namespace ac4::detail {
namespace {

// Table 188, d_pcm by frame_rate_index: 23.976 to 120 fps, then index 13,
// which is 23.4375 fps at 48 kHz and 21.5332 fps at 44.1 kHz and takes 352 at
// both.
constexpr std::array<int, 14> kAlignmentDelay = {288, 288, 352, 96, 96, 960, 960,
                                                 1056, 672, 672, 1312, 864, 864, 352};

// Table 188, d_ctrl by frame_rate_index: how many frames the QMF-domain
// control data waits for its signal.
constexpr std::array<int, 14> kControlDelay = {1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 1};

// The inverse transform as Pseudocodes 60 to 64 print it, with no "factor of
// 2" from the informative example after Table 187, puts full scale at 2^15:
// DEE's streams of a -20 dBFS tone decode to 0.1 * 32 768 within 0.01 dB. The
// QMF domain works at that scale, and the output is scaled to full scale 1.0.
// See src/ac4dec/ERRATA.md, "Full scale, and the overlap-add's factor of two".
constexpr double kFullScale = 32768.0;

// The QMF analysis and synthesis banks together (tests/ac4core).
constexpr int kQmfPairDelay = 577;

// Output far beyond full scale comes only from streams that are not audio;
// this bound keeps the conversion to float defined.
constexpr double kOutputLimit = 1e9;

constexpr std::size_t kSubbands = dsp::kQmfSubbands;

// The most channels an element here has (7.1.4), and aspx_data elements (the
// immersive element's six in ASPX_SCPL).
constexpr std::size_t kMaxChannels = 12;
constexpr std::size_t kMaxUnits = kMaxAspxElements;

// The chparam_info()s one channel data element holds: five_channel_data()'s.
constexpr std::size_t kMaxChparams = 5;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Whether an element of `kind` in `mode` applies A-CPL: the Part 1 A-CPL
// modes, and the immersive element's ASPX_ACPL_1 and 2 in full decoding (Part
// 2 clause 4.8.3.14; core decoding applies a gain instead).
[[nodiscard]] bool uses_acpl(ElementKind kind, int mode, DecodingMode decoding) noexcept {
    if (kind == ElementKind::kImmersive) {
        return decoding == DecodingMode::kFull &&
               (mode == immersive_mode::kAspxAcpl1 || mode == immersive_mode::kAspxAcpl2);
    }
    return mode == codec_mode::kAspxAcpl1 || mode == codec_mode::kAspxAcpl2 || mode == codec_mode::kAspxAcpl3;
}

// Whether an element of `kind` in `mode` carries A-SPX data.
[[nodiscard]] bool uses_aspx(ElementKind kind, int mode) noexcept {
    return kind == ElementKind::kImmersive ? mode != immersive_mode::kScpl
                                           : mode != codec_mode::kSimple;
}

// The chparam_info()s a processed channel data element of `count` tracks
// holds: one for a pair, two for three tracks, four and five for the others.
[[nodiscard]] std::size_t chparams_of(int count) noexcept {
    switch (count) {
        case 2:
            return 1;
        case 3:
            return 2;
        default:
            return static_cast<std::size_t>(count);
    }
}

}  // namespace

int SubstreamPcm::delay_samples() const noexcept {
    return delay_ + kQmfPairDelay + hfgen_ * dsp::kQmfSubbands;
}

void SubstreamPcm::reset() {
    for (Channel& channel : channels_) {
        channel.synthesis.reset();
        std::ranges::fill(channel.delay, 0.0);
        channel.analysis.reset();
        std::ranges::fill(channel.ext, QmfValue{});
        channel.aspx = AspxChannelState{};
    }
    for (Ghost& ghost : ghosts_) {
        ghost.aspx = AspxChannelState{};
    }
    scpl_mode_.reset();
    for (Output& output : outputs_) {
        output.synthesis.reset();
        if (output.converter) {
            output.converter->reset();
        }
    }
    held_.clear();
    master_.reset();
    acpl_.reset();
    acpl_history_ = {};
    ajcc_.reset();
    ajcc_history_ = {};
    decoded_mode_.reset();
    applied_mode_.reset();
    converter_phase_.reset();
    de_.reset();
    drc_.reset();
    downmix_.reset();
    last_spectra_.clear();
    last_lengths_.clear();
    losses_ = 0;
}

int SubstreamPcm::channel_of(Speaker speaker) const noexcept {
    for (std::size_t c = 0; c < speakers_.size(); ++c) {
        if (speakers_[c] == speaker) {
            return static_cast<int>(c);
        }
    }
    return -1;
}

void SubstreamPcm::configure_outputs(const SubstreamContext& ctx, const OutputConfig& output) {
    // The immersive element's source layout, which its presence flags give
    // (Part 2 clauses 6.3.2.7.3 to 6.3.2.7.5), for the renderer.
    std::optional<ImmersiveLayout> layout;
    if (is_immersive(ch_mode_)) {
        layout = ImmersiveLayout{.backs = ctx.b_4_back_channels_present,
                                 .tops = ctx.top_channels_present,
                                 .lfe = ch_mode_ == ch_mode::k7_1_4,
                                 .decoding = decoding_};
    }
    if (outputs_valid_ && add_ch_base_ == ctx.add_ch_base && downmix_target_ == output.downmix &&
        mix_lfe_ == output.mix_lfe && layout_ == layout) {
        return;
    }
    add_ch_base_ = ctx.add_ch_base;
    downmix_target_ = output.downmix;
    mix_lfe_ = output.mix_lfe;
    layout_ = layout;
    drc_.configure(internal_rate_, slots_, speakers_, add_ch_base_, layout_.has_value());
    downmix_.configure(speakers_, add_ch_base_, downmix_target_, mix_lfe_, layout_);
    outputs_.clear();
    for (std::size_t o = 0; o < downmix_.speakers().size(); ++o) {
        Output out{.synthesis = {}, .converter = {}};
        if (converter_filter_) {
            out.converter.emplace(converter_filter_);
        }
        outputs_.push_back(std::move(out));
    }
    // New converters start their grid at the next frame's phase.
    converter_phase_.reset();
    outputs_valid_ = true;
}

ParseResult SubstreamPcm::configure(const SubstreamContext& ctx, DecodingMode decoding) {
    const std::span<const Speaker> speakers = speakers_of(ctx.ch_mode, decoding);
    if (speakers.empty()) {
        return fail(DecodeError::kUnsupported, "this channel mode is not decoded to PCM yet");
    }
    if (full_length_ == ctx.frame_len_base && ch_mode_ == ctx.ch_mode &&
        frame_rate_index_ == ctx.frame_rate_index && fs_index_ == ctx.fs_index &&
        decoding_ == decoding && transforms_.has_value()) {
        return {};
    }
    transforms_.emplace(ctx.frame_len_base, 1);
    if (!transforms_->valid() || ctx.frame_len_base % dsp::kQmfSubbands != 0) {
        transforms_.reset();
        return fail(DecodeError::kInvalidStream, "a frame length with no transform");
    }
    full_length_ = ctx.frame_len_base;
    ch_mode_ = ctx.ch_mode;
    frame_rate_index_ = ctx.frame_rate_index;
    fs_index_ = ctx.fs_index;
    decoding_ = decoding;
    delay_ = kAlignmentDelay[static_cast<std::size_t>(ctx.frame_rate_index)];
    control_delay_ = kControlDelay[static_cast<std::size_t>(ctx.frame_rate_index)];
    slots_ = full_length_ / dsp::kQmfSubbands;
    ts_in_ats_ = aspx::num_ts_in_ats(full_length_);
    hfgen_ = aspx::ts_offset_hfgen(full_length_);
    const int ext_slots = aspx::kTsOffsetHfadj + hfgen_ + slots_;
    speakers_ = speakers;
    // Part 1 clause 6.2.15: 48 kHz from the internal rate.
    const ResamplingRatio ratio = resampling_ratio(ctx.frame_rate_index);
    converter_filter_.reset();
    if (ratio.up != ratio.down) {
        converter_filter_ = std::make_shared<const dsp::ResamplerFilter>(ratio.up, ratio.down);
    }
    channels_.clear();
    for (std::size_t c = 0; c < speakers_.size(); ++c) {
        Channel channel{.synthesis = dsp::ChannelSynthesis<double>(full_length_),
                        .delay = std::vector<double>(static_cast<std::size_t>(delay_), 0.0),
                        .analysis = {},
                        .ext = std::vector<QmfValue>(at(ext_slots) * kSubbands),
                        .out = std::vector<QmfValue>(at(slots_) * kSubbands),
                        .aspx = {}};
        channels_.push_back(std::move(channel));
    }
    time_.assign(speakers_.size(), std::vector<double>(at(full_length_), 0.0));
    // Core decoding's ASPX_SCPL takes the first channel of four of its six
    // aspx_data elements; the second's state and matrices are kept here.
    ghosts_.clear();
    if (is_immersive(ctx.ch_mode) && decoding == DecodingMode::kCore) {
        ghosts_.resize(kMaxAspxElements);
        for (const AspxUnit& unit : aspx_units(ctx.ch_mode, immersive_mode::kAspxScpl, decoding)) {
            if (unit.first_only) {
                Ghost& ghost = ghosts_[at(unit.index)];
                ghost.ext.assign(at(ext_slots) * kSubbands, QmfValue{});
                ghost.out.assign(at(slots_) * kSubbands, QmfValue{});
            }
        }
    }
    scpl_mode_.reset();
    held_.clear();
    master_.reset();
    acpl_.reset();
    acpl_history_ = {};
    ajcc_.reset();
    ajcc_history_ = {};
    decoded_mode_.reset();
    applied_mode_.reset();
    converter_phase_.reset();
    de_.configure(slots_, speakers_);
    // The QMF banks run at the internal rate.
    const double base_rate = ctx.fs_index == 0 ? 44100.0 : 48000.0;
    internal_rate_ = base_rate * static_cast<double>(ratio.down) / static_cast<double>(ratio.up);
    // The output stages follow on the frame's first configure_outputs().
    outputs_valid_ = false;
    last_spectra_.clear();
    last_lengths_.clear();
    losses_ = 0;
    return {};
}

// What one frame's A-SPX data would fail on when applied a frame later is
// checked now, before anything moves on: that the element carries the
// aspx_data elements and companding_control() Tables 212 and 213 give its
// codec mode, and each one's tables and interval.
ParseResult SubstreamPcm::check_control(const SubstreamContext& ctx, const ChannelElement& element) const {
    if (!uses_aspx(element.kind, element.codec_mode)) {
        return {};
    }
    const std::vector<AspxUnit> units = aspx_units(ctx.ch_mode, element.codec_mode, decoding_);
    const std::size_t companded = companded_speakers(ctx.ch_mode, element.codec_mode).size();
    const auto pairs = static_cast<std::size_t>(std::ranges::count_if(units, &AspxUnit::pair));
    const std::size_t singles = units.size() - pairs;
    const bool companding = companded != 0;
    if (!element.aspx_config || element.aspx_2ch.size() != pairs || element.aspx_1ch.size() != singles ||
        element.companding.has_value() != companding ||
        (companding && element.companding->num_chan != static_cast<int>(companded))) {
        return fail(DecodeError::kInvalidStream, "a channel element without the A-SPX or companding data of its mode");
    }
    for (const AspxUnit& unit : units) {
        AspxFrame frame{.config = &*element.aspx_config,
                        .xover_subband_offset = 0,
                        .balance = false,
                        .master_reset = false,
                        .base_48k = ctx.fs_index == 1,
                        .num_qmf_timeslots = slots_,
                        .num_ts_in_ats = ts_in_ats_,
                        .ts_offset_hfgen = hfgen_};
        std::array<const AspxChannel*, 2> data{};
        if (unit.pair) {
            const AspxData2ch& two = element.aspx_2ch[at(unit.index)];
            frame.xover_subband_offset = two.xover_subband_offset;
            frame.balance = two.balance;
            data = {&two.channels[0], &two.channels[1]};
        } else {
            const AspxData1ch& one = element.aspx_1ch[at(unit.index)];
            frame.xover_subband_offset = one.xover_subband_offset;
            data[0] = &one.channel;
        }
        if (auto ok = check_aspx(frame, std::span<const AspxChannel* const>(data).first(unit.pair ? 2 : 1)); !ok) {
            return ok;
        }
    }
    return {};
}

// Below the crossover and everywhere in SIMPLE mode: the analysis delayed by
// ts_offset_hfgen slots, the history the synthesis works behind (5.7.1).
void SubstreamPcm::pass_through(Channel& channel) const {
    std::copy_n(channel.ext.begin() + static_cast<std::ptrdiff_t>(at(aspx::kTsOffsetHfadj) * kSubbands),
                at(slots_) * kSubbands, channel.out.begin());
    channel.aspx.y_prev_slots = 0;
}

void SubstreamPcm::pass_through() {
    for (Channel& channel : channels_) {
        pass_through(channel);
    }
}

SubstreamPcm::UnitIo SubstreamPcm::unit_io(const AspxUnit& unit, const Control& control, bool master_reset) {
    UnitIo out;
    out.frame = AspxFrame{.config = &*control.aspx_config,
                          .xover_subband_offset = 0,
                          .balance = false,
                          .master_reset = master_reset,
                          .base_48k = fs_index_ == 1,
                          .num_qmf_timeslots = slots_,
                          .num_ts_in_ats = ts_in_ats_,
                          .ts_offset_hfgen = hfgen_};
    std::array<const AspxChannel*, 2> data{};
    if (unit.pair) {
        const AspxData2ch& two = control.aspx_2ch[at(unit.index)];
        out.frame.xover_subband_offset = two.xover_subband_offset;
        out.frame.balance = two.balance;
        data = {&two.channels[0], &two.channels[1]};
    } else {
        const AspxData1ch& one = control.aspx_1ch[at(unit.index)];
        out.frame.xover_subband_offset = one.xover_subband_offset;
        data[0] = &one.channel;
    }
    out.count = unit.pair ? 2 : 1;
    for (std::size_t c = 0; c < out.count; ++c) {
        if (unit.first_only && c == 1) {
            Ghost& ghost = ghosts_[at(unit.index)];
            out.channels[c] = -1;
            out.io[c] = AspxChannelIo{
                .data = data[c], .state = &ghost.aspx, .ext = ghost.ext, .out = ghost.out};
            continue;
        }
        const int index = channel_of(unit.speakers[c]);
        Channel& channel = channels_[at(index)];
        out.channels[c] = index;
        out.io[c] = AspxChannelIo{.data = data[c], .state = &channel.aspx, .ext = channel.ext, .out = channel.out};
    }
    return out;
}

void SubstreamPcm::apply(const Control& control) {
    if (control.new_source) {
        // The new source's first frame takes none of the old one's envelopes
        // as the base of its differences along time (ERRATA.md, "A change of
        // source"); the signal and the generators carry on.
        const auto forget = [](AspxChannelState& state) {
            state.have_previous = false;
            state.qscf_sig_prev = {};
            state.qscf_noise_prev = {};
        };
        for (Channel& channel : channels_) {
            forget(channel.aspx);
        }
        for (Ghost& ghost : ghosts_) {
            forget(ghost.aspx);
        }
    }
    if (!uses_aspx(control.kind, control.codec_mode) || !control.aspx_config) {
        pass_through();
        applied_mode_ = control.codec_mode;
        return;
    }
    units_ = aspx_units(ch_mode_, control.codec_mode, decoding_);
    companded_.clear();
    for (const Speaker speaker : companded_speakers(ch_mode_, control.codec_mode)) {
        companded_.push_back(channel_of(speaker));
    }
    const AspxConfig& config = *control.aspx_config;
    // 5.7.6.3.1.1: master_reset when the master table's parameters differ
    // from the last configuration's, and at the first.
    const std::array<int, 3> master = {config.master_freq_scale, config.start_freq, config.stop_freq};
    const bool master_reset = !master_ || *master_ != master;
    master_ = master;

    std::array<UnitIo, kMaxUnits> units{};
    std::array<aspx::SubbandGroups, kMaxUnits> groups{};
    std::array<aspx::PatchTables, kMaxUnits> patches{};
    for (std::size_t u = 0; u < units_.size(); ++u) {
        units[u] = unit_io(units_[u], control, master_reset);
        if (!aspx_tables(units[u].frame, groups[u], patches[u])) {
            pass_through();  // check_control() refused this a frame ago
            return;
        }
    }

    // Companding first (Figure 6), over each channel's own crossover and
    // interval, in companding_control()'s order (Table 212).
    if (control.companding && !companded_.empty()) {
        std::array<CompandingChannel, 5> companded{};
        for (std::size_t k = 0; k < companded_.size(); ++k) {
            for (std::size_t u = 0; u < units_.size(); ++u) {
                for (std::size_t c = 0; c < units[u].count; ++c) {
                    if (units[u].channels[c] == companded_[k]) {
                        companded[k] =
                            CompandingChannel{.ext = channels_[at(companded_[k])].ext,
                                              .sb1 = groups[u].sbx,
                                              .interval = aspx_interval(units[u].io[c].data->framing, ts_in_ats_)};
                    }
                }
            }
        }
        // 5.7.5.2: from acpl_qmf_band in ASPX_ACPL_1, where the pair below it
        // is mid-side coded. The immersive element compands in ASPX_AJCC
        // alone, from subband 0.
        const int sb0 = control.kind != ElementKind::kImmersive &&
                                control.codec_mode == codec_mode::kAspxAcpl1 && control.acpl &&
                                control.acpl->module_count > 0
                            ? control.acpl->modules[0].qmf_band
                            : 0;
        apply_companding(*control.companding, sb0, kFullScale,
                         std::span<const CompandingChannel>(companded).first(companded_.size()));
    }

    // A-SPX for each aspx_data element's channels; what none carries (the
    // LFE, and the immersive element's residuals in ASPX_ACPL_1) passes
    // through.
    std::array<bool, kMaxChannels> carried{};
    for (std::size_t u = 0; u < units_.size(); ++u) {
        UnitIo& unit = units[u];
        const bool decoded = static_cast<bool>(decode_aspx(unit.frame, std::span<AspxChannelIo>(unit.io).first(unit.count)));
        for (std::size_t c = 0; c < unit.count; ++c) {
            if (unit.channels[c] < 0) {
                continue;  // a ghost
            }
            carried[at(unit.channels[c])] = true;
            if (!decoded) {
                pass_through(channels_[at(unit.channels[c])]);
            }
        }
    }
    for (std::size_t c = 0; c < channels_.size(); ++c) {
        if (!carried[c]) {
            pass_through(channels_[c]);
        }
    }
    if (control.kind == ElementKind::kImmersive) {
        apply_immersive_gains(control, std::span<const UnitIo>(units).first(units_.size()),
                              std::span<const aspx::SubbandGroups>(groups).first(units_.size()));
    }

    // A-CPL on what A-SPX made (Figure 6, Table 214; Part 2 Table 12).
    if (uses_acpl(control.kind, control.codec_mode, decoding_) && control.acpl) {
        if (applied_mode_ != control.codec_mode) {
            acpl_.reset();
        }
        matrices_.clear();
        for (Channel& channel : channels_) {
            matrices_.push_back(&channel.out);
        }
        acpl_.apply(ch_mode_, control.add_ch_base, control.kind, control.codec_mode, *control.acpl, slots_,
                    AcplChannels{.speakers = speakers_, .matrices = matrices_});
    }
    // A-JCC on what A-SPX made (Part 2 clause 4.8.3.12).
    if (control.kind == ElementKind::kImmersive &&
        control.codec_mode == immersive_mode::kAspxAjcc && control.ajcc) {
        if (applied_mode_ != control.codec_mode) {
            ajcc_.reset();
        }
        matrices_.clear();
        for (Channel& channel : channels_) {
            matrices_.push_back(&channel.out);
        }
        ajcc_.apply(decoding_, *control.ajcc, slots_,
                    AcplChannels{.speakers = speakers_, .matrices = matrices_});
    }
    applied_mode_ = control.codec_mode;
}

void SubstreamPcm::apply_immersive_gains(const Control& control, std::span<const UnitIo> units,
                                         std::span<const aspx::SubbandGroups> groups) {
    // Every channel but the LFE comes out of A-SPX in the modes that apply a
    // gain; each takes its own unit's sbx.
    for (std::size_t u = 0; u < units.size(); ++u) {
        for (std::size_t c = 0; c < units[u].count; ++c) {
            const int index = units[u].channels[c];
            if (index < 0) {
                continue;
            }
            const BandGains gains =
                immersive_gains(control.codec_mode, decoding_, speakers_[at(index)]);
            if (gains.low != 1.0 || gains.high != 1.0) {
                apply_band_gains(channels_[at(index)].out, slots_, groups[u].sbx, gains);
            }
        }
    }
}

// Clause 5.3: each channel data element's matrix on its tracks, in bitstream
// order; then every channel's lines in window order (Pseudocode 25); then the
// 7.X element's Table 183 steps, which pair channels of different elements.
ParseResult SubstreamPcm::matrix(const SubstreamContext& ctx, const ChannelElement& element) {
    parameters_.resize(kMaxChparams);
    dual_layouts_.clear();
    dual_layout_of_.assign(element.tracks.size(), -1);
    for (const DataElementRoute& part : route_.data) {
        if (!part.processed || part.discarded) {
            continue;
        }
        const Track& first = element.tracks[at(part.first_track)];
        const SfInfo& info = element.infos[at(first.info)];
        for (int k = 1; k < part.count; ++k) {
            if (element.tracks[at(part.first_track + k)].info != first.info) {
                return fail(DecodeError::kInvalidStream, "stereo processing over tracks of different sf_info()s");
            }
        }
        const std::size_t needed = chparams_of(part.count);
        for (std::size_t i = 0; i < needed; ++i) {
            parameters_[i] = stereo_parameters(ctx, info, element.chparams[at(part.first_chparam) + i]);
        }
        if (part.count == 2) {
            // Clause 5.3.3.2, on tracks laid out alike: with b_dual_maxsfb
            // their bands differ.
            const std::size_t t0 = at(part.first_track);
            const SfData& second = element.tracks[t0 + 1].data;
            if (first.data.max_sfb != second.max_sfb) {
                align_tracks(ctx, info.psy, first.data, second, scaled_[t0], scaled_[t0 + 1],
                             dual_layouts_.emplace_back());
                dual_layout_of_[t0] = static_cast<int>(dual_layouts_.size()) - 1;
                dual_layout_of_[t0 + 1] = dual_layout_of_[t0];
            }
            const int dual = dual_layout_of_[t0];
            const SfData& layout = dual >= 0 ? dual_layouts_[at(dual)] : first.data;
            apply_stereo(info, layout, parameters_[0], scaled_[t0], scaled_[t0 + 1]);
            continue;
        }
        std::array<std::vector<double>*, 5> tracks{};
        for (int k = 0; k < part.count; ++k) {
            tracks[at(k)] = &scaled_[at(part.first_track + k)];
        }
        if (auto ok = apply_channel_data(info, first.data, part.chel_matsel,
                                         std::span<const StereoParameters>(parameters_).first(needed),
                                         std::span<std::vector<double>* const>(tracks).first(at(part.count)));
            !ok) {
            return ok;
        }
    }

    spectra_.resize(channels_.size());
    for (std::size_t c = 0; c < channels_.size(); ++c) {
        if (track_of_[c] < 0) {  // silent in this codec mode
            spectra_[c].assign(at(full_length_), 0.0);
            continue;
        }
        const Track& track = element.tracks[at(track_of_[c])];
        const SfInfo& info = element.infos[at(track.info)];
        const int dual = dual_layout_of_[at(track_of_[c])];
        ungroup(ctx, info.psy, dual >= 0 ? dual_layouts_[at(dual)] : track.data, lengths_[c],
                scaled_[at(track_of_[c])], spectra_[c]);
    }

    for (const PairStep& step : route_.steps) {
        const auto first = at(channel_of(step.first));
        const auto second = at(channel_of(step.second));
        const auto framing = at(channel_of(step.framing));
        const SfInfo& info = element.infos[at(element.tracks[at(track_of_[framing])].info)];
        parameters_[0] =
            stereo_parameters(ctx, info, element.chparams[at(step.chparam)],
                              step.prediction ? StereoUse::kPrediction : StereoUse::kPair);
        if (auto ok = apply_additional_pair(ctx, info.psy, parameters_[0], lengths_[first], lengths_[second],
                                            spectra_[first], spectra_[second]);
            !ok) {
            return ok;
        }
    }
    return {};
}

ParseResult SubstreamPcm::decode(const SubstreamContext& ctx, const AudioSubstream& substream,
                                 const FrameInputs& frame_inputs,
                                 std::vector<std::vector<float>>& channels,
                                 std::vector<Speaker>& speakers) {
    const int sequence_counter = frame_inputs.sequence_counter;
    const ChannelElement& element = substream.element;
    if (ctx.sf_multiplier.has_value()) {
        return fail(DecodeError::kUnsupported, "96 and 192 kHz decoding (the HSF extension) is not decoded yet");
    }
    if (auto ok = route_element(ctx, element, route_, frame_inputs.decoding); !ok) {
        return ok;
    }
    if (auto ok = configure(ctx, frame_inputs.decoding); !ok) {
        return ok;
    }
    configure_outputs(ctx, frame_inputs.output);
    const std::size_t channel_count = channels_.size();

    // Everything that can fail is checked before any channel's overlap buffer
    // moves on, so a refused frame leaves the substream as it was.
    track_of_.assign(channel_count, -1);
    for (const DataElementRoute& part : route_.data) {
        if (part.discarded) {
            continue;
        }
        for (int k = 0; k < part.count; ++k) {
            const int channel = channel_of(part.outputs[at(k)]);
            if (channel < 0 || track_of_[at(channel)] >= 0) {
                return fail(DecodeError::kInvalidStream, "a channel element that codes one channel twice");
            }
            track_of_[at(channel)] = part.first_track + k;
        }
    }
    lengths_.resize(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        if (track_of_[c] < 0) {
            // A channel the codec mode codes in the QMF domain alone: one
            // long block of silence.
            if (std::ranges::find(route_.silent, speakers_[c]) == route_.silent.end()) {
                return fail(DecodeError::kInvalidStream, "a channel element that leaves a channel uncoded");
            }
            lengths_[c].assign(1, full_length_);
            continue;
        }
        const SfInfo& info = element.infos[at(element.tracks[at(track_of_[c])].info)];
        if (auto ok = window_lengths(ctx, info.psy, lengths_[c]); !ok) {
            return ok;
        }
    }
    if (auto ok = check_control(ctx, element); !ok) {
        return ok;
    }
    // Clause 5.7.7.7 now, so that a value outside its table refuses the
    // frame; the history DIFF_TIME refers to moves on once the frame is kept.
    std::optional<AcplFrameValues> acpl;
    const bool fresh = frame_inputs.new_source || decoded_mode_ != element.codec_mode;
    AcplQuantHistory acpl_history = fresh ? AcplQuantHistory{} : acpl_history_;
    if (uses_acpl(element.kind, element.codec_mode, decoding_)) {
        AcplFrameValues values;
        if (auto ok = acpl_values(element, acpl_history, values); !ok) {
            return ok;
        }
        acpl = values;
    }
    // Part 2 clause 5.6.3.2 alike, for A-JCC.
    std::optional<AjccFrameValues> ajcc;
    AjccQuantHistory ajcc_history = fresh ? AjccQuantHistory{} : ajcc_history_;
    if (element.kind == ElementKind::kImmersive &&
        element.codec_mode == immersive_mode::kAspxAjcc) {
        if (!element.ajcc) {
            return fail(DecodeError::kInvalidStream,
                        "an ASPX_AJCC element without its ajcc_data()");
        }
        AjccFrameValues values;
        if (auto ok = ajcc_values(*element.ajcc, ajcc_history, values); !ok) {
            return ok;
        }
        ajcc = values;
    }
    // Clause 5.1.4.2: the noise fill's generator starts each frame from the
    // frame's sequence_counter, and runs through the tracks in syntax order.
    RandGenState noise = reset_rand_gen_state_snf(sequence_counter);
    scaled_.resize(element.tracks.size());
    for (std::size_t t = 0; t < element.tracks.size(); ++t) {
        const Track& track = element.tracks[t];
        const SfInfo& info = element.infos[static_cast<std::size_t>(track.info)];
        if (auto ok = reconstruct_track(info, track.data, noise, scaled_[t]); !ok) {
            return ok;
        }
    }
    if (auto ok = matrix(ctx, element); !ok) {
        return ok;
    }
    acpl_history_ = acpl_history;
    ajcc_history_ = ajcc_history;
    decoded_mode_ = element.codec_mode;
    scpl_mode_.reset();
    if (element.kind == ElementKind::kImmersive) {
        scpl_mode_ = element.codec_mode;
    }
    // What concealment repeats: this frame's spectra and blocks, and the
    // values its output stages take.
    last_spectra_ = spectra_;
    last_lengths_ = lengths_;
    last_kind_ = element.kind;
    last_drc_ = frame_inputs.drc;
    last_drc_.reset = false;
    last_de_ = frame_inputs.de;
    last_downmix_ = frame_inputs.downmix;
    losses_ = 0;

    return render(Control{.codec_mode = element.codec_mode,
                          .kind = element.kind,
                          .add_ch_base = ctx.add_ch_base,
                          .new_source = frame_inputs.new_source,
                          .aspx_config = element.aspx_config,
                          .companding = element.companding,
                          .aspx_1ch = element.aspx_1ch,
                          .aspx_2ch = element.aspx_2ch,
                          .acpl = acpl,
                          .ajcc = ajcc,
                          .drc = frame_inputs.drc,
                          .de = frame_inputs.de,
                          .downmix = frame_inputs.downmix},
                  frame_inputs, channels, speakers);
}

ParseResult SubstreamPcm::conceal(ConcealmentPolicy policy, const FrameInputs& frame_inputs,
                                  std::vector<std::vector<float>>& channels,
                                  std::vector<Speaker>& speakers) {
    if (!can_conceal()) {
        return fail(DecodeError::kInvalidStream, "no frame has decoded to conceal from");
    }
    ++losses_;
    // Silence, or the last good frame's spectra faded at the rate forge's
    // AC-3 and E-AC-3 decoders fade a repeat, 20 dB for each 32 ms lost, to
    // the level that fade reaches at the frame's end; either way through the
    // frame's own inverse transform, so the overlap with the last good frame
    // fades rather than cuts.
    constexpr double kSecondsPer20Db = 0.032;
    const double lost =
        static_cast<double>(losses_) * static_cast<double>(full_length_) / internal_rate_;
    const double gain =
        policy == ConcealmentPolicy::kRepeatFade ? std::pow(10.0, -lost / kSecondsPer20Db) : 0.0;
    spectra_ = last_spectra_;
    for (std::vector<double>& spectrum : spectra_) {
        for (double& v : spectrum) {
            v *= gain;
        }
    }
    lengths_ = last_lengths_;
    // No control data came with the frame: the QMF domain passes it through,
    // the d_ctrl queue keeps its place, and the output stages hold the last
    // good frame's values.
    return render(Control{.codec_mode = codec_mode::kSimple,
                          .kind = last_kind_,
                          .add_ch_base = add_ch_base_,
                          .new_source = false,
                          .aspx_config = std::nullopt,
                          .companding = std::nullopt,
                          .aspx_1ch = {},
                          .aspx_2ch = {},
                          .acpl = std::nullopt,
                          .ajcc = std::nullopt,
                          .drc = last_drc_,
                          .de = last_de_,
                          .downmix = last_downmix_},
                  frame_inputs, channels, speakers);
}

ParseResult SubstreamPcm::render(Control control, const FrameInputs& frame_inputs,
                                 std::vector<std::vector<float>>& channels,
                                 std::vector<Speaker>& speakers) {
    const int converter_phase = frame_inputs.converter_phase;
    const std::size_t channel_count = channels_.size();
    const auto frame = static_cast<std::size_t>(full_length_);
    const std::size_t history = at(aspx::kTsOffsetHfadj + hfgen_) * kSubbands;
    pcm_.resize(frame);
    aligned_.resize(frame);
    for (std::size_t c = 0; c < channel_count; ++c) {
        std::vector<double>& samples = time_[c];
        std::size_t offset = 0;
        for (const int length : lengths_[c]) {
            const auto n = static_cast<std::size_t>(length);
            // window_lengths() allows only lengths the transform set has.
            (void)channels_[c].synthesis.block(
                *transforms_, std::span<const double>(spectra_[c]).subspan(offset, n),
                std::span<double>(samples).subspan(offset, n));
            offset += n;
        }
    }
    // Part 2 clause 5.3: S-CPL on the inverse transform's output, the frame's
    // own, before the frame alignment and the analysis.
    if (scpl_mode_) {
        apply_scpl(*scpl_mode_, decoding_, speakers_, time_);
    }
    for (std::size_t c = 0; c < channel_count; ++c) {
        const std::vector<double>& samples = time_[c];
        // Clause 5.6.2: out[n] = in[n - d_pcm]. d_pcm exceeds the frame at
        // some rates (1 312 at 100 fps, whose frame is 512), so the held
        // samples and the new ones are one queue.
        std::vector<double>& held = channels_[c].delay;
        held.insert(held.end(), samples.begin(), samples.end());
        std::copy_n(held.begin(), frame, aligned_.begin());
        held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(frame));
        // Clause 5.7.3: this frame's slots after the history.
        channels_[c].analysis.process(aligned_, std::span<QmfValue>(channels_[c].ext).subspan(history));
    }

    // Clause 5.7.2: this frame's control data waits d_ctrl frames; the
    // signal now in the QMF domain is the frame's d_ctrl frames back.
    held_.push_back(std::move(control));
    // The DRC, dialnorm, dialogue enhancement and downmix gains of the frame
    // whose signal this is; none before the first one's arrives.
    DrcFrameValues drc;
    DeFrameValues de;
    DownmixValues downmix;
    if (held_.size() > static_cast<std::size_t>(control_delay_)) {
        apply(held_.front());
        drc = held_.front().drc;
        de = held_.front().de;
        downmix = held_.front().downmix;
        held_.pop_front();
    } else {
        pass_through();
    }

    // Clause 5.7.8, dialogue enhancement, then 5.7.9, the output level and
    // DRC, whose level is measured on the signal dialogue enhancement took
    // (6.2.13).
    matrices_.clear();
    for (Channel& channel : channels_) {
        matrices_.push_back(&channel.out);
    }
    const double de_gain = frame_inputs.output.dialogue_enhancement_db;
    std::span<std::vector<QmfValue>* const> side = matrices_;
    if (de_.active(de_gain, de)) {
        if (drc.curve) {
            side_.resize(channels_.size());
            side_matrices_.clear();
            for (std::size_t c = 0; c < channels_.size(); ++c) {
                side_[c] = channels_[c].out;
                side_matrices_.push_back(&side_[c]);
            }
            side = side_matrices_;
        }
        de_.process(de_gain, de, matrices_);
    }
    drc_.process(frame_inputs.output, drc, matrices_, side);

    // Clause 6.2.17: the downmix, and the channels that come out of it.
    std::span<std::vector<QmfValue>* const> rendered = matrices_;
    if (!downmix_.passes_through()) {
        downmix_.process(downmix, matrices_, mixed_);
        mixed_matrices_.clear();
        for (std::vector<QmfValue>& mixed : mixed_) {
            mixed_matrices_.push_back(&mixed);
        }
        rendered = mixed_matrices_;
    }
    for (Channel& channel : channels_) {
        // The last slots become the next frame's history.
        std::copy(channel.ext.end() - static_cast<std::ptrdiff_t>(history), channel.ext.end(),
                  channel.ext.begin());
    }

    channels.resize(outputs_.size());
    // Part 2 clause 5.11: the converter's grid starts at the frame's phase,
    // and moves where the phase jumps, so that each frame gives the count
    // Table 47 gives its phi_t.
    const auto grid = static_cast<std::int64_t>(converter_phase) * full_length_;
    const bool jumped = converter_phase_ && converter_phase != (*converter_phase_ + 1) % 5;
    for (std::size_t o = 0; o < outputs_.size(); ++o) {
        Output& output = outputs_[o];
        output.synthesis.process(*rendered[o], pcm_);
        std::span<const double> produced = pcm_;
        if (output.converter) {
            if (!converter_phase_) {
                output.converter->reset(grid);
            } else if (jumped) {
                output.converter->rephase(grid);
            }
            converted_.clear();
            output.converter->process(pcm_, converted_);
            produced = converted_;
        }
        std::vector<float>& out = channels[o];
        out.resize(produced.size());
        for (std::size_t n = 0; n < produced.size(); ++n) {
            out[n] = static_cast<float>(
                std::clamp(produced[n] / kFullScale, -kOutputLimit, kOutputLimit));
        }
    }
    converter_phase_ = converter_phase;
    const std::span<const Speaker> out_speakers = downmix_.speakers();
    speakers.assign(out_speakers.begin(), out_speakers.end());
    return {};
}

}  // namespace ac4::detail
