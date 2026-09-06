#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <sstream>
#include <string>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/mxf.hpp"

// ac3iab::parse_iabitstream(std::istream&) and ac3iab::parse_mxf_iab(std::istream&)
// (src/ac3iab/src/iab_reader.cpp, mxf_reader.cpp), plus ac3iab::parse_iaframe
// on the same bytes - the IAB reader, roadmap IM1 phases 1 and 2.
//
// The whole of ac3iab exists to read files this project did not write: an
// elementary .iab IABitstream, or an IAB track file that a mastering tool
// clip-wrapped into MXF. Both arrive from outside, and until now neither had
// a harness - the gap this file closes.
//
// Three entry points from one input rather than three harnesses, because they
// are three framings of the same bytes and libFuzzer's corpus is more useful
// shared between them than split three ways:
//
//   parse_iabitstream  §7's Preamble+IAFrame run, the elementary form
//   parse_mxf_iab      the KLV walk that extracts that run from a track file
//   parse_iaframe      §9.1 Table 5, one already-extracted frame's payload
//
// parse_iaframe is reached directly as well as through the other two because
// it is a public entry point in its own right (see ac3iab.hpp's own note on
// why), so a caller holding one extracted frame can reach the §9.1 parser
// without the framing above it ever running.
//
// What this is expected to press on: the reader sizes almost everything from
// the stream's own numbers - ElementSize, IAFrameLength, the BER lengths in
// the MXF Key/Length/Value walk, and the Plex(n) escape chain in
// BitReader::read_plex, whose §5.2 bound (`width >= 32` is kBadEscape) is
// exactly the kind of clause a fuzzer finds the far side of.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string bytes(reinterpret_cast<const char*>(data), size);

    {
        std::istringstream stream(bytes, std::ios::binary);
        (void)ac3iab::parse_iabitstream(stream);
    }
    {
        std::istringstream stream(bytes, std::ios::binary);
        (void)ac3iab::parse_mxf_iab(stream);
    }
    (void)ac3iab::parse_iaframe(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size));

    return 0;
}
