#include "ac3/verify/eac3_mirror.hpp"

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ac3::verify {

namespace {

// A coded stream's name in Annex E's terms rather than by its internal index.
// The numbering is the encoder's - fbw channels, then the LFE, then the
// coupling channel - which the decoder's trace-filling side maps onto, so a
// name can be derived from the substream's shape alone.
std::string stream_name(int stream, int fbw_channels, int coded_channels) {
    if (stream < 0) {
        return {};
    }
    if (stream < fbw_channels) {
        return "channel " + std::to_string(stream);
    }
    if (stream < coded_channels) {
        return "LFE";
    }
    return "coupling";
}

// Every value in an Eac3Mismatch travels as a double so the coordinates fit,
// but almost all of them are integers and reading "5.000000" where the field
// is a bap helps nobody.
std::string value_text(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        return std::to_string(static_cast<long long>(value));
    }
    return std::to_string(value);
}

struct Push {
    std::vector<Eac3Mismatch>& out;
    std::uint64_t frame;
    int substream;
    int block;

    void operator()(Eac3Field field, int stream, bool channel, int index, double encoder,
                    double decoder) const {
        out.push_back({.frame = frame,
                       .substream = substream,
                       .block = block,
                       .stream = stream,
                       .channel = channel,
                       .index = index,
                       .field = field,
                       .encoder = encoder,
                       .decoder = decoder});
    }
};

// Appends at most kEac3MaxPerArray mismatches between two same-length arrays,
// so one desynced allocation cannot bury the block and stream identity under a
// few hundred identical-looking lines.
template <typename T>
void diff_array(const Push& push, std::span<const T> encoder, std::span<const T> decoder,
                Eac3Field field, int stream, bool channel) {
    int reported = 0;
    for (std::size_t i = 0; i < encoder.size() && reported < kEac3MaxPerArray; ++i) {
        if (encoder[i] == decoder[i]) {
            continue;
        }
        push(field, stream, channel, static_cast<int>(i), static_cast<double>(encoder[i]),
             static_cast<double>(decoder[i]));
        ++reported;
    }
}

// Sizes first, then contents: two arrays of different lengths have nothing
// element-wise to say, and the length IS the finding.
template <typename T>
void diff_sized_array(const Push& push, const std::vector<T>& encoder,
                      const std::vector<T>& decoder, Eac3Field count_field, Eac3Field field,
                      int stream, bool channel) {
    if (encoder.size() != decoder.size()) {
        push(count_field, stream, channel, -1, static_cast<double>(encoder.size()),
             static_cast<double>(decoder.size()));
        return;
    }
    diff_array<T>(push, encoder, decoder, field, stream, channel);
}

void diff_delta(const Push& push, const DeltaSegments& encoder, const DeltaSegments& decoder,
                int stream) {
    if (encoder.deltnseg != decoder.deltnseg) {
        push(Eac3Field::kDeltaSegmentCount, stream, false, -1, encoder.deltnseg,
             decoder.deltnseg);
        // The per-segment comparison below would be comparing entries one side
        // never filled, so stop here: the counts differing IS the finding.
        return;
    }
    for (int seg = 0; seg < encoder.deltnseg; ++seg) {
        const auto i = static_cast<std::size_t>(seg);
        if (encoder.deltoffst[i] != decoder.deltoffst[i]) {
            push(Eac3Field::kDeltaOffset, stream, false, seg, encoder.deltoffst[i],
                 decoder.deltoffst[i]);
        }
        if (encoder.deltlen[i] != decoder.deltlen[i]) {
            push(Eac3Field::kDeltaLength, stream, false, seg, encoder.deltlen[i],
                 decoder.deltlen[i]);
        }
        if (encoder.deltba[i] != decoder.deltba[i]) {
            push(Eac3Field::kDeltaValue, stream, false, seg, encoder.deltba[i],
                 decoder.deltba[i]);
        }
    }
}

void diff_stream(const Push& push, const Eac3StreamTrace& e, const Eac3StreamTrace& d,
                 int stream) {
    // Delta before exponents and bap: it is upstream of the allocation, so
    // where it is the cause the other two are consequences, and a reader
    // should meet the cause first.
    diff_delta(push, e.delta, d.delta, stream);
    if (e.start != d.start) {
        push(Eac3Field::kStreamStart, stream, false, -1, e.start, d.start);
    }
    if (e.endmant != d.endmant) {
        push(Eac3Field::kStreamEnd, stream, false, -1, e.endmant, d.endmant);
    }
    if (e.aht != d.aht) {
        push(Eac3Field::kAhtInUse, stream, false, -1, e.aht ? 1 : 0, d.aht ? 1 : 0);
        // hebap and bap are different quantities read out of different
        // tables, so comparing them against each other says nothing.
        return;
    }
    if (e.aht && e.gaqmod != d.gaqmod) {
        push(Eac3Field::kGaqMode, stream, false, -1, e.gaqmod, d.gaqmod);
    }
    diff_sized_array<std::uint8_t>(push, e.exponents, d.exponents, Eac3Field::kExponentCount,
                                   Eac3Field::kExponent, stream, false);
    diff_sized_array<std::uint8_t>(push, e.bap, d.bap, Eac3Field::kBapCount, Eac3Field::kBap,
                                   stream, false);
    diff_sized_array<std::uint8_t>(push, e.gain, d.gain, Eac3Field::kAhtGainCount,
                                   Eac3Field::kAhtGain, stream, false);
}

void diff_channel(const Push& push, const Eac3ChannelTrace& e, const Eac3ChannelTrace& d,
                  int channel) {
    if (e.blksw != d.blksw) {
        push(Eac3Field::kBlockSwitch, channel, true, -1, e.blksw ? 1 : 0, d.blksw ? 1 : 0);
    }
    if (e.in_coupling != d.in_coupling) {
        push(Eac3Field::kChannelInCoupling, channel, true, -1, e.in_coupling ? 1 : 0,
             d.in_coupling ? 1 : 0);
    } else if (e.in_coupling) {
        diff_sized_array<double>(push, e.cplco, d.cplco, Eac3Field::kCouplingCoordinateCount,
                                 Eac3Field::kCouplingCoordinate, channel, true);
        if (e.ecpltrans != d.ecpltrans) {
            push(Eac3Field::kEcplTransient, channel, true, -1, e.ecpltrans ? 1 : 0,
                 d.ecpltrans ? 1 : 0);
        }
        diff_sized_array<int>(push, e.ecplamp, d.ecplamp, Eac3Field::kEcplCoordinateCount,
                              Eac3Field::kEcplAmplitude, channel, true);
        diff_sized_array<int>(push, e.ecplangle, d.ecplangle, Eac3Field::kEcplCoordinateCount,
                              Eac3Field::kEcplAngle, channel, true);
        diff_sized_array<int>(push, e.ecplchaos, d.ecplchaos, Eac3Field::kEcplCoordinateCount,
                              Eac3Field::kEcplChaos, channel, true);
    }
    if (e.in_spx != d.in_spx) {
        push(Eac3Field::kChannelInSpx, channel, true, -1, e.in_spx ? 1 : 0, d.in_spx ? 1 : 0);
    } else if (e.in_spx) {
        if (e.spxblnd != d.spxblnd) {
            push(Eac3Field::kSpxBlend, channel, true, -1, e.spxblnd, d.spxblnd);
        }
        diff_sized_array<double>(push, e.spxco, d.spxco, Eac3Field::kSpxCoordinateCount,
                                 Eac3Field::kSpxCoordinate, channel, true);
    }
}

// The frame-level state audfrm hoists out of the blocks. Compared before any
// block, because a disagreement here shifts every block that follows.
void diff_frame_level(const Push& push, const Eac3SubstreamTrace& e,
                      const Eac3SubstreamTrace& d) {
    if (e.strmtyp != d.strmtyp) {
        push(Eac3Field::kStreamType, -1, false, -1, static_cast<int>(e.strmtyp),
             static_cast<int>(d.strmtyp));
    }
    if (e.substreamid != d.substreamid) {
        push(Eac3Field::kSubstreamId, -1, false, -1, e.substreamid, d.substreamid);
    }
    if (e.blocks_coded != d.blocks_coded) {
        push(Eac3Field::kBlockCount, -1, false, -1, e.blocks_coded, d.blocks_coded);
    }
    if (e.transproce != d.transproce) {
        push(Eac3Field::kTransientProcInUse, -1, false, -1, e.transproce ? 1 : 0,
             d.transproce ? 1 : 0);
        return;
    }
    if (!e.transproce || e.chintransproc.size() != d.chintransproc.size()) {
        return;
    }
    for (std::size_t ch = 0; ch < e.chintransproc.size(); ++ch) {
        const auto at = static_cast<int>(ch);
        if (e.chintransproc[ch] != d.chintransproc[ch]) {
            push(Eac3Field::kTransientProcChannel, -1, false, at, e.chintransproc[ch] ? 1 : 0,
                 d.chintransproc[ch] ? 1 : 0);
            continue;
        }
        if (!e.chintransproc[ch]) {
            continue;
        }
        if (e.transprocloc[ch] != d.transprocloc[ch]) {
            push(Eac3Field::kTransientProcLocation, -1, false, at, e.transprocloc[ch],
                 d.transprocloc[ch]);
        }
        if (e.transproclen[ch] != d.transproclen[ch]) {
            push(Eac3Field::kTransientProcLength, -1, false, at, e.transproclen[ch],
                 d.transproclen[ch]);
        }
    }
}

}  // namespace

