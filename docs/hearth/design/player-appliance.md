# The appliance plan

This is a pointer, not a copy. Design proposals live in the repository's `planning/` directory
and are deliberately not republished onto this site — see
[`planning/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/README.md)
for why: a proposal read alongside reference documentation is too easily read as a shipped
feature.

## What's decided

Written and decided 2026-09-07, then reframed the same day the project's source/transport/sink
topology landed. **Hearth is the sink**: the reference implementation of that role on a machine
with an operating system, plugged in next to a receiver, that plays a queue, follows what the
sink will accept, and exposes a local web control page — building on `ac3cli play`'s
already-proven passthrough logic and `PassthroughSink`, neither of which is a product on its own
today. All ten of the plan's open decisions were taken on 2026-09-07, four of them against the
document's own recommendation, including: all three desktop platforms (not Linux-only), both a
headless daemon and an optional kiosk window, and `cpp-httplib` for the HTTP layer.

## What's built

Nothing under this name. **There is no `apps/hearth` in the tree.** What exists and is
hardware-verified is the embedded player documented on [Hearth's own overview](../index.md) and
the [ESP32-S3 platform page](../../platforms/bare-metal/esp32-s3.md) — a different codebase that
shares the role, not the implementation.

## The full record

[`planning/player-appliance.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/player-appliance.md)
has the complete design: the name and why it was chosen, what's shared with
[Crucible](../../crucible/index.md) and what isn't, the scope (and what's deliberately excluded),
the six gaps in today's sink-following logic that stand between it and an appliance, the build
and packaging identities, the CI plan, and the ten decisions with their reasoning.

## Where to go next

- [Hearth overview](../index.md) — what's real today.
- [Crucible's own design record](../../crucible/design/promotion.md) — the closest precedent: a
  demo promoted to a product, with the same phase-by-phase shape.
- [Roadmap](../../roadmap.md) — where this sits against everything else planned.
