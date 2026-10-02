# ESP32 sink tiers: C6 / C61 / S3 / P4 into one ES9080 TDM chain

**Status, 2026-09-30:** P0, P1 and P3 are built, P4 is partly built, and P2 and both C61
phases are not built.

- **P1 (the probe)** and **P3 (`hearth_sink` on the P4, with Wi-Fi over the onboard C6)** ran on
  a DFRobot FireBeetle 2 on 2026-09-23. The sink plays stereo in a paired Sendspin session. CI
  packages a firmware image for it (`hearth-sink-esp32p4-rev1`), and it takes updates over the
  network ([`esp32-ota.md`](esp32-ota.md)).
- **What is not met** is everything that needs TDM: this board's chip (revision v1.3) cannot open
  TDM above two channels, no ES9080 is wired to any board, and no firmware sets one up over I2C.
  Exit criteria 3 and 4 wait for a v3.x board and the DACs. Criterion 2 has no run of the three
  streams it names; the P4 page's AC-4 section has one E-AC-3 7.1.4 play, `714-walk`, at 0.38 of
  a frame.
- **P2** does not apply to that board, which has no Ethernet PHY.
- **C61** is not started: there is no C61 board, and the tree has no C61 probe or sink shape. The
  chip appears only in the chip-name tables of `ota.py` and the firmware code.
- **AC-4** decodes on the P4 behind `CONFIG_AC3FORGE_AC4` (D14b of [`ac4.md`](ac4.md)), in real
  time for 2.0 in the SIMPLE and A-SPX modes and for nothing wider. The C6 part of that phase
  (D14d) builds the decoder in the fixed-point tier, which does not fit the C6 beside WiFi; the
  S3 part (D14c) and AC-4 in a Sendspin group (I6) are not built.

