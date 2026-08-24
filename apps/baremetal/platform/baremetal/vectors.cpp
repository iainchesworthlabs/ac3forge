// The Cortex-M reset vector for the minimum-footprint decoder probe
// (roadmap PF7). Two words at address 0, which is the whole of what this core
// needs to start: the initial stack pointer, then the address to begin
// executing at. Everything after that - zeroing .bss, running static
// constructors, calling main, and turning main's return value into a
// semihosting exit - is newlib's crt0 and rdimon, reached through _start.
//
// This is the only file in the repository whose contents depend on the
// PROCESSOR rather than on the operating system, which is why it sits in its
// own platform directory and is added to the target only by the cross build
// (apps/baremetal/CMakeLists.txt) - the same mechanism src/audio's backends
// and ac3/internal/profiling.hpp use, and the reason nothing here needs an
// #ifdef.

extern "C" void _start();

// Defined by platform/baremetal/mps2-an385.ld as the top of the one memory
// region. Declared as an object so its ADDRESS is what gets taken - the value
// at that address is past the end of memory and is never read.
extern "C" unsigned __stack;

extern "C" {

using Handler = void (*)();

// KEEP(*(.vectors)) in the linker script places this first in .text, and
// .text starts at 0. `used` because nothing in the program refers to it and
// --gc-sections would otherwise be entirely right to discard it.
__attribute__((section(".vectors"), used))
Handler const kVectorTable[2] = {reinterpret_cast<Handler>(&__stack), &_start};

}  // extern "C"