void Eac3SubstreamTrace::reset() {
    strmtyp = eac3::StreamType::kIndependent;
    substreamid = 0;
    blocks_coded = kBlocksPerFrame;
    fbw_channels = 0;
    coded_channels = 0;
    transproce = false;
    // clear(), not fresh vectors: a caller encoding a whole file reuses one
    // trace per frame, and these vectors are the only allocation this
    // facility makes at all.
    chintransproc.clear();
    transprocloc.clear();
    transproclen.clear();
    for (auto& block : blocks) {
        block.entered = false;
        block.allocated = false;
        block.bit_offset = 0;
        block.deltbaie = false;
        block.cplinu = false;
        block.ecplinu = false;
        block.cplstrtmant = 0;
        block.cplendmant = 0;
        block.spxinu = false;
        block.spx_startmant = 0;
        block.spx_endmant = 0;
        block.spx_copystart = 0;
        block.streams.clear();
        block.channels.clear();
    }
}

std::span<const Eac3SubstreamTrace> Eac3AccessUnitTrace::substreams() const {
    return std::span{storage_}.first(used_);
}

void Eac3AccessUnitTrace::resize(std::size_t count) {
    if (storage_.size() < count) {
        storage_.resize(count);
    }
    used_ = count;
    for (std::size_t i = 0; i < used_; ++i) {
        storage_[i].reset();
    }
}

