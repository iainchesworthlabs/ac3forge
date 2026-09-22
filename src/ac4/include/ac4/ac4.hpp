#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac4/export.hpp"

// AC-4 sync-frame / table-of-contents / presentation / substream-group
// framing. ETSI TS 103 190-1 V1.4.1 (2025-07), "Digital Audio Compression
// (AC-4) Standard; Part 1: Channel based coding", and ETSI TS 103 190-2
// V1.3.1 (2025-07), "... Part 2: Immersive and personalized audio". Section
// numbers on each declaration cite whichever part actually defines that
// element; Part 2 clause 6 supersedes Part 1 clause 4 for bitstream_version
// >= 2 (see Toc::bitstream_version and parse_toc()).
//
// This is a bitstream INSPECTOR, not a decoder: audio_data and metadata()
// payloads are reported as byte ranges (Substream::audio_size, Substream
// itself), never decoded. It is deliberately codec-blind in the same sense
// mpegts::/mp4::/matroska:: are - it depends on nothing under ac3::forge,
// and knows nothing about AC-3, E-AC-3 or Atmos.
//
// Scope covers both channel-coded and object/A-JOC-coded substream groups
// (b_channel_coded 1 or 0): TOC/presentation/substream-group/substream-info
// framing for A-JOC-coded (§6.3.2.8), direct-coded-object (§6.3.2.10) and
// OAMD (§6.3.2.12) substreams is parsed the same way the channel-coded path
// is - object position/bed assignment (bed_dyn_obj_assignment(), §6.2.1.10)
// included. One piece is deliberately not: oamd_common_data() (§6.2.8.1),
// reachable only via ac4_substream_info_ajoc()'s own
// b_oamd_common_data_present flag, is a large separate metadata structure
// (bed assignment, DRC, target-device categories, dialogue enhancement) -
// a stream setting that flag is refused cleanly (Error::kOamdCommonDataPresent)
// rather than misparsed. The OAMD substream DATA payload itself
// (oamd_substream(), §6.2.2.4) was never in scope either way - like every
// non-audio substream, it is reported as a byte range only.
//
// The bitstream_version >= 2 path (TS 103 190-2 clause 6, presentation_v1
// and substream-group framing) is cross-checked against real Dolby
// Encoding Engine 6.5.4 output - both plain-channel and 5.1.4
// channel-based-immersive encodes - byte for byte against
// tools/references/ac4_parse.py's independent transcription, and
// semantically against MediaInfo's own AC-4 reader. The bitstream_version
// <= 1 path (legacy TS 103 190-1 ac4_toc()/ac4_presentation_info()) has no
// such stream to test against - no encoder available to this project
// writes it - so it is transcribed and page-verified against the published
// spec text only.
//
// A-JOC/direct-coded-object/OAMD framing has a narrower verification story
// still: no real stream reaches it either - `dee_ac4ajoc_encoder.exe`
// accepts only an Atmos ADM BWF mezzanine, which this project's own tooling
// cannot produce one DEE accepts (the same "gates on content provenance,
// not syntax" limit docs/verification.md already states for the AC-3/
// E-AC-3 side), and `dee_ac4ims_encoder.exe` - the other locally available
// object-adjacent encoder, despite its name - was confirmed to stay
// channel-coded regardless. What stands in for it is a set of synthetic,
// hand-built bitstreams cross-checked between this parser and
// tools/references/ac4_parse.py, each built by an independent bit writer
// in neither module - see tests/ac4/test_ac4.cpp. See docs/verification.md.

