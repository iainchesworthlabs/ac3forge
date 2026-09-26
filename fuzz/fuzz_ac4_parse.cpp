#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

#include "ac4/ac4.hpp"

// ac4::scan, ac4::SyncFrameSplitter and ac4::parse_raw_frame (src/ac4/src/
// ac4.cpp) - the AC-4 bitstream inspector.
//
// AC-4 reaches this project the same way AC-3 does, as bytes from a file or a
// stream nobody here produced, and the TOC is the densest untrusted structure
// in the tree after the E-AC-3 frame header: ac4_toc() reads a presentation
// list whose length, per-presentation substream group counts and
// substream_index_table sizes all come from the bitstream, and
// parse_raw_frame then locates payloads by those declared sizes rather than by
// parsing through audio_data. Every one of those numbers is attacker-chosen.
//
// Every entry point on the same input:
//
//   scan             walks ac4_syncframe() elements back to back, so it also
//                    exercises sync search and the frame_size bound that
//                    decides kLostSync from kTruncated
//   SyncFrameSplitter the same framing fed in pieces, into storage small and
//                    large: it must hand over, first and in order, every frame
//                    scan() found before it stopped, then resynchronise where
//                    scan() gives up
//   parse_raw_frame  §4.2.1, taking `data` as one already-framed
//                    raw_ac4_frame - the form scan() hands on, reached here
//                    directly so the TOC parser is pressed without a
//                    well-formed syncframe having to be guessed first
//   build_dac4       and dac4_refusal, cmaf_refusal and the codec string, on
//                    every table of contents that reads: what `ac3cli mp4`
//                    writes for a stream it is given
//   signalled_presentation and the rest of what a manifest says of a track
//                    (TS 103 190-2 Annex G), and Annex H.1.2.4's
//                    configuration_difference: what `ac3cli fmp4` and a
//                    record or live take's CMAF folder write
//
// That last call is the point of the harness. Requiring the fuzzer to produce
// a valid 0xAC40/0xAC41 syncframe before any TOC byte is read would spend most
// executions in the sync search; feeding the same bytes straight to
// parse_raw_frame puts them in front of the TOC parser immediately.
namespace {

// Splits `bytes` in pieces of `piece` into `capacity` bytes of storage and
// checks the property against `scanned`.
void split(std::span<const std::byte> bytes, const ac4::ScanResult& scanned, std::size_t piece,
           std::size_t capacity) {
    std::vector<std::byte> storage(capacity);
    ac4::SyncFrameSplitter splitter{storage};
    std::size_t fed = 0;
    std::size_t matched = 0;
    for (;;) {
        const ac4::SyncFrameSplitter::Result next = splitter.next();
        if (next.status == ac4::SyncFrameSplitter::Status::kNeedMoreInput) {
            if (fed == bytes.size()) {
                splitter.finish();
                continue;
            }
            const std::span<std::byte> space = splitter.writable();
            if (space.empty()) {
                std::abort();  // waiting for input with no room to take it
            }
            const std::size_t n = std::min({piece, space.size(), bytes.size() - fed});
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(fed), n, space.begin());
            splitter.commit(n);
            fed += n;
            continue;
        }
        if (next.status == ac4::SyncFrameSplitter::Status::kTruncated) {
            continue;
        }
        if (next.status != ac4::SyncFrameSplitter::Status::kFrame) {
            break;
        }
        // A frame scan() found: the same bytes at the same place.
        if (matched < scanned.frames.size() &&
            next.frame.offset == scanned.frames[matched].offset) {
            const ac4::SyncFrame& want = scanned.frames[matched];
            if (next.frame.sync_word != want.sync_word || next.frame.crc_ok != want.crc_ok ||
                !std::ranges::equal(next.frame.raw_ac4_frame, want.raw_ac4_frame)) {
                std::abort();
            }
            ++matched;
        } else if (matched < scanned.frames.size() && capacity >= bytes.size()) {
            std::abort();  // with room for all, nothing comes before a frame scan() found
        }
    }
    if (capacity >= bytes.size() + 16 && matched != scanned.frames.size()) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    const ac4::ScanResult scanned = ac4::scan(bytes);
    const std::size_t piece =
        size == 0 ? 1 : 1 + std::to_integer<std::size_t>(bytes[size - 1]) % 61;
    split(bytes, scanned, piece, 64);
    split(bytes, scanned, piece, size + 16);
    if (const auto frame = ac4::parse_raw_frame(bytes)) {
        // What an MP4 or CMAF writer makes of a table of contents it did not
        // write: the dac4 describes every presentation whole or is empty, and
        // dac4_refusal() names a reason exactly where it is empty.
        const std::vector<std::byte> dac4 = ac4::build_dac4(frame->toc);
        if (dac4.empty() == ac4::dac4_refusal(frame->toc).empty()) {
            std::abort();
        }
        (void)ac4::cmaf_refusal(frame->toc);
        (void)ac4::rfc6381_codec_string(frame->toc);
        // The presentation a manifest describes is one of the table's, where
        // it has any; and a table of contents is equivalent to itself.
        const ac4::Toc& toc = frame->toc;
        const std::size_t presentations = toc.presentations_v1.empty()
                                              ? toc.presentations_v0.size()
                                              : toc.presentations_v1.size();
        const std::optional<std::size_t> signalled = ac4::signalled_presentation(toc);
        if (signalled.has_value() != (presentations > 0) ||
            (signalled.has_value() && *signalled >= presentations)) {
            std::abort();
        }
        (void)ac4::dash_channel_configuration(toc);
        (void)ac4::dash_supplemental_properties(toc);
        (void)ac4::presentation_channel_count(toc);
        if (!ac4::configuration_difference(toc, toc).empty()) {
            std::abort();
        }
    }

    return 0;
}
