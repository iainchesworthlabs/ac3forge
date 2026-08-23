#pragma once

#include <string>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/io/elementary.hpp"

// The ISOBMFF codec-configuration box a container muxer embeds beside an
// 'ac-3'/'ec-3' sample entry: ETSI TS 102 366 Annex F §F.4 AC3SpecificBox
// ('dac3') or §F.6 EC3SpecificBox ('dec3'), plus - for E-AC-3 carrying Dolby
// Atmos objects - the flag_ec3_extension_type_a/complexity_index_type_a
// extension TS 103 420 §8.3.1/§8.3.2.2 defines for the bitstream's own addbsi
// and this box is documented to echo verbatim.
//
// Built here, in ac3::io beside the scanner that already reads every one of
// these values off the bitstream, rather than by a container muxer: fscod,
// bsid, bsmod, acmod, lfeon and the object-audio marker are all AC-3 syntax,
// not container concepts, and a general-purpose ISOBMFF writer (mp4::mp4) has
// no business re-deriving AC-3 semantics just to build one sample-entry
// child box. mp4::mux() therefore treats the return value as opaque bytes,
// the same way it treats every access unit as opaque bytes - see
// mp4::AudioTrack::codec_config and examples/mux_mp4.cpp.

namespace ac3::io {

// Returns the box's PAYLOAD only - everything after its own 8-byte
// size+FourCC header, which is the container muxer's job to write (it is the
// one that knows the ISOBMFF box-nesting mechanics; see mp4::mux()). The
// FourCC itself is implied by `stream.kind`: kAc3 -> 'dac3', kEac3 -> 'dec3'.
[[nodiscard]] AC3FORGE_EXPORT std::vector<std::byte> build_codec_config_box(
    const ScannedStream& stream);

// The DASH <AudioChannelConfiguration> @value for this stream on the Dolby
// scheme DASH-IF IOP Part 8 v5.0.0 §5.3.2 offers for E-AC-3 -
// "tag:dolby.com,2014:dash:audio_channel_configuration:2011 as defined in
// TS 102 366 clause I.1.2.1": four upper-case hexadecimal digits of the
// 16-bit channel-assignment field, left channel in the most significant bit,
// so a 5.1 stream is "F801". That field is ScannedStream::channel_map
// verbatim (ATSC A/52-2018 Table E2.5 is the same sixteen locations in the
// same order), which is why this is one std::format rather than a table.
//
// Beside build_codec_config_box for its own reason: which locations an AC-3
// or E-AC-3 stream carries is acmod/lfeon/chanmap syntax, read by the scanner
// and derived nowhere else. A container or manifest writer (mp4::, and
// mp4::DashOptions::dolby_channel_configuration in particular) has no business
// re-deriving AC-3 semantics to fill in one attribute, the same boundary the
// dac3/dec3 payload above already draws.
[[nodiscard]] AC3FORGE_EXPORT std::string dash_channel_configuration(const ScannedStream& stream);

}  // namespace ac3::io
