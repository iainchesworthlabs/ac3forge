# AC-3 & E-AC-3

This page covers the two "core" formats in the family described in [Concepts](index.md):
**AC-3** (Dolby Digital) and **E-AC-3** (Dolby Digital Plus). Dolby Atmos builds on top of
E-AC-3 and gets its own page, [Atmos & JOC](atmos-joc.md).

## Frames

Both formats chop audio into fixed-size chunks called **frames**, sometimes called
**syncframes** because each one starts with a recognisable sync pattern a decoder can search
for. Each frame is compressed independently enough that a decoder can find one, decode it, and
start playing without needing any frame before it. That is what makes it possible to seek
partway into a stream or tune into a broadcast already in progress — the decoder just waits
for the next syncframe rather than needing to start from the very beginning.

Inside a frame, the audio goes through a short pipeline:

```mermaid
graph LR
    A[PCM samples] --> B["Transform (MDCT)"]
    B --> C[Bit allocation]
    C --> D["Packed bitstream<br/>(syncframe)"]
```

- **Transform** — the raw waveform (a list of sample values over time) is converted into
  frequency information: roughly, "how much energy is at each pitch," rather than "what was
  the air pressure at each instant." Audio compresses much better once it's expressed this
  way, because a lot of that frequency information turns out to be small or predictable. The
  tool that does this is called the **MDCT** (modified discrete cosine transform) — the exact
  maths doesn't matter here, only that it is a *transform*, not a compression step by itself.
- **Bit allocation** — having converted the audio to frequency information, the encoder
  decides how many bits to spend describing each part of it, spending more where the human ear
  is more sensitive and less where it is not.
- **Packed bitstream** — the results are packed into the syncframe format the standard
  defines, ready to be written to disc, broadcast, or streamed.

### Transients and block switching

The transform above normally looks at a fairly long stretch of audio at once — about 10.7 ms —
which is what gives it good frequency resolution. That works well for steady, sustained sound,
but it has a cost on a sudden, sharp one: a drum hit or a cymbal crash: quantization error from
that one loud instant leaks backward across the whole stretch the transform covers, smearing a
faint echo of the hit into the silence just *before* it. That artefact is called **pre-echo**, and
it's audible precisely because it appears where there was nothing to mask it.

Both formats fix this by switching, per channel per block, to two half-length transforms instead
of one long one whenever a channel's encoder detects a transient — shorter transforms trade away
some frequency resolution for better time resolution, which confines the smearing to a much
narrower window around the transient instead of the whole block. The decoder does not need to be
told how the detector reached its decision, only which length transform to undo — the choice is a
single bit per channel per block, and everything downstream of the transform (which frequencies
got how many bits, and so on) is written identically either way.

## Channel beds and layout

You'll often see surround sound described as "5.1" or "7.1." The number before the dot is the
count of ordinary, directional speaker channels; the number after the dot is the count of
**LFE** (low-frequency effects) channels — bass-only channels with no fixed direction, because
very low frequencies aren't directional to human hearing anyway.

**5.1** means five directional channels — left (L), centre (C), right (R), left-surround (LS),
right-surround (RS) — plus one LFE channel. This fixed set of channels, all mixed and placed
by the engineer ahead of time, is often called the **bed**.

