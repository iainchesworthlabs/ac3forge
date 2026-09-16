#pragma once

#include <cstdint>

#include "bit_reader.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"
#include "syntax/metadata.hpp"

// ac4_substream() (ETSI TS 103 190-2 V1.3.1 clause 6.2.2.2) for a
// channel-coded substream: the audio_size header, audio_data_chan(), and
// metadata(), reached through audio_size as Part 1 clause 4.3.4.1 allows.

namespace ac4::detail {

// Everything one audio substream carries from frame to frame, and the channel
// mode and substream syntax version it was carried under (-1 before the
// first frame).
struct AudioSubstreamState {
    int ch_mode = -1;
    int sus_ver = -1;
    ChannelElementState element{};
    MetadataState metadata{};
};

struct AudioSubstream {
    std::uint32_t audio_size = 0;  // bytes of audio_data() and its fill
    ChannelElement element{};
    Metadata metadata{};
};

// Reads the whole substream. The reader spans exactly the substream's bytes.
// Checks that audio_data() ends inside audio_size (leaving only fill and
// alignment) and that metadata() ends inside the substream.
[[nodiscard]] ParseResult parse_audio_substream(BitReader& r, const SubstreamContext& ctx,
                                                AudioSubstreamState& state, AudioSubstream& out);

}  // namespace ac4::detail
