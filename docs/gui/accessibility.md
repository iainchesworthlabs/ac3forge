# Keyboard, text size and diagnostics

This page is what ac3gui offers a person who is not using a mouse, or who needs the window larger,
or who is writing a bug report. It also says what is still missing, because most of this landed in
one pass on 2026-09-06 and the window is large.

Crucible's equivalent page is [Keyboard and screen readers](../crucible/accessibility.md). The two
windows share `Theme.qml`, `SegmentedControl.qml` and `FocusRing.qml`, so the type scale, the
palettes and the focus ring are the same thing in both; the rest of this page is ac3gui's own.

## Text size

**Preferences → Appearance → Text size** is 100%, 125%, 150%, 175% or System, the same five choices
Crucible offers and stored under the same name, so a person who has set one is not surprised by the
other.

100% is the default and is the size the window is drawn at. System reads the point size the
platform's theme reports and counts 9 pt as 100%: 9 pt is the base size on Windows, and the
desktop's own Text size setting scales it, which is how that setting reaches ac3gui. Several Linux
desktops report 10 or 11 pt with nothing about text size touched, so System starts the window 11 to
22% larger there — pick a percentage if that is not what you want.

Every size in the window follows it. That is worth stating plainly because it was claimed before it
was true: `Theme.qml` said "a literal `pixelSize` in a view is a size that cannot follow the
person's text-size setting, so there are none", which was true of Crucible and had never been true
of ac3gui — on 2026-09-06 its QML held 377 literal type sizes (376 `font.pixelSize` values and
one `SegmentedControl.fontSize`) and there was no text-size setting at all, so the theme's
`fontScale` sat at 1.0 for the life of the process. All 377 now read from the scale.

Type size is only half of it. A control whose **height** is a fixed number grows its label inside a
box that does not move, and at 175% that clips. These grow with the setting instead — most by
taking their height from their own label, the two fixed lanes by multiplying the lane by the
scale:

- the bed and low-frequency chips on the Format tab;
- the tab bar and the runs strip (both are fixed lanes by design — the runs lane's cap is what
  stops a layout feedback loop — so their heights scale with the text rather than following a
  label);
- the command-line chip and the Encode button;
- every standard control (buttons, checkboxes, combo boxes, spin boxes), which size themselves.

These do not, and can clip at the larger sizes: the Guided wizard (twenty-one fixed heights), the
Objects and Live session tabs, the assignment table, the channel meters, and the QC, Inspect
objects, Stream player and About dialogs. Preferences holds one. They are on the list.

## What can be done without a mouse

The controls a person cannot avoid on the way to an encode are reachable with `Tab` and pressed
with `Space` or `Return`:

| Where | What Tab reaches |
| --- | --- |
| Header | The Guided / Advanced / Expert switch, and the QC, Inspect, Open stream, Preferences and About buttons. |
| Input rail | The File / Live capture switch, Choose WAV, Assign, and per source its own Remove and Start offset. |
| Format tab | The presets, the codec, rate and container controls, the bed chips, the low-frequency chips and the extras. |
| The tab bar | Format, Routing, Objects, Live session, Coding tools, Metadata — whichever the current tier shows. |
| Command bar | The `ac3cli` chip (which opens its popover, `Esc` closes it) and the Encode button. |
| Runs strip | Each run's own chip, which opens that run's details, and its Cancel, Play, Show in folder, Details and More… buttons. |

Inside a segmented control — the tier switch, the File / Live capture switch, the meters' Coded /
Rendered switch, and Preferences' Theme, Text size and Meters switches — `Left`, `Right`, `Home`
and `End` choose, and `Left` from the first wraps to the last. That behaviour is
`SegmentedControl.qml`'s and predates this pass.

Whatever has the keyboard draws a two-pixel ring just outside its own border. Clicking a chip does
not move the keyboard to it, so a mouse user sees no rings.

A control that cannot be pressed is not a tab stop either: object mode locks the bed and the
low-frequency count, and while it does, `Tab` steps over them rather than landing on something that
ignores the press.

## What a screen reader is told