<figure markdown>
<svg viewBox="0 0 420 400" xmlns="http://www.w3.org/2000/svg" role="img"
     aria-labelledby="fig-51-title fig-51-desc"
     style="width:100%;max-width:380px;height:auto;display:block;margin:0 auto;">
  <title id="fig-51-title">Top-down diagram of a 5.1 speaker layout around a listener</title>
  <desc id="fig-51-desc">
    A top-down view of a room. A listener sits at the centre facing a screen at the top.
    Left, centre and right speakers sit in front of the listener; left-surround and
    right-surround speakers sit behind and to the sides; the LFE bass channel has no fixed
    position and is drawn close to the listener.
  </desc>

  <!-- screen / front indicator -->
  <rect x="160" y="14" width="100" height="8" rx="2" fill="none" stroke="#888" stroke-width="2"/>
  <text x="210" y="10" text-anchor="middle" font-size="11" fill="currentColor">screen / front</text>

  <!-- room outline -->
  <circle cx="210" cy="212" r="172" fill="none" stroke="#888" stroke-width="1.5" stroke-dasharray="4 4"/>

  <!-- lines from listener to speakers -->
  <g stroke="#888" stroke-width="1" opacity="0.55">
    <line x1="210" y1="212" x2="210" y2="62"/>
    <line x1="210" y1="212" x2="135" y2="82.1"/>
    <line x1="210" y1="212" x2="285" y2="82.1"/>
    <line x1="210" y1="212" x2="69" y2="263.3"/>
    <line x1="210" y1="212" x2="351" y2="263.3"/>
  </g>

  <!-- listener -->
  <circle cx="210" cy="212" r="7" fill="#888"/>
  <text x="210" y="238" text-anchor="middle" font-size="11" fill="currentColor">listener</text>

  <!-- C -->
  <circle cx="210" cy="62" r="9" fill="#4C6EF5"/>
  <text x="210" y="48" text-anchor="middle" font-size="14" fill="currentColor">C</text>

  <!-- L -->
  <circle cx="135" cy="82.1" r="9" fill="#4C6EF5"/>
  <text x="115" y="78" text-anchor="middle" font-size="14" fill="currentColor">L</text>

  <!-- R -->
  <circle cx="285" cy="82.1" r="9" fill="#4C6EF5"/>
  <text x="305" y="78" text-anchor="middle" font-size="14" fill="currentColor">R</text>

  <!-- LS -->
  <circle cx="69" cy="263.3" r="9" fill="#7048E8"/>
  <text x="40" y="280" text-anchor="middle" font-size="14" fill="currentColor">LS</text>

  <!-- RS -->
  <circle cx="351" cy="263.3" r="9" fill="#7048E8"/>
  <text x="380" y="280" text-anchor="middle" font-size="14" fill="currentColor">RS</text>

  <!-- LFE -->
  <circle cx="238" cy="196" r="7" fill="#E8590C"/>
  <text x="272" y="192" text-anchor="middle" font-size="13" fill="currentColor">LFE</text>
  <text x="210" y="360" text-anchor="middle" font-size="10.5" fill="currentColor" opacity="0.85">
    LFE (the ".1") has no fixed direction — bass isn't directional
  </text>
</svg>
<figcaption>A 5.1 layout seen from above: L, C, R in front; LS, RS to the sides/rear; LFE
anywhere, because bass has no direction.</figcaption>
</figure>

E-AC-3 can describe larger layouts too — 7.1 and beyond — described in the next section.

One coding mode is deliberately not a bed at all: **dual mono**, sometimes written **1+1**.
Rather than one programme mixed onto a fixed set of speakers, it's *two* independent
single-channel programmes — a second language track, a director's commentary — sharing one
syncframe and never mixed together. There's no diagram for it, because there's no soundstage: a
receiver plays one programme or the other (or both to separate outputs), chosen by the listener,
not blended by the encoder the way L/C/R are.

## Bitrate

**AC-3 is CBR (constant bit rate) only.** Every frame of an AC-3 stream spends the same number
of bits, chosen from the 19 nominal rates the standard defines, from 32 kbps up to 640 kbps —
`frmsizecod` is a lookup into that fixed table, so there is no way to say anything else. As with
any lossy compressed format, the general rule holds: a higher bitrate means more bits are spent
describing each second of audio, which generally means better quality, at the cost of a larger
file or a bigger slice of a broadcast pipe's bandwidth.

**E-AC-3 additionally supports VBR (variable bit rate).** Unlike AC-3, E-AC-3 states its frame
size directly (`frmsiz`, an 11-bit word count) rather than indexing a table, so nothing stops a
frame from being a different size than the one before it. ac3forge's E-AC-3 encoder can use this
either way:

- **CBR** (the default): every frame is sized from a fixed `bitrate_kbps`, same as AC-3.
- **VBR**: a *quality* target (0–1) replaces the fixed rate, and each frame's size follows how
  much the content actually needs — a quiet passage produces a smaller frame than a busy one at
  the same quality. Optional `min_kbps`/`max_kbps` bounds cap how far that can drift, the way
  `-b`/`-B` bound LAME's own VBR mode.
- **ABR** (average bit rate): a *rate* target again, but a long-run one — the encoder holds one
  SNR offset across frames and steers it to deliver that average, so frame sizes still move with
  the content. This is what a streaming ladder rung or a DVB mux contracts for: a rate you can
  plan a pipe around, without pinning every frame to the same size.

