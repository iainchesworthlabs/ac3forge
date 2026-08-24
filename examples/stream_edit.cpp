// Change a stream's delivery metadata, and cut and rejoin it, without
// re-encoding a single coefficient.
//
// dialnorm, compr, bsmod and dsurmod all live in bsi, ahead of the first
// audblk. Correcting one by re-encoding the programme costs a whole
// generation of lossy coding for nothing, so ac3::io::edit_stream_metadata
// rewrites the bits in place and re-stamps the frame's CRCs instead - crc1
// through ac3::solve_leading_crc's GF(2) inverse, since A/52 §7.10.1 puts it
// BEFORE the region it protects and it therefore has to be solved rather than
// computed.
//
// The two claims this prints are the ones worth making about such a rewrite:
// the metadata really changed, and the decoded audio is bit-identical to what
// it was. A cut and a cat make the matching claim on the framing side - split
// on an access-unit boundary and joined back, the stream is byte-for-byte
// what it started as.

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/metadata_edit.hpp"

namespace {

// Every frame's decoded PCM, concatenated. The decoder validates crc1 AND
// crc2 before it produces a sample, so a successful decode of the rewritten
// stream is itself the proof that the re-stamp came out right.
std::vector<float> decode_all(std::span<const std::byte> stream, int& dialnorm) {
    const auto frames = ac3::split_frames(stream);
    if (!frames) {
        return {};
    }
    ac3::FrameDecoder decoder;
    std::vector<float> pcm;
    bool first = true;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        if (!decoded) {
            std::printf("decode failed: %d\n", std::to_underlying(decoded.error()));
            return {};
        }
        if (first) {
            dialnorm = decoded->dialnorm;
            first = false;
        }
        for (const auto& channel : decoded->channels) {
            pcm.insert(pcm.end(), channel.begin(), channel.end());
        }
    }
    return pcm;
}

}  // namespace

int main() {
    constexpr int kFrames = 12;

    // Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
    // state (PREfast's C6262).
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 192, .dialnorm = 31, .acmod = ac3::Acmod::k2_0});
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::byte> stream;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
            const double hz = ch == 0 ? 440.0 : 660.0;
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = (frame * ac3::kSamplesPerFrame + n) / 48000.0;
                pcm[ch][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * hz * t));
            }
        }
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto encoded = encoder->encode_frame(views);
        if (!encoded) {
            std::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
            return 1;
        }
        stream.insert(stream.end(), encoded->begin(), encoded->end());
    }

    // --- where each access unit sits ---------------------------------------
    const auto scanned = ac3::io::scan(stream);
    if (!scanned) {
        std::printf("scan failed: %.*s\n",
                    static_cast<int>(ac3::io::describe(scanned.error()).size()),
                    ac3::io::describe(scanned.error()).data());
        return 1;
    }
    std::printf("%zu access units, %.3f s total\n", scanned->access_units.size(),
                ac3::io::stream_duration_seconds(*scanned));
    for (const std::size_t index : {std::size_t{0}, std::size_t{6}, std::size_t{11}}) {
        const auto at = ac3::io::access_unit_timing(*scanned, index);
        if (!at) {
            continue;
        }
        // 90 kHz is MPEG-2 systems' clock; every figure is derived from the
        // absolute sample position, so a long stream cannot drift the way a
        // running sum of per-frame increments does.
        std::printf("  unit %2zu starts at %.3f s (%llu samples, %llu ticks at 90 kHz)\n", index,
                    at->start_seconds(), static_cast<unsigned long long>(at->start_sample),
                    static_cast<unsigned long long>(at->start_in_timescale(90'000)));
    }

    // --- rewrite dialnorm in place -----------------------------------------
    int before_dialnorm = 0;
    const auto before = decode_all(stream, before_dialnorm);
    if (before.empty()) {
        return 1;
    }

    auto rewritten = stream;
    const auto summary = ac3::io::edit_stream_metadata(rewritten, {.dialnorm = 24, .bsmod = 2});
    if (!summary) {
        std::printf("rewrite failed: %.*s\n",
                    static_cast<int>(ac3::io::describe(summary.error()).size()),
                    ac3::io::describe(summary.error()).data());
        return 1;
    }
    std::printf("rewrote %zu of %zu syncframes; stream is still %zu bytes\n", summary->changed,
                summary->syncframes, rewritten.size());

    int after_dialnorm = 0;
    const auto after = decode_all(rewritten, after_dialnorm);
    if (after.empty()) {
        return 1;
    }
    std::printf("  dialnorm %d -> %d\n", before_dialnorm, after_dialnorm);
    std::printf("  decoded audio identical: %s\n", after == before ? "yes" : "NO");

    // --- cut and rejoin ----------------------------------------------------
    // access_unit_at_seconds names the unit covering a position; a cut is
    // always the whole unit, never a split, so the two halves together are
    // exactly the stream they came from.
    const auto split = ac3::io::access_unit_at_seconds(*scanned, 0.2);
    if (!split) {
        std::printf("split point past the end\n");
        return 1;
    }
    std::vector<std::byte> head;
    std::vector<std::byte> tail;
    for (std::size_t i = 0; i < scanned->access_units.size(); ++i) {
        auto& target = i < *split ? head : tail;
        const auto unit = scanned->access_units[i];
        target.insert(target.end(), unit.begin(), unit.end());
    }
    std::vector<std::byte> rejoined = head;
    rejoined.insert(rejoined.end(), tail.begin(), tail.end());
    std::printf("cut at unit %zu: %zu + %zu bytes, rejoined identical to the source: %s\n", *split,
                head.size(), tail.size(), rejoined == stream ? "yes" : "NO");
    return 0;
}
