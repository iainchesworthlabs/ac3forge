#include <cstddef>
#include <cstdint>
#include <span>

#include "ac4/ac4.hpp"

// ac4::scan and ac4::parse_raw_frame (src/ac4/src/ac4.cpp) - the AC-4
// bitstream inspector, roadmap IM4.
//
// AC-4 reaches this project the same way AC-3 does, as bytes from a file or a
// stream nobody here produced, and the TOC is the densest untrusted structure
// in the tree after the E-AC-3 frame header: ac4_toc() reads a presentation
// list whose length, per-presentation substream group counts and
// substream_index_table sizes all come from the bitstream, and
// parse_raw_frame then locates payloads by those declared sizes rather than by
// parsing through audio_data. Every one of those numbers is attacker-chosen.
//
// Both entry points on the same input:
//
//   scan             walks ac4_syncframe() elements back to back, so it also
//                    exercises sync search and the frame_size bound that
//                    decides kLostSync from kTruncated
//   parse_raw_frame  §4.2.1, taking `data` as one already-framed
//                    raw_ac4_frame - the form scan() hands on, reached here
//                    directly so the TOC parser is pressed without a
//                    well-formed syncframe having to be guessed first
//
// That second call is the point of the harness. Requiring the fuzzer to
// produce a valid 0xAC40/0xAC41 syncframe before any TOC byte is read would
// spend most executions in the sync search; feeding the same bytes straight
// to parse_raw_frame puts them in front of the TOC parser immediately.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    (void)ac4::scan(bytes);
    (void)ac4::parse_raw_frame(bytes);

    return 0;
}
