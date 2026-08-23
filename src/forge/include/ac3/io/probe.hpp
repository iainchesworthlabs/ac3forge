#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/export.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/oba/oamd.hpp"

// What a stream IS, as opposed to what it sounds like.
//
// Everything else in this project that reads an elementary stream reads it in
// order to do something with the audio: decode it, measure it, wrap it in a
// container. This reads it in order to describe it - the question a pipeline,
// a bug report or a CI gate asks first, and the one nothing here could answer
// without decoding the whole file and inferring backwards from the result.
//
// Two passes' worth of information, taken in one walk:
//
//   - the header tier, from io::read_frame_header. Bounded, cheap and
//     independent of the decoder, so a syncframe whose AUDIO is unreadable
//     still reports its bsid, layout, rates and substream identity truthfully.
//     A CRC check rides alongside it for the same reason.
//   - the parse tier, from the real decoders run with
//     DecoderConfig::skip_reconstruction. This is what dynrng words, EMDF
//     payload ids, the OAMD/JOC object layer and the per-block tool usage
//     come from, and it costs the parse but not the transform.
//
// A frame the parse tier refuses is counted, reported and walked past - the
// header tier's answers for it stand. That is deliberate: an inspection tool
// is at its most useful on exactly the stream that does not decode.
//
// Memory is flat in the length of the stream. The report holds counters,
// ranges and one entry per substream IDENTITY - never per frame - and
// per-frame detail is handed to a callback as the walk reaches it rather than
// accumulated. AccessUnitReader below extends that to the input side, so a
// caller need not hold the file either.

namespace ac3::io {

// A field's observed extent over a whole stream. `seen` distinguishes "never
// carried" from "carried, and happened to be zero" - the same distinction
// std::optional draws on the single-frame fields this aggregates.
struct MinMax {
    bool seen = false;
    int min = 0;
    int max = 0;

    void add(int value) {
        if (!seen) {
            seen = true;
            min = value;
            max = value;
            return;
        }
        min = value < min ? value : min;
        max = value > max ? value : max;
    }

    [[nodiscard]] bool constant() const { return seen && min == max; }
};

// How often each coding tool was actually used, counted over every block of
// every syncframe the parse tier reached.
//
// Counts rather than flags: "coupling in 3 of 1440 blocks" and "coupling in
// every block" are different streams, and a bug report that says which is
// worth more than one that says "coupling: yes".
struct ProbeToolUsage {
    std::uint64_t blocks = 0;        // blocks the parse tier reached at all
    std::uint64_t block_switch = 0;  // blocks where any channel used the short transform
    std::uint64_t dither = 0;        // blocks where any channel had dithflag set
    std::uint64_t coupling = 0;
    std::uint64_t enhanced_coupling = 0;
    std::uint64_t spectral_extension = 0;
    std::uint64_t rematrixing = 0;
    std::uint64_t delta_bit_alloc = 0;
    std::uint64_t skip_field = 0;
    // Frame-level Annex E tools, counted in syncframes rather than blocks
    // because that is the granularity they are decided at (Table E1.3).
    std::uint64_t aht_frames = 0;
    std::uint64_t transient_prenoise_frames = 0;
    // Every coded stream's exponent strategy, summed over blocks: indexed by
    // ExpStrategy (reuse, D15, D25, D45). A stream a block does not code
    // contributes nothing.
    std::array<std::uint64_t, 4> exp_strategy{};
};

// One substream IDENTITY - the (strmtyp, substreamid) pair §E2.3.1.2 makes
// the real name of a substream - together with what it declared and how many
// syncframes carried it. AC-3 has exactly one of these.
struct ProbeSubstream {
    eac3::StreamType strmtyp = eac3::StreamType::kIndependent;
    int substreamid = 0;
    int bsid = 0;
    int bsmod = 0;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int numblkscod = 3;
    std::optional<std::uint16_t> chanmap = std::nullopt;
    std::uint64_t syncframes = 0;
};

// One syncframe's detail, for the per-frame dump. Handed to the callback and
// then reused, so nothing here outlives the call.
struct ProbeSyncframe {
    std::uint64_t byte_offset = 0;
    FrameHeader header{};
    // §5.4.1.2/§E2.3.4: crc1 and crc2 for AC-3, crc2 alone for E-AC-3.
    // Checked here rather than left to the decoder, which refuses a bad frame
    // outright - a probe reports it and keeps walking.
    bool crc_valid = false;
    // Set where the parse tier declined the frame; every field below it is
    // then whatever was reached before the refusal.
    std::optional<DecodeError> parse_error = std::nullopt;
    // Whether an authenticity tag is present - see ProbeOptions::authenticity.
    bool authenticity_tag = false;
    FrameSyntax syntax{};
    // §7.7.1.2's effective word per block, persistence already resolved.
    std::array<std::uint8_t, kBlocksPerFrame> dynrng{};
    // The object layer this syncframe carried, when it carried one.
    std::optional<oba::DecodedProgram> objects = std::nullopt;
};

// One access unit: an AC-3 syncframe, or an E-AC-3 independent substream
// together with the dependents that follow it.
struct ProbeAccessUnit {
    std::uint64_t index = 0;
    std::uint64_t byte_offset = 0;
    std::size_t bytes = 0;
    // Where this unit starts in the programme, from the units before it and
    // their own block counts - not from a container timestamp, which a bare
    // elementary stream does not have.
    double start_seconds = 0.0;
    std::vector<ProbeSyncframe> syncframes;
};

// Everything the walk concluded. Fixed size in the length of the stream,
// except `substreams`, which has one entry per identity (at most 8 dependents
// plus an independent, per §E2.3.1.2).
struct ProbeReport {
    // --- identity ----------------------------------------------------------
    StreamKind kind = StreamKind::kAc3;
    int bsid = 0;
    int bsmod = 0;
    SampleRate sample_rate = SampleRate::k48000;
    // §E2.3.1.3: the rate came from fscod2 (24 / 22.05 / 16 kHz).
    bool reduced_rate = false;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int numblkscod = 3;
    // What the independent substream itself codes, and what the program
    // RENDERS once every dependent's chanmap is unioned in (§E3.8.2). Equal
    // for AC-3 and for a lone E-AC-3 substream.
    int coded_channels = 0;
    int rendered_channels = 0;
    // The Table E2.5 locations those rendered channels occupy. Empty for
    // 1+1 dual mono, which has no spatial layout at all - Ch1 and Ch2 are
    // unrelated programmes, not directions.
    eac3::chanmap::Layout layout{};
    std::vector<ProbeSubstream> substreams;
    std::size_t substreams_per_unit = 0;

