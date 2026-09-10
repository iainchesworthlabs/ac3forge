// ac3probe::now_us() for arm-none-eabi on QEMU's mps2-an385, read from the
// board's CMSDK APB TIMER0 rather than from semihosting's std::clock().
// Selected by AC3FORGE_BAREMETAL_CLOCK=timer (apps/baremetal/CMakeLists.txt);
// clock.cpp beside this file is the default.
//
// WHY A SECOND CLOCK EXISTS. Under `qemu-system-arm -icount shift=0` the
// guest's virtual clock advances by exactly one nanosecond per executed
// instruction, deterministically, whatever the host is doing. Semihosting's
// std::clock() does not follow it - QEMU answers that call from the HOST's
// clock - but the board's timers do, and TIMER0 here runs from the AN385's
// 25 MHz peripheral clock: one tick every forty nanoseconds, so every forty
// instructions. What the probe then prints as microseconds is instructions
// divided by a thousand, the same on every machine and every run, and
// tools/checks/run_baremetal_probe.sh --icount is what reads it back as
// `<fixture>.instructions_per_frame=` and gates it.
//
// It is a count of Thumb-2 instructions on a soft-float Cortex-M3, not cycles
// on any real part; its value is that it is deterministic and moves when the
// code does, which the host-time figure never was. Outside -icount this clock
// still works - it then measures QEMU's virtual time at whatever pace the
// host manages, which is the same thing std::clock() was measuring.
//
// Register map of the CMSDK APB timer (Arm DDI 0479, §4.1): CTRL at +0x00
// (bit 0 enable, bit 3 interrupt enable), VALUE at +0x04, RELOAD at +0x08.
// It counts DOWN from RELOAD and reloads at zero; at 25 MHz a 32-bit reload
// wraps every 171 seconds of virtual time, and a whole probe run under
// -icount is a few seconds of it, so no wrap is handled - a reading past a
// wrap would come back small rather than wrong-looking, and the runner's
// ceilings would call it out as a regression the wrong way round, which is
// still not silent.

#include <cstdint>

#include "probe.hpp"

namespace {

constexpr std::uintptr_t kTimer0 = 0x40000000u;
constexpr std::uint32_t kCtrl = 0x00u;
constexpr std::uint32_t kValue = 0x04u;
constexpr std::uint32_t kReload = 0x08u;
constexpr std::uint64_t kClockHz = 25000000ULL;

volatile std::uint32_t* reg(std::uint32_t offset) {
    return reinterpret_cast<volatile std::uint32_t*>(kTimer0 + offset);
}

bool g_started = false;

}  // namespace

namespace ac3probe {

std::uint64_t now_us() {
    if (!g_started) {
        *reg(kReload) = 0xFFFFFFFFu;
        *reg(kValue) = 0xFFFFFFFFu;
        *reg(kCtrl) = 1u;  // enable; internal clock; no interrupt
        g_started = true;
    }
    const std::uint32_t ticks = 0xFFFFFFFFu - *reg(kValue);
    return (static_cast<std::uint64_t>(ticks) * 1000000ULL) / kClockHz;
}

}  // namespace ac3probe
