#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Where the bitstream comes from, as a seam CMake resolves - the mirror of
// audio_sink.hpp at the other end of the player, and the same rule the library
// follows for its own platform choices.
//
//   source/partition/  a flash partition. The default, and the first one CI
//                      RUNS: it needs no hardware beyond the flash the
//                      application is already in, so it works under QEMU.
//   source/fatfs/      a FAT volume in flash - the SD source's file layer,
//                      runnable under QEMU.
//   source/sd/         an SD card over SDMMC, through FATFS.
//   source/http/       an HTTP body, over WiFi on a board and over QEMU's
//                      emulated Ethernet in CI (source/http/network.hpp).
//
// The player never learns which it has. That is the point: a decoder does not
// care where its bytes were, and an integrator swapping one for another should
// be changing a build option, not a decode loop.
//
// WHAT IS AND IS NOT VERIFIED. partition, fatfs and http run end to end in CI
// under QEMU. sd is COMPILED there and no more - QEMU has no SD host - and its
// file layer is what fatfs exercises; only the SDMMC host stays untested, and
// that is Espressif's driver rather than ours.

namespace player {

// Brings the source up on its current location (the configured default, or
// what source_set_location last accepted). False means the caller should stop
// and say why - a missing card, no network, a partition that is not there.
// May be called again after a stream ends or is stopped, to open the current
// location afresh.
[[nodiscard]] bool source_open();

// Points the source somewhere else before the next source_open(): a URL for
// the HTTP source, a path for a file source. False if this source has only
// the one thing in it (a partition), or the location does not fit. Never
// called while the source is being read.
[[nodiscard]] bool source_set_location(const char* location);

// What source_open() will open, or did.
[[nodiscard]] const char* source_location();

// Fills as much of `dst` as it has. Returns 0 at end of stream, which is what
// makes the player call finish() on the accumulator; a source that would BLOCK
// for more should block rather than return 0, since 0 is not "wait", it is
// "there will never be more".
[[nodiscard]] std::size_t source_read(std::span<std::byte> dst);

// Back to the beginning, for a player that loops. False if this source cannot -
// a socket generally cannot, and saying so beats pretending.
[[nodiscard]] bool source_rewind();

// For the log line, so a run says which source produced its numbers.
[[nodiscard]] const char* source_name();

// How many bytes the stream is, when the source knows - a file size, a
// Content-Length, the build-supplied length of a partition's contents. 0 means
// unknown: read until source_read says there is no more.
[[nodiscard]] std::size_t source_length();

}  // namespace player
