#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/syntax_trace.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "ac3/core/aht_tables.hpp"
#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/coupling.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/eac3_tools.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/decoder/diagnostics.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/decoder/transient_prenoise.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/internal/decode_scalar.hpp"
#include "ac3/internal/profile.hpp"
#include "eac3_tools_fixed.hpp"
#include "fixed32.hpp"
#include "scalar_inverse.hpp"
#include "block_norm.hpp"
#include "ac3/internal/profiling.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "bitalloc_internal.hpp"
#include "bitalloc_memo.hpp"
#include "gain.hpp"

// E-AC-3 syncframe decoding, ATSC A/52:2018 Annex E Tables E1.2, E1.3 and E1.4.
//
// Annex E is not a variant of the AC-3 frame; it is a different container for
// the same coding tools. syncinfo is only the sync word, the frame size is
// stated outright, and everything that AC-3 decides per block - exponent
// strategies, coupling-in-use, the SNR offsets - can be hoisted into a
// frame-level audfrm element, which then makes several audblk fields
// conditional. That hoisting is why this cannot share decode_frame's loop:
// the two syntaxes agree only on the payload underneath.

namespace ac3 {


namespace {

using eac3::StreamType;

// A substream codes at most 3/2 plus LFE (Table 5.8).
constexpr int kMaxSubstreamChannels = 6;

// One more slot past the real channels for the shared coupling channel,
// mirroring how it rides alongside the fbw channels and LFE in the coded
// stream (§5.3.3). Channel indices never reach this far (nchans <= 6), so a
// single fixed slot at index kMaxSubstreamChannels never collides with a
// real channel, unlike AC-3's decoder which sizes its arrays dynamically.
constexpr int kCplStream = kMaxSubstreamChannels;
constexpr int kMaxSubstreamStreams = kMaxSubstreamChannels + 1;

// Table E1.4, the else-branch of if(bamode): with bamode == 0 the allocation
// parameters take THESE values. They are not the §8.2.12 basic-encoder
// recommendations AC-3 uses - floorcod is 0x7 here against §8.2.12's 4, which
// is what BitAllocCodes defaults to. floorcod sets the masking floor, so the
// wrong one changes every bap and therefore every block's mantissa bit count:
// block 1 onwards lands at the wrong bit offset and the frame decodes as
// noise. Silence cannot expose it, since zero SNR offsets make §7.2.2.1.1
// zero the allocation before floorcod is ever consulted.
constexpr BitAllocCodes kBamode0Codes{
    .sdcycod = 2, .fdcycod = 1, .sgaincod = 1, .dbpbcod = 2, .floorcod = 7, .fgaincod = 4};

struct Bsi {
    StreamType strmtyp = StreamType::kIndependent;
    int substreamid = 0;
    int bsid = eac3::kBsid;
    // 0 ("not indicated") unless infomdate carried one - the same convention
    // io::ScannedStream::bsmod keeps.
    int bsmod = 0;
    std::uint32_t words = 0;  // frmsiz + 1
    SampleRate sample_rate = SampleRate::k48000;
    int numblkscod = 3;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    bool compre = false;
    // Only ever set for an independent/convertible substream - see
    // parse_bsi's own comment on why a dependent's compre bit does not mean
    // this.
    std::optional<std::uint8_t> compr;
    std::optional<std::uint16_t> chanmap;
    // Ch2's own dialnorm/compr, present only when acmod is kDualMono (1+1).
    std::optional<int> dialnorm2;
    std::optional<std::uint8_t> compr2;
    // Table E1.2's two optional metadata elements, exactly as read.
    std::optional<meta::MixMetadata> mixing;
    std::optional<meta::BsiInfo> info;
};

// Table E1.2's mixdef element (§E2.3.1.18-52). mixdef 0x3's mixdeflen sizes
// the WHOLE element - sub-fields and byte-alignment fill included - so the
// contents are walked for their values and the reader is then placed from the
// length rather than from where the walk happened to stop. That way a stream
// using a sub-field this build does not model still lands the reader in the
// right place, which is the property the old skip-it-whole code had and is
// worth keeping now that the contents are read.
meta::MixingParameters read_mixing_parameters(BitReader& r) {
    const auto read_premix = [&r] {
        meta::PremixCompression premix;
        premix.premixcmpsel = static_cast<meta::PremixCompressionSource>(r.read(1));
        premix.drcsrc = static_cast<meta::DrcSource>(r.read(1));
        premix.premixcmpscl = static_cast<int>(r.read(3));
        return premix;
    };
    meta::MixingParameters mixing;
    mixing.mixdef = static_cast<meta::MixDefinition>(r.read(2));
    switch (mixing.mixdef) {
        case meta::MixDefinition::kNone:
            break;
        case meta::MixDefinition::kPremix:
            mixing.premix = read_premix();
            break;
        case meta::MixDefinition::kReserved:
            mixing.reserved = static_cast<std::uint16_t>(r.read(12));
            break;
        case meta::MixDefinition::kExtended: {
            const auto mixdeflen = r.read(5);
            const auto start = r.bit_position();
            if (r.read(1) != 0) {  // mixdata2e
                meta::ExternalScales external;
                external.premix = read_premix();
                const auto read_scale = [&r]() -> std::optional<int> {
                    if (r.read(1) == 0) {
                        return std::nullopt;
                    }
                    return static_cast<int>(r.read(4));
                };
                external.left = read_scale();
                external.centre = read_scale();
                external.right = read_scale();
                external.left_surround = read_scale();
                external.right_surround = read_scale();
                external.lfe = read_scale();
                external.dmixscl = read_scale();
                if (r.read(1) != 0) {  // addche
                    external.auxiliary =
                        std::array<std::optional<int>, 2>{read_scale(), read_scale()};
                }
                mixing.external = external;
            }
            if (r.read(1) != 0) {  // mixdata3e
                meta::SpeechEnhancement speech;
                speech.spchdat = static_cast<int>(r.read(5));
                if (r.read(1) != 0) {  // addspchdate
                    meta::SpeechEnhancement::Additional additional;
                    additional.spchdat1 = static_cast<int>(r.read(5));
                    additional.spchan1att = static_cast<int>(r.read(2));
                    if (r.read(1) != 0) {  // addspchdat1e
                        meta::SpeechEnhancement::Additional::More more;
                        more.spchdat2 = static_cast<int>(r.read(5));
                        more.spchan2att = static_cast<int>(r.read(3));
                        additional.more = more;
                    }
                    speech.additional = additional;
                }
                mixing.speech = speech;
            }
            // §E2.3.1.22: mixdeflen 0-31 means 2-33 bytes. Skip whatever the
            // walk above left, mixdatafill included.
            const auto total = (mixdeflen + 2) * 8;
            const auto used = static_cast<std::uint32_t>(r.bit_position() - start);
            r.skip(total > used ? total - used : 0);
            break;
        }
    }
    return mixing;
}

// Table E1.2's mixing-metadata payload. None of it changes how the audio is
// coded, but every field still has to be walked exactly: one bit out of place
// shifts audfrm along and the rest of the frame decodes as a different stream.
// The two strmtyp gates here are the point - an independent substream carries
// the program-scaling and mixing-configuration block that a dependent, which
// is only ever part of someone else's program, does not.
meta::MixMetadata read_mixing_metadata(BitReader& r, const Bsi& bsi, int nblks) {
    const auto acmod = static_cast<std::uint8_t>(bsi.acmod);
    meta::MixMetadata mix;
    if (acmod > 0x2) {
        const auto mode = r.read(2);  // dmixmod
        if (mode < 3) {               // Table D2.2's '11' reads as "not indicated"
            mix.dmixmod = static_cast<meta::DownmixMode>(mode);
        }
    }
    if ((acmod & 0x1) != 0 && acmod > 0x2) {
        mix.ltrtcmixlev = static_cast<meta::MixLevel>(r.read(3));
        mix.lorocmixlev = static_cast<meta::MixLevel>(r.read(3));
    }
    if ((acmod & 0x4) != 0) {
        // Tables D2.4/D2.6 reserve '000'..'010' for the surround levels, and
        // §E2.3.1.9 has a decoder receiving one substitute 0.841 - which is
        // exactly MixLevel::kMinus1_5dB, so the substitution is made here
        // rather than left for every reader of the field to remember.
        const auto surround = [&] {
            const auto level = static_cast<meta::MixLevel>(r.read(3));
            return meta::valid_surround_mix_level(level) ? level : meta::MixLevel::kMinus1_5dB;
        };
        mix.ltrtsurmixlev = surround();
        mix.lorosurmixlev = surround();
    }
    if (bsi.lfe && r.read(1) != 0) {  // lfemixlevcode
        mix.lfemixlevcod = static_cast<int>(r.read(5));
    }
    if (bsi.strmtyp != StreamType::kDependent) {
        const auto read_scale = [&r]() -> std::optional<int> {
            if (r.read(1) == 0) {
                return std::nullopt;
            }
            return static_cast<int>(r.read(6));
        };
        mix.pgmscl = read_scale();
        if (acmod == 0x0) {
            mix.pgmscl2 = read_scale();
        }
        mix.extpgmscl = read_scale();
        mix.mixing = read_mixing_parameters(r);
        if (acmod < 0x2) {
            const auto read_pan = [&r]() -> std::optional<meta::PanInfo> {
                if (r.read(1) == 0) {  // paninfoe
                    return std::nullopt;
                }
                meta::PanInfo pan;
                pan.panmean = static_cast<int>(r.read(8));
                pan.paninfo = static_cast<int>(r.read(6));
                return pan;
            };
            mix.pan = read_pan();
            if (acmod == 0x0) {
                mix.pan2 = read_pan();
            }
        }
        if (r.read(1) != 0) {  // frmmixcfginfoe
            std::array<std::optional<int>, kBlocksPerFrame> words{};
            // §E2.3.1.60: with one block per syncframe the per-block flag is
            // INFERRED as set, so the word is unconditional and there is no
            // flag on the wire to read.
            if (bsi.numblkscod == 0x0) {
                words[0] = static_cast<int>(r.read(5));
            } else {
                for (int blk = 0; blk < nblks; ++blk) {
                    if (r.read(1) != 0) {  // blkmixcfginfoe
                        words[static_cast<std::size_t>(blk)] = static_cast<int>(r.read(5));
                    }
                }
            }
            mix.blkmixcfginfo = words;
        }
    }
    return mix;
}

// Table E1.2's informational-metadata payload: bsmod and the production notes.
// Writes bsi.bsmod directly (the raw code, for a caller - an inspection
// tool's JSON output - that wants it off DecodedSubstream without unwrapping
// BitstreamMode) as well as returning the full decode.
meta::BsiInfo read_informational_metadata(BitReader& r, Bsi& bsi) {
    const auto acmod = static_cast<std::uint8_t>(bsi.acmod);
    meta::BsiInfo info;
    const auto bsmod = r.read(3);
    bsi.bsmod = static_cast<int>(bsmod);
    info.bsmod = static_cast<meta::BitstreamMode>(bsmod);
    info.copyrightb = r.read(1) != 0;
    info.origbs = r.read(1) != 0;
    if (acmod == 0x2) {
        const auto surround = r.read(2);  // dsurmod
        if (surround < 3) {
            info.dsurmod = static_cast<meta::SurroundMode>(surround);
        }
        const auto headphone = r.read(2);  // dheadphonmod
        if (headphone < 3) {               // Table D2.8's '11' reads as "not indicated"
            info.dheadphonmod = static_cast<meta::HeadphoneMode>(headphone);
        }
    }
    if (acmod >= 0x6) {
        info.dsurexmod = static_cast<meta::SurroundExMode>(r.read(2));
    }
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
        // Annex E's audprodie carries a third field AC-3's does not.
        production.adconvtyp = static_cast<meta::AdConverterType>(r.read(1));
        return production;
    };
    info.audprod = read_audprod();
    if (acmod == 0x0) {
        info.audprod2 = read_audprod();
    }
    // §E2.3.2.6: sourcefscod is present only when fscod != 0x3 - a fscod2
    // frame never carries it at all.
    if (!is_reduced_rate(bsi.sample_rate)) {
        info.sourcefscod = r.read(1) != 0;
    }
    return info;
}

std::expected<Bsi, DecodeError> parse_bsi(BitReader& r, std::size_t frame_bytes) {
    Bsi bsi;
    if (r.read(16) != kSyncWord) {
        return std::unexpected(DecodeError::kBadSyncWord);
    }
    const auto strmtyp = r.read(2);
    if (strmtyp == static_cast<std::uint32_t>(StreamType::kReserved)) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    bsi.strmtyp = static_cast<StreamType>(strmtyp);
    bsi.substreamid = static_cast<int>(r.read(3));
    bsi.words = r.read(11) + 1;  // frmsiz counts words minus one
    if (bsi.words * 2 != frame_bytes) {
        return std::unexpected(DecodeError::kTruncated);
    }
    const auto fscod = r.read(2);
    if (fscod == 0x3) {
        // §E2.3.1.3: fscod2 replaces numblkscod outright when it is used - a
        // reduced-rate frame is implicitly always six blocks, so numblkscod's
        // bits are never sent. Modelling that as numblkscod == 0x3 (rather
        // than adding a parallel "six blocks, no field" flag) means every
        // downstream numblkscod check below - which is really asking "is this
        // the always-six-blocks case?" - keeps working unmodified.
        const auto fscod2 = r.read(2);
        const auto rate = sample_rate_from_fscod2(fscod2);
        if (!rate.has_value()) {
            return std::unexpected(DecodeError::kReservedValue);
        }
        bsi.sample_rate = *rate;
        bsi.numblkscod = 0x3;
    } else {
        bsi.sample_rate = static_cast<SampleRate>(fscod);
        // Table E2.4. Fewer than six blocks shortens the syncframe and flips
        // four of Table E1.2/E1.3's implied values, all of which fall out of
        // nblks below. Nothing in this repo emits it and neither does
        // FFmpeg's encoder, so unlike the six-block path it is spec-derived
        // rather than measured.
        bsi.numblkscod = static_cast<int>(r.read(2));
    }
    bsi.acmod = static_cast<Acmod>(r.read(3));
    bsi.lfe = r.read(1) != 0;
    const auto bsid = static_cast<int>(r.read(5));
    if (bsid < eac3::kMinDecodableBsid || bsid > eac3::kBsid) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    bsi.bsid = bsid;
    bsi.dialnorm = static_cast<int>(r.read(5));
    // §E3.8.5: in a DEPENDENT substream compre marks the last dependent of the
    // program rather than announcing a compression word - though it still
    // drags one in. Either way the 8 bits have to be consumed; only stored
    // into bsi.compr when this substream is independent/convertible, where
    // the word is actually what it says it is.
    bsi.compre = r.read(1) != 0;
    if (bsi.compre) {
        const auto compr = static_cast<std::uint8_t>(r.read(8));
        if (bsi.strmtyp != StreamType::kDependent) {
            bsi.compr = compr;
        }
    }
    // Annex E Table E1.2: unconditional on strmtyp, mirroring the encoder's
    // own write side - even a dependent substream coding 1+1 would carry it,
    // though nothing in this repo ever builds one.
    if (bsi.acmod == Acmod::kDualMono) {
        bsi.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            bsi.compr2 = static_cast<std::uint8_t>(r.read(8));
        }
    }
    if (bsi.strmtyp == StreamType::kDependent && r.read(1) != 0) {  // chanmape
        bsi.chanmap = static_cast<std::uint16_t>(r.read(16));
    }
    const int nblks = eac3::blocks_per_syncframe(bsi.numblkscod);
    if (r.read(1) != 0) {  // mixmdate
        bsi.mixing = read_mixing_metadata(r, bsi, nblks);
    }
    if (r.read(1) != 0) {  // infomdate
        bsi.info = read_informational_metadata(r, bsi);
    }
    if (bsi.strmtyp == StreamType::kIndependent && bsi.numblkscod != 0x3) {
        r.skip(1);  // convsync
    }
    if (bsi.strmtyp == StreamType::kConvertible) {
        const bool blkid = bsi.numblkscod == 0x3 || r.read(1) != 0;
        if (blkid) {
            r.skip(6);  // frmsizecod, describing the AC-3 frame this came from
        }
    }
    if (r.read(1) != 0) {  // addbsie
        const auto addbsil = r.read(6);
        r.skip((addbsil + 1) * 8);
    }
    return bsi;
}

struct AudFrm {
    // §E2.3.2.7: the per-block exponent strategies were transmitted
    // individually rather than hoisted into Table E2.10's frame codes.
    // Resolved into chexpstr either way below, so it is not recoverable from
    // the strategies themselves - kept because a syntax dump reports it.
    bool expstre = true;
    bool ahte = false;
    int snroffststr = 0;
    bool transproce = false;
    bool blkswe = false;
    bool dithflage = false;
    bool bamode = false;
    bool frmfgaincode = false;
    bool dbaflde = false;
    bool skipflde = false;
    int frmcsnroffst = 0;
    int frmfsnroffst = 0;
    // [block][channel]; the LFE's is a separate one-bit strategy.
    std::array<std::array<ExpStrategy, kMaxSubstreamChannels>, kBlocksPerFrame> chexpstr{};
    std::array<ExpStrategy, kBlocksPerFrame> lfeexpstr{};
    // cplstre[blk]: whether THIS block resends the coupling strategy (true
    // for block 0's implied strategy). cplinu[blk]: the strategy in effect
    // for that block, valid whether resent here or carried over from an
    // earlier one. Both are decided in audfrm, ahead of any block's payload.
    std::array<bool, kBlocksPerFrame> cplstre{};
    std::array<bool, kBlocksPerFrame> cplinu{};
    // The coupling channel's own exponent strategy, same Table E2.10 shape
    // as chexpstr, only present where cplinu[blk] holds.
    std::array<ExpStrategy, kBlocksPerFrame> cplexpstr{};
    // §E3.6.4.2.3's per-channel notch code, frame-constant. -1 means this
    // channel does not attenuate (chinspxatten[ch] clear, or spxattene clear
    // for the whole frame).
    std::array<int, kMaxSubstreamChannels> spxattencod{};
    // §E2.2.3: which streams are AHT-coded this frame - cplahtinu at
    // kCplStream, chahtinu[ch] at [0, nfchans), lfeahtinu at [nfchans].
    // Frame-constant, like everything else AHT touches (it needs exactly one
    // exponent set for the whole frame, which rules out anything per-block).
    std::array<bool, kMaxSubstreamStreams> ahtinu{};
    // §3.7: per full-bandwidth channel, only meaningful where chintransproc
    // is set (which itself is only meaningful when transproce is). Location
    // is already in samples (multiplied by 4 at parse time), not the raw
    // 10-bit field.
    std::array<bool, kMaxSubstreamChannels> chintransproc{};
    std::array<int, kMaxSubstreamChannels> transprocloc{};
    std::array<int, kMaxSubstreamChannels> transproclen{};
};

std::expected<AudFrm, DecodeError> parse_audfrm(BitReader& r, const Bsi& bsi, int nblks) {
    const int nfchans = fullbw_channel_count(bsi.acmod);
    AudFrm frm;
    frm.spxattencod.fill(-1);
    // Only a six-block syncframe can hoist its exponent strategies; a shorter
    // one always carries them per block and never uses AHT.
    bool expstre = true;
    if (bsi.numblkscod == 0x3) {
        expstre = r.read(1) != 0;
        frm.ahte = r.read(1) != 0;
    }
    frm.expstre = expstre;
    frm.snroffststr = static_cast<int>(r.read(2));
    if (frm.snroffststr == 0x3) {
        return std::unexpected(DecodeError::kReservedValue);
    }
    frm.transproce = r.read(1) != 0;
    frm.blkswe = r.read(1) != 0;
    frm.dithflage = r.read(1) != 0;
    frm.bamode = r.read(1) != 0;
    frm.frmfgaincode = r.read(1) != 0;
    frm.dbaflde = r.read(1) != 0;
    frm.skipflde = r.read(1) != 0;
    const bool spxattene = r.read(1) != 0;

    // Coupling-in-use for every block is decided here, ahead of the blocks:
    // cplstre[0] is an implied 1 (block 0 always states a strategy), and
    // later blocks either resend one (cplstre[blk]) or inherit the last.
    if (static_cast<std::uint8_t>(bsi.acmod) > 0x1) {
        bool cplinu = r.read(1) != 0;
        frm.cplstre[0] = true;
        frm.cplinu[0] = cplinu;
        for (int blk = 1; blk < nblks; ++blk) {
            const bool resent = r.read(1) != 0;  // cplstre[blk]
            frm.cplstre[static_cast<std::size_t>(blk)] = resent;
            if (resent) {
                cplinu = r.read(1) != 0;
            }
            frm.cplinu[static_cast<std::size_t>(blk)] = cplinu;
        }
    }

    if (expstre) {
        // Per-block explicit strategies. This project's own encoder always
        // hoists (kExpstre == 0, the `else` branch below), so this path is
        // spec-derived generality rather than something measured against a
        // real stream - matching the project's existing stance on syntax
        // this encoder never exercises (e.g. numblkscod != 3).
        for (int blk = 0; blk < nblks; ++blk) {
            if (frm.cplinu[static_cast<std::size_t>(blk)]) {
                frm.cplexpstr[static_cast<std::size_t>(blk)] =
                    static_cast<ExpStrategy>(r.read(2));  // cplexpstr[blk]
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                frm.chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] =
                    static_cast<ExpStrategy>(r.read(2));
            }
        }
    } else {
        // Table E2.10: one 5-bit code per channel expands to all six blocks.
        // frmcplexpstr precedes the per-channel codes, and is present only
        // when some block in the frame actually couples.
        const bool cpl_active = std::find(frm.cplinu.begin(), frm.cplinu.begin() + nblks, true) !=
                                frm.cplinu.begin() + nblks;
        if (cpl_active) {
            const auto code = static_cast<int>(r.read(5));  // frmcplexpstr
            for (int blk = 0; blk < nblks; ++blk) {
                frm.cplexpstr[static_cast<std::size_t>(blk)] = eac3::frame_exp_strategy(code, blk);
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto code = static_cast<int>(r.read(5));
            for (int blk = 0; blk < nblks; ++blk) {
                frm.chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] =
                    eac3::frame_exp_strategy(code, blk);
            }
        }
    }
    if (bsi.lfe) {
        for (int blk = 0; blk < nblks; ++blk) {
            frm.lfeexpstr[static_cast<std::size_t>(blk)] =
                r.read(1) != 0 ? ExpStrategy::kD15 : ExpStrategy::kReuse;
        }
    }
    // The whole converter-exponent element is gated on strmtyp == 0x0: only an
    // independent substream can be converted back to AC-3, so a dependent
    // sends none of it. These strategies describe how such a converter would
    // code the frame and have no bearing on decoding it.
    if (bsi.strmtyp != StreamType::kDependent) {
        const bool convexpstre = bsi.numblkscod == 0x3 || r.read(1) != 0;
        if (convexpstre) {
            r.skip(static_cast<std::size_t>(nfchans) * 5);  // convexpstr[ch]
        }
    }
    if (frm.ahte) {
        // §E2.2.3: cplahtinu, then chahtinu[ch] per fbw channel, then
        // lfeahtinu - exactly which streams re-code their six blocks of
        // mantissas as one gain-adaptively-quantized set instead of the
        // ordinary per-block grouped format.
        //
        // None of the three is unconditional. AHT spans the whole frame and
        // cannot straddle a change of exponent set, so Table E1.2 transmits a
        // stream's flag only where that stream sends exponents exactly once
        // in the frame - the §3.4.2 nregs counts computed below - and the
        // coupling channel additionally has to be coupled in all six blocks.
        // Where the condition does not hold the bit is not in the stream at
        // all and the flag is 0, which is what `ahtinu` already holds.
        //
        // This project's own encoder meets every condition by construction
        // (Table E2.10 code 0 - D15 then reuse - for every channel, and
        // all-or-nothing coupling; see eac3_frame.cpp's own note beside the
        // matching writes), and so does FFmpeg's, which is why reading all
        // three unconditionally decoded both for as long as they were the
        // only encoders tried. A Dolby Encoding Engine 6.5.4 stream does not:
        // it resends the coupling channel's exponents mid-frame, so
        // ncplregs > 1, cplahtinu is absent, and reading it anyway put every
        // field after it one bit out - which is what
        // tests/golden/external-baseline/eac3-51-256/dee.ec3 and the
        // third-party interop checks in tools/checks/verify_gold_reference.sh
        // exist to catch.
        const auto blocks = static_cast<std::size_t>(nblks);
        const auto ncplblks = std::count(frm.cplinu.begin(), frm.cplinu.begin() + nblks, true);
        int ncplregs = 0;
        for (std::size_t blk = 0; blk < blocks; ++blk) {
            if (frm.cplstre[blk] || frm.cplexpstr[blk] != ExpStrategy::kReuse) {
                ++ncplregs;
            }
        }
        // The spec writes this as "ncplblks == 6"; nblks is that same 6
        // here, since expstre/ahte are only read at all when numblkscod is
        // 0x3 (§E2.3.2 - AHT exists only in six-block mode).
        if (ncplblks == nblks && ncplregs == 1) {
            frm.ahtinu[static_cast<std::size_t>(kCplStream)] = r.read(1) != 0;  // cplahtinu
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            int nchregs = 0;
            for (std::size_t blk = 0; blk < blocks; ++blk) {
                if (frm.chexpstr[blk][static_cast<std::size_t>(ch)] != ExpStrategy::kReuse) {
                    ++nchregs;
                }
            }
            if (nchregs == 1) {
                frm.ahtinu[static_cast<std::size_t>(ch)] = r.read(1) != 0;  // chahtinu[ch]
            }
        }
        if (bsi.lfe) {
            int nlferegs = 0;
            for (std::size_t blk = 0; blk < blocks; ++blk) {
                if (frm.lfeexpstr[blk] != ExpStrategy::kReuse) {
                    ++nlferegs;
                }
            }
            if (nlferegs == 1) {
                frm.ahtinu[static_cast<std::size_t>(nfchans)] = r.read(1) != 0;  // lfeahtinu
            }
        }
    }
    if (frm.snroffststr == 0x0) {
        frm.frmcsnroffst = static_cast<int>(r.read(6));
        frm.frmfsnroffst = static_cast<int>(r.read(4));
    }
    if (frm.transproce) {
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto uch = static_cast<std::size_t>(ch);
            frm.chintransproc[uch] = r.read(1) != 0;
            if (frm.chintransproc[uch]) {
                // §2.3.2.22/.23: transprocloc has 4-sample resolution -
                // multiplied out here so every other place this is used
                // works in plain sample counts, matching §3.7.2's own
                // pseudocode (which is written in samples throughout).
                frm.transprocloc[uch] = static_cast<int>(r.read(10)) * 4;
                frm.transproclen[uch] = static_cast<int>(r.read(8));
            }
        }
    }
    if (spxattene) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {  // chinspxatten[ch]
                frm.spxattencod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(5));
            }
        }
    }
    if (bsi.numblkscod != 0x0 && r.read(1) != 0) {  // blkstrtinfoe
        r.skip(static_cast<std::size_t>(eac3::block_start_info_bits(nblks, bsi.words)));
    }
    return frm;
}

}  // namespace

