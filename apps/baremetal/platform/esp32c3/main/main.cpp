// The probe's entry point and clock on ESP32-C3.
//
// The same shape as the esp32s3 sibling's main.cpp, which carries the
// arguments this file does not repeat: why the entry point is
// ac3probe::run() rather than main(), why the clock is measured instead of
// read from configuration, why the internal-SRAM figure is asked of the
// allocator rather than taken from `idf.py size`, and why there is a pause
// before the first line. What differs here is the part.
//
// One core, no FPU. The S3's file explains that CCOUNT is per-core and that a
// task migrating mid-decode would difference two unrelated counters; on a C3
// there is only one core, so that particular trap is gone - but the clock is
// still measured against esp_timer rather than trusted, for the other reason
// that file gives, which is that a stale sdkconfig or a failed PLL lock would
// otherwise silently scale every cycles-per-frame figure this target exists
// to produce.

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

std::uint64_t now_us() { return static_cast<std::uint64_t>(esp_timer_get_time()); }

// What the allocator has. The C3's internal SRAM is 400 KB against the S3's
// 512 KB of DIRAM, and unlike the S3's it is not split into byte-addressable
// and word-only halves - so the two totals the S3 reports separately are one
// number here, and the largest contiguous block is still worth reporting
// beside it for the reason that file gives: a single large allocation needs a
// run of free memory, not a total.
void report_internal_sram(const char* when) {
    constexpr std::uint32_t kInternal8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    std::printf("esp32c3.internal_free_bytes[%s]=%lu "
                "esp32c3.internal_largest_block_bytes[%s]=%lu\n",
                when, static_cast<unsigned long>(heap_caps_get_free_size(kInternal8Bit)), when,
                static_cast<unsigned long>(heap_caps_get_largest_free_block(kInternal8Bit)));
}

}  // namespace ac3probe

namespace {

// The CPU clock, measured. esp_cpu_get_cycle_count() reads mcycle on RV32;
// esp_rom_delay_us() busy-waits rather than yielding.
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
    vTaskDelay(pdMS_TO_TICKS(3000));

    std::printf("target=esp32c3 cpu_mhz=%lu\n",
                static_cast<unsigned long>(measure_cpu_mhz()));

    ac3probe::report_internal_sram("before");

    ac3probe::run();

    ac3probe::report_internal_sram("after");

    std::printf("esp32c3.main_task_stack_free_bytes=%lu\n",
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    vTaskDelay(pdMS_TO_TICKS(200));
}
