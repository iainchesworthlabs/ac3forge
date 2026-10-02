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
# (ICLFORGE_EXAMPLE_I2S_SECOND_LINE stays meaningless here: i2s_wide never
# reads it, and sink_second_line_possible() is hardcoded false).
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ICLFORGE_EXAMPLE_SINK_I2S_WIDE=y

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

# The partition table is sdkconfig.defaults' own, partitions.csv, and so is
# CONFIG_ESPTOOLPY_FLASHSIZE_16MB, which matches this board. Its two 4 MiB
# application slots hold this build - 1.46 MB in September 2026, with
# espressif/esp_wifi_remote and espressif/esp_hosted in it where the S3 and C6
# link plain esp_wifi - with the same room the single 4 MB slot of the P4's
# own table used to give it.
#
# The bootloader starts at 0x2000 on this part rather than 0x0, so it has the
# 24,576 bytes below the partition table at 0x8000, where the S3's and C6's
# have 32,768. With rollback on (sdkconfig.defaults) it measured 23,424 bytes
# on 2026-09-24 at the Info log level, 1,152 short of the window. A later
# ESP-IDF that grows it past the window fails the build and says so;
# CONFIG_BOOTLOADER_LOG_LEVEL_WARN took it to 20,896 bytes in the same
# measurement. planning/esp32-ota.md has the table.

# Quad I/O to the flash. The AC-4 decoder's code and the tables it reads are
# larger than the caches, so every frame refills thousands of 64-byte lines
# from flash, 2 bits a clock in DIO and 4 in QIO. On 2026-10-02 the same
# decoder took 6 to 7 ms a frame less at 5.1 and 3 to 5 ms less at 2.0 with the
# flash in QIO, to the same PCM (docs/platforms/bare-metal/esp32-p4.md,
# "Flash mode"). The mode is the second stage bootloader's, so an update over
# the network, which replaces the application and keeps the bootloader on the
# board, leaves a board flashed before this line in DIO until it is flashed
# again over USB, bootloader included (`idf.py flash`). QIO adds 944 bytes to
# the bootloader, 24,368 in all and 208 under the window above. A bootloader
# that cannot set a flash chip's quad-enable bit says so and stays in DIO.
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y

# The I2S queue: 12 descriptors of 256 frames, 64 ms, where the example's default is 21 ms
# (sdkconfig.psram gives the S3's network sources the same). The sink's write returns when the
# frame is in the queue, so a queue that holds less than a frame takes to decode runs dry in
# every frame: a 5.1 A-SPX stream folded to 2.0, 35 ms to decode of its 42.7, took 13.5 s for
# 10.1 s of audio through the default queue, and the sink counted 236 underruns and 3.5 s of
# silence; through this one it counted 1 underrun of 11 ms, the first frame's (planning/ac4.md,
# D14e). The queue is internal RAM, 24 KB at 2.0 (two 32-bit slots); a wide TDM frame (16 slots)
# would be 196 KB, which the AC-4 decoder leaves no room for, so size it down with that
# layout (it does not start on this chip revision).
CONFIG_ICLFORGE_EXAMPLE_I2S_DMA_DESCRIPTORS=12
CONFIG_ICLFORGE_EXAMPLE_I2S_DMA_FRAMES=256

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

# Keep the low-power SRAM out of the heap. ESP-IDF adds the P4's 32 KB of RTC fast memory
# (the LP system's SRAM, 0x50108000 to 0x50110000) to the heap by default,
# CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP, whose help says it "does not have much
# performance impact". It is the allocator's last internal region: an allocation of up to
# 16 KB that main RAM cannot satisfy comes from it before PSRAM is tried. The HP cores reach it
# over the LP bus with no cache between, and slowly. Measured on this board with a loop
# that touches nothing else (docs/platforms/bare-metal/esp32-p4.md, "The low-power SRAM"):
# a load takes 174 cycles when the next depends on it and 195 in sequence, a store 173,
# against 6 to 7 from main RAM and 18 a word for a cold sequential read of PSRAM. The
# AC-4 decoder takes nearly all of main RAM (343 to 350 KB free when a play starts, 1 to
# 11 KB left at its lowest), so what it allocates after that spills, and the four
# frame-rate plays that were measured used 25 to 31 KB of the LP SRAM at the most.
# Whether a hot buffer was among that depended on how full main RAM was at the moment it
# was allocated: the 29.97 fps frame-rate converter's history and output vectors were,
# after one sequence of plays and not after another, and the converter took 94 ms a frame
# where it takes 12 with them in PSRAM (planning/ac4.md, D14a6). Without the region the
# same allocations go to PSRAM, behind the cache, and the heap is 32 KB smaller. Nothing
# here asks for MALLOC_CAP_RTCRAM, so this option is the only way a buffer gets into that
# memory.
CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n

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
# only in the sense ICLFORGE_EXAMPLE_I2S_BCLK_GPIO's own Kconfig help text
# means it: ordinary GPIOs, not a match to any particular DAC board. Chosen
# clear of every pin range this board is confirmed to use for something else:
# GPIO14-19 (SDIO to the onboard C6 - main/idf_component.yml, confirmed by a
# real AP join and DHCP lease this session), GPIO54 (the C6's reset line,
# espressif/esp_hosted's own default for a P4 host), and GPIO34-36 (this
# part's boot-mode strapping group - GPIO35 is this board's own BOOT button,
# docs/platforms/bare-metal/esp32-p4.md's "Reading the console"). I2S goes
# through the GPIO matrix, so any free pin works; whether these three
# specific ones are free on the FireBeetle 2's exposed headers is a real
# board check still to do. It is not the standard-I2S (1-2 channel) path,
# though: that one is confirmed open and playing on these three pins
# (sink/i2s_wide/audio_sink.cpp's own comment has why TDM mode - three
# channels or more - cannot be checked on this exact chip revision at all).
CONFIG_ICLFORGE_EXAMPLE_I2S_BCLK_GPIO=20
CONFIG_ICLFORGE_EXAMPLE_I2S_WS_GPIO=21
CONFIG_ICLFORGE_EXAMPLE_I2S_DOUT_GPIO=22
