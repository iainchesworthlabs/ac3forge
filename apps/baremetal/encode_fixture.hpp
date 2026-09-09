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
// Since 2026-09-10 the profile's encoders run their analysis front end -
// transient detection, the block gather, the analysis window and the forward
// transform - in float (ac3/internal/encode_scalar.hpp, float32 under this
// profile), so these hashes are the FLOAT front end's bitstreams: the same
// recipe as the ordinary build's with float rounding, a different and equally
// valid stream. The host shape of the profile is the same float build, which
// is what keeps "regenerate on the host" true; the ordinary double build's
// streams are pinned elsewhere, by tests/golden/bitstream-hashes.json.
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
inline constexpr std::uint64_t kAc3Hash = 17097054118981639700ULL;

// E-AC-3 5.1 at 384 kbit/s: 1,536 bytes an access unit, 9,216 for six.
inline constexpr std::size_t kEac3Bytes = 9216;
inline constexpr std::uint64_t kEac3Hash = 11442942162113356738ULL;

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
inline constexpr std::uint64_t kEac3EcplHash = 6441094702899796872ULL;

// AC-3 2/0 at 192 kbit/s - the decode probe's ac3_stereo shape seen from the
// other side, and the layout most AC-3 encode on a small part actually is.
// 768 bytes a frame, 4,608 for six. Only two of the PCM block's channels are
// read: the encoder's own layout decides how many spans it takes.
inline constexpr std::size_t kAc3StereoBytes = 4608;
inline constexpr std::uint64_t kAc3StereoHash = 1195752761152509359ULL;

// E-AC-3 2/0 at 192 kbit/s with the default tools, which is none. The same
// layout and rate as the enhanced-coupling row above with §E3.5 off, so the
// two together say what the tool costs - in peak bytes and, under --icount,
// in instructions - at the same input. 768 bytes an access unit, 4,608 for six.
inline constexpr std::size_t kEac3StereoBytes = 4608;
inline constexpr std::uint64_t kEac3StereoHash = 17721662050392400292ULL;

// E-AC-3 2/0 at 192 kbit/s with standard coupling, spectral extension and the
// adaptive hybrid transform all in use - the tools the 5.1 row above never
// reaches, its default being no tool at all.
//
// WHY 2/0, AGAIN. The same finding as the enhanced-coupling row's, with the
// tools' own numbers: 5.1 at 256 kbit/s with the three permitted peaks at
// 369,790 bytes on the host profile, and each on its own says which part of
// that is whose - AHT alone 312,744, standard coupling alone 289,202,
// spectral extension alone 205,718, against 223,020 for the plain 5.1 row.
// AHT's per-channel six-block store and coupling's shared-channel state are
// what a 5.1 encode with either tool cannot fit beside on an ESP32-S3 whose
// encode build leaves 241,664 bytes in its largest free run.
//
// WHY THE BAND EDGES ARE PINNED. At 2/0 and 192 kbit/s the rate defaults put
// spectral extension's start below where coupling would begin, and §E3.3.1
// derives the coupling end from the spx start, so with both merely permitted
// the encoder drops coupling and the frame is spx+aht - the same bytes as
// asking for those two alone. cplbegf 0 (coupling from coefficient 37) and
// spxbegf 7 (synthesis from the highest start code) leave a coupling region
// between them, and `ac3cli probe` on the frame reports coupling in 6 of 6
// blocks, spx in 6 of 6 and AHT in the syncframe. 768 bytes an access unit,
// 4,608 for six.
inline constexpr std::size_t kEac3ToolsBytes = 4608;
inline constexpr std::uint64_t kEac3ToolsHash = 1673449135140366971ULL;

}  // namespace ac3probe
