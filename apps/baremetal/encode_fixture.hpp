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

// §E3.5 enhanced coupling. 2/0 at 192 kbit/s, and the layout is the finding.
//
// A third row for one reason: without it the ENCODER's enhanced-coupling path
// is linked into this profile and executed by none of it. The row above uses
// the default tools, which is no coupling at all, so fit_ecpl_band - the
// single largest allocator in the encoder - never ran under any probe. Exactly
// the gap the DECODE side had before fixture.hpp grew its own eac3_ecpl
// stream, and found the same way: by asking what the fixtures do not reach.
//
// WHY 2/0 AND NOT 5.1. Because 5.1 does not fit, and that is worth stating
// rather than working around quietly. Enhanced-coupling encode at 3/2+LFE
// peaks at 343,483 bytes on the host profile and dies on an ESP32-S3 with
// `out_of_memory bytes=147456` - one allocation of 6 channels x 6 blocks x 256
// doubles x 2, against a largest free run smaller than that by the time the
// other two fixtures have run. It is not a ceiling to raise: the part has
// 277,400 bytes free in total and this asks for 343,483.
//
// 2/0 reaches the same code - the same fit_ecpl_band, the same per-band
// amplitude/angle/chaos search, the same §E3.5 syntax - at a third of the
// channel count, so the path is covered and the profile still fits. What is
// NOT covered is 5.1 enhanced-coupling ENCODE on this part, because it cannot
// be; see docs/platforms/esp32.md.
//
// 192 kbit/s 2/0 is 768 bytes an access unit, 4,608 for six.
inline constexpr std::size_t kEac3EcplBytes = 4608;
inline constexpr std::uint64_t kEac3EcplHash = 4460190987537266377ULL;

}  // namespace ac3probe
