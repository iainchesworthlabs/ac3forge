#include "ac3/io/metadata_edit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/meta/mixing.hpp"

namespace ac3::io {

namespace {

constexpr int kAc3MaxBsid = 10;

// Where each rewritable field sits, in bits from the start of the syncframe.
// Only meaningful when the matching FrameMetadata optional holds a value (or,
// for dialnorm, always - every syncframe of both generations carries one).
struct FieldOffsets {
    std::size_t dialnorm = 0;
    std::size_t compr = 0;
    std::size_t dialnorm2 = 0;
    std::size_t compr2 = 0;
    std::size_t bsmod = 0;
    std::size_t dsurmod = 0;
};

struct Parsed {
    FrameMetadata meta;
    FieldOffsets at;
};

[[nodiscard]] bool sync_at(std::span<const std::byte> frame) {
    return frame.size() >= 2 && std::to_integer<std::uint8_t>(frame[0]) == 0x0B &&
           std::to_integer<std::uint8_t>(frame[1]) == 0x77;
}

void write_bits(std::span<std::byte> frame, std::size_t bit_at, std::uint32_t value, int bits) {
    for (int i = 0; i < bits; ++i) {
        const std::size_t pos = bit_at + static_cast<std::size_t>(i);
        const std::size_t byte = pos >> 3;
        const auto mask = static_cast<std::uint8_t>(1u << (7 - (pos & 7)));
        auto current = std::to_integer<std::uint8_t>(frame[byte]);
        const bool set = ((value >> (bits - 1 - i)) & 1u) != 0;
        current = static_cast<std::uint8_t>(set ? (current | mask)
                                                : (current & static_cast<std::uint8_t>(~mask)));
        frame[byte] = std::byte{current};
    }
}

// Table 5.9 / Table 5.10. '11' is reserved in both; §5.4.2.4/§5.4.2.5 tell a
// decoder to fall back on an intermediate value rather than treat it as an
// error, and neither enum has a member for it - so a reserved code reads back
// as "no level transmitted this reader can name" rather than as a wrong one.
[[nodiscard]] std::optional<meta::CentreMixLevel> centre_mix_level(std::uint32_t raw) {
    return raw <= 2 ? std::optional{static_cast<meta::CentreMixLevel>(raw)} : std::nullopt;
}

[[nodiscard]] std::optional<meta::SurroundMixLevel> surround_mix_level(std::uint32_t raw) {
    return raw <= 2 ? std::optional{static_cast<meta::SurroundMixLevel>(raw)} : std::nullopt;
}

// Table D2.2: '11' is reserved and reads as "not indicated", which the enum
// does have a member for.
[[nodiscard]] meta::DownmixMode downmix_mode(std::uint32_t raw) {
    return raw <= 2 ? static_cast<meta::DownmixMode>(raw) : meta::DownmixMode::kNotIndicated;
}

// --- AC-3 ------------------------------------------------------------------

std::expected<Parsed, EditError> parse_ac3(std::span<const std::byte> frame) {
    BitReader r{frame};
    r.skip(16 + 16);  // syncword, crc1
    const auto fscod = r.read(2);
    const auto frmsizecod = r.read(6);
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(EditError::kReservedValue);
    }
    Parsed out;
    out.meta.kind = StreamKind::kAc3;
    out.meta.sample_rate = static_cast<SampleRate>(fscod);
    const auto bytes = frame_size_bytes(out.meta.sample_rate, kBitratesKbps[frmsizecod >> 1],
                                        (frmsizecod & 1) != 0);
    if (!bytes) {
        return std::unexpected(EditError::kReservedValue);
    }
    out.meta.bytes = *bytes;
    if (frame.size() < out.meta.bytes) {
        return std::unexpected(EditError::kTruncated);
    }

    out.meta.bsid = static_cast<int>(r.read(5));
    if (out.meta.bsid > kAc3MaxBsid) {
        return std::unexpected(EditError::kUnsupportedBsid);
    }
    // bsmod is unconditional in AC-3 bsi (§5.4.2.2), which is what makes it
    // rewritable here at all - E-AC-3 hides it behind infomdate.
    out.at.bsmod = r.bit_position();
    out.meta.bsmod = static_cast<int>(r.read(3));

