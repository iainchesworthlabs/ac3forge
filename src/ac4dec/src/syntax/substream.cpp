#include "syntax/substream.hpp"

#include <cstddef>

namespace ac4::detail {

ParseResult parse_audio_substream(BitReader& r, const SubstreamContext& ctx, AudioSubstreamState& state,
                                  AudioSubstream& out) {
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
    if (ctx.sf_multiplier) {
        // The ASF tables at 96 kHz and 192 kHz (Annex B's other columns) and
        // the HSF extension are not transcribed.
        return fail(DecodeError::kUnsupported, "96 kHz and 192 kHz substreams are not decoded");
    }
    const std::size_t audio_start = r.position();
    const std::size_t metadata_start = audio_start + static_cast<std::size_t>(audio_size) * 8U;
    if (metadata_start > r.size_bits()) {
        return fail(DecodeError::kInvalidStream, "audio_size runs past the end of the substream");
    }
    out.audio_size = static_cast<std::uint32_t>(audio_size);

    if (auto ok = parse_audio_data_chan(r, ctx, state.element, out.element); !ok) {
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