namespace ac4 {

enum class Error : std::uint8_t {
    kTruncated,
    kLostSync,
    kUnsupportedBitstreamVersion,  // > 2; TS 103 190-2 §6.3.2.1.1
    kOamdCommonDataPresent,        // see module docs above
};

[[nodiscard]] AC4_EXPORT std::string_view describe(Error error);

// Annex G.3.1 ac4_syncframe(). `raw_ac4_frame` is the frame_size-bounded
// span passed to parse_raw_frame() - everything between the frame_size
// field and the optional trailing crc_word.
struct SyncFrame {
    std::size_t offset = 0;
    std::uint16_t sync_word = 0;  // 0xAC40 or 0xAC41 (Annex G.4.1)
    std::span<const std::byte> raw_ac4_frame;
    // nullopt when sync_word == 0xAC40 (no crc_word transmitted); Annex
    // G.4.2's CRC-16 (poly x^16+x^15+x^2+1, init 0, no reflection, no
    // final XOR) otherwise.
    std::optional<bool> crc_ok;
};

struct ScanResult {
    std::vector<SyncFrame> frames;
    // Set when the walk stopped before consuming all of `data` - either a
    // sync word that did not match 0xAC40/0xAC41 (kLostSync) or a frame
    // whose declared frame_size runs past the end of `data` (kTruncated).
    // `frames` still holds everything found before that point.
    std::optional<Error> stopped_at;
    std::size_t stopped_at_offset = 0;
};

// Walks ac4_syncframe() elements back to back. Never throws; a malformed
// tail is reported via ScanResult::stopped_at rather than losing whatever
// parsed cleanly before it.
[[nodiscard]] AC4_EXPORT ScanResult scan(std::span<const std::byte> data);

// --- §4.2.3.7 content_type --------------------------------------------------

struct ContentType {
    int content_classifier = 0;  // Table 91
    std::optional<std::vector<std::byte>> language_tag;
};

// --- §4.2.3.6 ac4_substream_info (presentation_version 0) / §6.2.1.8
// --- ac4_substream_info_chan (presentation_version 1) ----------------------

struct OriginalContent {
    // §6.3.2.7.3-.5: whether channels the coded channel_mode implies exist
    // are actually populated in the source, or carry encoded silence.
    bool b_4_back_channels_present = false;
    bool b_centre_present = false;
    int top_channels_present = 0;  // Table 59
};

struct ChannelSubstreamInfo {
    int channel_mode = 0;           // raw code, Table 88 or Table 56
    std::string channel_mode_name;  // e.g. "Stereo", "7.1.4"
    std::optional<int> ch_mode;     // nullopt for a reserved code
    std::optional<OriginalContent> original_content;
    std::optional<int> sf_multiplier;
    std::optional<int> bitrate_kbps;          // nullopt if unmapped ("unlimited" or reserved)
    std::optional<ContentType> content_type;  // presentation_version 0 only
    std::optional<int> substream_index;       // index into Toc::substream_sizes
};

// --- §6.2.1.10 bed_dyn_obj_assignment / §6.3.2.10.8 -------------------------

enum class ObjectKind : std::uint8_t { kBed, kDyn, kIsf };

struct ObjectEntry {
    ObjectKind kind = ObjectKind::kDyn;
    bool lfe = false;
    bool ajoc_coded = false;
};

// --- §6.2.1.13 oamd_substream_info ------------------------------------------

struct OamdSubstreamInfo {
    bool b_oamd_ndot = false;
    std::optional<int> substream_index;
};

// --- §6.2.1.9 ac4_substream_info_ajoc ---------------------------------------

struct AjocSubstreamInfo {
    bool b_lfe = false;
    bool b_static_dmx = false;
    int n_fullband_dmx_signals = 0;
    std::vector<ObjectEntry> static_objects;   // empty when b_static_dmx
    int n_fullband_upmix_signals = 0;
    std::vector<ObjectEntry> upmix_objects;
    std::optional<int> sf_multiplier;
    std::optional<int> bitrate_kbps;
    std::optional<int> substream_index;
};

// --- §6.2.1.11 ac4_substream_info_obj ---------------------------------------

struct ObjSubstreamInfo {
    std::vector<ObjectEntry> objects;
    bool b_dynamic_objects = false;
    std::optional<int> sf_multiplier;
    std::optional<int> bitrate_kbps;
    std::optional<int> substream_index;
};

// --- §6.2.1.6 ac4_substream_group_info --------------------------------------

// One entry of a substream group's own substream list. Exactly one of
// `chan`/`ajoc`/`obj` is set, selected by `kind` - a tagged union rather
// than std::variant so callers can query without visiting.
struct GroupSubstream {
    enum class Kind : std::uint8_t { kChan, kAjoc, kObj };
    Kind kind = Kind::kChan;
    std::optional<ChannelSubstreamInfo> chan;
    std::optional<AjocSubstreamInfo> ajoc;
    std::optional<ObjSubstreamInfo> obj;
};

struct SubstreamGroupInfo {
    bool b_substreams_present = false;
    bool b_channel_coded = true;
    std::optional<OamdSubstreamInfo> oamd;  // set only when !b_channel_coded and b_oamd_substream
    std::vector<GroupSubstream> substreams;
    std::optional<ContentType> content_type;
};

// --- §4.2.3.2 ac4_presentation_info (bitstream_version <= 1) ---------------

struct PresentationInfoV0 {
    int presentation_version = 0;
    std::optional<int>
        presentation_config;       // Table 85; nullopt for a single-substream presentation
    std::optional<int> md_compat;  // Table 86
    std::optional<int> presentation_id;
    std::vector<std::pair<std::string, ChannelSubstreamInfo>> substreams;  // role, info
};

// --- §6.2.1.3 ac4_presentation_v1_info (bitstream_version >= 2) ------------

struct PresentationInfoV1 {
    int presentation_version = 0;
    std::optional<int> presentation_config;  // Table 53
    std::vector<int> group_refs;             // ac4_sgi_specifier() group_index values
    std::optional<int> md_compat;            // Table 55
    std::optional<bool> enable_presentation;
    int frame_rate_factor = 1;  // Table 87; threaded into this frame's substream groups
};

// --- §4.2.1 / §6.2.1.1 ac4_toc ---------------------------------------------

struct Toc {
    int bitstream_version = 0;
    int sequence_counter = 0;
    std::optional<int> wait_frames;  // Table 81
    int sample_rate_hz = 48000;      // Table 82
    int frame_rate_index = 0;        // Table 83/84
    bool b_iframe_global = false;
    int n_presentations = 0;
    int payload_base = 0;  // bytes, relative to the end of the byte-aligned ac4_toc()

