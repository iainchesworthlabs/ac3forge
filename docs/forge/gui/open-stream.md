# Open stream

The GUI player/monitor for an existing file — the same shape [QC a stream](qc.md) and
[Inspect objects](inspect-objects.md) already use, for the same reason. Everything else in this
guide configures and runs an **encode**: a source is loaded, a plan is built on it, Encode writes
a new file. This dialog is the opposite shape again — an **already-encoded** `.ac3`/`.ec3` file is
opened and decoded, with no source, no plan and no encoder involved anywhere in the path — so it
lives as its own dialog too, opened from an **Open stream…** button in the header beside **QC a
stream…**, **Inspect objects…** and **Preferences**.

Where QC measures a stream and Inspect objects shows its Atmos object metadata, this one plays
it: the GUI twin of `ac3cli monitor`, plus `ac3cli decode`'s WAV and object export.

## Opening it

**Open stream…**, in the header beside **QC a stream…**, **Inspect objects…** and
**Preferences**. It opens regardless of what (if anything) is loaded in the main workbench.

**Choose file…** opens a standard file picker filtered to `*.ac3`/`*.ec3` (plus **All files**).
Picking one starts the decode immediately, off the window's own event loop so the dialog stays
responsive while a long file decodes — the whole file is held in memory once decoded, the same
trade QC and Inspect objects already make, because a real seek needs the samples already
resident rather than re-decoded on demand.

## The report

Once a file decodes, the dialog fills with:

- **A summary line** — codec, layout, sample rate, unit count and duration, e.g.
  `E-AC-3 · 3/2 + LFE · 48000 Hz · 62 frame(s) · 1.98 s`.
- **Transport** — **Play**/**Pause** and a scrub bar across the whole decoded programme, driven by
  a real `ac3::audio::MonitorSink` playing on an ordinary (non-bitstreamed) output — the same
  shared-mode playback path [Inspect objects](inspect-objects.md#audition)'s own Audition button
  and the Objects tab's motion preview both already use. Dragging the scrub bar seeks; playback
  resumes from wherever it is released.
- **Levels and soundfield** — the same [`ChannelMeter`](loading-a-source.md#02-levels) rows and
  loudspeaker-ring soundfield view the workbench's own encode side draws, reading live from this
  decode instead.

For an Atmos-mode stream, playback is the **5.1 bed only** — like `ac3cli monitor`, this plays
what a legacy decoder hears, not unmixed objects. Use **Export objects…** below, or
[Inspect objects](inspect-objects.md), for the object audio itself.

## Exporting

- **Export decoded WAV…** writes the whole decode to a WAV file, the GUI twin of `ac3cli decode`'s
  primary output.
- **Export objects…** — an Atmos stream only — writes one `object_NN.wav` per JOC-reconstructed
  object into a chosen folder, the same naming `ac3cli decode`'s own `objects_dir` argument
  writes.

## From a finished run

A finished run's own chip carries a **More…** menu with **QC this run** and **Inspect objects**,
jumping straight into those two dialogs with the run's own output file already chosen — see
[QC a stream](qc.md#what-it-does-not-do) and
[Inspect objects](inspect-objects.md#what-it-does-not-do). This dialog itself has no such shortcut
yet; open the file a run just wrote like any other.

## What it does not do

This reads a stream that already exists; it has no connection to the workbench's own encode plan,
the loaded source, or the run strip beyond the shortcut above. It plays the bed, not unmixed
objects — see Inspect objects for those — and it does not write metadata, transcode, or otherwise
modify the file it opens.
