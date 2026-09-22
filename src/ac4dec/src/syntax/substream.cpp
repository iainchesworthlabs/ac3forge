#include "syntax/substream.hpp"

#include <cstddef>

namespace ac4::detail {

ParseResult parse_audio_substream(BitReader& r, const SubstreamContext& ctx, AudioSubstreamState& state,
                                  AudioSubstream& out, BitReader* hsf_reader) {
    out = AudioSubstream{};

    // Part 2 6.2.2.2. The header is always a whole number of bytes: 16 bits,
    // plus 8 for each group of variable_bits(7).
    std::uint64_t audio_size = r.read(15, "audio_size_value");
    if (r.read_flag("b_more_bits")) {
        audio_size += std::uint64_t{r.variable_bits(7, "audio_size_value")} << 15U;
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    if (ctx.sf_multiplier && hsf_reader == nullptr) {
        // The core ASF syntax does not depend on sample rate (its tables are
        // keyed by transform length in samples, not Hz - only the HSF
        // extension's own tables are per-rate), so a 96/192 kHz substream is
        // refused only where its HSF extension substream could not be
        // resolved to read alongside it (decoder.cpp), not for being 96 or
        // 192 kHz as such.
        return fail(DecodeError::kUnsupported,
                    "a 96 kHz or 192 kHz substream whose HSF extension substream could not be read");
    }
    const std::size_t audio_start = r.position();
    const std::size_t metadata_start = audio_start + static_cast<std::size_t>(audio_size) * 8U;
    if (metadata_start > r.size_bits()) {
        return fail(DecodeError::kInvalidStream, "audio_size runs past the end of the substream");
    }
    out.audio_size = static_cast<std::uint32_t>(audio_size);

    if (auto ok = parse_audio_data_chan(r, ctx, state.element, out.element, hsf_reader); !ok) {
        return ok;
    }
    if (r.position() > metadata_start) {
        return fail(DecodeError::kInvalidStream, "audio_data() runs past audio_size");
    }

    // fill_bits and byte_align up to audio_size, then metadata().
    r.seek(metadata_start);
    if (auto ok = parse_metadata(r, ctx, state.metadata, out.metadata); !ok) {
        return ok;
    }
    r.align();
    return check(r);
}

}  // namespace ac4::detail