    // Exactly one of these two is populated, selected by bitstream_version
    // (see parse_toc()): presentations_v0 for <= 1, presentations_v1 and
    // substream_groups for >= 2.
    std::vector<PresentationInfoV0> presentations_v0;
    std::vector<PresentationInfoV1> presentations_v1;
    std::vector<SubstreamGroupInfo> substream_groups;

    int n_substreams = 0;
    std::vector<int> substream_sizes;  // bytes, §4.3.3.12.4
};

// --- §4.2.4.2 / §6.2.2.2 ac4_substream: outer envelope only -----------------

struct Substream {
    std::size_t offset = 0;  // byte offset of ac4_substream_data() within the raw frame
    std::size_t size = 0;    // bytes, from Toc::substream_sizes
    // True when this index was referenced by an ac4_substream_info()/
    // ac4_substream_info_chan()/ac4_substream_info_ajoc()/
    // ac4_substream_info_obj() element - i.e. this is an ac4_substream()
    // this parser knows how to read the audio_size header of (Table 50:
    // all four map to the same envelope). False covers
    // ac4_presentation_substream(), oamd_substream() and
    // emdf_payloads_substream() (§6.2.1.12, §6.2.2.4, §4.2.4.4) - different
    // shapes, reported by byte range only.
    bool is_audio = false;
    std::optional<int> audio_size;  // §4.3.4.1, only set when is_audio
};

struct RawFrame {
    Toc toc;
    std::vector<Substream> substreams;
};

// §4.2.1 raw_ac4_frame(): ac4_toc() then n_substreams substream payloads,
// located via payload_base and substream_index_table()'s sizes
// (§4.3.3.12.4's Pseudocode 1) rather than by parsing through audio_data.
[[nodiscard]] AC4_EXPORT std::expected<RawFrame, Error> parse_raw_frame(
    std::span<const std::byte> raw_ac4_frame);

}  // namespace ac4
