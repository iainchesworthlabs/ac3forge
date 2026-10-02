# Planning documents

Design proposals and phase plans, kept in the repository and **not published to the
documentation site**. They hold the *how* and *why* behind work that is proposed, partly done, or
decided against.

## How this relates to the roadmap

| Document | Role |
|---|---|
| [ROADMAP.md](../ROADMAP.md) | **Status board** — in-flight, partial, proposed, blocked, and out-of-scope work only. Plain-English names; no new numeric IDs. |
| [roadmap-inventory.md](roadmap-inventory.md) | Reconciliation of roadmap and planning claims against the tree as of 2026-09-17 (working record, not kept current). |
| `planning/*.md` (this folder) | **Design depth** — phases, decisions, exit criteria, measurements. Link from the roadmap; do not duplicate the status board. |
| Product `docs/*/index.md` | **What ships today** — status callouts and guides. |
| [CHANGELOG.md](../CHANGELOG.md) | **What shipped in each release**. |

When a plan lands, update the product index and CHANGELOG; trim the roadmap row; leave the plan as
a record or mark it superseded — see [SUPERSEDED.md](SUPERSEDED.md).

**Names.** These plans were written while the family was called AC3Forge, and they keep the names of
their time: `ac3cli`, `ac3gui`, `ac3hearth`, `ac3crucible`, `ac3forge`, the `ac3::` namespaces and
the `AC3FORGE_` variables. The family is ICL Forge now, and its programs are `forge`, `forge-gui`,
`hearth` and `crucible`; [Renamed](../docs/renamed.md) puts each old name beside its new one.

---

## Active plans

