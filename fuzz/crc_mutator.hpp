#pragma once

// A libFuzzer custom mutator that re-stamps AC-3/E-AC-3 CRC words after
// mutating, shared by fuzz_ac3_decode and fuzz_eac3_decode.
//
// Why: fuzz/README.md's own note on the differential harnesses records the
// problem - "the overwhelming majority of mutations get rejected immediately
// by this project's own decoder (bad sync word, bad CRC, a reserved field)".
// Of those three, bad CRC is the one that is pure loss. A bad sync word or a
// reserved value at least exercises the rejection path it names; a bad CRC
// rejects an input whose ONLY defect is the checksum, throwing away whatever
// the mutation did to the fields behind it. decode_frame checks crc1 and
// crc2 (decoder.cpp) before reading one bit of bsi, and decode_substream
// checks crc2 (eac3_decoder.cpp) before reading one bit of the audio blocks,
// so every mutation that lands in a skip field - which is where the EMDF
// container, and therefore all of the OAMD/JOC object metadata, lives - dies
// two orders of magnitude before the parser it was aimed at.
//
// Re-stamping is not a naive recompute. crc2 is an ordinary trailing CRC, but
// crc1 PRECEDES the region it protects: A/52 §7.10.1 requires the register to
// read zero after the first 5/8 of the syncframe has been shifted through,
// and explicitly says crc1 is not the CRC of that region. It is solved for,
// through the GF(2) polynomial inverse ac3::solve_leading_crc implements -
// the same call src/forge/src/encoder/encoder.cpp makes, including its
// crc2 == kSyncWord avoidance step (a crc2 that happens to equal 0x0B77 would
// make the frame's own tail look like the start of the next syncframe, so the
// encoder flips crcrsv and recomputes; a mutator that skipped this would
// hand the splitter a frame boundary that is not there).
//
// This does NOT repair anything else. Frame lengths, reserved fields and the
// bsi/audblk syntax are left exactly as the mutation left them - the goal is
// to stop losing inputs at the checksum, not to constrain the mutation engine
// to valid streams.

#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"

// libFuzzer's own mutation engine, callable from inside a custom mutator.
// Declared here rather than included from anywhere: libFuzzer ships no
// header, only this documented entry point.
extern "C" std::size_t LLVMFuzzerMutate(std::uint8_t* data, std::size_t size,
                                        std::size_t max_size);

namespace ac3fuzz {

// Walks `stream` as a concatenation of syncframes the way ac3::split_frames
// does - same bsid-at-bit-40 test, same two size derivations - and rewrites
// each frame's CRC words in place. Stops at the first byte that is not the
// start of a frame it can size and that fits, leaving the remainder alone: a
// well-formed prefix stays repaired even when the mutation truncated or
// corrupted the tail.
inline void restamp_syncframe_crcs(std::span<std::byte> stream) {
    std::size_t offset = 0;
    while (offset + 6 <= stream.size()) {
        const std::span<std::byte> from_here = stream.subspan(offset);
        const auto byte_at = [&](std::size_t i) {
            return std::to_integer<std::uint32_t>(from_here[i]);
        };
        constexpr std::uint32_t kSyncHigh = std::uint32_t{ac3::kSyncWord} >> 8;
        constexpr std::uint32_t kSyncLow = std::uint32_t{ac3::kSyncWord} & 0xFFU;
        if (byte_at(0) != kSyncHigh || byte_at(1) != kSyncLow) {
            return;
        }
        const auto bsid = byte_at(5) >> 3;
        std::size_t frame_bytes = 0;
        bool annex_e = false;
        if (bsid >= ac3::eac3::kMinDecodableBsid && bsid <= ac3::eac3::kBsid) {
            // §E2.3.1.3: frmsiz states the word count outright.
            frame_bytes = ((static_cast<std::size_t>(byte_at(2) & 0x07) << 8) | byte_at(3)) * 2 + 2;
            annex_e = true;
        } else if (bsid <= 8) {
            // §5.3.2: frmsizecod indexes Table 5.18 instead.
            const auto fscod = byte_at(4) >> 6;
            const auto frmsizecod = byte_at(4) & 0x3F;
            if (fscod == 3 || frmsizecod > 37) {
                return;
            }
            const auto sized = ac3::frame_size_bytes(static_cast<ac3::SampleRate>(fscod),
                                                     ac3::kBitratesKbps[frmsizecod >> 1],
                                                     (frmsizecod & 1) != 0);
            if (!sized) {
                return;
            }
            frame_bytes = *sized;
        } else {
            return;  // bsid 9/10, or 17+: no reading of the frame's size at all
        }
        // Both CRC regions below are expressed relative to a frame that has
        // at least the six header bytes plus the two crc2 bytes; frmsiz is
        // free to claim less than that, and split_frames rejects exactly the
        // same shape (DecodeError::kInvalidStream).
        if (frame_bytes < 8 || offset + frame_bytes > stream.size()) {
            return;
        }
        const std::span<std::byte> frame = stream.subspan(offset, frame_bytes);
        const std::span<const std::byte> view = frame;
        if (!annex_e) {
            // crc1 covers [2, 2*words58); its own two bytes lead that region,
            // so the body handed to solve_leading_crc starts at 4.
            const auto words58 =
                ac3::frame_size_58_words(static_cast<std::uint32_t>(frame_bytes / 2));
            const std::size_t region = static_cast<std::size_t>(words58) * 2;
            if (region < 4 || region > frame_bytes) {
                return;
            }
            const std::uint16_t crc1 = ac3::solve_leading_crc(view.subspan(4, region - 4));
            frame[2] = static_cast<std::byte>(crc1 >> 8);
            frame[3] = static_cast<std::byte>(crc1 & 0xFF);
        }
        std::uint16_t crc2 = ac3::crc16(view.subspan(2, frame_bytes - 4));
        if (crc2 == ac3::kSyncWord) {
            frame[frame_bytes - 3] ^= std::byte{0x01};  // crcrsv (§5.4.5.1)
            crc2 = ac3::crc16(view.subspan(2, frame_bytes - 4));
        }
        frame[frame_bytes - 2] = static_cast<std::byte>(crc2 >> 8);
        frame[frame_bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
        offset += frame_bytes;
    }
}

// The body of LLVMFuzzerCustomMutator: libFuzzer's own mutation, then the
// repair above on three inputs out of four.
//
// Not all four, deliberately. Always repairing would make a bad CRC
// unreachable by mutation, and that branch is a real one - decoder.cpp and
// eac3_decoder.cpp each reject on it, and the decoders' behaviour on a frame
// whose checksum is the ONLY thing wrong with it is exactly what a
// re-stamping mutator would stop anyone from ever checking again. Keeping a
// quarter of the mutations unrepaired costs almost nothing (the corpus that
// reaches the deep parsers is grown by the other three quarters) and leaves
// the rejection path where the engine can still find it.
inline std::size_t crc_repairing_mutate(std::uint8_t* data, std::size_t size,
                                        std::size_t max_size, unsigned int seed) {
    const std::size_t mutated = LLVMFuzzerMutate(data, size, max_size);
    if ((seed & 3U) != 0U) {
        restamp_syncframe_crcs({reinterpret_cast<std::byte*>(data), mutated});
    }
    return mutated;
}

}  // namespace ac3fuzz
