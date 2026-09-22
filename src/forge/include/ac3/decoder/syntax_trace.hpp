#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ac3/core/tables.hpp"

// What a syncframe's audio blocks actually said, as opposed to what they
// decoded to.
//
// A decoder's ordinary output answers "what does this stream sound like".
// A bug report needs the other question - which coding tools were in force,
// block by block, and what exponent strategy each stream was carrying - and
// that information is destroyed the moment the mantissas are turned into
// coefficients. Nothing in DecodedFrame/DecodedSubstream survives to say
// whether block 3 used spectral extension or why channel 1's exponents were
// reused.
//
// So the decoders record it here on the way past, into a caller-owned trace,
// on exactly the terms verify::FrameTrace already established: a null pointer
// in the DecoderConfig means not one byte of this is written, and a non-null
// one costs a branch per block. This is the syntax counterpart of that
// header - mirror.hpp compares an encoder's model against a decoder's to
// localise a desync, this simply reports what the wire said - and the two are
// deliberately separate types, because a trace kept for a whole file's worth
// of frames wants none of mirror.hpp's per-bin exponent/bap vectors.
//
// `ac3cli probe`'s per-block dump is built on this; see ac3/io/probe.hpp.

namespace ac3 {

// Stream slots in a syntax trace, matching the numbering both decoders
// already use internally: the full-bandwidth channels first (0 ..
// nfchans - 1), then the LFE, then - separately, at a fixed slot past every
// real channel - the shared coupling channel. A substream codes at most 3/2
// plus LFE (Table 5.8), so six real slots is the ceiling for both.
inline constexpr int kMaxSyntaxFullbw = 5;
inline constexpr int kMaxSyntaxChannels = 6;
inline constexpr int kCouplingSyntaxStream = kMaxSyntaxChannels;
inline constexpr int kMaxSyntaxStreams = kMaxSyntaxChannels + 1;

// The coding tools one audio block used. Every field is a transmitted flag or
// an immediate consequence of one - nothing here is inferred from the audio.
struct BlockSyntax {
    // Set once this block's audblk was reached at all. A frame the decoder
    // ends up refusing still leaves behind everything it read before the
    // refusal, which is the case a dump is most useful in, so absence is
    // recorded rather than inferred.
    bool entered = false;
    // §5.4.3.1/§E2.3.3: one bit per full-bandwidth channel, set where that
    // channel used the short (block-switched) transform. A bitmask rather
    // than an array of bool because a dump prints it as a set and a stream
    // where nothing switched should cost nothing to say so.
    std::uint8_t block_switch = 0;
    // §5.4.3.2/§7.3.4: dithflag, same per-channel bitmask shape.
    std::uint8_t dither = 0;
    // §5.3.3/§E2.3.3: cplinu - the coupling strategy in force for this block,
    // whether resent here or carried over from an earlier one.
    bool coupling = false;
    // §E2.3.3.16/§3.5: ecplinu. Only ever set alongside `coupling`; E-AC-3
    // only, since AC-3 has no enhanced coupling at all.
    bool enhanced_coupling = false;
    // §E2.3.3/§3.6: spxinu. E-AC-3 only.
    bool spectral_extension = false;
    // §5.4.3.9/§7.4.6: rematstr with at least one band rematrixed. 2/0 only.
    bool rematrixing = false;
    // §5.4.3.47/§7.2.2.6: the delta bit allocation in force for this block,
    // whether this block transmitted it or inherited it - the distinction
    // deltbaie == 0 means RETAIN, not "none", which is its own long story
    // (see verify/mirror.hpp).
    bool delta_bit_alloc = false;
    // §5.4.3.55: a skip field was present, and how many bytes it declared.
    bool skip_field = false;
    std::uint16_t skip_bytes = 0;
    // The exponent strategy each stream carried, indexed as described above -
    // full-bandwidth channels, then the LFE, then kCouplingSyntaxStream. A
    // stream this block does not code (the coupling slot with cplinu clear,
    // the LFE slot with lfeon clear) reports kReuse, which is also what a
    // block that genuinely reuses reports; `entered` plus the frame's own
    // channel counts say which slots are real.
    std::array<ExpStrategy, kMaxSyntaxStreams> exp_strategy{};
};

// EMDF payload ids are 5-bit (§H.2.2.2.2) and a real object-audio container
// carries two of them (OAMD and JOC). A fixed, small cap keeps a per-frame
// trace allocation-free; a container with more payloads than this reports the
// first kMaxSyntaxPayloads and sets `emdf_payloads_truncated`, so a dump never
// silently claims to have listed them all.
inline constexpr int kMaxSyntaxPayloads = 8;

// One syncframe's worth. Fixed size throughout and default-constructible, so a
// caller walking a whole file reuses one of these and allocates nothing.
struct FrameSyntax {
    // Set once bsi parsed and the frame-level fields below are meaningful.
    bool valid = false;
    int fbw_channels = 0;  // nfchans
    bool lfe = false;
    // §E2.3.1.4: blocks actually coded in this syncframe. Always 6 for AC-3.
    int block_count = kBlocksPerFrame;

