#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4enc/encoder.hpp"

// What forge's `ac4-encode` and forge-gui's AC-4 encode share, so that the
// command line the GUI echoes writes the same bytes the GUI does: the encoder's
// input channels for a WAV file's channel count, the programme's BS.1770
// measurements behind dialnorm=auto and loudness=, and the sync frames or the
// MP4 file the encoded frames become. Compiled straight into each application,
// as the rest of apps/common is (recording_sink.hpp says why there is no
// library target).

namespace iclforge::apps {

// The encoder's input channels, in iclforge::ac4::Decoder's order, for a WAV file of
// `count` channels, the 7.X element's additional pair, which seven or eight
// channels need and the other counts leave to another substream, whether the
// 3.0 element is asked for, and whether the immersive layouts' back pair is:
// nine and ten channels are 5.0.4 and 5.1.4, eleven and twelve 7.0.4 and
// 7.1.4. Empty for a count the encoder does not take so.
[[nodiscard]] std::vector<iclforge::ac4::Speaker> ac4_input_speakers(std::size_t count,
                                                           iclforge::ac4::AdditionalPair pair,
                                                           bool three_zero, bool back_pair);

// The layout `ac4_input_speakers` makes of `count` channels, by name.
[[nodiscard]] std::string_view ac4_layout_name(std::size_t count,
                                               iclforge::ac4::AdditionalPair pair);

// For each of the encoder's input channels, the WAV file's channel it takes
// (ac4_channels.hpp's order).
[[nodiscard]] std::vector<std::size_t> ac4_wav_index(
    std::span<const iclforge::ac4::Speaker> speakers);

// BS.1770's measurements of the programme, for dialnorm=auto and loudness=:
// the integrated loudness, the loudness range, the true peak, and the highest
// momentary and short-term loudness, read every 100 ms, the step at which the
// meter's 400 ms blocks overlap.
struct Ac4Measured {
    double integrated = 0.0;
    std::optional<double> range;
    std::optional<double> true_peak;
    std::optional<double> max_momentary;
    std::optional<double> max_short_term;
};

// Over the channels the encoder takes, `channels` in its order: the loudness
// over the 5.1 or 5.0 bed of a 7.X or immersive layout, as encode measures
// E-AC-3's, and the true peak over every channel. Nothing where no block
// passes the absolute gate.
[[nodiscard]] std::optional<Ac4Measured> measure_ac4_programme(
    std::span<const std::span<const float>> channels, std::uint32_t sample_rate);

// dialnorm=auto's value for a measured integrated loudness: the dB below full
// scale, 0 to 31.75 in steps of 0.25.
[[nodiscard]] double ac4_dialnorm_for(double integrated_lkfs);

// loudness='s values for a measured programme, each within what its code
// holds: -102.4 to +102.3, the range 0 to 102.3 LU (Part 1 clauses 4.3.12.3.8
// to 4.3.12.3.30).
[[nodiscard]] iclforge::ac4::FurtherLoudness ac4_further_loudness(
    iclforge::ac4::LoudnessPractice practice, const Ac4Measured& measured);

// The encoded frames as a file: each a raw stream's sync frame, with the CRC
// of TS 103 190-2 Annex G where `crc`, or together an MP4 file with Annex E's
// 'ac-4' sample entry, each frame a sample and the I-frames its sync samples.
struct Ac4Packaged {
    std::vector<std::vector<std::byte>> chunks;  // written one after another
    std::string rfc6381;                          // the MP4 track's codecs string
};

struct Ac4PackageError {
    std::string message;
    // Whether the configuration asked for something the container cannot
    // describe (forge's usage error), rather than a failure writing it.
    bool usage = false;
};

[[nodiscard]] std::expected<Ac4Packaged, Ac4PackageError> package_ac4(
    std::span<const iclforge::ac4::EncodedFrame> frames, const iclforge::ac4::Toc& toc, bool mp4,
    bool crc);

// Whether an output path names an MP4 file, as `remux` matches them: by the
// path's extension, case kept, .mp4, .m4a or .mov. What forge's ac4-encode and
// atmos-encode write an MP4 file for; the page writes one for its MP4
// container.
[[nodiscard]] bool ac4_output_names_mp4(std::string_view out_path);

}  // namespace iclforge::apps