The tier is complementary to the shipped S3 and C6 sinks, not a replacement for either. The
2026-09-08 close of the ESP32-P4 as a decoder target
([`docs/platforms/bare-metal/esp32-c3.md`](../docs/platforms/bare-metal/esp32-c3.md#why-not-the-esp32-p4))
still holds for *replacing* the S3 Wi-Fi Sendspin product; this page reopens P4 only as the
**best** tier of a shared TDM sink family. The probe is real time on every fixture, no network,
on a board: [`docs/platforms/bare-metal/esp32-p4.md`](../docs/platforms/bare-metal/esp32-p4.md).

**Update, 2026-09-23:** added a fourth tier, **C61**, between C6 and S3. Prompted by a question
about whether a PSRAM-equipped C6 board could unlock 5.1 — it can't, on any board, ever (the C6
die has no PSRAM controller; see [ESP32-C6 → Status](../docs/platforms/bare-metal/esp32-c6.md)
and "Why C61 is proposed" below). C6 drops from **good** to **OK**: its ceiling is relabelled,
not its capability — nothing about the shipped C6 sink changed. **Good** now names C61, a
shipping, PSRAM-capable chip, proposed the same way P4 was: unmeasured, gated, exit criteria
written down before hardware is on order. No C61 probe or sink shape has been added since.

## Product shape

**Longer term:** one base PCB that accepts a **modular ESP32 board** (C6, C61, S3, or P4
module) and routes TDM to a **pair of ESS ES9080** DACs. Each DAC is programmed once over I2C
for a fixed frame (slot count, width, channel map). Which module is fitted decides how many
DACs are driven and at what width — same firmware family, same analogue front end.

| Tier | Module | Decode ceiling | DACs driven | I2S / TDM |
|---|---|---|---|---|
| **OK** | ESP32-C6 | **2.0**, shipped (`sdkconfig.sendspin-c6`). 5.1 decodes in real time in isolation ([ESP32-C6 → Status](../docs/platforms/bare-metal/esp32-c6.md#status)) but the Sendspin sink can't carry it: this die has no PSRAM to move the ring/WebSocket/WiFi footprint off internal SRAM, so 2.0 is this tier's durable ceiling, not a gap waiting to close | **One** ES9080 | One controller, **128-bit** frame → 8×16-bit or 4×32-bit |
| **Good** | ESP32-C61 | **5.1** desired (PSRAM-backed; proposed 2026-09-23; no board yet — **measurement gate**, see below) | **One or both** ES9080s — unverified | One controller, **512-bit** frame (`I2S_LL_SLOT_FRAME_BIT_MAX`, ESP-IDF v6.1) → up to **16×32-bit**, the same ceiling as P4's controller, on a chip otherwise shaped like C6 |
| **Better** | ESP32-S3 | **7.1.4** at best; **no enhanced coupling** (and networked AHT / cpl+spx+AHT still miss — see below) | **Both** ES9080s | **Both** I2S controllers; **128 bits each** → **8×16-bit per DAC** = **16 channels @ 16-bit** (8×32-bit total if both lines at 32-bit) |
| **Best** | ESP32-P4 (+ Ethernet and/or C6 via `esp_hosted`) | **9.1.6** with **full** Annex E modes and combinations (desired; measurement gate) | **Both** ES9080s | **One** I2S controller, **512-bit** frame (`I2S_LL_SLOT_FRAME_BIT_MAX`, `components/esp_hal_i2s/esp32p4/include/hal/i2s_ll.h`, ESP-IDF v6.1) → **16×32-bit** to both DACs; on this board's v1.3 silicon only standard I2S, one or two channels, opens (see "As built" below) |

```
                    modular MCU (C6 | C61 | S3 | P4)
                            │
              ┌─────────────┼─────────────┐
              │ TDM data 0  │  TDM data 1 │   (C6/C61: data 1 unused unless both DACs confirmed)
              ▼             ▼             │
         [ ES9080 A ]  [ ES9080 B ]  ←── I2C (slot map once)
              │             │
           8 outs        8 outs     (pair = up to 16 channels)
```

Same `hearth_sink` family, same `SinkFrame::fixed` ES9080 contract
([`sink_plan.hpp`](../esp-idf/iclforge/include/iclforge/sink_plan.hpp)), same Sendspin /
Improv / page surface where the part allows. The tier is a **module choice on one PCB**,
not four products.

**As built, 2026-09-30.** The firmware family exists for three of the four modules; the PCB and
the DACs do not.

- **C6 (OK):** as designed. `hearth_sink` plays stereo with a 2.0 ceiling, and CI packages an image
  for a C6 with 4 MB of flash and one for 16 MB ([An ESP32-C6 sink](../docs/hearth/sink-esp32-c6.md)).
- **C61 (good):** not started.
- **S3 (better):** shipped. The `i2s` sink opens standard I2S for one or two channels and TDM
  from three, sized by `sink_plan.hpp`, and takes a second I2S line as a slave of the first
  (`CONFIG_AC3FORGE_EXAMPLE_I2S_SECOND_LINE`). It has run with no DAC behind it: whether the two
  lines stay sample-aligned into two DACs is unchecked
  ([An ESP32-S3 sink](../docs/hearth/sink-esp32-s3.md#wiring)).
- **P4 (best):** `hearth_sink` builds and runs on the P4 with the `i2s_wide` sink, one controller
  at the 512-bit frame (`sdkconfig.p4`). On the FireBeetle 2 it opens standard I2S and nothing
  wider. Its revision v1.3 chip has no PLL clock source for I2S, and the audio PLL it falls back
  to is too slow for a 512-bit frame; a v3.0 or newer chip has the 160 MHz PLL that clears it
  ([the example's README](../esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-p4)).
  `GET /hardware` says so on such a board ([the device page's plan](esp32-device-ui.md#what-hardware-adds)).
- **The ES9080 pair:** no PCB exists, no ES9080 is wired to any board, and no firmware programs
  one over I2C. What `SinkFrame::fixed` gives such a DAC, a fixed frame with the unused slots
  zeroed, is tested on the host (`tests/ac3/io/test_sink_plan.cpp`) and not against a DAC.

### Why the wiring differs

- **C6** has one I2S controller and a 128-bit TDM ceiling — only enough for one DAC's worth of
  slots at useful widths. Second DAC stays dark (or is not stuffed on a C6 SKU).
- **C61** has the *same* one-controller topology as C6 — identical `I2SO_`/`I2SI_` signal set in
  ESP-IDF's `gpio_sig_map.h`, right down to the same second `I2SO_SD1_OUT` data line — but a
  **512-bit** frame ceiling (`I2S_LL_SLOT_FRAME_BIT_MAX`,
  `components/esp_hal_i2s/esp32c61/include/hal/i2s_ll.h`, ESP-IDF v6.1), not C6's 128 bits. That
  is P4's number, not C6's. Read from the register headers only: nobody has opened a TDM channel
  this wide on a C61 board yet, so whether it actually reaches both ES9080s the way P4's
  controller does, or hits some other limit first (I2C, DMA, power), is unverified. A proposed
  reading, not a claim.
- **S3** has two controllers, each still capped at **128 bits/frame**. Driving both ES9080s
  needs **both** controllers (line 1 as slave from line 0's clocks, as today). Sixteen
  channels are possible only as **16×16-bit** (8 per DAC). Sixteen channels at 32-bit are
  impossible on this part.
- **P4** lifts the per-controller frame to **512 bits**, so one controller can feed **both**
  DACs with **16×32-bit** (8×32 into each DAC's half of the map, or whatever I2C map the pair
  uses). That is the quality path the S3 cannot take — until C61 measures whether it can too.

## Why C61 is proposed as "good", between C6 and S3

Espressif's fix for the C-series' PSRAM gap is a new chip, not a C6 revision: the **ESP32-C61**,
in mass production since June 2025
([Espressif announcement](https://www.espressif.com/en/news/ESP32-C61_SoC),
[mass-production notice](https://www.espressif.com/en/news/C61_Mass_Production)). Single-core
RISC-V like C6, no FPU (`SOC_CPU_HAS_FPU` unset, same as C6 — the fixed-point tier applies), a
160 MHz ceiling, Wi-Fi 6 + BLE — but with hardware PSRAM this time
(`SOC_SPIRAM_SUPPORTED=1`, `components/esp_psram/esp32c61/Kconfig.spiram` in the pinned v6.1
tree; quad mode, up to 120 MHz). Confirmed against the C6 die's own total absence of that
capability — `SOC_SPIRAM_SUPPORTED` is undefined for `esp32c6` throughout ESP-IDF, the official
datasheet has no PSRAM in its external-memory section, and
[espressif/esp-idf#11193](https://github.com/espressif/esp-idf/issues/11193) has an Espressif
engineer (igrr) stating that the C2, C3 and C6 have no hardware support for PSRAM, so it cannot
be mapped into the CPU's address space.

The trade: Espressif's own announcement gives C61 **320 KB** of internal SRAM against C6's
**512 KB** (+16 KB LP) — less fast RAM, made up for (if it works) by PSRAM the C6 structurally
can never have. That is not a strictly-better chip; it is a different memory shape, and this
repo already has a preview of what that shape costs: the S3's `sdkconfig.psram` history (a
decoder with state in PSRAM ran slower in bursts, and needed the DMA queue taken from 21 ms to
64 ms before nothing starved) is exactly the kind of cost C61 would need to re-measure for
itself. **No C61 board has been measured against Hearth's memory budget yet** — "good" is a
proposal, not a shipped tier.

### What "good" must prove, before it's real

Mirrors [What "best" must prove](#what-best-must-prove) below, at C61's scale:

1. **Probe** — fixed-point tier, no network: does 5.1 (bed and coupled) actually decode in real
   time on this core at 160 MHz? Expected similar to C6's own numbers, same core family, but
   unmeasured.
2. **Memory, no network** — with `CONFIG_SPIRAM` on, does the Sendspin ring/WebSocket/WiFi
   footprint fit C61's smaller 320 KB internal pool the way it fits C6's 512 KB today, or does
   the smaller internal pool make the *baseline* tighter before PSRAM even enters the picture?
3. **Memory, with WiFi and a stream** — repeat the measurement
   [`esp32-c6.md`](../docs/platforms/bare-metal/esp32-c6.md#memory) already ran for C6, on C61,
   with decoder scratch routed to PSRAM the way `sdkconfig.psram` routes it on S3.
4. **PSRAM latency** — does a 5.1 decode with part of its state in PSRAM still make a 32 ms
   frame budget under Wi-Fi jitter, or does it need the same DMA-queue-deepening
   `sdkconfig.psram` needed on S3?
5. **TDM** — does the 512-bit single controller actually reach both ES9080s (see "Why the
   wiring differs" above), which would make C61's *wiring* closer to P4's than to C6's despite
   sitting between them on decode and memory?
6. **Go / no-go** — raise `CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS` past 2 for this
   chip with numbers behind it, in a new `sdkconfig.sendspin-c61`, or report why not — the same
   rule `sdkconfig.sendspin-c6` already follows for C6.

Nothing above is measured. This section exists so the exit criteria are written down before the
board is on order, the same way P4's were.

## Why the S3 is "better" but not "best"

The bare-metal probe on the S3 clears every fixture in real time
([`esp32-s3.md`](../docs/platforms/bare-metal/esp32-s3.md)). The **networked player** does not,
once Annex E tools and wide layouts stack
([`esp32-714-realtime.md`](esp32-714-realtime.md#what-stays-out-of-reach)):

| Workload over Wi-Fi (post I-cache / hold-first-unit) | ~ms / 32 ms frame |
|---|---|
| 7.1.4 plain / spx / cpl / walk onto twelve slots | ~24–29 — OK |
| 7.1.4 + AHT | ~33–34 — miss |
| 7.1.4 + cpl + spx + AHT | ~37 — miss |
| 7.1.4 + enhanced coupling | ~56–60 — hard miss |

So the S3 product ceiling is stated as **7.1.4 without enhanced coupling** (and without the
AHT combinations that still underrun on Wi-Fi until further work). The P4 tier exists to push
past that to **9.1.6** and **full tool combinations**, plus 32-bit slots across both DACs.

Internal heap under Sendspin on the S3 is also thin (~1 KB free at play start). The P4's
768 KB L2MEM is the other half of the "best" bet. Naive clock scaling (400 / 240 ≈ **1.67×**)
brings AHT and `714-all` inside a frame; **ecpl may still sit near or over 32 ms** — so best
is a **measurement gate**, not an assumption.

**As built.** The P4 probe, with no network, decodes `714-aht` in 0.37 of a frame, `714-all` in
0.40 and `714-ecpl` in 0.70, at 360 MHz, which is this board's clock rather than the 400 MHz the
scaling above assumed ([ESP32-P4 → Stream set](../docs/platforms/bare-metal/esp32-p4.md#stream-set)).
Those are probe figures. Through the player over the hosted Wi-Fi, `714-walk` takes 0.38 of a
frame ([ESP32-P4 → AC-4](../docs/platforms/bare-metal/esp32-p4.md#what-it-decodes-in-real-time)),
and the three streams above have not been played through it.

## What "best" must prove

### Exit criteria (board)

1. **Probe** — float tier: fourteen fixtures + stream-set `714-*` and objects-render rows in
   real time; PCM levels/hashes as on the S3 float path. **Done, 2026-09-23**: all fourteen
   fixtures and all eleven `714-*` stream-set files, every one correct and in real time, no
   network, on a board (`docs/platforms/bare-metal/esp32-p4.md`).
2. **Player, tools** — `714-aht`, `714-all`, `714-ecpl` onto **12 and 16 slots** with zero
   underruns over ≥10 minutes (null/capture first; then TDM to the ES9080 pair).
   **Not met, 2026-09-30:** no run of these three streams through the P4's player is recorded, and
   the TDM half cannot be made on this board.
3. **TDM** — **16 × 32-bit** opens on **one** P4 I2S controller into **both** DACs; fixed-frame
   mode matches each ES9080's I2C setup; per-slot levels match the host decode.
   **Not met, 2026-09-30:** on this board's revision v1.3 chip, TDM opens for no layout of three
   channels or more, and no ES9080 is wired. It needs a v3.x board and the DACs.
4. **9.1.6** — sixteen-slot layout plays as coded (or with objects placed) without underrun
   when the stream and tools are in the supported set. **Not met, 2026-09-30:** it needs
   criterion 3.
5. **Network shapes** (same decode/TDM exit, measure both if both are product bets):
   - **P4 + Ethernet** — not applicable to the DFRobot FireBeetle 2 (compact SKU): no Ethernet
     PHY on this board.
   - **P4 + C6 (`esp_hosted` / SDIO)** — Wi-Fi groups like S3/C6; report SDIO + remote Wi-Fi
     cost against the same streams. **Foundation proven, 2026-09-23**: `esp_wifi_remote` +
     `esp_hosted` (both `espressif/`, gated `target in [esp32p4, esp32h2]` — already the default
     manifest in ESP-IDF v6.1's `examples/wifi/getting_started/station`) bring the onboard
     ESP32-C6-MINI-1 up over SDIO and join a real AP: `esp_wifi_init` → `wifi_init_sta finished`
     → associated → DHCP → IP address, cold boot to IP in ~6.8 s. This board's C6 is NOT one of
     the units affected by DFRobot's documented factory mis-flash (forum topic 400107) - no
     extra hardware needed. That was a standalone connectivity smoke test.
     **Built, 2026-09-23 (PR #941):** `hearth_sink` runs on the P4 over this link. A clean boot,
     a Wi-Fi join and a paired Sendspin play from `ac3hearth-testserver` played an Atmos E-AC-3
     fixture onto 2.0, 315 of 315 bursts with no underrun
     ([the example's README](../esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-p4)).
     **Not done:** a report of the SDIO and remote Wi-Fi cost against the same streams, and a
     group with an S3 or C6 board. The one comparison on record is the decoder time per frame
     through the player with the network up, against the probe with none
     ([ESP32-P4 → AC-4](../docs/platforms/bare-metal/esp32-p4.md#what-it-decodes-in-real-time)).
6. **Memory** — least free internal heap during play, compared to the S3’s ~1 KB margin.
   **Not met for AC-3 and E-AC-3, 2026-09-30:** neither the example's README nor the P4 page has
   a low-water figure for a play of either. The AC-4 plays do: 1 to 8 KB free at the least under
   ESP-IDF's default allocation policy, with the low-power SRAM out of the heap (1 to 11 at D14a6, 2 to 14 before)
   ([ESP32-P4 → AC-4](../docs/platforms/bare-metal/esp32-p4.md#decode-time-and-memory)).
7. **Go / no-go** — ship as the **best** module for the shared PCB, or leave P4 closed again
   with numbers. **Open, 2026-09-30.** The P4 ships as a stereo Wi-Fi sink image; whether it is
   the best module waits for criteria 2 to 4.

### Non-goals

- Replacing the S3 as the default Wi-Fi Sendspin sink.
- Expecting float PIE SIMD (still integer-only; same as S3).
- Asking the C6 alone to carry more than 2.0 over Sendspin — it structurally can't, on any
  board, because this die has no PSRAM (see "Why C61 is proposed" above). 5.1 is C61's ceiling
  to chase, not C6's.
- Diverting from C6 Sendspin (**OK**, shipped), C61 bring-up (**good**, proposed) or S3
  dual-DAC exits (**better**, shipped) — each tier keeps its own scope.

## Phasing

| Phase | Work | Size | Depends on | Status |
|---|---|---|---|---|
| **P0** | This page + roadmap Proposed line; correct the old “P4 closed” wording to “closed as S3 replacement” | S | — | Done |
| **C61-P1** | `esp32c61` probe under `apps/baremetal/platform/` + component target; board timing table (no network), mirroring the C6 probe | L | C61 board | Not started |
| **C61-P2** | Sendspin sink shape: `sdkconfig.sendspin-c61`, PSRAM-routed scratch, memory and DMA-queue measurement per "What 'good' must prove" | L | C61-P1 | Not started |
| **P1** | `esp32p4` probe under `apps/baremetal/platform/` + component target; board timing table (no network) | L | P4 board | **Done, 2026-09-23** — real time on every fixture and every stream-set `714-*` file, at 360 MHz (this board's chip-revision ceiling, not the part's 400 MHz maximum) |
| **P2** | Ethernet player shape: stream set + tools rows onto 12/16 slots (capture, then TDM to one or both ES9080s) | L | P1; ES9080 hardware | **N/A on the DFRobot FireBeetle 2 (compact SKU)** — no Ethernet PHY on this board. Still the right path on a board that has one (e.g. the Function-EV board) |
| **P3** | Hosted C6 Wi-Fi shape | L | P1; the onboard C6 | **Built, 2026-09-23 (PR #941).** `hearth_sink` for the P4 runs over `esp_wifi_remote`/`esp_hosted` on SDIO: a Wi-Fi join, mDNS and a paired Sendspin play onto 2.0 (exit criterion 5). TDM to the ES9080 pair is not (criterion 3) |
| **P4** | `hearth_sink` target + guide; advertise tier capabilities on the modular PCB | XL | P2 (and P3 if Wi-Fi best) | **Partly built.** The P4 target exists (`sdkconfig.p4`, the `i2s_wide` sink), CI packages it as `hearth-sink-esp32p4-rev1`, it takes updates over the network, and `GET /hardware` reports its capabilities and its limits. There is no P4 sink guide page (the S3 and C6 have one), and no modular PCB |

**Recommended order, revised for this board:** no Ethernet PHY here, so Wi-Fi via the onboard C6
(P3) is the only network path on the DFRobot FireBeetle 2 — P2 stays the right choice for a board
that does have Ethernet (the ES9080 pair isn't wired up on either board yet, so P4's TDM/DAC exit
criteria are unreached regardless of which network shape gets there first). C61-P1/P2 run
independently of this track entirely — whichever board arrives first. P3 was taken, as above.

## Shared ES9080 contract (all tiers)

Already assumed by the sink planner and `hearth_sink` I2S sink:

- Each DAC programmed once over I2C for slot count, slot width, and channel map.
- MCU always presents that **fixed frame**; unused slots zeroed (`SinkFrame::fixed`).
- Stereo/mono plays do **not** fall back to standard I2S (would move BCLK under the PLL).

As built, both of the last two are a switch, `CONFIG_AC3FORGE_EXAMPLE_I2S_FIXED_FRAME=1`
(PR #701), and the default is 0: one or two channels open standard I2S, which a stereo I2S DAC
needs. The I2C programming is not part of the example.

| Tier | Practical wiring on the shared PCB |
|---|---|
| C6 | One TDM line → **ES9080 A only** (up to 8 ch @ 16-bit or 4 @ 32-bit) |
| C61 | One controller, 512-bit frame → **both ES9080s, unverified** (up to 16 ch @ 32-bit if it reaches both the way P4's does — see "Why the wiring differs") |
| S3 | Two TDM lines → **both** ES9080s; **16 ch @ 16-bit** (8 per DAC); both I2S controllers |
| P4 | One wide TDM controller → **both** ES9080s; **16 ch @ 32-bit** (512-bit frame) |

## Relationship to existing plans

| Plan | Relationship |
|---|---|
| [hearth-reference-player.md](hearth-reference-player.md) Chip B / C | S3 = better (shipped); C6 sink C3 = **OK** (shipped, 2.0 ceiling). This page adds the **best** (P4) module class and proposes **good** (C61; not started, no board). |
| [esp32-714-realtime.md](esp32-714-realtime.md) | Defines the S3 misses the P4 tier must clear. |
| [esp32-c3.md](../docs/platforms/bare-metal/esp32-c3.md) "Why not P4" | Still true for "replace S3"; superseded as a blanket close by this study. |
| [esp32-ota.md](esp32-ota.md) | The firmware update path the P4 image, like the S3 and C6 images, takes. Built. |
| [ac4.md](ac4.md) D14b, D14c, D14d and I6 | AC-4 on each tier: the P4 (D14b) is built; the S3 (D14c), the C6 (D14d) and the ESP32 sinks taking AC-4 in a Sendspin group (I6) are not. |

## Decisions to take after P1–P2 / C61-P1–P2

Status, 2026-09-30, of each:

1. Is Ethernet-only enough for "best," or must hosted Wi-Fi match S3 group behaviour? **Open.**
   The FireBeetle 2 has no Ethernet PHY, and hosted Wi-Fi carries a paired Sendspin play to a
   single P4. No run of a group with a P4 in it is recorded.
2. How the pair's I2C maps split 16×32 across two ES9080s (8+8 vs other). **Open:** no DAC.
3. Does enhanced coupling (and full tool combinations) at 9.1.6 / 7.1.4 clear on silicon, or
   is any combination still documented as out of reach? **Open for the player.** The probe
   decodes `714-ecpl` in 0.70 of a frame; no layout wider than 2.0 has opened on the P4's I2S
   sink.
4. C6 SKU: leave the second DAC unstuffed, or stuff and ignore? **Open:** no PCB.
5. Does C61's single 512-bit-frame controller reach both ES9080s, or does it need two
   controllers like S3 once it's tried on hardware? **Open:** no C61 board.