// Every private data member, following the same pimpl pattern as
// ac3::io::WavStreamReader/Writer and ac3::FrameEncoder. The lazy per-
// substream-slot unique_ptr arrays (delay_/joc_state_/retained_) stay
// exactly as they were - a laziness optimization independent of this pimpl.
struct Eac3Decoder::Impl {
    DecoderConfig config_{};
    // §5.4.2.8/§7.8, applied to the assembled program rather than to each
    // substream: a dependent on its own is half a soundfield, and folding it
    // separately would mean folding something nobody was ever meant to hear.
    // Inert unless DecoderConfig::output asks for something.
    OutputStage output_{};
    // apply_output's and flush()'s own views onto whichever channels are
    // being folded. A member so a steady-state decode allocates nothing.
    std::vector<std::span<float>> au_views_;

    // §E2.3.1.2: "If an AC-3 bit stream is present in the E-AC-3 bit stream,
    // then the AC-3 bit stream shall be processed as an independent substream
    // assigned substream ID 0." Such a frame is AC-3 syntax throughout, so an
    // AC-3 decoder reads it and decode_ac3_core() presents the result as
    // substream (kIndependent, 0) for §E3.8.2 to combine exactly as it
    // combines an Annex E bed.
    //
    // One instance rather than a per-identity slot: a bitstream has exactly
    // one independent substream 0, so there is only ever one core. Holding it
    // here across calls is what gives the core's overlap-add and dither the
    // same continuity delay_ gives every Annex E substream. Lazily allocated
    // for the same reason delay_'s slots are - a FrameDecoder carries 12 KB
    // of overlap-add state, and the streams that never contain a legacy core
    // (every stream this project's own encoder produces) should not pay it.
    std::unique_ptr<FrameDecoder> core_;

    // Per-substream-identity state, indexed by strmtyp * 8 + substreamid: a
    // dependent's id lives in its own numbering space (§E2.3.1.2), so id
    // alone does not identify a substream. strmtyp is a 2-bit field and
    // substreamid a 3-bit one, so the whole key space is [0, 32) and a flat
    // 32-slot array replaces the std::map each of these used to be: O(1)
    // indexing with no tree walk and no node allocation per identity, and -
    // because slot order IS key order - the same ascending iteration
    // flush() always had. The two heavy states stay lazily allocated behind
    // unique_ptr exactly as the map's on-demand nodes were: a 5.1 stream
    // has one identity, and 32 by-value delay slots would pin 384 KB.
    static constexpr std::size_t kSubstreamSlots = 32;
    // At most six coded channels each (3/2 plus LFE); value-initialized
    // (zeroed) at first use, exactly as the map's operator[] created it.
    std::array<std::unique_ptr<std::array<std::array<internal::decode_scalar_t, 256>, 6>>,
               kSubstreamSlots>
        delay_;
    // The exponent each slot's channels' delay halves are stored under
    // (block_norm.hpp); zero in the floating tiers.
    std::array<std::array<int, 6>, kSubstreamSlots> delay_norm_ =
        internal::fresh_delay_norm_slots<kSubstreamSlots, 6>();
    // One per substream identity that has ever carried JOC:
    // oba::joc::reconstruct's own matrix-ramp and per-object/per-channel
    // overlap-add state, so a moving object's audio and the frame-to-frame
    // matrix interpolation both have real continuity instead of restarting
    // cold every frame - see oba::joc::ReconstructionState's own doc comment.
    std::array<std::unique_ptr<oba::joc::ReconstructionState>, kSubstreamSlots> joc_state_;
    // A substream identity's slot engages the first time one of its frames
    // sets transproce, and stays engaged (buffering one frame at a time)
    // for the rest of the stream - see decode_substream's own doc comment.
    std::array<std::optional<DecodedSubstream>, kSubstreamSlots> pending_;
    // decode_access_unit's own assembly cache: a substream identity's
    // RELEASED (by decode_substream) results, oldest first, waiting for
    // every other identity the same call's frames named to also have one -
    // see decode_access_unit's own doc comment. A queue rather than a single
    // slot: one identity can release several times while another is still
    // catching up (a dependent that never uses the tool releases every call,
    // while the independent using it lags by one), and an already-queued,
    // not-yet-assembled result must never be overwritten by a later one for
    // the same identity - that would silently splice two different points
    // in time into one access unit. A vector consumed from the front rather
    // than a deque: the queue is at most a frame or two deep, and an empty
    // vector - unlike some deques - allocates nothing, so 32 idle slots
    // cost nothing.
    std::array<std::vector<DecodedSubstream>, kSubstreamSlots> pending_au_parts_;

    // decode_substream's own per-block IMDCT/enhanced-coupling scratch
    // (PREfast's C6262, alert #63): reused across every (block, channel)
    // iteration of a call instead of stack-declared per iteration, the same
    // reasoning as FrameEncoder's MDCT scratch members. Each is fully
    // overwritten before being read, so nothing needs to persist beyond one
    // decode_substream call - unlike delay_ above, these don't need to be
    // keyed by substream identity.
    std::array<internal::decode_scalar_t, 512> imdct_scratch_{};
    // Enhanced coupling's spectrum, in the store's own type: eac3_tools
    // carries the §3.5.5 routines in both scalars (the double forms are the
    // encoder's), so nothing round-trips through double on the way to
    // `coeffs` - the coefficients are written into the store directly.
    std::array<internal::decode_scalar_t, 256> ecpl_spectrum_real_{};
    std::array<internal::decode_scalar_t, 256> ecpl_spectrum_imag_{};
    // §3.5.5.2/.3's per-bin amplitude and angle, for one channel of one block.
    //
    // Members rather than locals in the reconstruction loop, which is where
    // they were: a std::vector each, constructed and destroyed once per COUPLED
    // CHANNEL per BLOCK. On a 5.1 stream with five channels in the coupling
    // range that is 5 x 6 x 2 = 60 allocations per frame, and the bare-metal
    // probe measured exactly that - 60 per frame in the 1,024-1,535 byte class,
    // a class no other fixture touches at all (bins x sizeof(double) lands
    // there for any usual coupling range). It was 48% of enhanced coupling's
    // whole per-frame churn.
    //
    // std::vector grown on first use, NOT std::array like the two ecpl
    // scratches above it. Those are unconditional members and cost their
    // 4,096 bytes (2,048 at float) on every Eac3Decoder ever built; two more
    // arrays would have added as much again, and the probe measured exactly
    // that - peak heap 233,546 to 237,642, a third of the remaining margin
    // under a ceiling this port has spent a lot of effort getting under.
    //
    // Grown once, at the coupling range's width, and never shrunk, so the
    // steady state still allocates nothing. A stream that never uses enhanced
    // coupling - which is most streams, and notably the object fixture that
    // SETS that peak - pays nothing at all rather than 4 KB it never reads.
    std::vector<internal::decode_scalar_t> ecpl_amp_scratch_;
    std::vector<internal::decode_scalar_t> ecpl_angle_scratch_;
    // decode_substream's frame-lifetime coefficient buffers - the AHT
    // stream store (§3.4: all six blocks decoded at block 0) and the
    // enhanced-coupling channel store (§3.5.5.1: a block's reconstruction
    // reads its neighbors). Owned here for the same reuse reasoning as the
    // scratch above, with one extra property worth the wordier comment:
    // both used to be heap-allocated and zeroed afresh on every call (98 KB
    // per frame, the two largest per-frame heap costs in the decoder)
    // whether or not the stream used either tool. They are sized lazily at
    // first use instead - a stream using neither tool never allocates them
    // - and every read of a reused buffer is made safe at the write site:
    // an AHT stream's slot is cleared before its block-0 decode fills it
    // (bins past its endmant must read zero), and enhanced-coupling reads
    // are whole-array assignments from this call or gated by this call's
    // ecpl_active flags, so a previous frame's contents are never visible.
    std::vector<std::array<std::array<internal::decode_scalar_t, 256>, kBlocksPerFrame>>
        aht_coeffs_;
    std::vector<std::array<internal::decode_scalar_t, 256>> ecpl_all_coeffs_;
    // §7.1.3's packed exponent groups, for one stream of one block.
    //
    // A member, reused by assign(), because the two sites that read it are
    // inside the block loop and each constructed a fresh std::vector - so a
    // 5.1 frame paid one allocation per (stream, block) that sent exponents,
    // measured at 14 to 28 a frame. src/forge/src/decoder/decoder.cpp has done
    // this for AC-3 all along (its own `groups` is declared once at frame scope
    // and assign()ed at both its use sites); this is the same shape, taken one
    // step further to a member so it survives the frame as well as the block.
    //
    // Never read across a call - both sites fill it completely from the
    // bitstream before decode_exponents() sees it - so reuse cannot leak one
    // block's exponents into another's. assign() rather than resize() to keep
    // that explicit, and to match what the vectors it replaced did.
    std::vector<std::uint8_t> exp_groups_;
    // One entry per block: everything decode_substream's second pass (spx
    // synthesis, rematrixing, IMDCT and PCM write) needs from pass one -
    // the .cpp's comment at the use site explains why two passes exist at
    // all. A member for the same churn reason as the buffers above: the
    // per-block geometry copies (chincpl, spxco, the enhanced-coupling
    // index sets...) land in vectors that keep their capacity across
    // frames, and `coeffs` is what pass one parses into directly instead of
    // allocating a fresh 14 KB every block. The
    // enhanced-coupling fields are only assigned under cplinu &&
    // ecplinu_now and only read under the same guard - both flags ARE
    // re-assigned every block - so a reused entry's stale conditional
    // fields are never visible.
    //
    // Field order groups them by what fills them (pass one's per-band
    // arrays first, then the per-block scalar flags pass two reads), not by
    // size - reordering for the analyzer's 0-padding layout would scatter
    // that grouping across the struct for no reader benefit.
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
    struct BlockTail {
        // per stream; decoupled where standard. The single largest heap item
        // in an E-AC-3 decode: seven streams x 2,048 bytes x one entry per
        // block is 100,352 bytes, which is why it follows decode_scalar_t.
        std::vector<std::array<internal::decode_scalar_t, 256>> coeffs;
        // Each stream's block exponent (block_norm.hpp): the power of two the
        // coefficients above are scaled up by; all zero in the floating tiers.
        std::array<int, kMaxSubstreamStreams> norm{};
        // Set when pass one already gave a rematrixed pair its shared
        // exponent with room for the sum, so pass two does not take a
        // second bit for the same room.
        bool rematrix_room = false;
        std::vector<bool> chincpl;
        bool cplinu = false;
        bool ecplinu_now = false;
        // Standard coupling (valid when cplinu && !ecplinu_now): decoupling
        // already ran inline in pass one, so `coeffs` is final for these
        // channels and nothing further is needed here.
        //
        // Enhanced coupling (valid when cplinu && ecplinu_now):
        int firstchincpl = -1;
        bool ecplangleintrp = false;
        int ecpl_begin_subbnd = 0;
        int ecpl_end_subbnd = 0;
        std::array<bool, eac3::kEcplSubBands> ecpl_structure{};
        std::vector<std::vector<int>> ecplamp_raw;    // [ch][band]
        std::vector<std::vector<int>> ecplangle_raw;  // [ch][band]
        std::vector<std::vector<int>> ecplchaos_raw;  // [ch][band]
        std::vector<bool> ecpltrans;                  // [ch]
        int cplstrtmant = 0;
        int cplendmant = 0;
        // spx (§3.6)
        bool spxinu = false;
        std::vector<bool> chinspx;
        eac3::BandLayout spx_bands{};
        std::vector<std::vector<internal::decode_scalar_t>> spxco;
        // The fixed-point tier's split of a coordinate: spxco holds the
        // mantissa and this its power of two; empty in the floating tiers.
        std::vector<std::vector<int>> spxco_exp;
        std::vector<int> spxblnd;
        int spx_startmant = 0;
        int spx_endmant = 0;
        int spx_copystart = 0;
        // rematrixing (§7.5.4, 2/0 only) and block switching
        std::array<bool, 4> rematflg{};
        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> blksw{};
        // One slot per coded channel plus the shared coupling stream.
        std::array<int, eac3::chanmap::kMaxSubstreamChannels + 1> endmant{};
    };
    std::vector<BlockTail> tails_;
    // §7.10's raw material, per substream identity: the metadata of the last
    // frame of that identity that decoded, and its last BLOCK's windowed
    // transform output per coded channel. Lazily allocated behind unique_ptr
    // for the same reason delay_ is - 32 by-value slots would pin 768 KB for
    // a stream that has one identity and (usually) no concealment at all.
    struct RetainedSubstream {
        DecodedSubstream shape;
        std::array<std::array<double, 512>, 6> last_block{};
        int nchans = 0;
    };
    std::array<std::unique_ptr<RetainedSubstream>, kSubstreamSlots> retained_;
    // Where the block loop writes its last block while a frame is still in
    // progress, committed into retained_ only once the frame has decoded
    // cleanly - see FrameDecoder's own conceal_scratch_ for why. Sized lazily,
    // so a decoder with concealment off never allocates it.
    std::vector<std::array<double, 512>> conceal_scratch_;
    // The identity of the last frame that decoded, for the one concealment
    // case that cannot name its own: a frame damaged so far forward that even
    // strmtyp/substreamid cannot be trusted. -1 until something decodes.
    int last_identity_ = -1;

    // §7.3.4 dither (Annex E's dithflag[ch]/dithflage), shared across every
    // substream identity decode_substream ever sees - nothing about §7.3.4
    // requires per-identity separation, only that simultaneous channels'
    // noise stay uncorrelated, which independent draws from one sequential
    // generator already give.
    DitherGenerator dither_{};

    // --- per-frame scratch --------------------------------------------------
    // Everything decode_substream_core used to declare as a local before its
    // block loop. The same move tails_, aht_coeffs_, ecpl_all_coeffs_ and
    // exp_groups_ above have already had, and for the same reason: a local is
    // freshly allocated every frame, and at 5.1 this cluster was most of that
    // fixture's per-frame allocation count.
    //
    // WHAT MAKES IT SAFE, which is not automatic. A member carries the previous
    // frame's contents, and stale state leaking across a frame boundary is a
    // fault fixtures do not catch. decode_substream_core therefore
    // re-establishes every one of these at the top of the frame with exactly
    // the arguments its declaration used to carry - assign(n, v) where it read
    // `(n, v)`, clear() where it was default-constructed - so a frame begins
    // with contents identical to before, element for element. Only capacity
    // survives.
    //
    // The nested ones go through reset_nested(), which resizes the outer and
    // clears each inner rather than assign(n, {}): assign would free the inner
    // buffers this exists to keep, and they are where the cost was (cplco,
    // spxco and the three ecpl_*_raw are 1 + nfchans each).
    std::array<std::vector<std::uint8_t>, kMaxSubstreamStreams> exps_;
    std::array<std::vector<std::uint8_t>, kMaxSubstreamStreams> bap_;
    // One per stream: see bitalloc_memo.hpp for what it keeps and why. A
    // block whose inputs are the previous computation's keeps the allocation
    // `bap_` already holds.
    std::array<internal::BitAllocMemo, kMaxSubstreamStreams> bitalloc_memo_;
    std::vector<bool> chincpl_;
    std::vector<int> subband_band_;
    std::vector<bool> cpl_structure_;
    std::vector<std::vector<internal::decode_scalar_t>> cplco_;
    // The fixed-point tier's split of a coordinate: cplco_ holds the mantissa
    // and this its power of two (coupling.hpp's coordinate_exponent). Empty
    // in the floating tiers, whose coordinate carries its own.
    std::vector<std::vector<int>> cplco_exp_;
    std::vector<std::vector<int>> spxco_exp_;
    // An AHT stream's frame exponent (block_norm.hpp): its six blocks are
    // dequantised at once under one, exact from their reconstructed peaks;
    // and per bin, the exponent those peaks are actually below, which is
    // what a tool reading the stream bounds itself by.
    std::array<int, kMaxSubstreamStreams> aht_norm_{};
    std::array<std::vector<int>, kMaxSubstreamStreams> aht_eff_exps_;
    std::vector<bool> phsflg_;
    std::vector<bool> chinspx_;
    std::vector<bool> firstspxcos_;
    std::vector<bool> firstcplcos_;
    std::vector<std::vector<internal::decode_scalar_t>> spxco_;
    std::vector<int> spxblnd_;
    std::vector<std::vector<int>> ecplamp_raw_;
    std::vector<std::vector<int>> ecplangle_raw_;
    std::vector<std::vector<int>> ecplchaos_raw_;
    std::vector<bool> ecpltrans_persist_;
    // parse_coeffs is not here either, and no longer exists: rather than make
    // the swap partner a second member - measured on an ESP32-S3 at 7,168
    // bytes of peak, one 7-stream spectrum buffer live ALONGSIDE the tails_
    // one it used to trade with - pass one now parses straight into the
    // block's own tail and there is nothing to swap. The same zero
    // allocations a frame, with the extra buffer gone rather than relocated.
    std::vector<std::byte> joc_bytes_;
    // §E3.4.4.2's GAQ gains and the bins that carry one, for a single AHT
    // stream of block 0. Declared INSIDE the block loop rather than ahead of
    // it, which is why they are not in the list above - but between them they
    // were the largest per-frame cost left in an AHT decode: one vector<int>
    // as wide as the stream's coded region per stream, and a second grown from
    // empty by push_back.
    //
    // (3) both. aht_gain_ is assign()ed to the Gk=1 default and then only the
    // gain-CARRYING bins are written over, so that default is what every other
    // bin reads back; aht_gain_bins_ carries its state in being empty.
    std::vector<int> aht_gain_;
    std::vector<int> aht_gain_bins_;
    // One block's skipfld, read out bit by bit so it can be handed to
    // emdf::parse_container as a self-contained buffer. (3): filled by
    // push_back from empty, so the clear() is what makes its size mean "this
    // block's skipl bytes" rather than the longest skip field seen so far.
    std::vector<std::byte> skip_bytes_;
};
namespace {

// resize() the outer and clear() each inner, rather than assign(n, {}): same
// observable state - n empty vectors - while keeping the inner allocations.
template <typename T>
void reset_nested(std::vector<std::vector<T>>& v, std::size_t n) {
    v.resize(n);
    for (auto& inner : v) {
        inner.clear();
    }
}

// The std::array flavour, for the per-stream buffers whose outer storage is
// already fixed-size and never allocated.
template <typename T, std::size_t N>
void reset_nested(std::array<std::vector<T>, N>& a) {
    for (auto& inner : a) {
        inner.clear();
    }
}

// §3.5.5.1's spectrum in the coefficient store's scalar - a template for the
// reason scalar_inverse.hpp's inverse_transform_into is one: the float form
// of ecpl_channel_spectrum takes no `fast` (the direct form is double-only),
// and only a template's `if constexpr` discards the call that would not
// compile for the other scalar.
template <typename Scalar>
void ecpl_spectrum_into(const std::array<Scalar, 256>& prev, const std::array<Scalar, 256>& curr,
                        const std::array<Scalar, 256>& next, std::array<Scalar, 256>& zr,
                        std::array<Scalar, 256>& zi, bool fast) {
    if constexpr (std::is_same_v<Scalar, float>) {
        (void)fast;
        eac3::ecpl_channel_spectrum(prev, curr, next, zr, zi);
    } else {
        eac3::ecpl_channel_spectrum(prev, curr, next, zr, zi, fast);
    }
}

// A spectral extension band's RMS in the store's scalar (§E3.6.4.2.4). The
// floating tiers accumulate in their own type, as this always did - a band
// is at most a few dozen bins. The fixed tier sums squared raw units in 64
// bits: a band far below its block's peak has coefficients whose square is
// under a raw unit, and a Q7.24 accumulator would read it as silent and
// blend no noise there.
template <typename Scalar>
struct BandEnergy {
    Scalar accum{0};
    void add(Scalar value) { accum += value * value; }
    [[nodiscard]] Scalar rms(int size) const {
        return internal::scalar_sqrt(accum / static_cast<Scalar>(size));
    }
};

template <>
struct BandEnergy<internal::Fixed32> {
    std::uint64_t sum = 0;
    void add(internal::Fixed32 value) {
        const auto raw = static_cast<std::int64_t>(value.raw);
        sum += static_cast<std::uint64_t>(raw * raw);
    }
    [[nodiscard]] internal::Fixed32 rms(int size) const {
        // raw^2 is the value times 2^48; the root of the mean is the RMS
        // times 2^24, a raw value again.
        return internal::Fixed32::from_raw(static_cast<std::int32_t>(
            internal::isqrt64(sum / static_cast<std::uint64_t>(size))));
    }
};

// A trace field is double whatever the store's scalar is, and the fixed-point
// scalar widens only explicitly: the range assign the two floating tiers could
// use, spelled out per element.
template <typename Range>
void widen_into(std::vector<double>& out, const Range& in) {
    out.resize(std::size(in));
    std::transform(std::begin(in), std::end(in), out.begin(),
                   [](auto v) { return static_cast<double>(v); });
}

// §3.5.5's reconstruction of every coupled channel of one block, in the
// coefficient store's scalar. A function template rather than a plain block
// of the frame decoder for the reason ecpl_spectrum_into gives: only a
// template's `if constexpr` discards the branch whose calls do not exist for
// the other scalar. The fixed-point tier has no fixed form of the spectrum,
// the amplitudes, the angles or the reconstruction yet
// (planning/arithmetic-tiers.md, Phase C), so for it this runs the float forms
// over float copies and narrows the coupled bins back into the store; the
// two floating tiers call the forms in their own scalar directly. `prev` and
// `next` are the neighbouring blocks' enhanced coupling channels, or zero
// (§3.5.5.1); `zr`/`zi` and the two scratch vectors are the decoder's own
// storage, reused block to block.
template <typename Scalar, typename Tail>
void ecpl_reconstruct_block(const Tail& tail, const std::array<Scalar, 256>& prev,
                            const std::array<Scalar, 256>& curr,
                            const std::array<Scalar, 256>& next, int prev_norm, int next_norm,
                            int nfchans,
                            eac3::EcplNoise& ecpl_noise, std::array<Scalar, 256>& zr,
                            std::array<Scalar, 256>& zi, std::vector<Scalar>& amp_scratch,
                            std::vector<Scalar>& angle_scratch, bool fast,
                            std::vector<std::array<Scalar, 256>>& coeffs) {
    const auto ubins = static_cast<std::size_t>(tail.cplendmant - tail.cplstrtmant);
    if constexpr (std::is_same_v<Scalar, internal::Fixed32>) {
        (void)fast;
        // The tier's own stages (eac3_tools_fixed.hpp). The spectrum spans
        // three blocks stored under three exponents and reports the one its
        // bins share; each channel's reconstruction is handed the difference
        // between that and the exponent this channel is stored under, and
        // applies it per bin (block_norm.hpp).
        int spectrum_norm = 0;
        eac3::ecpl_channel_spectrum_fixed(prev, prev_norm, curr,
                                          tail.norm[static_cast<std::size_t>(kCplStream)], next,
                                          next_norm, zr, zi, spectrum_norm);
        for (int ch = 0; ch < nfchans; ++ch) {
            if (!tail.chincpl[static_cast<std::size_t>(ch)]) {
                continue;
            }
            const auto uch = static_cast<std::size_t>(ch);
            const bool is_first = ch == tail.firstchincpl;
            if (amp_scratch.size() < ubins) {
                amp_scratch.resize(ubins);
                angle_scratch.resize(ubins);
            }
            const std::span<Scalar> amp_bin{amp_scratch.data(), ubins};
            const std::span<Scalar> angle_bin{angle_scratch.data(), ubins};
            std::fill(amp_bin.begin(), amp_bin.end(), Scalar{0});
            std::fill(angle_bin.begin(), angle_bin.end(), Scalar{0});
            eac3::ecpl_amplitudes_fixed(tail.ecplamp_raw[uch], tail.ecplchaos_raw[uch],
                                        tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                        tail.ecpl_end_subbnd, tail.ecpl_structure, amp_bin);
            eac3::ecpl_angles_fixed(ch, tail.ecplangle_raw[uch], tail.ecplchaos_raw[uch],
                                    tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                    tail.ecpl_end_subbnd, tail.ecpl_structure, ecpl_noise,
                                    angle_bin, tail.ecplangleintrp);
            eac3::ecpl_channel_coefficients_fixed(zr, zi, amp_bin, angle_bin, tail.cplstrtmant,
                                                  tail.cplendmant,
                                                  tail.norm[uch] - spectrum_norm, coeffs[uch]);
        }
    } else {
        (void)prev_norm;
        (void)next_norm;
        ecpl_spectrum_into(prev, curr, next, zr, zi, fast);
        for (int ch = 0; ch < nfchans; ++ch) {
            if (!tail.chincpl[static_cast<std::size_t>(ch)]) {
                continue;
            }
            const auto uch = static_cast<std::size_t>(ch);
            const bool is_first = ch == tail.firstchincpl;
            // Grow to the widest coupling range seen, never shrink. The range
            // is a property of the stream rather than of the block, so in
            // practice this allocates on the first coupled block of the first
            // frame and never again.
            if (amp_scratch.size() < ubins) {
                amp_scratch.resize(ubins);
                angle_scratch.resize(ubins);
            }
            // Zeroed to the width in use before each call, because the two
            // callees write only the bins their band structure covers and the
            // vectors these replaced were value-initialised. Reusing storage
            // means that is no longer implied by the construction, so it is
            // done here - a few hundred stores against an allocation and a
            // free.
            const std::span<Scalar> amp_bin{amp_scratch.data(), ubins};
            const std::span<Scalar> angle_bin{angle_scratch.data(), ubins};
            std::fill(amp_bin.begin(), amp_bin.end(), Scalar{0});
            std::fill(angle_bin.begin(), angle_bin.end(), Scalar{0});
            eac3::ecpl_amplitudes(tail.ecplamp_raw[uch], tail.ecplchaos_raw[uch],
                                  tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                  tail.ecpl_end_subbnd, tail.ecpl_structure, amp_bin);
            eac3::ecpl_angles(ch, tail.ecplangle_raw[uch], tail.ecplchaos_raw[uch],
                              tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                              tail.ecpl_end_subbnd, tail.ecpl_structure, ecpl_noise, angle_bin,
                              tail.ecplangleintrp);
            // Straight into the store: the callee writes only
            // [cplstrtmant, cplendmant), so the bins outside it stand as
            // decoded. This used to round-trip through a double scratch,
            // because the routine existed only at double; on an ESP32-S3 that
            // copy alone was 512 software conversions per coupled channel per
            // block.
            eac3::ecpl_channel_coefficients(zr, zi, amp_bin, angle_bin, tail.cplstrtmant,
                                            tail.cplendmant, coeffs[uch]);
        }
    }
}

}  // namespace


