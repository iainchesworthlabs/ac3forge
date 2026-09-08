// Thread-local storage for a target that has no threads (roadmap PF7).
//
// ac3::eac3::ecpl_channel_spectrum keeps its 32 KB of §3.5.5 reconstruction
// scratch in a `static thread_local` - the right answer for a library whose
// callers may decode two streams at once, and one that costs nothing on any
// hosted platform. On arm-none-eabi it costs a link error: GCC emits a call to
// __aeabi_read_tp for every access, and newlib-nano has no thread-pointer
// runtime to resolve it against.
//
// A single-threaded target can supply one trivially. The ARM TLS ABI (variant
// 1) says the thread pointer addresses an 8-byte thread control block and the
// TLS image begins immediately after it, so returning `block - 8` puts every
// thread_local inside `block`. With exactly one thread there is exactly one
// block, and it never needs allocating, freeing or switching.
//
// Two things make that safe rather than merely plausible, and both are checked
// rather than assumed:
//
//   - Size. platform/baremetal/mps2-an385.ld asserts SIZEOF(.tbss) fits, so
//     growing the scratch past this block fails the LINK instead of quietly
//     writing past it.
//   - Initialisation. The same script asserts .tdata is empty, i.e. every
//     thread_local in the image is zero-initialised. A non-zero initialiser
//     would need its image copied in here, and nothing does that - so the
//     assert refuses the build rather than letting one start life as garbage.
//
// This file exists only in the bare-metal build (apps/baremetal/CMakeLists.txt
// adds this directory only when cross-compiling); the hosted build's C library
// provides the real thing.

#include <cstddef>
#include <cstdint>

namespace {

// 4 KiB. It was 64 KiB, sized against a 32 KiB thread_local
// EcplSpectrumScratch that no longer exists: src/forge/src/core/eac3_tools.cpp
// now keeps only a unique_ptr in TLS, because FreeRTOS carves each task's
// thread-local area out of that task's own stack and a 32 KiB one made the
// library unlinkable into any RTOS application (see that file's own comment).
//
// The whole TLS image is 552 bytes after that change - the two remaining
// thread_locals are a std::vector header and a unique_ptr - so 4 KiB is still
// better than seven times what is there, on the same "ordinary growth does not
// need this revisited" reasoning that chose the old number. It saves 61,440
// bytes of .bss, which on a target whose entire subject is footprint is not a
// rounding error.
//
// .bss, so it is zeroed by newlib's startup before any constructor runs - which
// is what makes the "every thread_local is zero-initialised" contract above
// hold in practice. The linker script's ASSERT is the thing that actually
// enforces the size; grow both together or neither.
constexpr std::size_t kTlsBlockBytes = 4 * 1024;
constexpr std::size_t kArmTcbBytes = 8;

alignas(16) std::uint8_t g_tls_block[kTlsBlockBytes];

}  // namespace

extern "C" void* __aeabi_read_tp() {
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(g_tls_block) - kArmTcbBytes);
}
