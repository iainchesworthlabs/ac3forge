# Planning documents

Design proposals and phase plans, kept in the repository and **not published to the
documentation site**. They describe work that is proposed, partly done, or decided against, so
publishing them alongside the reference documentation invited reading a proposal as a feature.

What is on the site instead: [the documentation](../docs/index.md) describes what exists, and
[ROADMAP.md](../ROADMAP.md) lists candidate work.

| Page | What it is | State as of 2026-09-08 |
|---|---|---|
| [recasting.md](recasting.md) | Splitting one name over everything into three named products | Phases 1–5 in; Phase 6 partly; Phase 7 waits on driver signing |
| [topology.md](topology.md) | Source, transport and sink roles, and the transport between them | One decision taken, nothing built |
| [player-appliance.md](player-appliance.md) | A playback appliance, "Hearth" | Not started — no `apps/hearth` in the tree |
| [host-plugin.md](host-plugin.md) | Whether a DAW/NLE plugin is possible, and what shipping one takes | A study; nothing decided, no code |
| [qc-report.md](qc-report.md) | A delivery-shaped QC report file | Not started — `ac3cli qc` writes no report file |
| [arithmetic-tiers.md](arithmetic-tiers.md) | One implementation, three arithmetics (`double`, `float`, fixed point) and an encoder effort axis; the platform choice matrix; the fixed-point decoder for the ESP32-C3 | Two tiers shipping and gated; the effort axis's first level measured 2026-09-10; the fixed tier proposed, not started |
| [esp32-player.md](esp32-player.md) | The ESP32-S3 player: a component layer under the examples, and an ESPHome media player | Phases 1 and 2 built and QEMU-verified; Phase 0 and Phase 2's hardware half wait on the board |
| [esp32-device-ui.md](esp32-device-ui.md) | The player's web UI: a page the board serves from flash beside the REST API, and calls it | Built 2026-09-11; tested on the host and under QEMU, and its requests measured on a board. What it says about the output layout proposed 2026-09-11 |
| [esp32-stream-set.md](esp32-stream-set.md) | Streams the player can fetch over HTTP: 7.1.4 streams for a 7.1.4 output, and a range of layouts, codecs and coding tools, with the level each slot should get | Proposed 2026-09-11; the streams made and played under QEMU |
| [esp32-714-realtime.md](esp32-714-realtime.md) | 7.1.4 E-AC-3 in real time on the player: a stage-by-stage profile of the frame on a board, the options with what each saves, and the decisions | Built and verified on a board 2026-09-11; PR #657 |

Two phase records stayed on the site because the reference pages cite them as evidence rather
than as plans: [the Crucible promotion record](../docs/crucible/promotion.md), which carries the
2026-09-05 hardware verification, and
[the Windows demo record](../docs/platforms/windows-demo.md).

Each page's own status block says what has landed. `tools/checks/check_doc_paths.py` resolves the
links here, and exempts the prose paths, since these pages name directories on purpose that the
tree does not have.
