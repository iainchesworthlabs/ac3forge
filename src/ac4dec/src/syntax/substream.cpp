#include "syntax/substream.hpp"

#include <cstddef>
#include <utility>

namespace ac4::detail {

namespace {

// num_obj_info_blocks for an OAMD portion whose own oamd_timing_data() this
// frame did not send: the group's OAMD substream's where the group has sent
// one, else the portion's own from an earlier frame (src/ac4dec/ERRATA.md,
// "Which oamd_timing_data() applies"). Nothing where neither exists.
[[nodiscard]] std::optional<int> carried_blocks(const ObjectAudioContext& objects,
                                                const std::optional<int>& own_before) noexcept {
    if (objects.group_blocks) {
        return objects.group_blocks;
    }
    return own_before;
}

// Part 2 6.2.3.4 audio_data_ajoc(n_fb_upmix_signals, b_static_dmx,
// n_fb_dmx_signals, b_lfe, b_iframe).
[[nodiscard]] ParseResult parse_audio_data_ajoc(BitReader& r, const SubstreamContext& ctx,
                                                const ObjectAudioContext& objects,
                                                AudioSubstreamState& state, AudioSubstream& out) {
    AjocSubstream& ajoc = out.ajoc.emplace();
    const int n_fb_dmx = ctx.n_fullband_dmx;
    const int n_fb_umx = ctx.n_fullband_umx;
    std::optional<int> dmx_blocks;
    if (ctx.b_static_dmx) {
        // audio_data_chan(b_lfe ? 5.1 : 5.0, b_iframe): the static bed.
        SubstreamContext bed = ctx;
        bed.ch_mode = ctx.b_lfe ? ch_mode::k5_1 : ch_mode::k5_0;
        if (auto ok = parse_audio_data_chan(r, bed, state.element, out.element); !ok) {
            return ok;
        }
    } else {
        ajoc.b_some_signals_inactive = r.read_flag("b_some_signals_inactive");
        ajoc.dmx_active_signals_mask =
            n_fb_dmx >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << n_fb_dmx) - 1U;
        if (ajoc.b_some_signals_inactive) {
            // dmx_active_signals_mask[] is one field, [0] its first bit
            // (src/ac4dec/ERRATA.md, "Arrays read as one field").
            const std::size_t start = r.position();
            std::uint64_t mask = 0;
            for (int s = 0; s < n_fb_dmx; ++s) {
                mask = (mask << 1U) | r.peek_raw(1);
                r.consume(1);
            }
            r.emit(start, n_fb_dmx, mask, "dmx_active_signals_mask");
            ajoc.dmx_active_signals_mask = mask;
        }
        if (auto ok =
                parse_var_channel_element(r, ctx, n_fb_dmx, ctx.b_lfe, state.element, out.element);
            !ok) {
            return ok;
        }
        if (r.read_flag("b_dmx_timing")) {
            OamdTimingData timing;
            if (auto ok = parse_oamd_timing_data(r, timing); !ok) {
                return ok;
            }
            ajoc.dmx_timing = timing;
            dmx_blocks = timing.num_obj_info_blocks;
        } else {
            dmx_blocks = carried_blocks(objects, state.dmx_blocks);
        }
        if (!dmx_blocks) {
            return fail(DecodeError::kMissingIFrame,
                        "oamd_dyndata_single() needs an oamd_timing_data() that no frame has sent");
        }
        ajoc.dmx_blocks = *dmx_blocks;
        OamdDynData dmx;
        if (auto ok = parse_oamd_dyndata_single(r, objects.dmx, *dmx_blocks, ctx.b_iframe,
                                                ctx.b_alternative, dmx);
            !ok) {
            return ok;
        }
        ajoc.dmx = std::move(dmx);
        ajoc.b_oamd_extension_present = r.read_flag("b_oamd_extension_present");
        if (ajoc.b_oamd_extension_present) {
            const std::uint64_t bytes = r.variable_bits(3, "skip_bits") + 1U;
            if (auto ok = check(r); !ok) {
                return ok;
            }
            if (bytes > r.remaining_bits() / 8U) {
                return fail(DecodeError::kTruncated, "skip_bits run past the end of the substream");
            }
            const std::uint64_t budget = bytes * 8U;
            const std::size_t before = r.position();
            AjocBedInfo bed_info;
            if (auto ok = parse_ajoc_bed_info(r, bed_info); !ok) {
                return ok;
            }
            const std::uint64_t used = r.position() - before;
            if (used > budget) {
                return fail(DecodeError::kInvalidStream,
                            "an element reads past the byte budget of the data that holds it");
            }
            ajoc.bed_info = bed_info;
            ajoc.skip_bits = budget - used;
            if (auto ok = read_bit_run(r, ajoc.skip_bits, "skip_data"); !ok) {
                return ok;
            }
        }
    }
    if (auto ok = parse_ajoc(r, n_fb_dmx, n_fb_umx, ajoc.ajoc); !ok) {
        return ok;
    }
    if (auto ok =
            parse_ajoc_dmx_de_data(r, n_fb_dmx, n_fb_umx, ctx.b_iframe, state.ajoc_de, ajoc.de);
        !ok) {
        return ok;
    }
    std::optional<int> umx_blocks;
    if (r.read_flag("b_umx_timing")) {
        OamdTimingData timing;
        if (auto ok = parse_oamd_timing_data(r, timing); !ok) {
            return ok;
        }
        ajoc.umx_timing = timing;
        umx_blocks = timing.num_obj_info_blocks;
    } else {
        ajoc.b_derive_timing_from_dmx = r.read_flag("b_derive_timing_from_dmx");
        umx_blocks = ajoc.b_derive_timing_from_dmx && dmx_blocks
                         ? dmx_blocks
                         : carried_blocks(objects, state.umx_blocks);
    }
    if (!umx_blocks) {
        return fail(DecodeError::kMissingIFrame,
                    "oamd_dyndata_single() needs an oamd_timing_data() that no frame has sent");
    }
    ajoc.umx_blocks = *umx_blocks;
    if (auto ok = parse_oamd_dyndata_single(r, objects.umx, *umx_blocks, ctx.b_iframe,
                                            ctx.b_alternative, ajoc.umx);
        !ok) {
        return ok;
    }
    // What each portion sent of its own serves a later frame that sends none.
    if (ajoc.dmx_timing) {
        state.dmx_blocks = ajoc.dmx_timing->num_obj_info_blocks;
    }
    if (ajoc.umx_timing) {
        state.umx_blocks = ajoc.umx_timing->num_obj_info_blocks;
    }
    return check(r);
}

}  // namespace