Each control carries a role and a name. Where a control repeats — one Remove per source, one Cancel
or Play per run — the name says **which**: "Remove stems.wav", "Play run 4 to the receiver". A
column of a dozen identical "Remove"s tells a reader nothing, and that is what these were.

Where a control has a visible label to borrow, its name comes from the same data that label reads
rather than from a second copy typed beside it — every per-source and per-run control, the extras
checkboxes, the bed chips and the Guided wizard's four room questions. Two of those are held by a
test: `tst_keyboard.qml` reads the model row the first two extras checkboxes draw, and the choice
list a bed chip draws, and compares the accessible name against that rather than against a literal
typed into the test. That catches a name that has drifted from the data; it would not catch a
second copy that still happens to match it. The per-source controls, the per-run controls and the
wizard's four room questions are written the same way and checked by reading the code — a name
typed a second time in one of those would fail no test. A few controls have no visible label of
their own — the File / Live capture switch, the meters' Coded / Rendered switch — and carry a
short written name instead, which is the one case where the two can be edited apart.

Two kinds of control were silently unnamed before this pass and are not now. The extras
checkboxes, whose labels are Texts beside the box rather than the box's own `text`, take their
name and their channel tokens from the same model row the visible label reads. And eight of the
window's fourteen segmented control groups had no name at all — a reader announced an unnamed
group of radio buttons, with the segments named and the question they answer not. All fourteen
carry one now: the tier switch, the File / Live capture switch, the meters' Coded / Rendered
switch and the object-motion switch were named in this pass, and the Guided wizard's four room
questions bind theirs to the label in the column beside them, which costs no second copy of the
text. `tst_keyboard.qml` asserts that the six reachable from the window itself are named and that
no two share a name.

## Saving a diagnostics file

**Preferences → Diagnostics → Save diagnostics…** writes a plain-text file wherever you choose.
Nothing is sent anywhere; the file is yours to read before you attach it to anything.

It carries the versions, this machine's platform and Qt build, what is loaded, the encode plan, the
settings in force, the run history and the last errors, followed by the recent messages the window
logged.

It does not carry:

- the signing key, in any form. The report says whether `AC3FORGE_SIGNING_KEY_FILE` and
  `AC3FORGE_SIGNING_KEY` are set and never what they hold, and any setting under `signing/` is
  written as `<withheld>` whatever value reached the renderer;
- the value of any other environment variable — the environment is never enumerated;
- one sample of audio, or any part of a file you loaded.

It does name your machine and the folder you write into. The platform section carries the OS name
and version, the kernel, the CPU architecture, the Qt version, the QPA plugin and the system
locale, and how many capture and output devices this machine offers — counts, not device names.
The settings section carries twenty-two `workbench/` keys as they stand, and two of them hold text
of your own rather than a choice from a fixed list: `outputFolder`, the folder runs are written
into, printed as the `file:///…` URL the folder picker returned, which on most machines contains
your account name (a store that has never held that key reads `(default)` instead); and
`namePattern`, the naming rule saves follow. The plan carries the output file's base name; each
run in the history is listed by number, base name and status, with its rate, duration and any
failure text; and the recent messages are the window's own status lines. Read the file before you
attach it to an issue.

Sources are named the way the input rail names them, by base name and shape rather than by the
folder they were opened from. The status messages the log carries name files the same way today —
"Could not read stems.wav: …", "Wrote 212 frames … to take.ec3" — but nothing enforces that for a
message somebody adds later, which is the other reason the file is written where you choose,
yours to read, rather than sent.

The rule lives in `apps/gui/gui_diagnostics.hpp` and is held in two places: structurally, because
the report is composed from named fields and there is no field for a key or for sample data; and by
a final pass that replaces every spelling of the two signing values with `<withheld>`.
`tests/gui/test_gui_diagnostics.cpp` holds it on every CI leg, including the ones that build no
window, and `tst_diagnostics.qml` holds the other half — that the controller fills those fields
from what the window is actually showing.

## Colour and contrast