Eac3Decoder::Eac3Decoder() : impl_(std::make_unique<Impl>()) {}

Eac3Decoder::~Eac3Decoder() = default;
Eac3Decoder::Eac3Decoder(Eac3Decoder&&) noexcept = default;
Eac3Decoder& Eac3Decoder::operator=(Eac3Decoder&&) noexcept = default;

int Eac3Decoder::output_latency_samples() const {
    return impl_->output_.latency_samples();
}

Eac3Decoder::Eac3Decoder(const DecoderConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->config_ = internal::resolve_operating_mode(config);
    impl_->output_ = OutputStage(config.output);
}

// §E2.3.1.2: "If an AC-3 bit stream is present in the E-AC-3 bit stream, then
// the AC-3 bit stream shall be processed as an independent substream assigned
// substream ID 0." FrameDecoder does the reading, since the frame is AC-3
// syntax throughout; this is the presentation layer that lets §E3.8.2's
// combining treat the result as the bed with no special case downstream of
// here.
std::expected<DecodedSubstream, DecodeError> Eac3Decoder::decode_ac3_core(
    std::span<const std::byte> frame) {
    if (!impl_->core_) {
        impl_->core_ = std::make_unique<FrameDecoder>(impl_->config_);
    }
    auto decoded = impl_->core_->decode_frame(frame);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    DecodedSubstream out;
    out.strmtyp = StreamType::kIndependent;
    out.substreamid = 0;
    out.sample_rate = decoded->sample_rate;
    out.acmod = decoded->acmod;
    out.lfe = decoded->lfe;
    out.dialnorm = decoded->dialnorm;
    out.compr = decoded->compr;
    out.dynrng = decoded->dynrng;
    out.dialnorm2 = decoded->dialnorm2;
    out.compr2 = decoded->compr2;
    out.dynrng2 = decoded->dynrng2;
    // An AC-3 syncframe is always six audblks (§5.3.1), which Annex E spells
    // numblkscod 3. That is also what every dependent riding alongside a core
    // must carry - §E2.3.1.2 requires a dependent to have "the same number of
    // blocks per syncframe" as its independent substream - so leaving it at
    // the DecodedSubstream default would make decode_access_unit_core's own
    // agreement check pass by luck rather than by matching.
    out.numblkscod = 3;
    // §E2.3.1.8: only a dependent substream may carry a custom channel map, so
    // the core's locations come from acmod/lfeon. Left std::nullopt to say so -
    // location_map() falls back to acmod_map() on exactly that.
    out.chanmap = std::nullopt;
    // §E3.8.5's last-dependent marker is a dependent's repurposed compre bit;
    // an AC-3 frame's compre means what it always meant, and is reported as
    // `compr` above.
    out.last_dependent = false;
    out.blksw = std::move(decoded->blksw);
    out.channels = std::move(decoded->channels);
    // Object audio never rides in an AC-3 core: TS 103 420 puts the marker in
    // addbsi and the container in a block skip field, both Annex E syntax. A
    // legacy-core Atmos delivery carries them in a dependent instead, which
    // decode_access_unit_core picks up from whichever substream has them.
    return out;
}

std::expected<std::optional<DecodedSubstream>, DecodeError> Eac3Decoder::decode_substream(
    std::span<const std::byte> frame) {
    auto decoded = decode_substream_core(frame);
    if (decoded) {
        return decoded;
    }
    if (impl_->config_.concealment == ConcealmentPolicy::kNone) {
        return decoded;
    }
    // Which identity's history to reconstruct from. strmtyp and substreamid
    // sit at fixed positions immediately after the sync word, so a frame
    // damaged anywhere past its header still names itself - which is the
    // shape nearly all transport corruption takes, and the CRC failure that
    // brought us here says nothing about where in the frame the damage is.
    //
    // The identity is NOT second-guessed when that read succeeds. An earlier
    // version fell back on the last identity that decoded whenever the named
    // one had no retained block, which sounds forgiving and is actively
    // wrong: the first dependent of a stream would then be reconstructed from
    // the BED's history, producing a six-channel "dependent" that the §E3.8.2
    // assembly rightly refuses. Returning the error instead lets
    // decode_access_unit_core do the right thing with it - render the bed
    // alone - which is a real answer rather than a plausible-looking wrong
    // one.
    std::size_t slot = 0;
    BitReader peek{frame};
    if (frame.size() >= 5 && peek.read(16) == kSyncWord) {
        const auto strmtyp = peek.read(2);
        const auto substreamid = peek.read(3);
        slot = static_cast<std::size_t>(strmtyp * 8 + substreamid);
    } else if (impl_->last_identity_ >= 0) {
        // No usable header at all: the last identity that decoded is the only
        // guess available, and it is the right one for the single-identity
        // stream that covers nearly every case.
        slot = static_cast<std::size_t>(impl_->last_identity_);
    } else {
        return decoded;
    }
    if (auto concealed = conceal(decoded.error(), slot)) {
        return concealed;
    }
    return decoded;
}

std::optional<DecodedSubstream> Eac3Decoder::conceal(DecodeError error, std::size_t slot) {
    const auto& retained = impl_->retained_[slot];
    // Nothing retained for this identity means the loss is at the head of it:
    // there is no previous block to reconstruct from, and inventing one would
    // be substituting audio rather than concealing a gap in it.
    if (!retained) {
        return std::nullopt;
    }
    const bool repeat = impl_->config_.concealment == ConcealmentPolicy::kRepeatFade;
    const int nchans = retained->nchans;

    DecodedSubstream out = retained->shape;
    out.dynrng.fill(meta::kDynrngUnity);
    out.dynrng2.fill(meta::kDynrngUnity);
    // A concealed frame carries no object layer: OAMD and JOC describe THIS
    // frame's objects, and repeating the previous frame's positions would put
    // moving objects somewhere they demonstrably are not.
    out.object_metadata = std::nullopt;
    out.object_audio.clear();

    const int nblks = eac3::blocks_per_syncframe(out.numblkscod);
    out.channels.assign(
        static_cast<std::size_t>(nchans),
        std::vector<float>(static_cast<std::size_t>(nblks * kSamplesPerBlock), 0.0f));

    auto& delay_slot = impl_->delay_[slot];
    if (!delay_slot) {
        delay_slot = std::make_unique<std::array<std::array<internal::decode_scalar_t, 256>, 6>>();
    }
    auto& delay = *delay_slot;
    auto& delay_norm = impl_->delay_norm_[slot];

    // 20 dB across six blocks, the same decay FrameDecoder::conceal uses - a
    // syncframe coding fewer blocks simply travels less of that curve, which
    // is the right relationship: the decay is per unit of TIME lost, and a
    // one-block syncframe loses a sixth as much of it.
    constexpr double kDecayPerBlock = 0.6812920690579611;  // 10^(-20/(20*6))
    double gain = kDecayPerBlock;
    for (int blk = 0; blk < nblks; ++blk) {
        for (int ch = 0; ch < nchans; ++ch) {
            const auto uch = static_cast<std::size_t>(ch);
            const auto& last = retained->last_block[uch];
            auto& history = delay[uch];
            auto& pcm = out.channels[uch];
            // The delay half's exponent (block_norm.hpp): read through it,
            // and zero once this loop has written true-scale values.
            const int history_norm = delay_norm[uch];
            delay_norm[uch] = repeat ? 0 : internal::kNormCeiling;
            for (int n = 0; n < kSamplesPerBlock; ++n) {
                const auto un = static_cast<std::size_t>(n);
                const double head = repeat ? last[un] * gain : 0.0;
                pcm[static_cast<std::size_t>(blk * kSamplesPerBlock + n)] =
                    static_cast<float>(2.0 * (head + internal::widen_stored(history[un], history_norm)));
                history[un] = repeat ? static_cast<internal::decode_scalar_t>(last[un + 256] * gain) : internal::decode_scalar_t{0};
            }
        }
        gain *= kDecayPerBlock;
    }
    if (repeat) {
        const double carried = gain / kDecayPerBlock;
        for (int ch = 0; ch < nchans; ++ch) {
            for (double& value : retained->last_block[static_cast<std::size_t>(ch)]) {
                value *= carried;
            }
        }
    } else {
        for (int ch = 0; ch < nchans; ++ch) {
            retained->last_block[static_cast<std::size_t>(ch)].fill(0.0);
        }
    }

    out.concealed =
        Concealment{.error = error,
                    .action = repeat ? ConcealmentAction::kRepeatFade : ConcealmentAction::kMute};
    return out;
}

