#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/oba/oamd.hpp"

// ac3::oba::parse_payload (src/forge/src/oba/oamd.cpp) - the
// object_audio_metadata_payload of TS 103 420 §5, as recovered from an EMDF
// payload with id 11.
//
// The bytes handed here have already been through emdf::parse_container, but
// that container carries no checksum of its own and validates nothing about
// what a payload holds: it copies `emdf_payload_size` bytes out of the skip
// field and calls them a payload. So everything OAMD then reads - the object
// count, the block-update count, the per-object position and gain fields, the
// bed assignment - is still exactly the bytes an attacker put in the frame.
//
// A separate harness from fuzz_emdf_parse rather than a chained one: seeding
// this with real OAMD payloads (fuzz/extract-metadata-seeds.py pulls them out
// of the Atmos streams generate-seeds.sh already builds) puts every mutation
// inside the payload, where reaching the same states through a container
// would spend most of the budget on container syntax instead.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    (void)ac3::oba::parse_payload(bytes);
    return 0;
}
