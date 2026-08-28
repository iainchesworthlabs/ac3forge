#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iamf/export.hpp"

// A minimal IAMF (Immersive Audio Model and Formats) OBU/ISOBMFF writer, per the AOM IAMF
// v1.1.0 specification (https://aomediacodec.github.io/iamf/v1.1.0.html, final).
//
// Roadmap item IM3 phase 1 of 3 (see ROADMAP.md): E-AC-3 is not in IAMF's codec list (Opus,
// AAC-LC, FLAC, LPCM only, per IAMF §3.5) and never can be, so this is not a new encoder output -
// it is a decode-then-rewrap bridge to the IAMF/Eclipsa Audio ecosystem, writing the `ipcm`
// (uncompressed LPCM) substreams IAMF §3.11.4 defines for a CHANNEL_BASED audio element whose one
// layer is 7.1.4ch (IAMF §3.6.2 loudspeaker_layout = 7). Phase 2 (object elements) waits on IAMF
// v2.0 going final; phase 3 (an OBU reader) is separate later work.
//
// Standalone and codec-blind by design, in exactly the sense matroska::matroska, mp4::mp4 and
// ac3iab::ac3iab already are: it links nothing from ac3::forge and knows nothing about AC-3,
// E-AC-3 or JOC (see CONTRIBUTING.md's repository-layout section on what a bare `include/iamf/`
// prefix, with no `ac3/`, means). A caller decoding a natively-7.1.4-coded E-AC-3 stream
// (ac3::plan::LayoutId::k714 - independent substream plus two E-AC-3 dependents) already gets the
// 12 discrete channels straight off ac3::Eac3Decoder::decode_access_unit's DecodedAccessUnit -
// this module just needs them permuted into the channel order below and handed over as PCM; see
// examples/mux_iamf.cpp for the full round trip.
//
// Every OBU/box field this module writes is transcribed directly from the published IAMF v1.1.0
// specification, with the section/table number cited at each call site in obu_detail.hpp/
// isobmff_detail.hpp, per CONTRIBUTING.md's clean-room rule. AOM's `libiamf` (BSD-3-Clause-Clear)
// and Open Audio Renderer are oracles only, used to validate this writer's output after the fact -
// never sources this code was transcribed from.
//
// Deliberately small, matching mp4::mux/matroska::mux's own starting point:
//   - one CHANNEL_BASED Audio Element, one layer (7.1.4ch, IAMF's own loudspeaker_layout = 7),
//   - one Mix Presentation with the mandatory Stereo loudness layout plus the 7.1.4 layout,
//   - batch API (every frame known up front) - no fragmented/live writer, no OBU reader,
//   - no Parameter Block OBUs (num_layers == 1 makes every optional parameter definition this
//     module could use, demixing/recon-gain, non-mandatory - see obu_detail.hpp), no Temporal
//     Delimiter OBU (IAMF §2.3.2.2: "MAY or MAY NOT be present"),
//   - ISOBMFF encapsulation only (IAMF §6's `iamf` ISO-BMFF brand / `iacb` box), not the separate
//     standalone raw-OBU-stream representation IAMF §5 also defines.

namespace iamf {

enum class MuxError : std::uint8_t {
    kNoFrames,          // frames is empty
    kInvalidTrack,      // sample_rate/bit_depth not one of IAMF §3.11.4's allowed sets, or
                        // samples_per_frame == 0
    kFrameSizeMismatch,  // a Frame's channel did not carry exactly samples_per_frame samples
};

[[nodiscard]] IAMF_EXPORT std::string_view describe(MuxError error);

// One Temporal Unit's worth of PCM (IAMF §2.3.2.2/§2.4): samples_per_frame samples of each of
// the 12 channels, already ordered the way this module's Audio Element OBU declares them (IAMF
// §3.6.2, loudspeaker_layout = 7, "7.1.4ch"): L, C, R, Lss, Rss, Lrs, Rrs, Ltf, Rtf, Ltb, Rtb,
// LFE. Planar, matching ac3::DecodedAccessUnit::channels' own storage - see
// examples/mux_iamf.cpp for how a caller permutes a decoded access unit's Table E2.5 bit order
// into this one. Every frame SHALL carry exactly AudioTrack::samples_per_frame samples per
// channel (mux() rejects anything else with kFrameSizeMismatch) - this writer never trims, so it
// needs no ISOBMFF edts/elst box (IAMF §6.2.2: those are only required "if there are audio
// samples to be trimmed").
struct Frame {
    std::array<std::vector<float>, 12> channels;
};

// IAMF §3.7.4's LoudnessInfo(): integrated_loudness and digital_peak are mandatory fields (this
// writer always sets info_type = 0, so neither true_peak nor anchored_loudness is written), and
// the specification defines no "unmeasured" sentinel for either. This module is deliberately
// DSP-free, like every other container module here - it takes the caller's own measurement
// rather than computing one - so a caller with no real number may pass the default {0.0F, 0.0F}
// and still produce a structurally valid file.
struct LoudnessInfo {
    float integrated_loudness_lkfs = 0.0F;  // IAMF §3.7.4: LKFS per ITU-R BS.1770-4
    float digital_peak_dbfs = 0.0F;         // IAMF §3.7.4: dBFS
};

struct AudioTrack {
    std::uint32_t sample_rate = 48000;       // IAMF §3.11.4: one of {44100,16000,32000,48000,96000}
    int bit_depth = 24;                      // IAMF §3.11.4: one of {16,24,32}
    std::uint32_t samples_per_frame = 1024;  // this writer's own choice; IAMF does not mandate one
    // IAMF §3.7: every sub-mix SHALL include loudness for the Stereo (Sound System A) layout, in
    // addition to whatever layout(s) the content actually is.
    LoudnessInfo stereo_loudness{};
    LoudnessInfo layout_714_loudness{};
    // Written into moov's hdlr name field (ISO/IEC 14496-12 §8.4.3), matching
    // mp4::MuxOptions::writing_app's own use.
    std::string writing_app{"ac3forge"};
};

// Mux `frames` into a complete IAMF ISOBMFF file (IAMF §6: `iamf`-branded ftyp, an `iamf`
// IASampleEntry carrying an `iacb` IAConfigurationBox, one IA Sample per frame in mdat), returned
// as bytes - no file I/O, matching mp4::mux/matroska::mux's own reasoning: this stays testable
// without touching a disk.
[[nodiscard]] IAMF_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const Frame> frames);

}  // namespace iamf