std::expected<std::optional<DecodedSubstream>, DecodeError> Eac3Decoder::decode_substream_core(
    std::span<const std::byte> frame) {
    AC3_ZONE_SCOPED_N("eac3_decode_substream");
    // Before the first early return, for the same reason FrameDecoder resets
    // its own: a caller reusing one trace across a file must never read a
    // previous frame's state out of a call that decoded nothing.
    if (impl_->config_.syntax != nullptr) {
        impl_->config_.syntax->reset();
    }
    // The direct-form (reference) transform is a CMake-selected translation
    // unit, and the minimum-footprint decoder profile leaves its 1.81 MiB of
    // tables out of the build (roadmap PF7; src/core/reference_transform.hpp).
    // Asking for it there is refused rather than silently served by the fast
    // path: fast_imdct == false exists so a caller can validate against the
    // arithmetic the spec writes down, and substituting a different one would
    // defeat the only reason to set it. Constant-folded away in every ordinary
    // build, where kReferenceTransformAvailable is true.
    if (!impl_->config_.fast_imdct && !internal::kReferenceTransformAvailable) {
        return std::unexpected(DecodeError::kUnsupported);
    }
    if (frame.size() < 8) {
        return std::unexpected(DecodeError::kTruncated);
    }
    // §E2.3.1.2's legacy core, before anything below reads a field that means
    // something different in AC-3. In particular the crc2 check: AC-3 has no
    // crc2: bytes 2-3 are crc1, and its error check is that word plus the 5/8
    // checkpoint, which FrameDecoder does itself.
    if (const auto bsid = stream_bsid(frame); bsid && *bsid <= 8) {
        auto core = decode_ac3_core(frame);
        if (!core.has_value()) {
            return std::unexpected(core.error());
        }
        return std::optional<DecodedSubstream>(std::move(*core));
    }
    // There is no crc1 in E-AC-3 and no 5/8 checkpoint to protect, so crc2 is
    // the whole error check: the register reads zero over the frame past the
    // sync word, its own two bytes included.
    if (crc16(frame.subspan(2)) != 0) {
        // See FrameDecoder::decode_frame_core's own comment on why this is
        // reported here and not only via the returned error.
        if (impl_->config_.diagnostics != nullptr) {
            impl_->config_.diagnostics({.event = DiagnosticEvent::kCrcMismatch},
                                       impl_->config_.diagnostics_context);
        }
        return std::unexpected(DecodeError::kBadCrc);
    }

    BitReader r{frame};
    const auto bsi = parse_bsi(r, frame.size());
    if (!bsi.has_value()) {
        return std::unexpected(bsi.error());
    }
    const int nblks = eac3::blocks_per_syncframe(bsi->numblkscod);
    const int nfchans = fullbw_channel_count(bsi->acmod);
    const int nchans = nfchans + (bsi->lfe ? 1 : 0);

    const auto frm = parse_audfrm(r, *bsi, nblks);
    if (!frm) {
        return std::unexpected(frm.error());
    }
    // The syntax trace's frame-wide half (ac3/decoder/syntax_trace.hpp).
    // Everything here comes straight off Table E1.3's audfrm section, which
    // is exactly why an Annex E dump is worth having at all: the frame
    // decides most of what the blocks are allowed to say, so "no delta bit
    // allocation in this frame" means something different depending on
    // whether dbaflde was clear or every block simply declined.
    if (impl_->config_.syntax != nullptr) {
        auto& syn = *impl_->config_.syntax;
        syn.valid = true;
        syn.fbw_channels = nfchans;
        syn.lfe = bsi->lfe;
        syn.block_count = nblks;
        syn.transient_prenoise = frm->transproce;
        syn.block_switch_enabled = frm->blkswe;
        syn.dither_enabled = frm->dithflage;
        syn.bamode = frm->bamode;
        syn.delta_bit_alloc_enabled = frm->dbaflde;
        syn.skip_enabled = frm->skipflde;
        // §E3.6.4.2.3: spxattene itself only matters through the per-channel
        // codes it gates, and parse_audfrm leaves those at -1 for a channel
        // that does not attenuate - so "some channel attenuates" is both what
        // the flag was for and what a reader wants to know.
        syn.spx_attenuation_enabled =
            std::ranges::any_of(frm->spxattencod, [](int code) { return code >= 0; });
        syn.snroffststr = frm->snroffststr;
        syn.per_block_exp_strategy = frm->expstre;
        for (int stream = 0; stream < kMaxSubstreamStreams; ++stream) {
            const int slot = stream == kCplStream ? kCouplingSyntaxStream : stream;
            syn.aht_stream[static_cast<std::size_t>(slot)] =
                frm->ahtinu[static_cast<std::size_t>(stream)];
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            if (frm->chintransproc[static_cast<std::size_t>(ch)]) {
                syn.transient_prenoise_channels |= static_cast<std::uint8_t>(1U << ch);
            }
        }
    }
    // §E2.3.1.8: a chanmap that does not account for exactly the channels
    // acmod and lfeon code would put audio in the wrong speakers rather than
    // fail to parse, so it has to be caught explicitly.
    if (bsi->chanmap.has_value() && eac3::chanmap::channel_count(*bsi->chanmap) != nchans) {
        return std::unexpected(DecodeError::kInvalidStream);
    }

    // The self-check's decoder-side view (ac3/verify/eac3_mirror.hpp). Opened
    // here, once bsi and audfrm have both parsed - everything below is filled
    // INCREMENTALLY, so a frame this call ends up refusing still leaves
    // behind everything it managed to read, which is the case the comparison
    // is most useful in. An independent substream starts a fresh access unit,
    // the same rule split_access_units delimits them by.
    verify::Eac3SubstreamTrace* trace = nullptr;
    if (impl_->config_.eac3_trace != nullptr) {
        trace =
            &impl_->config_.eac3_trace->begin_substream(bsi->strmtyp == StreamType::kIndependent);
        trace->strmtyp = bsi->strmtyp;
        trace->substreamid = bsi->substreamid;
        trace->blocks_coded = nblks;
        trace->fbw_channels = nfchans;
        trace->coded_channels = nchans;
        trace->transproce = frm->transproce;
        if (frm->transproce) {
            const auto count = static_cast<std::size_t>(nfchans);
            trace->chintransproc.assign(
                frm->chintransproc.begin(),
                frm->chintransproc.begin() + static_cast<std::ptrdiff_t>(count));
            trace->transprocloc.assign(
                frm->transprocloc.begin(),
                frm->transprocloc.begin() + static_cast<std::ptrdiff_t>(count));
            trace->transproclen.assign(
                frm->transproclen.begin(),
                frm->transproclen.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }

    DecodedSubstream out;
    out.strmtyp = bsi->strmtyp;
    out.substreamid = bsi->substreamid;
    out.bsid = bsi->bsid;
    out.bsmod = bsi->bsmod;
    out.sample_rate = bsi->sample_rate;
    out.acmod = bsi->acmod;
    out.lfe = bsi->lfe;
    out.dialnorm = bsi->dialnorm;
    out.compr = bsi->compr;
    out.dynrng.fill(meta::kDynrngUnity);
    out.dialnorm2 = bsi->dialnorm2;
    out.compr2 = bsi->compr2;
    out.dynrng2.fill(meta::kDynrngUnity);
    out.numblkscod = bsi->numblkscod;
    out.mixing = bsi->mixing;
    out.info = bsi->info;
    out.chanmap = bsi->chanmap;
    out.last_dependent = bsi->strmtyp == StreamType::kDependent && bsi->compre;
    out.blksw.assign(static_cast<std::size_t>(nfchans), {});
    // impl_->config_.skip_reconstruction stops before the second pass below, so
    // nothing ever writes these - see that option's own comment.
    if (!impl_->config_.skip_reconstruction) {
        out.channels.assign(
            static_cast<std::size_t>(nchans),
            std::vector<float>(static_cast<std::size_t>(nblks * kSamplesPerBlock), 0.0f));
    }

    // §7.10: whether the block loop below has to keep its last block for a
    // future loss of THIS identity to be reconstructed from. Skipped entirely
    // with concealment off, which is what keeps a decoder configured the way
    // every existing caller configures it from carrying 24 KB per identity it
    // will never read.
    const bool retain_last_block = impl_->config_.concealment != ConcealmentPolicy::kNone;
    if (retain_last_block) {
        impl_->conceal_scratch_.assign(static_cast<std::size_t>(nchans), std::array<double, 512>{});
    }

    // §E2.3.1.2: a dependent's substreamid starts again at 0 in its own space,
    // so identity - and hence which overlap-add history belongs to this frame
    // - is the pair, never the id alone. First use of an identity engages
    // its slot value-initialized (zeroed history), exactly as the map's
    // operator[] this replaced created it.
    const auto delay_index =
        static_cast<std::size_t>(static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid);
    auto& delay_slot = impl_->delay_[delay_index];
    if (!delay_slot) {
        delay_slot = std::make_unique<std::array<std::array<internal::decode_scalar_t, 256>, 6>>();
    }
    auto& delay = *delay_slot;
    auto& delay_norm = impl_->delay_norm_[delay_index];

    std::array<int, kMaxSubstreamStreams> endmant{};
    // Per-frame scratch owned by Impl, re-established here with exactly the
    // arguments these declarations used to carry - see Impl's own note.
    auto& exps = impl_->exps_;
    reset_nested(exps);
    auto& bap = impl_->bap_;
    reset_nested(bap);
    // The allocation memo describes what `bap` holds, and `bap` was just
    // emptied: every stream's first block this frame recomputes. What the memo
    // saves is the blocks after it, where E-AC-3 reuses exponents.
    for (auto& memo : impl_->bitalloc_memo_) {
        memo.valid = false;
    }
    // Only meaningful when block_trace != nullptr below (roadmap AP12) - see
    // decoder.cpp's identical comment on its own `mask` array.
    std::array<std::array<int, 50>, kMaxSubstreamStreams> mask{};
    BitAllocCodes codes = kBamode0Codes;
    std::array<int, kMaxSubstreamStreams> fgaincod{};
    fgaincod.fill(kBamode0Codes.fgaincod);
    std::array<int, kMaxSubstreamStreams> fsnroffst{};
    int csnroffst = 0;
    std::array<bool, 4> rematflg{};
    // §7.7.1.2: an absent word inherits from the previous BLOCK, and block 0
    // without one is unity - same persistence rule as the legacy AC-3
    // decoder's own dynrng_word/dynrng2_word (decoder.cpp).
    std::uint8_t dynrng_word = meta::kDynrngUnity;
    std::uint8_t dynrng2_word = meta::kDynrngUnity;
    // §7.2.2.6, reset to "no segments" at the start of every syncframe like
    // fsnroffst/codes above, then persisting block to block until
    // re-transmitted or cleared. Indexed by STREAM, so the coupling channel's
    // own `cpldeltbae` segments live at kCplStream alongside the fbw
    // channels' deltbae[ch] ones. The LFE slot is never written:
    // §5.4.3.49/E2.3.2.9 bound their deltbae[ch] loop by nfchans, so no
    // bitstream field for it exists.
    std::array<DeltaSegments, kMaxSubstreamStreams> delta{};

    // Coupling state (Annex E variant of §7.4). All of it persists until
    // re-transmitted - this encoder only ever (re)sends geometry in block 0,
    // but a general stream could resend it on any block whose cplstre is set.
    bool phsflginu = false;
    int cplbegf = 0;
    int ecplbegf = 0;  // persists like cplbegf; needed again by nrematbd below
    int cplstrtmant = 0;
    int cplendmant = 0;
    int ncplbnd = 0;
    int cplfleak = 0;
    int cplsleak = 0;
    auto& chincpl = impl_->chincpl_;
    chincpl.assign(static_cast<std::size_t>(nfchans), false);
    // Which coupling band each sub-band belongs to (cplbndstrc expansion).
    auto& subband_band = impl_->subband_band_;
    subband_band.clear();
    // The cplbndstrc[] merge flags themselves, indexed relative to this
    // block's cplbegf. Kept for the whole frame because a later block may
    // reuse them - see spx_structure_set below for the rule all three band
    // structures share.
    auto& cpl_structure = impl_->cpl_structure_;
    cpl_structure.clear();
    // [channel][sub-band] - already expanded from bands to sub-bands.
    auto& cplco = impl_->cplco_;
    reset_nested(cplco, static_cast<std::size_t>(nfchans));
    // The fixed-point tier's coordinate exponents (block_norm.hpp), this
    // one and spxco_exp_ below. Only that tier reads them, so only that tier
    // sizes, fills and snapshots them: in the floating tiers they stay
    // empty. Sized unconditionally they cost every build a deep copy into
    // each block's tail and, on a programme whose substreams differ in
    // channel count, reallocations every frame as reset_nested shrank and
    // regrew them - measured on the default build's probe, 7.1.4 went from
    // 35 allocations a frame to 42.
    auto& cplco_exp = impl_->cplco_exp_;
    if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
        reset_nested(cplco_exp, static_cast<std::size_t>(nfchans));
    }
    auto& phsflg = impl_->phsflg_;
    phsflg.clear();

    // Spectral extension state (§3.6). Persists until re-transmitted, same as
    // coupling above - this encoder only ever (re)sends geometry in block 0.
    // Unlike coupling's per-sub-band coordinates, spx coordinates are one per
    // BAND already (no sub-band duplication step), so spx_bands/spxco are
    // indexed by band directly throughout.
    bool spxinu = false;
    auto& chinspx = impl_->chinspx_;
    chinspx.assign(static_cast<std::size_t>(nfchans), false);
    int spxstrtf = 0;
    int spxbegf = 0;
    int spx_startmant = 0;  // spx_band_start(spx_begin_subbnd) - extension begins here
    int spx_endmant = 0;    // spx_band_start(spx_end_subbnd) - one past the last bin
    int spx_copystart = 0;  // spx_band_start(spxstrtf) - copy-up wraps back to here
    eac3::BandLayout spx_bands{};
    // spxbndstrc, relative to the region's first sub-band (as group_bands
    // wants it), frame-lifetime for the same reason cpl_structure is.
    std::array<bool, eac3::kSpxSubBands> spx_structure{};
    // §E2.3.3.7/.15/.18: spxbndstrce, cplbndstrce and ecplbndstrce all mean
    // "the band structure follows" when set. When CLEAR they mean one of two
    // different things depending on where in the frame they appear: the
    // DEFAULT table (Tables E2.11/E2.12/E2.13) in the first block that uses
    // that tool, and the PREVIOUS BLOCK's structure in every later one.
    // Taking the default table every time the bit is clear is wrong for the
    // second case, and wrong silently: the band count changes, so the
    // coordinates that follow are read into the wrong bands and every field
    // after them is at the wrong bit offset. Nothing this project's own
    // encoder or FFmpeg's produces reaches it - both send the geometry once,
    // in block 0, and never resend it - while a Dolby Encoding Engine 6.5.4
    // stream resends coupling geometry mid-frame with cplbndstrce clear; see
    // tests/golden/external-baseline/eac3-51-256/dee.ec3 and the third-party
    // interop checks in tools/checks/verify_gold_reference.sh.
    bool spx_structure_set = false;
    bool cpl_structure_set = false;
    bool ecpl_structure_set = false;

    // §E2.3.2.28-30: the "first time this frame" states, all initialised at
    // audfrm's end and then maintained by the blocks. They are what makes
    // block 0 cheaper than AC-3's - spxcoe, cplcoe and cplleake are implied
    // there rather than transmitted - but they are per-frame, per-channel
    // STATE, not a synonym for "blk == 0": a block in which a channel is not
    // in spectral extension (or not in coupling) sets that channel's flag
    // back to 1, so the block where it joins or rejoins implies its
    // coordinates again rather than transmitting an exist bit. Reading that
    // absent bit is a one-bit desynchronisation of everything after it.
    // Nothing this project's own encoder or FFmpeg's produces reaches it -
    // both couple the same channels in every block of every frame - while a
    // Dolby Encoding Engine 6.5.4 stream brings channels into coupling
    // part-way through a frame; see the third-party interop checks in
    // tools/checks/verify_gold_reference.sh.
    auto& firstspxcos = impl_->firstspxcos_;
    firstspxcos.assign(static_cast<std::size_t>(nfchans), true);
    auto& firstcplcos = impl_->firstcplcos_;
    firstcplcos.assign(static_cast<std::size_t>(nfchans), true);
    bool firstcplleak = true;
    // [channel][band]
    auto& spxco = impl_->spxco_;
    reset_nested(spxco, static_cast<std::size_t>(nfchans));
    auto& spxco_exp = impl_->spxco_exp_;
    if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
        reset_nested(spxco_exp, static_cast<std::size_t>(nfchans));
    }
    auto& spxblnd = impl_->spxblnd_;
    spxblnd.assign(static_cast<std::size_t>(nfchans), 0);
    eac3::SpxNoise spx_noise;

    // AHT-decoded coefficients (§3.4), one array of all six blocks per
    // stream. Unlike every other per-stream array above, these are produced
    // once - at block 0, since an AHT stream's mantissas exist only there -
    // rather than block by block, so they need their own frame-lifetime
    // buffer distinct from the per-block-local `coeffs` below. The buffer
    // itself lives on the decoder (see the members' comment in decoder.hpp):
    // decode_aht_stream sizes it at first AHT use and clears a stream's
    // slot before filling it, so a stream that never uses AHT never pays
    // the 86 KB, and a reused slot can never leak a previous frame's bins.
    auto& aht_coeffs = impl_->aht_coeffs_;

    // Enhanced coupling state (§E3.5), parallel to the standard-coupling
    // state above and mutually exclusive with it per block (ecplinu picks
    // one or the other). Persistence follows the same convention: geometry
    // and coordinates persist until re-transmitted.
    bool ecplinu_now = false;
    int ecpl_begin_subbnd = 0;
    int ecpl_end_subbnd = 0;
    // Indexed absolutely by sub-band, same convention as kDefaultEcplBandStructure.
    // std::array rather than vector<bool>: it needs to convert to
    // std::span<const bool> at the call sites below, which vector<bool>'s
    // bitset specialization cannot do.
    std::array<bool, eac3::kEcplSubBands> ecpl_structure{};
    bool ecplangleintrp = false;
    // [channel][band] - the raw transmitted indices, persisted until
    // re-sent. Kept as indices (not decoded values) because decoding
    // depends on a channel's role this block (is-first-channel, ecpltrans),
    // which can only be resolved once chincpl for THIS block is known.
    auto& ecplamp_raw = impl_->ecplamp_raw_;
    reset_nested(ecplamp_raw, static_cast<std::size_t>(nfchans));
    auto& ecplangle_raw = impl_->ecplangle_raw_;
    reset_nested(ecplangle_raw, static_cast<std::size_t>(nfchans));
    auto& ecplchaos_raw = impl_->ecplchaos_raw_;
    reset_nested(ecplchaos_raw, static_cast<std::size_t>(nfchans));
    auto& ecpltrans_persist = impl_->ecpltrans_persist_;
    ecpltrans_persist.assign(static_cast<std::size_t>(nfchans), false);
    eac3::EcplNoise ecpl_noise;
    // Every block's enhanced coupling channel raw mantissas (§3.5.5.1's
    // XCURR), stashed as each block is parsed so the second pass below can
    // look at any block's neighbors freely - a block whose neighbor did not
    // use enhanced coupling substitutes zero there (`ecpl_active`), exactly
    // the rule §3.5.5.1 itself specifies. This also covers this syncframe's
    // own first/last block, whose true neighbor lives in an adjacent
    // syncframe this call was not given: a real, documented approximation,
    // not a bug - every interior block reconstructs with its true
    // neighbors.
    // Heap-allocated like aht_coeffs above (PREfast's C6262, alert #63): a
    // fixed std::array here was the single largest contributor to
    // decode_substream's oversized stack frame. Like aht_coeffs it lives on
    // the decoder, sized lazily at the first block that stashes into it:
    // every read is either a whole-array assignment made this call or gated
    // by this call's ecpl_active flags, so nothing stale is ever visible,
    // and a stream that never uses enhanced coupling never allocates it.
    auto& ecpl_all_coeffs = impl_->ecpl_all_coeffs_;
    std::array<bool, kBlocksPerFrame> ecpl_active{};

    // Everything the second pass below (spx synthesis, rematrixing, IMDCT
    // and PCM write) needs from this block, captured once bitstream parsing
    // for it is done. A second pass is unavoidable rather than finishing
    // each block inline as AC-3's decoder and this decoder's OTHER tools
    // all do: enhanced coupling's channel reconstruction (§3.5.5.1) needs
    // the block AFTER the one it reconstructs, which is not available until
    // that later block has itself been parsed - and IMDCT's overlap-add
    // delay line is strictly sequential across blocks, so finishing block N
    // before block N-1 is not an option either. Capturing every block's
    // state (not only the enhanced-coupling ones) keeps that sequencing
    // simple: one pass parses everything in the bitstream's own order, the
    // next finishes every block, still in order 0..nblks-1.
    // The struct itself (BlockTail) and its storage live on the decoder so
    // the per-block copies below reuse capacity across frames - see the
    // members' comment in decoder.hpp. resize() keeps prior entries'
    // storage; every unconditional field is re-assigned per block, and the
    // conditional (enhanced-coupling) fields are read only under the same
    // guard they are written under.
    auto& tails = impl_->tails_;
    tails.resize(static_cast<std::size_t>(nblks));

    // Captured alongside out.object_metadata below, from whichever block's
    // skip field carries the EMDF container - kept raw here (not parsed
    // yet) because oba::joc::parse_payload needs FrameParameters::objects to
    // agree with the OAMD program it rides beside, which is only known once
    // both payloads have been seen.
    auto& joc_bytes = impl_->joc_bytes_;
    joc_bytes.clear();

    for (int blk = 0; blk < nblks; ++blk) {
        AC3_ZONE_SCOPED_N("eac3_parse_block");
        verify::Eac3BlockTrace* block_trace = nullptr;
        if (trace != nullptr) {
            block_trace = &trace->blocks[static_cast<std::size_t>(blk)];
            block_trace->entered = true;
            // The localiser, taken before this block's first field is read -
            // the same instant the encoder takes its own.
            block_trace->bit_offset = r.bit_position();
        }
        const auto strategy = [&](int ch) {
            return ch < nfchans
                       ? frm->chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)]
                       : frm->lfeexpstr[static_cast<std::size_t>(blk)];
        };
        // The syntax trace's per-block half. Unlike AC-3's, most of what goes
        // in it was settled in audfrm before this loop began, so the strategies
        // and cplinu can be recorded on entry rather than as they are read.
        BlockSyntax* syntax = impl_->config_.syntax != nullptr
                                  ? &impl_->config_.syntax->blocks[static_cast<std::size_t>(blk)]
                                  : nullptr;
        if (syntax != nullptr) {
            syntax->entered = true;
            syntax->coupling = frm->cplinu[static_cast<std::size_t>(blk)];
            for (int ch = 0; ch < nchans; ++ch) {
                syntax->exp_strategy[static_cast<std::size_t>(ch)] = strategy(ch);
            }
            if (syntax->coupling) {
                syntax->exp_strategy[kCouplingSyntaxStream] =
                    frm->cplexpstr[static_cast<std::size_t>(blk)];
            }
        }

        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> blksw{};
        if (frm->blkswe) {
            for (int ch = 0; ch < nfchans; ++ch) {
                blksw[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            }
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            out.blksw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(blk)] =
                blksw[static_cast<std::size_t>(ch)];
            if (syntax != nullptr && blksw[static_cast<std::size_t>(ch)]) {
                syntax->block_switch |= static_cast<std::uint8_t>(1U << ch);
            }
        }
        // Annex E Table E1.4's own audblk() syntax: full per-channel
        // dithflag[ch] syntax when dithflage is set, else every channel
        // defaults to "dithflag[ch] = 1 /* dither on */" for the block -
        // NOT off. Reconstruction happens in read_stream/the decoupling loop
        // below, the same split AC-3's own dithflag[ch] uses.
        std::array<bool, eac3::chanmap::kMaxSubstreamFullbw> dithflag{};
        if (frm->dithflage) {
            for (int ch = 0; ch < nfchans; ++ch) {
                dithflag[static_cast<std::size_t>(ch)] = r.read(1) != 0;
            }
        } else {
            for (int ch = 0; ch < nfchans; ++ch) {
                dithflag[static_cast<std::size_t>(ch)] = true;
            }
        }
        if (syntax != nullptr) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (dithflag[static_cast<std::size_t>(ch)]) {
                    syntax->dither |= static_cast<std::uint8_t>(1U << ch);
                }
            }
        }
        if (r.read(1) != 0) {  // dynrnge
            dynrng_word = static_cast<std::uint8_t>(r.read(8));
        }
        out.dynrng[static_cast<std::size_t>(blk)] = dynrng_word;
        if (bsi->acmod == Acmod::kDualMono) {
            if (r.read(1) != 0) {  // dynrng2e
                dynrng2_word = static_cast<std::uint8_t>(r.read(8));
            }
            out.dynrng2[static_cast<std::size_t>(blk)] = dynrng2_word;
        }

        // --- spectral extension strategy + geometry (§E2.3.3, §3.6) ---
        // Block 0's strategy is implied rather than sent; a later block only
        // resends it (spxstre) if the strategy actually changes - this
        // encoder never does, so everything below persists from block 0.
        const bool spxstre = blk == 0 || r.read(1) != 0;
        if (spxstre) {
            spxinu = r.read(1) != 0;
            if (spxinu) {
                // 1/0 is the one mode where chinspx is not transmitted: the
                // only channel there is always the one extended.
                if (bsi->acmod != Acmod::k1_0) {
                    for (int ch = 0; ch < nfchans; ++ch) {
                        chinspx[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                    }
                } else {
                    chinspx[0] = true;
                }
                spxstrtf = static_cast<int>(r.read(2));
                spxbegf = static_cast<int>(r.read(3));
                const int spxendf = static_cast<int>(r.read(3));
                const int begin_subbnd = eac3::spx_begin_subbnd(spxbegf);
                const int end_subbnd = eac3::spx_end_subbnd(spxendf);
                if (end_subbnd <= begin_subbnd || end_subbnd > eac3::kSpxSubBands) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                spx_startmant = eac3::spx_band_start(begin_subbnd);
                spx_endmant = eac3::spx_band_start(end_subbnd);
                spx_copystart = eac3::spx_band_start(spxstrtf);
                const int subband_count = end_subbnd - begin_subbnd;
                // spxbndstrc is relative to the region's first sub-band (as
                // group_bands wants it); the default table
                // (kDefaultSpxBandStructure) is ABSOLUTE-indexed, so it is
                // sliced at begin_subbnd rather than used as-is.
                if (r.read(1) != 0) {  // spxbndstrce
                    spx_structure.fill(false);
                    for (int i = 1; i < subband_count; ++i) {
                        spx_structure[static_cast<std::size_t>(i)] = r.read(1) != 0;
                    }
                    spx_structure_set = true;
                } else if (!spx_structure_set) {
                    // Unlike coupling's default table, spx's Table E2.11 is
                    // unambiguous (absolute-sub-band-indexed, verified
                    // against the spec text directly), so it is implemented
                    // for real rather than refused.
                    spx_structure.fill(false);
                    for (int i = 0; i < subband_count; ++i) {
                        spx_structure[static_cast<std::size_t>(i)] =
                            eac3::kDefaultSpxBandStructure[static_cast<std::size_t>(begin_subbnd +
                                                                                    i)];
                    }
                    spx_structure_set = true;
                }
                // else: a later block with the bit clear reuses what
                // spx_structure already holds, untouched.
                spx_bands = eac3::group_bands(
                    spx_startmant, subband_count, eac3::kSpxBinsPerSubBand,
                    std::span{spx_structure}.first(static_cast<std::size_t>(subband_count)));
                for (auto& channel : spxco) {
                    channel.assign(static_cast<std::size_t>(spx_bands.count),
                                   internal::decode_scalar_t{0});
                }
                if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                    for (auto& channel : spxco_exp) {
                        channel.assign(static_cast<std::size_t>(spx_bands.count), 0);
                    }
                }
            }
        }

        // --- spectral extension coordinates (§E3.3, block-0 spxcoe implied) ---
        if (spxinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto uch = static_cast<std::size_t>(ch);
                if (!chinspx[uch]) {
                    // §E2.3.3: a channel outside spectral extension this
                    // block has its first-coordinates state armed again.
                    firstspxcos[uch] = true;
                    continue;
                }
                bool send = true;
                if (firstspxcos[uch]) {
                    firstspxcos[uch] = false;  // spxcoe[ch] implied 1, not transmitted
                } else {
                    send = r.read(1) != 0;  // spxcoe[ch]
                }
                if (!send) {
                    continue;
                }
                spxblnd[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(5));
                const int master = static_cast<int>(r.read(2));
                auto& co = spxco[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < spx_bands.count; ++bnd) {
                    const auto exp = static_cast<std::uint8_t>(r.read(4));
                    const auto mant = static_cast<std::uint8_t>(r.read(2));
                    const coupling::Coordinate coordinate{.exp = exp, .mant = mant};
                    if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                        // Mantissa and power of two apart (block_norm.hpp), as
                        // the coupling coordinates are: the synthesis then
                        // applies the power as a shift, and a coordinate far
                        // below unity keeps every bit it was sent with.
                        co[static_cast<std::size_t>(bnd)] =
                            coupling::coordinate_mantissa_as<internal::decode_scalar_t>(
                                coordinate, coupling::kSpxMantissaBits);
                        spxco_exp[uch][static_cast<std::size_t>(bnd)] =
                            coupling::coordinate_exponent(coordinate, master);
                    } else {
                        co[static_cast<std::size_t>(bnd)] =
                            coupling::decode_coordinate_as<internal::decode_scalar_t>(
                                coordinate, master, coupling::kSpxMantissaBits);
                    }
                }
            }
        }

        // --- coupling strategy + geometry (§E2.3.3, Table E1.4) ---
        // cplinu itself was already decided for this block in audfrm
        // (frm->cplinu[blk]); only the GEOMETRY - which channels couple, the
        // coupled region, and how its sub-bands group into bands - is
        // per-block, and only present when this block resends the strategy
        // (frm->cplstre[blk]: true for block 0's implied strategy, and for
        // any later block that changes it - this encoder never does, so
        // everything below persists unchanged from block 0 onward).
        if (frm->cplstre[static_cast<std::size_t>(blk)] &&
            !frm->cplinu[static_cast<std::size_t>(blk)]) {
            // Table E1.4's other half of the same `if`: a block that states a
            // strategy of "no coupling" resets the coupling state outright,
            // so a later block that turns coupling back on starts from
            // implied coordinates and leak seeds again rather than from
            // whatever the last coupled block left behind.
            std::fill(chincpl.begin(), chincpl.end(), false);
            std::fill(firstcplcos.begin(), firstcplcos.end(), true);
            firstcplleak = true;
            phsflginu = false;
            ecplinu_now = false;
        }
        if (frm->cplstre[static_cast<std::size_t>(blk)] &&
            frm->cplinu[static_cast<std::size_t>(blk)]) {
            ecplinu_now = r.read(1) != 0;  // ecplinu: enhanced coupling
            // 2/0 is the one mode where chincpl is not transmitted: both
            // channels are coupled by definition. Every other mode sends
            // chincpl per channel. This part is common to both standard and
            // enhanced coupling; only what follows it differs.
            if (bsi->acmod == Acmod::k2_0) {
                chincpl[0] = chincpl[1] = true;
            } else {
                for (int ch = 0; ch < nfchans; ++ch) {
                    chincpl[static_cast<std::size_t>(ch)] = r.read(1) != 0;
                }
            }
            if (!ecplinu_now) {
                // --- standard coupling geometry (§7.4.2/§5.4.3.12-13) ---
                // phsflginu exists only for standard coupling - enhanced
                // coupling carries its own per-channel angle instead and has
                // no separate phase-restoration flag.
                phsflginu = bsi->acmod == Acmod::k2_0 && r.read(1) != 0;
                cplbegf = static_cast<int>(r.read(4));
                // §E3.3.1: with spectral extension active this block, cplendf is
                // derived from spxbegf instead of transmitted, so the coupling
                // region ends exactly where synthesis begins.
                const int cplendf =
                    spxinu ? eac3::derived_cplendf(spxbegf) : static_cast<int>(r.read(4));
                const int subband_count = coupling::sub_band_count(cplbegf, cplendf);
                if (subband_count < 1) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                cplstrtmant = coupling::start_mant(cplbegf);
                cplendmant = coupling::end_mant(cplendf);
                if (cplendmant > 253 || cplstrtmant >= cplendmant) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                // cplbndstrc: a 1 folds this sub-band into the previous coupling
                // band, so coordinates are per band and duplicated back out
                // across the sub-bands they cover. When cplbndstrce is 0 this
                // block doesn't transmit cplbndstrc: Table E2.12's default
                // applies in the frame's first coupled block, and the previous
                // block's structure in any later one (§E2.3.3.15 - see
                // cpl_structure_set's own comment). The default table is
                // indexed absolutely from cplbegf == 0, not relative to this
                // block's actual cplbegf, so the slice consulted starts at
                // kDefaultCplBandStructure[cplbegf].
                const auto subbands = static_cast<std::size_t>(subband_count);
                if (r.read(1) != 0) {  // cplbndstrce
                    cpl_structure.assign(subbands, false);
                    for (int bnd = 1; bnd < subband_count; ++bnd) {
                        cpl_structure[static_cast<std::size_t>(bnd)] = r.read(1) != 0;
                    }
                    cpl_structure_set = true;
                } else if (!cpl_structure_set) {
                    cpl_structure.assign(subbands, false);
                    for (int bnd = 1; bnd < subband_count; ++bnd) {
                        cpl_structure[static_cast<std::size_t>(bnd)] =
                            eac3::kDefaultCplBandStructure[static_cast<std::size_t>(cplbegf + bnd)];
                    }
                    cpl_structure_set = true;
                } else {
                    // Reuse. A later block may also move the coupled region,
                    // which the spec's one-line reuse rule does not cover: the
                    // shared prefix is reused exactly and any sub-band beyond
                    // it starts its own band, the value an untransmitted flag
                    // carries everywhere else.
                    cpl_structure.resize(subbands, false);
                }
                subband_band.assign(subbands, 0);
                ncplbnd = 1;
                for (int bnd = 1; bnd < subband_count; ++bnd) {
                    if (!cpl_structure[static_cast<std::size_t>(bnd)]) {
                        ++ncplbnd;
                    }
                    subband_band[static_cast<std::size_t>(bnd)] = ncplbnd - 1;
                }
                // Coordinates survive a re-sent strategy: cplcoe == 0 in this
                // very block legally means "reuse the previous coordinates", so
                // clearing them here would silence the coupled high band. Only a
                // change in geometry forces a resize, and only the new entries
                // start at zero.
                for (auto& channel : cplco) {
                    channel.assign(static_cast<std::size_t>(subband_count), internal::decode_scalar_t{0});
                }
                if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                    for (auto& channel : cplco_exp) {
                        channel.assign(static_cast<std::size_t>(subband_count), 0);
                    }
                }
                phsflg.assign(static_cast<std::size_t>(ncplbnd), false);
            } else {
                // --- enhanced coupling geometry (§E2.3.3.16-19, §E3.5.2) ---
                phsflginu = false;
                ecplbegf = static_cast<int>(r.read(4));
                ecpl_begin_subbnd = eac3::ecpl_begin_subbnd(ecplbegf);
                if (spxinu) {
                    ecpl_end_subbnd = eac3::ecpl_end_subbnd_from_spx(spxbegf);
                } else {
                    const auto ecplendf = static_cast<int>(r.read(4));
                    ecpl_end_subbnd = eac3::ecpl_end_subbnd(ecplendf);
                }
                if (ecpl_end_subbnd <= ecpl_begin_subbnd || ecpl_end_subbnd > eac3::kEcplSubBands) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                cplstrtmant = eac3::kEcplSubBandTab[static_cast<std::size_t>(ecpl_begin_subbnd)];
                cplendmant = eac3::kEcplSubBandTab[static_cast<std::size_t>(ecpl_end_subbnd)];
                if (r.read(1) != 0) {  // ecplbndstrce
                    ecpl_structure.fill(false);
                    const int first = std::max(9, ecpl_begin_subbnd + 1);
                    for (int sbnd = first; sbnd < ecpl_end_subbnd; ++sbnd) {
                        ecpl_structure[static_cast<std::size_t>(sbnd)] = r.read(1) != 0;
                    }
                    ecpl_structure_set = true;
                } else if (!ecpl_structure_set) {
                    // Table E2.13's default table has an unambiguous absolute
                    // sub-band index (verified against the spec text
                    // directly, unlike standard coupling's Table E2.12) - so
                    // it is used for real here rather than refused. Only in
                    // the frame's first enhanced-coupling block, though; a
                    // later one reuses the previous block's structure
                    // (§E2.3.3.18), which is what ecpl_structure already
                    // holds.
                    ecpl_structure = eac3::kDefaultEcplBandStructure;
                    ecpl_structure_set = true;
                }
                // Coordinates survive a re-sent strategy, same reasoning as
                // standard coupling above - only a geometry change forces a
                // resize.
                const auto band_count = static_cast<std::size_t>(
                    eac3::ecpl_group_bands(ecpl_begin_subbnd, ecpl_end_subbnd, ecpl_structure)
                        .count);
                for (auto& channel : ecplamp_raw) {
                    channel.assign(band_count, 0);
                }
                for (auto& channel : ecplangle_raw) {
                    channel.assign(band_count, 0);
                }
                for (auto& channel : ecplchaos_raw) {
                    channel.assign(band_count, 0);
                }
            }
        }

        // --- coupling coordinates (§7.4.3 shape, block-0 cplcoe implied) ---
        if (frm->cplinu[static_cast<std::size_t>(blk)] && !ecplinu_now) {
            bool any_new = false;
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto uch = static_cast<std::size_t>(ch);
                if (!chincpl[uch]) {
                    // §E2.3.3: an uncoupled channel re-arms its own
                    // first-coordinates state, same rule as spx above.
                    firstcplcos[uch] = true;
                    continue;
                }
                bool send = true;
                if (firstcplcos[uch]) {
                    firstcplcos[uch] = false;  // cplcoe[ch] implied 1, not transmitted
                } else {
                    send = r.read(1) != 0;  // cplcoe[ch]
                }
                if (!send) {
                    continue;
                }
                any_new = true;
                const int master = static_cast<int>(r.read(2));
                // One value per band, at most kMaxSubBands of them (standard
                // coupling has 18 sub-bands, so never more bands than that):
                // a fixed array rather than the std::vector this was, which
                // allocated once per coupled channel per block.
                std::array<internal::decode_scalar_t, eac3::kMaxSubBands> band_values{};
                std::array<int, eac3::kMaxSubBands> band_exps{};
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    const auto exp = static_cast<std::uint8_t>(r.read(4));
                    const auto mant = static_cast<std::uint8_t>(r.read(4));
                    const coupling::Coordinate coordinate{.exp = exp, .mant = mant};
                    if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                        // The fixed-point store keeps the mantissa and the
                        // power of two apart (block_norm.hpp) - see
                        // decoder.cpp's own copy of this split.
                        band_values[static_cast<std::size_t>(bnd)] =
                            coupling::coordinate_mantissa_as<internal::decode_scalar_t>(
                                coordinate);
                        band_exps[static_cast<std::size_t>(bnd)] =
                            coupling::coordinate_exponent(coordinate, master);
                    } else {
                        band_values[static_cast<std::size_t>(bnd)] =
                            coupling::decode_coordinate_as<internal::decode_scalar_t>(
                                coordinate, master);
                    }
                }
                auto& channel = cplco[static_cast<std::size_t>(ch)];
                for (std::size_t bnd = 0; bnd < channel.size(); ++bnd) {
                    const auto band = static_cast<std::size_t>(subband_band[bnd]);
                    channel[bnd] = band_values[band];
                    if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                        cplco_exp[static_cast<std::size_t>(ch)][bnd] = band_exps[band];
                    }
                }
            }
            if (phsflginu && any_new) {
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    phsflg[static_cast<std::size_t>(bnd)] = r.read(1) != 0;
                }
            }
        } else if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            // --- enhanced coupling coordinates (§E2.3.3.20-26, §3.5.4) ---
            ecplangleintrp = r.read(1) != 0;
            int firstchincpl = -1;
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto uch = static_cast<std::size_t>(ch);
                if (!chincpl[uch]) {
                    firstcplcos[uch] = true;
                    continue;
                }
                if (firstchincpl == -1) {
                    firstchincpl = ch;
                }
                // Table E1.4 gates these on the same per-channel
                // firstcplcos[ch] state standard coupling uses, not on the
                // block index.
                bool ecplparam1e = true;
                bool ecplparam2e = ch > firstchincpl;
                if (firstcplcos[uch]) {
                    firstcplcos[uch] = false;
                } else {
                    ecplparam1e = r.read(1) != 0;
                    ecplparam2e = ch > firstchincpl && r.read(1) != 0;
                }
                if (ecplparam1e) {
                    for (auto& v : ecplamp_raw[uch]) {
                        v = static_cast<int>(r.read(5));
                    }
                }
                if (ecplparam2e) {
                    for (std::size_t bnd = 0; bnd < ecplangle_raw[uch].size(); ++bnd) {
                        ecplangle_raw[uch][bnd] = static_cast<int>(r.read(6));
                        ecplchaos_raw[uch][bnd] = static_cast<int>(r.read(3));
                    }
                }
                // ecpltrans[ch] is read every block, unconditionally, for
                // every channel past the first - never gated by the exist
                // flags above and never persisted from a previous block.
                ecpltrans_persist[uch] = ch > firstchincpl && r.read(1) != 0;
            }
        }

        // A coupled or extended channel stops carrying its own coefficients
        // at whichever tool takes over first; coupling always wins the
        // channels it shares with spx, since the two are contiguous and
        // coupling sits below (§E3.3.1's whole point). Runs every block,
        // using the persistent geometry above, so a channel whose tool
        // membership never changes keeps the same cutoff without needing to
        // be re-derived only on the blocks that resend a strategy.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (frm->cplinu[static_cast<std::size_t>(blk)] &&
                chincpl[static_cast<std::size_t>(ch)]) {
                endmant[static_cast<std::size_t>(ch)] = cplstrtmant;
            } else if (spxinu && chinspx[static_cast<std::size_t>(ch)]) {
                endmant[static_cast<std::size_t>(ch)] = spx_startmant;
            }
        }

        if (bsi->acmod == Acmod::k2_0) {
            // Unlike AC-3, block 0's rematstr is IMPLIED 1 rather than
            // transmitted; only later blocks carry the bit.
            if (blk == 0 || r.read(1) != 0) {
                // §3.3.2 / §7.5.2: the rematrixing bands cannot reach above
                // whichever tool takes over the spectrum first, so their
                // count depends on where that tool starts. Coupling always
                // wins the comparison when both are active, since it is
                // always the lower of the two (§E3.3.1). Enhanced coupling
                // gets its own table (§3.3.2), keyed off ecplbegf rather than
                // cplbegf - a distinct formula, not a parameter substitution,
                // since its sub-band table starts at a different frequency.
                const int nrematbd = frm->cplinu[static_cast<std::size_t>(blk)]
                                         ? (ecplinu_now ? (ecplbegf == 0   ? 0
                                                           : ecplbegf == 1 ? 1
                                                           : ecplbegf == 2 ? 2
                                                           : ecplbegf < 5  ? 3
                                                                           : 4)
                                                        : (cplbegf > 2 ? 4 : (cplbegf > 0 ? 3 : 2)))
                                     : spxinu ? (spxbegf < 2 ? 3 : 4)
                                              : 4;
                rematflg.fill(false);
                for (int band = 0; band < nrematbd; ++band) {
                    rematflg[static_cast<std::size_t>(band)] = r.read(1) != 0;
                }
            }
        }

        // chbwcod accompanies a fresh strategy, but only for a channel
        // carrying its own high band: a coupled or extended channel's
        // bandwidth is fixed by whichever tool takes over, and sending
        // chbwcod anyway would both waste the bits and desynchronise the
        // block.
        for (int ch = 0; ch < nfchans; ++ch) {
            if (strategy(ch) == ExpStrategy::kReuse) {
                continue;
            }
            if (frm->cplinu[static_cast<std::size_t>(blk)] &&
                chincpl[static_cast<std::size_t>(ch)]) {
                continue;
            }
            if (spxinu && chinspx[static_cast<std::size_t>(ch)]) {
                continue;
            }
            const auto chbwcod = r.read(6);
            if (chbwcod > 60) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            endmant[static_cast<std::size_t>(ch)] = ((static_cast<int>(chbwcod) + 12) * 3) + 37;
        }

        // Coupling channel exponents, ahead of the fbw/LFE channels (§5.3.3
        // order: coupling channel first). Offset to its own start bin and
        // using the even-valued absolute reference, same as AC-3.
        if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            const auto strat = frm->cplexpstr[static_cast<std::size_t>(blk)];
            if (strat == ExpStrategy::kReuse) {
                if (blk == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
            } else {
                const int span = cplendmant - cplstrtmant;
                const int group_size = exponent_group_size(strat);
                if (group_size == 0 || span % (3 * group_size) != 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                const int ngrps = span / (3 * group_size);
                const auto cplabsexp = static_cast<std::uint8_t>(r.read(4));
                auto& groups = impl_->exp_groups_;
                groups.assign(static_cast<std::size_t>(ngrps), 0);
                for (auto& g : groups) {
                    g = static_cast<std::uint8_t>(r.read(7));
                    if (g > 124) {  // §7.10.2 error condition 17
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                auto& target = exps[static_cast<std::size_t>(kCplStream)];
                target.assign(static_cast<std::size_t>(cplendmant), kMaxExponent);
                decode_coupling_exponents(
                    cplabsexp, groups, strat,
                    std::span{target}.subspan(static_cast<std::size_t>(cplstrtmant)));
                // §7.2.2.2: exponents are 0..24, and the reconstruction
                // shifts by them - out of range is undefined behaviour.
                for (std::size_t bin = static_cast<std::size_t>(cplstrtmant); bin < target.size();
                     ++bin) {
                    if (target[bin] > kMaxExponent) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                endmant[static_cast<std::size_t>(kCplStream)] = cplendmant;
            }
        }

        {
            AC3_ZONE_SCOPED_N("eac3_exponents");
            for (int ch = 0; ch < nchans; ++ch) {
                const auto strat = strategy(ch);
                if (strat == ExpStrategy::kReuse) {
                    if (blk == 0) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                    continue;
                }
                const int end = ch < nfchans ? endmant[static_cast<std::size_t>(ch)] : kLfeEndmant;
                endmant[static_cast<std::size_t>(ch)] = end;
                const int ngrps = ch < nfchans ? exponent_group_count(strat, end) : 2;
                const auto absolute = static_cast<std::uint8_t>(r.read(4));
                auto& groups = impl_->exp_groups_;
                groups.assign(static_cast<std::size_t>(ngrps), 0);
                for (auto& g : groups) {
                    g = static_cast<std::uint8_t>(r.read(7));
                    if (g > 124) {  // §7.10.2 error condition 17
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                }
                auto& target = exps[static_cast<std::size_t>(ch)];
                target.assign(static_cast<std::size_t>(end), 0);
                decode_exponents(absolute, groups, strat, target);
                // §7.2.2.2: exponents are 0..24, and the reconstruction shifts by
                // them - out of range is undefined behaviour, not wrong audio.
                if (std::ranges::any_of(target, [](auto e) { return e > kMaxExponent; })) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                if (ch < nfchans) {
                    r.skip(2);  // gainrng
                }
            }
        }

        if (frm->bamode && r.read(1) != 0) {  // baie
            codes.sdcycod = static_cast<int>(r.read(2));
            codes.fdcycod = static_cast<int>(r.read(2));
            codes.sgaincod = static_cast<int>(r.read(2));
            codes.dbpbcod = static_cast<int>(r.read(2));
            codes.floorcod = static_cast<int>(r.read(3));
        }
        if (frm->snroffststr == 0x0) {
            // Strategy 1: the frame's pair applies to every channel of every
            // block, the LFE included.
            csnroffst = frm->frmcsnroffst;
            fsnroffst.fill(frm->frmfsnroffst);
        } else if (blk == 0 || r.read(1) != 0) {  // snroffste
            csnroffst = static_cast<int>(r.read(6));
            if (frm->snroffststr == 0x1) {
                // Strategy 2: one blkfsnroffst for the whole block, which
                // Table E1.4 assigns to the coupling channel and the LFE as
                // well as the fbw ones - hence fill() over the whole array,
                // kCplStream included.
                fsnroffst.fill(static_cast<int>(r.read(4)));
            } else {
                // Strategy 0x2: a fine offset per stream, in the order AC-3's
                // own snroffste group uses - cplfsnroffst first and only
                // where this block couples, then one per full-bandwidth
                // channel, then lfefsnroffst. The coupling channel's slot is
                // kCplStream, not part of the 0..nchans run; reading nchans
                // values into that run instead, as this did, both consumes
                // the wrong number of bits whenever coupling is on and leaves
                // the shared channel allocating against an offset nobody
                // sent.
                //
                // This is what tools/references/eac3_parse.py - the
                // independent transcription this project checks itself
                // against - has always read here, so the two now agree.
                //
                // Still unverified against a real stream, deliberately
                // flagged as such: nothing in reach emits snroffststr != 0 at
                // all. Neither FFmpeg 8.0.1's encoder nor Dolby's DEE 6.5.4
                // ever does (checked over tests/golden/external-baseline/,
                // every frame of both E-AC-3 legs), and when this project's
                // own encoder was made to emit strategies 0x1 and 0x2 to the
                // reading above, FFmpeg's decoder refused both - with and
                // without an explicit block-0 snroffste - so the block-level
                // element's shape is an open question no oracle can settle.
                // What is certain either way is that reading nchans values
                // and no coupling one, as this did, cannot be right.
                //
                // The fgaincode element below has the same shape and the
                // same Table E1.4 rule, and unlike this one it IS reached
                // by a real stream - see its own note.
                if (frm->cplinu[static_cast<std::size_t>(blk)]) {
                    fsnroffst[static_cast<std::size_t>(kCplStream)] = static_cast<int>(r.read(4));
                }
                for (int ch = 0; ch < nchans; ++ch) {
                    fsnroffst[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(4));
                }
            }
        }
        // fgaincode is only ever sent when the frame said it might be; absent,
        // every channel's fast gain reverts to 0x4 for this block.
        if (frm->frmfgaincode && r.read(1) != 0) {  // fgaincode
            // Table E1.4 again: cplfgaincod is transmitted ahead of the
            // per-channel codes whenever this block couples. Omitting it read
            // every fast gain code three bits early and desynchronised the
            // rest of the block - invisible against this project's own
            // encoder and FFmpeg's, which both leave frmfgaincode at 0 so the
            // whole element is absent, and reached for the first time by a
            // Dolby Encoding Engine stream (frmfgaincode == 1,
            // tests/oba/test_dee_joc_fixture.cpp).
            if (frm->cplinu[static_cast<std::size_t>(blk)]) {
                fgaincod[static_cast<std::size_t>(kCplStream)] = static_cast<int>(r.read(3));
            }
            for (int ch = 0; ch < nchans; ++ch) {
                fgaincod[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(3));
            }
        } else {
            // The else branch of the same table: 0x4 for every channel, the
            // coupling channel included - which fill() over the whole array
            // already covers.
            fgaincod.fill(kBamode0Codes.fgaincod);
        }
        if (bsi->strmtyp != StreamType::kDependent && r.read(1) != 0) {  // convsnroffste
            r.skip(10);  // convsnroffst: for a converter's allocation, not ours
        }
        // Coupling leak seeds. firstcplleak starts at 1: the seeds are
        // mandatory in the frame's first coupled block (no cplleake bit ahead
        // of them), unlike AC-3 where the gating bit is always present; every
        // later block sends an explicit cplleake bit and may choose to keep
        // the earlier seeds instead. "The frame's first coupled block" is not
        // always block 0 - see firstcplleak's own declaration.
        if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            bool cplleake = true;
            if (firstcplleak) {
                firstcplleak = false;
            } else {
                cplleake = r.read(1) != 0;
            }
            if (cplleake) {
                cplfleak = static_cast<int>(r.read(3));
                cplsleak = static_cast<int>(r.read(3));
            }
        }
        // Read into a local rather than tested inline so the self-check can
        // see it; the short-circuit is unchanged, so a frame with dbaflde
        // clear still consumes no bit here.
        const bool deltbaie = frm->dbaflde && r.read(1) != 0;
        if (block_trace != nullptr) {
            block_trace->deltbaie = deltbaie;
        }
        if (deltbaie) {
            // §E2.3.2.9/§5.4.3.49-57: the syntax table reads every stream's
            // 2-bit cpldeltbae/deltbae[ch] code FIRST, then every stream's
            // segment data - not interleaved per stream - so all codes are
            // read and validated up front. Bounds are checked here, before
            // compute_bit_allocation ever sees them, since deltoffst/deltlen
            // are attacker-controlled and mask[] is exactly 50 bands wide.
            //
            // The coupling channel is in §7.2.2.6's scope like any fbw
            // channel and carries its own cpldeltbae, exactly as AC-3's own
            // decoder reads it (decoder.cpp). Its band cursor starts at
            // bin_to_band(cplstrtmant) rather than 0, matching
            // compute_bit_allocation()'s own origin for a coupling region.
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
            // Table 5.16: 00 reuse, 01 new info follows, 10 no delta, 11
            // reserved. cplcode stays at "reuse" when coupling is not in use
            // this block, so delta[kCplStream] is left alone in that case.
            const bool cplinu_blk = frm->cplinu[static_cast<std::size_t>(blk)];
            int cplcode = 0;
            if (cplinu_blk) {
                cplcode = static_cast<int>(r.read(2));
                if (cplcode == 3) {  // Table 5.16: reserved
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (blk == 0 && cplcode == 0) {
                    return std::unexpected(
                        DecodeError::kInvalidStream);  // shall not reuse in block 0
                }
            }
            std::array<int, eac3::chanmap::kMaxSubstreamFullbw> chcodes{};
            for (int ch = 0; ch < nfchans; ++ch) {
                chcodes[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
                if (chcodes[static_cast<std::size_t>(ch)] == 3) {  // Table 5.16: reserved
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (blk == 0 && chcodes[static_cast<std::size_t>(ch)] == 0) {
                    return std::unexpected(
                        DecodeError::kInvalidStream);  // shall not reuse in block 0
                }
            }
            if (cplinu_blk && cplcode == 1) {  // new info follows
                auto segs = parse_segments(bin_to_band(cplstrtmant));
                if (!segs.has_value()) {
                    return std::unexpected(segs.error());
                }
                delta[static_cast<std::size_t>(kCplStream)] = *segs;
            } else if (cplinu_blk && cplcode == 2) {  // perform no delta alloc
                delta[static_cast<std::size_t>(kCplStream)] = {};
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const int chcode = chcodes[static_cast<std::size_t>(ch)];
                if (chcode == 1) {  // new info follows
                    auto segs = parse_segments(0);
                    if (!segs.has_value()) {
                        return std::unexpected(segs.error());
                    }
                    delta[static_cast<std::size_t>(ch)] = *segs;
                } else if (chcode == 2) {  // perform no delta alloc
                    delta[static_cast<std::size_t>(ch)] = {};
                }
                // chcode == 0 (reuse): leave delta[ch] exactly as it was.
            }
        } else if (blk == 0) {
            // §5.4.3.47: deltbaie == 0 in block 0 forces "no delta alloc" for
            // the coupling channel (if any) and every fbw channel. Reached
            // both when dbaflde is clear (delta[] is already {} from the
            // frame-start reset, so this is a no-op) and when dbaflde is set
            // but this frame's first block's deltbaie reads 0 (where it is
            // the rule that actually matters).
            delta[static_cast<std::size_t>(kCplStream)] = {};
            for (int ch = 0; ch < nfchans; ++ch) {
                delta[static_cast<std::size_t>(ch)] = {};
            }
        }
        // What is in FORCE, not what this block transmitted - a clear
        // deltbaie retains the previous block's segments (§5.4.3.47).
        if (syntax != nullptr) {
            for (int ch = 0; ch < nfchans && !syntax->delta_bit_alloc; ++ch) {
                syntax->delta_bit_alloc = delta[static_cast<std::size_t>(ch)].deltnseg > 0;
            }
        }
        if (frm->skipflde && r.read(1) != 0) {  // skiple
            const auto skipl = r.read(9);
            if (syntax != nullptr) {
                syntax->skip_field = true;
                syntax->skip_bytes = static_cast<std::uint16_t>(skipl);
            }
            // Materialized rather than left as a view into `frame`: skipfld
            // starts wherever the bits before it happened to end, not
            // necessarily on a byte boundary, so its bytes have to be read
            // out 8 bits at a time (matching exactly how eac3_frame.cpp's
            // put_skip_field wrote them) before they mean anything as a
            // self-contained EMDF container.
            auto& skip_bytes = impl_->skip_bytes_;
            skip_bytes.clear();
            skip_bytes.reserve(skipl);
            for (std::uint32_t i = 0; i < skipl; ++i) {
                skip_bytes.push_back(static_cast<std::byte>(r.read(8)));
            }
            // Which block carries the container is not fixed
            // (emdf::build_container's own comment), so every block's skip
            // field is a candidate; stop looking once one has produced OAMD.
            // A container that is present but fails to parse leaves
            // object_metadata unset, same as no container at all - it never
            // fails the surrounding frame decode, matching EMDF's whole
            // reason for existing: a decoder that does not understand this
            // data reads the rest of the frame exactly as it would without it.
            if (!out.object_metadata.has_value()) {
                const auto container = emdf::parse_container(skip_bytes);
                if (container.has_value() && container->has_value()) {
                    for (const auto& payload : **container) {
                        if (impl_->config_.syntax != nullptr) {
                            impl_->config_.syntax->add_emdf_payload(payload.id);
                        }
                        if (payload.id == emdf::kPayloadIdOamd) {
                            out.object_metadata = oba::parse_payload(payload.bytes);
                        } else if (payload.id == emdf::kPayloadIdJoc && joc_bytes.empty()) {
                            joc_bytes.assign(payload.bytes.begin(), payload.bytes.end());
                        } else if (payload.id != emdf::kPayloadIdOamd &&
                                   payload.id != emdf::kPayloadIdJoc &&
                                   impl_->config_.diagnostics != nullptr) {
                            // Any id this decoder does not interpret at all -
                            // a second JOC payload (joc_bytes already taken)
                            // is a recognised id this decoder simply has no
                            // use for twice, not an unknown one, so it does
                            // not reach here.
                            impl_->config_.diagnostics(
                                {.event = DiagnosticEvent::kUnknownEmdfPayload,
                                 .emdf_payload_id = static_cast<std::uint8_t>(payload.id)},
                                impl_->config_.diagnostics_context);
                        }
                    }
                }
            }
        }

        // §7.2.2.1.1 is frame-wide: csnroffst together with EVERY channel's
        // fine offset. Deciding it per channel would zero one allocation while
        // the others allocate normally, desynchronising the shared mantissa
        // stream.
        bool snr_all_zero = csnroffst == 0;
        for (int ch = 0; ch < nchans && snr_all_zero; ++ch) {
            snr_all_zero = fsnroffst[static_cast<std::size_t>(ch)] == 0;
        }
        // cplfsnroffst counts too, and it is a separate slot rather than part
        // of the run above. It could only ever be non-zero once strategy 0x2
        // was actually read (nothing else fills that slot), which is why this
        // arrives with it.
        if (snr_all_zero && frm->cplinu[static_cast<std::size_t>(blk)]) {
            snr_all_zero = fsnroffst[static_cast<std::size_t>(kCplStream)] == 0;
        }
        if (frm->cplinu[static_cast<std::size_t>(blk)]) {
            const auto s = static_cast<std::size_t>(kCplStream);
            const int end = endmant[s];
            if (static_cast<int>(exps[s].size()) != end) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            BitAllocCodes cpl_codes = codes;
            cpl_codes.fgaincod = fgaincod[s];
            const BitAllocRegion cpl_region{.start = cplstrtmant,
                                            .coupling = true,
                                            .cplfleak = cplfleak,
                                            .cplsleak = cplsleak,
                                            .snr_all_zero = snr_all_zero,
                                            .high_efficiency = frm->ahtinu[s],
                                            .delta = delta[static_cast<std::size_t>(kCplStream)]};
            auto& cpl_memo = impl_->bitalloc_memo_[s];
            if (block_trace != nullptr) {
                bap[s].assign(static_cast<std::size_t>(end), 0);
                internal::compute_bit_allocation_traced(exps[s], bsi->sample_rate, cpl_codes,
                                                        csnroffst, fsnroffst[s], bap[s], cpl_region,
                                                        mask[s]);
                cpl_memo.valid = false;
            } else if (!cpl_memo.matches(exps[s], bsi->sample_rate, cpl_codes, csnroffst,
                                         fsnroffst[s], cpl_region)) {
                bap[s].assign(static_cast<std::size_t>(end), 0);
                compute_bit_allocation(exps[s], bsi->sample_rate, cpl_codes, csnroffst,
                                       fsnroffst[s], bap[s], cpl_region);
                cpl_memo.remember(exps[s], bsi->sample_rate, cpl_codes, csnroffst, fsnroffst[s],
                                  cpl_region);
            }
        }
        {
            AC3_ZONE_SCOPED_N("eac3_bit_allocation");
            for (int ch = 0; ch < nchans; ++ch) {
                const auto uch = static_cast<std::size_t>(ch);
                const int end = endmant[uch];
                if (static_cast<int>(exps[uch].size()) != end) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                BitAllocCodes channel_codes = codes;
                channel_codes.fgaincod = fgaincod[uch];
                // delta[ch] for ch == LFE's index is always {} (never written -
                // §5.4.3.49/E2.3.2.9 bound their deltbae[ch] loop by nfchans, so
                // the LFE channel has no delta bit allocation field at all).
                const BitAllocRegion channel_region{.snr_all_zero = snr_all_zero,
                                                    .high_efficiency = frm->ahtinu[uch],
                                                    .delta = delta[uch]};
                auto& memo = impl_->bitalloc_memo_[uch];
                if (block_trace != nullptr) {
                    bap[uch].assign(static_cast<std::size_t>(end), 0);
                    internal::compute_bit_allocation_traced(
                        exps[uch], bsi->sample_rate, channel_codes, csnroffst, fsnroffst[uch],
                        bap[uch], channel_region, mask[uch]);
                    memo.valid = false;
                } else if (!memo.matches(exps[uch], bsi->sample_rate, channel_codes, csnroffst,
                                         fsnroffst[uch], channel_region)) {
                    // Unchanged inputs keep the allocation `bap` already holds
                    // from the block that computed it (bitalloc_memo.hpp).
                    bap[uch].assign(static_cast<std::size_t>(end), 0);
                    compute_bit_allocation(exps[uch], bsi->sample_rate, channel_codes, csnroffst,
                                           fsnroffst[uch], bap[uch], channel_region);
                    memo.remember(exps[uch], bsi->sample_rate, channel_codes, csnroffst,
                                  fsnroffst[uch], channel_region);
                }
            }
        }

        // The self-check's per-block view (ac3/verify/eac3_mirror.hpp), taken
        // here: everything the two sides model about this block - its tool
        // geometry, its exponents, its allocation, its coordinates - is
        // final by now, and the mantissas below are the first thing whose
        // WIDTH depends on all of it.
        if (block_trace != nullptr) {
            const bool cplinu = frm->cplinu[static_cast<std::size_t>(blk)];
            block_trace->cplinu = cplinu;
            block_trace->ecplinu = cplinu && ecplinu_now;
            block_trace->cplstrtmant = cplinu ? cplstrtmant : 0;
            block_trace->cplendmant = cplinu ? cplendmant : 0;
            block_trace->spxinu = spxinu;
            block_trace->spx_startmant = spxinu ? spx_startmant : 0;
            block_trace->spx_endmant = spxinu ? spx_endmant : 0;
            block_trace->spx_copystart = spxinu ? spx_copystart : 0;

            const auto coded = static_cast<std::size_t>(nchans);
            block_trace->streams.resize(coded + (cplinu ? 1U : 0U));
            for (std::size_t slot = 0; slot < block_trace->streams.size(); ++slot) {
                // The trace numbers the coupling stream just past the coded
                // channels, the way the encoder does; this decoder parks it
                // at a fixed internal slot instead, so the two are mapped
                // onto each other here rather than left to compare across
                // different numbering.
                const auto s = slot < coded ? slot : static_cast<std::size_t>(kCplStream);
                auto& stream = block_trace->streams[slot];
                stream.exponents = exps[s];
                stream.bap = bap[s];
                // Every full-bandwidth channel AND the coupling stream carry
                // a delta slot - the coupling channel's own cpldeltbae
                // (§E2.3.2.9, added alongside delta bit allocation under
                // coupling) lives at delta[kCplStream], mirroring the
                // encoder's trace. Only the LFE has none: §E2.3.2.9 bounds
                // the deltbae[ch] loop by nfchans, so delta[nfchans] (the
                // LFE's would-be slot) is never written by the parse above
                // and reads back {} on its own. Indexing `delta` directly for
                // every slot - rather than special-casing the coupling
                // stream to always show {} - is the fix for a real self-check
                // false positive this trace produced: the parser above
                // already reads cpldeltbae correctly into delta[kCplStream],
                // this was only failing to carry it into the trace.
                stream.delta = delta[s];
                stream.snr_offset = snr_offset(csnroffst, fsnroffst[s]);
                stream.mask = mask[s];
                stream.start = s == static_cast<std::size_t>(kCplStream) ? cplstrtmant : 0;
                stream.endmant = endmant[s];
                stream.aht = frm->ahtinu[s];
                // gaqmod and gain are patched in after the mantissas below -
                // they are transmitted once a frame, in block 0's mantissa
                // element, so this is too early to know them.
                stream.gaqmod = 0;
                stream.gain.clear();
            }

            block_trace->channels.resize(static_cast<std::size_t>(nfchans));
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto uch = static_cast<std::size_t>(ch);
                auto& channel = block_trace->channels[uch];
                channel.blksw = blksw[uch];
                channel.in_coupling = cplinu && chincpl[uch];
                channel.cplco.clear();
                channel.ecplamp.clear();
                channel.ecplangle.clear();
                channel.ecplchaos.clear();
                channel.ecpltrans = false;
                if (channel.in_coupling && !ecplinu_now) {
                    // The trace keeps its coordinates in double whatever the
                    // decoder's own store is; widen_into converts each one.
                    widen_into(channel.cplco, cplco[uch]);
                } else if (channel.in_coupling) {
                    channel.ecplamp = ecplamp_raw[uch];
                    channel.ecplangle = ecplangle_raw[uch];
                    channel.ecplchaos = ecplchaos_raw[uch];
                    channel.ecpltrans = ecpltrans_persist[uch];
                }
                channel.in_spx = spxinu && chinspx[uch];
                channel.spxblnd = 0;
                channel.spxco.clear();
                if (channel.in_spx) {
                    channel.spxblnd = spxblnd[uch];
                    widen_into(channel.spxco, spxco[uch]);
                }
            }
            block_trace->allocated = true;
        }

        // Mantissas, in coded order: fbw channels (the first coupled one
        // pulling in the shared coupling channel right after it, same as
        // AC-3), then the LFE.
        MantissaBlockReader mantissa_reader;
        // Heap-backed, matching decoder.cpp's own per-block coeffs: at
        // kMaxSubstreamStreams * 256 doubles, a stack std::array here is the
        // single largest contributor to this function's frame size. The
        // assign() re-zeroes exactly as a fresh vector did (uncoded bins must
        // read zero).
        //
        // Parsed straight into this block's own tail, rather than into a
        // separate buffer that the snapshot below then swapped in. The swap
        // was there to hand the parse buffer some storage to reuse, and it
        // did - but it also meant one more kMaxSubstreamStreams x 256 array
        // existed than there were blocks to hold, which is what made moving
        // that buffer onto the decoder cost 7,168 bytes of peak instead of
        // saving an allocation. Nothing in pass one reads any tail, so
        // writing into this one directly is the same sequence of values with
        // one fewer buffer and nothing to swap.
        auto& coeffs = tails[static_cast<std::size_t>(blk)].coeffs;
        coeffs.assign(static_cast<std::size_t>(kMaxSubstreamStreams), {});
        auto& norm = tails[static_cast<std::size_t>(blk)].norm;
        norm.fill(0);
        tails[static_cast<std::size_t>(blk)].rematrix_room = false;
        // The fixed-point tier's block exponents (block_norm.hpp): each
        // stream's own coded bins set its exponent here, and a tool that
        // needs more room - decoupling, the enhanced coupling reconstruction,
        // spectral extension's synthesis, rematrixing - lowers it where it
        // runs. An AHT stream's exponent is exact and set where its frame is
        // dequantised (decode_aht_stream), so it is left alone here. The
        // floating tiers' stay zero.
        if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
            const auto ucpl = static_cast<std::size_t>(kCplStream);
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto uch = static_cast<std::size_t>(ch);
                if (!frm->ahtinu[uch]) {
                    norm[uch] =
                        internal::store_norm(internal::min_exponent(exps[uch], 0, endmant[uch]));
                }
            }
            if (frm->cplinu[static_cast<std::size_t>(blk)] && !frm->ahtinu[ucpl]) {
                norm[ucpl] = internal::store_norm(
                    internal::min_exponent(exps[ucpl], cplstrtmant, cplendmant));
            }
            if (bsi->lfe) {
                const auto ulfe = static_cast<std::size_t>(nfchans);
                if (!frm->ahtinu[ulfe]) {
                    norm[ulfe] = internal::store_norm(
                        internal::min_exponent(exps[ulfe], 0, endmant[ulfe]));
                }
            }
            // A rematrixed pair of ordinary streams can take its shared
            // exponent now, before either is dequantised, and lose nothing to
            // the shift the second pass would otherwise apply; an AHT pair
            // takes it there (its exponents are exact and known only then).
            if (bsi->acmod == Acmod::k2_0 && !frm->ahtinu[0] && !frm->ahtinu[1] &&
                std::ranges::any_of(rematflg, [](bool on) { return on; })) {
                const int shared = std::max(
                    std::min(norm[0], norm[1]) - internal::kRematrixGuardBits, internal::kNormFloor);
                norm[0] = shared;
                norm[1] = shared;
                tails[static_cast<std::size_t>(blk)].rematrix_room = true;
            }
        }
        // §7.3.4, same split as decoder.cpp's own read_stream: only a stream
        // with its OWN dithflag (a full-bandwidth channel, s < nfchans)
        // dithers here. The LFE has no dithflag and always reconstructs as
        // zero; kCplStream's shared bins stay silent here too and are
        // dithered per receiving channel in the decoupling loop below
        // instead, per §7.3.4's "applied after the individual channels are
        // extracted ... uncorrelated" requirement.
        // Dequantised and scaled in the coefficient store's own type, not in
        // double and narrowed: on the single-precision FPU the
        // minimum-footprint profile targets, the double divide per mantissa
        // this used to do was a software routine, and this loop was costing
        // more than the inverse transform (docs/platforms/esp32.md). The
        // value is the same - dequantize_mantissa_as says why - and the
        // exponent's 2^-exp is an exact scale in either type.
        const auto read_stream = [&](int s, int begin) {
            using Scalar = internal::decode_scalar_t;
            const auto index = static_cast<std::size_t>(s);
            const bool dither_eligible = s < nfchans && dithflag[static_cast<std::size_t>(s)];
            const int stream_norm = norm[index];
            for (int bin = begin; bin < endmant[index]; ++bin) {
                const int bap_value = bap[index][static_cast<std::size_t>(bin)];
                const int exp = exps[index][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    coeffs[index][static_cast<std::size_t>(bin)] =
                        dither_eligible
                            ? impl_->dither_.next_as<Scalar>() *
                                  exponent_scale<Scalar>(exp - stream_norm)
                            : Scalar{0};
                    continue;
                }
                const auto code = mantissa_reader.read(r, bap_value);
                coeffs[index][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa_as<Scalar>(code, bap_value) *
                    exponent_scale<Scalar>(exp - stream_norm);
            }
        };

        // §3.4.4 + §3.4.5: an AHT stream's mantissas exist only in block 0 -
        // one gaqmod, its gain words, then per bin a VQ index (hebap 1-7) or
        // six gain-adaptively-quantized codewords (hebap 8-19), covering all
        // six blocks at once. `bap[s]` already holds hebap, not ordinary bap,
        // because its BitAllocRegion was built with high_efficiency=true.
        const auto decode_aht_stream = [&](int s, int begin) -> std::expected<void, DecodeError> {
            const auto us = static_cast<std::size_t>(s);
            // First AHT use on this decoder sizes the frame-lifetime buffer;
            // the slot clear keeps the read side's invariant that bins this
            // decode does not write - past endmant, below `begin` - read
            // zero, which the freshly-allocated buffer used to provide.
            if (aht_coeffs.size() < static_cast<std::size_t>(kMaxSubstreamStreams)) {
                aht_coeffs.resize(static_cast<std::size_t>(kMaxSubstreamStreams));
            }
            aht_coeffs[us] = {};
            const int end = endmant[us];
            // The stream's frame exponent (block_norm.hpp): its six blocks
            // are dequantised here at once, and their reconstructed peaks
            // are all known before any is stored, so the exponent is exact -
            // found once every bin is reconstructed, then applied.
            auto& effective_exps = impl_->aht_eff_exps_[us];
            int smallest_effective = internal::kNoExponent;
            if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                effective_exps.assign(256, internal::kNoExponent);
            }
            const auto& hebap = bap[us];

            const auto gaqmod = static_cast<int>(r.read(2));
            // Both on impl_ (see its scratch block): assign()ed to the Gk=1
            // default and cleared respectively, which is exactly the state the
            // fresh vectors they replace arrived in.
            auto& gain = impl_->aht_gain_;
            gain.assign(static_cast<std::size_t>(end), 1);  // default Gk=1
            auto& gain_carrying_bins = impl_->aht_gain_bins_;
            gain_carrying_bins.clear();
            for (int bin = begin; bin < end; ++bin) {
                if (eac3::aht_gaq_has_gain(hebap[static_cast<std::size_t>(bin)], gaqmod)) {
                    gain_carrying_bins.push_back(bin);
                }
            }
            if (gaqmod == 3) {
                // Table E3.4, base-3 unpacked: three three-state gains to a
                // 5-bit word, most significant first - the mirror image of
                // the encoder's packing.
                for (std::size_t i = 0; i < gain_carrying_bins.size(); i += 3) {
                    const auto packed = r.read(5);
                    const std::array<std::uint32_t, 3> mapped = {packed / 9, (packed % 9) / 3,
                                                                 (packed % 9) % 3};
                    for (std::size_t t = 0; t < 3 && i + t < gain_carrying_bins.size(); ++t) {
                        gain[static_cast<std::size_t>(gain_carrying_bins[i + t])] =
                            eac3::aht_gaq_gain_from_mapped(static_cast<int>(mapped[t]));
                    }
                }
            } else if (gaqmod != 0) {
                const int alt = gaqmod == 1 ? 2 : 4;
                for (const int bin : gain_carrying_bins) {
                    gain[static_cast<std::size_t>(bin)] = r.read(1) != 0 ? alt : 1;
                }
            }
            if (block_trace != nullptr) {
                // §E3.4.4.2's gain words exist once per frame, here in block
                // 0 - the only place either side can record them, and the
                // one AHT quantity that is neither transmitted plainly nor
                // derivable from the allocation.
                const auto slot = s == kCplStream ? static_cast<std::size_t>(nchans)
                                                  : static_cast<std::size_t>(s);
                if (slot < block_trace->streams.size()) {
                    auto& traced = block_trace->streams[slot];
                    traced.gaqmod = gaqmod;
                    traced.gain.assign(gain.size(), 1);
                    for (std::size_t i = 0; i < gain.size(); ++i) {
                        traced.gain[i] = static_cast<std::uint8_t>(gain[i]);
                    }
                }
            }

            for (int bin = begin; bin < end; ++bin) {
                const auto ubin = static_cast<std::size_t>(bin);
                const int hb = hebap[ubin];
                // Six mantissas, the six-point inverse and the exponent scale
                // all in the coefficient store's type - see read_stream above
                // for why, and eac3_tools.hpp's float aht_inverse for what the
                // float form of the inverse is and is not.
                using Scalar = internal::decode_scalar_t;
                std::array<Scalar, kBlocksPerFrame> mantissas{};
                if (hb >= 1 && hb <= 7) {
                    const auto book = tables::aht_vq_table(hb);
                    const auto index = r.read(eac3::aht_bin_bits(hb));
                    if (index >= book.size()) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        mantissas[j] = internal::vq_entry<Scalar>(book[index][j]);
                    }
                } else if (hb >= 8) {
                    const int mantissa_bits = eac3::aht_mantissa_bits(hb);
                    // hebap is clamped to kHeBapTab's 0..19 range inside
                    // compute_bit_allocation, so this always holds for
                    // hb >= 8 - matching the invariant aht_quantize_mantissa
                    // (the encode direction) already asserts on the same
                    // grounds, rather than a second, redundant runtime check.
                    // The assert alone does not satisfy the static analyzer
                    // in a build where it compiles out (NDEBUG), hence the
                    // NOLINT below on the same proven-safe grounds.
                    assert(mantissa_bits >= 3);
                    const int g = gain[ubin];
                    // The bin's constants once, its six codewords through them.
                    const eac3::AhtGaqDequantizer<Scalar> dequantize{mantissa_bits, g};
                    const int small_bits = dequantize.small_bits;
                    const int large_bits = dequantize.large_bits;
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        const auto raw = r.read(small_bits);
                        bool has_escape = false;
                        std::uint32_t escape = 0;
                        // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift)
                        if (g != 1 && raw == (1u << (small_bits - 1))) {
                            has_escape = true;
                            escape = r.read(large_bits);
                        }
                        mantissas[j] = dequantize(raw, escape, has_escape);
                    }
                }
                // hb == 0: mantissas stays all-zero.
                std::array<Scalar, kBlocksPerFrame> blocks{};
                eac3::aht_inverse(mantissas, blocks);
                const int exp = exps[us][ubin];
                if constexpr (internal::kNormalisedStore<Scalar>) {
                    // Unscaled for now; the frame's exponent is applied below
                    // once every bin's peak is known.
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        aht_coeffs[us][j][ubin] = blocks[j];
                    }
                    const int effective = internal::aht_effective_exponent(blocks, exp);
                    effective_exps[ubin] = effective;
                    smallest_effective = std::min(smallest_effective, effective);
                } else {
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        aht_coeffs[us][j][ubin] = blocks[j] * exponent_scale<Scalar>(exp);
                    }
                }
            }
            int aht_norm = 0;
            if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                aht_norm = internal::store_norm(smallest_effective);
                for (int bin = begin; bin < end; ++bin) {
                    const auto ubin = static_cast<std::size_t>(bin);
                    const int shift = aht_norm - exps[us][ubin];
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        aht_coeffs[us][j][ubin] =
                            internal::scalar_ldexp(aht_coeffs[us][j][ubin], shift);
                    }
                }
            }
            impl_->aht_norm_[us] = aht_norm;
            return {};
        };
        const auto read_stream_dispatch = [&](int s,
                                              int begin) -> std::expected<void, DecodeError> {
            const auto us = static_cast<std::size_t>(s);
            if (frm->ahtinu[us]) {
                if (blk == 0) {
                    // Its own zone inside eac3_mantissas: an AHT stream's
                    // six blocks of mantissas arrive here at once, and the
                    // inverse transform behind them is the part of a
                    // mantissa read that is not a bitstream read.
                    AC3_ZONE_SCOPED_N("eac3_aht");
                    if (const auto result = decode_aht_stream(s, begin); !result) {
                        return result;
                    }
                }
                coeffs[us] = aht_coeffs[us][static_cast<std::size_t>(blk)];
                // Under the frame's exact exponent (zero in the floating
                // tiers); a tool that needs more room lowers it where it
                // runs (block_norm.hpp's renormalise).
                norm[us] = impl_->aht_norm_[us];
                return {};
            }
            read_stream(s, begin);
            return {};
        };

        // Every stream's quantized mantissas off the wire - or, for an AHT
        // stream, its whole frame of them out of block 0 (§3.4.4).
        {
            AC3_ZONE_SCOPED_N("eac3_mantissas");
            bool read_coupling = false;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (const auto result = read_stream_dispatch(ch, 0); !result) {
                    return std::unexpected(result.error());
                }
                if (frm->cplinu[static_cast<std::size_t>(blk)] &&
                    chincpl[static_cast<std::size_t>(ch)] && !read_coupling) {
                    const auto shared = read_stream_dispatch(kCplStream, cplstrtmant);
                    if (!shared.has_value()) {
                        return std::unexpected(shared.error());
                    }
                    read_coupling = true;
                }
            }
            if (bsi->lfe) {
                if (const auto result = read_stream_dispatch(nfchans, 0); !result) {
                    return std::unexpected(result.error());
                }
            }
        }

        // §7.4.3 decoupling: each coupled channel's high band is the shared
        // channel scaled by that channel's coordinate, times 8 - undoing the
        // encoder's /8 headroom scaling. Standard coupling only: it has no
        // neighbor-block dependency, so it finishes right here, same as
        // every other tool. Enhanced coupling's own reconstruction (§3.5.5)
        // needs the block AFTER this one, which the bitstream has not
        // reached yet - so it, and everything that runs after decoupling
        // for EVERY block (spx synthesis, rematrixing, IMDCT), is deferred
        // to a second pass below that runs once every block has been parsed
        // and can therefore look at any block's neighbors freely, still in
        // strict block order (IMDCT's overlap-add delay line requires that
        // regardless of coupling mode).
        if (frm->cplinu[static_cast<std::size_t>(blk)] && !ecplinu_now) {
            AC3_ZONE_SCOPED_N("eac3_decoupling");
            const auto& shared = coeffs[static_cast<std::size_t>(kCplStream)];
            const auto& cpl_bap = bap[static_cast<std::size_t>(kCplStream)];
            const auto& cpl_exps = exps[static_cast<std::size_t>(kCplStream)];
            const int shared_norm = norm[static_cast<std::size_t>(kCplStream)];
            // The shared channel's smallest exponent over a band - its exact
            // ones when it is an AHT stream (block_norm.hpp).
            [[maybe_unused]] const auto shared_min = [&](int low, int high) {
                return frm->ahtinu[static_cast<std::size_t>(kCplStream)]
                           ? internal::min_exponent(
                                 impl_->aht_eff_exps_[static_cast<std::size_t>(kCplStream)], low,
                                 high)
                           : internal::min_exponent(cpl_exps, low, high);
            };
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& target = coeffs[static_cast<std::size_t>(ch)];
                const bool ch_dither = dithflag[static_cast<std::size_t>(ch)];
                // In the coefficient store's type throughout, for the reason
                // read_stream gives. Same value: the coordinate has at most
                // five significant bits, so the product with a stored
                // coefficient rounds once in either type, and 8 and the sign
                // are exact.
                using Scalar = internal::decode_scalar_t;
                if constexpr (internal::kNormalisedStore<Scalar>) {
                    // The room this channel's coupled bands need, from the
                    // shared channel's exponents and each band's coordinate
                    // (block_norm.hpp); the channel comes down to it first.
                    int wanted = internal::kNoExponent;
                    for (std::size_t bnd = 0; bnd < subband_band.size(); ++bnd) {
                        const int low =
                            cplstrtmant + static_cast<int>(bnd) * coupling::kBinsPerSubBand;
                        const int high = std::min(low + coupling::kBinsPerSubBand, cplendmant);
                        wanted = std::min(wanted, internal::coupled_exponent(
                                                      shared_min(low, high),
                                                      cplco_exp[static_cast<std::size_t>(ch)][bnd]));
                    }
                    internal::renormalise(target, norm[static_cast<std::size_t>(ch)],
                                          internal::store_norm(wanted));
                }
                for (int bnd = 0; bnd < static_cast<int>(subband_band.size()); ++bnd) {
                    const Scalar coordinate =
                        cplco[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bnd)];
                    // §7.4.1: a set phase flag negates the right channel of a
                    // 2/0 pair across that band, restoring the phase the
                    // coupling sum discarded.
                    const Scalar sign = (phsflginu && ch == 1 &&
                                         phsflg[static_cast<std::size_t>(
                                             subband_band[static_cast<std::size_t>(bnd)])])
                                            ? Scalar{-1}
                                            : Scalar{1};
                    const int low = cplstrtmant + bnd * coupling::kBinsPerSubBand;
                    const int high = std::min(low + coupling::kBinsPerSubBand, cplendmant);
                    // The fixed-point store's shift from the shared channel's
                    // exponent to this one's, with the coordinate's power of
                    // two and the eight folded in (block_norm.hpp).
                    int shift = 0;
                    if constexpr (internal::kNormalisedStore<Scalar>) {
                        shift = norm[static_cast<std::size_t>(ch)] - shared_norm -
                                cplco_exp[static_cast<std::size_t>(ch)][static_cast<std::size_t>(bnd)] +
                                3;
                    }
                    for (int bin = low; bin < high; ++bin) {
                        const std::size_t ubin = static_cast<std::size_t>(bin);
                        // §7.3.4: independent per-channel dither for a
                        // zero-bap shared bin, run through the same
                        // extraction formula a real coupling coefficient
                        // uses - see decoder.cpp's own copy of this comment
                        // for why reusing one dithered coupling-domain
                        // sample across channels would be wrong.
                        const Scalar coeff =
                            (cpl_bap[ubin] == 0 && ch_dither)
                                ? impl_->dither_.next_as<Scalar>() *
                                      exponent_scale<Scalar>(cpl_exps[ubin] - shared_norm)
                                : shared[ubin];
                        if constexpr (internal::kNormalisedStore<Scalar>) {
                            target[ubin] = internal::scalar_ldexp(coeff * coordinate * sign, shift);
                        } else {
                            target[ubin] = coeff * coordinate * Scalar{8} * sign;
                        }
                    }
                }
            }
        }

        // Stash this block's raw enhanced coupling channel mantissas
        // regardless of mode - a NEIGHBORING block that used enhanced
        // coupling needs them even when this block did not (§3.5.5.1's own
        // zero-substitution rule reads them via ecpl_active below).
        if (frm->cplinu[static_cast<std::size_t>(blk)] && ecplinu_now) {
            if (ecpl_all_coeffs.empty()) {
                ecpl_all_coeffs.resize(static_cast<std::size_t>(kBlocksPerFrame));
            }
            // A whole-array assignment in the store's own type: the spectrum
            // routine it feeds exists in both scalars now.
            ecpl_all_coeffs[static_cast<std::size_t>(blk)] =
                coeffs[static_cast<std::size_t>(kCplStream)];
            ecpl_active[static_cast<std::size_t>(blk)] = true;
        }

        // Snapshot everything the second pass needs to finish this block.
        auto& tail = tails[static_cast<std::size_t>(blk)];
        // tail.coeffs needs nothing here: pass one wrote this block's spectra
        // into it directly (see the alias at the top of the block).
        tail.chincpl = chincpl;
        tail.cplinu = frm->cplinu[static_cast<std::size_t>(blk)];
        tail.ecplinu_now = ecplinu_now;
        if (tail.cplinu && tail.ecplinu_now) {
            tail.firstchincpl = -1;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (chincpl[static_cast<std::size_t>(ch)]) {
                    tail.firstchincpl = ch;
                    break;
                }
            }
            tail.ecplangleintrp = ecplangleintrp;
            tail.ecpl_begin_subbnd = ecpl_begin_subbnd;
            tail.ecpl_end_subbnd = ecpl_end_subbnd;
            tail.ecpl_structure = ecpl_structure;
            tail.ecplamp_raw = ecplamp_raw;
            tail.ecplangle_raw = ecplangle_raw;
            tail.ecplchaos_raw = ecplchaos_raw;
            tail.ecpltrans = ecpltrans_persist;
        }
        tail.cplstrtmant = cplstrtmant;
        tail.cplendmant = cplendmant;
        tail.spxinu = spxinu;
        tail.chinspx = chinspx;
        tail.spx_bands = spx_bands;
        tail.spxco = spxco;
        if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
            tail.spxco_exp = spxco_exp;
        }
        tail.spxblnd = spxblnd;
        tail.spx_startmant = spx_startmant;
        tail.spx_endmant = spx_endmant;
        tail.spx_copystart = spx_copystart;
        tail.rematflg = rematflg;
        tail.blksw = blksw;
        tail.endmant = endmant;

        // The three remaining per-block tool answers, taken from the same
        // snapshot the second pass reads rather than from where each was
        // parsed: spxinu and the coupling flags persist across blocks, so
        // "was it in use for THIS block" is only settled here.
        if (syntax != nullptr) {
            syntax->spectral_extension = tail.spxinu;
            syntax->enhanced_coupling = tail.cplinu && tail.ecplinu_now;
            syntax->rematrixing = bsi->acmod == Acmod::k2_0 &&
                                  std::ranges::any_of(rematflg, [](bool on) { return on; });
        }

        if (r.overflowed()) {
            return std::unexpected(DecodeError::kTruncated);
        }
    }

    // impl_->config_.skip_reconstruction stops here. Everything above read the wire
    // - bsi, audfrm, every block's side information and its mantissas - and
    // everything below turns what it read into audio: enhanced coupling's
    // reconstruction, spectral extension, rematrixing, the §7.7 gain, the
    // IMDCT, JOC's object reconstruction and the transient pre-noise
    // holdback. `out` is already complete as metadata, so it goes back as
    // it stands, with no channels and no object audio.
    //
    // Returning here also bypasses the pre-noise holdback deliberately: that
    // exists so a correction reaching back into the previous frame can be
    // applied before that frame is handed over, which is a statement about
    // audio. A parse has no such dependency and a caller walking a file
    // wants one report per syncframe, in order.
    if (impl_->config_.skip_reconstruction) {
        return std::optional<DecodedSubstream>(std::move(out));
    }

    // Second pass: finish every block in order. Standard-coupled, plain and
    // AHT channels already carry their final coefficients from pass one
    // above; only enhanced coupling's own reconstruction happens here, right
    // before the spx/rematrix/IMDCT tail every block goes through.
    for (int blk = 0; blk < nblks; ++blk) {
        AC3_ZONE_SCOPED_N("eac3_reconstruct_block");
        auto& tail = tails[static_cast<std::size_t>(blk)];
        auto& coeffs = tail.coeffs;

        if (tail.cplinu && tail.ecplinu_now) {
            AC3_ZONE_SCOPED_N("eac3_ecpl_reconstruct");
            // §3.5.5: reconstruct each coupled channel from the enhanced
            // coupling channel, using this block's neighbors. A neighbor is
            // zero when the adjacent block did not use enhanced coupling
            // (§3.5.5.1's own rule) - which includes this syncframe's first
            // and last block, whose true neighbor lives in an adjacent
            // syncframe this call was not given (see this function's
            // comment on `prev_ecpl_coeffs` above).
            static constexpr std::array<internal::decode_scalar_t, 256> kZero{};
            const auto& prev = (blk > 0 && ecpl_active[static_cast<std::size_t>(blk - 1)])
                                   ? ecpl_all_coeffs[static_cast<std::size_t>(blk - 1)]
                                   : kZero;
            const auto& next = (blk + 1 < nblks && ecpl_active[static_cast<std::size_t>(blk + 1)])
                                   ? ecpl_all_coeffs[static_cast<std::size_t>(blk + 1)]
                                   : kZero;
            const int prev_norm = (blk > 0 && ecpl_active[static_cast<std::size_t>(blk - 1)])
                                      ? tails[static_cast<std::size_t>(blk - 1)]
                                            .norm[static_cast<std::size_t>(kCplStream)]
                                      : 0;
            const int next_norm =
                (blk + 1 < nblks && ecpl_active[static_cast<std::size_t>(blk + 1)])
                    ? tails[static_cast<std::size_t>(blk + 1)].norm[static_cast<std::size_t>(kCplStream)]
                    : 0;
            if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                // The room each coupled channel needs for the reconstruction
                // (block_norm.hpp): the shared channel's own exponent, less
                // kEcplGuardBits for what the reconstruction can add.
                const int wanted = std::max(
                    tail.norm[static_cast<std::size_t>(kCplStream)] - internal::kEcplGuardBits,
                    internal::kNormFloor);
                for (int ch = 0; ch < nfchans; ++ch) {
                    const auto uch = static_cast<std::size_t>(ch);
                    if (tail.chincpl[uch]) {
                        internal::renormalise(coeffs[uch], tail.norm[uch], wanted);
                    }
                }
            }
            ecpl_reconstruct_block(tail, prev, ecpl_all_coeffs[static_cast<std::size_t>(blk)],
                                   next, prev_norm, next_norm, nfchans, ecpl_noise,
                                   impl_->ecpl_spectrum_real_,
                                   impl_->ecpl_spectrum_imag_, impl_->ecpl_amp_scratch_,
                                   impl_->ecpl_angle_scratch_, impl_->config_.fast_imdct,
                                   coeffs);
        }

        // §3.6.4 spectral extension synthesis: translate the low band up,
        // notch the seams, blend with noise to approximate the original
        // band's coarse energy, then scale by the transmitted coordinate.
        // Runs after decoupling, so a channel that is both coupled and
        // extended already has its coupling-restored content in place below
        // spx_startmant to copy from - coupling always ends exactly where
        // spx begins (§E3.3.1), so there is no gap and nothing to reconcile.
        if (tail.spxinu) {
            AC3_ZONE_SCOPED_N("eac3_spx");
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!tail.chinspx[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& tc = coeffs[static_cast<std::size_t>(ch)];
                if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                    // The room the synthesis needs (block_norm.hpp's
                    // spx_room), from the copy source as it stands and this
                    // channel's smallest coordinate exponent.
                    const auto& exponents = tail.spxco_exp[static_cast<std::size_t>(ch)];
                    int smallest = internal::kNoExponent;
                    for (const int value : exponents) {
                        smallest = std::min(smallest, value);
                    }
                    const int room = internal::spx_room(tc, tail.spx_copystart,
                                                        tail.spx_startmant, smallest);
                    internal::renormalise(tc, tail.norm[static_cast<std::size_t>(ch)],
                                          tail.norm[static_cast<std::size_t>(ch)] - room);
                }

                // §3.6.4.1 Transform Coefficient Translation: copy low-band
                // coefficients up into the extension region, banded, wrapping
                // the copy source back to spx_copystart whenever a band would
                // run past spx_startmant. copyindex never leaves
                // [spx_copystart, spx_startmant) - strictly below the region
                // this loop writes into - so mutating tc in place is safe.
                // The whole synthesis in the coefficient store's type. At
                // double this stage was 57% of a 5.1 decode on an ESP32-S3 -
                // the blend below, the noise draw and the band arithmetic
                // were each software routines there - and the float form
                // differs from it by the rounding of sums and products, the
                // same class of difference the float store already accepted
                // at the transform.
                using Scalar = internal::decode_scalar_t;
                std::array<bool, eac3::kMaxSubBands> wrapflag{};
                std::array<Scalar, eac3::kMaxSubBands> band_rms{};
                int copyindex = tail.spx_copystart;
                for (int bnd = 0; bnd < tail.spx_bands.count; ++bnd) {
                    const auto ubnd = static_cast<std::size_t>(bnd);
                    const int size = tail.spx_bands.size[ubnd];
                    const int low = tail.spx_bands.start[ubnd];
                    if (copyindex + size > tail.spx_startmant) {
                        copyindex = tail.spx_copystart;
                        wrapflag[ubnd] = true;
                    }
                    BandEnergy<Scalar> energy;
                    for (int i = 0; i < size; ++i) {
                        if (copyindex == tail.spx_startmant) {
                            copyindex = tail.spx_copystart;
                        }
                        const auto value = tc[static_cast<std::size_t>(copyindex++)];
                        tc[static_cast<std::size_t>(low + i)] = value;
                        energy.add(value);
                    }
                    band_rms[ubnd] = energy.rms(size);
                }

                // §3.6.4.2.3 Band Border Filtering: the notch runs on the
                // already-translated, not-yet-blended region, using RMS
                // measured before it (matching the encoder's own order).
                eac3::spx_apply_notch(
                    std::span{tc}.subspan(
                        static_cast<std::size_t>(tail.spx_startmant),
                        static_cast<std::size_t>(tail.spx_endmant - tail.spx_startmant)),
                    tail.spx_startmant, tail.spx_bands, wrapflag,
                    frm->spxattencod[static_cast<std::size_t>(ch)]);

                // §3.6.4.2.4 Noise Scaling and Blending, then §3.6.4.3
                // Blended Transform Coefficient Scaling.
                const int blend = tail.spxblnd[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < tail.spx_bands.count; ++bnd) {
                    const auto ubnd = static_cast<std::size_t>(bnd);
                    const int size = tail.spx_bands.size[ubnd];
                    const int low = tail.spx_bands.start[ubnd];
                    const Scalar nratio =
                        eac3::spx_noise_ratio_as<Scalar>(low, size, tail.spx_endmant, blend);
                    const Scalar nscale = band_rms[ubnd] * internal::scalar_sqrt(nratio);
                    const Scalar sscale = internal::scalar_sqrt(Scalar{1} - nratio);
                    if constexpr (internal::kNormalisedStore<Scalar>) {
                        // The coordinate's mantissa, then its power of two and
                        // §E3.6.4.3's thirty-two as one shift (block_norm.hpp).
                        const Scalar mantissa = tail.spxco[static_cast<std::size_t>(ch)][ubnd];
                        const int shift = 5 - tail.spxco_exp[static_cast<std::size_t>(ch)][ubnd];
                        for (int i = 0; i < size; ++i) {
                            const auto at = static_cast<std::size_t>(low + i);
                            tc[at] = internal::scalar_ldexp(
                                (tc[at] * sscale + spx_noise.next_as<Scalar>() * nscale) *
                                    mantissa,
                                shift);
                        }
                    } else {
                        const Scalar coordinate =
                            tail.spxco[static_cast<std::size_t>(ch)][ubnd] * Scalar{32};
                        for (int i = 0; i < size; ++i) {
                            const auto at = static_cast<std::size_t>(low + i);
                            tc[at] = (tc[at] * sscale + spx_noise.next_as<Scalar>() * nscale) *
                                     coordinate;
                        }
                    }
                }
            }
        }

        if (bsi->acmod == Acmod::k2_0) {
            if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                // One exponent for the pair, with room for the sum
                // (block_norm.hpp).
                if (!tail.rematrix_room &&
                    std::ranges::any_of(tail.rematflg, [](bool on) { return on; })) {
                    const int shared = std::max(
                        std::min(tail.norm[0], tail.norm[1]) - internal::kRematrixGuardBits,
                        internal::kNormFloor);
                    internal::renormalise(coeffs[0], tail.norm[0], shared);
                    internal::renormalise(coeffs[1], tail.norm[1], shared);
                }
            }
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, up to the
            // lower bandwidth of the two channels.
            const int cap = std::min(tail.endmant[0], tail.endmant[1]) - 1;
            for (std::size_t band = 0; band < kRematrixBands.size(); ++band) {
                if (!tail.rematflg[band]) {
                    continue;
                }
                const int high = std::min(kRematrixBands[band][1], cap);
                for (int bin = kRematrixBands[band][0]; bin <= high; ++bin) {
                    // Sum and difference of two stored coefficients - exact in
                    // whatever type they are stored in, so this follows them
                    // rather than detouring through double.
                    const auto l = coeffs[0][static_cast<std::size_t>(bin)];
                    const auto rr = coeffs[1][static_cast<std::size_t>(bin)];
                    coeffs[0][static_cast<std::size_t>(bin)] = l + rr;
                    coeffs[1][static_cast<std::size_t>(bin)] = l - rr;
                }
            }
        }

        // §7.7 gain, applied to the COEFFICIENTS rather than to the output
        // samples - same reasoning and the same block_gain helper as the
        // legacy AC-3 decoder (decoder.cpp): the overlap-add window then
        // cross-fades one block's gain into the next, which is what keeps a
        // per-block gain change from clicking. Applied to every coded
        // channel including the LFE; the coupling channel is skipped
        // because it is never one of the nchans real channels here (standard
        // decoupling and, for enhanced coupling, the reconstruction above
        // have already spread it into the channels above). Dual mono's two
        // channels are independent programmes, so Ch2 gets its own gain
        // from its own words (out.dynrng2/out.compr2) rather than sharing
        // Ch1's.
        for (int ch = 0; ch < nchans; ++ch) {
            const bool second_programme = bsi->acmod == Acmod::kDualMono && ch == 1;
            const double drc =
                second_programme
                    ? internal::block_gain(impl_->config_,
                                           out.dynrng2[static_cast<std::size_t>(blk)], out.compr2)
                    : internal::block_gain(impl_->config_,
                                           out.dynrng[static_cast<std::size_t>(blk)], out.compr);
            if (drc != 1.0) {
                double gain = drc;
                if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                    // The gain's power of two goes into the block exponent
                    // and only its mantissa, in [0.5, 1), into the
                    // coefficients (block_norm.hpp).
                    int power = 0;
                    gain = std::frexp(drc, &power);
                    tail.norm[static_cast<std::size_t>(ch)] -= power;
                }
                // Narrowed once, not per coefficient: the gain is one number
                // for the whole block, and rounding it here costs a single
                // rounding step instead of 256 round trips through double.
                const auto block_scale = static_cast<internal::decode_scalar_t>(gain);
                for (auto& value : coeffs[static_cast<std::size_t>(ch)]) {
                    value *= block_scale;
                }
            }
        }

        // The transform pair plus the overlap-add that reconstructs PCM from it -
        // where a decode frame spends most of its time, and the stage
        // DecoderConfig::fast_imdct's default switched under in 0.9.0.
        {
            AC3_ZONE_SCOPED_N("eac3_imdct_overlap");
            for (int ch = 0; ch < nchans; ++ch) {
                const auto index = static_cast<std::size_t>(ch);
                auto& x = impl_->imdct_scratch_;
                // Two overloads, one call site. The float32 inverse takes no
                // `fast` parameter - the direct form is double-only - and this
                // profile has already refused fast_imdct=false with
                // kUnsupported long before reaching here, so there is no
                // choice being silently dropped.
                const bool short_block = ch < nfchans && tail.blksw[static_cast<std::size_t>(ch)];
                internal::inverse_transform_into(coeffs[index], x, short_block,
                                       impl_->config_.fast_imdct);
                auto& history = delay[index];
                auto& pcm = out.channels[index];
                if constexpr (internal::kNormalisedStore<internal::decode_scalar_t>) {
                    // The two halves under their own exponents, aligned and
                    // scaled once (block_norm.hpp).
                    internal::overlap_add_normalised(
                        x, history, delay_norm[index], tail.norm[index],
                        std::span<float>{pcm}.subspan(
                            static_cast<std::size_t>(blk) * kSamplesPerBlock, kSamplesPerBlock));
                } else {
                    for (int n = 0; n < kSamplesPerBlock; ++n) {
                        pcm[static_cast<std::size_t>(blk * kSamplesPerBlock + n)] =
                            static_cast<float>(internal::decode_scalar_t{2} *
                                               (x[static_cast<std::size_t>(n)] +
                                                history[static_cast<std::size_t>(n)]));
                        history[static_cast<std::size_t>(n)] = x[static_cast<std::size_t>(256 + n)];
                    }
                }
                // §7.10's raw material, captured into scratch rather than
                // straight into impl_->retained_: this frame may still be refused
                // further down, and a refused frame must not become what the
                // NEXT loss is reconstructed from.
                if (retain_last_block && blk == nblks - 1 &&
                    index < impl_->conceal_scratch_.size()) {
                    // Element-wise: the retained block is double whatever the
                    // store's scalar is (a loss path, not the decode's), and the
                    // fixed-point scalar widens only explicitly.
                    const int stored_norm = tail.norm[index];
                    std::transform(x.begin(), x.end(), impl_->conceal_scratch_[index].begin(),
                                   [stored_norm](internal::decode_scalar_t v) {
                                       return internal::widen_stored(v, stored_norm);
                                   });
                }
            }
        }
    }

    // §3.7: apply any transient pre-noise correction THIS frame's fields
    // specify, against this frame's own head plus whatever the previous
    // frame (still held back in `impl_->pending_`, if any) contributed as its
    // tail - the only combination a correction can ever need, because
    // transprocloc is relative to this frame's own first sample and this
    // decoder keeps exactly one frame of lookback (see decode_substream's
    // own doc comment; a stream needing more is refused rather than read
    // out of bounds).
    const int key = static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid;

    // --- JOC audio reconstruction -----------------------------------------
    // JOC's outputs are the program's objects with the LFE positions removed
    // (§6.3.2.2 bypasses them), which oba::joc_object_indices() spells out.
    // For the dynamic-object-only program AtmosEncoder writes, that is
    // exactly object_metadata->objects index for index; for a bed program -
    // what channel-based-immersive third-party content is - it is the bed's
    // own channels, and out.object_indices is what says which.
    {
        AC3_ZONE_SCOPED_N("eac3_joc_reconstruct");
        // DecoderConfig::skip_object_reconstruction stops here rather than
        // further in, so the ReconstructionState is never allocated at all -
        // which is the point of the flag on a target where that one 147,504-byte
        // block is the thing that will not fit. object_metadata is already
        // parsed and stays; only the audio the objects would carry is skipped.
        if (out.object_metadata && !joc_bytes.empty() &&
            !impl_->config_.skip_object_reconstruction) {
            const auto params = oba::joc::parse_payload(joc_bytes);
            const auto indices = oba::joc_object_indices(out.object_metadata->program);
            // §6.3.2.2 Table 47: the JOC downmix is the five channels this
            // substream carries, in JOC order. A 7-channel downmix needs
            // Lb/Rb from a dependent substream, which decode_substream does
            // not have in hand here, so those configurations parse but do
            // not reconstruct.
            if (params && params->objects == static_cast<int>(indices.size()) &&
                params->channels == oba::joc::kNumChannels5X) {
                constexpr std::array<int, oba::joc::kNumChannels5X> kAc3FromJoc = {0, 2, 1, 3, 4};
                // Spans, not copies: this permutation used to deep-copy five
                // channels (~30 KB a frame) purely to reorder them.
                std::array<std::span<const float>, oba::joc::kNumChannels5X> bed_joc_order{};
                bool have_bed =
                    static_cast<std::size_t>(oba::joc::kNumChannels5X) <= out.channels.size();
                for (int jc = 0; have_bed && jc < oba::joc::kNumChannels5X; ++jc) {
                    bed_joc_order[static_cast<std::size_t>(jc)] =
                        out.channels[static_cast<std::size_t>(
                            kAc3FromJoc[static_cast<std::size_t>(jc)])];
                }
                if (have_bed) {
                    auto& joc_slot = impl_->joc_state_[static_cast<std::size_t>(key)];
                    if (!joc_slot) {
                        joc_slot = std::make_unique<oba::joc::ReconstructionState>();
                    }
                    out.object_audio = oba::joc::reconstruct(
                        bed_joc_order, *params, *joc_slot, impl_->config_.fast_mdct,
                        impl_->config_.fast_imdct, impl_->config_.joc_domain);
                    out.object_indices = indices;
                }
            }
        }
    }

    // §7.10: this frame decoded, so its last block becomes what a future loss
    // of this identity is reconstructed from. Committed here, past every
    // return that refuses the frame - including transient pre-noise
    // processing's own kUnsupported refusal below, which is why this cannot
    // simply live at the end of the block loop.
    //
    // Deliberately BEFORE the §3.7 hold-back: a held-back frame has still
    // decoded, and its overlap tail is what the next frame of the identity
    // continues from whether or not the PCM has been released yet.
    if (retain_last_block) {
        auto& retained = impl_->retained_[static_cast<std::size_t>(key)];
        if (!retained) {
            retained = std::make_unique<Impl::RetainedSubstream>();
        }
        retained->nchans = nchans;
        for (int ch = 0;
             ch < nchans && static_cast<std::size_t>(ch) < impl_->conceal_scratch_.size(); ++ch) {
            retained->last_block[static_cast<std::size_t>(ch)] =
                impl_->conceal_scratch_[static_cast<std::size_t>(ch)];
        }
        // Metadata only - the PCM belongs to this frame, and object_metadata/
        // object_audio describe objects a concealed frame has no business
        // repeating (see conceal()).
        retained->shape = out;
        retained->shape.channels.clear();
        retained->shape.object_metadata = std::nullopt;
        retained->shape.object_audio.clear();
        retained->shape.concealed = std::nullopt;
        impl_->last_identity_ = key;
    }

    auto& pending_slot = impl_->pending_[static_cast<std::size_t>(key)];
    if (frm->transproce) {
        // One splice buffer for every processed channel, re-cleared per
        // channel rather than re-allocated: the zero fill is load-bearing
        // (with no pending frame the history half must read silence), the
        // 12 KB allocation per channel was not.
        std::vector<float> combined;
        for (int ch = 0; ch < nfchans; ++ch) {
            const auto uch = static_cast<std::size_t>(ch);
            if (!frm->chintransproc[uch]) {
                continue;
            }
            const int transloc = frm->transprocloc[uch];
            const int translen = frm->transproclen[uch];
            const auto range = transient_prenoise_range(transloc, translen);
            if (range.first < -kSamplesPerFrame || range.last > kSamplesPerFrame) {
                // Reaches further back or forward than the one frame of
                // history/lookahead this decoder buffers - recognised,
                // refused, not misdecoded (same stance as every other
                // syntax this project's own encoder does not exercise).
                return std::unexpected(DecodeError::kUnsupported);
            }
            combined.assign(static_cast<std::size_t>(kSamplesPerFrame) * 2, 0.0f);
            if (pending_slot.has_value()) {
                std::ranges::copy(pending_slot->channels[uch], combined.begin());
            }
            std::ranges::copy(out.channels[uch], combined.begin() + kSamplesPerFrame);
            apply_transient_prenoise(combined, kSamplesPerFrame + transloc, translen);
            if (pending_slot.has_value()) {
                std::ranges::copy(combined.begin(), combined.begin() + kSamplesPerFrame,
                                  pending_slot->channels[uch].begin());
            }
            std::ranges::copy(combined.begin() + kSamplesPerFrame, combined.end(),
                              out.channels[uch].begin());
        }
    }

    if (pending_slot.has_value()) {
        DecodedSubstream ready = std::move(*pending_slot);
        // Assign the optional, not through it - `*opt = v` needs the optional
        // engaged, which here is only true because of the has_value() above.
        pending_slot = std::move(out);
        return std::optional<DecodedSubstream>(std::move(ready));
    }
    if (frm->transproce) {
        // First frame to use the tool for this substream identity: hold it
        // back, nothing is ready to return yet - see decode_substream's own
        // doc comment.
        pending_slot = std::move(out);
        return std::optional<DecodedSubstream>(std::nullopt);
    }
    return std::optional<DecodedSubstream>(std::move(out));
}