Eac3SubstreamTrace& Eac3AccessUnitTrace::substream(std::size_t index) {
    return storage_[index];
}

Eac3SubstreamTrace& Eac3AccessUnitTrace::begin_substream(bool independent) {
    if (independent) {
        used_ = 0;
    }
    if (storage_.size() <= used_) {
        storage_.emplace_back();
    }
    auto& slot = storage_[used_];
    ++used_;
    // A reused slot keeps its vectors' storage, which is the whole reason
    // storage_ is never shrunk; reset() puts it back to the unvisited state
    // a freshly emplaced one is already in, so both routes agree.
    slot.reset();
    return slot;
}

std::string_view describe(Eac3Field field) {
    switch (field) {
        case Eac3Field::kSubstreamCount:
            return "substreams in the access unit";
        case Eac3Field::kStreamType:
            return "strmtyp";
        case Eac3Field::kSubstreamId:
            return "substreamid";
        case Eac3Field::kBlockCount:
            return "blocks per syncframe";
        case Eac3Field::kTransientProcInUse:
            return "transproce";
        case Eac3Field::kTransientProcChannel:
            return "chintransproc";
        case Eac3Field::kTransientProcLocation:
            return "transprocloc";
        case Eac3Field::kTransientProcLength:
            return "transproclen";
        case Eac3Field::kBlockReached:
            return "block reached";
        case Eac3Field::kBitOffset:
            return "bit offset at block start";
        case Eac3Field::kAllocationReached:
            return "bit allocation computed";
        case Eac3Field::kStreamCount:
            return "coded stream count";
        case Eac3Field::kChannelCount:
            return "full-bandwidth channel count";
        case Eac3Field::kDeltbaie:
            return "deltbaie";
        case Eac3Field::kCouplingInUse:
            return "cplinu";
        case Eac3Field::kEnhancedCouplingInUse:
            return "ecplinu";
        case Eac3Field::kCouplingStart:
            return "cplstrtmant";
        case Eac3Field::kCouplingEnd:
            return "cplendmant";
        case Eac3Field::kSpxInUse:
            return "spxinu";
        case Eac3Field::kSpxStart:
            return "spx start bin";
        case Eac3Field::kSpxEnd:
            return "spx end bin";
        case Eac3Field::kSpxCopyStart:
            return "spx copy-source start bin";
        case Eac3Field::kStreamStart:
            return "stream start bin";
        case Eac3Field::kStreamEnd:
            return "endmant";
        case Eac3Field::kExponentCount:
            return "exponent count";
        case Eac3Field::kExponent:
            return "exponent";
        case Eac3Field::kBapCount:
            return "bap count";
        case Eac3Field::kBap:
            return "bap";
        case Eac3Field::kDeltaSegmentCount:
            return "deltnseg";
        case Eac3Field::kDeltaOffset:
            return "deltoffst";
        case Eac3Field::kDeltaLength:
            return "deltlen";
        case Eac3Field::kDeltaValue:
            return "deltba";
        case Eac3Field::kAhtInUse:
            return "ahtinu";
        case Eac3Field::kGaqMode:
            return "chgaqmod";
        case Eac3Field::kAhtGainCount:
            return "AHT gain count";
        case Eac3Field::kAhtGain:
            return "AHT gain";
        case Eac3Field::kBlockSwitch:
            return "blksw";
        case Eac3Field::kChannelInCoupling:
            return "chincpl";
        case Eac3Field::kCouplingCoordinateCount:
            return "coupling coordinate count";
        case Eac3Field::kCouplingCoordinate:
            return "cplco";
        case Eac3Field::kEcplAmplitude:
            return "ecplamp";
        case Eac3Field::kEcplAngle:
            return "ecplangle";
        case Eac3Field::kEcplChaos:
            return "ecplchaos";
        case Eac3Field::kEcplTransient:
            return "ecpltrans";
        case Eac3Field::kEcplCoordinateCount:
            return "enhanced coupling coordinate count";
        case Eac3Field::kChannelInSpx:
            return "chinspx";
        case Eac3Field::kSpxCoordinateCount:
            return "spx coordinate count";
        case Eac3Field::kSpxCoordinate:
            return "spxco";
        case Eac3Field::kSpxBlend:
            return "spxblnd";
    }
    return "unknown field";
}