    const auto acmod = r.read(3);
    out.meta.acmod = static_cast<Acmod>(acmod);
    if ((acmod & 0x1) != 0 && acmod != 0x1) {
        out.meta.cmixlev = centre_mix_level(r.read(2));
    }
    if ((acmod & 0x4) != 0) {
        out.meta.surmixlev = surround_mix_level(r.read(2));
    }
    if (acmod == 0x2) {
        out.at.dsurmod = r.bit_position();
        out.meta.dsurmod = static_cast<int>(r.read(2));
    }
    out.meta.lfe = r.read(1) != 0;

    out.at.dialnorm = r.bit_position();
    out.meta.dialnorm = static_cast<int>(r.read(5));
    if (r.read(1) != 0) {  // compre
        out.at.compr = r.bit_position();
        out.meta.compr = static_cast<std::uint8_t>(r.read(8));
    }
    if (r.read(1) != 0) {  // langcode
        r.skip(8);
    }
    if (r.read(1) != 0) {  // audprodie
        r.skip(5 + 2);     // mixlevel, roomtyp
    }
    if (acmod == 0x0) {
        out.at.dialnorm2 = r.bit_position();
        out.meta.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            out.at.compr2 = r.bit_position();
            out.meta.compr2 = static_cast<std::uint8_t>(r.read(8));
        }
        // langcod2e / audprodi2e follow; nothing past here is rewritable, so
        // the walk stops rather than re-deriving the whole of §5.4.2.
    }
    if (r.overflowed()) {
        return std::unexpected(EditError::kTruncated);
    }
    return out;
}

// --- E-AC-3 ----------------------------------------------------------------

// Table E1.2's mixmdate group. Walked in full (not just to the fields worth
// reporting) because infomdate - which carries bsmod and dsurmod - sits
// immediately after it, and its bit offset is only right if every conditional
// here is. Mirrors io/elementary.cpp's skip_mixing_metadata field for field;
// the difference is that this keeps the values.
void read_mixing_metadata(BitReader& r, const FrameMetadata& meta, int nblks,
                          WireMixMetadata& mix) {
    const auto acmod = static_cast<std::uint8_t>(meta.acmod);
    if (acmod > 0x2) {
        mix.dmixmod = downmix_mode(r.read(2));
    }
    if ((acmod & 0x1) != 0 && acmod > 0x2) {
        mix.ltrtcmixlev = static_cast<meta::MixLevel>(r.read(3));
        mix.lorocmixlev = static_cast<meta::MixLevel>(r.read(3));
    }
    if ((acmod & 0x4) != 0) {
        mix.ltrtsurmixlev = static_cast<meta::MixLevel>(r.read(3));
        mix.lorosurmixlev = static_cast<meta::MixLevel>(r.read(3));
    }
    if (meta.lfe && r.read(1) != 0) {
        mix.lfemixlevcod = static_cast<int>(r.read(5));
    }
    if (meta.strmtyp != static_cast<int>(eac3::StreamType::kDependent)) {
        if (r.read(1) != 0) r.skip(6);                  // pgmscl
        if (acmod == 0x0 && r.read(1) != 0) r.skip(6);  // pgmscl2
        if (r.read(1) != 0) r.skip(6);                  // extpgmscl
        switch (r.read(2)) {                            // mixdef
            case 0x1: r.skip(1 + 1 + 3); break;         // premixcmpsel, drcsrc, premixcmpscl
            case 0x2: r.skip(12); break;                // mixdata
            case 0x3: {
                // mixdeflen sizes the WHOLE remaining element, sub-fields and
                // byte-alignment padding included.
                const auto mixdeflen = r.read(5);
                r.skip((mixdeflen + 2) * 8);
                break;
            }
            default: break;
        }
        if (acmod < 0x2) {
            if (r.read(1) != 0) r.skip(8 + 6);  // panmean, paninfo
            if (acmod == 0x0 && r.read(1) != 0) r.skip(8 + 6);
        }
        if (r.read(1) != 0) {  // frmmixcfginfoe
            if (meta.numblkscod == 0x0) {
                r.skip(5);  // blkmixcfginfo[0]
            } else {
                for (int blk = 0; blk < nblks; ++blk) {
                    if (r.read(1) != 0) r.skip(5);  // blkmixcfginfo[blk]
                }
            }
        }
    }
}