int Eac3Decoder::latency_samples() const {
    // A slot holds a value exactly while that substream identity is one frame
    // behind (see decode_substream's hold-back), so "any slot pending" IS
    // "this decoder is currently a frame late". impl_->pending_au_parts_ is not
    // consulted: it holds results already RELEASED by decode_substream and
    // only waiting on a sibling identity, so whatever delay it represents is
    // the impl_->pending_ slot of that sibling, already counted here.
    for (const auto& slot : impl_->pending_) {
        if (slot.has_value()) {
            return kSamplesPerFrame;
        }
    }
    return 0;
}

std::vector<DecodedSubstream> Eac3Decoder::flush() {
    std::vector<DecodedSubstream> ready;
    // Slot order is key order, so this drains in the same ascending
    // identity order the maps this replaced iterated in.
    for (auto& slot : impl_->pending_) {
        if (slot.has_value()) {
            ready.push_back(std::move(*slot));
            slot.reset();
        }
    }
    // decode_access_unit's own assembly cache: whatever is left here is one
    // or more substreams whose sibling(s) never caught up before the stream
    // ended, so there is no complete DecodedAccessUnit to hand back for
    // them - the raw substreams, oldest first, are the best this can do (see
    // flush()'s own doc comment).
    for (auto& queue : impl_->pending_au_parts_) {
        for (auto& substream : queue) {
            ready.push_back(std::move(substream));
        }
        queue.clear();
    }
    // §7.8, applied here too so a stream that ends mid-hold-back hands its
    // last frames back at the same channel count every other frame of it came
    // out at - a sink opened for a stereo fold cannot take six channels for
    // the final access unit. Each flushed substream is folded on its own
    // because that is all there is: by definition the assembly these belong
    // to never completed (see this function's own doc comment), so there is
    // no rendered program to fold instead.
    for (auto& substream : ready) {
        // A local rather than impl_->au_views_: OutputStage keeps working storage
        // of its own and this is one call per stream, not a hot path.
        std::vector<std::span<float>> views;
        views.reserve(substream.channels.size());
        for (auto& channel : substream.channels) {
            views.emplace_back(channel);
        }
        const auto layout = eac3::chanmap::expand(substream.location_map());
        impl_->output_.apply(views, layout, substream.acmod, substream.lfe,
                             mix_levels(substream.mixing), substream.dialnorm);
        substream.channels.resize(
            output_channel_count(impl_->config_.output, substream.acmod, substream.lfe));
    }
    return ready;
}

