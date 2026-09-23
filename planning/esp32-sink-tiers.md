# ESP32 sink tiers: C6 / C61 / S3 / P4 into one ES9080 TDM chain

**Status, 2026-09-21:** proposed study. Complementary to the shipped S3 sink and the
planned C6 sink — not a replacement for either. The 2026-09-08 close of the ESP32-P4 as a
decoder target ([`docs/platforms/bare-metal/esp32-c3.md`](../docs/platforms/bare-metal/esp32-c3.md#why-not-the-esp32-p4))
still holds for *replacing* the S3 Wi-Fi Sendspin product; this page reopens P4 only as the
**best** tier of a shared TDM sink family.

**Update, 2026-09-23:** added a fourth tier, **C61**, between C6 and S3. Prompted by a question
about whether a PSRAM-equipped C6 board could unlock 5.1 — it can't, on any board, ever (the C6
die has no PSRAM controller; see [ESP32-C6 → Status](../docs/platforms/bare-metal/esp32-c6.md)
and "Why C61 is proposed" below). C6 drops from **good** to **OK**: its ceiling is relabelled,
not its capability — nothing about the shipped C6 sink changed. **Good** now names C61, a real,
shipping, PSRAM-capable chip, proposed the same way P4 was: unmeasured, gated, exit criteria
written down before hardware is on order.

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
| **Best** | ESP32-P4 (+ Ethernet and/or C6 via `esp_hosted`) | **9.1.6** with **full** Annex E modes and combinations (desired; measurement gate) | **Both** ES9080s | **One** I2S controller, **512-bit** frame (`SOC_I2S_TDM_FULL_DATA_WIDTH`) → **16×32-bit** to both DACs |

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
([`sink_plan.hpp`](../esp-idf/ac3forge/include/ac3forge/sink_plan.hpp)), same Sendspin /
Improv / page surface where the part allows. The tier is a **module choice on one PCB**,
not four products.

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
160 MHz ceiling, Wi-Fi 6 + BLE — but with real hardware PSRAM this time
(`SOC_SPIRAM_SUPPORTED=1`, `components/esp_psram/esp32c61/Kconfig.spiram` in the pinned v6.1
tree; quad mode, up to 120 MHz). Confirmed against the C6 die's own total absence of that
capability — `SOC_SPIRAM_SUPPORTED` is undefined for `esp32c6` throughout ESP-IDF, the official
datasheet has no PSRAM in its external-memory section, and
[espressif/esp-idf#11193](https://github.com/espressif/esp-idf/issues/11193) has an Espressif
engineer (igrr) stating the C-series die has no hardware path to map external RAM into the CPU's
address space at all.

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

## What "best" must prove

### Exit criteria (board)

1. **Probe** — float tier: fourteen fixtures + stream-set `714-*` and objects-render rows in
   real time; PCM levels/hashes as on the S3 float path.
2. **Player, tools** — `714-aht`, `714-all`, `714-ecpl` onto **12 and 16 slots** with zero
   underruns over ≥10 minutes (null/capture first; then TDM to the ES9080 pair).
3. **TDM** — **16 × 32-bit** opens on **one** P4 I2S controller into **both** DACs; fixed-frame
   mode matches each ES9080's I2C setup; per-slot levels match the host decode.
4. **9.1.6** — sixteen-slot layout plays as coded (or with objects placed) without underrun
   when the stream and tools are in the supported set.
5. **Network shapes** (same decode/TDM exit, measure both if both are product bets):
   - **P4 + Ethernet** — appliance / PoE; isolates decode and TDM from radio tax.
   - **P4 + C6 (`esp_hosted` / SDIO)** — Wi-Fi groups like S3/C6; report SDIO + remote Wi-Fi
     cost against the same streams.
6. **Memory** — least free internal heap during play, compared to the S3's ~1 KB margin.
7. **Go / no-go** — ship as the **best** module for the shared PCB, or leave P4 closed again
   with numbers.

### Non-goals

- Replacing the S3 as the default Wi-Fi Sendspin sink.
- Expecting float PIE SIMD (still integer-only; same as S3).
- Asking the C6 alone to carry more than 2.0 over Sendspin — it structurally can't, on any
  board, because this die has no PSRAM (see "Why C61 is proposed" above). 5.1 is C61's ceiling
  to chase, not C6's.
- Diverting from C6 Sendspin (**OK**, shipped), C61 bring-up (**good**, proposed) or S3
  dual-DAC exits (**better**, shipped) — each tier keeps its own scope.

## Phasing

| Phase | Work | Size | Depends on |
|---|---|---|---|
| **P0** | This page + roadmap Proposed line; correct the old "P4 closed" wording to "closed as S3 replacement" | S | — |
| **C61-P1** | `esp32c61` probe under `apps/baremetal/platform/` + component target; board timing table (no network), mirroring the C6 probe | L | C61 board |
| **C61-P2** | Sendspin sink shape: `sdkconfig.sendspin-c61`, PSRAM-routed scratch, memory and DMA-queue measurement per "What 'good' must prove" | L | C61-P1 |
| **P1** | `esp32p4` probe under `apps/baremetal/platform/` + component target; board timing table (no network) | L | P4 board |
| **P2** | Ethernet player shape: stream set + tools rows onto 12/16 slots (capture, then TDM to one or both ES9080s) | L | P1; ES9080 hardware |
| **P3** | Hosted C6 Wi-Fi shape (optional if Ethernet-only "best" is enough) | L | P2; Function-EV or equivalent |
| **P4** | `hearth_sink` target + guide; advertise tier capabilities on the modular PCB | XL | P2 (and P3 if Wi-Fi best) |

**Recommended order:** Ethernet first (clean headroom), hosted Wi-Fi only if multi-room
groups on the best module matter. C61-P1/P2 run independently of the P4 track — whichever
board arrives first.

## Shared ES9080 contract (all tiers)

Already assumed by the sink planner and `hearth_sink` I2S sink:

- Each DAC programmed once over I2C for slot count, slot width, and channel map.
- MCU always presents that **fixed frame**; unused slots zeroed (`SinkFrame::fixed`).
- Stereo/mono plays do **not** fall back to standard I2S (would move BCLK under the PLL).

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

## Decisions to take after P1–P2 / C61-P1–P2

1. Is Ethernet-only enough for "best," or must hosted Wi-Fi match S3 group behaviour?
2. How the pair's I2C maps split 16×32 across two ES9080s (8+8 vs other).
3. Does enhanced coupling (and full tool combinations) at 9.1.6 / 7.1.4 clear on silicon, or
   is any combination still documented as out of reach?
4. C6 SKU: leave the second DAC unstuffed, or stuff and ignore?
5. Does C61's single 512-bit-frame controller really reach both ES9080s, or does it need two
   controllers like S3 once it's tried on real hardware?
