# Superseded planning records

Some pages in `planning/` describe work that was **decided against**, **replaced by a later plan**, or
**fully shipped** and folded into product docs. They stay in the tree as decision records; they are
not the place to look for current direction.

For **what is in flight today**, read [ROADMAP.md](../ROADMAP.md) first, then the active plan
linked from [planning/README.md](README.md).

## Replaced by another plan

| Record | Superseded on | Read instead |
|---|---|---|
| [player-appliance.md](player-appliance.md) | 2026-09-15 | [hearth-reference-player.md](hearth-reference-player.md) — daemon/web/kiosk/HLS form dropped; desktop app + Sendspin sinks |
| [topology.md](topology.md) decision 1 for Hearth sinks | 2026-09-15 | Sendspin + extension role in [hearth-reference-player.md](hearth-reference-player.md) and [hearth-sendspin-extension.md](hearth-sendspin-extension.md). HLS/CMAF remains the frame for **non-Hearth** sources and sinks. |
| [esp32-player.md](esp32-player.md) Sendspin phases | 2026-09-15 | [hearth-reference-player.md](hearth-reference-player.md) chips B–C; `hearth_sink` in tree |
| [esp32-stream-set.md](esp32-stream-set.md) HTTP source for playing to a board | 2026-09-15 | Sendspin bitstream carriage, since Hearth B3. The set itself is in use: the `http` source remains for `POST /play`, CI's checks under QEMU and board measurements |

## Shipped — detail moved to docs

| Record | Landed in | Notes |
|---|---|---|
| [esp32-714-realtime.md](esp32-714-realtime.md) | Platform pages, decoder profile | 7.1.4 real-time on ESP32-S3; PR #657 |
| [esp32-device-ui.md](esp32-device-ui.md) | `hearth_sink` web UI | Board status page beside REST API |
| [arithmetic-tiers.md](arithmetic-tiers.md) | `docs/library/`, bare-metal targets | Three tiers built for AC-3 and E-AC-3. The fixed-point tier ran on the host, the Cortex-M3 leg and an ESP32-C6, and on the C3 under QEMU only. AC-4's decoder shares the `double` and `float` tiers (D14a, D14b) and has no fixed-point tier |

## Historical planning sections inside active pages

| Location | Status |
|---|---|
| [recasting.md](recasting.md) § The roadmap | Written 2026-09-05 for the old nine-theme ID model. **Superseded** by the 2026-09-17 slim [ROADMAP.md](../ROADMAP.md). The page's phases 1 to 6 are largely built and phase 7 has not started; the names they use were overtaken (last row). |
| [player-appliance.md](player-appliance.md) § The roadmap entry | Proposed `PLn` ID for the old appliance form — **never adopted**; Hearth shipped under [hearth-reference-player.md](hearth-reference-player.md) instead. |
| [recasting.md](recasting.md) § Decisions 2, 3, 5 and 6 and the recommended scheme S1 (the library's identifiers, the family's spelling, package tokens and binary names stay) | **Superseded** by the user's decisions of 2026-09-25 to 2026-09-29 ([N1 in ac4.md](ac4.md#n1-the-names)): the programs take the names `forge`, `forge-gui`, `hearth` and `crucible`, and the family becomes ICL Forge (`iclforge`), with no old name kept. N1 has run: the tree and the docs use the new names, and that page keeps the old ones as a record. |

## Do not merge or delete

These files are cited from CHANGELOG entries, design records, and git history. Consolidation means
**clear pointers**, not removal. When editing a superseded page, add to its status block rather
than rewriting history.
