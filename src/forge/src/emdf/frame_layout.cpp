#include "ac3/emdf/frame_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_tools.hpp"

namespace ac3::emdf {

namespace {

// Table E2.10 (frmchexpstr code -> six blocks' strategies). 0=reuse,1=D15,2=D25,3=D45.
constexpr std::array<std::array<std::uint8_t, 6>, 32> kE210 = {{
    {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 3}, {1, 0, 0, 0, 2, 0}, {1, 0, 0, 0, 3, 3},
    {2, 0, 0, 2, 0, 0}, {2, 0, 0, 2, 0, 3}, {2, 0, 0, 3, 2, 0}, {2, 0, 0, 3, 3, 3},
    {2, 0, 1, 0, 0, 0}, {2, 0, 2, 0, 0, 3}, {2, 0, 2, 0, 2, 0}, {2, 0, 2, 0, 3, 3},
    {2, 0, 3, 2, 0, 0}, {2, 0, 3, 2, 0, 3}, {2, 0, 3, 3, 2, 0}, {2, 0, 3, 3, 3, 3},
    {3, 1, 0, 0, 0, 0}, {3, 1, 0, 0, 0, 3}, {3, 2, 0, 0, 2, 0}, {3, 2, 0, 0, 3, 3},
    {3, 2, 0, 2, 0, 0}, {3, 2, 0, 2, 0, 3}, {3, 2, 0, 3, 2, 0}, {3, 2, 0, 3, 3, 3},
    {3, 3, 1, 0, 0, 0}, {3, 3, 2, 0, 0, 3}, {3, 3, 2, 0, 2, 0}, {3, 3, 2, 0, 3, 3},
    {3, 3, 3, 2, 0, 0}, {3, 3, 3, 2, 0, 3}, {3, 3, 3, 3, 2, 0}, {3, 3, 3, 3, 3, 3},
}};

constexpr std::array<int, 16> kMantBits = {0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};
constexpr int kLfeEndmant = 7;
constexpr int kMaxFbwChannels = 5;
// numblkscod 3 (the only value in scope below) is six blocks per syncframe.
constexpr std::size_t kBlocksPerFrame = 6;

// The one frame shape this walker maps - ac3forge's Atmos output. See the
// header's SCOPE note for why anything else comes back unsupported rather
// than approximated.
constexpr int kSupportedStrmtyp = 0;  // independent
constexpr int kSupportedAcmod = 7;    // 3/2
constexpr int kSupportedNumblkscod = 3;
constexpr int kSupportedBamode = 1;   // roadmap EQ3: the encoder transmits its own
                                      // allocation parameters rather than taking
                                      // Table E1.4's bamode == 0 defaults

ExpStrategy exp_strategy(int code) {
    switch (code) {
        case 1: return ExpStrategy::kD15;
        case 2: return ExpStrategy::kD25;
        case 3: return ExpStrategy::kD45;
        default: return ExpStrategy::kReuse;
    }
}

int fullbw_channels(int acmod) {
    static const std::array<int, 8> t = {2, 1, 2, 3, 3, 4, 4, 5};
    return t[static_cast<std::size_t>(acmod)];
}

bool bit_at(std::span<const std::byte> f, std::size_t p) {
    // Matches BitReader::read_bit()'s own contract: past the end reads as
    // zero rather than indexing out of bounds. This function indexes `f`
    // directly, outside BitReader, so it needs its own bound.
    if ((p >> 3) >= f.size()) {
        return false;
    }
    return ((std::to_integer<std::uint32_t>(f[p >> 3]) >> (7 - (p & 7))) & 1) != 0;
}

}  // namespace

std::size_t syncframe_size(std::span<const std::byte> at) {
    const std::uint32_t b2 = std::to_integer<std::uint32_t>(at[2]);
    const std::uint32_t b3 = std::to_integer<std::uint32_t>(at[3]);
    const std::uint32_t frmsiz = ((b2 & 0x7) << 8) | b3;
    return static_cast<std::size_t>(frmsiz + 1) * 2;
}

// Mirrors tools/references/eac3_parse.py for this configuration.
//
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one linear walk
// of Table E1.1's syncframe, block by block, in the standard's own order. Any
// split would have to hand a dozen pieces of bit-position state across the
// seam, which is exactly the state a bit-accurate walk cannot afford to get
// wrong.
FrameLayout walk_frame(std::span<const std::byte> frame) {
    FrameLayout out;
    if (frame.size() < 6) {
        return out;
    }
    BitReader r{frame};
    const std::size_t total = frame.size() * 8;
    out.frame_bits = total;

    const auto put_hole = [&](std::size_t a, std::size_t b) {
        out.holes.push_back(BitRange{.first = a, .last = b});
    };
    const auto unsupported = [&] { return FrameLayout{}; };

    (void)r.read(16);  // syncword
    const int strmtyp = static_cast<int>(r.read(2));
    (void)r.read(3);  // substreamid
    const std::uint32_t frmsiz = r.read(11);
    const int fscod = static_cast<int>(r.read(2));
    const int numblkscod = fscod == 3 ? 3 : static_cast<int>(r.read(2));
    if (fscod == 3) {
        (void)r.read(2);
    }
    const int acmod = static_cast<int>(r.read(3));
    const int lfeon = static_cast<int>(r.read(1));
    (void)r.read(5);  // bsid
    (void)r.read(5);  // dialnorm
    if (r.read(1)) {
        (void)r.read(8);  // compre/compr
    }
    if (acmod == 0) {
        (void)r.read(5);
        if (r.read(1)) {
            (void)r.read(8);
        }
    }
    if (strmtyp == 1) {
        if (r.read(1)) {
            (void)r.read(16);
        }
    }
    if (strmtyp == 3) {
        // §E2.3.1.1 reserves it; nothing downstream of here has a defined
        // layout, so not even the object signals can be read.
        return unsupported();
    }
    const int nfchans = fullbw_channels(acmod);
    const int nblks = (numblkscod == 3) ? 6 : (numblkscod + 1);

    put_hole(0, 31);  // sync + strmtyp + substreamid + frmsiz

    if (r.read(1)) {  // mixmdate
        if (acmod > 2) {
            (void)r.read(2);
        }
        if ((acmod & 1) && acmod > 2) {
            (void)r.read(3);
            (void)r.read(3);
        }
        if (acmod & 4) {
            (void)r.read(3);
            (void)r.read(3);
        }
        if (lfeon) {
            if (r.read(1)) {
                (void)r.read(5);
            }
        }
        // Table E1.2 writes this gate as `strmtyp == 0x0`, but strmtyp 0x2 is
        // also an independent substream (§E1.3.1 - one convertible back to
        // AC-3), and it carries the same program-scaling and
        // mixing-configuration block a dependent does not. This follows
        // eac3_decoder.cpp's own reading of that gate rather than the table's
        // literal wording, so the two walks agree on where the bits are.
        if (strmtyp != 1) {
            if (r.read(1)) {
                (void)r.read(6);
            }
            if (acmod == 0) {
                if (r.read(1)) {
                    (void)r.read(6);
                }
            }
            if (r.read(1)) {
                (void)r.read(6);
            }
            const int mixdef = static_cast<int>(r.read(2));
            if (mixdef == 1) {
                (void)r.read(1);
                (void)r.read(1);
                (void)r.read(3);
            } else if (mixdef == 2) {
                (void)r.read(12);
            } else if (mixdef == 3) {
                // mixdeflen sizes the WHOLE remaining element, sub-fields and
                // byte-alignment padding included, so it can be skipped whole
                // without walking mixdata2e/mixdata3e. skip() rather than
                // read(): (mixdeflen + 2) * 8 runs to 264 bits, well past the
                // 32 read() accepts.
                const std::uint32_t mixdeflen = r.read(5);
                r.skip(static_cast<std::size_t>(mixdeflen + 2) * 8);
            }
            if (acmod < 2) {
                if (r.read(1)) {
                    (void)r.read(8);
                    (void)r.read(6);
                }
                if (acmod == 0) {
                    if (r.read(1)) {
                        (void)r.read(8);
                        (void)r.read(6);
                    }
                }
            }
            if (r.read(1)) {  // frmmixcfginfoe
                if (numblkscod == 0) {
                    (void)r.read(5);
                } else {
                    for (int b = 0; b < nblks; ++b) {
                        if (r.read(1)) {
                            (void)r.read(5);
                        }
                    }
                }
            }
        }
    }
    bool infomdate = false;
    {  // infomdate flag: a hole whether set or not (ac3forge sends 0)
        const std::size_t p = r.bit_position();
        infomdate = r.read(1) != 0;
        put_hole(p, p);  // the flag bit
        if (infomdate) {
            // Table E1.2's informational metadata, walked (not interpreted)
            // purely to reach addbsi at the right bit offset - field for
            // field the same walk read_eac3_substream already makes
            // (src/forge/src/io/elementary.cpp). Only its POSITION is used:
            // this walker holes the flag bit alone and then declines the
            // frame below, since which of these bytes a licensed decoder may
            // rewrite is not something this project has established. Walking
            // it anyway is what keeps the object signals readable on a frame
            // that carries it.
            (void)r.read(3);      // bsmod
            (void)r.read(1 + 1);  // copyrightb, origbs
            if (acmod == 0x2) {
                (void)r.read(2 + 2);  // dsurmod, dheadphonmod
            }
            if (acmod >= 0x6) {
                (void)r.read(2);  // dsurexmod
            }
            if (r.read(1)) {
                (void)r.read(5 + 2 + 1);  // mixlevel, roomtyp, adconvtyp
            }
            if (acmod == 0x0 && r.read(1)) {
                (void)r.read(5 + 2 + 1);
            }
            // §E2.3.2.6: sourcefscod is present only when fscod != 0x3 - a
            // fscod2 frame never carries it at all.
            if (fscod != 3) {
                (void)r.read(1);
            }
        }
    }
    if (strmtyp == 0 && numblkscod != 3) {
        (void)r.read(1);  // convsync
    }
    if (strmtyp == 2) {
        // §E2.3.1.1's convertible stream: blkid, then the frmsizecod of the
        // AC-3 frame it converts to. Never produced here, walked for the same
        // reason the rest of this function walks syntax it does not use.
        const bool blkid = numblkscod == 3 || r.read(1) != 0;
        if (blkid) {
            (void)r.read(6);  // frmsizecod
        }
    }
    {  // addbsi element -> a hole, and the object-audio marker when it is one
        const std::size_t p = r.bit_position();
        if (r.read(1)) {  // addbsie
            // §E2.3.1.28: addbsil counts BYTES MINUS ONE, so the element is
            // always at least one byte and at most 64.
            const std::uint32_t addbsil = r.read(6);
            std::uint32_t consumed_bytes = 1;
            const std::uint32_t first = r.read(8);
            // TS 103 420 §8.3.1 fixes an object-audio stream's addbsi to
            // seven reserved bits then flag_ec3_extension_type_a, then - only
            // when that bit is set - an 8-bit complexity_index_type_a
            // (§8.3.2.2). Anything else in addbsi belongs to whoever put it
            // there; recognising it is not the same as claiming it. A flag
            // set with no second byte to hold the index is not that shape, so
            // it is not treated as the marker either.
            const bool marker = (first >> 1) == 0 && (first & 1) != 0 && addbsil >= 1;
            if (marker) {
                out.oba_complexity_index = static_cast<int>(r.read(8));
                consumed_bytes = 2;
            }
            // skip(), not read(), for whatever is left: addbsil's payload runs
            // to 512 bits, well past the 32 read() accepts.
            r.skip(static_cast<std::size_t>(addbsil + 1 - consumed_bytes) * 8);
            out.addbsi = BitRange{.first = p, .last = r.bit_position() - 1};
            out.addbsi_object_extension = marker;
        }
        put_hole(p, r.bit_position() - 1);
    }

    // ---- audfrm ----
    int expstre = 1;
    int ahte = 0;
    if (numblkscod == 3) {
        expstre = static_cast<int>(r.read(1));
        ahte = static_cast<int>(r.read(1));
    }
    const int snroffststr = static_cast<int>(r.read(2));
    const int transproce = static_cast<int>(r.read(1));
    const int blkswe = static_cast<int>(r.read(1));
    const int dithflage = static_cast<int>(r.read(1));
    const int bamode = static_cast<int>(r.read(1));
    const int frmfgaincode = static_cast<int>(r.read(1));
    const int dbaflde = static_cast<int>(r.read(1));
    {  // skipflde flag -> a hole, and the anchor a rewriter clears
        out.skipflde_bit = r.bit_position();
        out.skipflde = r.read(1) != 0;
        put_hole(out.skipflde_bit, out.skipflde_bit);
    }
    const int spxattene = static_cast<int>(r.read(1));
    // Everything from the syncword to here is walkable for any E-AC-3
    // syncframe - no content-dependent field width has been needed yet - so
    // the object-layer signals are now known whatever this frame's shape
    // turns out to be. Past this point they are not, which is why the shape
    // is checked here and not earlier.
    out.object_signals = !r.overflowed();
    if (!out.object_signals) {
        return unsupported();
    }
    FrameLayout signals_only;
    signals_only.frame_bits = out.frame_bits;
    signals_only.object_signals = true;
    signals_only.addbsi = out.addbsi;
    signals_only.addbsi_object_extension = out.addbsi_object_extension;
    signals_only.oba_complexity_index = out.oba_complexity_index;
    signals_only.skipflde_bit = out.skipflde_bit;
    signals_only.skipflde = out.skipflde;
    const auto out_of_scope = [&] { return signals_only; };

    if (strmtyp != kSupportedStrmtyp || acmod != kSupportedAcmod || lfeon != 1 ||
        numblkscod != kSupportedNumblkscod || ahte != 0 || infomdate) {
        return out_of_scope();
    }

    // Past the shape check numblkscod is pinned to 3, so nblks is six - the
    // same fixed count chexpstr and lfeexpstr below are already sized for. A
    // fixed array rather than a vector: no allocation, and no "could this be
    // empty?" question for an optimiser to ask about the [0] below (GCC 16 at
    // -O3 asks it, and -Werror=null-dereference makes it fatal).
    std::array<int, kBlocksPerFrame> cplinu{};
    if (acmod > 1) {
        cplinu[0] = static_cast<int>(r.read(1));
        for (std::size_t b = 1; b < kBlocksPerFrame; ++b) {
            if (r.read(1)) {
                cplinu[b] = static_cast<int>(r.read(1));
            } else {
                cplinu[b] = cplinu[b - 1];
            }
        }
    }
    for (const int in_use : cplinu) {
        if (in_use != 0) {
            return out_of_scope();
        }
    }

    std::array<std::array<int, kMaxFbwChannels>, kBlocksPerFrame> chexpstr{};  // [blk][ch]
    if (expstre) {
        // per-block exponent strategy, 2 bits per channel per block
        for (int b = 0; b < nblks; ++b) {
            for (int ch = 0; ch < nfchans; ++ch) {
                chexpstr[static_cast<std::size_t>(b)][static_cast<std::size_t>(ch)] =
                    static_cast<int>(r.read(2));
            }
        }
    } else {
        // frame-level (Table E2.10): one 5-bit code per channel -> six blocks
        for (int ch = 0; ch < nfchans; ++ch) {
            const int code = static_cast<int>(r.read(5));
            for (int b = 0; b < nblks; ++b) {
                chexpstr[static_cast<std::size_t>(b)][static_cast<std::size_t>(ch)] =
                    kE210[static_cast<std::size_t>(code)][static_cast<std::size_t>(b)];
            }
        }
    }
    std::array<int, kBlocksPerFrame> lfeexpstr{};
    if (lfeon) {
        for (int b = 0; b < nblks; ++b) {
            lfeexpstr[static_cast<std::size_t>(b)] = static_cast<int>(r.read(1));
        }
    }
    // Same gate, same reading as the mixing-metadata block above: only a
    // dependent substream sends none of the converter-exponent element.
    if (strmtyp != 1) {
        const int convexpstre = (numblkscod == 3) ? 1 : static_cast<int>(r.read(1));
        if (convexpstre) {
            for (int ch = 0; ch < nfchans; ++ch) {
                (void)r.read(5);
            }
        }
    }
    int frmcsnroffst = 0;
    int frmfsnroffst = 0;
    if (snroffststr == 0) {
        frmcsnroffst = static_cast<int>(r.read(6));
        frmfsnroffst = static_cast<int>(r.read(4));
    }
    if (transproce) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1)) {
                (void)r.read(10);
                (void)r.read(8);
            }
        }
    }
    if (spxattene) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1)) {
                (void)r.read(5);
            }
        }
    }
    if (numblkscod != 0) {
        if (r.read(1)) {  // blkstrtinfoe
            out.blkstrtinfoe = true;
            int bl = 0;
            for (std::uint32_t x = frmsiz + 1; x; x >>= 1) {
                ++bl;  // (frmsiz+1).bit_length()
            }
            r.skip(static_cast<std::size_t>((nblks - 1) * (4 + bl)));
        }
    }
    // dbaflde is deliberately NOT checked here - it is a real, content-driven
    // case the audblk loop below handles per block. The other two are fixed
    // constants in this encoder's output (see eac3_frame.cpp), and bamode is
    // pinned to kSupportedBamode above, so a frame that disagrees on any of
    // the three is a frame this walker was not written against.
    if (snroffststr != 0 || bamode != kSupportedBamode || frmfgaincode != 0) {
        return out_of_scope();
    }

    // ---- audblk x nblks ----
    std::array<std::vector<std::uint8_t>, kMaxFbwChannels> exps;  // decoded exponents per fbw ch
    std::array<int, kMaxFbwChannels> endmant{};
    std::vector<std::uint8_t> lfeexps;
    // Table E1.4's bamode == 0 defaults, the starting point before block 0's
    // baie (bamode is pinned to 1 above, and this encoder always sends baie
    // in block 0 - see the read below) overwrites every field the transmitted
    // codes actually control.
    BitAllocCodes codes{.sdcycod = 2, .fdcycod = 1, .sgaincod = 1,
                        .dbpbcod = 2, .floorcod = 7, .fgaincod = 4};
    const SampleRate sr = SampleRate::k48000;  // fscod 0

    // Spectral extension state, persisting block to block exactly like
    // eac3_decoder.cpp's own spxinu/chinspx/spx_bands do - a later block only
    // resends the strategy (spxstre) if it actually changed, so a block that
    // says "reuse" needs last block's values still in scope. Every helper
    // reused here (spx_begin_subbnd, group_bands, kDefaultSpxBandStructure,
    // ...) is this project's own clean-room ac3::eac3 code, the same
    // functions eac3_decoder.cpp itself calls to decode real spx content.
    // This mirrors that decoder's parse exactly, just discarding the
    // coordinate values once their bit width is known, since a map only needs
    // to know where the bits end.
    bool spxinu = false;
    std::array<bool, kMaxFbwChannels> chinspx{};
    int spx_band_count = 0;
    int spx_startmant = 0;

    // Delta bit allocation state, persisting block to block like everything
    // else above: deltbae[ch]==0 ("reuse") leaves a channel's segments
    // exactly as they were, so a later block that says reuse needs an earlier
    // block's real segments still in scope, not the empty default. Read but
    // never stored, the independently-recomputed bap below silently disagrees
    // with whatever bap the real encoder used to size its mantissa tokens -
    // matching only on frames where no channel ever had real delta segments.
    // Content-driven, so the resulting mantissa-count drift shows up only on
    // some frames, at whatever block first carries real segments.
    std::array<DeltaSegments, kMaxFbwChannels> delta_state{};

    for (int blk = 0; blk < nblks; ++blk) {
        if (blkswe) {
            for (int ch = 0; ch < nfchans; ++ch) {
                (void)r.read(1);
            }
        }
        if (dithflage) {
            for (int ch = 0; ch < nfchans; ++ch) {
                (void)r.read(1);
            }
        }
        if (r.read(1)) {
            (void)r.read(8);  // dynrnge
        }
        if (acmod == 0) {
            if (r.read(1)) {
                (void)r.read(8);
            }
        }

        // --- spectral extension strategy + geometry (mirrors eac3_decoder.cpp) ---
        const bool spxstre = (blk == 0) || (r.read(1) != 0);
        if (spxstre) {
            spxinu = r.read(1) != 0;
            if (spxinu) {
                if (acmod != 0) {  // acmod 0 (1/0) is the one mode with no chinspx
                    for (int ch = 0; ch < nfchans; ++ch) {
                        chinspx[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                    }
                } else {
                    chinspx[0] = true;
                }
                (void)r.read(2);  // spxstrtf
                const int spxbegf = static_cast<int>(r.read(3));
                const int spxendf = static_cast<int>(r.read(3));
                const int begin_subbnd = eac3::spx_begin_subbnd(spxbegf);
                const int end_subbnd = eac3::spx_end_subbnd(spxendf);
                // The same validity check eac3_decoder.cpp itself makes
                // before trusting these - group_bands() asserts subbands is
                // in [1,kMaxSubBands], and std::span::first() below has its
                // own hard precondition on the count. Reaching this in
                // practice means bit tracking has already desynced upstream
                // in this frame, so give up cleanly on it rather than
                // pretend a clamped guess is still correct.
                if (end_subbnd <= begin_subbnd || end_subbnd > eac3::kSpxSubBands) {
                    return out_of_scope();
                }
                const int subband_count = end_subbnd - begin_subbnd;
                std::array<bool, eac3::kSpxSubBands> structure{};
                if (r.read(1)) {  // spxbndstrce
                    for (int i = 1; i < subband_count; ++i) {
                        structure[static_cast<std::size_t>(i)] = r.read(1) != 0;
                    }
                } else {
                    for (int i = 0; i < subband_count; ++i) {
                        structure[static_cast<std::size_t>(i)] =
                            eac3::kDefaultSpxBandStructure[static_cast<std::size_t>(begin_subbnd +
                                                                                    i)];
                    }
                }
                const auto spx_bands = eac3::group_bands(
                    eac3::spx_band_start(begin_subbnd), subband_count, eac3::kSpxBinsPerSubBand,
                    std::span{structure}.first(static_cast<std::size_t>(subband_count)));
                spx_band_count = spx_bands.count;
                spx_startmant = eac3::spx_band_start(begin_subbnd);
            }
        }
        if (spxinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chinspx[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                const bool send = (blk == 0) || (r.read(1) != 0);  // spxcoe[ch]
                if (!send) {
                    continue;
                }
                (void)r.read(5);  // spxblnd
                (void)r.read(2);  // mstrspxco
                for (int bnd = 0; bnd < spx_band_count; ++bnd) {
                    (void)r.read(4);  // exp
                    (void)r.read(2);  // mant
                }
            }
        }
        // A channel spx has taken over stops carrying its own high band -
        // chbwcod is skipped for it entirely (its bandwidth is fixed by
        // spx_startmant instead), exactly per eac3_decoder.cpp's own
        // "coupled or extended channel" comment. No coupling in this shape
        // (checked above), so that half of the same rule is inert here.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (spxinu && chinspx[static_cast<std::size_t>(ch)]) {
                endmant[static_cast<std::size_t>(ch)] = spx_startmant;
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            if (chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] != 0 &&
                !(spxinu && chinspx[static_cast<std::size_t>(ch)])) {
                const int chbwcod = static_cast<int>(r.read(6));
                endmant[static_cast<std::size_t>(ch)] = (chbwcod + 12) * 3 + 37;
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            if (chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] == 0) {
                continue;
            }
            const ExpStrategy st = exp_strategy(
                chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)]);
            const int ngrps = exponent_group_count(st, endmant[static_cast<std::size_t>(ch)]);
            const auto absexp = static_cast<std::uint8_t>(r.read(4));
            std::vector<std::uint8_t> grps(static_cast<std::size_t>(ngrps), std::uint8_t{0});
            for (int g = 0; g < ngrps; ++g) {
                grps[static_cast<std::size_t>(g)] = static_cast<std::uint8_t>(r.read(7));
            }
            (void)r.read(2);  // gainrng
            exps[static_cast<std::size_t>(ch)].assign(
                static_cast<std::size_t>(endmant[static_cast<std::size_t>(ch)]), 0);
            decode_exponents(absexp, grps, st, exps[static_cast<std::size_t>(ch)]);
        }
        if (lfeon && lfeexpstr[static_cast<std::size_t>(blk)] != 0) {
            const auto absexp = static_cast<std::uint8_t>(r.read(4));
            const std::array<std::uint8_t, 2> grps{static_cast<std::uint8_t>(r.read(7)),
                                                   static_cast<std::uint8_t>(r.read(7))};
            lfeexps.assign(static_cast<std::size_t>(kLfeEndmant), 0);
            decode_exponents(absexp, grps, ExpStrategy::kD15, lfeexps);
        }
        // roadmap EQ3: bamode is pinned to 1 above, so every block carries its
        // own baie flag (§7.2.1) rather than the shape omitting it entirely
        // the way it still does for snroffste below. This encoder's own
        // block 0 always sets it and states the codes; the remaining five
        // blocks say "keep them" (eac3_frame.cpp). The codes themselves DO
        // matter downstream: compute_bit_allocation() below sizes every
        // mantissa from them (dbpbcod in particular moves off its bamode==0
        // default - see eac3_frame.cpp's own note), so reading and keeping
        // them, not just skipping their 11 bits, is what keeps the mantissa
        // tally in sync with what the encoder actually sent.
        if (r.read(1)) {  // baie
            codes.sdcycod = static_cast<int>(r.read(2));
            codes.fdcycod = static_cast<int>(r.read(2));
            codes.sgaincod = static_cast<int>(r.read(2));
            codes.dbpbcod = static_cast<int>(r.read(2));
            codes.floorcod = static_cast<int>(r.read(3));
        }
        // snroffststr==0: frame SNR. snroffste itself is correctly absent
        // here, not just skipped: this project's own encoder (eac3_frame.cpp)
        // omits the flag bit entirely whenever snroffststr is 0 at the frame
        // level.
        const int csnroffst = frmcsnroffst;
        const int fsnroffst = frmfsnroffst;
        // Same gate again (eac3_decoder.cpp reads convsnroffste the same
        // way); inert here, since the shape check above already pinned
        // strmtyp to 0, but spelled consistently with the two gates that are
        // not.
        if (strmtyp != 1) {
            if (r.read(1)) {
                (void)r.read(10);  // convsnroffste
            }
        }
        // dbaflde IS a real per-block field once set at the frame level:
        // every block sends its own deltbaie bit (§5.4.3.47-57), even a block
        // with nothing to say, and this encoder's delta bit allocation
        // (src/forge/src/core/bitalloc.cpp) sets dbaflde whenever a channel's
        // real spectral energy diverges enough from the default allocation
        // model to warrant a correction - ordinary, content-driven behaviour.
        // The cpldeltbae branch is never taken in this shape (no coupling),
        // and is walked for the same reason the inert cplinu checks above
        // exist: correctness if that ever changes, not speculation about it.
        if (dbaflde) {
            const bool cpl_in_use_here = cplinu[static_cast<std::size_t>(blk)] != 0;
            if (r.read(1)) {  // deltbaie
                if (cpl_in_use_here) {
                    (void)r.read(2);  // cpldeltbae
                }
                std::array<int, kMaxFbwChannels> deltbae{};
                for (int ch = 0; ch < nfchans; ++ch) {
                    deltbae[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
                }
                auto read_segments = [&r]() {
                    DeltaSegments segs;
                    segs.deltnseg = static_cast<int>(r.read(3)) + 1;
                    for (int seg = 0; seg < segs.deltnseg; ++seg) {
                        segs.deltoffst[static_cast<std::size_t>(seg)] =
                            static_cast<std::uint8_t>(r.read(5));
                        segs.deltlen[static_cast<std::size_t>(seg)] =
                            static_cast<std::uint8_t>(r.read(4));
                        segs.deltba[static_cast<std::size_t>(seg)] =
                            static_cast<std::uint8_t>(r.read(3));
                    }
                    return segs;
                };
                // §5.4.3.47-57: every stream's 2-bit code first, then every
                // stream's segment data - not interleaved per stream.
                for (int ch = 0; ch < nfchans; ++ch) {
                    // 1 = new info follows (store it); 2 = perform no delta
                    // alloc (clear it); 0 = reuse (leave delta_state[ch]
                    // exactly as an earlier block left it).
                    if (deltbae[static_cast<std::size_t>(ch)] == 1) {
                        delta_state[static_cast<std::size_t>(ch)] = read_segments();
                    } else if (deltbae[static_cast<std::size_t>(ch)] == 2) {
                        delta_state[static_cast<std::size_t>(ch)] = DeltaSegments{};
                    }
                }
            }
        }
        // skiple - only present at all when the frame-level skipflde is set
        // (eac3_frame.cpp: skipflde = metadata.empty() ? 0 : 1, and
        // put_skip_field is only called per block `if (skipflde)`). A frame
        // with no object container has NO skip field syntax anywhere in it;
        // reading a phantom skiple bit there would read the first bit of real
        // mantissa data instead.
        if (out.skipflde) {
            SkipField field{.range = {}, .block = blk, .present = false, .carries_container = false};
            const std::size_t sp = r.bit_position();
            const int skiple = static_cast<int>(r.read(1));
            if (skiple) {
                field.present = true;
                const int skipl = static_cast<int>(r.read(9));
                const std::size_t data_start = r.bit_position();
                // Find 0x5838 within [data_start, data_start+skipl*8), never
                // past the frame itself: this loop indexes `frame` directly
                // rather than through `r`, so it does not get BitReader's
                // overflow()-on-read-past-end protection for free.
                //
                // Only ever the FIRST match, frame-wide: this encoder embeds
                // its one real container in exactly one skip field per access
                // unit, but a frame can legitimately carry several skiple=1
                // padding fields with no container at all, and unconstrained
                // 16-bit content in one of those can coincidentally equal
                // 0x5838. Without this guard a false match in a LATER block
                // silently overwrites an already-found real container_start.
                if (!out.has_container) {
                    const std::size_t scan_limit =
                        std::min(data_start + static_cast<std::size_t>(skipl) * 8, total);
                    for (std::size_t bitp = data_start; bitp + 16 <= scan_limit; ++bitp) {
                        std::uint32_t w = 0;
                        for (int i = 0; i < 16; ++i) {
                            w = (w << 1) | (bit_at(frame, bitp + static_cast<std::size_t>(i)) ? 1u
                                                                                             : 0u);
                        }
                        if (w == kSyncWord) {
                            out.has_container = true;
                            out.container_start = bitp;
                            field.carries_container = true;
                            break;
                        }
                    }
                }
                r.skip(static_cast<std::size_t>(skipl) * 8);
            }
            field.range = BitRange{.first = sp, .last = r.bit_position() - 1};
            put_hole(field.range.first, field.range.last);
            out.skip_fields.push_back(field);
        }
        // mantissas (constant per block for this config, but recompute generally)
        int counts1 = 0;
        int counts2 = 0;
        int counts4 = 0;
        int mant = 0;
        bool tally_desynced = false;
        auto tally = [&](std::span<const std::uint8_t> e, int end, int cs, int fs,
                         const DeltaSegments& delta) {
            // `end` is this block's own endmant for the channel; `e` is what
            // the exponent walk above actually produced for it. A malformed
            // frame can have them disagree, and subspan() below is a
            // precondition, not a clamp: asking for more than `e` holds is
            // undefined, and on an EMPTY `e` it manufactures a span with a
            // null data pointer and a non-zero size, which
            // compute_bit_allocation then dereferences (the defect
            // ac3::signing::emdf_atmos_signer.cpp's own tally hit first -
            // roadmap VX3, fuzz_signing_verify - before this walk existed;
            // ported here since verify_atmos_stream now runs through it).
            // Disagreement here means the bit walk has already lost sync, so
            // this frame is out of scope rather than tallied against a
            // mantissa count that was never right.
            if (end < 0 || static_cast<std::size_t>(end) > e.size()) {
                tally_desynced = true;
                return;
            }
            std::vector<std::uint8_t> bap(static_cast<std::size_t>(end), std::uint8_t{0});
            BitAllocRegion region{};
            region.snr_all_zero = (cs == 0 && fs == 0);
            region.delta = delta;
            compute_bit_allocation(e.subspan(0, static_cast<std::size_t>(end)), sr, codes, cs, fs,
                                   bap, region);
            for (const std::uint8_t bp : bap) {
                if (bp == 1) {
                    ++counts1;
                } else if (bp == 2) {
                    ++counts2;
                } else if (bp == 4) {
                    ++counts4;
                } else if (bp) {
                    mant += kMantBits[bp];
                }
            }
        };
        // LFE has no delta bit allocation field at all (§5.4.3.49/E2.3.2.9's
        // deltbae[ch] loop is bounded by nfchans), so it always gets the
        // empty default rather than anything from delta_state.
        for (int ch = 0; ch < nfchans; ++ch) {
            tally(exps[static_cast<std::size_t>(ch)], endmant[static_cast<std::size_t>(ch)],
                  csnroffst, fsnroffst, delta_state[static_cast<std::size_t>(ch)]);
        }
        if (lfeon) {
            tally(lfeexps, kLfeEndmant, csnroffst, fsnroffst, DeltaSegments{});
        }
        if (tally_desynced) {
            return out_of_scope();
        }
        mant += 5 * ((counts1 + 2) / 3);
        mant += 7 * ((counts2 + 2) / 3);
        mant += 7 * ((counts4 + 1) / 2);
        r.skip(static_cast<std::size_t>(mant));
    }

    out.audio_end_bits = r.bit_position();
    if (r.overflowed() || out.audio_end_bits + 18 > total) {
        return out_of_scope();
    }
    // aux + crc tail
    put_hole(out.audio_end_bits, total - 18);
    put_hole(total - 17, total - 1);

    // Parse the container to get its length + protection codes.
    if (out.has_container) {
        BitReader cr{frame};
        cr.skip(out.container_start);
        (void)cr.read(16);  // sync
        out.container_len = static_cast<int>(cr.read(16));
        (void)cr.read(2);  // version
        (void)cr.read(3);  // key_id (ac3forge writes 0)
        // Bounded by cr.overflowed(): a misparsed payload_config or a
        // corrupt/unexpected container shape can desync this reader from the
        // real field boundaries, which lets a garbage `size` value - up to
        // ~2^32 - drive the skip loop below into a multi-billion-iteration
        // hang despite the reader having already run off the end of `frame`
        // (BitReader::read() past end is well-defined - it sets a sticky
        // overflow flag and returns zeros - but the loop count itself is not
        // checked against it). Once overflowed, every further pid/size read
        // is a meaningless zero, so bailing out is exactly "stop trusting a
        // reader that has left valid data".
        while (!cr.overflowed()) {
            const int pid = static_cast<int>(cr.read(5));
            if (pid == 0) {
                break;
            }
            // payload config (fixed shape TS 103 420 Table 56, ac3forge)
            (void)cr.read(1);  // smploffste=0
            (void)cr.read(1);  // duratione=0
            if (cr.read(1)) {  // groupide, variable_bits(2)
                while (!cr.overflowed()) {
                    (void)cr.read(2);
                    if (!cr.read(1)) {
                        break;
                    }
                }
            }
            if (cr.read(1)) {
                (void)cr.read(8);  // codecdatae
            }
            (void)cr.read(1);  // discard_unknown_payload
            // smploffste==0
            const int aligned = static_cast<int>(cr.read(1));
            if (aligned) {
                (void)cr.read(1);
                (void)cr.read(1);
                (void)cr.read(5);
                (void)cr.read(2);
            }
            // payload size (variable_bits 8)
            std::uint32_t size = 0;
            while (!cr.overflowed()) {
                size += cr.read(8);
                if (!cr.read(1)) {
                    break;
                }
                size <<= 8;
                size += 256;
            }
            for (std::uint32_t i = 0; i < size && !cr.overflowed(); ++i) {
                (void)cr.read(8);
            }
        }
        out.protection_primary_code = static_cast<int>(cr.read(2));
        out.protection_secondary_code = static_cast<int>(cr.read(2));
        const auto pbits = [](int code) {
            return code == 0 ? 0 : code == 1 ? 8 : code == 2 ? 32 : 128;
        };
        cr.skip(static_cast<std::size_t>(pbits(out.protection_primary_code) +
                                         pbits(out.protection_secondary_code)));
        out.container_parsed_bits = cr.bit_position() - out.container_start;
    }
    out.supported = true;
    return out;
}

}  // namespace ac3::emdf
