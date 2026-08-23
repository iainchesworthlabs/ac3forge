#include "ac3/io/elementary.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"

namespace ac3::io {

namespace {

constexpr int kAc3MaxBsid = 10;   // §5.4.1.3: 8 for the base standard, 10 with annexes
constexpr int kBsidBitOffset = 40;

// Table 5.8 channel positions as chanmap locations, so an E-AC-3 bed and its
// dependents' maps can be unioned in one vocabulary rather than compared as
// two different kinds of thing.
[[nodiscard]] std::uint16_t bed_locations(Acmod acmod, bool lfe) {
    using namespace eac3::chanmap;
    std::uint16_t map = 0;
    switch (acmod) {
        // 1+1 has no Table E2.5 location - Ch1 and Ch2 are independent
        // programmes, not directions - but this function's only consumer
        // wants a channel COUNT, so the same placeholder acmod_map() uses for
        // that reason stands in here too: two bits, for two coded channels.
        case Acmod::kDualMono:
            map = kLeftBit | kRightBit;
            break;
        case Acmod::k1_0:
            map = kCentreBit;
            break;
        case Acmod::k2_0:
            map = kLeftBit | kRightBit;
            break;
        case Acmod::k3_0:
            map = kLeftBit | kCentreBit | kRightBit;
            break;
        case Acmod::k2_1:
            map = kLeftBit | kRightBit | kCsBit;
            break;
        case Acmod::k3_1:
            map = kLeftBit | kCentreBit | kRightBit | kCsBit;
            break;
        case Acmod::k2_2:
            map = kLeftBit | kRightBit | kLeftSurroundBit | kRightSurroundBit;
            break;
        case Acmod::k3_2:
            map = kLeftBit | kCentreBit | kRightBit | kLeftSurroundBit | kRightSurroundBit;
            break;
    }
    return static_cast<std::uint16_t>(map | (lfe ? kLfeBit : 0));
}

[[nodiscard]] bool sync_at(std::span<const std::byte> stream, std::size_t offset) {
    return offset + 2 <= stream.size() &&
           std::to_integer<std::uint8_t>(stream[offset]) == 0x0B &&
           std::to_integer<std::uint8_t>(stream[offset + 1]) == 0x77;
}

// --- AC-3 ------------------------------------------------------------------

// Walk bsi far enough to reach lfeon, whose position depends on which of
// cmixlev, surmixlev and dsurmod acmod brought with it (§5.4.2). bsid/bsmod
// are captured, not skipped: build_codec_config_box() (ac3/io/dec3.hpp) needs
// both to fill in AC3SpecificBox's own bsid/bsmod fields (ETSI TS 102 366
// Annex F §F.4).
std::expected<void, ScanError> read_ac3_bsi(BitReader& r, int& bsid, int& bsmod, Acmod& acmod,
                                            bool& lfe) {
    bsid = static_cast<int>(r.read(5));
    bsmod = static_cast<int>(r.read(3));
    const auto raw = r.read(3);
    acmod = static_cast<Acmod>(raw);
    if ((raw & 0x1) && raw != 0x1) {
        r.skip(2);  // cmixlev
    }
    if (raw & 0x4) {
        r.skip(2);  // surmixlev
    }
    if (raw == 0x2) {
        r.skip(2);  // dsurmod
    }
    lfe = r.read(1) != 0;
    return r.overflowed() ? std::unexpected(ScanError::kTruncated)
                          : std::expected<void, ScanError>{};
}

std::expected<ScannedStream, ScanError> scan_ac3(std::span<const std::byte> stream) {
    ScannedStream out{.kind = StreamKind::kAc3, .substreams_per_unit = 1};
    std::size_t offset = 0;
    bool first = true;
    while (offset < stream.size()) {
        if (!sync_at(stream, offset) || offset + 5 > stream.size()) {
            return std::unexpected(ScanError::kLostSync);
        }
        // syncinfo: fscod and frmsizecod share byte 4, and together index
        // Table 5.18 for the frame size.
        const auto byte4 = std::to_integer<std::uint32_t>(stream[offset + 4]);
        const auto fscod = byte4 >> 6;
        const auto frmsizecod = byte4 & 0x3F;
        if (fscod == 3 || frmsizecod > 37) {
            return std::unexpected(ScanError::kReservedValue);
        }
        const auto rate = static_cast<SampleRate>(fscod);
        const auto bytes =
            frame_size_bytes(rate, kBitratesKbps[frmsizecod >> 1], (frmsizecod & 1) != 0);
        if (!bytes || offset + *bytes > stream.size()) {
            return std::unexpected(ScanError::kTruncated);
        }
        if (first) {
            BitReader r{stream.subspan(offset)};
            r.skip(kBsidBitOffset);
            int bsid = 0;
            int bsmod = 0;
            Acmod acmod = Acmod::k2_0;
            bool lfe = false;
            if (const auto ok = read_ac3_bsi(r, bsid, bsmod, acmod, lfe); !ok) {
                return std::unexpected(ok.error());
            }
            out.sample_rate = rate;
            out.acmod = acmod;
            out.lfe = lfe;
            out.channels = fullbw_channel_count(acmod) + (lfe ? 1 : 0);
            out.bsid = bsid;
            out.bsmod = bsmod;
            // Table 5.18: frmsizecod's high bits already index kBitratesKbps
            // above; AC3SpecificBox's bit_rate_code (ETSI TS 102 366 Annex F
            // §F.4) is exactly that same index.
            out.bit_rate_code = static_cast<int>(frmsizecod >> 1);
            first = false;
        }
        out.access_units.push_back(stream.subspan(offset, *bytes));
        offset += *bytes;
    }
    // AC-3 has no substream layer at all, so the whole stream is one programme
    // by construction. The single-entry list is what lets a caller walk
    // `programmes` without first asking which generation it is holding.
    out.programmes.push_back({.substreamid = 0,
                              .acmod = out.acmod,
                              .lfe = out.lfe,
                              .channels = out.channels,
                              .bsid = out.bsid,
                              .bsmod = out.bsmod,
                              .substreams_per_unit = 1,
                              .oba_complexity_index = std::nullopt,
                              .access_units = out.access_units});
    return out;
}

// --- E-AC-3 ----------------------------------------------------------------

struct Substream {
    int strmtyp = 0;
    // §E2.3.1.2. On an independent substream this is the programme id (I0-I7);
    // on a dependent it numbers within its own parent's space and says nothing
    // about which parent that is.
    int substreamid = 0;
    std::size_t bytes = 0;
    SampleRate sample_rate = SampleRate::k48000;
    // 0x3 doubles as "fscod2 was used" (always six blocks), matching
    // decoder/eac3_decoder.cpp's Bsi::numblkscod convention - every
    // downstream "is this the always-six-blocks case?" check keeps working
    // unmodified for a value nothing ever actually transmits as 0x3 outright.
    int numblkscod = 3;
    int bsid = 0;
    int bsmod = 0;  // 0 (not indicated) unless infomdate carried one
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    std::uint16_t chanmap = 0;  // 0 when chanmape was clear
    // TS 103 420 §8.3.1/§8.3.2.2, out of THIS substream's own addbsi.
    bool oba_extension = false;
    int oba_complexity_index = 0;
};

// Table E1.2's mixing-metadata payload, walked (not interpreted) purely to
// reach addbsi at the right bit offset - every field here mirrors
// decoder/eac3_decoder.cpp's function of the same name field for field
// (including its comments), which is deliberate: this file re-derives bsi
// independently rather than reusing decoder internals, the same way
// read_eac3_substream above already re-derives everything up through
// chanmap on its own. A scan is a much smaller job than a decode and has no
// business depending on the decoder's private Bsi/parse_bsi.
void skip_mixing_metadata(BitReader& r, const Substream& s, int nblks) {
    const auto acmod = static_cast<std::uint8_t>(s.acmod);
    if (acmod > 0x2) {
        r.skip(2);  // dmixmod
    }
    if ((acmod & 0x1) != 0 && acmod > 0x2) {
        r.skip(3 + 3);  // ltrtcmixlev, lorocmixlev
    }
    if ((acmod & 0x4) != 0) {
        r.skip(3 + 3);  // ltrtsurmixlev, lorosurmixlev
    }
    if (s.lfe && r.read(1) != 0) {
        r.skip(5);  // lfemixlevcod
    }
    if (s.strmtyp != static_cast<int>(eac3::StreamType::kDependent)) {
        if (r.read(1) != 0) r.skip(6);  // pgmscl
        if (acmod == 0x0 && r.read(1) != 0) r.skip(6);  // pgmscl2
        if (r.read(1) != 0) r.skip(6);  // extpgmscl
        switch (r.read(2)) {            // mixdef
            case 0x1: r.skip(1 + 1 + 3); break;  // premixcmpsel, drcsrc, premixcmpscl
            case 0x2: r.skip(12); break;         // mixdata
            case 0x3: {
                // mixdeflen sizes the WHOLE remaining element, sub-fields and
                // byte-alignment padding included, so it can be skipped whole
                // without walking mixdata2e/mixdata3e.
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
            if (s.numblkscod == 0x0) {
                r.skip(5);  // blkmixcfginfo[0]
            } else {
                for (int blk = 0; blk < nblks; ++blk) {
                    if (r.read(1) != 0) r.skip(5);  // blkmixcfginfo[blk]
                }
            }
        }
    }
}

// Table E1.2's informational-metadata payload: bsmod and the production
// notes. bsmod is READ here (not skipped) - see Substream::bsmod above.
void skip_informational_metadata(BitReader& r, Substream& s) {
    const auto acmod = static_cast<std::uint8_t>(s.acmod);
    s.bsmod = static_cast<int>(r.read(3));
    r.skip(1 + 1);  // copyrightb, origbs
    if (acmod == 0x2) {
        r.skip(2 + 2);  // dsurmod, dheadphonmod
    }
    if (acmod >= 0x6) {
        r.skip(2);  // dsurexmod
    }
    if (r.read(1) != 0) r.skip(5 + 2 + 1);  // mixlevel, roomtyp, adconvtyp
    if (acmod == 0x0 && r.read(1) != 0) r.skip(5 + 2 + 1);
    // §E2.3.2.6: sourcefscod is present only when fscod != 0x3 - a fscod2
    // frame never carries it at all.
    if (!is_reduced_rate(s.sample_rate)) {
        r.skip(1);
    }
}

std::expected<Substream, ScanError> read_eac3_substream(std::span<const std::byte> at) {
    BitReader r{at};
    r.skip(16);  // syncword
    Substream s;
    s.strmtyp = static_cast<int>(r.read(2));
    s.substreamid = static_cast<int>(r.read(3));
    s.bytes = (static_cast<std::size_t>(r.read(11)) + 1) * 2;
    const auto fscod = r.read(2);
    if (fscod == 0x3) {
        // §E2.3.1.3: fscod2 replaces numblkscod outright - a reduced-rate
        // substream is implicitly always six blocks, so numblkscod's bits are
        // never sent.
        const auto fscod2 = r.read(2);
        const auto rate = sample_rate_from_fscod2(fscod2);
        if (!rate) {
            return std::unexpected(ScanError::kReservedValue);
        }
        s.sample_rate = *rate;
        s.numblkscod = 0x3;
    } else {
        s.sample_rate = static_cast<SampleRate>(fscod);
        s.numblkscod = static_cast<int>(r.read(2));
    }
    const auto acmod = r.read(3);
    s.acmod = static_cast<Acmod>(acmod);
    s.lfe = r.read(1) != 0;
    s.bsid = static_cast<int>(r.read(5));
    r.skip(5);  // dialnorm
    if (r.read(1)) {
        r.skip(8);  // compr
    }
    if (acmod == 0x0) {
        r.skip(5);  // dialnorm2
        if (r.read(1)) {
            r.skip(8);  // compr2
        }
    }
    if (s.strmtyp == static_cast<int>(eac3::StreamType::kDependent)) {
        if (r.read(1)) {  // chanmape
            s.chanmap = static_cast<std::uint16_t>(r.read(16));
        }
    }
    const int nblks = eac3::blocks_per_syncframe(s.numblkscod);
    if (r.read(1)) {  // mixmdate
        skip_mixing_metadata(r, s, nblks);
    }
    if (r.read(1)) {  // infomdate
        skip_informational_metadata(r, s);
    }
    if (s.strmtyp == static_cast<int>(eac3::StreamType::kIndependent) && s.numblkscod != 0x3) {
        r.skip(1);  // convsync
    }
    if (s.strmtyp == static_cast<int>(eac3::StreamType::kConvertible)) {
        const bool blkid = s.numblkscod == 0x3 || r.read(1) != 0;
        if (blkid) {
            r.skip(6);  // frmsizecod, describing the AC-3 frame this came from
        }
    }
    if (r.read(1)) {  // addbsie
        const auto addbsil = r.read(6);
        const std::uint32_t addbsi_bits = (addbsil + 1) * 8;  // always >= 8
        // TS 103 420 §8.3.1 fixes an object-audio stream's addbsi to 7
        // reserved bits then flag_ec3_extension_type_a, then (only when that
        // bit is set) an 8-bit complexity_index_type_a (§8.3.2.2) - exactly
        // what encoder/eac3_frame.cpp writes when config.oba_complexity_index
        // is set, and the same position FFmpeg and other tools are documented
        // to key their own Atmos detection off (see encoder/eac3_frame.hpp's
        // oba_complexity_index comment). Reading those bits unconditionally
        // (rather than short-circuiting on whether they turn out to look like
        // the marker) is what keeps `consumed` correct either way - a stream
        // that used addbsi for something else still has its declared length
        // skipped intact, just with `consumed` stopping at 8 instead of 16.
        const auto reserved = r.read(7);
        const auto flag = r.read(1);
        std::uint32_t consumed = 8;
        if (reserved == 0 && flag != 0 && addbsi_bits >= 16) {
            s.oba_extension = true;
            s.oba_complexity_index = static_cast<int>(r.read(8));
            consumed = 16;
        }
        if (addbsi_bits > consumed) {
            r.skip(static_cast<std::size_t>(addbsi_bits - consumed));
        }
    }
    if (r.overflowed()) {
        return std::unexpected(ScanError::kTruncated);
    }
    return s;
}

// One programme's running state while scan_eac3 walks the stream: everything
// that used to be a single set of locals, now one set per independent
// substream id, because a stream carrying I0 and I1 has two of each and
// unioning them would describe a programme that does not exist.
struct ProgrammeScan {
    ScannedProgramme summary{};
    // Table E2.5 locations unioned across the first access unit's substreams
    // (§E3.8.2), which is what gives the RENDERED channel count.
    std::uint16_t locations = 0;
    // Substreams seen so far in the access unit currently open.
    std::size_t substreams = 0;
    std::size_t unit_start = 0;
    bool unit_open = false;
    bool first_unit = true;
};

std::expected<ScannedStream, ScanError> scan_eac3(std::span<const std::byte> stream) {
    ScannedStream out{.kind = StreamKind::kEac3};
    std::size_t offset = 0;
    // Ascending by substreamid, and short: §E2.3.1.2 caps independent
    // substreams at eight, so a linear find beats any keyed container.
    std::vector<ProgrammeScan> programmes;
    // The programme the substreams currently being walked belong to. A
    // dependent joins whichever independent substream last opened a unit -
    // that adjacency IS how §E2.3.1.2 associates the two, since a dependent's
    // own substreamid numbers within its parent's space and says nothing
    // about which parent that is.
    ProgrammeScan* current = nullptr;

    const auto close_unit = [&](ProgrammeScan& p, std::size_t end) {
        if (!p.unit_open || end <= p.unit_start) {
            return;
        }
        p.summary.access_units.push_back(stream.subspan(p.unit_start, end - p.unit_start));
        if (p.first_unit) {
            p.summary.substreams_per_unit = p.substreams;
            p.first_unit = false;
        }
        p.unit_open = false;
    };

    while (offset < stream.size()) {
        if (!sync_at(stream, offset)) {
            return std::unexpected(ScanError::kLostSync);
        }
        const auto sub = read_eac3_substream(stream.subspan(offset));
        if (!sub) {
            return std::unexpected(sub.error());
        }
        if (offset + sub->bytes > stream.size()) {
            return std::unexpected(ScanError::kTruncated);
        }
        if (offset == 0) {
            out.sample_rate = sub->sample_rate;
        }
        // An independent substream begins a new access unit OF ITS OWN
        // PROGRAMME; dependents join the one in progress. Which programme
        // that is comes from substreamid (§E2.3.1.2) - not from position in
        // the stream, which is what made a two-programme stream look like one
        // programme running at twice the frame rate.
        if (sub->strmtyp == static_cast<int>(eac3::StreamType::kIndependent)) {
            // Close whichever programme's unit was open, not this one's: a
            // programme's access unit ends at the next INDEPENDENT substream
            // of ANY programme, because that is where its own dependents stop
            // and someone else's substreams begin. Running it to this
            // programme's own next frame instead would swallow every other
            // programme's frame sitting in between - a span twice the size it
            // should be, which a container would then declare and hand a
            // player whole.
            if (current != nullptr) {
                close_unit(*current, offset);
            }
            auto found = std::ranges::find(programmes, sub->substreamid,
                                           [](const ProgrammeScan& p) {
                                               return p.summary.substreamid;
                                           });
            if (found == programmes.end()) {
                // Inserted in ascending substreamid order rather than
                // appended, so `programmes` reads the same whichever order
                // the stream happens to introduce its programmes in.
                const auto at = std::ranges::lower_bound(
                    programmes, sub->substreamid,
                    std::ranges::less{},
                    [](const ProgrammeScan& p) { return p.summary.substreamid; });
                ProgrammeScan fresh;
                fresh.summary.substreamid = sub->substreamid;
                fresh.summary.acmod = sub->acmod;
                fresh.summary.lfe = sub->lfe;
                fresh.summary.bsid = sub->bsid;
                fresh.summary.bsmod = sub->bsmod;
                fresh.locations = bed_locations(sub->acmod, sub->lfe);
                found = programmes.insert(at, std::move(fresh));
            }
            current = &*found;
            current->unit_start = offset;
            current->unit_open = true;
            current->substreams = 0;
        } else if (current == nullptr) {
            // A dependent ahead of any independent substream has no parent to
            // extend, so there is nothing to attribute it to - the same
            // constraint ac3::split_access_units enforces on the decode side.
            return std::unexpected(ScanError::kOrphanDependent);
        } else if (current->first_unit) {
            // §E3.8.2: a dependent's channels overwrite the bed's where they
            // correspond and extend the layout where they do not, so unioning
            // locations - not adding counts - is what gives the rendered
            // channel count.
            current->locations = static_cast<std::uint16_t>(current->locations | sub->chanmap);
        }
        // TS 103 420 §8.3.1: "whichever substream carries the EMDF
        // container" sets the flag (encoder/eac3_frame.hpp), which this
        // project's own encoder always makes the independent one, but a
        // dependent is legal too - so this checks every substream of the
        // first access unit rather than just the independent one, and takes
        // the first that has it set. Per programme: an object layer belongs
        // to the programme whose substream carries it, not to the stream.
        if (current->first_unit && sub->oba_extension &&
            !current->summary.oba_complexity_index) {
            current->summary.oba_complexity_index = sub->oba_complexity_index;
        }
        ++current->substreams;
        offset += sub->bytes;
    }
    // Only the last programme to open one can still have a unit open - every
    // other was closed the moment the next independent substream arrived - but
    // close_unit already no-ops on a closed one, so this needs no bookkeeping
    // of its own to say which.
    for (auto& p : programmes) {
        close_unit(p, offset);
    }
    if (programmes.empty() || programmes.front().summary.access_units.empty()) {
        return std::unexpected(ScanError::kEmpty);
    }
    out.programmes.reserve(programmes.size());
    for (auto& p : programmes) {
        p.summary.channels = eac3::chanmap::channel_count(p.locations);
        out.programmes.push_back(std::move(p.summary));
    }
    // The scalar summary describes the FIRST programme - see ScannedStream's
    // own comments on access_units and programmes.
    const auto& lead = out.programmes.front();
    out.acmod = lead.acmod;
    out.lfe = lead.lfe;
    out.bsid = lead.bsid;
    out.bsmod = lead.bsmod;
    out.channels = lead.channels;
    out.substreams_per_unit = lead.substreams_per_unit;
    out.oba_complexity_index = lead.oba_complexity_index;
    out.access_units = lead.access_units;
    return out;
}

}  // namespace

std::string_view describe(ScanError error) {
    switch (error) {
        case ScanError::kEmpty:
            return "no frames in stream";
        case ScanError::kLostSync:
            return "lost sync: expected 0x0B77";
        case ScanError::kUnsupportedBsid:
            return "unsupported bsid (expected AC-3 <= 10 or E-AC-3 16)";
        case ScanError::kReservedValue:
            return "reserved value in syncinfo";
        case ScanError::kTruncated:
            return "stream ends mid-frame";
        case ScanError::kOrphanDependent:
            return "dependent substream with no independent substream ahead of it";
    }
    return "unknown error";
}

std::expected<ScannedStream, ScanError> scan(std::span<const std::byte> stream) {
    if (stream.size() < 6) {
        return std::unexpected(ScanError::kEmpty);
    }
    if (!sync_at(stream, 0)) {
        return std::unexpected(ScanError::kLostSync);
    }
    // Both formats spend exactly 40 bits before bsid, which is what lets a
    // reader identify the stream without knowing its kind in advance.
    BitReader probe{stream};
    probe.skip(kBsidBitOffset);
    const auto bsid = static_cast<int>(probe.read(5));
    if (bsid <= kAc3MaxBsid) {
        return scan_ac3(stream);
    }
    if (bsid == eac3::kBsid) {
        return scan_eac3(stream);
    }
    return std::unexpected(ScanError::kUnsupportedBsid);
}

}  // namespace ac3::io