std::expected<std::optional<DecodedAccessUnit>, DecodeError> Eac3Decoder::decode_access_unit(
    std::span<const std::byte> unit) {
    return decode_access_unit_core(unit, {});
}

std::expected<std::optional<DecodedAccessUnit>, DecodeError> Eac3Decoder::decode_access_unit_into(
    std::span<const std::byte> unit, std::span<const std::span<float>> channels) {
    return decode_access_unit_core(unit, channels);
}

std::expected<std::optional<DecodedAccessUnit>, DecodeError>
Eac3Decoder::decode_access_unit_by_block(std::span<const std::byte> unit, BlockSink sink) {
    return decode_access_unit_core(unit, {}, &sink);
}

// §5.4.2.8/§7.8 over an assembled program, in whichever storage it landed -
// the result's own vectors, or the caller's spans when decode_access_unit_into
// supplied them. A no-op unless DecoderConfig::output asks for something, and
// the one place the fold happens for the access-unit forms.
void Eac3Decoder::apply_output(DecodedAccessUnit& out, std::span<const std::span<float>> external) {
    if (impl_->config_.output.target == DownmixTarget::kAsCoded &&
        impl_->config_.output.mode == OperatingMode::kCustom &&
        !impl_->config_.output.apply_dialnorm) {
        return;
    }
    // impl_->config_.skip_reconstruction leaves `out.channels` empty (harmless below)
    // but, for the _into form, leaves `external` UNWRITTEN - there is no PCM
    // to fold, and folding those spans anyway would read stale caller memory
    // and, for a caller who also asked for a downmix, silently overwrite
    // buffers this call promised to leave untouched.
    if (impl_->config_.skip_reconstruction) {
        return;
    }
    const auto slots =
        out.channels.empty() ? static_cast<std::size_t>(out.layout.count) : out.channels.size();
    impl_->au_views_.clear();
    if (external.empty()) {
        for (auto& channel : out.channels) {
            impl_->au_views_.emplace_back(channel);
        }
    } else {
        for (std::size_t i = 0; i < slots && i < external.size(); ++i) {
            impl_->au_views_.emplace_back(external[i]);
        }
    }
    // `lfe` here is about the RENDERED layout, not the bed's own lfeon: a
    // dependent can add an LFE2 the bed never had. render_output only reads
    // it on the pass-through path anyway (the fold works out for itself which
    // seats the layout filled), but passing the rendered answer keeps the two
    // in agreement.
    const bool rendered_lfe = out.layout.index_of(eac3::chanmap::Location::kLfe) >= 0;
    {
        // Outside eac3_decode_access_unit, so it reports as its own root
        // zone: this is the fold the caller's config asked for, applied to a
        // finished program, not part of decoding one.
        AC3_ZONE_SCOPED_N("eac3_output");
        impl_->output_.apply(impl_->au_views_, out.layout, out.acmod, rendered_lfe,
                             mix_levels(out.mixing), out.dialnorm);
    }
    if (!external.empty()) {
        return;
    }
    // output_channel_count() keys off acmod, which describes the BED; what
    // came back here is a fold of the assembled program, so the count comes
    // from the same three cases it does, read against the rendered layout.
    if (impl_->config_.output.target == DownmixTarget::kAsCoded || out.acmod == Acmod::kDualMono) {
        return;
    }
    out.channels.resize(impl_->config_.output.target == DownmixTarget::kMono ? 1U : 2U);
}

