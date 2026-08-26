#include "ac3/core/tables.hpp"
#include "ac3/decoder/syntax_trace.hpp"
#include "ac3/decoder/decoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/aht_tables.hpp"
#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitreader.hpp"
#include "ac3/core/crc16.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/decoder/diagnostics.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/decoder/transient_prenoise.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/coupling.hpp"
#include "ac3/encoder/eac3_tools.hpp"
#include "ac3/internal/profile.hpp"
#include "ac3/internal/profiling.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/verify/eac3_mirror.hpp"
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
constexpr BitAllocCodes kBamode0Codes{.sdcycod = 2,
                                      .fdcycod = 1,
                                      .sgaincod = 1,
                                      .dbpbcod = 2,
                                      .floorcod = 7,
                                      .fgaincod = 4};

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
                    external.auxiliary = std::array<std::optional<int>, 2>{read_scale(),
                                                                          read_scale()};
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
        if (!rate) {
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
        const bool cpl_active =
            std::find(frm.cplinu.begin(), frm.cplinu.begin() + nblks, true) !=
            frm.cplinu.begin() + nblks;
        if (cpl_active) {
            const auto code = static_cast<int>(r.read(5));  // frmcplexpstr
            for (int blk = 0; blk < nblks; ++blk) {
                frm.cplexpstr[static_cast<std::size_t>(blk)] =
                    eac3::frame_exp_strategy(code, blk);
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
        const auto ncplblks =
            std::count(frm.cplinu.begin(), frm.cplinu.begin() + nblks, true);
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
    std::array<std::unique_ptr<std::array<std::array<double, 256>, 6>>, kSubstreamSlots>
        delay_;
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
    std::array<double, 512> imdct_scratch_{};
    std::array<double, 256> ecpl_spectrum_real_{};
    std::array<double, 256> ecpl_spectrum_imag_{};
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
    std::vector<std::array<std::array<double, 256>, kBlocksPerFrame>> aht_coeffs_;
    std::vector<std::array<double, 256>> ecpl_all_coeffs_;
    // One entry per block: everything decode_substream's second pass (spx
    // synthesis, rematrixing, IMDCT and PCM write) needs from pass one -
    // the .cpp's comment at the use site explains why two passes exist at
    // all. A member for the same churn reason as the buffers above: the
    // per-block geometry copies (chincpl, spxco, the enhanced-coupling
    // index sets...) land in vectors that keep their capacity across
    // frames, and `coeffs` cycles storage with the parse loop by swap
    // instead of forcing a fresh 14 KB allocation every block. The
    // enhanced-coupling fields are only assigned under cplinu &&
    // ecplinu_now and only read under the same guard - both flags ARE
    // re-assigned every block - so a reused entry's stale conditional
    // fields are never visible.
    struct BlockTail {
        std::vector<std::array<double, 256>> coeffs;  // per stream; decoupled where standard
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
        std::vector<std::vector<double>> spxco;
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
};

Eac3Decoder::Eac3Decoder() : impl_(std::make_unique<Impl>()) {}

Eac3Decoder::~Eac3Decoder() = default;
Eac3Decoder::Eac3Decoder(Eac3Decoder&&) noexcept = default;
Eac3Decoder& Eac3Decoder::operator=(Eac3Decoder&&) noexcept = default;

int Eac3Decoder::output_latency_samples() const { return impl_->output_.latency_samples(); }

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
    out.channels.assign(static_cast<std::size_t>(nchans),
                        std::vector<float>(static_cast<std::size_t>(nblks * kSamplesPerBlock),
                                           0.0f));

    auto& delay_slot = impl_->delay_[slot];
    if (!delay_slot) {
        delay_slot = std::make_unique<std::array<std::array<double, 256>, 6>>();
    }
    auto& delay = *delay_slot;

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
            for (int n = 0; n < kSamplesPerBlock; ++n) {
                const auto un = static_cast<std::size_t>(n);
                const double head = repeat ? last[un] * gain : 0.0;
                pcm[static_cast<std::size_t>(blk * kSamplesPerBlock + n)] =
                    static_cast<float>(2.0 * (head + history[un]));
                history[un] = repeat ? last[un + 256] * gain : 0.0;
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

    out.concealed = Concealment{.error = error,
                                .action = repeat ? ConcealmentAction::kRepeatFade
                                                 : ConcealmentAction::kMute};
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
        if (!core) {
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
    if (!bsi) {
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
            const int slot =
                stream == kCplStream ? kCouplingSyntaxStream : stream;
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
    if (bsi->chanmap && eac3::chanmap::channel_count(*bsi->chanmap) != nchans) {
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
        trace = &impl_->config_.eac3_trace->begin_substream(bsi->strmtyp == StreamType::kIndependent);
        trace->strmtyp = bsi->strmtyp;
        trace->substreamid = bsi->substreamid;
        trace->blocks_coded = nblks;
        trace->fbw_channels = nfchans;
        trace->coded_channels = nchans;
        trace->transproce = frm->transproce;
        if (frm->transproce) {
            const auto count = static_cast<std::size_t>(nfchans);
            trace->chintransproc.assign(frm->chintransproc.begin(),
                                        frm->chintransproc.begin() +
                                            static_cast<std::ptrdiff_t>(count));
            trace->transprocloc.assign(frm->transprocloc.begin(),
                                       frm->transprocloc.begin() +
                                           static_cast<std::ptrdiff_t>(count));
            trace->transproclen.assign(frm->transproclen.begin(),
                                       frm->transproclen.begin() +
                                           static_cast<std::ptrdiff_t>(count));
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
        out.channels.assign(static_cast<std::size_t>(nchans),
                            std::vector<float>(static_cast<std::size_t>(nblks * kSamplesPerBlock),
                                               0.0f));
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
    auto& delay_slot =
        impl_->delay_[static_cast<std::size_t>(static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid)];
    if (!delay_slot) {
        delay_slot = std::make_unique<std::array<std::array<double, 256>, 6>>();
    }
    auto& delay = *delay_slot;

    std::array<int, kMaxSubstreamStreams> endmant{};
    std::array<std::vector<std::uint8_t>, kMaxSubstreamStreams> exps;
    std::array<std::vector<std::uint8_t>, kMaxSubstreamStreams> bap;
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
    std::vector<bool> chincpl(static_cast<std::size_t>(nfchans), false);
    // Which coupling band each sub-band belongs to (cplbndstrc expansion).
    std::vector<int> subband_band;
    // The cplbndstrc[] merge flags themselves, indexed relative to this
    // block's cplbegf. Kept for the whole frame because a later block may
    // reuse them - see spx_structure_set below for the rule all three band
    // structures share.
    std::vector<bool> cpl_structure;
    // [channel][sub-band] - already expanded from bands to sub-bands.
    std::vector<std::vector<double>> cplco(static_cast<std::size_t>(nfchans));
    std::vector<bool> phsflg;

    // Spectral extension state (§3.6). Persists until re-transmitted, same as
    // coupling above - this encoder only ever (re)sends geometry in block 0.
    // Unlike coupling's per-sub-band coordinates, spx coordinates are one per
    // BAND already (no sub-band duplication step), so spx_bands/spxco are
    // indexed by band directly throughout.
    bool spxinu = false;
    std::vector<bool> chinspx(static_cast<std::size_t>(nfchans), false);
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
    std::vector<bool> firstspxcos(static_cast<std::size_t>(nfchans), true);
    std::vector<bool> firstcplcos(static_cast<std::size_t>(nfchans), true);
    bool firstcplleak = true;
    // [channel][band]
    std::vector<std::vector<double>> spxco(static_cast<std::size_t>(nfchans));
    std::vector<int> spxblnd(static_cast<std::size_t>(nfchans), 0);
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
    std::vector<std::vector<int>> ecplamp_raw(static_cast<std::size_t>(nfchans));
    std::vector<std::vector<int>> ecplangle_raw(static_cast<std::size_t>(nfchans));
    std::vector<std::vector<int>> ecplchaos_raw(static_cast<std::size_t>(nfchans));
    std::vector<bool> ecpltrans_persist(static_cast<std::size_t>(nfchans), false);
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
    // The per-block spectra, declared here so the swap at each block's
    // snapshot can cycle storage with the tails - see its assign() inside
    // the block loop for the zeroing contract. Pass one aliases it as
    // `coeffs` block-locally; pass two's own `coeffs` refers to each
    // tail's, so the two never share a scope.
    std::vector<std::array<double, 256>> parse_coeffs;

    // Captured alongside out.object_metadata below, from whichever block's
    // skip field carries the EMDF container - kept raw here (not parsed
    // yet) because oba::joc::parse_payload needs FrameParameters::objects to
    // agree with the OAMD program it rides beside, which is only known once
    // both payloads have been seen.
    std::vector<std::byte> joc_bytes;

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
        BlockSyntax* syntax =
            impl_->config_.syntax != nullptr ? &impl_->config_.syntax->blocks[static_cast<std::size_t>(blk)]
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
                            eac3::kDefaultSpxBandStructure[static_cast<std::size_t>(
                                begin_subbnd + i)];
                    }
                    spx_structure_set = true;
                }
                // else: a later block with the bit clear reuses what
                // spx_structure already holds, untouched.
                spx_bands = eac3::group_bands(spx_startmant, subband_count,
                                              eac3::kSpxBinsPerSubBand,
                                              std::span{spx_structure}.first(
                                                  static_cast<std::size_t>(subband_count)));
                for (auto& channel : spxco) {
                    channel.assign(static_cast<std::size_t>(spx_bands.count), 0.0);
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
                    co[static_cast<std::size_t>(bnd)] = coupling::decode_coordinate(
                        {.exp = exp, .mant = mant}, master, coupling::kSpxMantissaBits);
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
                            eac3::kDefaultCplBandStructure[static_cast<std::size_t>(cplbegf +
                                                                                    bnd)];
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
                    channel.assign(static_cast<std::size_t>(subband_count), 0.0);
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
                if (ecpl_end_subbnd <= ecpl_begin_subbnd ||
                    ecpl_end_subbnd > eac3::kEcplSubBands) {
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
                std::vector<double> band_values(static_cast<std::size_t>(ncplbnd));
                for (int bnd = 0; bnd < ncplbnd; ++bnd) {
                    const auto exp = static_cast<std::uint8_t>(r.read(4));
                    const auto mant = static_cast<std::uint8_t>(r.read(4));
                    band_values[static_cast<std::size_t>(bnd)] =
                        coupling::decode_coordinate({.exp = exp, .mant = mant}, master);
                }
                auto& channel = cplco[static_cast<std::size_t>(ch)];
                for (std::size_t bnd = 0; bnd < channel.size(); ++bnd) {
                    channel[bnd] = band_values[static_cast<std::size_t>(
                        subband_band[bnd])];
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
                const int nrematbd =
                    frm->cplinu[static_cast<std::size_t>(blk)]
                        ? (ecplinu_now
                               ? (ecplbegf == 0   ? 0
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
                std::vector<std::uint8_t> groups(static_cast<std::size_t>(ngrps));
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
                for (std::size_t bin = static_cast<std::size_t>(cplstrtmant);
                     bin < target.size(); ++bin) {
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
                std::vector<std::uint8_t> groups(static_cast<std::size_t>(ngrps));
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
                    fsnroffst[static_cast<std::size_t>(kCplStream)] =
                        static_cast<int>(r.read(4));
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
                    return std::unexpected(DecodeError::kInvalidStream);  // shall not reuse in block 0
                }
            }
            std::array<int, eac3::chanmap::kMaxSubstreamFullbw> chcodes{};
            for (int ch = 0; ch < nfchans; ++ch) {
                chcodes[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
                if (chcodes[static_cast<std::size_t>(ch)] == 3) {  // Table 5.16: reserved
                    return std::unexpected(DecodeError::kReservedValue);
                }
                if (blk == 0 && chcodes[static_cast<std::size_t>(ch)] == 0) {
                    return std::unexpected(DecodeError::kInvalidStream);  // shall not reuse in block 0
                }
            }
            if (cplinu_blk && cplcode == 1) {  // new info follows
                auto segs = parse_segments(bin_to_band(cplstrtmant));
                if (!segs) {
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
                    if (!segs) {
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
            std::vector<std::byte> skip_bytes;
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
            if (!out.object_metadata) {
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
            bap[s].assign(static_cast<std::size_t>(end), 0);
            compute_bit_allocation(exps[s], bsi->sample_rate, cpl_codes, csnroffst,
                                   fsnroffst[s], bap[s],
                                   {.start = cplstrtmant,
                                    .coupling = true,
                                    .cplfleak = cplfleak,
                                    .cplsleak = cplsleak,
                                    .snr_all_zero = snr_all_zero,
                                    .high_efficiency = frm->ahtinu[s],
                                    .delta = delta[static_cast<std::size_t>(kCplStream)]});
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
                bap[uch].assign(static_cast<std::size_t>(end), 0);
                // delta[ch] for ch == LFE's index is always {} (never written -
                // §5.4.3.49/E2.3.2.9 bound their deltbae[ch] loop by nfchans, so
                // the LFE channel has no delta bit allocation field at all).
                compute_bit_allocation(exps[uch], bsi->sample_rate, channel_codes, csnroffst,
                                       fsnroffst[uch], bap[uch],
                                       {.snr_all_zero = snr_all_zero,
                                        .high_efficiency = frm->ahtinu[uch],
                                        .delta = delta[uch]});
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
                    channel.cplco = cplco[uch];
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
                    channel.spxco = spxco[uch];
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
        // assign() re-zeroes exactly as the fresh vector did (uncoded bins
        // must read zero) into whatever storage the swap with this block's
        // tail handed back - see the swap at the snapshot below.
        auto& coeffs = parse_coeffs;
        coeffs.assign(static_cast<std::size_t>(kMaxSubstreamStreams), {});
        // §7.3.4, same split as decoder.cpp's own read_stream: only a stream
        // with its OWN dithflag (a full-bandwidth channel, s < nfchans)
        // dithers here. The LFE has no dithflag and always reconstructs as
        // zero; kCplStream's shared bins stay silent here too and are
        // dithered per receiving channel in the decoupling loop below
        // instead, per §7.3.4's "applied after the individual channels are
        // extracted ... uncorrelated" requirement.
        const auto read_stream = [&](int s, int begin) {
            const auto index = static_cast<std::size_t>(s);
            const bool dither_eligible = s < nfchans && dithflag[static_cast<std::size_t>(s)];
            for (int bin = begin; bin < endmant[index]; ++bin) {
                const int bap_value = bap[index][static_cast<std::size_t>(bin)];
                const int exp = exps[index][static_cast<std::size_t>(bin)];
                if (bap_value == 0) {
                    coeffs[index][static_cast<std::size_t>(bin)] =
                        dither_eligible ? impl_->dither_.next() / static_cast<double>(1u << exp) : 0.0;
                    continue;
                }
                const auto code = mantissa_reader.read(r, bap_value);
                coeffs[index][static_cast<std::size_t>(bin)] =
                    dequantize_mantissa(code, bap_value) / static_cast<double>(1u << exp);
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
            const auto& hebap = bap[us];

            const auto gaqmod = static_cast<int>(r.read(2));
            std::vector<int> gain(static_cast<std::size_t>(end), 1);  // default Gk=1
            std::vector<int> gain_carrying_bins;
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
                    const std::array<std::uint32_t, 3> mapped = {
                        packed / 9, (packed % 9) / 3, (packed % 9) % 3};
                    for (std::size_t t = 0;
                         t < 3 && i + t < gain_carrying_bins.size(); ++t) {
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
                std::array<double, kBlocksPerFrame> mantissas{};
                if (hb >= 1 && hb <= 7) {
                    const auto book = tables::aht_vq_table(hb);
                    const auto index = r.read(eac3::aht_bin_bits(hb));
                    if (index >= book.size()) {
                        return std::unexpected(DecodeError::kInvalidStream);
                    }
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        mantissas[j] = static_cast<double>(book[index][j]) / 32768.0;
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
                    const int small_bits =
                        g == 1 ? mantissa_bits : (g == 2 ? mantissa_bits - 1 : mantissa_bits - 2);
                    const int large_bits = g == 2 ? mantissa_bits - 1 : mantissa_bits;
                    for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                        const auto raw = r.read(small_bits);
                        bool has_escape = false;
                        std::uint32_t escape = 0;
                        // NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift)
                        if (g != 1 && raw == (1u << (small_bits - 1))) {
                            has_escape = true;
                            escape = r.read(large_bits);
                        }
                        mantissas[j] = eac3::aht_dequantize_mantissa(raw, escape, has_escape,
                                                                     mantissa_bits, g);
                    }
                }
                // hb == 0: mantissas stays all-zero.
                std::array<double, kBlocksPerFrame> blocks{};
                eac3::aht_inverse(mantissas, blocks);
                const int exp = exps[us][ubin];
                for (std::size_t j = 0; j < kBlocksPerFrame; ++j) {
                    aht_coeffs[us][j][ubin] = std::ldexp(blocks[j], -exp);
                }
            }
            return {};
        };
        const auto read_stream_dispatch = [&](int s, int begin) -> std::expected<void, DecodeError> {
            const auto us = static_cast<std::size_t>(s);
            if (frm->ahtinu[us]) {
                if (blk == 0) {
                    if (const auto result = decode_aht_stream(s, begin); !result) {
                        return result;
                    }
                }
                coeffs[us] = aht_coeffs[us][static_cast<std::size_t>(blk)];
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
                    if (!shared) {
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
            const auto& shared = coeffs[static_cast<std::size_t>(kCplStream)];
            const auto& cpl_bap = bap[static_cast<std::size_t>(kCplStream)];
            const auto& cpl_exps = exps[static_cast<std::size_t>(kCplStream)];
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& target = coeffs[static_cast<std::size_t>(ch)];
                const bool ch_dither = dithflag[static_cast<std::size_t>(ch)];
                for (int bnd = 0; bnd < static_cast<int>(subband_band.size()); ++bnd) {
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
                        // §7.3.4: independent per-channel dither for a
                        // zero-bap shared bin, run through the same
                        // extraction formula a real coupling coefficient
                        // uses - see decoder.cpp's own copy of this comment
                        // for why reusing one dithered coupling-domain
                        // sample across channels would be wrong.
                        const double coeff =
                            (cpl_bap[ubin] == 0 && ch_dither)
                                ? impl_->dither_.next() / static_cast<double>(1u << cpl_exps[ubin])
                                : shared[ubin];
                        target[ubin] = coeff * coordinate * 8.0 * sign;
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
            ecpl_all_coeffs[static_cast<std::size_t>(blk)] =
                coeffs[static_cast<std::size_t>(kCplStream)];
            ecpl_active[static_cast<std::size_t>(blk)] = true;
        }

        // Snapshot everything the second pass needs to finish this block.
        auto& tail = tails[static_cast<std::size_t>(blk)];
        // Swap, not move: the tail gets this block's spectra either way, but
        // coeffs gets the tail's previous-frame storage back, so the next
        // block's assign() above never has to allocate.
        tail.coeffs.swap(coeffs);
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
            // §3.5.5: reconstruct each coupled channel from the enhanced
            // coupling channel, using this block's neighbors. A neighbor is
            // zero when the adjacent block did not use enhanced coupling
            // (§3.5.5.1's own rule) - which includes this syncframe's first
            // and last block, whose true neighbor lives in an adjacent
            // syncframe this call was not given (see this function's
            // comment on `prev_ecpl_coeffs` above).
            static constexpr std::array<double, 256> kZero{};
            const auto& prev = (blk > 0 && ecpl_active[static_cast<std::size_t>(blk - 1)])
                                   ? ecpl_all_coeffs[static_cast<std::size_t>(blk - 1)]
                                   : kZero;
            const auto& next =
                (blk + 1 < nblks && ecpl_active[static_cast<std::size_t>(blk + 1)])
                    ? ecpl_all_coeffs[static_cast<std::size_t>(blk + 1)]
                    : kZero;
            auto& zr = impl_->ecpl_spectrum_real_;
            auto& zi = impl_->ecpl_spectrum_imag_;
            eac3::ecpl_channel_spectrum(prev, ecpl_all_coeffs[static_cast<std::size_t>(blk)],
                                        next, zr, zi, impl_->config_.fast_imdct);

            const int bins = tail.cplendmant - tail.cplstrtmant;
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!tail.chincpl[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                const auto uch = static_cast<std::size_t>(ch);
                const bool is_first = ch == tail.firstchincpl;
                std::vector<double> amp_bin(static_cast<std::size_t>(bins));
                std::vector<double> angle_bin(static_cast<std::size_t>(bins));
                eac3::ecpl_amplitudes(tail.ecplamp_raw[uch], tail.ecplchaos_raw[uch],
                                      tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                      tail.ecpl_end_subbnd, tail.ecpl_structure, amp_bin);
                eac3::ecpl_angles(ch, tail.ecplangle_raw[uch], tail.ecplchaos_raw[uch],
                                  tail.ecpltrans[uch], is_first, tail.ecpl_begin_subbnd,
                                  tail.ecpl_end_subbnd, tail.ecpl_structure, ecpl_noise,
                                  angle_bin, tail.ecplangleintrp);
                eac3::ecpl_channel_coefficients(zr, zi, amp_bin, angle_bin, tail.cplstrtmant,
                                                tail.cplendmant, coeffs[uch]);
            }
        }

        // §3.6.4 spectral extension synthesis: translate the low band up,
        // notch the seams, blend with noise to approximate the original
        // band's coarse energy, then scale by the transmitted coordinate.
        // Runs after decoupling, so a channel that is both coupled and
        // extended already has its coupling-restored content in place below
        // spx_startmant to copy from - coupling always ends exactly where
        // spx begins (§E3.3.1), so there is no gap and nothing to reconcile.
        if (tail.spxinu) {
            for (int ch = 0; ch < nfchans; ++ch) {
                if (!tail.chinspx[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                auto& tc = coeffs[static_cast<std::size_t>(ch)];

                // §3.6.4.1 Transform Coefficient Translation: copy low-band
                // coefficients up into the extension region, banded, wrapping
                // the copy source back to spx_copystart whenever a band would
                // run past spx_startmant. copyindex never leaves
                // [spx_copystart, spx_startmant) - strictly below the region
                // this loop writes into - so mutating tc in place is safe.
                std::array<bool, eac3::kMaxSubBands> wrapflag{};
                std::array<double, eac3::kMaxSubBands> band_rms{};
                int copyindex = tail.spx_copystart;
                for (int bnd = 0; bnd < tail.spx_bands.count; ++bnd) {
                    const auto ubnd = static_cast<std::size_t>(bnd);
                    const int size = tail.spx_bands.size[ubnd];
                    const int low = tail.spx_bands.start[ubnd];
                    if (copyindex + size > tail.spx_startmant) {
                        copyindex = tail.spx_copystart;
                        wrapflag[ubnd] = true;
                    }
                    double accum = 0.0;
                    for (int i = 0; i < size; ++i) {
                        if (copyindex == tail.spx_startmant) {
                            copyindex = tail.spx_copystart;
                        }
                        const double value = tc[static_cast<std::size_t>(copyindex++)];
                        tc[static_cast<std::size_t>(low + i)] = value;
                        accum += value * value;
                    }
                    band_rms[ubnd] = std::sqrt(accum / size);
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
                    const double nratio =
                        eac3::spx_noise_ratio(low, size, tail.spx_endmant, blend);
                    const double nscale = band_rms[ubnd] * std::sqrt(nratio);
                    const double sscale = std::sqrt(1.0 - nratio);
                    const double coordinate = tail.spxco[static_cast<std::size_t>(ch)][ubnd] * 32.0;
                    for (int i = 0; i < size; ++i) {
                        const auto at = static_cast<std::size_t>(low + i);
                        tc[at] = (tc[at] * sscale + spx_noise.next() * nscale) * coordinate;
                    }
                }
            }
        }

        if (bsi->acmod == Acmod::k2_0) {
            // §7.5.4: L = L' + R', R = L' - R' in flagged bands, up to the
            // lower bandwidth of the two channels.
            const int cap = std::min(tail.endmant[0], tail.endmant[1]) - 1;
            for (std::size_t band = 0; band < kRematrixBands.size(); ++band) {
                if (!tail.rematflg[band]) {
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
                    ? internal::block_gain(impl_->config_, out.dynrng2[static_cast<std::size_t>(blk)],
                                           out.compr2)
                    : internal::block_gain(impl_->config_, out.dynrng[static_cast<std::size_t>(blk)],
                                           out.compr);
            if (drc != 1.0) {
                for (auto& value : coeffs[static_cast<std::size_t>(ch)]) {
                    value *= drc;
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
                if (ch < nfchans && tail.blksw[static_cast<std::size_t>(ch)]) {
                    imdct256_pair_windowed(coeffs[index], x, impl_->config_.fast_imdct);
                } else {
                    imdct512_windowed(coeffs[index], x, impl_->config_.fast_imdct);
                }
                auto& history = delay[index];
                auto& pcm = out.channels[index];
                for (int n = 0; n < kSamplesPerBlock; ++n) {
                    pcm[static_cast<std::size_t>(blk * kSamplesPerBlock + n)] =
                        static_cast<float>(2.0 * (x[static_cast<std::size_t>(n)] +
                                                  history[static_cast<std::size_t>(n)]));
                    history[static_cast<std::size_t>(n)] = x[static_cast<std::size_t>(256 + n)];
                }
                // §7.10's raw material, captured into scratch rather than
                // straight into impl_->retained_: this frame may still be refused
                // further down, and a refused frame must not become what the
                // NEXT loss is reconstructed from.
                if (retain_last_block && blk == nblks - 1 && index < impl_->conceal_scratch_.size()) {
                    std::copy(x.begin(), x.end(), impl_->conceal_scratch_[index].begin());
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
        if (out.object_metadata && !joc_bytes.empty()) {
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
                    out.object_audio = oba::joc::reconstruct(bed_joc_order, *params, *joc_slot,
                                                        impl_->config_.fast_mdct,
                                                        impl_->config_.fast_imdct,
                                                        impl_->config_.joc_domain);
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
        for (int ch = 0; ch < nchans && static_cast<std::size_t>(ch) < impl_->conceal_scratch_.size();
             ++ch) {
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
        *pending_slot = std::move(out);
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

// §5.4.2.8/§7.8 over an assembled program, in whichever storage it landed -
// the result's own vectors, or the caller's spans when decode_access_unit_into
// supplied them. A no-op unless DecoderConfig::output asks for something, and
// the one place the fold happens for the access-unit forms.
void Eac3Decoder::apply_output(DecodedAccessUnit& out,
                               std::span<const std::span<float>> external) {
    if (impl_->config_.output.target == DownmixTarget::kAsCoded &&
        impl_->config_.output.mode == OperatingMode::kCustom && !impl_->config_.output.apply_dialnorm) {
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
    const auto slots = out.channels.empty() ? static_cast<std::size_t>(out.layout.count)
                                            : out.channels.size();
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
    impl_->output_.apply(impl_->au_views_, out.layout, out.acmod, rendered_lfe, mix_levels(out.mixing),
                  out.dialnorm);
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
    std::span<const std::byte> unit, std::span<const std::span<float>> external) {
    AC3_ZONE_SCOPED_N("eac3_decode_access_unit");
    const auto frames = split_frames(unit);
    if (!frames) {
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
    // programme id.
    if (impl_->config_.programme) {
        BitReader peek{frames->front()};
        const auto lead_bsi = parse_bsi(peek, frames->front().size());
        if (!lead_bsi) {
            return std::unexpected(lead_bsi.error());
        }
        if (lead_bsi->substreamid != *impl_->config_.programme ||
            lead_bsi->strmtyp == eac3::StreamType::kDependent) {
            return std::optional<DecodedAccessUnit>(std::nullopt);
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
        // §E2.3.1.2 assigns an AC-3 bit stream present in an E-AC-3 bit stream
        // the identity (independent, 0) without it carrying either field -
        // parse_bsi would read strmtyp out of crc1 and substreamid out of the
        // rest of it, so the key is asserted here rather than parsed.
        const auto frame_bsid = stream_bsid(frame);
        if (!frame_bsid) {
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
            if (!bsi) {
                return std::unexpected(bsi.error());
            }
            keys.push_back(static_cast<int>(bsi->strmtyp) * 8 + bsi->substreamid);
            frame_is_dependent = bsi->strmtyp == StreamType::kDependent;
        }

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
    for (const auto& sub : substreams) {
        if (sub.object_metadata) {
            out.object_metadata = sub.object_metadata;
            out.object_audio = sub.object_audio;
            out.object_indices = sub.object_indices;
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
    if (bed_only) {
        out.concealed = Concealment{.error = *bed_only, .action = ConcealmentAction::kBedOnly};
    }
    for (const auto& sub : substreams) {
        if (sub.concealed) {
            out.concealed = sub.concealed;
            break;
        }
    }

    // The PCM target for one program slot: the caller's span when
    // decode_access_unit_into supplied them (every slot's samples are
    // copied in full below, so external storage needs no pre-clearing),
    // otherwise a vector allocated into the result exactly as before -
    // decode_frame_core's own split, at access-unit granularity.
    const auto write_slot = [&](std::size_t slot, const std::vector<float>& src) {
        if (external.empty()) {
            out.channels[slot] = src;
            return;
        }
        assert(external.size() > slot);
        assert(external[slot].size() >= src.size());
        std::copy(src.begin(), src.end(), external[slot].begin());
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
        if (external.empty()) {
            out.channels.resize(lead.channels.size());
        }
        for (std::size_t ch = 0; ch < lead.channels.size(); ++ch) {
            write_slot(ch, lead.channels[ch]);
        }
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
            write_slot(static_cast<std::size_t>(slot), sub.channels[static_cast<std::size_t>(i)]);
        }
    }
    apply_output(out, external);
    return std::optional<DecodedAccessUnit>(std::move(out));
}

}  // namespace ac3
