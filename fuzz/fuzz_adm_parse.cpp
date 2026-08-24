#include <cstddef>
#include <cstdint>
#include <ios>
#include <sstream>
#include <string>

#include "ac3adm/ac3adm.hpp"

// ac3adm::parse_bw64(std::istream&) (src/ac3adm/src/adm.cpp) - the BW64/RF64
// container walk plus the ADM XML document inside <axml>.
//
// The widest untrusted surface in the tree by input language: a BW64 file is
// chunk-structured binary carrying an arbitrary XML document, and both halves
// come straight from whoever produced the file. It is also the one parser
// here that is not clean-room - it delegates to the vendored libbw64 and
// libadm (see src/ac3adm/CMakeLists.txt) - so a report from this harness may
// land in third-party code rather than in ac3forge's own; that is worth
// knowing either way, since the bytes reach it through an ac3forge API.
//
// Built only with -DAC3FORGE_BUILD_ADM=ON, which is off by default and
// additionally needs vcpkg's "adm" feature for libadm's Boost headers, so
// this harness is not in fuzz/run.sh's default target list - see fuzz/
// README.md's "The ADM harness is opt-in".
//
// parse_bw64 spools the stream to a temporary file and reopens it by path
// (libbw64's reader has no istream constructor - see the header's own note),
// so each execution costs a file create/write/read/unlink. That is the same
// unavoidable round trip fuzz_wav_read already pays, and the reason this
// harness runs at a fraction of the others' exec/s.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::istringstream stream(std::string(reinterpret_cast<const char*>(data), size),
                              std::ios::binary);
    (void)ac3adm::parse_bw64(stream);
    return 0;
}
