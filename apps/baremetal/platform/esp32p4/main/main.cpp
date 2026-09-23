// The probe's entry point and clock on ESP32-P4.
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
// and wraps every ~10.7 seconds at 400 MHz, and it is PER-CORE - a task that
// migrates between cores mid-decode would read two unrelated counters and
// produce nonsense. Microseconds times a known clock rate is the number that
// survives both. The clock rate is reported below so cycles can be recovered.
std::uint64_t now_us() { return static_cast<std::uint64_t>(esp_timer_get_time()); }

void report_internal_sram(const char* when) {
    // Byte-addressable internal SRAM (this part's 768 KB of L2MEM): what
    // plain malloc/operator new can return, and therefore the only pool the
    // decoder reaches today. Unlike the S3 page's report_internal_sram, this
    // does not split out 32-bit-only donated IRAM - that split is a property
    // of the Xtensa IRAM/DRAM alias the S3 has, not measured here yet on this
    // part's memory map. Two fields, matching the C3 project's, until a board
    // run says a third is worth adding.
    constexpr std::uint32_t kInternal8Bit = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    std::printf("esp32p4.internal_free_bytes[%s]=%lu "
                "esp32p4.internal_largest_block_bytes[%s]=%lu\n",
                when,
                static_cast<unsigned long>(heap_caps_get_free_size(kInternal8Bit)),
                when,
                static_cast<unsigned long>(heap_caps_get_largest_free_block(kInternal8Bit)));
}

}  // namespace ac3probe

namespace {

// The CPU clock, measured rather than read from configuration.
//
// CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ says what the build ASKED for, which is not
// the same claim - see the S3 project's copy of this function for why. Same
// reasoning here: esp_rom_delay_us() busy-waits rather than yielding, since
// CCOUNT is per-core and a task that slept and resumed on the other core
// would difference two unrelated counters.
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
    // This board has no USB-UART bridge chip, but it does have two USB-C
    // connectors: one wired to the native USB-OTG peripheral (where the
    // ROM's download mode answers - esptool reports "USB mode: USB-OTG" -
    // and the only one silkscreened, "USB 2.0 OTG"), the other to
    // SOC_USB_SERIAL_JTAG_SUPPORTED, the lightweight controller the S3/C3/C6
    // boards use for their console. sdkconfig.defaults asks for the console
    // there (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG), and it works: confirmed on
    // a board 2026-09-23, a new composite device (VID_303A, PID_1001)
    // enumerates on the second connector the moment the chip boots, whether
    // or not the OTG connector is plugged in at all.
    //
    // Reading it needs `idf.py monitor` (or another tool that toggles
    // DTR/RTS on attach) rather than a bare pyserial open: a plain open,
    // however precisely timed against a separate reset, caught nothing on
    // this board every time it was tried, while idf_monitor's own
    // attach-triggered reset (rst:0x17, CHIP_USB_UART_RESET, distinct from
    // esptool's SW_CPU_RESET) came through complete on the first attempt.
    //
    // The delay itself is the same three seconds the S3 project's copy of
    // this comment gives for a native-USB console re-enumerating after reset,
    // for the same reason: a flat pause costs a measurement harness nothing
    // and is not conditional on which transport answers.
    vTaskDelay(pdMS_TO_TICKS(3000));

    std::printf("target=esp32p4 cpu_mhz=%lu\n",
                static_cast<unsigned long>(measure_cpu_mhz()));

    ac3probe::report_internal_sram("before");

    ac3probe::run();

    ac3probe::report_internal_sram("after");

    // The decode runs on THIS task - see sdkconfig.defaults for the stack
    // size and why. uxTaskGetStackHighWaterMark() reports the minimum free
    // the stack has ever had, so the configured size minus this is the
    // deepest the decode actually went.
    std::printf("esp32p4.main_task_stack_free_bytes=%lu\n",
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));

    // app_main returns into a FreeRTOS task that is then deleted; without this
    // the console output can be cut off by the task teardown before the
    // console's buffer drains. Nothing here is timing-sensitive, so a flat
    // delay is enough.
    vTaskDelay(pdMS_TO_TICKS(200));
}
