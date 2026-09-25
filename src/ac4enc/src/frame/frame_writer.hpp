#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "ac4/syntax.hpp"
#include "bit_writer.hpp"
#include "frame/metadata.hpp"

// One raw_ac4_frame() (ETSI TS 103 190-2 V1.3.1 clause 6.2.1, Part 1 clause
// 4.2.1): the table of contents at bitstream_version 2 with one version 1
// presentation over one substream group of one channel-coded substream, then
// the substreams it indexes: the presentation substream (index 0) and the audio
// substream (index 1). The encoder writes mono and stereo; the decoder's tests
// build frames of the other channel modes of Part 1 Table 88 with it.

namespace ac4::detail {

struct FrameFields {
    int sequence_counter = 0;   // 0 to 1020
    // Part 1 Table 81: 0 a constant bit rate, 1 to 6 an average one with the
    // decoder waiting 0 to 5 frames (twice that at indices 10 to 12), 7 a
    // variable one; above 0, Part 2 Table 52's br_code follows.
    int wait_frames = 0;
    int br_code = 0;
    bool iframe = true;         // b_iframe_global, b_pres_ndot and b_audio_ndot
    int fs_index = 1;           // Part 1 Table 82: 1 = 48 kHz, 0 = 44.1 kHz
    int frame_rate_index = 13;
    int ch_mode = 1;            // Part 1 Table 88: 0 mono, 1 stereo, 2 3.0, 3 and 4 5.X, 5 to 10 7.X
    bool add_ch_base = false;   // for 7.X 5/2/0 and 3/2/2 (Part 2 clause 6.3.2.7)
    int dialnorm_bits = 124;    // Part 1 clause 4.3.12.2.1: -dialnorm_bits / 4 dBFS
    // The stream's metadata beside dialnorm (frame/metadata.hpp), and with
    // dialogue enhancement this frame's parameters and the last frame's, which
    // a frame that is not an I-frame codes against.
    const StreamMetadata* metadata = nullptr;
    const DeFrameParameters* de = nullptr;
    const DeFrameParameters* de_previous = nullptr;
};

// The bits the frame spends on everything but the channel element and its
// fill, with the audio substream at `audio_substream_bytes`: the table of
// contents, the presentation substream, the audio substream's header,
// metadata() and the alignment after both. An upper bound over the alignment.
[[nodiscard]] std::size_t frame_overhead_bits(const FrameFields& fields, std::size_t audio_substream_bytes);

// Writes the frame. `audio` is a buffered writer holding audio_data_chan(): the
// channel element and nothing after it. With `frame_bytes` above zero, the
// audio substream takes fill_bits so that the frame is exactly that long, and
// nothing is returned when the content alone is longer; with zero, the frame
// is as long as its content. The substreams' records go to `sink`, the audio
// data's offset by where it lands.
[[nodiscard]] std::optional<std::vector<std::byte>> write_frame(const FrameFields& fields, const BitWriter& audio,
                                                                std::size_t frame_bytes, SyntaxSink sink);

}  // namespace ac4::detail
