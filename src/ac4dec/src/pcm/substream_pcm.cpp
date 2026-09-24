#include "pcm/substream_pcm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

#include "aspx/hf_generator.hpp"
#include "pcm/asf_reconstruct.hpp"
#include "pcm/companding.hpp"
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

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
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
        channel.qmf_synthesis.reset();
        std::ranges::fill(channel.ext, QmfValue{});
        channel.aspx = AspxChannelState{};
    }
    held_.clear();
    master_.reset();
}

ParseResult SubstreamPcm::configure(const SubstreamContext& ctx, std::size_t channel_count) {
    if (full_length_ == ctx.frame_len_base && ch_mode_ == ctx.ch_mode && frame_rate_index_ == ctx.frame_rate_index &&
        fs_index_ == ctx.fs_index && channels_.size() == channel_count && transforms_.has_value()) {
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
    delay_ = kAlignmentDelay[static_cast<std::size_t>(ctx.frame_rate_index)];
    control_delay_ = kControlDelay[static_cast<std::size_t>(ctx.frame_rate_index)];
    slots_ = full_length_ / dsp::kQmfSubbands;
    ts_in_ats_ = aspx::num_ts_in_ats(full_length_);
    hfgen_ = aspx::ts_offset_hfgen(full_length_);
    const int ext_slots = aspx::kTsOffsetHfadj + hfgen_ + slots_;
    channels_.clear();
    for (std::size_t c = 0; c < channel_count; ++c) {
        Channel channel{.synthesis = dsp::ChannelSynthesis<double>(full_length_),
                        .delay = std::vector<double>(static_cast<std::size_t>(delay_), 0.0),
                        .analysis = {},
                        .qmf_synthesis = {},
                        .ext = std::vector<QmfValue>(at(ext_slots) * kSubbands),
                        .out = std::vector<QmfValue>(at(slots_) * kSubbands),
                        .aspx = {}};
        channels_.push_back(std::move(channel));
    }
    held_.clear();
    master_.reset();
    return {};
}

// What one frame's A-SPX data would fail on when applied a frame later is
// checked now, before anything moves on: its tables and its interval.
ParseResult SubstreamPcm::check_control(const SubstreamContext& ctx, const ChannelElement& element) const {
    if (element.codec_mode != codec_mode::kAspx) {
        return {};
    }
    const bool pair = element.kind == ElementKind::kPair;
    const std::size_t expected = pair ? 2 : 1;
    if (!element.aspx_config || !element.companding || element.companding->num_chan != static_cast<int>(expected) ||
        (pair ? element.aspx_2ch.size() != 1 || !element.aspx_1ch.empty()
              : element.aspx_1ch.size() != 1 || !element.aspx_2ch.empty())) {
        return fail(DecodeError::kInvalidStream, "an ASPX channel element without its A-SPX or companding data");
    }
    AspxFrame frame{.config = &*element.aspx_config,
                    .xover_subband_offset = pair ? element.aspx_2ch[0].xover_subband_offset
                                                 : element.aspx_1ch[0].xover_subband_offset,
                    .balance = pair && element.aspx_2ch[0].balance,
                    .master_reset = false,
                    .base_48k = ctx.fs_index == 1,
                    .num_qmf_timeslots = slots_,
                    .num_ts_in_ats = ts_in_ats_,
                    .ts_offset_hfgen = hfgen_};
    std::array<const AspxChannel*, 2> data{};
    if (pair) {
        data = {&element.aspx_2ch[0].channels[0], &element.aspx_2ch[0].channels[1]};
    } else {
        data[0] = &element.aspx_1ch[0].channel;
    }
    return check_aspx(frame, std::span<const AspxChannel* const>(data).first(expected));
}

// Below the crossover and everywhere in SIMPLE mode: the analysis delayed by
// ts_offset_hfgen slots, the history the synthesis works behind (5.7.1).
void SubstreamPcm::pass_through() {
    for (Channel& channel : channels_) {
        std::copy_n(channel.ext.begin() + static_cast<std::ptrdiff_t>(at(aspx::kTsOffsetHfadj) * kSubbands),
                    at(slots_) * kSubbands, channel.out.begin());
        channel.aspx.y_prev_slots = 0;
    }
}

void SubstreamPcm::apply(const Control& control) {
    if (control.codec_mode != codec_mode::kAspx || !control.aspx_config) {
        pass_through();
        return;
    }
    const AspxConfig& config = *control.aspx_config;
    // 5.7.6.3.1.1: master_reset when the master table's parameters differ
    // from the last configuration's, and at the first.
    const std::array<int, 3> master = {config.master_freq_scale, config.start_freq, config.stop_freq};
    const bool master_reset = !master_ || *master_ != master;
    master_ = master;

    const bool pair = !control.aspx_2ch.empty();
    AspxFrame frame{.config = &config,
                    .xover_subband_offset = pair ? control.aspx_2ch[0].xover_subband_offset
                                                 : control.aspx_1ch[0].xover_subband_offset,
                    .balance = pair && control.aspx_2ch[0].balance,
                    .master_reset = master_reset,
                    .base_48k = fs_index_ == 1,
                    .num_qmf_timeslots = slots_,
                    .num_ts_in_ats = ts_in_ats_,
                    .ts_offset_hfgen = hfgen_};
    const std::size_t count = pair ? 2 : 1;
    std::array<AspxChannelIo, 2> io{};
    for (std::size_t c = 0; c < count; ++c) {
        Channel& channel = channels_[c];
        io[c] = AspxChannelIo{.data = pair ? &control.aspx_2ch[0].channels[c] : &control.aspx_1ch[0].channel,
                              .state = &channel.aspx,
                              .ext = channel.ext,
                              .out = channel.out};
    }

    // Companding first (Figure 6), over each channel's own crossover and
    // interval. Table 212: C for a single channel element, L and R for a
    // pair, in companding_control()'s order.
    aspx::SubbandGroups groups;
    aspx::PatchTables patches;
    if (!aspx_tables(frame, groups, patches)) {
        pass_through();  // check_control() refused this a frame ago
        return;
    }
    if (control.companding) {
        std::array<CompandingChannel, 2> companded{};
        for (std::size_t c = 0; c < count; ++c) {
            companded[c] = CompandingChannel{.ext = channels_[c].ext,
                                             .sb1 = groups.sbx,
                                             .interval = aspx_interval(io[c].data->framing, ts_in_ats_)};
        }
        apply_companding(*control.companding, 0, kFullScale,
                         std::span<const CompandingChannel>(companded).first(count));
    }
    if (!decode_aspx(frame, std::span<AspxChannelIo>(io).first(count))) {
        pass_through();
    }
}

ParseResult SubstreamPcm::decode(const SubstreamContext& ctx, const AudioSubstream& substream, int sequence_counter,
                                 std::vector<std::vector<float>>& channels, std::vector<Speaker>& speakers) {
    const ChannelElement& element = substream.element;
    if (ctx.sf_multiplier.has_value()) {
        return fail(DecodeError::kUnsupported, "96 and 192 kHz decoding (the HSF extension) is not decoded yet");
    }
    if (ctx.frame_rate_index != 13) {
        return fail(DecodeError::kUnsupported,
                    "frame rates other than frame_rate_index 13 need the sample rate converter, not built yet");
    }
    if (element.kind != ElementKind::kSingle && element.kind != ElementKind::kPair) {
        return fail(DecodeError::kUnsupported, "only mono and stereo substreams are decoded yet");
    }
    if (element.codec_mode != codec_mode::kSimple && element.codec_mode != codec_mode::kAspx) {
        return fail(DecodeError::kUnsupported,
                    "the A-CPL codec modes are not decoded yet; SIMPLE and ASPX are");
    }
    const std::size_t channel_count = element.kind == ElementKind::kPair ? 2 : 1;
    if (element.tracks.size() != channel_count) {
        return fail(DecodeError::kInvalidStream, "a channel element with an unexpected number of tracks");
    }
    if (auto ok = configure(ctx, channel_count); !ok) {
        return ok;
    }

    // Everything that can fail is checked before any channel's overlap buffer
    // moves on, so a refused frame leaves the substream as it was.
    lengths_.resize(channel_count);
    for (std::size_t t = 0; t < channel_count; ++t) {
        const SfInfo& info = element.infos[static_cast<std::size_t>(element.tracks[t].info)];
        if (auto ok = window_lengths(ctx, info.psy, lengths_[t]); !ok) {
            return ok;
        }
    }
    if (auto ok = check_control(ctx, element); !ok) {
        return ok;
    }
    // Clause 5.1.4.2: the noise fill's generator starts each frame from the
    // frame's sequence_counter.
    RandGenState noise = reset_rand_gen_state_snf(sequence_counter);
    scaled_.resize(channel_count);
    for (std::size_t t = 0; t < channel_count; ++t) {
        const Track& track = element.tracks[t];
        const SfInfo& info = element.infos[static_cast<std::size_t>(track.info)];
        if (auto ok = reconstruct_track(info, track.data, noise, scaled_[t]); !ok) {
            return ok;
        }
    }
    // Clause 5.3.3.2: a stereo_data() with b_enable_mdct_stereo_proc shares
    // one sf_info() between its tracks, and its chparam_info() sets the matrix.
    if (channel_count == 2 && !element.b_enable_mdct_stereo_proc.empty() && element.b_enable_mdct_stereo_proc[0]) {
        if (element.chparams.empty() || element.tracks[0].info != element.tracks[1].info) {
            return fail(DecodeError::kInvalidStream, "stereo processing without its chparam_info()");
        }
        const SfInfo& info = element.infos[static_cast<std::size_t>(element.tracks[0].info)];
        const StereoParameters parameters = stereo_parameters(ctx, info, element.chparams[0]);
        apply_stereo(info, element.tracks[0].data, parameters, scaled_[0], scaled_[1]);
    }

    const auto frame = static_cast<std::size_t>(full_length_);
    const std::size_t history = at(aspx::kTsOffsetHfadj + hfgen_) * kSubbands;
    pcm_.resize(frame);
    aligned_.resize(frame);
    for (std::size_t c = 0; c < channel_count; ++c) {
        const Track& track = element.tracks[c];
        const SfInfo& info = element.infos[static_cast<std::size_t>(track.info)];
        ungroup(ctx, info.psy, track.data, lengths_[c], scaled_[c], spectrum_);
        std::size_t offset = 0;
        for (const int length : lengths_[c]) {
            const auto n = static_cast<std::size_t>(length);
            // window_lengths() allows only lengths the transform set has.
            (void)channels_[c].synthesis.block(*transforms_, std::span<const double>(spectrum_).subspan(offset, n),
                                               std::span<double>(pcm_).subspan(offset, n));
            offset += n;
        }
        // Clause 5.6.2: out[n] = in[n - d_pcm]. d_pcm exceeds the frame at
        // some rates (1 312 at 100 fps, whose frame is 512), so the held
        // samples and the new ones are one queue.
        std::vector<double>& held = channels_[c].delay;
        held.insert(held.end(), pcm_.begin(), pcm_.end());
        std::copy_n(held.begin(), frame, aligned_.begin());
        held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(frame));
        // Clause 5.7.3: this frame's slots after the history.
        channels_[c].analysis.process(aligned_, std::span<QmfValue>(channels_[c].ext).subspan(history));
    }

    // Clause 5.7.2: this frame's control data waits d_ctrl frames; the
    // signal now in the QMF domain is the frame's d_ctrl frames back.
    held_.push_back(Control{.codec_mode = element.codec_mode,
                            .aspx_config = element.aspx_config,
                            .companding = element.companding,
                            .aspx_1ch = element.aspx_1ch,
                            .aspx_2ch = element.aspx_2ch});
    if (held_.size() > static_cast<std::size_t>(control_delay_)) {
        apply(held_.front());
        held_.pop_front();
    } else {
        pass_through();
    }

    channels.resize(channel_count);
    for (std::size_t c = 0; c < channel_count; ++c) {
        Channel& channel = channels_[c];
        channel.qmf_synthesis.process(channel.out, pcm_);
        std::vector<float>& out = channels[c];
        out.resize(frame);
        for (std::size_t n = 0; n < frame; ++n) {
            out[n] = static_cast<float>(std::clamp(pcm_[n] / kFullScale, -kOutputLimit, kOutputLimit));
        }
        // The last slots become the next frame's history.
        std::copy(channel.ext.end() - static_cast<std::ptrdiff_t>(history), channel.ext.end(), channel.ext.begin());
    }
    speakers.clear();
    if (channel_count == 1) {
        speakers.push_back(Speaker::kCentre);
    } else {
        speakers.push_back(Speaker::kLeft);
        speakers.push_back(Speaker::kRight);
    }
    return {};
}

}  // namespace ac4::detail
