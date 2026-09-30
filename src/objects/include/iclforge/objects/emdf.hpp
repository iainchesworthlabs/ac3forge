#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/bitwriter.hpp"
#include "ac3/export.hpp"

// The Extensible Metadata Delivery Format - ETSI TS 102 366 Annex H. A/52:2018
// Annex H defers to it wholesale rather than restating it, so this is the one
// piece of the bitstream whose authority is the ETSI text.
//
// EMDF is the sanctioned way to put data an AC-3 decoder has never heard of
// into an AC-3 or E-AC-3 frame. §H.1 places the container in a reserved space -
// it names auxdata and the per-block skip fields - so a decoder that knows
// nothing about it reads the frame exactly as it did before. That is precisely
// what makes Dolby Atmos in DD+ backward compatible: the object metadata rides
// here, not as extra coded channels, and a plain 5.1 decoder is untouched.
//
// Nothing in the container is codec state. It is a length-prefixed bag of
// identified payloads, so this module knows only how to pack bits, and the
// OAMD and JOC payload writers know nothing about where they end up.

namespace ac3::emdf {

// §H.2.2.1.1. The container is found by scanning for this word rather than by
// a fixed offset, because nauxbits is only known after the audio is decoded.
inline constexpr std::uint16_t kSyncWord = 0x5838;

// §H.2.1.2.1 / §H.2.2.2.1. A value is sent as groups of `group_bits`, most
// significant group first, each followed by a read_more bit. A k-group field
// carries the value MINUS the offset (2^n + 2^2n + ... + 2^(k-1)n), which is
// what stops the encodings overlapping: every value has exactly one shortest
// form, and the decoder recovers the offset from the group count alone.
AC3FORGE_EXPORT void put_variable_bits(BitWriter& w, std::uint32_t value, int group_bits);
[[nodiscard]] AC3FORGE_EXPORT int variable_bits_size(std::uint32_t value, int group_bits);

// Table H.2.3 assigns 0x1..0x7 and reserves the rest; TS 103 420 Table 55
// claims two of the reserved values for object audio.
inline constexpr int kPayloadIdOamd = 11;
inline constexpr int kPayloadIdJoc = 14;

struct Payload {
    int id = 0;
    // Whole bytes: emdf_payload_size (§H.2.2.2.5) counts bytes, so a payload
    // that does not fill its last byte has to pad itself.
    std::span<const std::byte> bytes;
};

// emdf_sync() followed by emdf_container(), zero-padded to a whole number of
// bytes as §H.2.2.1.2 requires.
//
// Every payload is written with the configuration TS 103 420 Table 56 makes
// mandatory for object audio - frame-aligned, no sample offset, no duration,
// grouped, highest priority, no processing allowed - because that is the only
// configuration this encoder ever needs and the only one a JOC decoder will
// accept. `groupid` ties the OAMD and JOC payloads together and must be equal
// for both.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_container(
    std::span<const Payload> payloads, int groupid = 0);

// --- Decode ------------------------------------------------------------

// §H.2.1.3's emdf_payload_config, as it was actually found on the wire.
//
// build_container() only ever writes the one shape TS 103 420 Table 56
// mandates for object audio, but real Dolby streams do not restrict
// themselves to it even for their own object payloads: a DD+ JOC stream from
// the Dolby Encoding Engine sends the OAMD payload with
// payload_frame_aligned == 0 and the JOC payload with it set, in the same
// container, alongside two further payloads that use `duratione` and
// `discard_unknown_payload`. So the reader parses the whole clause and
// reports what it found rather than insisting on one configuration.
struct PayloadConfig {
    // §H.2.2.3.2 smploffst, in samples; -1 when smploffste is 0, which is
    // "the payload applies from the frame's first sample".
    int sample_offset = -1;
    // §H.2.2.3.4 duration, in samples; -1 when duratione is 0 ("to the
    // frame's last sample").
    std::int64_t duration = -1;
    // §H.2.2.3.6 groupid; -1 when groupide is 0.
    std::int64_t group_id = -1;
    // §H.2.2.3.7. The eight reserved bits that follow are skipped.
    bool codec_data = false;
    bool discard_unknown = false;
    // §H.2.2.3.11 and the fields it gates. `frame_aligned` is false both
    // when the flag was sent as 0 and when the syntax never reached it
    // (discard_unknown, or a sample offset).
    bool frame_aligned = false;
    bool create_duplicate = false;
    bool remove_duplicate = false;
    // §H.2.2.3.14/.15, present only when smploffste or payload_frame_aligned
    // is set; 0 (highest priority / discard if processed) otherwise, which is
    // what the absent case means anyway.
    int priority = 0;
    int proc_allowed = 0;
};

// One payload as recovered from a container. `bytes` is freshly materialized
// (not a view into the input), because a payload's bits are not generally
// byte-aligned within the frame that carried them.
struct DecodedPayload {
    // §H.2.2.2.2. Ids at or above 0x1F arrive through the extension escape,
    // so this is the resolved value, not the 5-bit field.
    int id = 0;
    PayloadConfig config{};
    std::vector<std::byte> bytes;
};

// A container found whose bits cannot be walked cleanly. `kTruncated` covers
// a declared length or payload size that runs past `data`;
// `kUnsupportedConfig` covers the two places where continuing would mean
// inventing syntax rather than reading it - an `emdf_version` other than 0
// (§H.2.2.2 defines the container's fields only for version 0, so every
// offset after it is unknown) and a `protection_length_primary` of 0b00,
// which Table H.2.5 reserves and therefore gives no field width to skip.
//
// An unfamiliar payload id, sample offset, duration, priority or alignment
// is NOT an error: every one of those has a defined width, so the reader
// walks past it and reports it on PayloadConfig.
enum class ParseError : std::uint8_t {
    kTruncated,
    kUnsupportedConfig,
};

// Decode-side inverse of build_container(). Scans `data` bit by bit for
// kSyncWord (§H.2.2.1.1 - the container's position is not fixed, so a
// decoder locates it the same way an encoder's reader would), then walks the
// payload list into {id, config, bytes} triples, stopping at the terminating
// 0 payload id and ignoring the trailing protection bits (§H.2.2.4: their
// content is implementation-defined and unverifiable by any decoder that
// does not share the algorithm that produced them).
//
// std::nullopt means no sync word was found anywhere in `data` - the
// ordinary shape of a skip field with no EMDF at all, not an error. A sync
// word that IS found but whose container cannot be read cleanly reports
// ParseError instead of a best-effort guess.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::optional<std::vector<DecodedPayload>>, ParseError>
parse_container(std::span<const std::byte> data);

}  // namespace ac3::emdf