std::expected<std::optional<DecodedAccessUnit>, DecodeError> Eac3Decoder::decode_access_unit_core(
    std::span<const std::byte> unit, std::span<const std::span<float>> external,
    const BlockSink* sink) {
    AC3_ZONE_SCOPED_N("eac3_decode_access_unit");
    AC3_ZONE_BEGIN(split_zone, "eac3_au_split");
    const auto frames = split_frames(unit);
    AC3_ZONE_END(split_zone);
    if (!frames.has_value()) {
        return std::unexpected(frames.error());
    }
    if (frames->empty()) {
        return std::unexpected(DecodeError::kInvalidStream);
    }

    // §E2.3.1.2 programme selection, ahead of any decoding at all: a unit
    // belonging to another programme is skipped whole rather than decoded and
    // discarded, so none of this decoder's per-identity state (overlap-add,
    // JOC continuity, the §3.7 hold-back queues) ever advances for a
    // programme the caller did not ask for. The unit's first frame is by
    // definition its independent substream, and its substreamid IS the
    // programme id - EXCEPT for §E2.3.1.2's legacy core, which carries neither
    // field. An AC-3 frame's bits 16-17 are the top of crc1, not strmtyp, so
    // parse_bsi here would read a programme id out of a checksum: on the
    // FFmpeg FATE fixture the_great_wall_7.1.eac3 that lands on strmtyp 0x3
    // and the whole decode fails with kReservedValue, and on the 59% of that
    // file's frames whose crc1 happens to start 00/01/10 it would instead have
    // parsed as a plausible id and silently selected the wrong programme. The
    // identity is asserted rather than parsed, exactly as the key loop below
    // and decode_substream both already do.
    if (impl_->config_.programme.has_value()) {
        const auto lead_bsid = stream_bsid(frames->front());
        if (!lead_bsid.has_value()) {
            return std::unexpected(lead_bsid.error());
        }
        if (*lead_bsid <= 8) {
            // §E2.3.1.2 assigns the core the identity (independent, 0), so it
            // is programme 0 and never a dependent.
            if (*impl_->config_.programme != 0) {
                return std::optional<DecodedAccessUnit>(std::nullopt);
            }
        } else {
            BitReader peek{frames->front()};
            const auto lead_bsi = parse_bsi(peek, frames->front().size());
            if (!lead_bsi.has_value()) {
                return std::unexpected(lead_bsi.error());
            }
            if (lead_bsi->substreamid != *impl_->config_.programme ||
                lead_bsi->strmtyp == eac3::StreamType::kDependent) {
                return std::optional<DecodedAccessUnit>(std::nullopt);
            }
        }
    }

    // §3.7: each frame's substream identity is needed below regardless of
    // whether decode_substream releases it or holds it back this call - a
    // held-back frame has no DecodedSubstream to read strmtyp/substreamid
    // from, so bsi is parsed here too. This is the same parse
    // decode_substream itself does a moment later; cheap enough that
    // duplicating it beats threading the key back out through decode_substream's
    // own return type.
    std::vector<int> keys;
    keys.reserve(frames->size());
    // Set when a dependent substream of this unit was dropped rather than
    // decoded - see the loop below and ConcealmentAction::kBedOnly.
    std::optional<DecodeError> bed_only;
    for (const auto& frame : *frames) {
        AC3_ZONE_BEGIN(key_zone, "eac3_au_key");
        // §E2.3.1.2 assigns an AC-3 bit stream present in an E-AC-3 bit stream
        // the identity (independent, 0) without it carrying either field -
        // parse_bsi would read strmtyp out of crc1 and substreamid out of the
        // rest of it, so the key is asserted here rather than parsed.
        const auto frame_bsid = stream_bsid(frame);
        if (!frame_bsid.has_value()) {
            return std::unexpected(frame_bsid.error());
        }
        // §E2.3.1.2's AC-3 core is always the independent substream, never a
        // dependent - it has no strmtyp field to say otherwise - so the
        // concealment fallback below only ever applies to a real Annex E
        // dependent frame.
        bool frame_is_dependent = false;
        if (*frame_bsid <= 8) {
            keys.push_back(static_cast<int>(StreamType::kIndependent) * 8);
        } else {
            BitReader peek{frame};
            const auto bsi = parse_bsi(peek, frame.size());
            if (!bsi.has_value()) {
                return std::unexpected(bsi.error());
            }
            keys.push_back(static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid);
            frame_is_dependent = bsi->strmtyp == StreamType::kDependent;
        }
        AC3_ZONE_END(key_zone);

        auto decoded = decode_substream(frame);
        if (!decoded) {
            // §7.10, the access-unit-level case. Concealment is tried at
            // the SUBSTREAM level first (decode_substream above), so a
            // dependent that has decoded before is reconstructed from its own
            // previous block and never reaches here - which keeps the
            // programme at its full rendered width, height layer included.
            // This is the fallback for the dependent that has nothing to be
            // reconstructed FROM: its first frame, or a new identity
            // appearing mid-stream. Dropping it beats failing the whole
            // access unit, because the bed is a self-sufficient rendering of
            // the same programme - just narrower than the stream promised.
            // The independent substream is a different matter: without it
            // there is no program at all.
            if (impl_->config_.concealment != ConcealmentPolicy::kNone && frame_is_dependent &&
                keys.size() > 1) {
                bed_only = decoded.error();
                keys.pop_back();
                continue;
            }
            return std::unexpected(decoded.error());
        }
        if (decoded->has_value()) {
            AC3_ZONE_SCOPED_N("eac3_au_queue");
            impl_->pending_au_parts_[static_cast<std::size_t>(keys.back())].push_back(
                std::move(**decoded));
        }
        // A held-back frame adds nothing to this identity's queue - whatever
        // it already holds (if anything, from an earlier call) is still
        // waiting in order, and remains what completes the assembly below
        // once every other identity also has one queued.
    }

    // Every identity this call's frames named must have at least one queued,
    // released result before there is a complete access unit to assemble. A
    // stream that never uses transient pre-noise processing always does:
    // every substream releases every call, so this is never false for it.
    for (const int key : keys) {
        if (impl_->pending_au_parts_[static_cast<std::size_t>(key)].empty()) {
            return std::optional<DecodedAccessUnit>(std::nullopt);
        }
    }
    AC3_ZONE_BEGIN(assemble_zone, "eac3_au_assemble");
    std::vector<DecodedSubstream> substreams;
    substreams.reserve(keys.size());
    for (const int key : keys) {
        auto& queue = impl_->pending_au_parts_[static_cast<std::size_t>(key)];
        substreams.push_back(std::move(queue.front()));
        queue.erase(queue.begin());
    }
    const auto& lead = substreams.front();
    if (lead.strmtyp == StreamType::kDependent) {
        return std::unexpected(DecodeError::kInvalidStream);
    }
    for (std::size_t i = 1; i < substreams.size(); ++i) {
        // Every substream of a program codes the same samples of the same
        // audio, so a dependent that disagrees with its parent about the rate
        // or the block count desynchronises the program silently rather than
        // failing to parse - which is exactly why it is checked here.
        const auto& sub = substreams[i];
        if (sub.strmtyp != StreamType::kDependent || sub.sample_rate != lead.sample_rate ||
            sub.numblkscod != lead.numblkscod) {
            return std::unexpected(DecodeError::kInvalidStream);
        }
    }

    DecodedAccessUnit out;
    out.sample_rate = lead.sample_rate;
    out.acmod = lead.acmod;
    out.dialnorm = lead.dialnorm;
    out.compr = lead.compr;
    out.dynrng = lead.dynrng;
    out.numblkscod = lead.numblkscod;
    out.mixing = lead.mixing;
    out.info = lead.info;
    // TS 103 420 §8.3.1's "whichever substream carries the EMDF container":
    // this project's own AtmosEncoder always makes that the bed, but a
    // dependent is equally legal and a legacy-core delivery has no choice -
    // §E2.3.1.2's AC-3 core cannot carry object audio at all (addbsi and the
    // block skip fields it rides in are Annex E syntax), so its objects are
    // in a dependent. Taking the first substream that has any keeps the bed's
    // own the winner wherever there is one, which is every stream this
    // project produces, so nothing about those changes.
    // Moved rather than copied: `substreams` is consumed by this function,
    // and a copy here was the object description - its vectors and the
    // object PCM behind them - duplicated once per frame for nothing.
    for (auto& sub : substreams) {
        if (sub.object_metadata.has_value()) {
            out.object_metadata = std::move(sub.object_metadata);
            out.object_audio = std::move(sub.object_audio);
            out.object_indices = std::move(sub.object_indices);
            break;
        }
    }
    // The independent substream's own substreamid: 0 for every
    // single-programme stream, and under a std::nullopt
    // DecoderConfig::programme the only thing distinguishing one programme's
    // units from another's.
    out.programme = lead.substreamid;
    out.substream_count = static_cast<int>(substreams.size());
    // §7.10: kBedOnly when a dependent was dropped above, otherwise whatever
    // the substreams themselves reported - a concealed BED is what the
    // program as a whole was concealed by, and it outranks a narrowed layout
    // because it says the audio itself was substituted rather than merely
    // that some of it is missing.
    if (bed_only.has_value()) {
        out.concealed = Concealment{.error = *bed_only, .action = ConcealmentAction::kBedOnly};
    }
    for (const auto& sub : substreams) {
        if (sub.concealed.has_value()) {
            out.concealed = sub.concealed;
            break;
        }
    }

    // The PCM target for one program slot: the caller's span when
    // decode_access_unit_into supplied them (every slot's samples are
    // copied in full below, so external storage needs no pre-clearing),
    // otherwise a vector allocated into the result exactly as before -
    // decode_frame_core's own split, at access-unit granularity.
    //
    // std::memcpy rather than std::copy, on purpose. The two ranges never
    // overlap - a substream's own vector against the caller's spans or the
    // result's vectors - and std::copy lowers to memmove, which on the
    // ESP32-S3 is a mask-ROM routine that measured some twelve cycles a byte:
    // 1.9 ms of a 5.1 frame went into this loop's 36 KB, against 0.12 ms for
    // the same bytes through the ROM's memcpy. On every other target the two
    // are the same call.
    const auto write_slot = [&](std::size_t slot, const std::vector<float>& src) {
        if (external.empty()) {
            out.channels[slot].resize(src.size());
        } else {
            assert(external.size() > slot);
            assert(external[slot].size() >= src.size());
        }
        if (src.empty()) {
            return;
        }
        float* const dst = external.empty() ? out.channels[slot].data() : external[slot].data();
        std::memcpy(dst, src.data(), src.size() * sizeof(float));
    };

    // The block form copies nothing. Each output slot becomes a view onto the
    // substream vector that supplies it - the same "a later dependent wins
    // the locations it shares" rule write_slot follows - the output stage
    // runs on those views in place, and the sink is handed the finished
    // programme a block at a time. A frame's worth of caller storage, and
    // the copy into it, both go.
    constexpr std::size_t kMaxSlots = 16;  // §E3.8.2's cap on a rendered programme
    const auto emit_blocks = [&](std::span<std::span<float>> views) {
        apply_output(out, views);
        // After the fold there are one or two slots; before it, every slot the
        // layout has - the three cases apply_output's own tail sorts by.
        const bool folded = impl_->config_.output.target != DownmixTarget::kAsCoded &&
                            lead.acmod != Acmod::kDualMono;
        const std::size_t slots =
            folded ? (impl_->config_.output.target == DownmixTarget::kMono ? 1U : 2U)
                   : views.size();
        const std::size_t samples = views.empty() ? 0 : views.front().size();
        const int blocks = static_cast<int>(samples / static_cast<std::size_t>(kSamplesPerBlock));
        std::array<std::span<const float>, kMaxSlots> block_views{};
        // The objects the same way: a view per JOC output onto the frame of
        // audio §6 reconstructed, cut to this block. object_audio has moved
        // into `out` by now (from whichever substream carried the container),
        // so these are views onto the unit's own vectors, alive until it is
        // returned. A unit reconstructs at most oba::joc::kMaxObjects outputs,
        // and a longer object_audio is a decoder fault this would rather
        // bound than overrun.
        constexpr auto kMaxObjectViews = static_cast<std::size_t>(oba::joc::kMaxObjects);
        std::array<std::span<const float>, kMaxObjectViews> object_views{};
        const std::size_t objects = std::min(out.object_audio.size(), kMaxObjectViews);
        const oba::DecodedProgram* const metadata =
            out.object_metadata.has_value() ? &*out.object_metadata : nullptr;
        // Its own zone, so the time a caller's sink spends in here reads as
        // the caller's rather than as the access unit's.
        AC3_ZONE_SCOPED_N("eac3_au_emit");
        for (int b = 0; b < blocks; ++b) {
            const auto offset =
                static_cast<std::size_t>(b) * static_cast<std::size_t>(kSamplesPerBlock);
            for (std::size_t s = 0; s < slots; ++s) {
                block_views[s] =
                    views[s].subspan(offset, static_cast<std::size_t>(kSamplesPerBlock));
            }
            std::size_t delivered = 0;
            for (std::size_t o = 0; o < objects; ++o) {
                const auto& audio = out.object_audio[o];
                if (audio.size() < offset + static_cast<std::size_t>(kSamplesPerBlock)) {
                    break;  // shorter than the frame: not this unit's objects
                }
                object_views[o] = std::span<const float>(audio).subspan(
                    offset, static_cast<std::size_t>(kSamplesPerBlock));
                ++delivered;
            }
            (*sink)(PcmBlock{.index = b,
                             .blocks = blocks,
                             .channels = std::span<const std::span<const float>>(block_views)
                                             .first(slots),
                             .objects = std::span<const std::span<const float>>(object_views)
                                            .first(delivered),
                             .object_indices = delivered > 0 ? std::span<const int>(out.object_indices)
                                                             : std::span<const int>{},
                             .object_metadata = delivered > 0 ? metadata : nullptr});
        }
    };

    // Dual mono has no Table E2.5 location - Ch1 and Ch2 are unrelated
    // programmes, not directions - and it has no bed/dependent split to make:
    // 1+1 is always this one lone independent substream. acmod_map() has a
    // placeholder L/R entry for it purely so channel-count bookkeeping
    // elsewhere still adds up; consulting
    // it here would mislabel Ch2 as a right channel, which is exactly the
    // "not a pair" distinction dual mono exists to preserve. So: pass the
    // substream's own two channels straight through in coded order, and leave
    // `layout` empty to say plainly that there is no spatial layout to report.
    if (lead.acmod == Acmod::kDualMono) {
        if (sink != nullptr) {
            auto& own = substreams.front();
            std::array<std::span<float>, kMaxSlots> views{};
            const std::size_t count = std::min(own.channels.size(), kMaxSlots);
            for (std::size_t ch = 0; ch < count; ++ch) {
                views[ch] = own.channels[ch];
            }
            AC3_ZONE_END(assemble_zone);
            emit_blocks(std::span<std::span<float>>(views).first(count));
            return std::optional<DecodedAccessUnit>(std::move(out));
        }
        if (external.empty()) {
            out.channels.resize(lead.channels.size());
        }
        for (std::size_t ch = 0; ch < lead.channels.size(); ++ch) {
            write_slot(ch, lead.channels[ch]);
        }
        AC3_ZONE_END(assemble_zone);
        apply_output(out, external);
        return std::optional<DecodedAccessUnit>(std::move(out));
    }

    // §E3.8.2: the bed's locations, then every dependent's unioned in. A
    // dependent's channels that correspond to the independent's REPLACE them;
    // the rest extend the layout.
    std::uint16_t occupied = 0;
    for (const auto& sub : substreams) {
        occupied = static_cast<std::uint16_t>(occupied | sub.location_map());
    }
    out.layout = eac3::chanmap::expand(occupied);
    // §E3.8.2 caps a single program at 16 rendered channels.
    if (out.layout.count > 16) {
        return std::unexpected(DecodeError::kInvalidStream);
    }
    // Everything above settled what the program IS - its layout, its
    // metadata, its object description. Everything below moves PCM into that
    // layout, and there is none to move when only the parse was asked for.
    // Returning here rather than letting the loop below run is not an
    // optimisation: that loop checks each substream's channel count against
    // its own location map, which an empty `channels` would fail.
    if (impl_->config_.skip_reconstruction) {
        return std::optional<DecodedAccessUnit>(std::move(out));
    }
    if (sink != nullptr) {
        std::array<std::span<float>, kMaxSlots> views{};
        for (auto& sub : substreams) {
            const auto locations = eac3::chanmap::expand(sub.location_map());
            if (static_cast<std::size_t>(locations.count) != sub.channels.size()) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            for (int i = 0; i < locations.count; ++i) {
                const int slot = out.layout.index_of(locations[i]);
                if (slot < 0 || static_cast<std::size_t>(slot) >= kMaxSlots) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                views[static_cast<std::size_t>(slot)] = sub.channels[static_cast<std::size_t>(i)];
            }
        }
        AC3_ZONE_END(assemble_zone);
        emit_blocks(std::span<std::span<float>>(views).first(
            static_cast<std::size_t>(out.layout.count)));
        return std::optional<DecodedAccessUnit>(std::move(out));
    }
    const std::size_t samples = lead.channels.empty() ? 0 : lead.channels.front().size();
    if (external.empty()) {
        out.channels.assign(static_cast<std::size_t>(out.layout.count),
                            std::vector<float>(samples, 0.0f));
    }

    // Transmission order is overwrite order, so a later dependent wins the
    // locations it shares with an earlier substream. Every slot of
    // out.layout comes from some substream's location_map() (the union
    // above), so every slot is written in full here - which is what lets
    // write_slot skip pre-clearing external storage.
    {
        AC3_ZONE_SCOPED_N("eac3_au_pcm");
        for (const auto& sub : substreams) {
            const auto locations = eac3::chanmap::expand(sub.location_map());
            if (static_cast<std::size_t>(locations.count) != sub.channels.size()) {
                return std::unexpected(DecodeError::kInvalidStream);
            }
            for (int i = 0; i < locations.count; ++i) {
                const int slot = out.layout.index_of(locations[i]);
                if (slot < 0) {
                    return std::unexpected(DecodeError::kInvalidStream);
                }
                write_slot(static_cast<std::size_t>(slot),
                           sub.channels[static_cast<std::size_t>(i)]);
            }
        }
    }
    AC3_ZONE_END(assemble_zone);
    apply_output(out, external);
    return std::optional<DecodedAccessUnit>(std::move(out));
}

}  // namespace ac3
