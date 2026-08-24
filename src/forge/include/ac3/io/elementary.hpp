#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Reading the shape of an AC-3 or E-AC-3 elementary stream back off the wire.
//
// This is the inverse of the encoder's framing: given a bare byte stream, find
// the access-unit boundaries and work out what the stream actually carries.
// A muxer needs exactly this - a container has to know where packets begin and
// how many channels to declare - and deriving it from the bitstream beats
// asking the caller, who can be wrong.
//
// Both formats put bsid at bit 40, deliberately, so a reader can tell them
// apart before committing to a layout: AC-3 spends its first 40 bits on
// syncword, crc1, fscod and frmsizecod, and E-AC-3 on syncword, strmtyp,
// substreamid, frmsiz, fscod, numblkscod, acmod and lfeon.

namespace ac3::io {

enum class StreamKind : std::uint8_t {
    kAc3,   // bsid <= 10
    kEac3,  // bsid 16 (Annex E)
};

enum class ScanError : std::uint8_t {
    kEmpty,
    kLostSync,
    kUnsupportedBsid,
    kReservedValue,
    kTruncated,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(ScanError error);

// One syncframe's bit stream information, read straight off the wire without
// decoding any audio.
//
// This is the bounded, always-affordable half of reading a stream: syncinfo
// plus the whole of bsi (Table 5.2 for AC-3, Table E1.2 for E-AC-3), stopping
// at the first audio block. Everything here is a transmitted field or an
// immediate consequence of one - nothing is derived from the audio, and
// nothing needs the frame to decode, so a frame whose audio a decoder would
// refuse still reports its header truthfully. `ac3cli probe` is built on
// exactly that property; scan() below is the same walk with only the first
// access unit's answers kept.
struct FrameHeader {
    StreamKind kind = StreamKind::kAc3;
    // The whole syncframe, from its sync word: §5.4.1's frame_size_bytes for
    // AC-3, (frmsiz + 1) * 2 for E-AC-3.
    std::size_t bytes = 0;
    int bsid = 0;
    // §5.4.2.1 / Annex E's infomdate payload. 0 ("not indicated") when the
    // frame carried no bsmod at all, matching ScannedStream::bsmod.
    int bsmod = 0;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    // §5.4.2.9: std::nullopt where compre was clear, so "no word" and "a word
    // that says unity" stay distinguishable - the same convention
    // DecodedFrame::compr keeps.
    std::optional<std::uint8_t> compr = std::nullopt;
    // Ch2's own pair (§5.4.2.16-18), present only for acmod 1+1.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;

    // --- E-AC-3 only (Table E1.2) ------------------------------------------
    eac3::StreamType strmtyp = eac3::StreamType::kIndependent;
    int substreamid = 0;
    // §E2.3.1.4. Reported as 0x3 for a reduced-rate frame, which transmits no
    // numblkscod at all and is implicitly six blocks - the same convention the
    // decoder's own Bsi keeps, with `reduced_rate` below saying which of the
    // two produced it.
    int numblkscod = 3;
    // §E2.3.1.3: fscod was 0x3 and the rate came from fscod2 (24/22.05/16 kHz),
    // a case AC-3 has no counterpart for.
    bool reduced_rate = false;
    // §E2.3.1.8: only a dependent substream may carry one.
    std::optional<std::uint16_t> chanmap = std::nullopt;
    // TS 103 420 §8.3.2.2's complexity_index_type_a, when this substream's own
    // addbsi carried the flag - see ScannedStream::oba_complexity_index.
    std::optional<int> oba_complexity_index = std::nullopt;

    // --- AC-3 only ---------------------------------------------------------
    // Table 5.18's index into kBitratesKbps, i.e. frmsizecod >> 1.
    int bit_rate_code = 0;
    // The rate that index names. E-AC-3 has no such field - its rate is
    // whatever `bytes` works out to over the frame's own duration.
    std::uint32_t bitrate_kbps = 0;

