#pragma once

#include <cstddef>
#include <cstdint>

// What apps/baremetal/encode_probe.cpp expects its own encode to produce
// (roadmap PF7).
//
// Unlike the decode side's fixture.hpp this is not generated from a committed
// WAV by a committed tool - there is no WAV. The probe synthesises its input
// from a formula (see fill_signal) precisely so that six frames of 5.1 PCM does
// not have to be linked into an image, and these are the sizes and checksums
// that formula's output encodes to.
//
// To regenerate: build the host shape of the profile and run it. It prints
// every number below on its own key=value lines, and it is the same code the
// target runs, so no second implementation of the generator has to be kept
// agreeing with the first.
//
//     cmake --preset config-linux-gcc-minimal-encoder
//     cmake --build --preset build-linux-gcc-minimal-encoder
//     ./build/config-linux-gcc-minimal-encoder/bin/ac3probe
//
// What a hash match establishes and what it does not: it says the target's
// encoder produced the same bitstream the host's did from the same input. It
// does NOT say either is correct - tests/golden/bitstream-hashes.json and the
// FFmpeg/Dolby comparisons in tools/ci/ are what say that, on the host where
// there is something to compare against. This is a REGRESSION reference, the
// same standing fixture.hpp has on the decode side.

namespace ac3probe {

inline constexpr int kEncodeFrames = 6;

// AC-3 5.1 at 448 kbit/s. 1,792 bytes a frame at 48 kHz, so six frames is
// 10,752 - a fixed rate, which is why the size is worth checking separately
// from the hash: a wrong size is a framing fault, a wrong hash with the right
// size is an arithmetic one.
inline constexpr std::size_t kAc3Bytes = 10752;
inline constexpr std::uint64_t kAc3Hash = 5257466536860864961ULL;

// E-AC-3 5.1 at 384 kbit/s: 1,536 bytes an access unit, 9,216 for six.
inline constexpr std::size_t kEac3Bytes = 9216;
inline constexpr std::uint64_t kEac3Hash = 1012121234525177924ULL;

}  // namespace ac3probe
