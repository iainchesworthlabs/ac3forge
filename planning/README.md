# Planning documents

Design proposals and phase plans, kept in the repository and **not published to the
documentation site**. They hold the *how* and *why* behind work that is proposed, partly done, or
decided against.

## How this relates to the roadmap

| Document | Role |
|---|---|
| [ROADMAP.md](../ROADMAP.md) | **Status board** — in-flight, partial, proposed, blocked, and out-of-scope work only. Plain-English names; no new numeric IDs. |
| [roadmap-inventory.md](roadmap-inventory.md) | Reconciliation of roadmap and planning claims against the tree (working record). |
| `planning/*.md` (this folder) | **Design depth** — phases, decisions, exit criteria, measurements. Link from the roadmap; do not duplicate the status board. |
| Product `docs/*/index.md` | **What ships today** — status callouts and guides. |
| [CHANGELOG.md](../CHANGELOG.md) | **What shipped in each release**. |

When a plan lands, update the product index and CHANGELOG; trim the roadmap row; leave the plan as
a record or mark it superseded — see [SUPERSEDED.md](SUPERSEDED.md).

---

## Active plans

| Page | What it is | State as of 2026-09-17 |
|---|---|---|
| [hearth-reference-player.md](hearth-reference-player.md) | Hearth desktop app (`ac3hearth`) and ESP32 Sendspin sinks (`hearth_sink`) | Being built. Engine + S3 sink merged; **window (A5) not started**; Sendspin server not wired into engine yet. See [ROADMAP.md](../ROADMAP.md) Hearth section. |
| [hearth-sendspin-extension.md](hearth-sendspin-extension.md) | Sendspin conformance, Music Assistant compatibility, `_ac3forge_player@v1` | Draft for review |
| [recasting.md](recasting.md) | Library / Forge / Crucible family naming and docs | Phases 1–5 in; Phase 6 partly; Phase 7 waits on driver signing |
| [ac4.md](ac4.md) | AC-4 in full: a decoder beside the inspector, an encoder, and both in the applications; the oracles, the phases and the decisions | 2026-09-24: decisions taken; D1 merged (#700); G0, the DEE reference streams, and D2 next, before DEE's licence here ends on 2026-11-06 |

---

## Studies and framework (not started or decision-only)

| Page | What it is | Roadmap |
|---|---|---|
| [topology.md](topology.md) | Source, transport, sink roles; HLS/CMAF transport | Hearth sinks use Sendspin instead ([SUPERSEDED.md](SUPERSEDED.md)); HLS frame still applies elsewhere |
| [host-plugin.md](host-plugin.md) | DAW/NLE metering/QC plugin feasibility | [Proposed — DAW/NLE host plugin](../ROADMAP.md#proposed) |
| [qc-report.md](qc-report.md) | Delivery-shaped QC report file | [Proposed — QC delivery report file](../ROADMAP.md#proposed) |
| [eac3-programme-mixing-metadata.md](eac3-programme-mixing-metadata.md) | `mixmdate` reporting/API completeness and decode-time associated-service mixing | [Partial tails — Multi-programme E-AC-3 encode](../ROADMAP.md#partial-tails-on-shipped-work); scoped alongside two sibling efforts (CLI `programmeN=` authoring, MPEG-TS `mainid`/`asvc`) |
| [esp32-sink-tiers.md](esp32-sink-tiers.md) | C6 / S3 / P4 good·better·best modules on one dual-ES9080 PCB | [Proposed — ESP32 sink tiers](../ROADMAP.md#proposed) |

---

## Measurement and phase records (ESP32 / library)

Built work; kept for evidence. Current user-facing docs supersede these for setup and capability
claims.

| Page | What it is | State |
|---|---|---|
| [esp32-714-realtime.md](esp32-714-realtime.md) | 7.1.4 E-AC-3 real-time on ESP32-S3 | Built 2026-09-11 |
| [esp32-device-ui.md](esp32-device-ui.md) | Board web UI beside REST API | Built 2026-09-11 |
| [esp32-stream-set.md](esp32-stream-set.md) | HTTP stream set for old appliance transport | Streams made under QEMU; transport superseded |
| [esp32-player.md](esp32-player.md) | Component layer and ESPHome path | Phases 0–2 built; Sendspin sections superseded |
| [arithmetic-tiers.md](arithmetic-tiers.md) | Decode arithmetic tiers and platform matrix | Fixed-point tier shipping |

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
| [roadmap-inventory.md](roadmap-inventory.md) | Codebase-backed inventory for the 2026-09-17 roadmap rewrite |

---

## On the published site

Two phase records live under `docs/` because reference pages cite them as evidence, not as plans:

- [Crucible promotion record](../docs/crucible/design/promotion.md)
- [Windows demo record](../docs/platforms/windows-demo.md)

Each page's status block says what has landed. `tools/checks/check_doc_paths.py` resolves the
links here and exempts prose paths that name directories the tree does not have yet.
