// The probe's entry point and clock on ESP32-C6.
//
// The same shape as the esp32c3 sibling's main.cpp, which points at the
// esp32s3 one for the arguments all three share: why the entry point is
// ac3probe::run(), why the clock is measured, why the heap figures are asked of
// the allocator, and why there is a pause before the first line. What differs
// is the part, and the network load (network_load.hpp) the decode may share it
// with.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network_load.hpp"
#include "probe.hpp"

namespace ac3probe {

std::uint64_t now_us() { return static_cast<std::uint64_t>(esp_timer_get_time()); }

// What the allocator has. The C6's 512 KB of SRAM is byte-addressable
// throughout, as the C3's is, so there is one total to report and not the S3's
// two. The largest block matters for the reason the S3's file gives, and the
// minimum since boot is the lowest the heap has been: the decode's peaks and,
// in the WiFi load, the radio's and lwIP's buffers together, which is the
// headroom a player on this part would have left.
void report_internal_sram(const char* when) {
    constexpr std::uint32_t kInternal8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    std::printf("esp32c6.internal_free_bytes[%s]=%lu "
                "esp32c6.internal_largest_block_bytes[%s]=%lu "
                "esp32c6.internal_min_free_bytes[%s]=%lu\n",
                when, static_cast<unsigned long>(heap_caps_get_free_size(kInternal8Bit)), when,
                static_cast<unsigned long>(heap_caps_get_largest_free_block(kInternal8Bit)),
                when,
                static_cast<unsigned long>(heap_caps_get_minimum_free_size(kInternal8Bit)));
}

}  // namespace ac3probe

namespace {

// The CPU clock, measured. esp_cpu_get_cycle_count() reads the RISC-V cycle
// counter; esp_rom_delay_us() busy-waits rather than yielding. In the WiFi load
// an interrupt inside the window adds its cycles to both counts alike, so the
// figure is still the clock.
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

    std::printf("target=esp32c6 cpu_mhz=%lu\n",
                static_cast<unsigned long>(measure_cpu_mhz()));

    ac3probe::report_internal_sram("boot");

    if (!ac3probe::network_start()) {
        return;
    }

    ac3probe::report_internal_sram("before");

    ac3probe::run();

    ac3probe::report_internal_sram("after");

    ac3probe::network_report();

    std::printf("esp32c6.main_task_stack_free_bytes=%lu\n",
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    vTaskDelay(pdMS_TO_TICKS(200));
}