The palettes are `Theme.qml`'s, shared with Crucible and checked by the test suite rather than by
eye. The measured numbers, including the one that does not reach 4.5:1, are on
[Crucible's page](../crucible/accessibility.md#colour-and-contrast); they apply here unchanged
because they are the same tokens.

## What is still mouse-only

Reachable by mouse, and not yet by keyboard:

- **the Guided wizard's hand-drawn press targets** — the step dots, the source cards, the rate
  cards and the destination cards, none of which is a tab stop. Its Back and Next buttons, its
  four room questions and the standard controls between them are reachable, so a person can walk
  the steps; the cards they would choose with on the way are the gap;
- **the object list rows** on the Objects tab and in a live session, and the room plan, elevation
  and motion timeline beside them;
- **the per-device Remove** in the Live capture branch of the input rail;
- **the CLIP box** on a channel meter, which clears that channel's clip latch;
- **the first-run screen's** three cards;
- **the "Coding tools and broadcast metadata →" link** that switches to Expert, below the
  loudness block on the Routing tab.

Everything in that list already reports a role and a name, so a screen reader describes it
correctly; what is missing is the tab stop and the key handling. Adding them is the same four lines
per control that the bed chips got, and the next pass should take the Guided wizard first — it is
the tier the window opens in.

## What has not been checked

Most of what this page describes is checked by reading the code. The Qt Quick suites hold a named,
countable part of it, and that part is a good deal smaller than **What can be done without a mouse**
and **What a screen reader is told** above. They are built where the window is built with
`AC3FORGE_BUILD_TESTS` on, one ctest entry per `tst_*.qml` (`apps/gui/tests/CMakeLists.txt`), and
they run under the offscreen platform, which has no accessibility bridge, so they read the
properties a bridge would read and leave what a screen reader does with them untested.

What they hold, in full:

- `tst_keyboard.qml` takes six controls — the `3/2` and `1+1` bed chips, the 0 and 1 low-frequency
  chips, the Format tab and the command-line chip — and asserts of each that it is in the tab
  chain, that it carries `FocusRing.qml`'s ring, and that its accessible name is not empty. It also
  asserts that the bed and low-frequency chips leave the tab chain while object mode locks them;
  that the first two extras checkboxes take their name and their channel tokens from the model row;
  that a bed chip's name follows the choice it draws, and that it reports the Button role and is
  checkable; and that six segmented groups — the tier switch, the File / Live capture switch and
  the Guided wizard's four room questions — are named, with no two names alike.
- `tst_accessibility.qml` works on four controls built on their own rather than in the window: the
  channel meter (its name follows the channel, its description follows the live reading, and the
  CLIP box reports checked), the QC gate meter (the description says within or outside limit, and
  `n/a` where there is no value), a segmented control the test names itself (that name reaches
  `Accessible.name`, and which segment reports checked follows `currentValue`), and the card (its
  name follows the title).
- `tst_main_shell.qml` asserts that the three tier segments are named and that the first of them
  reports the RadioButton role, and `tst_qc_panel.qml` asserts the gate meter's pass and fail
  wording.
- `tst_localisation_pipeline.qml` runs with `AC3GUI_LOCALE=xx`, the pseudo-locale that decorates
  every string `lupdate` found, and asserts that the Guided segment's accessible name carries that
  decoration — so that name is the translated label rather than an English copy typed beside it.

That is the whole of it. Nothing asserts the tab chain for the rest of the table under **What can be
done without a mouse** — the header buttons, the input rail, the presets, the codec, rate and
container controls, the other tabs, the Encode button and the runs strip are in that table by
reading, not by assertion. Nothing asserts the accessible name of a per-source or per-run control.
And `tst_diagnostics.qml` covers the diagnostics file rather than the window's controls: it asserts
nothing about a name, a role or a tab stop.

**No manual screen-reader pass has been run.** Neither NVDA on Windows nor Orca on Linux has been
through this window. That check is still to do, and until it is, treat the screen-reader behaviour
described here as designed rather than as observed.

Nor has the window been driven end to end by keyboard alone at 175% on a desktop. The suites assert
that those six controls are in the tab chain and that the type sizes follow the scale; they do not
assert that the resulting window is comfortable to use, and the fixed-height panels listed under
**Text size** above are the reason to expect it is not, yet.
