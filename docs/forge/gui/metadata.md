# Metadata

Expert tier only — Advanced folds the Loudness half of this onto the
[Format tab](format-and-channels.md#presets-codec-bit-rate-container) instead and leaves the rest
at their defaults. Guided has no separate Loudness step of its own; instead it applies its own
[loudness contract](index.md#the-loudness-contract) automatically, unless the fields here have
already been edited by hand. Downmix, Heavy compression, Mixing metadata and Service and
production are Expert-only in every tier.

![Metadata tab: Loudness, Downmix, Heavy compression, Mixing metadata, Service and production](screenshots/metadata-tab.png)

## Loudness

- **DRC profile** — `none` plus five profiles (`film-standard`, `film-light`, `music-standard`,
  `music-light`, `speech`), §7.7.1.
- **dialnorm** — a 1–31 spin box, disabled by a **measure** checkbox that derives it instead from
  BS.1770-4 gated loudness over the whole programme (§5.4.2.8). Getting it wrong isn't cosmetic —
  a levelled playback system plays the difference.
- **DRC profile / dialnorm — programme 2** — appear only with a
  [`1+1` dual-mono bed](format-and-channels.md#dual-mono) selected. Each programme states its own
  DRC curve and dialogue level, and each **measure** checkbox measures its own programme's coded
  channel — nothing is inherited or averaged between the two; the dual-mono section linked above
  explains why.

## Downmix

**Centre downmix** and **Surround downmix** dropdowns — Table 5.9 / Table 5.10 — control how a
wide source folds down to a narrower speaker layout.

## Heavy compression

A checkbox that reveals a **ceiling** spin box (in tenths of a dB, so the −0.5 dBFS default
survives) and a **dialogue** spin box — §7.7.2's peak-limited mono downmix, at syncframe
resolution. Both are levels at the output of an RF-mode decode, which normalises dialnorm and adds
11 dB with each word, so the −20 dBFS dialogue default asks for no make-up. A second,
identically-shaped **Heavy compression — programme 2** card appears beside
it under a `1+1` dual-mono bed, for the same reason DRC gets its own programme-2 copy above: Ch2's
own compr2 bounds Ch2's own signal, never Ch1's.

## Mixing metadata

E-AC-3 only. A checkbox reveals a preferred stereo downmix mode and an LFE mix level — the
`mixmdate` group, Table E1.2.

## Service and production

What the stream says about itself, as opposed to how to decode it. The card holds:

- **service** — which kind of service the stream is: complete main, music and effects, visually
  impaired, hearing impaired, dialogue, commentary, emergency, or voice over / karaoke (`bsmod`).
  ATSC A/53 and DVB key associated-service handling off this field. Voice over and karaoke share
  one value, since no bit separates them.
- **mixed at** and **room type** — the level the mix was monitored at, from 80 to 111 dB SPL, or
  `not stated`; the room type (`not indicated`, `large, X curve` or `small, flat`) can be set once
  a level is stated.
- **Dolby Surround** and **Dolby Headphone** — whether the programme was made for either; shown
  for a 2/0 bed only. **Surround EX** — shown for a bed with surround channels (2/2 and 3/2).
  Each reads `not indicated` until set.
- **A/D** — the analogue-to-digital converter type, `standard` or `HDCD`.
- **Copyright** and **Original bit stream** — the two flags of those names.
- **Annex D (bsid 6)** — AC-3 only. AC-3 carries the surround, headphone and Surround EX flags
  only under Annex D, which reuses the two time code fields that §D1 says were never applied for
  their original purpose. E-AC-3 gathers the whole group into `infomdat`, which setting any of
  these turns on.

The library page on [Metadata](../../library/metadata.md#bit-stream-information-ac3metabsihpp)
lists these fields and their sections of the standard.

Every field on this tab maps directly onto the [Metadata](../../library/metadata.md) library page's
config structs, and onto the [CLI's metadata options](../cli/metadata-options.md)
(`drc=`, `dialnorm=`, `cmixlev=`, `heavy`, `mixmeta`, …) — the values entered here and the tokens
on the command line are the same data, just two ways to set it.

## Next

[Objects & motion](objects-and-motion.md) — the Objects tab, where a plain bed becomes an Atmos
carrier.
