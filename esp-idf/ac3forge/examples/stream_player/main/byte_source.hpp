#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Where the bitstream comes from, as a seam CMake resolves - the mirror of
// audio_sink.hpp at the other end of the player, and the same rule the library
// follows for its own platform choices.
//
//   source/partition/  a flash partition. The default, and the only one CI can
//                      RUN: it needs no hardware beyond the flash the
//                      application is already in, so it works under QEMU.
//   source/sd/         an SD card over SDMMC, through FATFS.
//   source/http/       an HTTP body, over WiFi.
//
// The player never learns which it has. That is the point: a decoder does not
// care where its bytes were, and an integrator swapping one for another should
// be changing a build option, not a decode loop.
//
// WHAT IS AND IS NOT VERIFIED. The partition source is exercised end to end in
// CI under QEMU. The SD and HTTP sources are COMPILED there and no more - QEMU
// has no SD host and no network, so anything further would be a test of the
// emulator. Both are small on purpose for that reason: the less that lives
// behind an unrunnable seam, the less can be wrong in it.

namespace player {

// Brings the source up. False means the caller should stop and say why - a
// missing card, no WiFi, a partition that is not there.
[[nodiscard]] bool source_open();

// Fills as much of `dst` as it has. Returns 0 at end of stream, which is what
// makes the player call finish() on the accumulator; a source that would BLOCK
// for more should block rather than return 0, since 0 is not "wait", it is
// "there will never be more".
[[nodiscard]] std::size_t source_read(std::span<std::byte> dst);

// Back to the beginning, for a player that loops. False if this source cannot -
// a socket generally cannot, and saying so beats pretending.
[[nodiscard]] bool source_rewind();

// For the log line, so a run says where its bytes came from.
[[nodiscard]] const char* source_name();

// How long the stream is, or 0 for "unknown".
//
// Not decoration: a raw partition has no length, so without one the player reads
// the whole partition and the framer skips a quarter of a megabyte of erased
// flash looking for a sync word on every lap. Sources that know - a file size, a
// Content-Length - report it, and the partition source is told at build time.
[[nodiscard]] std::size_t source_length();

}  // namespace player