std::expected<Parsed, EditError> parse_eac3(std::span<const std::byte> frame) {
    BitReader r{frame};
    r.skip(16);  // syncword
    Parsed out;
    out.meta.kind = StreamKind::kEac3;
    out.meta.strmtyp = static_cast<int>(r.read(2));
    if (out.meta.strmtyp == static_cast<int>(eac3::StreamType::kConvertible) ||
        out.meta.strmtyp == 0x3) {
        // strmtyp 2's own blkid/frmsizecod branch (and 3, which is reserved)
        // - see this module's header comment.
        return std::unexpected(EditError::kReservedValue);
    }
    out.meta.substreamid = static_cast<int>(r.read(3));
    out.meta.bytes = (static_cast<std::size_t>(r.read(11)) + 1) * 2;
    if (frame.size() < out.meta.bytes) {
        return std::unexpected(EditError::kTruncated);
    }
    const auto fscod = r.read(2);
    if (fscod == 0x3) {
        // §E2.3.1.3: fscod2 replaces numblkscod outright - a reduced-rate
        // substream is implicitly always six blocks.
        const auto rate = sample_rate_from_fscod2(r.read(2));
        if (!rate) {
            return std::unexpected(EditError::kReservedValue);
        }
        out.meta.sample_rate = *rate;
        out.meta.numblkscod = 0x3;
    } else {
        out.meta.sample_rate = static_cast<SampleRate>(fscod);
        out.meta.numblkscod = static_cast<int>(r.read(2));
    }
    const auto acmod = r.read(3);
    out.meta.acmod = static_cast<Acmod>(acmod);
    out.meta.lfe = r.read(1) != 0;
    out.meta.bsid = static_cast<int>(r.read(5));
    if (out.meta.bsid != eac3::kBsid) {
        return std::unexpected(EditError::kUnsupportedBsid);
    }

    out.at.dialnorm = r.bit_position();
    out.meta.dialnorm = static_cast<int>(r.read(5));
    const bool dependent = out.meta.strmtyp == static_cast<int>(eac3::StreamType::kDependent);
    if (r.read(1) != 0) {  // compre
        const auto at = r.bit_position();
        const auto word = static_cast<std::uint8_t>(r.read(8));
        // §E3.8.5: on a dependent substream compre marks the last dependent
        // of the programme rather than announcing a compression word, so
        // these 8 bits are not a compr value and must not be rewritten as
        // one - reported absent, exactly as the decoder reports it.
        if (!dependent) {
            out.at.compr = at;
            out.meta.compr = word;
        }
    }
    if (acmod == 0x0) {
        out.at.dialnorm2 = r.bit_position();
        out.meta.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            const auto at = r.bit_position();
            const auto word = static_cast<std::uint8_t>(r.read(8));
            if (!dependent) {
                out.at.compr2 = at;
                out.meta.compr2 = word;
            }
        }
    }
    if (dependent && r.read(1) != 0) {  // chanmape
        r.skip(16);
    }
    const int nblks = eac3::blocks_per_syncframe(out.meta.numblkscod);
    if (r.read(1) != 0) {  // mixmdate
        WireMixMetadata mix;
        read_mixing_metadata(r, out.meta, nblks, mix);
        out.meta.mix = mix;
    }
    if (r.read(1) != 0) {  // infomdate
        out.at.bsmod = r.bit_position();
        out.meta.bsmod = static_cast<int>(r.read(3));
        r.skip(1 + 1);  // copyrightb, origbs
        if (acmod == 0x2) {
            out.at.dsurmod = r.bit_position();
            out.meta.dsurmod = static_cast<int>(r.read(2));
        }
        // dheadphonmod onwards is not rewritable, so the walk stops here.
    }
    if (r.overflowed()) {
        return std::unexpected(EditError::kTruncated);
    }
    return out;
}

std::expected<Parsed, EditError> parse(std::span<const std::byte> frame) {
    if (!sync_at(frame)) {
        return std::unexpected(EditError::kBadSyncWord);
    }
    if (frame.size() < 6) {
        return std::unexpected(EditError::kTruncated);
    }
    // bsid at bit 40 in both generations - the same probe ac3::io::scan uses.
    BitReader probe{frame};
    probe.skip(40);
    const auto bsid = static_cast<int>(probe.read(5));
    if (bsid <= kAc3MaxBsid) {
        return parse_ac3(frame);
    }
    if (bsid == eac3::kBsid) {
        return parse_eac3(frame);
    }
    return std::unexpected(EditError::kUnsupportedBsid);
}

