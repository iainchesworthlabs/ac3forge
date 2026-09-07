// The probe's entry point and clock on ESP32-S3.
//
// ESP-IDF calls app_main() rather than main(), which is why apps/baremetal
// exposes ac3probe::run() through probe.hpp instead of defining main() itself.
// Everything the probe checks - both codecs against fixture.hpp's levels, the
// allocation counts, the heap peak, the refusal of the direct-form transform -
// is the same code the host and arm-none-eabi shapes run. Nothing about the
// decode is special-cased here, and nothing should be.

#include <cstdint>
#include <cstdio>

#include "esp_cpu.h"
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

    ac3probe::run();

    // app_main returns into a FreeRTOS task that is then deleted; without this
    // the console output can be cut off by the task teardown before the UART
    // FIFO drains. Nothing here is timing-sensitive, so a flat delay is enough.
    vTaskDelay(pdMS_TO_TICKS(200));
}
