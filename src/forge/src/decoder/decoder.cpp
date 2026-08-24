#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/internal/profiling.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "gain.hpp"

namespace ac3 {

// Each case says what is wrong with the stream, except kUnsupported, which
// says what is wrong with this decoder — the difference decides whether the
// caller should reach for another file or another tool.
std::string_view describe(DecodeError error) {
    switch (error) {
        case DecodeError::kTruncated: return "the stream ends part-way through a frame";
        case DecodeError::kBadSyncWord:
            return "no 0x0B77 sync word where a frame should begin";
        case DecodeError::kBadCrc: return "the frame's CRC does not check out";
        case DecodeError::kReservedValue: return "a header field holds a value A/52 reserves";
        case DecodeError::kUnsupported:
            return "valid AC-3 this decoder does not implement (bsid > 8)";
        case DecodeError::kInvalidStream:
            return "the frame contradicts a constraint A/52 requires of it";
    }
    return "unknown decode error";
}

namespace {

// Byte length of the syncframe at `offset`, whichever generation it is.
// AC-3 and E-AC-3 both put bsid at bit 40 - deliberately, so that a reader
// can tell the two apart before committing to a layout - but they express the
// size completely differently: AC-3 looks frmsizecod up in Table 5.18, while
// E-AC-3 states the word count outright in the 11-bit frmsiz.
std::expected<std::size_t, DecodeError> syncframe_bytes(std::span<const std::byte> stream,
                                                        std::size_t offset) {
    if (offset + 6 > stream.size()) {
        return std::unexpected(DecodeError::kTruncated);
    }
    const auto byte = [&](std::size_t i) {
        return std::to_integer<std::uint32_t>(stream[offset + i]);
    };
    if (byte(0) != 0x0B || byte(1) != 0x77) {
        return std::unexpected(DecodeError::kBadSyncWord);
    }
    const auto bsid = byte(5) >> 3;
    if (bsid >= eac3::kMinDecodableBsid && bsid <= eac3::kBsid) {
        const std::size_t frmsiz = ((byte(2) & 0x07) << 8) | byte(3);
        const std::size_t bytes = (frmsiz + 1) * 2;
        // frmsiz is free to say anything, including a size that does not even
        // cover the six header bytes just read. Callers index into the spans
        // this hands back, so a self-contradictory size is rejected here
        // rather than becoming a short span someone reads past.
        if (bytes < 6) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        return bytes;
    }
    if (bsid > 8) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    const auto fscod = byte(4) >> 6;
    const auto frmsizecod = byte(4) & 0x3F;
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    const std::uint32_t kbps = kBitratesKbps[frmsizecod >> 1];
    // kbps came from the very table frame_size_bytes -> bitrate_index
    // searches for an exact match, so the lookup inside it always succeeds.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    return *frame_size_bytes(static_cast<SampleRate>(fscod), kbps, (frmsizecod & 1) != 0);
}

}  // namespace

std::expected<int, DecodeError> stream_bsid(std::span<const std::byte> frame) {
    if (frame.size() < 6) {
        return std::unexpected(DecodeError::kTruncated);
    }
    return static_cast<int>(std::to_integer<std::uint32_t>(frame[5]) >> 3);
}

std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_frames(
    std::span<const std::byte> stream) {
    std::vector<std::span<const std::byte>> frames;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const auto bytes = syncframe_bytes(stream, offset);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (offset + *bytes > stream.size()) {
            return std::unexpected(DecodeError::kTruncated);
        }
        frames.push_back(stream.subspan(offset, *bytes));
        offset += *bytes;
    }
    return frames;
}

namespace {

// strmtyp and substreamid out of byte 2 of an E-AC-3 syncframe: strmtyp(2) |
// substreamid(3) | frmsiz's top three bits. Read straight off the byte rather
// than through parse_bsi, which is a whole frame's worth of work for the two
// fields the framing layer needs.
//
// Gated on the frame's OWN bsid, because byte 2 only holds those fields in a
// genuine Annex E frame - in an AC-3 one it is part of crc1, which is
// effectively random per frame and aliases to "dependent" about a quarter of
// the time. A stream may legally mix the two: real commercial discs author a
// bsid-6 independent substream with a bsid-16 Atmos-carrying dependent right
// behind it, and reading crc1 as strmtyp there swallows whole runs of access
// units into one group (see apps/android/.../file_replay.cpp, which measured
// exactly that and worked around it locally). bsid does not move - it sits at
// bit 40 in both generations, deliberately - so gating on it is the reading
// that is correct for either.
struct Identity {
    eac3::StreamType strmtyp;
    int substreamid;
};

[[nodiscard]] Identity frame_identity(std::span<const std::byte> frame) {
    const auto bsid = std::to_integer<std::uint32_t>(frame[5]) >> 3;
    if (bsid < eac3::kMinDecodableBsid || bsid > eac3::kBsid) {
        // An AC-3 syncframe is a whole programme's whole access unit on its
        // own: no substream layer, so nothing to be dependent on and no
        // second programme to number away from.
        return {eac3::StreamType::kIndependent, 0};
    }
    const auto byte = std::to_integer<std::uint32_t>(frame[2]);
    return {static_cast<eac3::StreamType>(byte >> 6), static_cast<int>((byte >> 3) & 0x07)};
}

// §E1.3.1: strmtyp 2 is an independent substream too - one whose programme was
// previously coded as AC-3 - so it opens an access unit exactly as strmtyp 0
// does.
[[nodiscard]] bool begins_unit(eac3::StreamType strmtyp) {
    return strmtyp == eac3::StreamType::kIndependent ||
           strmtyp == eac3::StreamType::kConvertible;
}

}  // namespace

bool has_eac3_extension_substreams(std::span<const std::byte> stream) {
    const auto core_bsid = stream_bsid(stream);
    if (!core_bsid || *core_bsid > 8) {
        return false;  // not AC-3-led, so not this arrangement
    }
    const auto core_bytes = syncframe_bytes(stream, 0);
    if (!core_bytes || *core_bytes >= stream.size()) {
        return false;  // unreadable, or the core is the whole stream
    }
    const auto next = stream.subspan(*core_bytes);
    // syncframe_bytes rather than stream_bsid alone: it checks the sync word
    // and the declared size, so a second "frame" that is really trailing
    // rubbish does not get read as a dependent on the strength of five bits.
    const auto next_bytes = syncframe_bytes(next, 0);
    const auto next_bsid = stream_bsid(next);
    if (!next_bytes || !next_bsid || *next_bsid <= 8) {
        return false;  // another AC-3 syncframe: plain AC-3
    }
    return static_cast<eac3::StreamType>(std::to_integer<std::uint32_t>(next[2]) >> 6) ==
           eac3::StreamType::kDependent;
}

std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_access_units(
    std::span<const std::byte> stream) {
    const auto frames = split_frames(stream);
    if (!frames) {
        return std::unexpected(frames.error());
    }
    // An access unit is its substreams concatenated, so it is delimited rather
    // than framed: a new one starts wherever an independent substream does,
    // and everything up to the next one belongs to it.
    std::vector<std::span<const std::byte>> units;
    std::size_t start = 0;
    std::size_t offset = 0;
    for (const auto& frame : *frames) {
        if (begins_unit(frame_identity(frame).strmtyp) && offset != start) {
            units.push_back(stream.subspan(start, offset - start));
            start = offset;
        }
        offset += frame.size();
    }
    if (offset != start) {
        units.push_back(stream.subspan(start, offset - start));
    }
    // A stream whose very first syncframe is a dependent has lost its parent;
    // its channels have nothing to extend.
    if (!units.empty() &&
        frame_identity(units.front()).strmtyp == eac3::StreamType::kDependent) {
        return std::unexpected(DecodeError::kInvalidStream);
    }
    return units;
}

