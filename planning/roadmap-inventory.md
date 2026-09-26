# Roadmap inventory (codebase reconciliation)

Working document for roadmap maintenance. **Not published to the documentation site.**

**Last reconciled: 2026-09-26.** The live status board is [ROADMAP.md](../ROADMAP.md); this file is
a reconciliation aid, not a second authority. When they disagree, trust ROADMAP and the product
index pages.

## How to use this file

1. Read [ROADMAP.md](../ROADMAP.md) for in-flight, partial, proposed and out-of-scope work.
2. Use this inventory when adding a roadmap row or trimming a shipped one — spot-check the tree,
   then update ROADMAP first.
3. Do not duplicate ROADMAP prose here; record **code anchors** and **doc accuracy** only.

## Reconciliation notes (2026-09-26)

| Theme | ROADMAP status | Key code anchors | Doc accuracy |
|---|---|---|---|
| Hearth desktop | In progress — window shipped; user guide open | `apps/hearth/ui/`, `apps/hearth/engine/`, `tests/hearth/` | `docs/hearth/index.md` accurate |
| Hearth network / Sendspin | Partial — app plays to groups; Music Assistant open | `apps/hearth/engine/group.cpp`, `src/sendspin/` | `docs/hearth/index.md` accurate |
| ESP32 sinks (S3, C6, P4 rev1) | Partial — firmware CI + OTA; TDM hardware open | `esp-idf/ac3forge/examples/hearth_sink/` | `docs/hearth/sink-firmware.md` updated for release pipeline |
| AC-4 library | Partial — decode/encode channel-based shipped; objects/ESP32 open | `src/ac4{,dec,enc,core}/`, `tests/ac4*/` | `docs/library/ac4.md`, `capabilities.md` accurate |
| AC-4 in applications | Partial — I1/I2 shipped; GUI/bindings/ESP32 open | `apps/cli/commands/ac4_*`, `apps/hearth/engine/ac4_stream.cpp` | `planning/ac4.md` § applications updated |
| AC-4 performance reporting | Proposed | No workloads in `tests/performance/` yet | `docs/performance-quality.md` notes gap |
| API freeze → v1.0.0 | In progress | `docs/library/api-stability.md`, `_ci-core.yml` `ABI_ENFORCE` | Accurate |
| Crucible cross-platform | Partial | `apps/crucible/`, macOS CI headless | Accurate |
| TrueHD (IM5) | In progress off `main` | `feature/truehd-atmos-support` branch | Accurate |

## Shipped — do not reopen as roadmap rows

These belong in [CHANGELOG.md](../CHANGELOG.md) and product index pages only:

- AC-4 inspector (`ac4::ac4`), container carriage (MP4/TS/fMP4), IEC 61937-14 bursts
- AC-4 PCM decode through 7.1.4 and object paths in the library; encoder through 5.1.4
- `ac3cli` AC-4 command surface (I1); Hearth channel-based AC-4 playback (I2)
- `hearth_sink` Sendspin player on ESP32-S3, C6 and P4 rev1 with network OTA

## Historical inventory

The 2026-09-17 rewrite (`feature/roadmap-simplify`, commit `a7d3bd56`) lives in git history.
It predates the Hearth window, ESP32 OTA, and AC-4 application integration on `main`.
