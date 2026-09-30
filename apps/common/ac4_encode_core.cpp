#include "ac4_encode_core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <utility>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac3/meta/loudness.hpp"
#include "ac4_channels.hpp"
#include "iclforge/mp4/mp4.hpp"

namespace iclforge::apps {

std::vector<iclforge::ac4::Speaker> ac4_input_speakers(std::size_t count,
                                                       iclforge::ac4::AdditionalPair pair,
                                                       bool three_zero, bool back_pair) {
    using S = iclforge::ac4::Speaker;
    const bool seven = count == 7 || count == 8;
    if (seven && pair == iclforge::ac4::AdditionalPair::kNone) {
        return {};
    }
    if ((count == 11 || count == 12) && !back_pair) {
        return {};
    }
    switch (count) {
        case 1:
            return {S::kCentre};
        case 2:
            return {S::kLeft, S::kRight};
        case 3:
            if (three_zero) {
                return {S::kLeft, S::kRight, S::kCentre};
            }
            return {};
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
            break;
        default:
            return {};
    }
    std::vector<S> out = {S::kLeft, S::kRight, S::kCentre};
    if (count % 2 == 0) {
        out.push_back(S::kLfe);
    }
    out.push_back(S::kLeftSurround);
    out.push_back(S::kRightSurround);
    if (count >= 9) {
        if (count >= 11) {
            out.insert(out.end(), {S::kLeftBack, S::kRightBack});
        }
        out.insert(out.end(),
                   {S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight});
        return out;
    }
    if (!seven) {
        return out;
    }
    if (pair == iclforge::ac4::AdditionalPair::kBack) {
        out.insert(out.end(), {S::kLeftBack, S::kRightBack});
    } else if (pair == iclforge::ac4::AdditionalPair::kWide) {
        out.insert(out.end(), {S::kLeftWide, S::kRightWide});
    } else if (pair == iclforge::ac4::AdditionalPair::kTopFront) {
        out.insert(out.end(), {S::kTopFrontLeft, S::kTopFrontRight});
    }
    return out;
}

std::string_view ac4_layout_name(std::size_t count, iclforge::ac4::AdditionalPair pair) {
    switch (count) {
        case 1:
            return "mono";
        case 2:
            return "stereo";
        case 3:
            return "3.0";
        case 5:
            return "5.0";
        case 6:
            return "5.1";
        case 9:
            return "5.0.4";
        case 10:
            return "5.1.4";
        case 11:
            return "7.0.4";
        case 12:
            return "7.1.4";
        default:
            break;
    }
    const bool lfe = count == 8;
    switch (pair) {
        case iclforge::ac4::AdditionalPair::kBack:
            return lfe ? "7.1, 3/4/0" : "7.0, 3/4/0";
        case iclforge::ac4::AdditionalPair::kWide:
            return lfe ? "7.1, 5/2/0" : "7.0, 5/2/0";
        default:
            return lfe ? "7.1, 3/2/2" : "7.0, 3/2/2";
    }
}

std::vector<std::size_t> ac4_wav_index(std::span<const iclforge::ac4::Speaker> speakers) {
    const std::vector<std::size_t> wav_order = ac4_order(speakers, ac4_wav_rank);
    std::vector<std::size_t> index(speakers.size());
    for (std::size_t w = 0; w < wav_order.size(); ++w) {
        index[wav_order[w]] = w;
    }
    return index;
}

std::optional<Ac4Measured> measure_ac4_programme(std::span<const std::span<const float>> channels,
                                                 std::uint32_t sample_rate) {
    const std::size_t count = channels.size();
    // The bed: every channel up to 5.1, L R C Ls Rs and the LFE where there is
    // one past that; the pairs after it, the 7.X pair or the immersive
    // layouts' back and top pairs, add their true peaks.
    const bool lfe_after_five = count == 8 || count == 10 || count == 12;
    const std::size_t bed = count <= 6 ? count : (lfe_after_five ? 6 : 5);
    const bool lfe = bed == 6;
    const auto acmod = bed == 1   ? iclforge::Acmod::k1_0
                       : bed == 2 ? iclforge::Acmod::k2_0
                                  : (bed == 3 ? iclforge::Acmod::k3_0 : iclforge::Acmod::k3_2);
    const auto rate =
        sample_rate == 48000 ? iclforge::SampleRate::k48000 : iclforge::SampleRate::k44100;
    iclforge::meta::LoudnessMeter meter{rate, acmod, lfe};
    // The meter takes AC-3's coded order, L C R Ls Rs and the LFE last, and
    // the encoder's order for the bed is a 5.1 WAV file's, whose permutation
    // ac3_layout_for gives.
    std::vector<std::size_t> order(bed);
    if (const auto layout = iclforge::io::ac3_layout_for(bed);
        layout && layout->wav_index.size() == bed) {
        order.assign(layout->wav_index.begin(), layout->wav_index.end());
    } else {
        for (std::size_t k = 0; k < bed; ++k) {
            order[k] = k;
        }
    }
    // Each pair's true peak after the bed, from a stereo meter of its own.
    std::vector<iclforge::meta::LoudnessMeter> pair_meters;
    for (std::size_t k = bed; k + 1 < count; k += 2) {
        pair_meters.emplace_back(rate, iclforge::Acmod::k2_0, false);
    }
    Ac4Measured out;
    const std::size_t length = channels.empty() ? 0 : channels.front().size();
    const std::size_t step = sample_rate / 10;
    std::vector<std::span<const float>> views(bed);
    std::vector<std::span<const float>> pair_views(2);
    for (std::size_t at = 0; at < length; at += step) {
        const std::size_t n = std::min(step, length - at);
        for (std::size_t k = 0; k < bed; ++k) {
            views[k] = channels[order[k]].subspan(at, n);
        }
        meter.push(views);
        for (std::size_t p = 0; p < pair_meters.size(); ++p) {
            pair_views[0] = channels[bed + 2 * p].subspan(at, n);
            pair_views[1] = channels[bed + 2 * p + 1].subspan(at, n);
            pair_meters[p].push(pair_views);
        }
        const auto keep_max = [](std::optional<double>& max, std::optional<double> value) {
            if (value && (!max || *value > *max)) {
                max = value;
            }
        };
        keep_max(out.max_momentary, meter.momentary_lkfs());
        keep_max(out.max_short_term, meter.short_term_lkfs());
    }
    const auto integrated = meter.integrated_lkfs();
    if (!integrated) {
        return std::nullopt;
    }
    out.integrated = *integrated;
    out.range = meter.loudness_range();
    out.true_peak = meter.true_peak_dbtp();
    for (const iclforge::meta::LoudnessMeter& pair_meter : pair_meters) {
        if (const auto pair_peak = pair_meter.true_peak_dbtp();
            pair_peak && (!out.true_peak || *pair_peak > *out.true_peak)) {
            out.true_peak = pair_peak;
        }
    }
    return out;
}

double ac4_dialnorm_for(double integrated_lkfs) {
    return std::clamp(std::round(-integrated_lkfs * 4.0) / 4.0, 0.0, 31.75);
}

iclforge::ac4::FurtherLoudness ac4_further_loudness(iclforge::ac4::LoudnessPractice practice,
                                          const Ac4Measured& measured) {
    const auto held = [](std::optional<double> value, double low) {
        return value ? std::optional<double>{std::clamp(*value, low, 102.3)} : value;
    };
    iclforge::ac4::FurtherLoudness loudness;
    loudness.practice = practice;
    loudness.integrated_lkfs = held(measured.integrated, -102.4);
    loudness.loudness_range_lu = held(measured.range, 0.0);
    loudness.max_true_peak_dbtp = held(measured.true_peak, -102.4);
    loudness.max_momentary_lufs = held(measured.max_momentary, -102.4);
    loudness.max_short_term_lufs = held(measured.max_short_term, -102.4);
    return loudness;
}

bool ac4_output_names_mp4(std::string_view out_path) {
    constexpr std::array<std::string_view, 3> kMp4Exts{".mp4", ".m4a", ".mov"};
    const std::string ext = std::filesystem::path{std::string{out_path}}.extension().string();
    return std::ranges::any_of(kMp4Exts,
                               [&](std::string_view candidate) { return ext == candidate; });
}

std::expected<Ac4Packaged, Ac4PackageError> package_ac4(
    std::span<const iclforge::ac4::EncodedFrame> frames, const iclforge::ac4::Toc& toc, bool mp4,
    bool crc) {
    Ac4Packaged out;
    if (!mp4) {
        out.chunks.reserve(frames.size());
        for (const iclforge::ac4::EncodedFrame& frame : frames) {
            out.chunks.push_back(iclforge::ac4::sync_frame(frame.raw_ac4_frame, crc));
        }
        return out;
    }
    // Part 2 Annex E: each frame a sample, the I-frames its sync samples,
    // timed as Table E.1 says.
    std::vector<std::span<const std::byte>> samples;
    iclforge::mp4::MuxOptions options;
    samples.reserve(frames.size());
    // Sized, not reserved: GCC 16's -Wnull-dereference flags vector<bool>::reserve on an
    // empty vector.
    options.sync_samples = std::vector<bool>(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        samples.emplace_back(frames[i].raw_ac4_frame);
        options.sync_samples[i] = frames[i].iframe;
    }
    const auto timing = iclforge::ac4::media_timing(toc);
    if (!timing) {
        return std::unexpected(Ac4PackageError{
            .message = fmt::format("frame_rate_index {} has no MP4 timing (Part 2 Table E.1)",
                                   toc.frame_rate_index),
            .usage = false});
    }
    std::vector<std::byte> dac4 = iclforge::ac4::build_dac4(toc);
    if (dac4.empty()) {
        return std::unexpected(Ac4PackageError{
            .message = fmt::format(
                "the MP4 sample entry's dac4 cannot describe {}; write a raw .ac4 instead",
                iclforge::ac4::dac4_refusal(toc)),
            .usage = true});
    }
    const iclforge::mp4::AudioTrack track{.codec_id = std::string{iclforge::mp4::kCodecAc4},
                                .sample_rate = static_cast<std::uint32_t>(toc.sample_rate_hz),
                                .channels = 2,  // TS 103 190-2 E.4.5: "should be set to 2"
                                .samples_per_frame = timing->sample_delta,
                                .codec_config = std::move(dac4),
                                .rfc6381 = iclforge::ac4::rfc6381_codec_string(toc),
                                .timescale = timing->timescale};
    out.rfc6381 = track.rfc6381;
    auto muxed = iclforge::mp4::mux(track, samples, options);
    if (!muxed.has_value()) {
        return std::unexpected(Ac4PackageError{
            .message = std::string{iclforge::mp4::describe(muxed.error())}, .usage = false});
    }
    out.chunks.push_back(std::move(*muxed));
    return out;
}

}  // namespace iclforge::apps
