# Overlay for an ESP32-P4: two 360 MHz RISC-V cores, each with an FPU, and no
# radio of its own - Wi-Fi reaches it over SDIO to an onboard ESP32-C6
# co-processor (main/idf_component.yml's espressif/esp_wifi_remote and
# espressif/esp_hosted), the two-chip solution ESP-IDF v6.1's own
# examples/wifi/getting_started/station already ships wired for this target.
#
#   idf.py -DIDF_TARGET=esp32p4 "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4" build
#
# One I2S controller reaches this part's whole channel target alone: its TDM
# frame holds 512 bits, sixteen 32-bit slots, in one line - see
# main/sink/i2s_wide/audio_sink.cpp for why that is a fork of sink/i2s rather
# than the second line the S3 and C6 overlays reach for
# (AC3FORGE_EXAMPLE_I2S_SECOND_LINE stays meaningless here: i2s_wide never
# reads it, and sink_second_line_possible() is hardcoded false).
CONFIG_IDF_TARGET="esp32p4"
CONFIG_AC3FORGE_EXAMPLE_SINK_I2S_WIDE=y

# The chip revision and clock. This board carries pre-production ESP32-P4
# silicon, revision v1.3 - a different hardware generation from ESP-IDF v6.1's
# v3.1+ default, and 400 MHz (v3.x's own ceiling, reached by a CPLL
# calibration this revision's boot asserts on) is not this part's number, 360
# MHz is. Both measured the hard way on this exact board - see
# docs/platforms/bare-metal/esp32-p4.md's "The chip revision" section for the
# full story (a masked bootloader refusal, then a 187-cycle boot-crash loop)
# before assuming either setting is optional here.
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y

# Light Sleep's clock-gating control, which this board never uses
# (CONFIG_PM_ENABLE is off) - but IDF's default still compiles it in and runs
# it at boot regardless, unconditionally reaching into a memory pool this
# build cannot spare. Found on this exact board 2026-09-23: the first
# hearth_sink boot for P4 hit `init function ... has failed (0x101),
# aborting` in sleep_clock_icg_startup_init, a boot-crash loop before
# app_main. 0x101 is ESP_ERR_NO_MEM. On pre-v3 P4 silicon
# (CONFIG_ESP32P4_SELECTS_REV_LESS_V3), the MALLOC_CAP_RETENTION pool this
# feature allocates from is not separate memory - components/heap/port/
# esp32p4/memory_layout.c carves it from the same low-DRAM range the app's
# own .data/.bss reserves out of, so a bigger image leaves less of it free
# than a small one does. The apps/baremetal probe (small, no WiFi/FAT/SDMMC)
# never came close to exhausting it; hearth_sink's real footprint does. Fix
# is upstream of that arithmetic entirely: esp_pm's own Kconfig help says
# PM_SLEEP_CLK_ICG_ENABLE exists only to keep specific peripheral clocks
# running during Light Sleep, and components/esp_hw_support/port/esp32p4/
# CMakeLists.txt compiles pmu_sleep_clock_icg.c in only when this is set -
# so turning it off removes the file, the allocation, and the crash
# together, for a feature this build was never going to reach anyway.
CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n

# The audio and storage partitions need more room here than partitions.csv or
# partitions_c6.csv give - see partitions_p4.csv's own comment for why.
# CONFIG_ESPTOOLPY_FLASHSIZE_16MB is already sdkconfig.defaults' own setting
# and matches this board too, so it is not repeated here.
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_p4.csv"

# PSRAM, on: this board has 32 MB of it (docs/platforms/bare-metal/esp32-p4.md),
# unlike the minimum-footprint bare-metal probe that deliberately leaves it off
# to measure what fits in internal SRAM alone - hearth_sink has no such goal,
# and needs the room. Found on this exact board 2026-09-23, the first time
# hearth_sink booted for this target with PSRAM still off: FreeRTOS could not
# even create app_main's own task - `assert failed: esp_startup_start_app
# app_startup.c:83 (res == pdTRUE)`, a boot-crash loop before any of this
# example's own code (or its own Kconfig-sized buffers) ever got a chance to
# run. The S3 sink hits the same shape of pressure once WiFi, lwIP and the
# decoder are all up (see sdkconfig.psram's own measurement), and turning
# PSRAM on is the fix there too - but P4's Kconfig differs from the S3's: one
# PSRAM line mode only (SPIRAM_MODE_HEX, this part's only option) rather than
# a quad/octal choice, so unlike sdkconfig.psram this needs no mode/speed
# guess, just the same SPIRAM_USE_MALLOC integration.
# CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL and the DMA-queue/cache/hold-first-unit
# tuning sdkconfig.psram carries for the S3 are S3 board measurements, not
# reused here - this board's own numbers are still to be measured.
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y

# esp_hosted's own SDIO transport buffers (main/idf_component.yml) still
# reach for internal DMA-capable RAM by default even with PSRAM on -
# managed_components/espressif__esp_hosted/host/port/esp/freertos/src/
# port_esp_hosted_host_os.c's hosted_malloc_align() only tries PSRAM first
# when this is set. Needed on THIS board: found 2026-09-23, one board flash
# after the PSRAM fix above got hearth_sink past the previous crash and into
# `assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)` - the ~31
# blocks x 1536 bytes (CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE + this driver's own
# fixed minimum) that transport's own mempool asks for, still all from
# internal RAM, still failed. The component's own CHANGELOG.md names this
# option for exactly this target: "added ESP_HOSTED_MEMPOOL_PREFER_SPIRAM to
# allocate transport buffers from PSRAM (e.g. ESP32-P4), saving internal
# RAM; off by default" - and its Kconfig help says the same thing this
# board's boot log just proved: "preserves scarce internal RAM on targets
# where GDMA can reach PSRAM through cache (e.g. ESP32-P4...)". Falls back
# to internal RAM on its own if a PSRAM request ever fails, so this is safe
# to leave on rather than a workaround to revisit.
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y

# THE WIDE LINE'S PINS. Not yet checked against the FireBeetle 2's own
# silkscreened header - unlike the SDIO pins below, which this board's own
# esp_hosted Wi-Fi join already exercised - so these are placeholder-safe
# only in the sense AC3FORGE_EXAMPLE_I2S_BCLK_GPIO's own Kconfig help text
# means it: ordinary GPIOs, not a match to any particular DAC board. Chosen
# clear of every pin range this board is confirmed to use for something else:
# GPIO14-19 (SDIO to the onboard C6 - main/idf_component.yml, confirmed by a
# real AP join and DHCP lease this session), GPIO54 (the C6's reset line,
# espressif/esp_hosted's own default for a P4 host), and GPIO34-36 (this
# part's boot-mode strapping group - GPIO35 is this board's own BOOT button,
# docs/platforms/bare-metal/esp32-p4.md's "Reading the console"). I2S goes
# through the GPIO matrix, so any free pin works; whether these three
# specific ones are free on the FireBeetle 2's exposed headers is what the
# next board flash checks, the same "TDM opens, nothing connected to the
# pins yet" checkpoint the ESP32-C6 page used for its own first measurement.
CONFIG_AC3FORGE_EXAMPLE_I2S_BCLK_GPIO=20
CONFIG_AC3FORGE_EXAMPLE_I2S_WS_GPIO=21
CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT_GPIO=22
