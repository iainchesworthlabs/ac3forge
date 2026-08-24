#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/bitalloc.hpp"     // DeltaSegments
#include "ac3/core/eac3_tables.hpp"  // StreamType
#include "ac3/core/tables.hpp"       // kBlocksPerFrame
#include "ac3/export.hpp"

// E-AC-3 encoder/decoder mirror verification - Annex E's counterpart to
// ac3/verify/mirror.hpp, which this deliberately parallels rather than
// extends.
//
// The reasoning is the AC-3 one, only more so. An encoder carries a model of
// the decoder it writes for, every mantissa field's WIDTH comes out of that
// model, and the moment the two copies disagree the shared bit stream stops
// being parseable somewhere downstream of the field that actually diverged.
// What Annex E changes is how much model there is and how little else checks
// it:
//
//   - There is strictly more of it. Exponent strategies and coupling-in-use
//     are hoisted to a frame-level audfrm element, the adaptive hybrid
//     transform packs a whole frame of one stream's mantissas into block 0
//     under its own allocation table and gain-adaptive quantizer, spectral
//     extension replaces a band with per-band scale factors, enhanced
//     coupling replaces a per-band scale factor with an amplitude/angle/
//     chaos triple, and a program can be split across an independent
//     substream and its dependents.
//   - There is less to check it against. docs/verification.md's own table:
//     FFmpeg reads no enhanced coupling and no transient pre-noise syntax at
//     all, refuses a second dependent substream (so 7.1.4 has no external
//     oracle), and refuses fscod2 audio - as, on the same streams, does
//     Dolby's Reference Player. For those the in-repo round trip is the only
//     check there is, and a round trip cannot see a misreading of the spec
//     that BOTH sides share.
//
// That last case is what this closes. A shared misreading produces identical
// audio on both sides and an identical bit layout, so no round trip and no
// SNR gate can see it - but the two sides reach that layout through
// independent code, and comparing the intermediate model is what makes the
// agreement mean something. Where they part company for any other reason,
// the per-block bit offset localises it exactly as it does for AC-3.
//
// Nothing here is on by default: a trace is written only where an
// eac3::FrameConfig/DecoderConfig carries a non-null pointer to one, so an
// ordinary encode or decode pays one null test per block and no allocation.
// See ac3/verify/eac3_selfcheck.hpp for the encode-then-decode-then-compare
// driver most callers actually want.