    // Full-bandwidth channels plus the LFE, as this syncframe codes them.
    [[nodiscard]] int coded_channels() const {
        return fullbw_channel_count(acmod) + (lfe ? 1 : 0);
    }
};

// Reads the header of the syncframe starting at `at`. `at` must begin with a
// sync word and hold at least the whole of bsi; it may be longer (the rest of
// the stream is fine) - FrameHeader::bytes says where the frame itself ends,
// which is not checked against `at.size()` here because a caller walking a
// stream needs that length in order to do the checking.
[[nodiscard]] AC3FORGE_EXPORT std::expected<FrameHeader, ScanError> read_frame_header(
    std::span<const std::byte> at);

struct ScannedStream {
    StreamKind kind = StreamKind::kAc3;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;  // of the first (or only) substream
    bool lfe = false;
    // Channels the stream RENDERS, which for E-AC-3 folds in every dependent
    // substream's chanmap and so is not the bed's channel count.
    int channels = 0;
    // One entry per access unit: an AC-3 syncframe, or an E-AC-3 independent
    // substream together with the dependents that follow it. Spans point into
    // the caller's buffer.
    std::vector<std::span<const std::byte>> access_units{};
    // Samples each of those access units codes, parallel to `access_units`.
    // Always 1536 for AC-3 (§5.3.1: six blocks of 256, no other option), but
    // E-AC-3's numblkscod lets an independent substream code 1, 2, 3 or 6
    // blocks (§E2.3.1.4), so an E-AC-3 access unit is 256, 512, 768 or 1536
    // samples long and a stream may mix lengths. Kept here rather than
    // recomputed by every caller because the scan has already read
    // numblkscod off the wire and nobody downstream should have to parse a
    // syncframe again to find out how long it is - see access_unit_timing()
    // below for what this is actually for.
    std::vector<std::uint32_t> access_unit_samples{};
    // Substreams in the first access unit; always 1 for AC-3.
    std::size_t substreams_per_unit = 0;

    // The raw syntax values below exist for build_codec_config_box() (see
    // ac3/io/dec3.hpp): an ISOBMFF dac3/dec3 box wants bsid/bsmod/bit-rate
    // straight off the bitstream, not just the derived channel summary
    // above, and a container muxer has no business re-deriving them itself.
    // Every one of them is captured from the same first-access-unit walk
    // that fills in acmod/lfe/sample_rate above.

    // A/52 §5.4.1.3 / Annex E §E2.3.1.6.
    int bsid = 0;
    // §5.4.2.1 / Annex E's infomdate payload (0 when infomdate was clear,
    // matching "not indicated" - see Table 5.5's own bsmod semantics).
    int bsmod = 0;
    // AC-3 only: Table 5.18's index into kBitratesKbps (0-18), exactly what
    // AC3SpecificBox's bit_rate_code reports. Meaningless for E-AC-3, which
    // has no equivalent fixed-table field (see build_codec_config_box()).
    int bit_rate_code = 0;

    // TS 103 420 §8.3.1/§8.3.2.2: flag_ec3_extension_type_a and, when it is
    // set, complexity_index_type_a - read out of the first substream's
    // addbsi that carries them (see encoder/eac3_frame.hpp's
    // oba_complexity_index for the write side). This is the only Atmos/JOC
    // marker readable without decoding the EMDF container itself, and what
    // a dec3 box's own Atmos extension echoes verbatim. std::nullopt for a
    // stream that never sets the flag - AC-3 included, since addbsi's
    // object-audio use is E-AC-3 only.
    std::optional<int> oba_complexity_index = std::nullopt;

