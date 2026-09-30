// Wrap an elementary stream in MP4.
//
// iclforge::mp4 links nothing from iclforge::ac3 — it takes frames, and the codec's
// sample-entry configuration box, as opaque bytes. Pairing it with
// iclforge::io::scan and iclforge::io::build_codec_config_box is what keeps the dec3
// box honest: fscod/bsid/bsmod/acmod/lfeon and the Dolby Atmos extension
// (flag_ec3_extension_type_a/complexity_index_type_a) come straight off the
// bitstream rather than from the caller.

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
#include "iclforge/mp4/mp4.hpp"

int main() {
    // Some AC-3 to wrap.
    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
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

    // Ask the bitstream what it is rather than asserting it.
    const auto scanned = iclforge::io::scan(elementary);
    if (!scanned) {
        fmt::printf("scan failed\n");
        return 1;
    }

    // One MP4 sample per access unit. For E-AC-3 an access unit is the
    // independent substream plus its dependents, which is exactly what scan
    // groups — a player must receive them together.
    std::vector<std::vector<std::byte>> frames;
    frames.reserve(scanned->access_units.size());
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const iclforge::mp4::AudioTrack track{
        .codec_id = std::string{scanned->kind == iclforge::io::StreamKind::kAc3
                                    ? iclforge::mp4::kCodecAc3
                                    : iclforge::mp4::kCodecEac3},
        .sample_rate = iclforge::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = iclforge::kSamplesPerFrame,
        // The dac3/dec3 sample-entry box, built from the same scan result -
        // see ac3/io/dec3.hpp for why this lives in iclforge::io rather than in
        // iclforge::mp4 itself.
        .codec_config = iclforge::io::build_codec_config_box(*scanned),
    };

    const auto file = iclforge::mp4::mux(track, frames);
    if (!file) {
        fmt::printf("mux failed: %.*s\n",
                    static_cast<int>(iclforge::mp4::describe(file.error()).size()),
                    iclforge::mp4::describe(file.error()).data());
        return 1;
    }

    fmt::printf("%zu bytes of MP4 from %zu frames\n", file->size(), frames.size());
    return 0;
}