FrameDecoder::FrameDecoder(const DecoderConfig& config)
    : config_(internal::resolve_operating_mode(config)), output_(config.output) {}

std::expected<std::vector<std::span<const std::byte>>, DecodeError> split_access_units(
    std::span<const std::byte> stream, int programme) {
    auto units = split_access_units(stream);
    if (!units) {
        return std::unexpected(units.error());
    }
    // Each unit begins with its programme's own independent substream, so the
    // selection is one field read per unit - no second walk of the frames.
    // A dependent's substreamid numbers in its parent's space (§E2.3.1.2) and
    // is never consulted here.
    std::erase_if(*units, [programme](std::span<const std::byte> unit) {
        return frame_identity(unit).substreamid != programme;
    });
    return units;
}

std::expected<std::vector<int>, DecodeError> programme_ids(std::span<const std::byte> stream) {
    const auto units = split_access_units(stream);
    if (!units) {
        return std::unexpected(units.error());
    }
    std::vector<int> ids;
    for (const auto& unit : *units) {
        const int id = frame_identity(unit).substreamid;
        // Ascending and unique: the stream repeats its programmes once per
        // frame period, so the same handful of ids arrives over and over. At
        // most eight of them (§E2.3.1.2), which is why this is a sorted
        // insert rather than a set.
        if (const auto at = std::ranges::lower_bound(ids, id);
            at == ids.end() || *at != id) {
            ids.insert(at, id);
        }
    }
    // A stream with no access units at all reports no programmes rather than
    // an empty-but-present one; split_frames already rejected anything that
    // was not framing.
    return ids;
}

std::expected<DecodedFrame, DecodeError> FrameDecoder::decode_frame(
    std::span<const std::byte> frame) {
    auto decoded = decode_frame_core(frame, {});
    if (decoded) {
        return decoded;
    }
    if (auto concealed = conceal(decoded.error(), {})) {
        return std::move(*concealed);
    }
    return decoded;
}

std::expected<DecodedFrame, DecodeError> FrameDecoder::decode_frame_into(
    std::span<const std::byte> frame, std::span<const std::span<float>> channels) {
    auto decoded = decode_frame_core(frame, channels);
    if (decoded) {
        return decoded;
    }
    if (auto concealed = conceal(decoded.error(), channels)) {
        return std::move(*concealed);
    }
    return decoded;
}

std::optional<DecodedFrame> FrameDecoder::conceal(DecodeError error,
                                                  std::span<const std::span<float>> external) {
    // Nothing retained means this is the head of the stream: there is no
    // previous block to reconstruct from, and inventing one would be
    // substituting audio rather than concealing a gap in it.
    if (config_.concealment == ConcealmentPolicy::kNone || !retained_) {
        return std::nullopt;
    }
    const bool repeat = config_.concealment == ConcealmentPolicy::kRepeatFade;
    const int nchans = retained_->nchans;

    DecodedFrame out = retained_->shape;
    // No word was transmitted, so none is reported - the persistence rule in
    // §7.7.1.2 is about blocks within a syncframe, not about a syncframe that
    // never arrived.
    out.dynrng.fill(meta::kDynrngUnity);
    out.dynrng2.fill(meta::kDynrngUnity);
    out.concealed = Concealment{.error = error,
                                .action = repeat ? ConcealmentAction::kRepeatFade
                                                 : ConcealmentAction::kMute};

    std::array<std::span<float>, 6> pcm_target{};
    if (external.empty()) {
        out.channels.assign(static_cast<std::size_t>(nchans),
                            std::vector<float>(kSamplesPerFrame, 0.0f));
        for (int ch = 0; ch < nchans; ++ch) {
            pcm_target[static_cast<std::size_t>(ch)] = out.channels[static_cast<std::size_t>(ch)];
        }
    } else {
        assert(static_cast<int>(external.size()) >= nchans);
        for (int ch = 0; ch < nchans; ++ch) {
            pcm_target[static_cast<std::size_t>(ch)] = external[static_cast<std::size_t>(ch)];
        }
    }

    // The concealed frame goes through the SAME overlap-add the real ones do,
    // which is what makes it join up at both ends. Under kRepeatFade each
    // block's transform output is the last good block's, decayed; under kMute
    // it is zero, and the first block still carries the previous frame's own
    // window tail out of delay_ - so silence arrives as the codec's own
    // fade rather than as a cut.
    //
    // 20 dB across the frame: enough that a lost frame audibly steps back
    // instead of ringing on, and gentle enough that one lost frame in an
    // otherwise clean stream is not itself the artefact. A run of losses
    // keeps decaying, because the retained block is scaled by where this
    // frame's decay finished.
    constexpr double kDecayPerBlock = 0.6812920690579611;  // 10^(-20/(20*6))
    double gain = kDecayPerBlock;
    for (int block = 0; block < kBlocksPerFrame; ++block) {
        for (int ch = 0; ch < nchans; ++ch) {
            const auto& last = retained_->last_block[static_cast<std::size_t>(ch)];
            auto& delay = delay_[static_cast<std::size_t>(ch)];
            const auto pcm = pcm_target[static_cast<std::size_t>(ch)];
            for (int n = 0; n < 256; ++n) {
                const auto un = static_cast<std::size_t>(n);
                const double head = repeat ? last[un] * gain : 0.0;
                pcm[static_cast<std::size_t>(block * 256 + n)] =
                    static_cast<float>(2.0 * (head + delay[un]));
                delay[un] = repeat ? last[un + 256] * gain : 0.0;
            }
        }
        gain *= kDecayPerBlock;
    }
    if (repeat) {
        // Where the next consecutive loss picks the decay up from. Without
        // this a long dropout would repeat the same block at the same level
        // forever, which is the one concealment artefact worse than the gap.
        const double carried = gain / kDecayPerBlock;
        for (int ch = 0; ch < nchans; ++ch) {
            for (double& value : retained_->last_block[static_cast<std::size_t>(ch)]) {
                value *= carried;
            }
        }
    } else {
        // kMute has already driven delay_ to zero; clearing the retained
        // block keeps a later switch of policy from resurrecting audio from
        // before the dropout.
        for (int ch = 0; ch < nchans; ++ch) {
            retained_->last_block[static_cast<std::size_t>(ch)].fill(0.0);
        }
    }

    const auto levels = mix_levels(out.cmixlev, out.surmixlev);
    if (external.empty()) {
        output_.apply(out.channels, out.acmod, out.lfe, levels, out.dialnorm);
    } else {
        output_.apply(external.first(static_cast<std::size_t>(nchans)), out.acmod, out.lfe,
                      levels, out.dialnorm);
    }
    return out;
}

