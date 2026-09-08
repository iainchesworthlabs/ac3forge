// The probe's entry point and clock on ESP32-S3.
//
// ESP-IDF calls app_main() rather than main(), which is why apps/baremetal
// exposes ac3probe::run() through probe.hpp instead of defining main() itself.
// Everything the probe checks - every fixture in fixture.hpp against its levels, the
// allocation counts, the heap peak, the refusal of the direct-form transform -
// is the same code the host and arm-none-eabi shapes run. Nothing about the
// decode is special-cased here, and nothing should be.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "probe.hpp"

namespace ac3probe {

// esp_timer is a 64-bit microsecond counter driven by the systimer peripheral,
// not by the CPU clock, so it does not stop or stretch when the CPU is
// throttled - and it is the same clock across both cores.
//
// esp_cpu_get_cycle_count() (CCOUNT) would be finer-grained, but it is 32 bits
// and wraps every ~17.9 seconds at 240 MHz, and it is PER-CORE - a task that
// migrates between cores mid-decode would read two unrelated counters and
// produce nonsense. Microseconds times a known clock rate is the number that
// survives both. The clock rate is reported below so cycles can be recovered.
std::uint64_t now_us() { return static_cast<std::uint64_t>(esp_timer_get_time()); }

void report_internal_sram(const char* when) {
    // Byte-addressable internal SRAM: what plain malloc/operator new can return,
    // and therefore the only pool the decoder reaches today.
    constexpr std::uint32_t kInternal8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    // All internal SRAM, byte-addressable or not. IDF adds whatever IRAM the
    // application did not fill to the heap as 32-BIT-ACCESS-ONLY memory: a load
    // or store narrower than a word faults there, so malloc and operator new
    // never hand it out and the difference between these two totals is invisible
    // to a C++ program that only ever says `new`.
    //
    // It is reported because of what this decoder allocates. Its large buffers
    // are arrays of float and double - aht_coeffs_ at 43,008 bytes, the coupling
    // and transform scratch, oba::joc::ReconstructionState at 147,504 - and word
    // arrays are exactly what 32-bit-only memory is good for. Reaching it needs
    // heap_caps_malloc(n, MALLOC_CAP_32BIT) behind those allocations rather than
    // the global operator new they use now, so this number is a measurement of
    // an OPPORTUNITY, not of anything the decode currently uses.
    constexpr std::uint32_t kInternalAny = MALLOC_CAP_INTERNAL;
    const std::size_t any = heap_caps_get_free_size(kInternalAny);
    const std::size_t byte_addressable = heap_caps_get_free_size(kInternal8Bit);
    std::printf("esp32s3.internal_free_bytes[%s]=%lu "
                "esp32s3.internal_largest_block_bytes[%s]=%lu "
                "esp32s3.internal_word_only_bytes[%s]=%lu\n",
                when, static_cast<unsigned long>(byte_addressable), when,
                static_cast<unsigned long>(
                    heap_caps_get_largest_free_block(kInternal8Bit)),
                when, static_cast<unsigned long>(any - byte_addressable));
}

}  // namespace ac3probe

namespace {

// The CPU clock, measured rather than read from configuration.
//
// CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ says what the build ASKED for, which is not
// the same claim: dynamic frequency scaling, a failed PLL lock or simply a
// stale sdkconfig would each leave it saying 240 while the part runs at 80, and
// every cycles-per-frame figure derived from it would then be three times too
// large with nothing to show for it. Since the whole reason this port exists is
// to produce a trustworthy cycles-per-frame number, the clock underneath it had
// better be observed too.
//
// esp_rom_delay_us() busy-waits rather than yielding, which matters: CCOUNT is
// per-core, so a task that slept and resumed on the other core would difference
// two unrelated counters. A busy wait cannot migrate.
std::uint32_t measure_cpu_mhz() {
    constexpr std::uint32_t kWindowUs = 10000;
    const std::uint32_t start_cycles = esp_cpu_get_cycle_count();
    const std::int64_t start_us = esp_timer_get_time();
    esp_rom_delay_us(kWindowUs);
    const std::uint32_t elapsed_cycles = esp_cpu_get_cycle_count() - start_cycles;
    const std::int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (elapsed_us <= 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((static_cast<std::int64_t>(elapsed_cycles) + elapsed_us / 2) /
                                      elapsed_us);
}

}  // namespace

extern "C" void app_main() {
    // Printed before the probe runs so that a run at an unexpected clock is
    // visible in the log even if the decode later fails, and so every
    // microsecond figure below can be converted to cycles by a reader.
    std::printf("target=esp32s3 cpu_mhz=%lu\n",
                static_cast<unsigned long>(measure_cpu_mhz()));

    // What the ALLOCATOR has, asked of the allocator.
    //
    // `idf.py size` reports a "remain" figure for DIRAM, and it is a linker
    // estimate: it subtracts the image from the pool without accounting for what
    // the ROM and IDF hold at runtime, and it was 73,708 bytes PESSIMISTIC when
    // the two were first compared (207,084 against the 280,792 below). A
    // footprint budget quoted from it is a budget nobody has checked.
    //
    // The largest contiguous block is the second number and not a refinement of
    // the first. A single 147,504-byte oba::joc::ReconstructionState needs one
    // run of free memory, not a total - so free_bytes alone cannot say whether an
    // allocation of a given size will succeed, and the two diverge as the heap
    // fragments.
    ac3probe::report_internal_sram("before");

    ac3probe::run();

    // Again afterwards. The difference is what the decode did not give back -
    // the thread_local enhanced-coupling scratch the probe reports as
    // heap.retained_bytes, seen from the system side rather than from inside the
    // allocator hooks - plus whatever fragmentation the run left behind, which
    // shows up in the largest block rather than in the total.
    ac3probe::report_internal_sram("after");

    // The decode runs on THIS task, and sdkconfig.defaults sets its stack to
    // 32,768 bytes with, in its own words, "no attempt to trim it" - after an
    // overflow that surfaced as a LoadProhibited panic on the other core. That
    // comment tells an integrator to measure with uxTaskGetStackHighWaterMark()
    // and then nothing measured it. High-water is the minimum free the stack has
    // ever had, so 32768 minus this is the deepest the decode actually went.
    std::printf("esp32s3.main_task_stack_free_bytes=%lu\n",
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    // app_main returns into a FreeRTOS task that is then deleted; without this
    // the console output can be cut off by the task teardown before the UART
    // FIFO drains. Nothing here is timing-sensitive, so a flat delay is enough.
    vTaskDelay(pdMS_TO_TICKS(200));
}