Quality is encoder-relative, not a perceptual scale that means the same thing across encoders —
and it is **not linear in bit cost**: masking-model bit allocation spends roughly twice the bits
for a fixed step up in precision, so cost rises steeply in the top part of the quality range. A
high quality with no `max_kbps` bound will often ask for more bits than any legal E-AC-3 frame
can hold at all for ordinary multi-channel material, and the encoder reports that plainly rather
than silently truncating — pairing a high quality with a `max_kbps` ceiling is the normal way to
use it. AC-3 has no equivalent: its frame size cannot vary at all — and for the same reason it
has no ABR either.

## E-AC-3 rate control: what VBR and ABR are worth

The paragraph above used to be the whole story, and "cost rises steeply in the top part of the
range" was a warning with no number behind it. `python tools/ci/quality_race.py vbr` is that
number. It sweeps the quality target, measures what each point actually costs, and then encodes
the *same programme at that same measured rate* three other ways — CBR, ABR, and FFmpeg's own
E-AC-3 encoder — so every comparison is like for like. Measured 2026-08-23 against a real build,
stereo, `tools=none`, FFmpeg 8.0.1, on the synthesized multi-segment programme
`quality_race.py` uses everywhere else (chord, sweep, filtered noise, near-mono speech-like
content, tone bursts):

| quality | measured kbps | SNR dB | LSD dB | MOS-LQO | vs CBR | vs ABR | vs FFmpeg |
|--------:|--------------:|-------:|-------:|--------:|-------:|-------:|----------:|
| 0.10 |    69.6 |  7.05 | 17.33 | 3.53 |  −1.26 |  +0.67 |  +1.38 |
| 0.20 |   106.6 | 22.97 |  8.28 | 4.20 |  −4.96 |  −6.56 | +16.23 |
| 0.30 |   166.2 | 39.96 |  6.45 | 4.30 |  +2.05 |  +2.22 |  +1.83 |
| 0.40 |   261.1 | 54.98 |  4.86 | 4.73 | +11.10 | +10.94 | +12.13 |
| 0.50 |   404.4 | 58.96 |  4.18 | 4.73 | +11.96 | +11.99 | +13.03 |
| 0.60 |   585.0 | 59.04 |  3.82 | 4.73 |  +2.39 |  +2.51 | +11.12 |
| 0.70 |   762.7 | 59.04 |  3.60 | 4.73 |  +0.15 |  +0.15 | +11.07 |
| 0.80 |   961.4 | 59.04 |  3.55 | 4.73 |  +0.01 |  +0.01 | +11.07 |
| 0.90 |  1025.5 | 59.04 |  3.54 | 4.73 |  +0.00 |  +0.00 | +11.07 |

The gap columns are SNR differences at the same measured rate; positive means VBR won. Four
things fall out of it.

**VBR beats CBR at the same rate, but only in the middle of the range.** Between roughly 0.3 and
0.5 the advantage is real and large — up to +12 dB — because VBR is allowed to overspend on the
frames that need it without ever paying that back. Below 0.3 it *loses*, by 1.3 dB at quality
0.10 and 5.0 dB at 0.20: a quality target that low leaves the rate it does spend badly
distributed, and CBR's flat budget does better with the same bits.

**The top of the range is waste, not headroom.** Everything at 0.6 and above scores 59.04 dB.
Quality 0.5 reaches 58.96 dB for 404 kbit/s; 0.8 pays 961 kbit/s — 2.4× the bits — for another
0.08 dB. MOS-LQO saturates even earlier, at 0.40. The old warning was right about the shape and
understated the conclusion: past about 0.5 there is nothing left to buy on this encoder.

**ABR scores with CBR, not with VBR — and that is what it is for.** Holding an average is the
same constraint CBR encodes under, so an ABR stream lands within a few tenths of a dB of CBR at
the same rate (37.75 vs 37.91 at ~166 kbit/s, 44.03 vs 43.88 at ~261, 46.96 vs 46.99 at ~404).
It is not a quality feature and this table is the evidence: if you can accept a rate that goes
where the content takes it, plain VBR at 0.3–0.5 is where the quality is. What ABR delivers
instead is the *rate*, within 1.1–1.8% of target across 96–384 kbit/s, with frame sizes still
swinging over 3× around it — a mux can plan around that and cannot plan around VBR. Windows
much longer than the default cost real quality: at a 64-frame window the same encode drops 5 dB
against CBR, where 8–32 frames stay within 1.5 dB.