std::expected<DecodedFrame, DecodeError> FrameDecoder::decode_frame_core(
    std::span<const std::byte> frame, std::span<const std::span<float>> external) {
    AC3_ZONE_SCOPED_N("ac3_decode_frame");
    // Before the first early return, for the same reason FrameEncoder resets
    // its own: a caller reusing one trace across a file must never read a
    // previous frame's state out of a call that decoded nothing.
    if (config_.trace != nullptr) {
        config_.trace->reset();
    }
    if (config_.syntax != nullptr) {
        config_.syntax->reset();
    }
    if (frame.size() < 6) {
        return std::unexpected(DecodeError::kTruncated);
    }
    // bsid sits at bit 40 in both generations precisely so it can be read
    // before anything else is interpreted; every field below means something
    // different in an Annex E frame, so check it first rather than letting the
    // AC-3 reading of frmsizecod fail in some incidental way.
    if (*stream_bsid(frame) > 8) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    BitReader r{frame};
    if (r.read(16) != kSyncWord) {
        return std::unexpected(DecodeError::kBadSyncWord);
    }
    (void)r.read(16);  // crc1 (validated by register property below)
    const auto fscod = r.read(2);
    const auto frmsizecod = r.read(6);
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    const auto sample_rate = static_cast<SampleRate>(fscod);
    const std::uint32_t kbps = kBitratesKbps[frmsizecod >> 1];
    const auto expected_bytes = frame_size_bytes(sample_rate, kbps, (frmsizecod & 1) != 0);
    if (!expected_bytes || frame.size() != *expected_bytes) {
        return std::unexpected(DecodeError::kTruncated);
    }
    const std::uint32_t words = *expected_bytes / 2;
    const std::uint32_t words58 = frame_size_58_words(words);
    if (crc16(frame.subspan(2, 2 * words58 - 2)) != 0 || crc16(frame.subspan(2)) != 0) {
        return std::unexpected(DecodeError::kBadCrc);
    }

    // --- bsi ---
    const auto bsid = r.read(5);
    if (bsid > 8) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    meta::BsiInfo info;
    const auto bsmod = r.read(3);  // §5.4.2.1, also reported raw on DecodedFrame
    info.bsmod = static_cast<meta::BitstreamMode>(bsmod);
    const auto acmod_value = r.read(3);
    const auto acmod = static_cast<Acmod>(acmod_value);
    // §5.4.2.4/§5.4.2.5. Both fields are conditional on acmod, so "absent"
    // and "present and says the default" stay distinguishable on the result -
    // which is what lets ac3::mix_levels() apply §7.8's own fallbacks rather
    // than inventing values here. Code '11' is reserved for cmixlev and
    // §5.4.2.4 says to read it as the intermediate -4.5 dB, which is what
    // leaving it std::nullopt already produces.
    std::optional<meta::CentreMixLevel> cmixlev;
    std::optional<meta::SurroundMixLevel> surmixlev;
    if ((acmod_value & 0x1) != 0 && acmod != Acmod::k1_0) {
        const auto code = r.read(2);
        if (code <= static_cast<std::uint32_t>(meta::CentreMixLevel::kMinus6dB)) {
            cmixlev = static_cast<meta::CentreMixLevel>(code);
        }
    }
    if ((acmod_value & 0x4) != 0) {
        const auto code = r.read(2);
        // §5.4.2.5 leaves '11' reserved with no stated substitution; treating
        // it as "not indicated" gets §7.8's own -6 dB default, which is the
        // only reading that does not put a made-up level on the wire's behalf.
        if (code <= static_cast<std::uint32_t>(meta::SurroundMixLevel::kSilent)) {
            surmixlev = static_cast<meta::SurroundMixLevel>(code);
        }
    }
    if (acmod == Acmod::k2_0) {
        const auto code = r.read(2);  // dsurmod
        if (code < 3) {
            info.dsurmod = static_cast<meta::SurroundMode>(code);
        }
    }
    const bool lfe = r.read(1) != 0;
    const auto dialnorm = static_cast<int>(r.read(5));
    std::optional<std::uint8_t> compr;
    if (r.read(1) != 0) {  // compre (§5.4.2.9)
        compr = static_cast<std::uint8_t>(r.read(8));
    }
    // §5.4.2.12: langcod is a reserved 0xFF wherever it appears, so the byte
    // itself carries nothing and only its presence is worth reporting.
    const auto read_langcod = [&r] {
        const bool present = r.read(1) != 0;
        if (present) {
            r.skip(8);
        }
        return present;
    };
    const auto read_audprod = [&r]() -> std::optional<meta::AudioProduction> {
        if (r.read(1) == 0) {  // audprodie
            return std::nullopt;
        }
        meta::AudioProduction production;
        production.mixlevel = static_cast<int>(r.read(5));
        const auto room = r.read(2);
        if (room < 3) {  // Table 5.12's '11' reads as "not indicated"
            production.roomtyp = static_cast<meta::RoomType>(room);
        }
        // adconvtyp is not part of AC-3's audprodie - see AudioProduction's
        // own comment - so it keeps its default here and only Annex D's xbsi2
        // below can set it.
        return production;
    };
    info.langcod = read_langcod();
    info.audprod = read_audprod();
    std::optional<int> dialnorm2;
    std::optional<std::uint8_t> compr2;
    if (acmod == Acmod::kDualMono) {
        dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            compr2 = static_cast<std::uint8_t>(r.read(8));
        }
        info.langcod2 = read_langcod();
        info.audprod2 = read_audprod();
    }
    info.copyrightb = r.read(1) != 0;
    info.origbs = r.read(1) != 0;
    std::optional<meta::AlternateBsi> alternate_bsi;
    if (bsid == 6) {
        // §D2.2: bsid 6 spends the two 14-bit timecod fields on xbsi1 and
        // xbsi2 instead. §D3.2 is explicit that a legacy decoder reading them
        // as a time code it ignores is harmless - the fields are the same
        // size - which is exactly why this branch can sit here and change
        // nothing about where addbsie lands.
        meta::AlternateBsi alternate;
        if (r.read(1) != 0) {  // xbsi1e
            meta::MixMetadata mix;
            const auto mode = r.read(2);
            if (mode < 3) {  // Table D2.2's '11' reads as "not indicated"
                mix.dmixmod = static_cast<meta::DownmixMode>(mode);
            }
            // Table D2.1's order: both Lt/Rt levels, then both Lo/Ro ones.
            mix.ltrtcmixlev = static_cast<meta::MixLevel>(r.read(3));
            mix.ltrtsurmixlev = static_cast<meta::MixLevel>(r.read(3));
            mix.lorocmixlev = static_cast<meta::MixLevel>(r.read(3));
            mix.lorosurmixlev = static_cast<meta::MixLevel>(r.read(3));
            // Annex D has no LFE mix level at all; std::nullopt is already
            // MixMetadata's own "LFE mixing disabled", which is the right
            // reading of a syntax that cannot express one.
            alternate.mix = mix;
        }
        if (r.read(1) != 0) {  // xbsi2e
            meta::ExtendedBsi extended;
            extended.dsurexmod = static_cast<meta::SurroundExMode>(r.read(2));
            const auto headphone = r.read(2);
            if (headphone < 3) {  // Table D2.8's '11' reads as "not indicated"
                extended.dheadphonmod = static_cast<meta::HeadphoneMode>(headphone);
            }
            extended.adconvtyp = static_cast<meta::AdConverterType>(r.read(1));
            extended.xbsi2 = static_cast<std::uint8_t>(r.read(8));
            extended.encinfo = r.read(1) != 0;
            alternate.extended = extended;
        }
        alternate_bsi = alternate;
    } else {
        if (r.read(1) != 0) {  // timecod1e
            meta::TimeCodeCoarse coarse;
            coarse.hours = static_cast<int>(r.read(5));
            coarse.minutes = static_cast<int>(r.read(6));
            coarse.eight_seconds = static_cast<int>(r.read(3));
            info.timecod1 = coarse;
        }
        if (r.read(1) != 0) {  // timecod2e
            meta::TimeCodeFine fine;
            fine.seconds = static_cast<int>(r.read(3));
            fine.frames = static_cast<int>(r.read(5));
            fine.sixty_fourths = static_cast<int>(r.read(6));
            info.timecod2 = fine;
        }
    }
    if (r.read(1) != 0) {  // addbsie
        const auto addbsil = r.read(6);
        r.skip((addbsil + 1) * 8);
    }

    const int nfchans = fullbw_channel_count(acmod);
    const int nchans = nfchans + (lfe ? 1 : 0);

    DecodedFrame out;
    out.sample_rate = sample_rate;
    out.bitrate_kbps = kbps;
    out.bsid = static_cast<int>(bsid);
    out.bsmod = static_cast<int>(bsmod);
    out.acmod = acmod;
    out.lfe = lfe;
    out.cmixlev = cmixlev;
    out.surmixlev = surmixlev;
    out.info = info;
    out.alternate_bsi = alternate_bsi;
    out.dialnorm = dialnorm;
    out.cmixlev = cmixlev;
    out.surmixlev = surmixlev;
    out.compr = compr;
    out.dynrng.fill(meta::kDynrngUnity);
    out.dialnorm2 = dialnorm2;
    out.compr2 = compr2;
    out.dynrng2.fill(meta::kDynrngUnity);
    out.blksw.assign(static_cast<std::size_t>(nfchans), {});
    // The PCM target: the caller's spans when decode_frame_into supplied
    // them, otherwise vectors allocated into the result exactly as before.
    // Every sample of every coded channel is written below (six blocks of
    // 256 each), so external storage needs no pre-clearing.
    // config_.skip_reconstruction leaves both forms untouched: nothing below
    // writes PCM at all, so the value form allocates none (its `channels`
    // comes back empty, as that option's own comment promises) and the
    // decode_frame_into form simply never writes through the caller's spans.
    std::array<std::span<float>, 6> pcm_target{};
    if (config_.skip_reconstruction) {
        // Nothing to point at.
    } else if (external.empty()) {
        out.channels.assign(static_cast<std::size_t>(nchans),
                            std::vector<float>(kSamplesPerFrame, 0.0f));
        for (int ch = 0; ch < nchans; ++ch) {
            pcm_target[static_cast<std::size_t>(ch)] = out.channels[static_cast<std::size_t>(ch)];
        }
    } else {
        assert(static_cast<int>(external.size()) >= nchans);
        for (int ch = 0; ch < nchans; ++ch) {
            assert(external[static_cast<std::size_t>(ch)].size() >=
                   static_cast<std::size_t>(kSamplesPerFrame));
            pcm_target[static_cast<std::size_t>(ch)] = external[static_cast<std::size_t>(ch)];
        }
    }

    // §7.10: whether the block loop below has to keep its last block for a
    // future loss to be reconstructed from. Sized here rather than in the
    // constructor because nchans is not known until now, and skipped
    // entirely with concealment off - which is what keeps a decoder
    // configured the way every existing caller configures it from carrying
    // 24 KB it will never read.
    const bool retain_last_block = config_.concealment != ConcealmentPolicy::kNone;
    if (retain_last_block) {
        conceal_scratch_.assign(static_cast<std::size_t>(nchans), std::array<double, 512>{});
    }

    // §7.7.1.2: an absent word inherits from the previous BLOCK, and block 0
    // without one is unity — never the previous frame's value, which is what
    // lets a decoder join a stream mid-programme at the right level.
    std::uint8_t dynrng_word = meta::kDynrngUnity;
    std::uint8_t dynrng2_word = meta::kDynrngUnity;

    // Decode state persisting across blocks. Streams are the fbw channels,
    // then the LFE, then - when coupling is in use - the coupling channel as
    // one more stream, exactly as the encoder lays them out.
    const std::size_t max_streams = static_cast<std::size_t>(nchans) + 1;
    std::vector<int> endmant(max_streams, kLfeEndmant);
    std::vector<std::vector<std::uint8_t>> exps(max_streams);
    BitAllocCodes base_codes{};
    std::vector<int> fgaincod(max_streams, base_codes.fgaincod);
    int csnroffst = 0;
    std::vector<int> fsnroffst(max_streams, 0);
    // §7.2.2.6: per-stream delta bit allocation, reset to "no segments" at the
    // start of every syncframe (the spec's own recommended initialization),
    // then persisting block to block exactly like base_codes/fsnroffst above
    // until re-transmitted or explicitly cleared.
    std::vector<DeltaSegments> delta(max_streams);
    std::array<bool, 4> rematflg{};

    // Coupling state (§7.4). All of it persists until re-transmitted.
    const int cpl_stream = nchans;
    bool cplinu = false;
    bool phsflginu = false;
    int cplbegf = 0;
    int cplstrtmant = 0;
    int ncplsubnd = 0;
    int cplfleak = 0;
    int cplsleak = 0;
    std::vector<bool> chincpl(static_cast<std::size_t>(nfchans), false);
    // Which coupling band each sub-band belongs to (cplbndstrc expansion).
    std::vector<int> subband_band;
    int ncplbnd = 0;
    // [channel][sub-band] - already expanded from bands to sub-bands.
    std::vector<std::vector<double>> cplco(static_cast<std::size_t>(nfchans));
    std::vector<bool> phsflg;

    // The self-check's decoder-side view (ac3/verify/mirror.hpp). nfchans and
    // nchans are frame-wide, so the shape is settled here; the per-block half
    // is filled twice below - the block-boundary bit position on entry, the
    // per-stream state once the allocation for the block exists - so that a
    // frame this decoder ends up REFUSING still leaves behind the offsets it
    // reached, which is what localises a desync to a block.
    if (config_.trace != nullptr) {
        config_.trace->fbw_channels = nfchans;
        config_.trace->coded_channels = nchans;
    }
    // The syntax trace's frame-wide shape (ac3/decoder/syntax_trace.hpp).
    // Every Annex E frame gate it carries stays false here: AC-3 has no
    // audfrm section at all, so blkswe/dithflage/bamode and the rest simply
    // do not exist, and every one of their per-block fields is unconditional
    // instead. per_block_exp_strategy is true for the same reason - Table
    // E2.10's hoisted frame codes are an Annex E addition.
    if (config_.syntax != nullptr) {
        config_.syntax->valid = true;
        config_.syntax->fbw_channels = nfchans;
        config_.syntax->lfe = lfe;
        config_.syntax->block_count = kBlocksPerFrame;
    }

    // Per-block scratch, declared once ahead of the block loop rather than
    // freshly inside it: each is fully re-assigned or overwritten before it
    // is read within any block (the .assign() calls below keep the exact
    // clearing semantics the in-loop declarations had), so hoisting changes
    // nothing observable - it only stops the loop from re-allocating some
    // twenty buffers per block. `x` additionally carries no zero-init at
    // all: both imdct512_windowed and imdct256_pair_windowed write every
    // element of the 512-wide output (mdct.cpp's step 5 covers x[0..511] in
    // eight strided sequences), so a cleared buffer was never load-bearing.
    std::vector<double> band_values;
    std::vector<ExpStrategy> strategy;
    std::vector<std::uint8_t> groups;
    std::vector<std::vector<std::uint8_t>> bap(max_streams);
    std::vector<std::array<double, 256>> coeffs(max_streams);
    std::array<double, 512> x;

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        AC3_ZONE_SCOPED_N("ac3_decode_block");
        if (config_.trace != nullptr) {
            auto& trace = config_.trace->blocks[static_cast<std::size_t>(block)];
            trace.entered = true;
            trace.bit_offset = r.bit_position();
        }
        BlockSyntax* syntax =
            config_.syntax != nullptr ? &config_.syntax->blocks[static_cast<std::size_t>(block)]
                                      : nullptr;
        if (syntax != nullptr) {
            syntax->entered = true;
        }
        std::array<bool, 5> blksw{};  // AC-3's widest acmod (3/2) has 5 fbw channels
        for (int ch = 0; ch < nfchans; ++ch) {
            blksw[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            out.blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(block)] =
                blksw[static_cast<std::size_t>(ch)];
            if (syntax != nullptr && blksw[static_cast<std::size_t>(ch)]) {
                syntax->block_switch |= static_cast<std::uint8_t>(1U << ch);
            }
        }
        // §5.4.3.2/§7.3.4: per-channel, read fresh every block (unlike
        // E-AC-3's frame-gated dithflage). Reconstruction is done in
        // read_stream/the decoupling loop below; a coupled channel's own
        // bit still gates dither for its shared high band, applied there
        // after extraction rather than here.
        std::array<bool, 5> dithflag{};
        for (int ch = 0; ch < nfchans; ++ch) {
            dithflag[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            if (syntax != nullptr && dithflag[static_cast<std::size_t>(ch)]) {
                syntax->dither |= static_cast<std::uint8_t>(1U << ch);
            }
        }
        if (r.read(1) != 0) {  // dynrnge
            dynrng_word = static_cast<std::uint8_t>(r.read(8));
        }
        out.dynrng[static_cast<std::size_t>(block)] = dynrng_word;
        if (acmod == Acmod::kDualMono) {
            if (r.read(1) != 0) {  // dynrng2e
                dynrng2_word = static_cast<std::uint8_t>(r.read(8));
            }
            out.dynrng2[static_cast<std::size_t>(block)] = dynrng2_word;
        }

        // --- coupling strategy (§5.3.3) ---
        if (r.read(1) != 0) {  // cplstre: a new strategy, else the prior one stands
            cplinu = r.read(1) != 0;
            if (cplinu) {
                for (int ch = 0; ch < nfchans; ++ch) {
                    chincpl[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                }
                phsflginu = acmod == Acmod::k2_0 && r.read(1) != 0;
                cplbegf = static_cast<int>(r.read(4));
                const int cplendf = static_cast<int>(r.read(4));
                ncplsubnd = coupling::sub_band_count(cplbegf, cplendf);
                if (ncplsubnd < 1) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                cplstrtmant = coupling::start_mant(cplbegf);
                const int cplendmant = coupling::end_mant(cplendf);
                if (cplendmant > 253 || cplstrtmant >= cplendmant) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                endmant[static_cast<std::size_t>(cpl_stream)] = cplendmant;

                // cplbndstrc: a 1 folds this sub-band into the previous
                // coupling band, so coordinates are per band and duplicated
                // back out across the sub-bands they cover.
                subband_band.assign(static_cast<std::size_t>(ncplsubnd), 0);
                ncplbnd = 1;
                for (int bnd = 1; bnd < ncplsubnd; ++bnd) {
                    const bool merged = r.read(1) != 0;
                    if (!merged) {
                        ++ncplbnd;
                    }
                    subband_band[static_cast<std::size_t>(bnd)] = ncplbnd - 1;
                }
                // Coordinates survive a re-sent strategy: cplcoe == 0 in this
                // very block legally means "reuse the previous coordinates"
                // (§5.4.3.14), so clearing them here would silence the
                // coupled high band. Only a change in geometry forces a
                // resize, and then only the new entries start at zero.
                for (auto& channel : cplco) {
                    channel.resize(static_cast<std::size_t>(ncplsubnd), 0.0);
                }
                phsflg.resize(static_cast<std::size_t>(ncplbnd), false);
                // Coupled channels stop carrying their own coefficients here.
                for (int ch = 0; ch < nfchans; ++ch) {
                    if (chincpl[static_cast<std::size_t>(ch)]) {
                        endmant[static_cast<std::size_t>(ch)] = cplstrtmant;
                    }
                }
            }
        }

        // --- coupling coordinates (§7.4.3) ---
        if (cplinu) {
            bool any_new = false;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                if (r.read(1) == 0) {  // cplcoe: reuse the previous values
                    continue;
                }
                any_new = true;
                const int master = static_cast<int>(r.read(2));
                band_values.assign(static_cast<std::size_t>(ncplbnd), 0.0);
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    const auto exp = static_cast<std::uint8_t>(r.read(4));
                    const auto mant = static_cast<std::uint8_t>(r.read(4));
                    band_values[static_cast<std::size_t>(bnd)] =
                        coupling::decode_coordinate({.exp = exp, .mant = mant}, master);
                }
                for (int bnd = 0; bnd < ncplsubnd; ++bnd) {
                    cplco[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bnd)] =
                        band_values[static_cast<std::size_t>(
                            subband_band[static_cast<std::size_t>(bnd)])];
                }
            }
            if (phsflginu && any_new) {
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    phsflg[static_cast<std::size_t>(bnd)] = r.read(1) != 0;
                }
            }
        }

        if (acmod == Acmod::k2_0) {
            if (r.read(1) != 0) {  // rematstr: new flags; else prior flags persist
                // §7.5.2: coupling limits how many rematrix bands exist.
                const int nrematbd = !cplinu ? 4 : (cplbegf > 2 ? 4 : (cplbegf > 0 ? 3 : 2));
                rematflg.fill(false);
                for (int band = 0; band < nrematbd; ++band) {
                    rematflg[static_cast<std::size_t>(band)] = r.read(1) != 0;
                }
            }
        }

        if (syntax != nullptr) {
            syntax->coupling = cplinu;
            syntax->rematrixing =
                acmod == Acmod::k2_0 && std::ranges::any_of(rematflg, [](bool on) { return on; });
        }

        // §5.3.3 exponent strategies: coupling channel first, then fbw, then LFE.
        strategy.assign(max_streams, ExpStrategy::kReuse);
        if (cplinu) {
            strategy[static_cast<std::size_t>(cpl_stream)] =
                static_cast<ExpStrategy>(r.read(2));
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            strategy[static_cast<std::size_t>(ch)] = static_cast<ExpStrategy>(r.read(2));
            if (block == 0 && strategy[static_cast<std::size_t>(ch)] == ExpStrategy::kReuse) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
        }
        if (lfe) {
            strategy[static_cast<std::size_t>(nfchans)] =
                r.read(1) != 0 ? ExpStrategy::kD15 : ExpStrategy::kReuse;
            if (block == 0 && strategy[static_cast<std::size_t>(nfchans)] == ExpStrategy::kReuse) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
        }
        if (syntax != nullptr) {
            for (int ch = 0; ch < nfchans; ++ch) {
                syntax->exp_strategy[static_cast<std::size_t>(ch)] =
                    strategy[static_cast<std::size_t>(ch)];
            }
            if (lfe) {
                syntax->exp_strategy[static_cast<std::size_t>(nfchans)] =
                    strategy[static_cast<std::size_t>(nfchans)];
            }
            if (cplinu) {
                syntax->exp_strategy[kCouplingSyntaxStream] =
                    strategy[static_cast<std::size_t>(cpl_stream)];
            }
        }
        // chbwcod exists only for fbw channels that are NOT coupled.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (strategy[static_cast<std::size_t>(ch)] != ExpStrategy::kReuse &&
                !(cplinu && chincpl[static_cast<std::size_t>(ch)])) {
                const auto chbwcod = r.read(6);
                if (chbwcod > 60) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                endmant[static_cast<std::size_t>(ch)] =
                    ((static_cast<int>(chbwcod) + 12) * 3) + 37;
            }
        }

        // Exponents, in the same order. The coupling channel's set is offset
        // to its own start bin and uses the even-valued absolute reference.
        if (cplinu && strategy[static_cast<std::size_t>(cpl_stream)] != ExpStrategy::kReuse) {
            const auto strat = strategy[static_cast<std::size_t>(cpl_stream)];
            const int end = endmant[static_cast<std::size_t>(cpl_stream)];
            const int span = end - cplstrtmant;
            const int group_size = exponent_group_size(strat);
            if (group_size == 0 || span % (3 * group_size) != 0) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            const int ngrps = span / (3 * group_size);
            const auto cplabsexp = static_cast<std::uint8_t>(r.read(4));
            groups.assign(static_cast<std::size_t>(ngrps), 0);
            for (auto& g : groups) {
                g = static_cast<std::uint8_t>(r.read(7));
                // §7.10.2 error condition 17: a grouped value above 124 is
                // not a legal triple of mapped values.
                if (g > 124) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            auto& target = exps[static_cast<std::size_t>(cpl_stream)];
            target.assign(static_cast<std::size_t>(end), kMaxExponent);
            decode_coupling_exponents(
                cplabsexp, groups, strat,
                std::span{target}.subspan(static_cast<std::size_t>(cplstrtmant)));
            // §7.2.2.2: exponents are 0..24. A malformed differential chain
            // can walk outside that, and the reconstruction shifts by the
            // exponent - undefined behaviour, not merely wrong audio.
            for (std::size_t bin = static_cast<std::size_t>(cplstrtmant); bin < target.size();
                 ++bin) {
                if (target[bin] > kMaxExponent) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
        }
        {
            AC3_ZONE_SCOPED_N("ac3_exponents");
            for (int ch = 0; ch < nchans; ++ch) {
                const auto strat = strategy[static_cast<std::size_t>(ch)];
                if (strat == ExpStrategy::kReuse) {
                    continue;
                }
                const int end = endmant[static_cast<std::size_t>(ch)];
                const int ngrps = ch < nfchans ? exponent_group_count(strat, end) : 2;
                const auto absolute = static_cast<std::uint8_t>(r.read(4));
                groups.assign(static_cast<std::size_t>(ngrps), 0);
                for (auto& g : groups) {
                    g = static_cast<std::uint8_t>(r.read(7));
                    if (g > 124) {  // §7.10.2 error condition 17
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                exps[static_cast<std::size_t>(ch)].assign(static_cast<std::size_t>(end), 0);
                decode_exponents(absolute, groups, strat, exps[static_cast<std::size_t>(ch)]);
                // §7.2.2.2: exponents must stay within 0..24; the mantissa
                // reconstruction shifts by them.
                for (const auto exp : exps[static_cast<std::size_t>(ch)]) {
                    if (exp > kMaxExponent) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                if (ch < nfchans) {
                    (void)r.read(2);  // gainrng
                }
            }
        }

        if (r.read(1) != 0) {  // baie
            base_codes.sdcycod = static_cast<int>(r.read(2));
            base_codes.fdcycod = static_cast<int>(r.read(2));
            base_codes.sgaincod = static_cast<int>(r.read(2));
            base_codes.dbpbcod = static_cast<int>(r.read(2));
            base_codes.floorcod = static_cast<int>(r.read(3));
        } else if (block == 0) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        if (r.read(1) != 0) {  // snroffste
            csnroffst = static_cast<int>(r.read(6));
            if (cplinu) {
                fsnroffst[static_cast<std::size_t>(cpl_stream)] = static_cast<int>(r.read(4));
                fgaincod[static_cast<std::size_t>(cpl_stream)] = static_cast<int>(r.read(3));
            }
            for (int ch = 0; ch < nchans; ++ch) {
                fsnroffst[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(4));
                fgaincod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(3));
            }
        } else if (block == 0) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
        if (cplinu) {
            if (r.read(1) != 0) {  // cplleake
                cplfleak = static_cast<int>(r.read(3));
                cplsleak = static_cast<int>(r.read(3));
            }
        }
        // Held in a named variable rather than tested inline: the self-check
        // records it, and what §5.4.3.47 makes of a CLEAR one (keep the
        // previous block's state, except in block 0) is exactly the rule an
        // encoder can get wrong invisibly.
        const bool deltbaie = r.read(1) != 0;
        if (deltbaie) {
            // §5.4.3.48-57: a segment set is (deltnseg+1) triples of
            // (deltoffst, deltlen, deltba); bounds are checked here, before
            // compute_bit_allocation ever sees them, since deltoffst/deltlen
            // are attacker-controlled and mask[] is exactly 50 bands wide.
            // band_start is bin_to_band(cplstrtmant) for the coupling
            // channel's segments, 0 for an fbw channel's - matching
            // compute_bit_allocation()'s own band-cursor origin (see its
            // note on the coupling channel's start band not being 0).
            const auto parse_segments =
                [&r](int band_start) -> std::expected<DeltaSegments, DecodeError> {
                DeltaSegments segs;
                segs.deltnseg = static_cast<int>(r.read(3)) + 1;
                int band = band_start;
                for (int seg = 0; seg < segs.deltnseg; ++seg) {
                    segs.deltoffst[static_cast<std::size_t>(seg)] =
                        static_cast<std::uint8_t>(r.read(5));
                    segs.deltlen[static_cast<std::size_t>(seg)] =
                        static_cast<std::uint8_t>(r.read(4));
                    segs.deltba[static_cast<std::size_t>(seg)] =
                        static_cast<std::uint8_t>(r.read(3));
                    band += segs.deltoffst[static_cast<std::size_t>(seg)];
                    const int len = segs.deltlen[static_cast<std::size_t>(seg)];
                    if (band < 0 || band + len > 50) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                    band += len;
                }
                return segs;
            };
            // §5.4.3.48-49's own syntax table reads every stream's 2-bit
            // cpldeltbae/deltbae[ch] code FIRST, then every stream's segment
            // data - the two are not interleaved per stream, so the codes
            // must all be read (and validated) before any segment parsing.
            // Table 5.16: 00 reuse, 01 new info follows, 10 no delta, 11
            // reserved. cplcode stays at the "reuse" value when coupling is
            // not in use, so the fbw loop below never touches delta[cpl_stream].
            int cplcode = 0;
            if (cplinu) {
                cplcode = static_cast<int>(r.read(2));
                if (cplcode == 3) {
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (block == 0 && cplcode == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            // deltbae[ch]: fbw channels only (§5.4.3.49) - the LFE channel has
            // no delta bit allocation field at all. AC-3's widest acmod (3/2)
            // codes 5 full-bandwidth channels.
            std::array<int, 5> chcodes{};
            for (int ch = 0; ch < nfchans; ++ch) {
                chcodes[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
                if (chcodes[static_cast<std::size_t>(ch)] == 3) {
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (block == 0 && chcodes[static_cast<std::size_t>(ch)] == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            }
            if (cplinu && cplcode == 1) {
                auto segs = parse_segments(bin_to_band(cplstrtmant));
                if (!segs) {
                    return std::unexpected(segs.error());
                }
                delta[static_cast<std::size_t>(cpl_stream)] = *segs;
            } else if (cplinu && cplcode == 2) {
                delta[static_cast<std::size_t>(cpl_stream)] = {};
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const int chcode = chcodes[static_cast<std::size_t>(ch)];
                if (chcode == 1) {
                    auto segs = parse_segments(0);
                    if (!segs) {
                        return std::unexpected(segs.error());
                    }
                    delta[static_cast<std::size_t>(ch)] = *segs;
                } else if (chcode == 2) {
                    delta[static_cast<std::size_t>(ch)] = {};
                }
            }
        } else if (block == 0) {
            // §5.4.3.47: deltbaie == 0 in block 0 forces "no delta alloc" for
            // the coupling channel (if any) and every fbw channel.
            if (cplinu) {
                delta[static_cast<std::size_t>(cpl_stream)] = {};
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                delta[static_cast<std::size_t>(ch)] = {};
            }
        }
        if (r.read(1) != 0) {  // skiple
            const auto skipl = r.read(9);
            if (syntax != nullptr) {
                syntax->skip_field = true;
                syntax->skip_bytes = static_cast<std::uint16_t>(skipl);
            }
            r.skip(skipl * 8);
        }

        // Bit allocation (recomputed every block: parameters are stored
        // state, so the result matches the decoder-update rule of §7.2.1).
        const int streams = nchans + (cplinu ? 1 : 0);
        // §7.2.2.1.1 is frame-wide: csnroffst plus EVERY channel's fine
        // offset, including the coupling channel's and the LFE's.
        bool snr_all_zero = csnroffst == 0;
        for (int s = 0; s < streams && snr_all_zero; ++s) {
            snr_all_zero = fsnroffst[static_cast<std::size_t>(s)] == 0;
        }
        {
            AC3_ZONE_SCOPED_N("ac3_bit_allocation");
            for (int s = 0; s < streams; ++s) {
                const bool is_cpl = s == cpl_stream;
                const int end = endmant[static_cast<std::size_t>(s)];
                if (static_cast<int>(exps[static_cast<std::size_t>(s)].size()) != end) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                BitAllocCodes codes = base_codes;
                codes.fgaincod = fgaincod[static_cast<std::size_t>(s)];
                const BitAllocRegion region{.start = is_cpl ? cplstrtmant : 0,
                                            .coupling = is_cpl,
                                            .cplfleak = cplfleak,
                                            .cplsleak = cplsleak,
                                            .snr_all_zero = snr_all_zero,
                                            .delta = delta[static_cast<std::size_t>(s)]};
                bap[static_cast<std::size_t>(s)].assign(static_cast<std::size_t>(end), 0);
                compute_bit_allocation(exps[static_cast<std::size_t>(s)], sample_rate, codes,
                                       csnroffst, fsnroffst[static_cast<std::size_t>(s)],
                                       bap[static_cast<std::size_t>(s)], region);
            }
        }

        // The other half of the self-check's per-block record: everything the
        // encoder's own model claims about this block, as this side derived it
        // from the wire. Placed after the allocation rather than after the
        // audio, so a block whose mantissas turn out to be unreadable still
        // reports the state that decided how wide they were.
        // §5.4.3.47's gate bit says what this block TRANSMITTED; the syntax
        // trace reports what is in FORCE, which is not the same question - a
        // clear deltbaie retains the previous block's segments rather than
        // clearing them (see verify/mirror.hpp for why that distinction has
        // its own bug attached).
        if (syntax != nullptr) {
            for (int stream = 0; stream < streams && !syntax->delta_bit_alloc; ++stream) {
                syntax->delta_bit_alloc = delta[static_cast<std::size_t>(stream)].deltnseg > 0;
            }
        }
        if (config_.trace != nullptr) {
            auto& trace = config_.trace->blocks[static_cast<std::size_t>(block)];
            trace.deltbaie = deltbaie;
            trace.streams.resize(static_cast<std::size_t>(streams));
            for (int s = 0; s < streams; ++s) {
                auto& stream = trace.streams[static_cast<std::size_t>(s)];
                stream.exponents = exps[static_cast<std::size_t>(s)];
                stream.bap = bap[static_cast<std::size_t>(s)];
                stream.delta = delta[static_cast<std::size_t>(s)];
            }
            trace.allocated = true;
        }

        // Mantissas -> coefficients. §5.3.3 orders them by fbw channel, with
        // the coupling channel inserted after the FIRST coupled one, then the
        // LFE. Everything is unpacked before any reconstruction, because
        // decoupling and the rematrix undo both need whole channels.
        MantissaBlockReader mantissa_reader;
        coeffs.assign(max_streams, {});
        // §7.3.4: dither is substituted at a bap-0 bin only for a stream that
        // has its OWN dithflag - a full-bandwidth channel's own spectrum
        // (s < nfchans). The LFE has no dithflag bit at all (§5.4.3.2's loop
        // is bounded by nfchans) and always reconstructs as zero; the shared
        // coupling stream (s == cpl_stream) is deliberately left silent HERE
        // too, because §7.3.4 requires dither to be "applied after the
        // individual channels are extracted from the coupling channel" so
        // that "the dither applied to each channel's upper frequencies is
        // uncorrelated" - see the decoupling loop below for that half.
        const auto read_stream = [&](int s) {
            const int begin = s == cpl_stream ? cplstrtmant : 0;
            const int end = endmant[static_cast<std::size_t>(s)];
            const bool dither_eligible =
                s < nfchans && dithflag[static_cast<std::size_t>(s)];
            for (int bin = begin; bin < end; ++bin) {
                const int bap_value =
                    bap[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)];
                const int exp =
                    exps[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    coeffs[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)] =
                        dither_eligible ? dither_.next() / static_cast<double>(1u << exp) : 0.0;
                    continue;
                }
                const auto code = mantissa_reader.read(r, bap_value);
                coeffs[static_cast<std::size_t>(s)][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa(code, bap_value) / static_cast<double>(1u << exp);
            }
        };
        // Every stream's quantized mantissas off the wire, in the order
        // §5.4.3.28 packs them.
        {
            AC3_ZONE_SCOPED_N("ac3_mantissas");
            bool read_coupling = false;
            for (int ch = 0; ch < nfchans; ++ch) {
                read_stream(ch);
                if (cplinu && chincpl[static_cast<std::size_t>(ch)] && !read_coupling) {
                    read_stream(cpl_stream);
                    read_coupling = true;
                }
            }
            if (lfe) {
                read_stream(nfchans);
            }
        }

        // §7.4.3 decoupling: each coupled channel's high band is the shared
        // channel scaled by that channel's coordinate, times 8 - undoing the
        // encoder's /8 headroom scaling.
        {
            AC3_ZONE_SCOPED_N("ac3_decoupling");
            if (cplinu) {
                const auto& shared = coeffs[static_cast<std::size_t>(cpl_stream)];
                const auto& cpl_bap = bap[static_cast<std::size_t>(cpl_stream)];
                const auto& cpl_exps = exps[static_cast<std::size_t>(cpl_stream)];
                const int cplendmant = endmant[static_cast<std::size_t>(cpl_stream)];
                for (int ch = 0; ch < nfchans; ++ch) {
                    if (!chincpl[static_cast<std::size_t>(ch)]) {
                        continue;
                    }
                    auto& target = coeffs[static_cast<std::size_t>(ch)];
                    const bool ch_dither = dithflag[static_cast<std::size_t>(ch)];
                    for (int bnd = 0; bnd < ncplsubnd; ++bnd) {
                        const double coordinate =
                            cplco[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bnd)];
                        // §7.4.1: a set phase flag negates the right channel of a
                        // 2/0 pair across that band, restoring the phase the
                        // coupling sum discarded.
                        const double sign =
                            (phsflginu && ch == 1 &&
                             phsflg[static_cast<std::size_t>(
                                 subband_band[static_cast<std::size_t>(bnd)])])
                                ? -1.0
                                : 1.0;
                        const int low = cplstrtmant + bnd * coupling::kBinsPerSubBand;
                        const int high = std::min(low + coupling::kBinsPerSubBand, cplendmant);
                        for (int bin = low; bin < high; ++bin) {
                            const std::size_t ubin = static_cast<std::size_t>(bin);
                            // §7.3.4: a zero-bap shared bin is dither-substituted
                            // per RECEIVING channel, independently - reusing one
                            // dithered coupling-domain sample for every coupled
                            // channel would make their noise correlated (just
                            // scaled differently), which is exactly what "applied
                            // after the individual channels are extracted ...
                            // uncorrelated" rules out. Each channel draws its own
                            // sample and runs it through the same extraction
                            // formula a real coupling coefficient would use.
                            const double coeff =
                                (cpl_bap[ubin] == 0 && ch_dither)
                                    ? dither_.next() / static_cast<double>(1u << cpl_exps[ubin])
                                    : shared[ubin];
                            target[ubin] = coeff * coordinate * 8.0 * sign;
                        }
                    }
                }
            }
        }
        if (acmod == Acmod::k2_0) {
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, applied up
            // to the lower bandwidth of the two channels.
            const int cap = std::min(endmant[0], endmant[1]) - 1;
            for (std::size_t band = 0; band < kRematrixBands.size(); ++band) {
                if (!rematflg[band]) {
                    continue;
                }
                const int high = std::min(kRematrixBands[band][1], cap);
                for (int bin = kRematrixBands[band][0]; bin <= high; ++bin) {
                    const double l = coeffs[0][static_cast<std::size_t>(bin)];
                    const double rr = coeffs[1][static_cast<std::size_t>(bin)];
                    coeffs[0][static_cast<std::size_t>(bin)] = l + rr;
                    coeffs[1][static_cast<std::size_t>(bin)] = l - rr;
                }
            }
        }
        // §7.7 gain, applied to the COEFFICIENTS rather than to the output
        // samples: the overlap-add window then cross-fades one block's gain
        // into the next, which is what keeps a per-block gain change from
        // clicking. Scaling the 256 output samples instead would step.
        // Applied to every coded channel including the LFE: §7.7.1 describes a
        // gain change to the audio block, not to a subset of its channels. The
        // coupling channel is skipped because decoupling has already spread it
        // into the channels above. Dual mono's two channels are independent
        // programmes, so Ch2 gets its own gain from its own words rather than
        // sharing Ch1's - applying one programme's compression to the other
        // would be exactly the cross-talk 1+1 exists to avoid.
        for (int ch = 0; ch < nchans; ++ch) {
            const bool second_programme = acmod == Acmod::kDualMono && ch == 1;
            const double drc = second_programme
                                    ? internal::block_gain(config_, dynrng2_word, compr2)
                                    : internal::block_gain(config_, dynrng_word, compr);
            if (drc != 1.0) {
                for (auto& value : coeffs[static_cast<std::size_t>(ch)]) {
                    value *= drc;
                }
            }
        }
        // Everything above this point read the wire; everything below turns
        // what it read into audio. config_.skip_reconstruction stops here -
        // see its own comment for why an inspection pass wants exactly that
        // cut, and note that it is the LAST thing in the block: no field this
        // frame still has to read depends on it, so a parse that stops here
        // reads the identical bits a full decode does.
        //
        // The transform pair plus the overlap-add that reconstructs PCM from it -
        // where a decode frame spends most of its time, and the stage
        // DecoderConfig::fast_imdct's default switched under in 0.9.0. Skipped
        // (zone included) whenever the reconstruction itself is.
        if (!config_.skip_reconstruction) {
            AC3_ZONE_SCOPED_N("ac3_imdct_overlap");
            for (int ch = 0; ch < nchans; ++ch) {
                if (ch < nfchans && blksw[static_cast<std::size_t>(ch)]) {
                    imdct256_pair_windowed(coeffs[static_cast<std::size_t>(ch)], x,
                                           config_.fast_imdct);
                } else {
                    imdct512_windowed(coeffs[static_cast<std::size_t>(ch)], x, config_.fast_imdct);
                }
                auto& delay = delay_[static_cast<std::size_t>(ch)];
                const auto pcm = pcm_target[static_cast<std::size_t>(ch)];
                for (int n = 0; n < 256; ++n) {
                    const auto sample = static_cast<std::size_t>(n);
                    pcm[static_cast<std::size_t>(block * 256 + n)] =
                        static_cast<float>(2.0 * (x[sample] + delay[sample]));
                    delay[sample] = x[static_cast<std::size_t>(256 + n)];
                }
                // §7.10's raw material, captured into scratch rather than
                // straight into retained_: this frame may still be refused
                // below, and a refused frame must not become what the NEXT
                // loss is reconstructed from.
                if (retain_last_block && block == kBlocksPerFrame - 1) {
                    std::copy(x.begin(), x.end(),
                             conceal_scratch_[static_cast<std::size_t>(ch)].begin());
                }
            }
        }
        if (r.overflowed()) {
            return std::unexpected(DecodeError::kTruncated);
        }
    }
    if (retain_last_block) {
        if (!retained_) {
            // Aggregate-initialised rather than emplace()d: Retained is an
            // aggregate, and libstdc++'s optional::emplace() goes through
            // is_constructible_v, which clang does not satisfy for an
            // aggregate with no constructor of its own. MSVC accepts the
            // emplace form, so this is exactly the kind of difference the
            // Linux legs exist to catch.
            retained_ = Retained{};
        }
        retained_->nchans = nchans;
        // A swap rather than a copy: the buffer retained_ was holding comes
        // back the other way and is re-zeroed at the top of the next frame,
        // so neither side ever allocates again.
        retained_->last_block.swap(conceal_scratch_);
        // Metadata only - the PCM belongs to this frame and would be a
        // per-frame copy of every channel if it were kept.
        retained_->shape = out;
        retained_->shape.channels.clear();
        retained_->shape.concealed = std::nullopt;
    }
    // §5.4.2.8/§7.8, last: everything above reconstructs the CODED channels,
    // which is what the decoder is a reference for. Whatever the caller
    // wanted to hear instead happens here, once - except under
    // skip_reconstruction, where there is no PCM to fold: the value form's
    // `channels` is empty (harmless no-op below) but the decode_frame_into
    // form's `external` spans were never written through, and folding them
    // would read - and, for a caller who also asked for a downmix, silently
    // rewrite - buffers this call promised to leave untouched.
    if (!config_.skip_reconstruction) {
        const auto levels = mix_levels(cmixlev, surmixlev);
        if (external.empty()) {
            output_.apply(out.channels, acmod, lfe, levels, dialnorm);
        } else {
            output_.apply(external.first(static_cast<std::size_t>(nchans)), acmod, lfe, levels,
                          dialnorm);
        }
    }
    return out;
}

}  // namespace ac3