ParseResult parse_audio_substream(BitReader& r, const SubstreamContext& ctx,
                                  AudioSubstreamState& state, AudioSubstream& out,
                                  BitReader* hsf_reader, const ObjectAudioContext* objects) {
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

    switch (ctx.coding) {
        case AudioCoding::kChannel:
            if (auto ok = parse_audio_data_chan(r, ctx, state.element, out.element, hsf_reader);
                !ok) {
                return ok;
            }
            break;
        case AudioCoding::kAjoc:
            if (objects == nullptr) {
                return fail(DecodeError::kInvalidStream, "an A-JOC substream without its objects");
            }
            if (auto ok = parse_audio_data_ajoc(r, ctx, *objects, state, out); !ok) {
                return ok;
            }
            break;
        case AudioCoding::kObjects:
            if (auto ok = parse_audio_data_objs(r, ctx, ctx.n_objects, ctx.b_lfe, state.element,
                                                out.element);
                !ok) {
                return ok;
            }
            break;
    }
    if (r.position() > metadata_start) {
        return fail(DecodeError::kInvalidStream, "audio_data() runs past audio_size");
    }

    // fill_bits and byte_align up to audio_size, then metadata().
    r.seek(metadata_start);
    if (auto ok = parse_metadata(r, ctx, state.metadata, out.metadata, objects); !ok) {
        return ok;
    }
    r.align();
    return check(r);
}

}  // namespace ac4::detail