std::string describe(const Eac3Mismatch& mismatch, int fbw_channels, int coded_channels) {
    std::string out = "frame " + std::to_string(mismatch.frame);
    if (mismatch.substream >= 0) {
        out += " substream " + std::to_string(mismatch.substream);
    }
    if (mismatch.block >= 0) {
        out += " block " + std::to_string(mismatch.block);
    }
    if (mismatch.stream >= 0) {
        out += " ";
        out += mismatch.channel ? "channel " + std::to_string(mismatch.stream)
                                : stream_name(mismatch.stream, fbw_channels, coded_channels);
    }
    out += ": ";
    out += describe(mismatch.field);
    if (mismatch.index >= 0) {
        out += "[" + std::to_string(mismatch.index) + "]";
    }
    out += " encoder=" + value_text(mismatch.encoder);
    out += " decoder=" + value_text(mismatch.decoder);
    return out;
}

std::vector<Eac3Mismatch> compare(const Eac3SubstreamTrace& encoder,
                                  const Eac3SubstreamTrace& decoder, std::uint64_t frame_index,
                                  int substream_index) {
    std::vector<Eac3Mismatch> out;
    diff_frame_level({.out = out,
                      .frame = frame_index,
                      .substream = substream_index,
                      .block = -1},
                     encoder, decoder);
    if (!out.empty()) {
        // audfrm sizes and gates the blocks; comparing them against a
        // different frame-level reading reports the same finding once per
        // block instead of once.
        return out;
    }

    for (int block = 0; block < kBlocksPerFrame; ++block) {
        const auto& e = encoder.blocks[static_cast<std::size_t>(block)];
        const auto& d = decoder.blocks[static_cast<std::size_t>(block)];
        const Push push{
            .out = out, .frame = frame_index, .substream = substream_index, .block = block};
        if (!e.entered && !d.entered) {
            break;  // both sides are done; nothing beyond here was coded
        }
        if (e.entered != d.entered) {
            push(Eac3Field::kBlockReached, -1, false, -1, e.entered ? 1 : 0, d.entered ? 1 : 0);
            break;
        }

        // The localiser first: any disagreement about ANY field's width, from
        // any cause, lands here at the next block boundary.
        if (e.bit_offset != d.bit_offset) {
            push(Eac3Field::kBitOffset, -1, false, -1, static_cast<double>(e.bit_offset),
                 static_cast<double>(d.bit_offset));
        }
        if (e.deltbaie != d.deltbaie) {
            push(Eac3Field::kDeltbaie, -1, false, -1, e.deltbaie ? 1 : 0, d.deltbaie ? 1 : 0);
        }
        // The tool geometry next: it is what decides how many streams there
        // are and where each one's coded region sits, so it is upstream of
        // everything the per-stream comparison looks at.
        if (e.cplinu != d.cplinu) {
            push(Eac3Field::kCouplingInUse, -1, false, -1, e.cplinu ? 1 : 0, d.cplinu ? 1 : 0);
        } else if (e.cplinu) {
            if (e.ecplinu != d.ecplinu) {
                push(Eac3Field::kEnhancedCouplingInUse, -1, false, -1, e.ecplinu ? 1 : 0,
                     d.ecplinu ? 1 : 0);
            }
            if (e.cplstrtmant != d.cplstrtmant) {
                push(Eac3Field::kCouplingStart, -1, false, -1, e.cplstrtmant, d.cplstrtmant);
            }
            if (e.cplendmant != d.cplendmant) {
                push(Eac3Field::kCouplingEnd, -1, false, -1, e.cplendmant, d.cplendmant);
            }
        }
        if (e.spxinu != d.spxinu) {
            push(Eac3Field::kSpxInUse, -1, false, -1, e.spxinu ? 1 : 0, d.spxinu ? 1 : 0);
        } else if (e.spxinu) {
            if (e.spx_startmant != d.spx_startmant) {
                push(Eac3Field::kSpxStart, -1, false, -1, e.spx_startmant, d.spx_startmant);
            }
            if (e.spx_endmant != d.spx_endmant) {
                push(Eac3Field::kSpxEnd, -1, false, -1, e.spx_endmant, d.spx_endmant);
            }
            if (e.spx_copystart != d.spx_copystart) {
                push(Eac3Field::kSpxCopyStart, -1, false, -1, e.spx_copystart, d.spx_copystart);
            }
        }

        if (e.allocated != d.allocated) {
            push(Eac3Field::kAllocationReached, -1, false, -1, e.allocated ? 1 : 0,
                 d.allocated ? 1 : 0);
        } else if (e.allocated) {
            if (e.streams.size() != d.streams.size()) {
                push(Eac3Field::kStreamCount, -1, false, -1,
                     static_cast<double>(e.streams.size()),
                     static_cast<double>(d.streams.size()));
            } else {
                for (std::size_t s = 0; s < e.streams.size(); ++s) {
                    diff_stream(push, e.streams[s], d.streams[s], static_cast<int>(s));
                }
            }
            if (e.channels.size() != d.channels.size()) {
                push(Eac3Field::kChannelCount, -1, false, -1,
                     static_cast<double>(e.channels.size()),
                     static_cast<double>(d.channels.size()));
            } else {
                for (std::size_t c = 0; c < e.channels.size(); ++c) {
                    diff_channel(push, e.channels[c], d.channels[c], static_cast<int>(c));
                }
            }
        }

        if (!out.empty()) {
            // Everything after the first divergent block is a consequence of
            // it - see the header's note on why that is not reported.
            break;
        }
    }
    return out;
}

