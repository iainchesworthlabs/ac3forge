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

// syncinfo (§5.4.1) plus the whole of bsi (Table 5.2) up to and including
// Ch2's dual-mono metadata - everything FrameHeader reports for an AC-3
// frame, and no further: the timecode and addbsi fields past it say nothing
// this reader surfaces, and AC-3 has no counterpart to Annex E's object-audio
// addbsi marker (TS 103 420 §8.3.1 is E-AC-3 only).
//
// bsid/bsmod are captured rather than skipped because two separate consumers
// need them off the wire: build_codec_config_box() (ac3/io/dec3.hpp) fills in
// AC3SpecificBox's own bsid/bsmod fields from them (ETSI TS 102 366 Annex F
// §F.4), and `ac3cli probe` reports them directly.
std::expected<FrameHeader, ScanError> read_ac3_header(std::span<const std::byte> at) {
    if (at.size() < 5) {
        return std::unexpected(ScanError::kTruncated);
    }
    FrameHeader h{.kind = StreamKind::kAc3};
    BitReader r{at};
    r.skip(16 + 16);  // syncword, crc1
    const auto fscod = r.read(2);
    const auto frmsizecod = r.read(6);
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(ScanError::kReservedValue);
    }
    h.sample_rate = static_cast<SampleRate>(fscod);
    // Table 5.18: frmsizecod's high bits already index kBitratesKbps, which is
    // exactly what AC3SpecificBox's bit_rate_code reports.
    h.bit_rate_code = static_cast<int>(frmsizecod >> 1);
    h.bitrate_kbps = kBitratesKbps[frmsizecod >> 1];
    const auto bytes = frame_size_bytes(h.sample_rate, h.bitrate_kbps, (frmsizecod & 1) != 0);
    if (!bytes) {
        return std::unexpected(ScanError::kReservedValue);
    }
    h.bytes = *bytes;

    h.bsid = static_cast<int>(r.read(5));
    h.bsmod = static_cast<int>(r.read(3));
    const auto raw = r.read(3);
    h.acmod = static_cast<Acmod>(raw);
    if ((raw & 0x1) && raw != 0x1) {
        r.skip(2);  // cmixlev
    }
    if (raw & 0x4) {
        r.skip(2);  // surmixlev
    }
    if (raw == 0x2) {
        r.skip(2);  // dsurmod
    }
    h.lfe = r.read(1) != 0;
    h.dialnorm = static_cast<int>(r.read(5));
    if (r.read(1) != 0) {  // compre (§5.4.2.9)
        h.compr = static_cast<std::uint8_t>(r.read(8));
    }
    if (r.read(1) != 0) {
        r.skip(8);  // langcode -> langcod
    }
    if (r.read(1) != 0) {
        r.skip(5 + 2);  // audprodie -> mixlevel, roomtyp
    }
    if (h.acmod == Acmod::kDualMono) {
        h.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            h.compr2 = static_cast<std::uint8_t>(r.read(8));
        }
    }
    return r.overflowed() ? std::unexpected(ScanError::kTruncated)
                          : std::expected<FrameHeader, ScanError>{h};
}

// syncinfo (§5.4.1): fscod and frmsizecod share byte 4, and together index
// Table 5.18 for the frame size.
struct Ac3Syncinfo {
    SampleRate sample_rate = SampleRate::k48000;
    std::size_t bytes = 0;
    // Table 5.18's frmsizecod high bits already index kBitratesKbps;
    // AC3SpecificBox's bit_rate_code (ETSI TS 102 366 Annex F §F.4) is
    // exactly that same index.
    int bit_rate_code = 0;
};

std::expected<Ac3Syncinfo, ScanError> read_ac3_syncinfo(std::span<const std::byte> stream,
                                                        std::size_t offset) {
    if (offset + 5 > stream.size()) {
        return std::unexpected(ScanError::kTruncated);
    }
    const auto byte4 = std::to_integer<std::uint32_t>(stream[offset + 4]);
    const auto fscod = byte4 >> 6;
    const auto frmsizecod = byte4 & 0x3F;
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(ScanError::kReservedValue);
    }
    const auto rate = static_cast<SampleRate>(fscod);
    const auto bytes =
        frame_size_bytes(rate, kBitratesKbps[frmsizecod >> 1], (frmsizecod & 1) != 0);
    if (!bytes) {
        return std::unexpected(ScanError::kReservedValue);
    }
    return Ac3Syncinfo{.sample_rate = rate,
                       .bytes = *bytes,
                       .bit_rate_code = static_cast<int>(frmsizecod >> 1)};
}