namespace ac3::verify {

// One coded stream's state within one block. Streams are numbered the way
// the ENCODER numbers them and the way ac3/verify/mirror.hpp already does:
// the full-bandwidth channels, then the LFE, then - when coupling is in use -
// the coupling channel as one more stream at index `coded_channels`. The
// decoder parks its coupling stream at a fixed internal slot instead; its
// trace-filling side maps that onto this numbering, so the two traces index
// identically whatever each side does internally.
struct Eac3StreamTrace {
    // The DECODED exponents, indexed from bin 0 (bins below `start` are
    // filler on both sides). Not the grouped/coded form: what is being
    // compared is the model, and two sides agreeing on the wire bytes but
    // disagreeing on what they mean is exactly the failure mode of interest.
    std::vector<std::uint8_t> exponents;
    // The bit allocation pointers derived from those exponents, same
    // indexing. For an AHT stream (`aht` below) these are §E3.4.3's hebap,
    // read out of the high-efficiency table - a different quantity from the
    // ordinary bap, and the one that actually sizes that stream's fields.
    std::vector<std::uint8_t> bap;
    // §7.2.2.6: the delta correction IN FORCE for this block's allocation,
    // not the deltbae code on the wire. Always default-constructed for the
    // LFE, which has no delta bit allocation field at all (§5.4.3.49).
    DeltaSegments delta;
    int start = 0;    // first coded bin: cplstrtmant for the coupling stream, else 0
    int endmant = 0;  // one past the last coded bin
    // §E3.4. Set per block, since ahtinu is a frame-level decision that
    // applies to every one of them.
    bool aht = false;
    // §E3.4.4.2's gain mode and its per-bin gain (1, 2 or 4, indexed from bin
    // 0 like `exponents`), recorded in BLOCK 0 ONLY on both sides: the gain
    // words are transmitted once per frame, inside block 0's mantissa
    // element, so no other block is one where a decoder has read them. Zero
    // and empty everywhere else, and equally when `aht` is clear.
    //
    // The gain is the one AHT quantity that is neither transmitted plainly
    // nor derivable from the allocation alone - the encoder chooses it from
    // the mantissa it is about to write and the decoder recovers it from the
    // gain words - so a divergence here is invisible everywhere else until
    // the mantissas come out wrong.
    int gaqmod = 0;
    std::vector<std::uint8_t> gain;
};

// One full-bandwidth channel's per-block tool state. Separate from
// Eac3StreamTrace because these are per CHANNEL, not per coded stream: the
// coupling stream and the LFE have none of them.
struct Eac3ChannelTrace {
    bool blksw = false;
    // §E3.3 / §E3.5. Standard and enhanced coupling are mutually exclusive per
    // block, so at most one of `cplco` and the ecpl* triple below carries
    // anything.
    bool in_coupling = false;
    // The DECODED coordinate per coupled sub-band, indexed from the first
    // coupled sub-band - the form the decoder actually multiplies by, after
    // the band-to-sub-band expansion, so a disagreement about the band
    // structure shows up here as well as a disagreement about a coordinate.
    std::vector<double> cplco;
    // §E3.5.4's raw transmitted indices per coupling band. Kept as indices
    // rather than decoded values because decoding them depends on the
    // channel's role in the block (is-first-coupled, ecpltrans), and the
    // indices are what both sides put on and take off the wire.
    std::vector<int> ecplamp;
    std::vector<int> ecplangle;
    std::vector<int> ecplchaos;
    bool ecpltrans = false;
    // §E3.6. Coordinates are one per BAND here (no sub-band expansion step),
    // decoded, matching how both sides store them.
    bool in_spx = false;
    std::vector<double> spxco;
    int spxblnd = 0;
};

struct Eac3BlockTrace {
    // Set when this side reached the block at all. The decoder can stop
    // early - a desynced stream usually gets refused before the last block -
    // and a trace that simply ends is itself a finding, so absence is
    // recorded rather than inferred.
    bool entered = false;
    // Set when this side got as far as computing the block's bit allocation,
    // i.e. `streams` below is populated. Between `entered` and `allocated`
    // sits the whole of the block's side information, which is where a
    // refusing decoder tends to stop.
    bool allocated = false;
    // Bits consumed/emitted from the start of the syncframe at the moment
    // this block's audblk() begins - BitWriter::bit_count() on the encoder,
    // BitReader::bit_position() on the decoder. The localiser: any
    // disagreement about any field's width, from any cause, lands here at
    // the next block boundary.
    std::size_t bit_offset = 0;
    bool deltbaie = false;
    bool cplinu = false;
    bool ecplinu = false;
    int cplstrtmant = 0;
    int cplendmant = 0;
    bool spxinu = false;
    int spx_startmant = 0;
    int spx_endmant = 0;
    int spx_copystart = 0;
    std::vector<Eac3StreamTrace> streams;
    std::vector<Eac3ChannelTrace> channels;  // full-bandwidth channels only
};

// One side's view of one substream's syncframe.
struct Eac3SubstreamTrace {
    eac3::StreamType strmtyp = eac3::StreamType::kIndependent;
    int substreamid = 0;
    int blocks_coded = kBlocksPerFrame;  // blocks_per_syncframe(numblkscod)
    int fbw_channels = 0;                // nfchans
    int coded_channels = 0;              // nfchans + lfe; the coupling stream sits here
    // §3.7, frame-level. `chintransproc`/`transprocloc`/`transproclen` are
    // sized to fbw_channels whenever `transproce` is set and empty otherwise.
    // Locations and lengths are in SAMPLES on both sides (transprocloc's
    // 4-sample wire resolution is multiplied out at parse and divided back
    // at emit), so they compare directly.
    bool transproce = false;
    std::vector<bool> chintransproc;
    std::vector<int> transprocloc;
    std::vector<int> transproclen;
    std::array<Eac3BlockTrace, kBlocksPerFrame> blocks{};

    // Returns the trace to its unvisited state, keeping whatever capacity the
    // vectors already hold so a per-frame loop does not reallocate.
    void reset();
};

// One side's view of a whole access unit: the independent substream followed
// by its dependents, in transmission order.
//
// The two sides fill one of these differently, which is why it has two ways
// in. An encoder knows its substream count from its config before it encodes
// anything, so it sizes the trace once and each substream's FrameEncoder
// writes its own slot - resize() then substream(). A decoder learns the shape
// one syncframe at a time, so it appends as it goes - begin_substream(),
// whose `independent` flag delimits access units by the same rule
// split_access_units uses.
//
// Slots are retained, not freed, when an access unit ends: a caller stepping
// through a whole stream reuses one trace, and the per-substream vectors are
// the only allocation this facility makes at all.
class AC3FORGE_EXPORT Eac3AccessUnitTrace {
   public:
    // The substreams of the access unit currently traced, in transmission
    // order.
    [[nodiscard]] std::span<const Eac3SubstreamTrace> substreams() const;
    [[nodiscard]] std::size_t size() const { return used_; }

    // Sizes the trace to a known substream count and resets every slot. The
    // references substream() returns stay valid until the next resize() that
    // asks for more slots than this one did, which is what lets an encoder
    // hand each of its FrameEncoders a pointer to its own slot up front.
    void resize(std::size_t count);
    [[nodiscard]] Eac3SubstreamTrace& substream(std::size_t index);

    // The slot for a substream about to be decoded. `independent` starts a
    // new access unit, dropping whatever the previous one left here; anything
    // else appends to it. The returned reference is reset() and stays valid
    // until the next call.
    [[nodiscard]] Eac3SubstreamTrace& begin_substream(bool independent);

