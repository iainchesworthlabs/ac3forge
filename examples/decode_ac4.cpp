// Decode an AC-4 stream the way a player does. The bytes arrive in pieces, as they do from a
// socket or an HTTP body; the inspector's SyncFrameSplitter (ac4::ac4) hands over each sync frame
// once all of it is in, and the decoder (ac4::decoder) turns it into PCM for one presentation,
// handed over in blocks of 256 samples. A television's settings go in the decoder's
// configuration: the output level, the dynamic range control mode it selects, a stereo downmix
// and dialogue enhancement. Once the stream has played, the decoder says what it found: the
// presentations and the metadata of the one it decoded.
//
// Usage: decode_ac4 <stream.ac4>. ctest runs it on a committed DEE stream.

#include <fmt/printf.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <span>
#include <string_view>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        fmt::printf("usage: decode_ac4 <stream.ac4>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        fmt::printf("cannot open %s\n", argv[1]);
        return 1;
    }

    ac4::DecoderConfig config;
    config.output.output_level_dbfs = -24.0;     // Lout: the dialogue level the output is taken to
    config.output.drc = ac4::DrcMode::kDefault;  // the mode that output level selects
    config.output.downmix = ac4::DownmixTarget::kStereo;
    config.output.dialogue_enhancement_db = 6.0;  // up to the stream's cap
    config.presentation.language = "en";          // where the stream offers a choice
    ac4::Decoder decoder(config);

    std::uint64_t samples = 0;
    std::size_t channels = 0;
    float peak = 0.0F;
    const auto sink = [&](const ac4::PcmBlock& block) {
        channels = block.channels.size();
        for (const std::span<const float> channel : block.channels) {
            for (const float sample : channel) {
                peak = std::max(peak, std::abs(sample));
            }
        }
        samples += block.samples;
    };

    // The splitter owns no memory: this holds the frame being assembled.
    std::vector<std::byte> storage(ac4::kSplitterRecommendedBuffer);
    ac4::SyncFrameSplitter splitter{storage};
    std::size_t frames = 0;
    for (;;) {
        const auto next = splitter.next();
        if (next.status == ac4::SyncFrameSplitter::Status::kNeedMoreInput) {
            const std::span<std::byte> space = splitter.writable();
            const std::size_t want = std::min<std::size_t>(space.size(), 4096);
            in.read(reinterpret_cast<char*>(space.data()), static_cast<std::streamsize>(want));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) {
                splitter.finish();
            } else {
                splitter.commit(got);
            }
            continue;
        }
        if (next.status != ac4::SyncFrameSplitter::Status::kFrame) {
            break;  // kEndOfStream, or kTruncated at a cut-off last frame
        }
        ++frames;
        const auto info = decoder.decode_by_block(next.frame.raw_ac4_frame, sink);
        if (!info) {
            const std::string_view reason = decoder.refusal_reason();
            fmt::printf("frame %zu: %.*s\n", frames, static_cast<int>(reason.size()),
                        reason.data());
            return 1;
        }
    }
    decoder.flush(sink);  // the samples held back short of a block, as one shorter block

    for (const ac4::PresentationInfo& presentation : decoder.presentations()) {
        fmt::printf("presentation %zu: %zu channels as coded, %s%s\n", presentation.index,
                    presentation.speakers.size(),
                    presentation.language.empty() ? "no language" : presentation.language.c_str(),
                    presentation.decodable ? "" : ", not decoded by this version");
    }
    const ac4::PresentationMetadata& metadata = decoder.metadata();
    if (metadata.loudness.dialnorm_dbfs) {
        fmt::printf("dialnorm %.0f dBFS\n", *metadata.loudness.dialnorm_dbfs);
    }
    if (metadata.drc && metadata.drc->applied_mode) {
        fmt::printf("DRC decoder mode %d applied\n", *metadata.drc->applied_mode);
    }
    fmt::printf("%zu frames: %zu channels of %llu samples, peak %.3f, decoder delay %d samples\n",
                frames, channels, static_cast<unsigned long long>(samples),
                static_cast<double>(peak), decoder.latency_samples());
    return frames > 0 && samples > 0 ? 0 : 1;
}
