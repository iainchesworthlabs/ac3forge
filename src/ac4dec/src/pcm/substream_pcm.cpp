#include "pcm/substream_pcm.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "pcm/asf_reconstruct.hpp"
#include "pcm/snf_random.hpp"
#include "pcm/stereo.hpp"
#include "syntax/channel_elements.hpp"

namespace ac4::detail {
namespace {

// Table 188, d_pcm by frame_rate_index: 23.976 to 120 fps, then index 13,
// which is 23.4375 fps at 48 kHz and 21.5332 fps at 44.1 kHz and takes 352 at
// both.
constexpr std::array<int, 14> kAlignmentDelay = {288, 288, 352, 96, 96, 960, 960,
                                                 1056, 672, 672, 1312, 864, 864, 352};

// The inverse transform as Pseudocodes 60 to 64 print it, with no "factor of
// 2" from the informative example after Table 187, puts full scale at 2^15:
// DEE's streams of a -20 dBFS tone decode to 0.1 * 32 768 within 0.01 dB. The
// output is scaled to full scale 1.0. See src/ac4dec/ERRATA.md, "Full scale,
// and the overlap-add's factor of two".
constexpr double kFullScale = 32768.0;

}  // namespace

void SubstreamPcm::reset() {
    for (Channel& channel : channels_) {
        channel.synthesis.reset();
        std::ranges::fill(channel.delay, 0.0);
    }
}

ParseResult SubstreamPcm::configure(const SubstreamContext& ctx, std::size_t channel_count) {
    if (full_length_ == ctx.frame_len_base && ch_mode_ == ctx.ch_mode && frame_rate_index_ == ctx.frame_rate_index &&
        channels_.size() == channel_count && transforms_.has_value()) {
        return {};
    }
    transforms_.emplace(ctx.frame_len_base, 1);
    if (!transforms_->valid()) {
        transforms_.reset();
        return fail(DecodeError::kInvalidStream, "a frame length with no transform");
    }
    full_length_ = ctx.frame_len_base;
    ch_mode_ = ctx.ch_mode;
    frame_rate_index_ = ctx.frame_rate_index;
    delay_ = kAlignmentDelay[static_cast<std::size_t>(ctx.frame_rate_index)];
    channels_.clear();
    for (std::size_t c = 0; c < channel_count; ++c) {
        channels_.push_back(Channel{dsp::ChannelSynthesis<double>(full_length_),
                                    std::vector<double>(static_cast<std::size_t>(delay_), 0.0)});
    }
    return {};
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
    if (element.codec_mode != codec_mode::kSimple) {
        return fail(DecodeError::kUnsupported, "only the SIMPLE codec mode is decoded yet; A-SPX is not");
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
    channels.resize(channel_count);
    pcm_.resize(frame);
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
        std::vector<float>& out = channels[c];
        out.resize(frame);
        for (std::size_t n = 0; n < frame; ++n) {
            out[n] = static_cast<float>(held[n] / kFullScale);
        }
        held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(frame));
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
