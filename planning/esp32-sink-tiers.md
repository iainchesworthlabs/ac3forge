# ESP32 sink tiers: C6 / S3 / P4 into one ES9080 TDM chain

**Status, 2026-09-23:** Phase P1 done. Complementary to the shipped S3 sink and the
planned C6 sink — not a replacement for either. The 2026-09-08 close of the ESP32-P4 as a
decoder target ([`docs/platforms/bare-metal/esp32-c3.md`](../docs/platforms/bare-metal/esp32-c3.md#why-not-the-esp32-p4))
still holds for *replacing* the S3 Wi-Fi Sendspin product; this page reopens P4 only as the
**best** tier of a shared TDM sink family. The probe now exists and is real time on every
fixture, no network, on a board:
[`docs/platforms/bare-metal/esp32-p4.md`](../docs/platforms/bare-metal/esp32-p4.md).

## Product shape

**Longer term:** one base PCB that accepts a **modular ESP32 board** (C6, S3, or P4 module)
and routes TDM to a **pair of ESS ES9080** DACs. Each DAC is programmed once over I2C for a
fixed frame (slot count, width, channel map). Which module is fitted decides how many DACs
are driven and at what width — same firmware family, same analogue front end.

| Tier | Module | Decode ceiling (desired) | DACs driven | I2S / TDM |
|---|---|---|---|---|
| **Good** | ESP32-C6 | **5.1** at best (fixed-point; Atmos bed OK; not objects / 7.1.4 tools) | **One** ES9080 | One controller, **128-bit** frame → 8×16-bit or 4×32-bit |
| **Better** | ESP32-S3 | **7.1.4** at best; **no enhanced coupling** (and networked AHT / cpl+spx+AHT still miss — see below) | **Both** ES9080s | **Both** I2S controllers; **128 bits each** → **8×16-bit per DAC** = **16 channels @ 16-bit** (8×32-bit total if both lines at 32-bit) |
| **Best** | ESP32-P4 (+ Ethernet and/or C6 via `esp_hosted`) | **9.1.6** with **full** Annex E modes and combinations (desired; measurement gate) | **Both** ES9080s | **One** I2S controller, **512-bit** frame (`SOC_I2S_TDM_FULL_DATA_WIDTH`) → **16×32-bit** to both DACs |

```
                    modular MCU (C6 | S3 | P4)
                            │
              ┌─────────────┼─────────────┐
              │ TDM data 0  │  TDM data 1 │   (C6: data 1 unused)
              ▼             ▼             │
         [ ES9080 A ]  [ ES9080 B ]  ←── I2C (slot map once)
              │             │
           8 outs        8 outs     (pair = up to 16 channels)
```

Same `hearth_sink` family, same `SinkFrame::fixed` ES9080 contract
([`sink_plan.hpp`](../esp-idf/ac3forge/include/ac3forge/sink_plan.hpp)), same Sendspin /
Improv / page surface where the part allows. The tier is a **module choice on one PCB**,
not three products.

### Why the wiring differs

- **C6** has one I2S controller and a 128-bit TDM ceiling — only enough for one DAC’s worth of
  slots at useful widths. Second DAC stays dark (or is not stuffed on a C6 SKU).
- **S3** has two controllers, each still capped at **128 bits/frame**. Driving both ES9080s
  needs **both** controllers (line 1 as slave from line 0’s clocks, as today). Sixteen
  channels are possible only as **16×16-bit** (8 per DAC). Sixteen channels at 32-bit are
  impossible on this part.
- **P4** lifts the per-controller frame to **512 bits**, so one controller can feed **both**
  DACs with **16×32-bit** (8×32 into each DAC’s half of the map, or whatever I2C map the pair
  uses). That is the quality path the S3 cannot take.

## Why the S3 is “better” but not “best”

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

Internal heap under Sendspin on the S3 is also thin (~1 KB free at play start). The P4’s
768 KB L2MEM is the other half of the “best” bet. Naive clock scaling (400 / 240 ≈ **1.67×**)
brings AHT and `714-all` inside a frame; **ecpl may still sit near or over 32 ms** — so best
is a **measurement gate**, not an assumption.

## What “best” must prove

### Exit criteria (board)

1. **Probe** — float tier: fourteen fixtures + stream-set `714-*` and objects-render rows in
   real time; PCM levels/hashes as on the S3 float path. **Done, 2026-09-23**: all fourteen
   fixtures and all eleven `714-*` stream-set files, every one correct and in real time, no
   network, on a board (`docs/platforms/bare-metal/esp32-p4.md`).
2. **Player, tools** — `714-aht`, `714-all`, `714-ecpl` onto **12 and 16 slots** with zero
   underruns over ≥10 minutes (null/capture first; then TDM to the ES9080 pair).
3. **TDM** — **16 × 32-bit** opens on **one** P4 I2S controller into **both** DACs; fixed-frame
   mode matches each ES9080’s I2C setup; per-slot levels match the host decode.
4. **9.1.6** — sixteen-slot layout plays as coded (or with objects placed) without underrun
   when the stream and tools are in the supported set.
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
     extra hardware needed. **Not yet done**: the actual player (`hearth_sink` for P4, streaming
     the same fixtures/stream-set over this link) and the cost report the criterion asks for -
     this was a standalone connectivity smoke test, not `hearth_sink` integration.
6. **Memory** — least free internal heap during play, compared to the S3’s ~1 KB margin.
7. **Go / no-go** — ship as the **best** module for the shared PCB, or leave P4 closed again
   with numbers.

### Non-goals

- Replacing the S3 as the default Wi-Fi Sendspin sink.
- Expecting float PIE SIMD (still integer-only; same as S3).
- Asking the C6 alone to carry 7.1.4 or objects (ceiling stays **5.1**).
- Diverting from C6 Sendspin (planning C3) or S3 dual-DAC exits — those remain **good** and
  **better**.

## Phasing

| Phase | Work | Size | Depends on | Status |
|---|---|---|---|---|
| **P0** | This page + roadmap Proposed line; correct the old “P4 closed” wording to “closed as S3 replacement” | S | — | Done |
| **P1** | `esp32p4` probe under `apps/baremetal/platform/` + component target; board timing table (no network) | L | P4 board | **Done, 2026-09-23** — real time on every fixture and every stream-set `714-*` file, at 360 MHz (this board's chip-revision ceiling, not the part's 400 MHz maximum) |
| **P2** | Ethernet player shape: stream set + tools rows onto 12/16 slots (capture, then TDM to one or both ES9080s) | L | P1; ES9080 hardware | **N/A on the DFRobot FireBeetle 2 (compact SKU)** — no Ethernet PHY on this board. Still the right path on a board that has one (e.g. the Function-EV board) |
| **P3** | Hosted C6 Wi-Fi shape | L | P1; the onboard C6 | **In progress.** Foundation done 2026-09-23: `esp_wifi_remote`/`esp_hosted` over SDIO join a real AP and get an IP address (see exit criterion 5). Not yet: `hearth_sink` built for P4 and streaming over this link, TDM to the ES9080 pair (not wired up yet) |
| **P4** | `hearth_sink` target + guide; advertise tier capabilities on the modular PCB | XL | P2 (and P3 if Wi-Fi best) | Not started |

**Recommended order, revised for this board:** no Ethernet PHY here, so Wi-Fi via the onboard C6
(P3) is the only network path on the DFRobot FireBeetle 2 — P2 stays the right choice for a board
that does have Ethernet (the ES9080 pair isn't wired up on either board yet, so P4's TDM/DAC exit
criteria are unreached regardless of which network shape gets there first).

## Shared ES9080 contract (all tiers)

Already assumed by the sink planner and `hearth_sink` I2S sink:

- Each DAC programmed once over I2C for slot count, slot width, and channel map.
- MCU always presents that **fixed frame**; unused slots zeroed (`SinkFrame::fixed`).
- Stereo/mono plays do **not** fall back to standard I2S (would move BCLK under the PLL).

| Tier | Practical wiring on the shared PCB |
|---|---|
| C6 | One TDM line → **ES9080 A only** (up to 8 ch @ 16-bit or 4 @ 32-bit) |
| S3 | Two TDM lines → **both** ES9080s; **16 ch @ 16-bit** (8 per DAC); both I2S controllers |
| P4 | One wide TDM controller → **both** ES9080s; **16 ch @ 32-bit** (512-bit frame) |

## Relationship to existing plans

| Plan | Relationship |
|---|---|
| [hearth-reference-player.md](hearth-reference-player.md) Chip B / C | S3 = better (in progress); C6 sink C3 = good (not started). This page adds the **best** (P4) module class. |
| [esp32-714-realtime.md](esp32-714-realtime.md) | Defines the S3 misses the P4 tier must clear. |
| [esp32-c3.md](../docs/platforms/bare-metal/esp32-c3.md) “Why not P4” | Still true for “replace S3”; superseded as a blanket close by this study. |

## Decisions to take after P1–P2

1. Is Ethernet-only enough for “best,” or must hosted Wi-Fi match S3 group behaviour?
2. How the pair’s I2C maps split 16×32 across two ES9080s (8+8 vs other).
3. Does enhanced coupling (and full tool combinations) at 9.1.6 / 7.1.4 clear on silicon, or
   is any combination still documented as out of reach?
4. C6 SKU: leave the second DAC unstuffed, or stuff and ignore?
