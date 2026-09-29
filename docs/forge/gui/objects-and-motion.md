# Objects & motion

The Objects tab is always present in Advanced and Expert, and reachable from Guided too — step 4
(**Movement**) drives the same switch, and its trajectory presets author real keyframes onto the
same objects (see below). The tab starts with a single **Encode as Dolby Atmos objects** switch;
its tab-bar entry carries an `on` badge while it's active. Off, it's out of the way entirely. On,
it takes over the format choice:

![Objects mode on: room plan and elevation, object list, motion timeline](screenshots/objects-tab.png)

Turning it on fixes the layout, and the codec to one of two. Under E-AC-3, the default, objects
ride as JOC + OAMD side data over a plain 5.1 E-AC-3 bed: the bed picker freezes and the plan
strip reads `E-AC-3 · 5.1 bed + <n> objects · … · .ec3`. If the bit rate is under 384 kbps, a
warning chip appears (`Objects over a 5.1 bed want 384 kbps or better` — the metadata competes
with the audio for the same frame) with a one-click **Set it** fix, right here rather than on
the tab the bit-rate control lives on. The Format tab's codec field reads *Codec — object mode:
E-AC-3 or AC-4*: it takes Dolby Digital Plus or AC-4, and its AC-3 entry is greyed out, since AC-3
has no place for objects. With AC-4 chosen the switch writes [AC-4 objects](#ac-4-objects)
instead.

If any of "5.1 bed", "JOC", or "OAMD" aren't already clear, read
[Concepts → Atmos & JOC](../../concepts/atmos-joc.md) first — this page assumes that vocabulary.

## Objects come from the assignments

**Which channels ride as objects follows the
[assignment table](source-assignment.md#assigning-channels).** With nothing explicitly assigned,
every loaded channel becomes an object — the sensible default for a file full of stems. Once
anything is explicit, the table is the whole truth:

- A channel assigned **A new object** is a dynamic object, placed and moved in the room.
- A channel assigned to a **bed position** becomes a *static object pinned at that speaker's
  position* — in a JOC stream the bed *is* the panned objects, so "carried as a channel" and "an
  object that never leaves the L speaker" are the same coded thing. The LFE position pins as a
  pure LFE send, since no direction points at it.
- A channel assigned **Nothing** is dropped, with a named warning until that's explicit.

Dynamic objects plus pinned channels together must fit TS 103 420's sixteen-object programme cap
(the bed's LFE is one of the sixteen); an encode over it is refused with the count. An empty
object list (object mode on, nothing assigned to an object) says so — *"Objects come from the
assignments — send a sound to 'an object' and it appears here with a place in the room"* — with
an **Open assignments** button; **Add an object** and **Change what feeds them →** on the tab
itself jump to the same table.

Guided's own Movement step offers two one-click ways to fill this table once object mode is on,
rather than requiring a trip to the full assignment table first: **Everything moves** sends every
loaded channel to a new object (no bed position survives), and **Keep the bed, add movers** leaves
whatever is already assigned alone and only sends still-unassigned channels — typically a file
added since — to an object. Both write through `setAssignment` exactly as a hand edit in the table
above would, so either is a starting point, not a locked-in mode.

## Sounds available, room plan, elevation, object list

- **Sounds available** (top): one chip per loaded source (`orbit51.wav · 6 ch · in use`) with
  **Import audio…**, **Add live input** (switches the rail to Live capture — one input at a time
  today) and a **Change →** jump to the assignment table that decides what each sound does.
- **Room — plan** (left): a top-down grid, front/rear. Drag anywhere to place the selected
  object — or drag the marker itself. If the object has an authored path, a note under the room
  says the drag edits its *idle* position, not the path.
- **Room — elevation** (beneath it): a true side view — the horizontal axis is the room's
  *depth* (`front … rear`), so dragging edits y and z, never x. `ceiling`, `ear level` (drawn at
  66% of the view's height) and the floor are marked, the bed's speakers sit on their lines for
  context, the
  selected object carries an `obj n · z 0.NN` chip, and its drop line reaches the floor — height
  reads as height above the ground. Height changes the *metadata*, not the bed: a 5.1 ring has
  no speakers above it, so two objects at one azimuth and different heights are identical in the
  downmix and only the object layer tells them apart. X/Y/Z readouts sit below, and they follow
  the **path** during preview rather than freezing on the idle position.
- **Objects** (right): one row per object — number, **Sound** (which loaded channel it is: `Ch
  <n>` with one source, `<file> ch <n>` with several), X/Y/Z, path (`static`, the preset's own
  name like `orbit`, or `<n> keys` for a hand-authored one), LFE send, and keyframe count. The
  count line keeps the budget accurate — the denominator is what is left once bed-pinned
  channels have spent their slots (`4 of 13 objects · 2 pinned to the bed`), since the bed's LFE
  is the sixteenth. The selected object gets an **LFE send** slider (0.00–1.00) — the only route
  to that channel, since panning never reaches it.

  A row's position in this list follows the assignment table — object 3 today might be object 2
  tomorrow, if an earlier channel stops feeding an object. Its *motion* does not follow along:
  authored keyframes, position and LFE send belong to the actual (source, channel) they were
  placed on, so reassigning channels around never migrates one sound's path onto a different one,
  and removing a non-primary source (see [Multi-source & assignment](source-assignment.md#the-source-list))
  never disturbs a surviving source's motion.

## Motion

A timeline beneath the object list — a ruler, a clip band per loaded source, one lane per object
with its keyframes as rotated squares, and a playhead. It is an *editor*, not a display.

**The timeline's length is derived, never set by hand**: it is `max(offset + duration)` over
every loaded source (see the next section for what "offset" means), falling back to a fixed 8 s
only when nothing loaded has a duration to derive it from — a live session with no source, say. Loading a longer file, or dragging a source's clip band further out, grows
the ruler and every lane along with it; nothing needs re-authoring just because the programme got
longer.

- **Click or drag** anywhere on the timeline to scrub the playhead (pausing a running preview).
- **Double-click a lane** to author a key at that instant from the object's current position.
- **Drag a key** to retime it — the move commits on release, snapped to the current zoom tier (see
  below), and landing on another key replaces it (one instant, one cue).
- **Right-click a key** (or select it and press **Delete key**) to remove it.
- **Add key** captures the selected object's current position at the playhead. A hand-added key
  seeds the same `0.7/√n` gain the path-less fallback encodes at, so an object never jumps
  louder the moment its first cue lands.
- **Preview** plays every object through the Atmos encoder for real — its 5.1 bed through the same
  monitor path a live session uses, paced in real time — while the plan view, the elevation view
  and the playhead all follow the same audio clock, not a separate visual clock that could drift
  from it.

### Zoom, pan and snap

The ruler starts fit to the whole derived length. **Scroll wheel** over the timeline zooms,
centred on whatever time is under the cursor; **+ / − / Fit** by the Motion header do the same
from a click, up to 40×. Past 100% zoom, a thin strip above the lanes becomes a draggable viewport
indicator for panning. Ruler ticks promote as the view zooms in — 10 s, then 1 s, then 0.1 s apart
— and every snap (key drags, double-click-added keys, clip-band drags) follows the same tiers: 1 s
while zoomed out, 0.1 s once zoomed in, down to a 32 ms floor (one 1536-sample OAMD frame at
48 kHz) that no amount of further zooming crosses — finer than that is precision the format
cannot actually carry.

### Per-source offsets and keyframe timing

Each loaded source gets its own **clip band** at the top of the timeline, spanning its active
range (`offset … offset + duration`), and its own numeric **start offset** field on the rail's
source row (see [Loading a source](loading-a-source.md#01-input)). Drag a clip band (or edit the
rail field directly) to shift when that source's channels start — encoded as leading silence
ahead of its own audio, never as a change to the audio itself, the same way `ac3cli`'s `offset=`
token works (see [CLI → Options & grammars](../cli/metadata-options.md)).

**Keyframe times are programme-absolute, on purpose.** Sliding a clip band does *not* move that
source's objects' keys by default — a key at 4.2 s means 4.2 s into the programme, regardless of
which source is playing there. To bring an object's motion along with its source, **hold Shift**
while dragging the clip band; every key belonging to that source's objects shifts by the same
delta, clamped so none lands before 0. Without Shift, a source slides freely under motion that was
already authored to land where it lands.

### Trajectory presets

Guided step 4 offers **trajectory presets** — *Stay put*, *Circle the room* (one lap every eight
seconds), *Lift overhead* (floor to ceiling and back every eight seconds), and *Place them
myself*, which is this tab — that author real keyframes through the same API, so a preset is a
starting point on this timeline, not a separate motion system. For a file source, a preset repeats
its eight-second cycle in whole laps across the *entire derived programme length*, ending exactly
at the programme's own end rather than cutting off mid-turn; a live session has no such length, so
its presets simply loop the one fixed cycle for as long as the session runs. A path a preset
authored keeps the preset's name in the object table until a hand edit makes it something else.

### Export paths

**Export paths…** writes every dynamic object's current motion — or, for a path-less object, its
static position as a single time-0 keyframe — to a file `ac3cli atmos-path` and `atmos-encode`
read. Which form depends on the name you save under: a `.json` name writes the
`ac3::oba::ObjectScene` form (named objects, per-segment interpolation, a scene orientation — see
[Spatial & Atmos objects](../../library/spatial-and-atmos.md#the-serialised-form)), and anything else
writes the keyframe columns this export has always produced (`object_index time_s x y z gain
lfe_send`, one line per keyframe, addressed by each object's flat WAV channel index). `ac3cli`
tells the two apart by their first character rather than their suffix, so either file works
wherever the other does. JSON identifies an object by its position in the array rather than by an
index column, so where the column form simply skips a bed-pinned channel, the JSON form writes it
as a silent object holding at room centre — keeping every later object at the index a plain
`atmos-encode` run addresses it by. Once exported, the command bar's `atmos-encode` line
names that file as its trailing argument, so the line it shows is finally something that
reproduces this tab's authored motion from the command line, not just a static per-channel
placement. See [CLI → `atmos-encode`](../cli/commands.md) for the argument itself.

### Signed output

Forge GUI Atmos output is unsigned. To produce signed output, export the object paths and rerun
the equivalent `ac3cli atmos-encode` command shown in the command bar with `sign-objects` and a
runtime-provided key:

```bash
ac3cli atmos-encode in.wav out.ec3 448 0 paths.json sign-objects signing-key=/path/to/key
```

Keep the command bar's source, bit rate, object count, paths, `src=`, and `map=` arguments when
they differ from this example. Signing happens during the CLI encode; there is no supported
command that post-processes an existing GUI output file. See
[Object signing](../../library/signing.md) for key handling.

**Author a path / Drive it live** (top right) are the two ways to get motion in. Live driving
needs a monitored capture — the option points at [Live session](live-session.md) rather than
offering a dead control; during a live Atmos session the room is dragged in real time instead of
keyframed.

See [Spatial & Atmos objects](../../library/spatial-and-atmos.md) for the library API this timeline
is a UI over — `Keyframe`/`KeyframePath` per object, and `ObjectScene` for the scene the export
writes.

## AC-4 objects

With AC-4 as the codec the switch writes AC-4 objects (ETSI TS 103 190-2) in place of E-AC-3's
JOC over a bed. Choose AC-4 on the Format tab, then turn the switch on: the codec stays AC-4, the
plan strip reads `AC-4 · <n> objects · … · .ac4`, and the bit rate is left where it was. The
assignments, the room plan, the timeline, the trajectory presets and **Export paths…** work as
they do for E-AC-3, with what follows different.

The **AC-4** tab keeps its place in the tab bar, in place of Coding tools and Metadata, and
carries what an object stream takes:

| Control | `ac3cli atmos-encode … codec=ac4` option | Default |
|---|---|---|
| Coding: A-JOC, or direct-coded object substreams | `coding=ajoc\|direct` | A-JOC |
| dialnorm, in whole dB from 1 to 31 | `dialnorm=` | 31 |
| CRC on each raw sync frame | `crc=off` (a raw stream only) | on |

A-JOC codes a downmix and the matrices that rebuild the objects from it; direct coding sends
each object as a channel of its own. The frame rate is fixed at the native one, 2 048 samples,
and the rate is constant, so those controls are shown and off; so are the loudness values and
DRC, which describe channels, and the stereo downmix and dialogue enhancement cards are hidden.
Measured dialnorm is refused, since an object stream has no bed to measure it on.

**The writer's limits are on the page.** An AC-4 object stream is written at 2 048 samples a
frame with one position update per object per frame, taken from the timeline at each frame's
end as the E-AC-3 encoders take it; it holds 64 objects at most, one of them the LFE; and it is
a raw stream or an MP4 file, so Container's other choices are refused. The Objects tab says so
under its header, its count line reads `<n> of 64 objects`, less one for each channel assigned to
a speaker, which it names (`<k> assigned to speakers`), and what cannot be written now is named
there and at the top of the AC-4 tab, in the words Encode would refuse it with, before anything
starts:

- a container that is not a raw stream or an MP4 file;
- more than 64 objects, more than one channel assigned to an LFE, or no object that is not the
  LFE;
- a dialnorm that is measured, or not a whole number of dB from 1 to 31;
- sources that were resampled to the first one's rate, which no command line reproduces.

Encode also refuses a key whose gain is outside the +15 to -49 dB AC-4 codes (or silence), or
whose place is outside the room, and a bit rate the writer refuses for that many objects, quoting
the writer's own reason.

**What an object is.** Channels assigned to **an object** are dynamic objects, an `objm` range
folds to one, and a trim applies as it does for E-AC-3. A channel assigned to a speaker is an
object held at that speaker's place on the ring ADM's polar coordinates give a bed channel
(the left speaker at 0.25, 0.067), at unity; one assigned to an LFE is the stream's LFE object.
The LFE send has no AC-4 counterpart, so its slider is off, and there is no bed: the room plan
and the meters show the objects as a 5.1 monitor sees them. An object with no path keeps the
inverse-root gain E-AC-3 gives it, which AC-4 codes in whole dB.

**The command.** The command bar echoes one command,
`ac3cli atmos-encode <source> out.ac4 <kbps> <objects> <name>-paths.json [src=… map=… offset=…]
codec=ac4 [coding=direct] [dialnorm=…] [crc=off]`, with `out.mp4` for MP4, since `atmos-encode`
writes the MP4 file itself. Encoding writes `<name>-paths.json` beside the stream, the scene the
command reads (the file **Export paths…** writes when the name ends in `.json`, listing the
dynamic objects in the stream's order); copy it with the sources to where the command runs and
it writes the same bytes the page writes. The Qt Quick Tests hold a raw stream, an MP4 file and a
run of two sources with an assignment, an offset, a trim, a fold, a speaker and an LFE to that.
The page and the command share which channels become which objects and where a pinned channel
sits, the audio each object carries, the metadata updates and the call into the writer
(`apps/common/ac4_objects_core.hpp`).

**What it leaves out.** A live session encodes AC-3 or E-AC-3 only, so a live session is refused
under AC-4, and Guided's Movement step writes E-AC-3 objects. **Preview** plays the objects
through the E-AC-3 object encoder's 5.1 bed, the first fifteen of them, whichever codec is
chosen: it shows the motion, not the AC-4 stream's own rendering. The page reads audio, not ADM
BWF or IAB files; `ac3cli atmos-adm` and `atmos-iab` write those to AC-4 with `codec=ac4`.

## Next

[Live capture & session](live-session.md) — the same object machinery, but driven from a live
capture instead of a file.
