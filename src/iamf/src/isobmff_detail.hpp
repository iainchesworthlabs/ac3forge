#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "obu_detail.hpp"

// ISOBMFF-level plumbing for iamf::mux(): the generic box/FullBox primitives (ISO/IEC 14496-12
// §4.2) and builders for every box this writer's file tree needs (IAMF §6's `iamf` ISO-BMFF
// encapsulation: ftyp / moov / trak / mdia / minf / stbl / the `iamf` IASampleEntry and its
// `iacb` IAConfigurationBox / mdat). A fresh implementation rather than a reuse of
// src/mp4/src/isobmff_detail.hpp's own box primitives - that header is explicitly internal to
// mp4's own translation units (see its own header comment), and mp4/matroska already don't share
// EBML/ISOBMFF plumbing with each other either, so duplicating the handful of generic
// put_box/put_fullbox-style helpers here follows the same precedent.
//
// Internal to src/iamf/src/, same reasoning as obu_detail.hpp.

namespace iamf::detail {

inline void put_u64(Bytes& out, std::uint64_t v) {
    put_u32(out, static_cast<std::uint32_t>(v >> 32));
    put_u32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFU));
}

// ISO/IEC 14496-12 §4.2's Box: a 32-bit size (the whole box, header included), then the 4-byte
// type, then the body. Files this writer produces stay well under 4 GiB, so the 64-bit largesize
// escape is never needed.
inline void put_box(Bytes& out, std::string_view fourcc, const Bytes& body) {
    put_u32(out, static_cast<std::uint32_t>(8 + body.size()));
    put_fourcc(out, fourcc);
    put_bytes(out, body);
}

// ISO/IEC 14496-12 §4.2's FullBox: a Box with a 1-byte version and 3-byte flags prepended to the
// body.
inline void put_fullbox(Bytes& out, std::string_view fourcc, std::uint8_t version,
                        std::uint32_t flags, const Bytes& body) {
    Bytes full;
    put_u8(full, version);
    put_u8(full, static_cast<std::uint8_t>(flags >> 16));
    put_u8(full, static_cast<std::uint8_t>((flags >> 8) & 0xFF));
    put_u8(full, static_cast<std::uint8_t>(flags & 0xFF));
    put_bytes(full, body);
    put_box(out, fourcc, full);
}

// ISO/IEC 14496-12 §4.3 FileTypeBox, per IAMF §6.1: compatible_brands SHALL include `iamf` and
// SHOULD include a structural ISOBMFF brand such as `iso6`.
[[nodiscard]] inline Bytes build_ftyp() {
    constexpr std::array<std::string_view, 2> kCompatibleBrands{"iamf", "iso6"};
    Bytes body;
    put_fourcc(body, "iamf");  // major_brand
    put_u32(body, 0);          // minor_version
    for (const auto brand : kCompatibleBrands) {
        put_fourcc(body, brand);
    }
    Bytes out;
    put_box(out, "ftyp", body);
    return out;
}

// ISO/IEC 14496-12 §8.2.2.2/§8.3.2.2's identity transform - mvhd's and tkhd's 3x3 matrix, stored
// row-major in 16.16/2.30 fixed point with an implicit third column.
inline constexpr std::array<std::uint32_t, 9> kUnityMatrix{
    0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000,
};