// Every value the edit names must fit its field AND actually be on the wire,
// checked before a single bit is written: a half-applied edit would leave a
// frame claiming metadata nobody asked for.
std::expected<void, EditError> check(const Parsed& parsed, const MetadataEdit& edit) {
    const auto in_range = [](int value, int low, int high) { return value >= low && value <= high; };
    if (edit.dialnorm && !in_range(*edit.dialnorm, 1, 31)) {
        return std::unexpected(EditError::kOutOfRange);
    }
    if (edit.dialnorm2) {
        if (!in_range(*edit.dialnorm2, 1, 31)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.dialnorm2) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    if (edit.compr && !parsed.meta.compr) {
        return std::unexpected(EditError::kFieldAbsent);
    }
    if (edit.compr2 && !parsed.meta.compr2) {
        return std::unexpected(EditError::kFieldAbsent);
    }
    if (edit.bsmod) {
        if (!in_range(*edit.bsmod, 0, 7)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.bsmod) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    if (edit.dsurmod) {
        if (!in_range(*edit.dsurmod, 0, 3)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.dsurmod) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    return {};
}

}  // namespace

std::string_view describe(EditError error) {
    switch (error) {
        case EditError::kBadSyncWord: return "no syncword: expected 0x0B77";
        case EditError::kTruncated: return "stream ends mid-frame";
        case EditError::kUnsupportedBsid:
            return "unsupported bsid (expected AC-3 <= 10 or E-AC-3 16)";
        case EditError::kReservedValue: return "reserved value in the frame header";
        case EditError::kFieldAbsent:
            return "this stream does not transmit that field, so there are no bits to rewrite";
        case EditError::kOutOfRange: return "value outside the field's range";
    }
    return "unknown error";
}

std::expected<FrameMetadata, EditError> read_frame_metadata(std::span<const std::byte> frame) {
    const auto parsed = parse(frame);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return parsed->meta;
}

std::expected<void, EditError> restamp_crc(std::span<std::byte> frame) {
    const auto parsed = parse(frame);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const std::size_t bytes = parsed->meta.bytes;
    const std::span<const std::byte> view{frame.data(), bytes};
    if (parsed->meta.kind == StreamKind::kAc3) {
        // crc1 PRECEDES the region it covers, so it is solved rather than
        // computed - see this module's header comment and crc16.hpp's own.
        const std::uint32_t words58 = frame_size_58_words(static_cast<std::uint32_t>(bytes / 2));
        const std::uint16_t crc1 = solve_leading_crc(view.subspan(4, 2 * words58 - 4));
        frame[2] = static_cast<std::byte>(crc1 >> 8);
        frame[3] = static_cast<std::byte>(crc1 & 0xFF);
    }
    std::uint16_t crc2 = crc16(view.subspan(2, bytes - 4));
    if (crc2 == kSyncWord) {
        // §5.4.5.1: crcrsv exists so a crc2 that would collide with the sync
        // word can be perturbed. The same trick the encoder uses, and it is
        // reachable here for exactly the same reason it is there.
        frame[bytes - 3] ^= std::byte{0x01};
        crc2 = crc16(view.subspan(2, bytes - 4));
    }
    frame[bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
    return {};
}

std::expected<FrameMetadata, EditError> edit_frame_metadata(std::span<std::byte> frame,
                                                            const MetadataEdit& edit) {
    auto parsed = parse(frame);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (const auto ok = check(*parsed, edit); !ok) {
        return std::unexpected(ok.error());
    }
    if (edit.dialnorm) {
        write_bits(frame, parsed->at.dialnorm, static_cast<std::uint32_t>(*edit.dialnorm), 5);
        parsed->meta.dialnorm = *edit.dialnorm;
    }
    if (edit.dialnorm2) {
        write_bits(frame, parsed->at.dialnorm2, static_cast<std::uint32_t>(*edit.dialnorm2), 5);
        parsed->meta.dialnorm2 = *edit.dialnorm2;
    }
    if (edit.compr) {
        write_bits(frame, parsed->at.compr, *edit.compr, 8);
        parsed->meta.compr = *edit.compr;
    }
    if (edit.compr2) {
        write_bits(frame, parsed->at.compr2, *edit.compr2, 8);
        parsed->meta.compr2 = *edit.compr2;
    }
    if (edit.bsmod) {
        write_bits(frame, parsed->at.bsmod, static_cast<std::uint32_t>(*edit.bsmod), 3);
        parsed->meta.bsmod = *edit.bsmod;
    }
    if (edit.dsurmod) {
        write_bits(frame, parsed->at.dsurmod, static_cast<std::uint32_t>(*edit.dsurmod), 2);
        parsed->meta.dsurmod = *edit.dsurmod;
    }
    if (const auto ok = restamp_crc(frame); !ok) {
        return std::unexpected(ok.error());
    }
    return parsed->meta;
}

std::expected<EditSummary, EditError> edit_stream_metadata(std::span<std::byte> stream,
                                                            const MetadataEdit& edit) {
    // Two passes, so a stream is either fully rewritten or not touched at
    // all. The first works out, per named field, whether ANY syncframe in
    // the stream carries it; the second applies the edit frame by frame,
    // narrowed to what each one actually has.
    //
    // The split is what makes "this stream has no such field" and "this
    // particular substream has no such field" two different answers. A 1+1
    // programme's dialnorm2, an acmod 2/0 substream's dsurmod and an
    // independent substream's compr are all fields another substream of the
    // same stream may legitimately lack, so refusing on the first frame that
    // lacks one would refuse perfectly ordinary streams - and silently
    // skipping a field NO frame has would be worse still, since a metadata
    // option that quietly does nothing is indistinguishable from one that
    // does not work.
    bool any_dialnorm2 = false;
    bool any_compr = false;
    bool any_compr2 = false;
    bool any_bsmod = false;
    bool any_dsurmod = false;
    for (std::size_t offset = 0; offset < stream.size();) {
        const auto parsed = parse(stream.subspan(offset));
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        any_dialnorm2 = any_dialnorm2 || parsed->meta.dialnorm2.has_value();
        any_compr = any_compr || parsed->meta.compr.has_value();
        any_compr2 = any_compr2 || parsed->meta.compr2.has_value();
        any_bsmod = any_bsmod || parsed->meta.bsmod.has_value();
        any_dsurmod = any_dsurmod || parsed->meta.dsurmod.has_value();
        offset += parsed->meta.bytes;
    }
    if ((edit.dialnorm2 && !any_dialnorm2) || (edit.compr && !any_compr) ||
        (edit.compr2 && !any_compr2) || (edit.bsmod && !any_bsmod) ||
        (edit.dsurmod && !any_dsurmod)) {
        return std::unexpected(EditError::kFieldAbsent);
    }

    EditSummary summary;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const auto remaining = stream.subspan(offset);
        const auto parsed = parse(remaining);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        const auto bytes = parsed->meta.bytes;
        auto frame = remaining.first(bytes);

        MetadataEdit per_frame = edit;
        if (!parsed->meta.dialnorm2) {
            per_frame.dialnorm2.reset();
        }
        // compr/compr2 are the independent substream's alone (§E3.8.5) and
        // read back as absent on a dependent, so this covers both "not a
        // compression word here" and "compre was simply clear".
        if (!parsed->meta.compr) {
            per_frame.compr.reset();
        }
        if (!parsed->meta.compr2) {
            per_frame.compr2.reset();
        }
        if (!parsed->meta.bsmod) {
            per_frame.bsmod.reset();
        }
        if (!parsed->meta.dsurmod) {
            per_frame.dsurmod.reset();
        }

        const std::vector<std::byte> before(frame.begin(), frame.end());
        const auto edited = edit_frame_metadata(frame, per_frame);
        if (!edited) {
            return std::unexpected(edited.error());
        }
        ++summary.syncframes;
        if (!std::equal(before.begin(), before.end(), frame.begin())) {
            ++summary.changed;
        }
        offset += bytes;
    }
    if (summary.syncframes == 0) {
        return std::unexpected(EditError::kTruncated);
    }
    return summary;
}

}  // namespace ac3::io