std::vector<Eac3Mismatch> compare(const Eac3AccessUnitTrace& encoder,
                                  const Eac3AccessUnitTrace& decoder,
                                  std::uint64_t frame_index) {
    const auto encoded = encoder.substreams();
    const auto decoded = decoder.substreams();
    if (encoded.size() != decoded.size()) {
        return {Eac3Mismatch{.frame = frame_index,
                             .field = Eac3Field::kSubstreamCount,
                             .encoder = static_cast<double>(encoded.size()),
                             .decoder = static_cast<double>(decoded.size())}};
    }
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        auto found = compare(encoded[i], decoded[i], frame_index, static_cast<int>(i));
        if (!found.empty()) {
            // A dependent substream is decoded with the bed already read, so
            // a later substream's divergence is as likely to be a
            // consequence of an earlier one as a block's is of the block
            // before it. Same rule, one level up.
            return found;
        }
    }
    return {};
}

std::string report(std::span<const Eac3Mismatch> mismatches, const Eac3AccessUnitTrace& shape) {
    std::string out;
    for (const auto& mismatch : mismatches) {
        if (!out.empty()) {
            out += "\n";
        }
        int fbw = 0;
        int coded = 0;
        const auto substreams = shape.substreams();
        if (mismatch.substream >= 0 &&
            static_cast<std::size_t>(mismatch.substream) < substreams.size()) {
            const auto& sub = substreams[static_cast<std::size_t>(mismatch.substream)];
            fbw = sub.fbw_channels;
            coded = sub.coded_channels;
        }
        out += describe(mismatch, fbw, coded);
    }
    return out;
}

}  // namespace ac3::verify
