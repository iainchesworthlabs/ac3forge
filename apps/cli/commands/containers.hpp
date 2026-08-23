#pragma once

#include <cstdint>
#include <string_view>

#include "../support.hpp"

// The container-wrapping commands: mkv/mp4/fmp4/ts each take an already-encoded AC-3 or E-AC-3
// elementary stream and wrap it for a specific downstream consumption method, reading everything
// the container needs to declare (format, access-unit boundaries, sample rate, channel count)
// straight off the bitstream via ac3::io::scan rather than from a caller-supplied layout that
// could disagree with what the file actually contains. Split out of main.cpp as part of the
// repo-structure review's H4 monolith split - see support.hpp's own top comment for the overall
// plan; this is the first commands/ group, the four physically-contiguous container commands
// (spdif is elsewhere in main.cpp and will be grouped with its own file-position neighbors in a
// later step, not here).
namespace ac3cli::commands {

int run_mkv(std::string_view in_path, std::string_view out_path);

// Same shape as run_mkv, wrapping mp4::mux() instead: ac3::io::scan() still supplies everything
// the container needs to declare, and additionally - via ac3::io::build_codec_config_box() - the
// exact dac3/dec3 sample-entry payload straight off the bitstream.
int run_mp4(std::string_view in_path, std::string_view out_path);

// fMP4/CMAF segmenting plus HLS/DASH signaling (ROADMAP.md's A2) - writes a DIRECTORY of files
// (an init segment, one media segment per fragment, an HLS media+master playlist pair, and a
// DASH MPD) rather than one file, so a packager or CDN origin can be pointed at out_dir directly.
// meta.hls_fallback_51 additionally writes the object-stripped 5.1 companion rendition into a
// bed51/ subdirectory and lists it in the same #EXT-X-MEDIA group as the JOC one, which is what
// Apple's HLS Authoring Specification asks for alongside CHANNELS="<N>/JOC" - see
// ac3::io::strip_objects. Ignored for a stream with no object layer.
int run_fmp4(std::string_view in_path, std::string_view out_dir, std::uint32_t frames_per_fragment,
             const Options& meta);

// Same shape as run_mkv: wraps as an MPEG-2 Transport Stream. `profile` selects which registry
// identifies the stream in the PMT - "dvb" (the default: stream_type 0x06 plus ETSI EN 300 468
// Annex D's AC3_descriptor/enhanced_AC-3_descriptor) or "atsc" (stream_type 0x81/0x87 plus
// A/52:2018 Annex A/Annex G's own descriptors). Everything either descriptor says about the
// service is read off the bitstream by ac3::io::scan, except the service associations meta
// carries (mainid=, asvc=).
int run_ts(std::string_view in_path, std::string_view out_path, std::string_view profile,
           const Options& meta);

}  // namespace ac3cli::commands