**Against FFmpeg's E-AC-3 encoder at the same rate, ac3forge's VBR is ahead everywhere**, by
about 11 dB from quality 0.4 upward. Read that alongside `README.md`'s CBR numbers rather than
instead of them — the comparison here is one encoder's VBR against another's CBR, because FFmpeg
has no E-AC-3 VBR mode to race.

Every rate in the sweep is capped at E-AC-3's own ceiling — `frmsiz` is 11 bits, so 2048 words a
frame, which is 1024 kbit/s at 48 kHz. That is why the last row measures 1025.5 rather than
whatever quality 0.90 would otherwise have asked for, and why the three modes converge exactly
there: at the ceiling nobody has a choice left to make.

## What E-AC-3 adds over AC-3

E-AC-3 keeps everything AC-3 can do and adds more on top:

- **More channel layouts.** Beyond the plain 5.1-and-smaller layouts AC-3 supports, E-AC-3 can
  describe 7.1, 5.1.2, 5.1.4 and 7.1.4. It does this through **dependent substreams** — extra
  layers of channels riding alongside the main 5.1 bed, adding channels like extra height or
  rear speakers without redefining the whole stream format.
- **Better compression tools**, each recognised by name in the standard:
    - **Coupling** shares high-frequency detail across channels, because at high frequencies
      the ear is poor at telling *which* channel a sound is coming from anyway, so several
      channels can share one coded copy of that detail instead of each paying for their own.
    - **Spectral extension** predicts a channel's higher frequencies from its lower ones,
      instead of coding the high end directly — cheaper than describing every frequency band
      from scratch.
    - **Adaptive hybrid transform** swaps in a sharper transform for parts of the signal that
      need the extra precision, rather than using one fixed transform for everything.
- **Reduced sample rates.** Alongside the usual 48/44.1/32 kHz, E-AC-3 can code at half those
  rates — 24, 22.05 and 16 kHz — through a second sample-rate field (`fscod2`) that Annex E adds.
  Useful for low-bandwidth material that does not need the full audio band; classic AC-3 has no
  way to express these rates at all.

Together, these are why E-AC-3 fits more channels and better quality into a given bitrate than
plain AC-3 can.

### Choosing which tools to use

Every one of these tools trades something away. Coupling gives up per-channel detail above the
coupling frequency; spectral extension gives up the high band's fine structure entirely and paints
a described one back in its place. Each is a gain below some bitrate and a loss above it, so an
encoder has to choose — and this one will choose for you if you ask it to (`tools=auto`).

It used to choose from the bitrate alone. It now also measures the frame: how well the channels'
high band would survive being replaced by one shared copy, and how much of the signal is up in the
band synthesis would take over. That matters because the same bitrate can afford a nearly empty
top end and not a busy one, and because two channels that are already nearly the same thing above
8 kHz can be coupled almost for free while two genuinely different ones cannot.

Two of the tools are not in that automatic set, and the reasons are worth stating plainly:

- **Enhanced coupling** sounds *better* than ordinary coupling wherever the bitrate can carry its
  extra side information — measured on real film material, at every bitrate and layout tried. It
  is left out because FFmpeg cannot read it, and the automatic set has to produce streams that
  ordinary decoders can play. Ask for it by name (`cpl+ecpl`) if you control the decoder.
- **Transient pre-noise processing** does not pay. It replaces the audio just before a sharp
  attack with a copy of slightly earlier audio, to cover up noise the coder can smear backwards
  into the quiet run-up. Measured over exactly the samples it touches, the copy is 6.5 to 24 dB
  further from the original than the coder's own output was — at every bitrate, with the gap
  widening as the bitrate rises — and listeners' predicted scores do not move at all. The reason
  is that block switching already handles the problem: the encoder shortens its transform around
  a transient, which confines the noise, and the correction then substitutes for audio that was
  not damaged in the first place. It remains implemented and correct, as a demonstration of the
  syntax rather than as a quality tool.

!!! example "See it in code"
    - [Encoding AC-3](../library/encoding-ac3.md)
    - [Encoding E-AC-3](../library/encoding-eac3.md)
    - [CLI commands](../cli/commands.md)

---

Next: [Atmos & JOC](atmos-joc.md), where E-AC-3 gains a layer of 3D-positioned sound objects.