[[nodiscard]] inline Bytes build_mvhd(std::uint32_t timescale, std::uint64_t duration) {
    Bytes body;
    put_u32(body, 0);  // creation_time
    put_u32(body, 0);  // modification_time
    put_u32(body, timescale);
    put_u32(body, static_cast<std::uint32_t>(duration));
    put_u32(body, 0x00010000);  // rate: 1.0x
    put_u16(body, 0x0100);      // volume: 1.0
    put_u16(body, 0);           // reserved
    put_u32(body, 0);
    put_u32(body, 0);  // reserved[2]
    for (const auto v : kUnityMatrix) {
        put_u32(body, v);
    }
    for (int i = 0; i < 6; ++i) {
        put_u32(body, 0);  // pre_defined[6]
    }
    put_u32(body, 2);  // next_track_ID: this module writes exactly one track (ID 1)
    Bytes out;
    put_fullbox(out, "mvhd", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_tkhd(std::uint64_t duration) {
    Bytes body;
    put_u32(body, 0);  // creation_time
    put_u32(body, 0);  // modification_time
    put_u32(body, 1);  // track_ID
    put_u32(body, 0);  // reserved
    put_u32(body, static_cast<std::uint32_t>(duration));
    put_u32(body, 0);
    put_u32(body, 0);       // reserved[2]
    put_u16(body, 0);       // layer
    put_u16(body, 0);       // alternate_group
    put_u16(body, 0x0100);  // volume: an audio track plays at full volume
    put_u16(body, 0);       // reserved
    for (const auto v : kUnityMatrix) {
        put_u32(body, v);
    }
    put_u32(body, 0);  // width: 0, this is an audio track
    put_u32(body, 0);  // height
    Bytes out;
    // flags 0x000007: Track_enabled | Track_in_movie | Track_in_preview.
    put_fullbox(out, "tkhd", 0, 0x000007, body);
    return out;
}

[[nodiscard]] inline Bytes build_mdhd(std::uint32_t timescale, std::uint64_t duration) {
    Bytes body;
    put_u32(body, 0);  // creation_time
    put_u32(body, 0);  // modification_time
    put_u32(body, timescale);
    put_u32(body, static_cast<std::uint32_t>(duration));
    // language: "und" (undetermined), per IAMF §6.2.2's own recommendation for an IA Track's
    // mdhd/elng when content is not tied to one language - packed per ISO/IEC 14496-12 §8.4.2.2
    // (three 5-bit (letter - 0x60) codes, top bit 0): 'u'=0x15,'n'=0x0E,'d'=0x04.
    put_u16(body, 0x55C4);
    put_u16(body, 0);  // pre_defined
    Bytes out;
    put_fullbox(out, "mdhd", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_hdlr(std::string_view name) {
    Bytes body;
    put_u32(body, 0);          // pre_defined
    put_fourcc(body, "soun");  // handler_type: sound media
    put_u32(body, 0);
    put_u32(body, 0);
    put_u32(body, 0);  // reserved[3]
    for (const char c : name) {
        put_u8(body, static_cast<std::uint8_t>(c));
    }
    put_u8(body, 0);  // NUL-terminated string
    Bytes out;
    put_fullbox(out, "hdlr", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_smhd() {
    Bytes body;
    put_u16(body, 0);  // balance: centred
    put_u16(body, 0);  // reserved
    Bytes out;
    put_fullbox(out, "smhd", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_dinf() {
    // 'url ', flags bit 0 ("media data is in the same file as the Movie Box") set, so no
    // location string is needed in the body.
    Bytes url;
    put_fullbox(url, "url ", 0, 0x000001, {});
    Bytes dref_body;
    put_u32(dref_body, 1);  // entry_count
    put_bytes(dref_body, url);
    Bytes dref;
    put_fullbox(dref, "dref", 0, 0, dref_body);
    Bytes out;
    put_box(out, "dinf", dref);
    return out;
}

// The IASampleEntry (IAMF §6.2.3): `AudioSampleEntry('iamf')` (ISO/IEC 14496-12 §12.2.3, itself
// extending §8.5.2's SampleEntry) with channelcount and samplerate both zeroed - IAMF §6.2.3:
// "The channelcount field ... SHALL be set to 0. The samplerate field ... SHALL be set to 0 ...
// Parsers SHALL ignore these two fields" - followed by one child box, the IAConfigurationBox
// (`iacb`, IAMF §6.2.4) carrying `config_obus` (the four Descriptor OBUs, built by
// build_config_obus() in iamf.cpp).
[[nodiscard]] inline Bytes build_sample_entry(std::span<const std::byte> config_obus) {
    Bytes body;
    put_u32(body, 0);
    put_u16(body, 0);  // SampleEntry::reserved[6]
    put_u16(body, 1);  // SampleEntry::data_reference_index
    put_u32(body, 0);
    put_u32(body, 0);  // AudioSampleEntry::reserved[2]
    put_u16(body, 0);  // channelcount: SHALL be 0 (IAMF §6.2.3)
    put_u16(body, 0);  // samplesize: ignored per IAMF §6.2.3, same as channelcount/samplerate
    put_u16(body, 0);  // pre_defined
    put_u16(body, 0);  // reserved
    put_u32(body, 0);  // samplerate: SHALL be 0 (IAMF §6.2.3); no SamplingRateBox

    Bytes iacb_body;
    put_u8(iacb_body, 1);  // configurationVersion: SHALL be 1 (IAMF §6.2.4)
    put_leb128(iacb_body, config_obus.size());  // configOBUs_size
    put_bytes(iacb_body, config_obus);
    put_box(body, "iacb", iacb_body);

    Bytes out;
    put_box(out, "iamf", body);
    return out;
}

[[nodiscard]] inline Bytes build_stsd(std::span<const std::byte> config_obus) {
    Bytes body;
    put_u32(body, 1);  // entry_count: exactly one sample description
    put_bytes(body, build_sample_entry(config_obus));
    Bytes out;
    put_fullbox(out, "stsd", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_stts(std::uint32_t sample_count, std::uint32_t sample_delta) {
    Bytes body;
    put_u32(body, 1);  // entry_count: every IA Sample carries samples_per_frame samples (IAMF
                       // §6.2.2: "the number of audio samples in an IA Sample")
    put_u32(body, sample_count);
    put_u32(body, sample_delta);
    Bytes out;
    put_fullbox(out, "stts", 0, 0, body);
    return out;
}

// One sample per chunk - the simplest legal stsc/stco pairing, matching
// src/mp4/src/isobmff_detail.hpp's own build_stsc for the identical reasoning.
[[nodiscard]] inline Bytes build_stsc(std::uint32_t chunk_count) {
    Bytes body;
    put_u32(body, 1);  // entry_count
    put_u32(body, 1);  // first_chunk
    put_u32(body, 1);  // samples_per_chunk
    put_u32(body, 1);  // sample_description_index
    (void)chunk_count;
    Bytes out;
    put_fullbox(out, "stsc", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_stsz(std::span<const Bytes> samples) {
    Bytes body;
    put_u32(body, 0);  // sample_size: 0 means "read each size from the table below" - IA Samples
                       // vary in size (different frames' Audio Frame OBU payloads differ)
    put_u32(body, static_cast<std::uint32_t>(samples.size()));
    for (const auto& sample : samples) {
        put_u32(body, static_cast<std::uint32_t>(sample.size()));
    }
    Bytes out;
    put_fullbox(out, "stsz", 0, 0, body);
    return out;
}

[[nodiscard]] inline Bytes build_stco(std::span<const std::uint32_t> chunk_offsets) {
    Bytes body;
    put_u32(body, static_cast<std::uint32_t>(chunk_offsets.size()));
    for (const auto offset : chunk_offsets) {
        put_u32(body, offset);
    }
    Bytes out;
    put_fullbox(out, "stco", 0, 0, body);
    return out;
}

}  // namespace iamf::detail
