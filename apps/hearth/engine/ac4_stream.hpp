#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac4/ac4.hpp"

// An AC-4 elementary stream as a session plays it (planning/ac4.md, I2): its
// sync frames, the samples each one decodes to, and which of them a decoder
// can start at.
//
// The samples: ac4::Decoder puts out a frame's worth at the output rate, which
// at 29.97, 59.94 and 119.88 fps alternates by a sample in the five-frame
// cycle ETSI TS 103 190-2 clause 5.11 locks to sequence_counter. Its phase is
// sequence_counter modulo 5, and where a splice has set the counter to 0, the
// frame before's plus one. Table 47 gives each phase's count, which is the
// integer sample grid below the exact frame starts: floor((phase + 1) * L) -
// floor(phase * L) for a frame of L samples, taken here in the time scale
// ac4::media_timing() gives for the stream, in which a frame is whole. The
// session's timeline is built from these, before anything is decoded; the
// decoder's own counts are the check (tests/hearth/test_ac4_engine.cpp).
//
// Where a decoder can start: an I-frame (b_iframe_global), whose substreams
// send the configuration every other frame reuses. A decoder started anywhere
// else waits for one, putting out nothing.

namespace ac3::hearth {

struct Ac4Units {
    // Each sync frame whole - syncword, frame_size, raw_ac4_frame and, with
    // syncword 0xAC41, the CRC word - which is what the extension role's
    // bursts carry; views of the bytes read.
    std::vector<std::span<const std::byte>> frames;
    // The samples each decodes to at `sample_rate`.
    std::vector<std::uint32_t> samples;
    // Sized with the frames and set by index: whether each is an I-frame.
    std::vector<bool> iframes;
    // The output rate: the base sampling frequency, 48 or 44.1 kHz.
    std::uint32_t sample_rate = 0;
    // The first frame, whose table of contents stands for the stream's
    // presentations.
    ac4::RawFrame first{};
    // Frames whose table of contents did not read: each is given the length
    // of the frame before, as the decoder conceals it as the frame the stream
    // expected.
    std::size_t unread = 0;
};

// The sync frames of `bytes`, or a sentence saying why there are none to play.
[[nodiscard]] std::expected<Ac4Units, std::string> read_ac4_units(std::span<const std::byte> bytes);

// Whether `bytes` starts with an AC-4 sync word, 0xAC40 or 0xAC41.
[[nodiscard]] bool starts_ac4(std::span<const std::byte> bytes);

// The samples a frame of `toc`'s rate decodes to at phase `phase` (0 to 4) of
// the five-frame cycle; 0 for a frame rate Tables 83 and 84 do not define.
[[nodiscard]] std::uint32_t ac4_frame_samples(const ac4::Toc& toc, int phase);

// The raw_ac4_frame of the one sync frame `sync_frame` holds; empty when it
// holds no whole sync frame.
[[nodiscard]] std::span<const std::byte> raw_frame_of(std::span<const std::byte> sync_frame);

}  // namespace ac3::hearth
