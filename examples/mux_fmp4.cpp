// Fragment an elementary stream into CMAF and signal it for HLS/DASH.
//
// iclforge::mp4::fragment() shares its AudioTrack/frame shape with iclforge::mp4::mux() (see
// examples/mux_mp4.cpp) - the only new step is FragmentOptions and reading
// back FragmentedOutput's init_segment/media_segments. mp4/hls.hpp and
// mp4/dash.hpp then build the manifests that point at those same segments,
// codec-blind the same way iclforge::mp4 itself is: nothing here is AC-3/E-AC-3
// specific except HlsOptions::channels_attribute, which - like
// AudioTrack::codec_config - is opaque to iclforge::mp4:: and supplied by this caller.

#include <cstddef>
#include <cstdio>
#include <fmt/printf.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/dec3.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/mp4/dash.hpp"
#include "iclforge/mp4/hls.hpp"
#include "iclforge/mp4/mp4.hpp"

int main() {
    // Some AC-3 to fragment - enough frames to span several fragments below
    // (31 frames at 8/fragment is 4 fragments: 8, 8, 8, 7).
    auto encoder = std::make_unique<iclforge::FrameEncoder>(
        iclforge::EncoderConfig{.bitrate_kbps = 192, .acmod = iclforge::Acmod::k2_0});
    std::vector<std::vector<float>> pcm(2, std::vector<float>(iclforge::kSamplesPerFrame));
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};

    std::vector<std::byte> elementary;
    for (int frame = 0; frame < 31; ++frame) {
        for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
            for (int n = 0; n < iclforge::kSamplesPerFrame; ++n) {
                pcm[ch][static_cast<std::size_t>(n)] =
                    0.2F * static_cast<float>((n % 61) - 30) / 30.0F;
            }
        }
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            return 1;
        }
        elementary.insert(elementary.end(), encoded->begin(), encoded->end());
    }

    const auto scanned = iclforge::io::scan(elementary);
    if (!scanned) {
        fmt::printf("scan failed\n");
        return 1;
    }

    std::vector<std::vector<std::byte>> frames;
    frames.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const iclforge::mp4::AudioTrack track{
        .codec_id = std::string{scanned->kind == iclforge::io::StreamKind::kAc3 ? iclforge::mp4::kCodecAc3
                                                                           : iclforge::mp4::kCodecEac3},
        .sample_rate = iclforge::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = iclforge::kSamplesPerFrame,
        .codec_config = iclforge::io::build_codec_config_box(*scanned),
    };

    const auto fragmented =
        iclforge::mp4::fragment(track, frames, iclforge::mp4::FragmentOptions{.frames_per_fragment = 8});
    if (!fragmented) {
        fmt::printf("fragment failed: %.*s\n",
                    static_cast<int>(iclforge::mp4::describe(fragmented.error()).size()),
                    iclforge::mp4::describe(fragmented.error()).data());
        return 1;
    }

    // Dolby Digital Plus with Atmos objects would set channels_attribute to
    // "<N>/JOC" here instead (see mp4/hls.hpp's own citations) - plain AC-3
    // has no such extension, so the default (just the channel count) is
    // correct as-is.
    const auto media_playlist =
        iclforge::mp4::build_hls_media_playlist(track, fragmented->media_segments, iclforge::mp4::HlsOptions{});
    const auto master_playlist = iclforge::mp4::build_hls_master_playlist(track, fragmented->media_segments,
                                                                "audio.m3u8", iclforge::mp4::HlsOptions{});
    const auto dash_snippet = iclforge::mp4::build_dash_adaptation_set(track, fragmented->media_segments);

    fmt::printf("%zu bytes init segment, %zu media segment(s) from %zu frames\n",
                fragmented->init_segment.size(), fragmented->media_segments.size(), frames.size());
    fmt::printf("HLS media playlist:\n%s\n", media_playlist.c_str());
    fmt::printf("HLS master playlist:\n%s\n", master_playlist.c_str());
    fmt::printf("DASH AdaptationSet:\n%s\n", dash_snippet.c_str());
    return 0;
}
