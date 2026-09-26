#pragma once

#include <cstdint>
#include <optional>

#include "bit_reader.hpp"
#include "syntax/ajoc.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"
#include "syntax/metadata.hpp"
#include "syntax/oamd.hpp"

// ac4_substream() (ETSI TS 103 190-2 V1.3.1 clause 6.2.2.2): the audio_size
// header, the audio data - audio_data_chan() for a channel-coded substream,
// audio_data_ajoc() (6.2.3.4) for an A-JOC coded one and audio_data_objs()
// (6.2.3.2) for a direct-coded object one - and metadata(), reached through
// audio_size as Part 1 clause 4.3.4.1 allows.

namespace ac4::detail {

// audio_data_ajoc(n_fb_upmix_signals, b_static_dmx, n_fb_dmx_signals, b_lfe,
// b_iframe) besides its channel element: the first OAMD portion, for core
// decoding, where the downmix is not the static bed; ajoc() and
// ajoc_dmx_de_data(); and the second portion, for full decoding (Part 2
// clause 4.8.3.4.2).
struct AjocSubstream {
    bool b_some_signals_inactive = false;
    // dmx_active_signals_mask[], a flag per fullband downmix signal in the order
    // they are sent, [0] the field's first bit; all set where not sent.
    std::uint64_t dmx_active_signals_mask = 0;
    std::optional<OamdTimingData> dmx_timing;  // b_dmx_timing
    std::optional<OamdDynData> dmx;            // b_static_dmx 0
    bool b_oamd_extension_present = false;
    std::optional<AjocBedInfo> bed_info;
    std::uint64_t skip_bits = 0;  // the skip_data after ajoc_bed_info()
    AjocData ajoc{};
    AjocDmxDeData de{};
    std::optional<OamdTimingData> umx_timing;  // b_umx_timing
    bool b_derive_timing_from_dmx = false;
    OamdDynData umx{};
    // num_obj_info_blocks each portion was read with.
    int dmx_blocks = 0;
    int umx_blocks = 0;
};

// Everything one audio substream carries from frame to frame, and the channel
// mode and substream syntax version it was carried under (-1 before the
// first frame).
struct AudioSubstreamState {
    int ch_mode = -1;
    int sus_ver = -1;
    // An object substream's coding and the counts its syntax is read with.
    AudioCoding coding = AudioCoding::kChannel;
    bool b_lfe = false;
    bool b_static_dmx = false;
    int n_fullband_dmx = 0;
    int n_fullband_umx = 0;
    int n_objects = 0;
    ChannelElementState element{};
    MetadataState metadata{};

    // Whether what this state carries was read under the same channel mode,
    // substream version and object shape as `ctx`: a change of any starts the
    // substream afresh.
    [[nodiscard]] bool carries(const SubstreamContext& ctx) const noexcept {
        return ch_mode == ctx.ch_mode && sus_ver == ctx.sus_ver && coding == ctx.coding &&
               b_lfe == ctx.b_lfe && b_static_dmx == ctx.b_static_dmx &&
               n_fullband_dmx == ctx.n_fullband_dmx && n_fullband_umx == ctx.n_fullband_umx &&
               n_objects == ctx.n_objects;
    }
    void carry(const SubstreamContext& ctx) noexcept {
        ch_mode = ctx.ch_mode;
        sus_ver = ctx.sus_ver;
        coding = ctx.coding;
        b_lfe = ctx.b_lfe;
        b_static_dmx = ctx.b_static_dmx;
        n_fullband_dmx = ctx.n_fullband_dmx;
        n_fullband_umx = ctx.n_fullband_umx;
        n_objects = ctx.n_objects;
    }
    // An A-JOC substream's: ajoc_dmx_de_data()'s configuration and
    // coefficients, and the num_obj_info_blocks of the last timing each OAMD
    // portion sent of its own.
    AjocDmxDeState ajoc_de{};
    std::optional<int> dmx_blocks;
    std::optional<int> umx_blocks;
};

struct AudioSubstream {
    std::uint32_t audio_size = 0;  // bytes of audio_data() and its fill
    ChannelElement element{};
    Metadata metadata{};
    std::optional<AjocSubstream> ajoc;  // audio_data_ajoc()
};

// Reads the whole substream. The reader spans exactly the substream's bytes.
// Checks that audio_data() ends inside audio_size (leaving only fill and
// alignment) and that metadata() ends inside the substream. `hsf_reader`, as
// for parse_audio_data_chan(), is the linked HSF extension substream's own
// reader, or nullptr where none is linked or `ctx.sf_multiplier` is unset -
// see decoder.cpp, which reads the rest of the extension (sf_hsf_data() per
// track) once this call returns. An object substream (ctx.coding kAjoc or
// kObjects) needs `objects`.
[[nodiscard]] ParseResult parse_audio_substream(BitReader& r, const SubstreamContext& ctx,
                                                AudioSubstreamState& state, AudioSubstream& out,
                                                BitReader* hsf_reader = nullptr,
                                                const ObjectAudioContext* objects = nullptr);

}  // namespace ac4::detail