    // The stream's rendered channel LOCATIONS as one ATSC A/52-2018 Table
    // E2.5 custom-channel-map word: bit 0 (Left) in the most significant bit
    // through bit 15 (LFE) in the least, six of the sixteen naming a PAIR
    // rather than one channel (see ac3::eac3::chanmap). `channels` above is
    // this word's channel count and nothing more - the scan already unions
    // the independent substream's acmod/lfeon with every dependent's own
    // chanmap to compute it (§E3.8.2), so keeping the word itself costs
    // nothing and answers questions a bare count cannot: which locations,
    // not how many.
    //
    // For AC-3 there are no dependents to union, so this is just acmod/lfeon
    // expressed in the same vocabulary. 1+1 (dual mono) has no Table E2.5
    // location at all - Ch1/Ch2 are independent programmes rather than
    // directions - and stands in as Left|Right there, the same placeholder
    // ac3::eac3::chanmap::acmod_map() already uses for the channel count's
    // sake.
    //
    // Written for ac3::io::dash_channel_configuration() (ac3/io/dec3.hpp),
    // whose DASH AudioChannelConfiguration @value IS this word in hex.
    std::uint16_t channel_map = 0;
};

[[nodiscard]] AC3FORGE_EXPORT std::expected<ScannedStream, ScanError> scan(
    std::span<const std::byte> stream);

// --- timing ----------------------------------------------------------------
//
// Where access unit i starts and how long it lasts. Every container writer in
// this project computes this privately from a samples_per_frame it was handed
// (mp4::AudioTrack, mpegts::AudioTrack, matroska::AudioTrack all take one),
// which is correct only while every access unit is the same length - true of
// everything this project's own encoders produce and not true in general, and
// in any case not something a caller could ask about before this existed.
//
// The arithmetic is deliberately integer: a frame duration is very often not
// a whole number of ticks in whatever timescale a container uses (1536
// samples at 44.1 kHz is 34.83 ms), so a running sum of per-frame increments
// drifts. Every value below is computed from the ABSOLUTE sample position, so
// the error against the true time never exceeds one tick however long the
// stream runs - the same rule mpegts::/matroska:: already follow internally.

struct AccessUnitTiming {
    // Samples from the start of the stream to the first sample this access
    // unit codes.
    std::uint64_t start_sample = 0;
    std::uint32_t duration_samples = 0;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] double start_seconds() const {
        return sample_rate == 0 ? 0.0
                                : static_cast<double>(start_sample) /
                                      static_cast<double>(sample_rate);
    }
    [[nodiscard]] double duration_seconds() const {
        return sample_rate == 0 ? 0.0
                                : static_cast<double>(duration_samples) /
                                      static_cast<double>(sample_rate);
    }
    // The same instant in an arbitrary clock - 90000 for MPEG-2 systems, 1000
    // for Matroska's default millisecond timecode scale, the track timescale
    // for ISOBMFF. Rounded down, from the absolute sample position, for the
    // no-drift reason in this section's own comment.
    [[nodiscard]] std::uint64_t start_in_timescale(std::uint32_t timescale) const {
        return sample_rate == 0 ? 0 : start_sample * timescale / sample_rate;
    }
    // The difference between this unit's start and the next one's, in the
    // same clock - NOT duration_samples converted on its own, which would
    // round independently and let a run of durations disagree with the
    // start times they are supposed to add up to.
    [[nodiscard]] std::uint64_t duration_in_timescale(std::uint32_t timescale) const {
        if (sample_rate == 0) {
            return 0;
        }
        const std::uint64_t end = (start_sample + duration_samples) * timescale / sample_rate;
        return end - start_in_timescale(timescale);
    }
};

// Access unit `index`, or nothing when there is no such unit.
[[nodiscard]] AC3FORGE_EXPORT std::optional<AccessUnitTiming> access_unit_timing(
    const ScannedStream& stream, std::size_t index);

// Total samples the stream codes, and the same figure in seconds.
[[nodiscard]] AC3FORGE_EXPORT std::uint64_t stream_duration_samples(const ScannedStream& stream);
[[nodiscard]] AC3FORGE_EXPORT double stream_duration_seconds(const ScannedStream& stream);

// The access unit covering `sample` - i.e. the one to cut at for a given
// position. Nothing when `sample` is past the end. A cut is only ever
// access-unit-aligned, so a caller asking for a time inside a unit gets that
// whole unit's index, never a split.
[[nodiscard]] AC3FORGE_EXPORT std::optional<std::size_t> access_unit_at_sample(
    const ScannedStream& stream, std::uint64_t sample);

// Same question in seconds, rounded to the nearest sample first.
[[nodiscard]] AC3FORGE_EXPORT std::optional<std::size_t> access_unit_at_seconds(
    const ScannedStream& stream, double seconds);

// The one length every access unit shares, or nothing when they differ. This
// is exactly the question a fixed-duration container track can answer and a
// variable one cannot: mp4::AudioTrack/mpegts::AudioTrack/matroska::AudioTrack
// each hold a single samples_per_frame, so a stream this returns nothing for
// cannot be described to them without per-sample durations they do not model.
[[nodiscard]] AC3FORGE_EXPORT std::optional<std::uint32_t> uniform_access_unit_samples(
    const ScannedStream& stream);

}  // namespace ac3::io