   private:
    // Sized to the high-water mark rather than the current unit, so slots and
    // their vectors survive to be reused - see the class comment.
    std::vector<Eac3SubstreamTrace> storage_;
    std::size_t used_ = 0;
};

enum class Eac3Field : std::uint8_t {
    // --- access unit and substream identity --------------------------------
    kSubstreamCount,  // the two units' shapes differ; nothing below lines up
    kStreamType,
    kSubstreamId,
    kBlockCount,  // numblkscod, as a block count
    kTransientProcInUse,
    kTransientProcChannel,   // chintransproc[ch]
    kTransientProcLocation,  // transprocloc[ch], samples
    kTransientProcLength,    // transproclen[ch], samples
    // --- block level -------------------------------------------------------
    kBlockReached,       // one side stopped before the other
    kBitOffset,          // the localiser: block-boundary bit position
    kAllocationReached,  // one side computed a bit allocation, the other did not
    kStreamCount,        // e.g. a disagreement about cplinu
    kChannelCount,
    kDeltbaie,
    kCouplingInUse,
    kEnhancedCouplingInUse,
    kCouplingStart,  // cplstrtmant
    kCouplingEnd,    // cplendmant
    kSpxInUse,
    kSpxStart,      // the first synthesized coefficient
    kSpxEnd,        // one past the last synthesized coefficient
    kSpxCopyStart,  // where the copy-up source region begins
    // --- per coded stream --------------------------------------------------
    kStreamStart,
    kStreamEnd,
    kExponentCount,  // the coded bandwidth itself differs
    kExponent,
    kBapCount,
    kBap,  // a bit allocation pointer - the value that sizes a mantissa field
    kDeltaSegmentCount,
    kDeltaOffset,
    kDeltaLength,
    kDeltaValue,
    kAhtInUse,
    kGaqMode,
    kAhtGainCount,
    kAhtGain,
    // --- per full-bandwidth channel ----------------------------------------
    kBlockSwitch,
    kChannelInCoupling,
    kCouplingCoordinateCount,
    kCouplingCoordinate,
    kEcplAmplitude,
    kEcplAngle,
    kEcplChaos,
    kEcplTransient,
    kEcplCoordinateCount,
    kChannelInSpx,
    kSpxCoordinateCount,
    kSpxCoordinate,
    kSpxBlend,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(Eac3Field field);

struct Eac3Mismatch {
    std::uint64_t frame = 0;
    int substream = -1;  // index within the access unit, -1 for a unit-level field
    int block = -1;
    // A coded stream index for the kExponent/kBap/kDelta*/kAht* fields, a
    // full-bandwidth channel index for the per-channel ones, -1 for a
    // block-level or substream-level field. `channel` says which numbering
    // applies, since the two overlap at the low indices.
    int stream = -1;
    bool channel = false;
    // A bin index for kExponent/kBap/kAhtGain, a band or sub-band index for
    // the coordinate fields, a segment index for the kDelta* fields, a
    // channel index for the kTransientProc* ones, -1 where not indexed.
    int index = -1;
    Eac3Field field = Eac3Field::kBitOffset;
    // Wide enough for every field here: the coordinates are the only
    // non-integral ones, and every integral value in play is exact in a
    // double.
    double encoder = 0.0;
    double decoder = 0.0;
};

// "frame 12 substream 1 block 3 channel 1: bap[87] encoder=5 decoder=4", with
// the stream named the way a reader of Annex E would name it rather than by
// its internal index.
[[nodiscard]] AC3FORGE_EXPORT std::string describe(const Eac3Mismatch& mismatch,
                                                   int fbw_channels, int coded_channels);

// A single stream's exponent, bap or coordinate array can disagree in
// hundreds of places at once; at most this many are reported per stream per
// array, since the first few plus the block and stream identity is what a
// reader needs and the rest is the same finding repeated.
inline constexpr int kEac3MaxPerArray = 4;

// Diffs one substream's two views and returns what they disagree about, most
// useful first.
//
// Reporting stops at the END of the first block that disagrees, for the same
// reason ac3/verify/mirror.hpp's compare() does: once the two sides are
// reading different bits every later block differs as a consequence, and a
// report listing all of them buries the one line that names the cause.
// Within that block the block-level fields come first (the bit offset above
// all), then the per-stream detail, then the per-channel tool state.
[[nodiscard]] AC3FORGE_EXPORT std::vector<Eac3Mismatch> compare(
    const Eac3SubstreamTrace& encoder, const Eac3SubstreamTrace& decoder,
    std::uint64_t frame_index, int substream_index);

// The access-unit form: substream by substream, stopping at the first
// substream that disagrees. A count mismatch is reported on its own, since
// nothing below it lines up.
[[nodiscard]] AC3FORGE_EXPORT std::vector<Eac3Mismatch> compare(
    const Eac3AccessUnitTrace& encoder, const Eac3AccessUnitTrace& decoder,
    std::uint64_t frame_index);

// The whole set as one multi-line block, one mismatch per line, for a test
// failure message or a diagnostic dump. Stream names are resolved from the
// substream each mismatch names, where `shape` has one. Empty string for an
// empty span.
[[nodiscard]] AC3FORGE_EXPORT std::string report(std::span<const Eac3Mismatch> mismatches,
                                                 const Eac3AccessUnitTrace& shape);

}  // namespace ac3::verify
