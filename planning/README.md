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

Two phase records stayed on the site because the reference pages cite them as evidence rather
than as plans: [the Crucible promotion record](../docs/crucible/promotion.md), which carries the
2026-09-05 hardware verification, and
[the Windows demo record](../docs/platforms/windows-demo.md).

Each page's own status block says what has landed. `tools/checks/check_doc_paths.py` resolves the
links here, and exempts the prose paths, since these pages name directories on purpose that the
tree does not have.