    // --- frame-level tool gates (Table E1.3; AC-3 leaves these clear) -------
    // §E2.2.3: which streams are AHT-coded, indexed as BlockSyntax's
    // exp_strategy is. Frame-constant, because AHT needs one exponent set for
    // the whole frame.
    std::array<bool, kMaxSyntaxStreams> aht_stream{};
    // §3.7: transproce, and the per-channel chintransproc under it.
    bool transient_prenoise = false;
    std::uint8_t transient_prenoise_channels = 0;
    // The remaining Table E1.3 frame gates, each of which decides whether a
    // per-block field exists at all - a dump that reports "no delta bit
    // allocation anywhere" is saying something different depending on whether
    // dbaflde was clear or every block simply declined.
    bool block_switch_enabled = false;   // blkswe
    bool dither_enabled = false;         // dithflage
    bool bamode = false;                 // bamode
    bool delta_bit_alloc_enabled = false;  // dbaflde
    bool skip_enabled = false;           // skipflde
    bool spx_attenuation_enabled = false;  // spxattene
    // §E2.3.2.13: snroffststr, which decides how many SNR offsets the frame
    // carries (0 = one for everything).
    int snroffststr = 0;
    // §E2.3.2.7: expstre - per-block exponent strategies were transmitted
    // individually rather than hoisted into Table E2.10's frame codes. Always
    // true for AC-3 and for any syncframe shorter than six blocks.
    bool per_block_exp_strategy = true;

    std::array<BlockSyntax, kBlocksPerFrame> blocks{};

    // §H.2.2: the payload ids the frame's EMDF container declared, in the
    // order they appear in it. Empty for a frame with no EMDF at all, which
    // is every ordinary AC-3/E-AC-3 frame. This is the FIRST container the
    // decoder found - it stops looking through later blocks' skip fields once
    // one has yielded an object-audio payload, exactly as it does for
    // DecodedSubstream::object_metadata, and for the same reason: which block
    // carries the container is not fixed, but a frame only ever has one.
    std::array<std::uint8_t, kMaxSyntaxPayloads> emdf_payload_ids{};
    int emdf_payload_count = 0;
    bool emdf_payloads_truncated = false;

    // Returns the trace to its unvisited state. Called by the decoders before
    // their first early return, for the same reason verify::FrameTrace::reset
    // is: a caller reusing one trace across a file must never read a previous
    // frame's state out of a call that decoded nothing.
    void reset() { *this = FrameSyntax{}; }

    // Records one EMDF payload id, honouring the cap above.
    void add_emdf_payload(int id) {
        if (emdf_payload_count >= kMaxSyntaxPayloads) {
            emdf_payloads_truncated = true;
            return;
        }
        emdf_payload_ids[static_cast<std::size_t>(emdf_payload_count)] =
            static_cast<std::uint8_t>(id);
        ++emdf_payload_count;
    }

    // The LFE's stream slot, which sits immediately after the full-bandwidth
    // channels - so it moves with acmod, unlike the coupling channel's.
    [[nodiscard]] int lfe_stream() const { return fbw_channels; }
};

}  // namespace ac3