    // --- extent ------------------------------------------------------------
    std::uint64_t access_units = 0;
    std::uint64_t syncframes = 0;
    std::uint64_t bytes = 0;
    double duration_seconds = 0.0;
    // Measured over the whole stream: bytes * 8 / duration. This is the rate
    // the stream actually costs, which for E-AC-3 is the only rate there is.
    double bitrate_kbps = 0.0;
    // AC-3's Table 5.18 rate, which is declared rather than measured and so
    // exists only where frmsizecod does. std::nullopt for E-AC-3.
    std::optional<std::uint32_t> nominal_bitrate_kbps = std::nullopt;
    std::size_t min_access_unit_bytes = 0;
    std::size_t max_access_unit_bytes = 0;
    // Access units differ in size. AC-3 at a fixed frmsizecod alternates
    // between two sizes at 44.1 kHz by design (§5.4.1.2's half-word), so this
    // is reported alongside the min/max rather than instead of them.
    bool variable_bitrate = false;

    // --- metadata ----------------------------------------------------------
    // dialnorm is mandatory, so its range is always `seen`. compr and dynrng
    // are not: `seen` false means no syncframe in the stream carried one.
    // dynrng's range is over the EFFECTIVE per-block words (persistence
    // resolved), and counts as unseen where every one of them was unity.
    MinMax dialnorm;
    MinMax dialnorm2;
    MinMax compr;
    MinMax compr2;
    MinMax dynrng;
    MinMax dynrng2;

    // --- object audio ------------------------------------------------------
    // TS 103 420 §8.3.2.2, straight off addbsi - readable without decoding
    // the EMDF container, and what a dec3 box's Atmos extension echoes.
    std::optional<int> oba_complexity_index = std::nullopt;
    // §H.2.2: every EMDF payload id seen anywhere in the stream, ascending
    // and deduplicated (11 is OAMD, 14 is JOC).
    std::vector<int> emdf_payload_ids;
    bool oamd = false;
    bool joc = false;
    // The program the first OAMD payload described: its bed configuration and
    // dynamic object count. std::nullopt where no OAMD parsed.
    std::optional<oba::Program> program = std::nullopt;
    std::uint64_t object_frames = 0;
    // Frames carrying a non-zero authenticity tag - see
    // ProbeOptions::authenticity. Always 0 when no probe was supplied.
    std::uint64_t authenticity_tagged_frames = 0;