| Page | What it is | State (as of 2026-09-26 unless the row says otherwise) |
|---|---|---|
| [hearth-reference-player.md](hearth-reference-player.md) | Hearth desktop app (`ac3hearth`) and ESP32 Sendspin sinks (`hearth_sink`) | As of 2026-09-30: built, with the user guide and three hardware exits open; plays AC-4 (I2, I5), and the ESP32 sinks take no AC-4 in a group (I6, not built). See [ROADMAP.md](../ROADMAP.md) Hearth section. |
| [hearth-sendspin-extension.md](hearth-sendspin-extension.md) | Sendspin conformance, Music Assistant compatibility, `_ac3forge_player@v1` | As of 2026-09-30: built as written, and carries AC-4 since D11 (no ESP32 sink lists it); aiosendspin's client and server stand in for Music Assistant in CI, and no run has been made against Music Assistant |
| [esp32-ota.md](esp32-ota.md) | Firmware updates over the network for `hearth_sink` boards (S3, C6, P4): A/B slots, rollback, integrity checks, flash mode; firmware published by CI, and a user guide | As of 2026-09-30: O1 to O5, O8 and O9 built and merged; O6 (the P4's co-processor firmware) is a study and O7 (signed images) is not built, by the user's decision. See [Sink firmware](../docs/hearth/sink-firmware.md) |
| [recasting.md](recasting.md) | Library / Forge / Crucible family naming and docs | As of 2026-09-30: phases 1 to 6 largely built, phase 7 (driver signing) not started; its naming decisions were overtaken by N1 ([SUPERSEDED.md](SUPERSEDED.md)) |
| [ac4.md](ac4.md) | AC-4 in full: a decoder beside the inspector, an encoder, and both in the applications; the oracles, the phases and the decisions | As of 2026-09-30: G0, G1, D1–D11, E1–E10, I1–I5b and D14a–D14b (the ESP32-P4) merged; not built: I6 (ESP32 sinks), D14c (S3) and D14d (C6); DEE's licence ends 2026-11-06. N1 (program names and layout) was built on 2026-09-30 and 2026-10-01, with the owner's renames and S6 left: see [N1](ac4.md#n1-the-names). See its [state table](ac4.md#state-on-2026-09-30) |
| [eac3-programme-mixing-metadata.md](eac3-programme-mixing-metadata.md) | `mixmdate` reporting/API completeness and decode-time associated-service mixing, scoped alongside two sibling efforts (CLI `programmeN=` authoring, MPEG-TS `mainid`/`asvc`) | As of 2026-09-30: phases 1 and 2 built (#797: reporting in `ac3cli decode` and in Hearth's media information), phases 3 to 5 not: the C API and Python bindings, a GUI summary, and mixing an associated service into the main programme. See [ROADMAP.md](../ROADMAP.md#partial-tails-on-shipped-work) |
| [esp32-sink-tiers.md](esp32-sink-tiers.md) | C6 / C61 / S3 / P4 good·better·best modules on one dual-ES9080 PCB | As of 2026-09-30: P0, P1 and P3 built, P4 partly, P2 and both C61 phases not built. The P4 sink plays stereo; everything that needs TDM waits for a v3.x board and the DACs. See [ROADMAP.md](../ROADMAP.md#proposed) |

---

## Studies and framework (not started or decision-only)

| Page | What it is | Roadmap |
|---|---|---|
| [topology.md](topology.md) | Source, transport, sink roles; HLS/CMAF transport | Hearth sinks use Sendspin instead ([SUPERSEDED.md](SUPERSEDED.md)); none of the HLS/CMAF transport is built, and its frame still applies elsewhere |
| [host-plugin.md](host-plugin.md) | DAW/NLE metering/QC plugin feasibility | [Proposed — DAW/NLE host plugin](../ROADMAP.md#proposed) |
| [qc-report.md](qc-report.md) | Delivery-shaped QC report file | [Proposed — QC delivery report file](../ROADMAP.md#proposed) |
| [layout.md](layout.md) | The layout and names of `src/` (N1B): codecs as peers over a codec-blind base, three layouts, the migration stages and a prototype, with an [inventory](layout-inventory.md); built as L2 in the stages S0 to S6 and N1A (see [what the runs found](layout.md#what-the-runs-found-that-the-plan-did-not)) | [N1 in the AC-4 plan](ac4.md#n1-the-names) |

---

## Design records (Hearth UI)

| Page | What it is | State |
|---|---|---|
| [hearth-design.md](hearth-design.md) | Signed UI design mockups (A0); palette, page layout, Sendspin extension page | Signed off 2026-09-22, and built (the images are mockups, not screenshots of the app); superseded for capability claims by [docs/hearth/index.md](../docs/hearth/index.md) |

---

## Measurement and phase records (ESP32 / library)

Built work; kept for evidence. Current user-facing docs supersede these for setup and capability
claims.

| Page | What it is | State |
|---|---|---|
| [esp32-714-realtime.md](esp32-714-realtime.md) | 7.1.4 E-AC-3 real-time on ESP32-S3 | Built 2026-09-11 |
| [esp32-device-ui.md](esp32-device-ui.md) | Board web UI beside REST API | Built 2026-09-11, and extended in nine more pull requests to 2026-09-26 |
| [esp32-stream-set.md](esp32-stream-set.md) | The 38 AC-3 and E-AC-3 streams the player's `http` source is served | Built and in use: CI plays them under QEMU, and board measurements use them; Sendspin replaced the `http` source only for playing to a board |
| [esp32-player.md](esp32-player.md) | Component layer and ESPHome path | As of 2026-09-30: phases 0 to 2 built; phase 6 built as Hearth B3; phases 3 to 5 (ESPHome, upstream, the encode direction) not built; phase 7 replaced |
| [arithmetic-tiers.md](arithmetic-tiers.md) | Decode arithmetic tiers and platform matrix | As of 2026-09-30: three tiers built for AC-3 and E-AC-3 (the fixed-point tier on the C3 under QEMU only, and on a board on the C6); AC-4's decoder shares `double` and `float` through `src/arithmetic` (D14a) and decodes on an ESP32-P4 in real time at 2.0 only, 0.53 to 0.74 of real time (D14b); D14d adds its fixed-point tier, which does not fit a C6 beside WiFi |

---

## Superseded records

| Page | Superseded by |
|---|---|
| [player-appliance.md](player-appliance.md) | [hearth-reference-player.md](hearth-reference-player.md) |

Full index: [SUPERSEDED.md](SUPERSEDED.md).

---

## Meta

| Page | What it is |
|---|---|
| [roadmap-inventory.md](roadmap-inventory.md) | A snapshot of 2026-09-17, made for the roadmap rewrite and not kept current; its status block lists the rows the tree has since overtaken |

---

## On the published site

Two phase records live under `docs/` because reference pages cite them as evidence, not as plans:

- [Crucible promotion record](../docs/crucible/design/promotion.md)
- [Windows demo record](../docs/platforms/windows-demo.md)

Each page's status block says what has landed. `tools/checks/check_doc_paths.py` resolves the
links here and exempts prose paths that name directories the tree does not have yet.