// --- E-AC-3 ----------------------------------------------------------------

// Table E1.2's fields land straight in the public FrameHeader (see
// elementary.hpp) rather than in a scan-private struct: `ac3cli probe` reports
// every one of them per frame, and scan() below keeps only the first
// programme's own units - two consumers of one walk, not two walks.
// Table E1.2's mixing-metadata payload, walked (not interpreted) purely to
// reach addbsi at the right bit offset - every field here mirrors
// decoder/eac3_decoder.cpp's function of the same name field for field
// (including its comments), which is deliberate: this file re-derives bsi
// independently rather than reusing decoder internals, the same way
// read_eac3_header above already re-derives everything up through
// chanmap on its own. A scan is a much smaller job than a decode and has no
// business depending on the decoder's private Bsi/parse_bsi.
void skip_mixing_metadata(BitReader& r, const FrameHeader& s, int nblks) {
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
    if (s.strmtyp != eac3::StreamType::kDependent) {
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
// notes. bsmod is READ here (not skipped) - see FrameHeader::bsmod.
void skip_informational_metadata(BitReader& r, FrameHeader& s) {
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

std::expected<FrameHeader, ScanError> read_eac3_header(std::span<const std::byte> at) {
    if (at.size() < 6) {
        return std::unexpected(ScanError::kTruncated);
    }
    BitReader r{at};
    r.skip(16);  // syncword
    FrameHeader s{.kind = StreamKind::kEac3};
    const auto strmtyp = r.read(2);
    if (strmtyp == static_cast<std::uint32_t>(eac3::StreamType::kReserved)) {
        return std::unexpected(ScanError::kReservedValue);
    }
    s.strmtyp = static_cast<eac3::StreamType>(strmtyp);
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
        s.reduced_rate = true;
    } else {
        s.sample_rate = static_cast<SampleRate>(fscod);
        s.numblkscod = static_cast<int>(r.read(2));
    }
    const auto acmod = r.read(3);
    s.acmod = static_cast<Acmod>(acmod);
    s.lfe = r.read(1) != 0;
    s.bsid = static_cast<int>(r.read(5));
    s.dialnorm = static_cast<int>(r.read(5));
    // §E3.8.5: in a DEPENDENT substream compre marks the last dependent of the
    // program rather than announcing a compression word - though it still
    // drags one in. Either way the 8 bits have to be consumed; only reported
    // as a compression word where the substream is independent/convertible and
    // the word is actually what it says it is.
    if (r.read(1)) {
        const auto compr = static_cast<std::uint8_t>(r.read(8));
        if (s.strmtyp != eac3::StreamType::kDependent) {
            s.compr = compr;
        }
    }
    if (acmod == 0x0) {
        s.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1)) {  // compr2e
            s.compr2 = static_cast<std::uint8_t>(r.read(8));
        }
    }
    if (s.strmtyp == eac3::StreamType::kDependent) {
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
    if (s.strmtyp == eac3::StreamType::kIndependent && s.numblkscod != 0x3) {
        r.skip(1);  // convsync
    }
    if (s.strmtyp == eac3::StreamType::kConvertible) {
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
    // (§E3.8.2), which is what gives the RENDERED channel count and, for the
    // lead programme, ScannedStream::channel_map.
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
        const auto sub = read_eac3_header(stream.subspan(offset));
        if (!sub) {
            return std::unexpected(sub.error());
        }
        // §E3.8.2: a dependent substream extends the independent one it
        // follows. One that opens the stream has no independent to extend -
        // matches ac3::split_access_units' identical guard in decoder.cpp.
        if (offset == 0 && sub->strmtyp == eac3::StreamType::kDependent) {
            return std::unexpected(ScanError::kUnsupportedStructure);
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
        if (sub->strmtyp == eac3::StreamType::kIndependent) {
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
            return std::unexpected(ScanError::kUnsupportedStructure);
        } else if (current->first_unit) {
            // §E3.8.2: a dependent's channels overwrite the bed's where they
            // correspond and extend the layout where they do not, so unioning
            // locations - not adding counts - is what gives the rendered
            // channel count.
            current->locations =
                static_cast<std::uint16_t>(current->locations | sub->chanmap.value_or(0));
        }
        // TS 103 420 §8.3.1: "whichever substream carries the EMDF
        // container" sets the flag (encoder/eac3_frame.hpp), which this
        // project's own encoder always makes the independent one, but a
        // dependent is legal too - so this checks every substream of the
        // first access unit rather than just the independent one, and takes
        // the first that has it set. Per programme: an object layer belongs
        // to the programme whose substream carries it, not to the stream.
        if (current->first_unit && sub->oba_complexity_index &&
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
    std::uint16_t lead_locations = 0;
    for (std::size_t i = 0; i < programmes.size(); ++i) {
        auto& p = programmes[i];
        p.summary.channels = eac3::chanmap::channel_count(p.locations);
        if (i == 0) {
            lead_locations = p.locations;
        }
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
    out.channel_map = lead_locations;
    return out;
}

// --- AC-3, with or without Annex E extension substreams ---------------------

// §E2.3.1.2: "If an AC-3 bit stream is present in the E-AC-3 bit stream, then
// the AC-3 bit stream shall be processed as an independent substream assigned
// substream ID 0." An AC-3 syncframe therefore opens an access unit exactly
// the way an Annex E independent substream does, and the dependent substreams
// that immediately follow it belong to it - StreamKind::kAc3CoreEac3Extension.
//
// One walk covers both that and plain AC-3, rather than committing to a kind
// on the strength of the first frame and then refusing whatever contradicts
// the guess. A stream where no dependent ever turns up is plain AC-3 and every
// access unit is a lone syncframe, which is exactly what this produces.
//
// Always exactly one programme: §E2.3.1.2 gives this shape no Annex E
// independent substream to number a second one from, so unlike scan_eac3
// there is no grouping to do here - the single-entry `programmes` list is
// only what lets a caller walk it without first asking which kind of stream
// this is.
std::expected<ScannedStream, ScanError> scan_ac3_led(std::span<const std::byte> stream) {
    ScannedStream out{.kind = StreamKind::kAc3};
    std::size_t offset = 0;
    std::size_t unit_start = 0;
    std::size_t substreams = 0;
    std::uint16_t locations = 0;
    bool first_unit = true;

    const auto close_unit = [&](std::size_t end) {
        if (end > unit_start) {
            out.access_units.push_back(stream.subspan(unit_start, end - unit_start));
            if (first_unit) {
                out.substreams_per_unit = substreams;
                first_unit = false;
            }
        }
    };

    while (offset < stream.size()) {
        if (!sync_at(stream, offset) || offset + 6 > stream.size()) {
            return std::unexpected(ScanError::kLostSync);
        }
        BitReader probe{stream.subspan(offset)};
        probe.skip(kBsidBitOffset);
        const auto bsid = static_cast<int>(probe.read(5));

        if (bsid <= kAc3MaxBsid) {
            const auto info = read_ac3_syncinfo(stream, offset);
            if (!info) {
                return std::unexpected(info.error());
            }
            if (offset + info->bytes > stream.size()) {
                return std::unexpected(ScanError::kTruncated);
            }
            close_unit(offset);
            unit_start = offset;
            substreams = 0;
            if (out.access_units.empty()) {
                // The lean read_ac3_syncinfo above is what every frame needs
                // (a size, to step to the next one); the core's first frame
                // additionally sets the stream-level fields, and for those
                // read_ac3_header is the one parse that fills the whole
                // public FrameHeader - the same one `ac3cli probe` reports.
                const auto header = read_ac3_header(stream.subspan(offset));
                if (!header) {
                    return std::unexpected(header.error());
                }
                out.sample_rate = header->sample_rate;
                out.acmod = header->acmod;
                out.lfe = header->lfe;
                out.bsid = header->bsid;
                out.bsmod = header->bsmod;
                out.bit_rate_code = header->bit_rate_code;
                locations = bed_locations(header->acmod, header->lfe);
            }
            ++substreams;
            offset += info->bytes;
            continue;
        }
        if (bsid != eac3::kBsid) {
            return std::unexpected(ScanError::kUnsupportedBsid);
        }

        // Annex E syntax inside an AC-3-led stream. §E2.3.1.2 gives substream
        // ID 0 to the AC-3 frame, so the only thing that legitimately follows
        // it here is one of ITS dependents. An Annex E independent substream
        // would be a second programme (§E3.8.4's mixture), which is a
        // different shape from the one this walk models - recognised and
        // refused rather than folded into the core's access unit, where its
        // channels would be unioned into a layout they have nothing to do
        // with.
        const auto sub = read_eac3_header(stream.subspan(offset));
        if (!sub) {
            return std::unexpected(sub.error());
        }
        if (sub->strmtyp != eac3::StreamType::kDependent) {
            return std::unexpected(ScanError::kUnsupportedStructure);
        }
        if (substreams == 0) {
            // A dependent with no core ahead of it to extend. Unreachable via
            // scan() below, which only comes here when the first frame is
            // AC-3, but the walk should not depend on its caller for that.
            return std::unexpected(ScanError::kUnsupportedStructure);
        }
        if (offset + sub->bytes > stream.size()) {
            return std::unexpected(ScanError::kTruncated);
        }
        out.kind = StreamKind::kAc3CoreEac3Extension;
        if (first_unit) {
            // §E3.8.2, exactly as scan_eac3 unions them: a dependent's
            // channels overwrite the core's where they correspond and extend
            // the layout where they do not.
            locations = static_cast<std::uint16_t>(locations | sub->chanmap.value_or(0));
        }
        // TS 103 420 §8.3.1: the core cannot carry the object-audio marker
        // (addbsi's object-audio use is Annex E only), so in this arrangement
        // it is always a dependent that has it - see scan_eac3's own comment.
        if (first_unit && sub->oba_complexity_index && !out.oba_complexity_index) {
            out.oba_complexity_index = sub->oba_complexity_index;
        }
        ++substreams;
        offset += sub->bytes;
    }
    close_unit(offset);
    if (out.access_units.empty()) {
        return std::unexpected(ScanError::kEmpty);
    }
    out.channels = eac3::chanmap::channel_count(locations);
    out.channel_map = locations;
    // Always one programme - see this function's own comment.
    out.programmes.push_back({.substreamid = 0,
                              .acmod = out.acmod,
                              .lfe = out.lfe,
                              .channels = out.channels,
                              .bsid = out.bsid,
                              .bsmod = out.bsmod,
                              .substreams_per_unit = out.substreams_per_unit,
                              .oba_complexity_index = out.oba_complexity_index,
                              .access_units = out.access_units});
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
        case ScanError::kUnsupportedStructure:
            return "substreams arranged in a way this reader does not model";
    }
    return "unknown error";
}

std::expected<FrameHeader, ScanError> read_frame_header(std::span<const std::byte> at) {
    if (at.size() < 6) {
        return std::unexpected(ScanError::kTruncated);
    }
    if (!sync_at(at, 0)) {
        return std::unexpected(ScanError::kLostSync);
    }
    // Both formats spend exactly 40 bits before bsid, deliberately, so which
    // reading of everything before it was correct can be settled after the
    // fact - see this file's own header comment.
    BitReader r{at};
    r.skip(kBsidBitOffset);
    const auto bsid = static_cast<int>(r.read(5));
    if (bsid <= kAc3MaxBsid) {
        return read_ac3_header(at);
    }
    if (bsid == eac3::kBsid) {
        return read_eac3_header(at);
    }
    return std::unexpected(ScanError::kUnsupportedBsid);
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
        // Plain AC-3 and §E2.3.1.2's legacy-core delivery are the same walk;
        // which one this is falls out of whether any Annex E dependent
        // actually turns up (scan_ac3_led).
        return scan_ac3_led(stream);
    }
    if (bsid == eac3::kBsid) {
        return scan_eac3(stream);
    }
    return std::unexpected(ScanError::kUnsupportedBsid);
}

}  // namespace ac3::io
