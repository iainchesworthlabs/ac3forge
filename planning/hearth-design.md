# Hearth: the desktop app's design (A0)

The record for [A0](hearth-reference-player.md#a0-design-rounds), the first phase of
[chip A](hearth-reference-player.md#chip-a-the-desktop-app). A design canvas, built with the
`design` skill, drew an artboard for each part of `ac3hearth` from the family's existing QML
components (Theme, Card, RailBlock, SegmentedControl, FocusRing). The plan's own condition was
that no QML is written before this is signed off; this page is that sign-off.

## The canvas

Published 2026-09-15: [Hearth Desktop Design](https://claude.ai/artifact/C4izzSr755SyBzTtExt8Qg).
20 artboards over six pages:

| Page | Artboards |
|---|---|
| Player | the main window at a typical size and at its minimum, the output picker, first run |
| Speakers and decoder | speaker layout/routing/levels/delays/bass, the AC-3/E-AC-3 decoder, the AC-4 decoder in its inactive state |
| Media information | AC-3, E-AC-3 JOC in MP4, AC-4 |
| Network | discovery and pairing, a sink in use elsewhere, editing a group, a sink's own speakers and decoder, a group's reported levels |
| Settings and dialogs | Settings, About, Licences |
| Components | control states and keyboard focus, light and dark |

Every control is drawn in both light and dark, with keyboard focus states shown on the
Components page. The per-output meters, loudness and object view that the plan calls "the
monitor" are folded into the Play page rather than given their own artboard, since they are
what the window shows while something plays.

## Round 1

Reviewed with the user over 2026-09-20 to 2026-09-22. Before asking for a decision, the design
was checked against [A0](hearth-reference-player.md#a0-design-rounds)'s own list — the main
window, output picker, speaker setup, AC-3/E-AC-3 and AC-4 decoder settings, the monitor, media
information for AC-3/E-AC-3 JOC/AC-4, the network pages, settings, first run, and About with
licences — and found complete.

One question came up during review: whether the Decoder and Media pages were actually part of
the published canvas, since a first look at it did not show them. They were: the artifact's own
saved content was read directly and its embedded `canvas.json` matched the working copy exactly,
20 artboards on both sides. The canvas groups artboards into the six pages above at overlapping
coordinates, switched with a page control rather than laid out on one continuous scroll, which is
what made them easy to miss on a first pass. No artboard was missing and none was redrawn.

No changes were requested. Round 1 stood as published; round 2 did not run.

## Sign-off

The user, 2026-09-22:

> design considered signed off

That is A0's exit. [A5](hearth-reference-player.md#a5-the-application) starts from here, building
`apps/hearth/ui/` to this design.

## The artboards

Exported from the canvas the same day as sign-off, so what is below is what was approved.

### Player

**The main window, playing** — the queue, now playing, per-output levels, BS.1770-4 loudness,
this frame's metadata, the object placement view and the signal path from decode through render
to the output device.

![The main window at 1280x800, playing an E-AC-3 JOC file with a 5.1.2 render, object placement view and signal path panel](../docs/hearth/design/screenshots/main-play.png)

![The same window in dark](../docs/hearth/design/screenshots/main-play-dark.png)

**The main window at its minimum size**, 960x620.

![The main window at 960x620 with an empty second column](../docs/hearth/design/screenshots/play-minimum-size.png)

**The output picker** — local devices, passthrough devices, sinks, groups and standard Sendspin
players in one list.

![The output picker listing local, passthrough and network destinations](../docs/hearth/design/screenshots/output-picker.png)

**First run**, an empty queue with the three things a new user needs to know before anything
plays.

![The first-run dialog over an empty queue, explaining the default output, per-output setup and pairing](../docs/hearth/design/screenshots/first-run.png)

### Speakers and decoder

**Speaker setup** — layout, plan, routing grid, and per-output size/trim/delay/identify, with
bass management and crossover.

![The Speakers page: layout as text and diagram, an 8-output routing grid, and per-output levels/delays/bass](../docs/hearth/design/screenshots/speakers-setup.png)

![The same page in dark](../docs/hearth/design/screenshots/speakers-setup-dark.png)

**The AC-3 and E-AC-3 decoder**: mode, DRC, downmix and every other control the library already
has.

![Decoder settings for AC-3 and E-AC-3](../docs/hearth/design/screenshots/decoder-ac3-eac3.png)

**The AC-4 decoder, inactive**: every control the format will use, shown so the page is complete,
with the reason it cannot be used yet stated at the top and the two controls AC-4 shares with
AC-3/E-AC-3 left live.

![The AC-4 decoder page, marked "not in this build" with its presentation table, dialogue enhancement, dynamic range and downmix controls shown inactive](../docs/hearth/design/screenshots/decoder-ac4-inactive.png)

### Media information

**E-AC-3 JOC in MP4**, **AC-3**, and **AC-4** (not playable, shown for its metadata only).

![Media information for an E-AC-3 JOC stream in an MP4 container](../docs/hearth/design/screenshots/media-eac3-joc.png)

![Media information for an AC-3 stream](../docs/hearth/design/screenshots/media-ac3.png)

![Media information for an AC-4 stream, marked not playable](../docs/hearth/design/screenshots/media-ac4.png)

### Network

**Discovery and pairing**, **a sink already in use by another server**, **editing a group**,
**a Hearth sink's own speakers**, **a Hearth sink's own decoder**, and **a group's reported
levels beside the local decode**.

![Network discovery and pairing](../docs/hearth/design/screenshots/network-pairing.png)

![A sink shown in use by another server, with the explicit takeover action](../docs/hearth/design/screenshots/network-in-use.png)

![Editing a group: three members, per-member volume and mute, group volume, lead time and late chunks](../docs/hearth/design/screenshots/network-group.png)

![A Hearth sink's own speaker layout, routing and levels, set from this app](../docs/hearth/design/screenshots/network-sink-speakers.png)

![A Hearth sink's own decoder settings](../docs/hearth/design/screenshots/network-sink-decoder.png)

![A group's reported per-output levels shown beside this app's own local decode](../docs/hearth/design/screenshots/network-levels.png)

### Settings and dialogs

![Settings: playback, network, appearance (theme, palette, text size), language and diagnostics export](../docs/hearth/design/screenshots/settings.png)

![The About dialog](../docs/hearth/design/screenshots/about.png)

![The Licences dialog, listing the generated third-party notices](../docs/hearth/design/screenshots/licences.png)

### Components

Every control's rest, primary, disabled and keyboard-focus states, the four palettes' signal
range, and the block states media information uses, in both themes.

![Buttons, transport controls, segmented controls, checkboxes, fields, sliders, meters, list rows, the routing grid and status blocks, each in rest/chosen/focused/disabled states](../docs/hearth/design/screenshots/components.png)

![The same sheet in dark](../docs/hearth/design/screenshots/components-dark.png)