    // --- integrity ---------------------------------------------------------
    std::uint64_t crc_failures = 0;
    std::uint64_t parse_failures = 0;
    // The first parse refusal's reason, for a report that has room for one
    // line about why - the rest are counted.
    std::optional<DecodeError> first_parse_error = std::nullopt;

    ProbeToolUsage tools;
};

// What to do with each access unit as the walk reaches it. The reference is
// valid for the call only.
using ProbeAccessUnitSink = std::function<void(const ProbeAccessUnit&)>;

struct ProbeOptions {
    // Fill in each syncframe's per-block syntax and object layer, and call
    // `on_access_unit` for every unit. Off by default: the summary needs
    // neither, and a dump of a long file is a lot of output nobody asked for.
    bool detail = false;
    ProbeAccessUnitSink on_access_unit;
    // Whether a syncframe carries an authenticity tag. Supplied by the caller
    // rather than called directly because signing lives in its own library
    // (ac3::signing, which this one does not and should not link) - pass
    // ac3::signing::has_authenticity_tag here. Unset means the question is
    // not asked and every frame reports untagged.
    std::function<bool(std::span<const std::byte>)> authenticity;
};

// The walk, fed one access unit at a time.
//
// A caller that already holds the whole stream should use probe() below; this
// exists for the one that does not want to - see AccessUnitReader.
class AC3FORGE_EXPORT Prober {
   public:
    explicit Prober(ProbeOptions options = {});
    ~Prober();
    Prober(const Prober&) = delete;
    Prober& operator=(const Prober&) = delete;
    Prober(Prober&&) noexcept;
    Prober& operator=(Prober&&) noexcept;

    // One access unit, in stream order: an AC-3 syncframe, or an E-AC-3
    // independent substream plus its dependents, exactly as
    // split_access_units and AccessUnitReader delimit them. Fails only where
    // the unit's own framing does not hold together - a frame that merely
    // does not DECODE is recorded, not rejected.
    [[nodiscard]] std::expected<void, ScanError> push(std::span<const std::byte> unit);

    // The report so far. Complete once every unit has been pushed; the
    // derived figures (duration, bit rate) are recomputed on each call, so
    // this is meaningful at any point.
    [[nodiscard]] ProbeReport report() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The whole-buffer form, for a caller that already holds the stream.
[[nodiscard]] AC3FORGE_EXPORT std::expected<ProbeReport, ScanError> probe(
    std::span<const std::byte> stream, const ProbeOptions& options = {});

// Access units off an istream, without holding the stream.
//
// The boundary rule is split_access_units': a new unit begins at each AC-3
// syncframe or each E-AC-3 INDEPENDENT substream, and dependents join the one
// in progress. What differs is the memory: this keeps a fixed read window
// plus the unit being assembled - never the file - so probing a two-hour
// stream costs the same as probing a two-second one.
//
// It also differs in how it decides. split_access_units reads strmtyp at a
// fixed byte offset, which only means strmtyp in a genuine Annex E frame - in
// an AC-3 one those bits belong to crc1, so its grouping of a stream whose
// leading substream is not bsid 16 depends on a checksum (see
// apps/android/.../file_replay.cpp's group_by_bsid, which documents a real
// commercial disc where that mis-grouped 176 of 480 access units). This goes
// through read_frame_header, which settles the generation from bsid at bit 40
// first - the deterministic route that comment recommends - and so groups such
// a stream correctly. Callers that only ever hand it E-AC-3 will see no
// difference.
class AC3FORGE_EXPORT AccessUnitReader {
   public:
    explicit AccessUnitReader(std::istream& in);
    ~AccessUnitReader();
    AccessUnitReader(const AccessUnitReader&) = delete;
    AccessUnitReader& operator=(const AccessUnitReader&) = delete;

    // The next access unit, or an empty span once the stream is exhausted.
    // The span is valid until the next call.
    [[nodiscard]] std::expected<std::span<const std::byte>, ScanError> next();

    // Where in the stream the unit just returned began.
    [[nodiscard]] std::uint64_t byte_offset() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::io
