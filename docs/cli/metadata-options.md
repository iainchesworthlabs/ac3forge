# Options & grammars

Encoding commands in [Commands](commands.md) take these after their positional arguments, in any
order. Not every command honors every option, though the parser accepts them anywhere: `silence`
takes none at all; `record` and `live mode=channels` build a real encoder plan, so they honor the
whole metadata group below (`drc=`, `heavy`, `dialnorm=<n>`, `cmixlev=`/`surmixlev=` — which is
also what their §7.8 fold-down uses — `mixmeta`, `lfemix=`, `dmixmod=`) alongside `fast-mdct=off`,
`container=`/`fmp4-window=`, `layout=`, `codec=`, `watchdog=` and, `live` only, `capture2=`,
`objects=`, `map=` and `downmix=`. `dialnorm=auto` is the one they refuse: it measures a whole
programme before encoding it, and a live capture has not got one yet. `live mode=atmos` encodes
the fixed TS 103 420 shape, so it takes `dialnorm=<n>` and `fast-mdct=off` from the group and
nothing else. `atmos`, `atmos-path` and `atmos-encode` all apply `dialnorm=<n>`, `fast-mdct=off`,
`joc-domain=`
and the object-signing flags below, and `dialnorm=auto` is silently inert on `atmos`/`atmos-path`
— of the three Atmos commands, only `atmos-encode` measures, and not when `src=`/`map=` are in play
(an assembled object set has no single fixed layout to measure a whole-programme loudness
against). Every command honors `quiet`, `verbose` and `--help`:

```text
metadata options (any order, after the positional arguments):
  drc=<profile>     §7.7.1 dynamic range control per block
                    film-standard | film-light | music-standard | music-light | speech
  heavy             §7.7.2 heavy compression: a peak ceiling in the
                    mono downmix, at syncframe resolution
  ceiling=<dBFS>    that ceiling (default -0.5)
  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)
  drc2=<profile>    Ch2's own DRC profile, layout 1+1 only (§7.7.1) - not
                    inherited from drc=, set both to compress both programmes alike
  heavy2            Ch2's own heavy compression, layout 1+1 only (§7.7.2.2)
  ceiling2=<dBFS>   that ceiling for Ch2 (default -0.5)
  dialogue2=<dBFS>  where Ch2's heavy compression puts dialogue (default -20)
  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)
  dialnorm=<1..31>  set it directly (default 31)
  dialnorm2=auto | <1..31>   Ch2's own dialnorm, layout 1+1 only (§5.4.2.16, default 31)
  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)
  surmixlev=-3|-6|off     surround downmix level (Table 5.10)
  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)
  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)
  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)
  ltrtcmixlev=<dB>  Lt/Rt centre level, Table D2.3: +3|+1.5|0|-1.5|-3|-4.5|-6|off
  lorocmixlev=<dB>  Lo/Ro centre level, Table D2.5 (same eight values)
  ltrtsurmixlev=<dB>      Lt/Rt surround level, Table D2.4: -1.5|-3|-4.5|-6|off
                    (the three louder codes are reserved)
  lorosurmixlev=<dB>      Lo/Ro surround level, Table D2.6 (same five)

  annexd            AC-3 only: emit bsid 6, spending the two 14-bit timecod
                    fields on Annex D's xbsi1/xbsi2 instead (§D1)
  dsurexmod=none|off|ex|pliiz     Dolby Surround EX (Table D2.7)
  dheadphonmod=none|off|on        Dolby Headphone (Table D2.8)
  adconvtyp=standard|hdcd         A/D converter type (Table D2.9)
  encinfo           AC-3 Annex D: set the encoder's own reserved bit (§D2.3.1.12)

  infomdat          E-AC-3 only: emit the infomdat group (Table E1.2)
  bsmod=<service>   type of service, Table 5.7: cm | me | vi | hi | dialogue |
                    commentary | emergency | voiceover (or 0..7)
  dsurmod=none|off|on     Dolby Surround, 2/0 only (Table 5.11)
  mixlevel=<dB SPL> peak mixing level, 80..111 (§5.4.2.14)
  roomtyp=none|large|small        mixing room (Table 5.12)
  mixlevel2= / roomtyp2=  Ch2's own pair, layout 1+1 only (§5.4.2.22/23)
  langcod / langcod2      emit the reserved 0xFF language byte (§5.4.2.12); AC-3 only
  copyright         set copyrightb (§5.4.2.24; default clear)
  origbs=on|off     original bit stream vs. a copy (§5.4.2.25; default on)
  sourcefscod       E-AC-3: source sampled at twice fscod's rate (§E2.3.1.63)
  timecode=HH:MM:SS[:FF[.N]]      AC-3 bsid 8 only (§5.4.2.26-28)

  pgmscl=<dB>|mute  E-AC-3 programme scale factor, -50..+12 dB (§E2.3.1.13)
  pgmscl2=<dB>|mute Ch2's own, layout 1+1 only (§E2.3.1.15)
  extpgmscl=<dB>|mute     the external programme's (§E2.3.1.17)
  mixdef=none|premix|reserved|ext  mixing-parameter block (Table E2.6)
  premixcmp=<sel>:<src>:<scale>   dynrng|compr : external|local : 0..7
                    (§E2.3.1.19-21)
  mixdata=<0..4095> the twelve bits mixdef=reserved reserves (§E2.3.1.23)
  extmix=<L>,<C>,<R>,<Ls>,<Rs>,<LFE>[,<dmix>]   mixdef=ext external channel
                    scale codes 0..15 (Table E2.8), 'off' for a channel the
                    external programme lacks
  auxmix=<a1>,<a2>  mixdef=ext auxiliary channel scales, same codes
  speechmix=<d>[,<d1>:<att1>[,<d2>:<att2>]]     mixdef=ext speech enhancement
                    data (§E2.3.1.44-51)
  paninfo=<0..239>[:<0..63>]      E-AC-3 pan position, 1.5 degree steps
                    clockwise from centre, mono/1+1 only (§E2.3.1.53-58)
  paninfo2=         Ch2's own, layout 1+1 only
  blkmixcfg=<b0,..,b5>    E-AC-3 per-block mixing configuration, six 0..31
                    words or '-' for a block that sends none (§E2.3.1.59-61)
  couple            enable channel coupling - honored by 'encode' and 'sine' ('sine' can also
                    spell it as a 'c' layout suffix); E-AC-3 coupling is the tools argument's
                    cpl token instead
  keep-partial      encode/eac3-encode/atmos-encode: if the run fails partway, keep whatever
                    frames were already encoded (named beside the intended output as
                    <name>.partial.<ext>) instead of discarding them - off by default, matching
                    the GUI's own keep-partial-output preference
  fast-mdct=off     force the direct §8.2.3.2 forward MDCT instead of the default §7.9.4 fast
                    path (identical streams to within ~3e-12 max relative coefficient error;
                    the direct form is the validation oracle) - applies wherever this command
                    encodes, incl. atmos/record/live/eac3-sine/eac3-encode; eac3-encode's
                    [tools] positional can also reach this field via a bare nofastmdct token,
                    which wins if both are given; bare fast-mdct (the old opt-in) is a no-op
  fast-imdct=off    decode: force the direct §7.9.4 step-3 inverse instead of the default
                    radix-4 FFT evaluation - the decode-side mirror of fast-mdct=off above,
                    with the same relationship to its oracle (both codecs; bare fast-imdct,
                    the old opt-in, is a no-op)
  search=<what>     AC-3 encode (encode/sine) and E-AC-3 encode (eac3-encode/eac3-sine): choose
                    §7.2.2's transmitted bit allocation parameters per frame, from the error a
                    decoder will reconstruct, instead of taking the rate-derived defaults.
                    distortion minimises that error; perceptual weights it by a tonality/masking
                    model first. off (the default) keeps the fixed values every release before
                    this emitted. E-AC-3 narrows it: CBR only (silently inert under vbr=),
                    distortion only (perceptual parses and does nothing), AHT streams excluded
                    from the measurement, and the candidates are dbpbcod - kAllocCodes' 3
                    against Table E1.4's 2 - crossed with Table 7.11's rate-adaptive fgaincod,
                    which unlike AC-3's is not free (it opens the per-block fgaincode element)
                    and so is scored after a refit rather than assumed. Costs encode time - see
                    docs/library/quality.md for the measured figures
  mode=reference    both switches above in one word: every transform this command runs falls
                    back to the spec's own direct evaluation. mode=performance (the default
                    state) names the fast paths. Tokens apply in order, so a later
                    fast-mdct=off / fast-imdct=off still adjusts one half on its own
  joc-domain=mdct   atmos*/decode: estimate and apply the JOC reconstruction matrix over 256
                    MDCT bins instead of the default §7.1 64-band complex QMF - cheaper, ~5 dB
                    worse per object, and not the domain a licensed decoder reconstructs in.
                    Not part of mode= in either direction (=qmf names the default)
  dither=off        pin §7.3.4 dithflag at 0 instead of deciding it per channel per block from
                    content - the same reach as fast-mdct=off (encode/sine and the
                    atmos/record/live session builders); eac3-encode's [tools] positional can
                    also reach this field via a bare nodither token. Real dither values are
                    decoder-defined (the spec's own "any reasonably random sequence"), so this
                    is for a run that needs bit-for-bit agreement between two decoders of the
                    same stream more than it needs dither's own perceptual benefit -
                    tools/checks/verify_gold_reference.sh is the one caller that does
  verify            eac3-encode: decode every access unit as it is encoded and diff the
                    decoder's model against the encoder's own, refusing the run at the first
                    disagreement - off by default, since it roughly doubles the work

qc options (qc; any order, after the positional arguments):
  preset=<name>     gate the measurement against a named delivery spec
                    ebu-r128-s2 | atsc-a85 | atsc-a85-streaming | netflix | apple-music-atmos
  preset=all        gate against every preset above
                    omitted: measure and report only, no gate
  layout=bed        the default - meter the independent substream's own
                    Table 5.8 bed (BS.1770 Annex 1's basic algorithm)
  layout=rendered   meter the whole assembled program instead, every
                    dependent substream's height/wide/rear channels
                    included (BS.1770-5 Annex 3's extended algorithm)
  objects=<layout>  re-render dynamic objects by their own OAMD position
                    onto <layout> (71|512|514|714) and meter that too
                    (BS.1770-5 Annex 4); dynamic-object-only programmes
                    only
```

For `decode`, `drc=<scale>` instead applies §7.7.1 partial compression (`0` = ignore, `1` = as
encoded), and bare `heavy` prefers `compr` where the stream carries it — the decode-time meaning
of these two tokens is deliberately the mirror of their encode-time meaning. Both apply to
E-AC-3 decode too, matching the legacy AC-3 decoder — `.ec3` input no longer accepts and silently
ignores them. `fast-imdct=off` and `mode=` select the inverse transform's evaluation and apply to
both codecs' decode alike.

See [Metadata](../library/metadata.md) for what each of these fields actually is at the library
level (`dynrng`, `compr`, `dialnorm`, downmix levels) — the CLI tokens above map directly onto
that page's config fields.

## Bit stream information: `annexd`, `infomdat` and the service fields

Everything in this group is metadata a receiver reads *about* the programme; none of it changes
an output sample. What it decides is whether that receiver can tell a complete main programme
from an audio-description track, whether a stereo pair is a Dolby Surround matrix, and what
acoustic level the mix was judged at.

The two gate tokens exist because the two formats carry the group differently:

- **`annexd`** (AC-3 only) writes `bsid` 6. AC-3's two 14-bit `timecod` fields have never been
  applied for their originally anticipated purpose (§D1), so Annex D spends them on `xbsi1`
  (`dmixmod` plus separate Lt/Rt and Lo/Ro centre and surround levels) and `xbsi2` (`dsurexmod`,
  `dheadphonmod`, `adconvtyp`). It is implied by `dmixmod=`, by any of the four level tokens, and
  by the three `xbsi2` tokens — so `ac3cli encode in.wav out.ac3 384 51 dmixmod=ltrt` writes a
  `bsid`-6 stream without being asked twice. `timecode=` and `annexd` are refused together: they
  are the same 28 bits.
- **`infomdat`** (E-AC-3 only) opens Table E1.2's `infomdat` element. It is implied by every
  informational token — `bsmod=`, `dsurmod=`, `dsurexmod=`, `dheadphonmod=`, `adconvtyp=`,
  `mixlevel=`, `roomtyp=`, `copyright`, `origbs=`, `sourcefscod`. AC-3 needs no such gate: those
  fields are unconditional in its own bsi.

Fields the layout has no room for are simply not written, per the syntax: `dsurmod` and
`dheadphonmod` exist only at 2/0, `dsurexmod` only at 2/2 and 3/2, `mixlevel2`/`roomtyp2`/
`paninfo2`/`pgmscl2` only at 1+1. Setting one anyway is not an error — it just does not reach the
wire, which is what lets one set of options describe a programme at any layout.

`langcod` is a presence flag rather than a value: §5.4.2.12 makes the byte a reserved `0xFF`, the
language table it once indexed having been dropped in favour of the signalling layer's own
ISO 639-2 code.

```bash
ac3cli encode in.wav out.ac3 384 51 bsmod=vi mixlevel=105 roomtyp=large copyright dmixmod=ltrt ltrtcmixlev=-1.5 dsurexmod=ex adconvtyp=hdcd
```

That is an AC-3 visually-impaired associated service, mixed at 105 dB SPL in a large X-curve
room, copyright asserted, preferring an Lt/Rt downmix with the centre 1.5 dB down in it, flagged
as Surround EX and captured through an HDCD converter — `bsid` 6, because the last three of those
have nowhere else to go.

## Programme mixing (E-AC-3): `pgmscl`, `mixdef`, `paninfo`, `blkmixcfg`

The `mixmeta` group's five downmix levels say how to fold *this* programme down. The tokens above
say how to combine it with a *second* one — the audio-description or commentary service a
receiver mixes against the main programme. §E2.3.1.17's "external program" is that second stream.
The whole group is written only by the independent substream, since a dependent is part of
someone else's programme and has no second one of its own to talk about.

`mixdef=` picks one of Table E2.6's four options and decides which of the tokens below are
written: `premix` sends `premixcmp=` alone, `reserved` sends `mixdata=`'s twelve bits, and `ext`
sends whichever of `extmix=`/`auxmix=`/`speechmix=` are given inside a length-prefixed block.
Naming any of them turns `mixmeta` on.

```bash
ac3cli eac3-encode in.wav out.ec3 448 none 51 pgmscl=-6 extpgmscl=+3 \
    mixdef=ext premixcmp=dynrng:external:0 extmix=0,2,0,off,off,15 \
    blkmixcfg=3,-,7,-,-,31 bsmod=commentary mixlevel=98 roomtyp=small
```

In `extmix=`, `off` means the external programme has **no** such channel (§E2.3.1.31's cleared
flag), which is not the same as a scale factor of `0` — that is Table E2.8's −1 dB. `blkmixcfg=`
takes one word per block with `-` for a block whose `blkmixcfginfoe` stays clear.

`decode` reports both groups back: any service, production, Surround or programme-mixing field
the stream actually carries is printed under the `dynrng`/`compr` lines. A plain complete-main
programme with none of them prints nothing extra.

## The `tools:` token (`eac3-encode`)

Annex E coding tools, `+`-joined:

```text
tools:  Annex E coding tools, '+'-joined — none | cpl | spx | aht | tpn |
        nofastmdct | nodither | numblkscod:N | all
        (cpl:N / spx:N pin a band edge, aht:N the gain mode, ecpl selects
        enhanced coupling instead of standard, tpn selects transient
        pre-noise processing)
        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);
        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;
        atten:N pins the SPX notch depth, noatten removes it;
        ecpl only takes effect alongside cpl (e.g. cpl+ecpl);
        nofastmdct forces the direct-form forward MDCT instead of the
        default §7.9.4 fast path, nodither pins dithflag at 0 instead of
        deciding it from content — neither is a coding tool (nothing in
        the bitstream's syntax changes either way), so 'none' and 'all'
        both leave them alone, and the older opt-in spelling 'fastmdct'
        still parses as a no-op;
        numblkscod:N (0-3, default 3) shortens the syncframe to 1/2/3/6
        blocks (5.3/10.7/16/32 ms) — every substream of the access unit
        takes the same value, AHT is unavailable below 3 (Table E1.3 has
        no ahte bit there — combining it with aht/auto is refused up
        front, not silently dropped), and 'none'/'all' leave it alone too
```

The tool set is the fourth positional argument, not an `=` option. Example:
`ac3cli eac3-encode in.wav out.ec3 192 cpl+spx:5+aht:0` turns on coupling (auto band edge),
spectral extension pinned to band 5, and AHT with GAQ off; `cpl+ecpl+tpn` in the same slot turns
on enhanced coupling (auto band edge) and transient pre-noise processing together — `ecpl` and
`tpn` are independent tools, not alternatives to each other or to `spx`/`aht`, so any
combination the tools argument accepts is legal here. `all` does not currently imply `ecpl` or
`tpn`; name them explicitly to get either.

`ac3cli eac3-encode in.wav out.ec3 192 cpl+numblkscod:1` couples and halves the syncframe to two
blocks (10.7 ms) — useful where 32 ms of encode latency is too much (live monitoring, a
round-trip over a network link) at the cost of the bsi/audfrm header repeating three times as
often for the same audio, which comes straight out of the mantissas at a fixed bit rate.
`atmos-encode` does not accept this token yet — Atmos's object metadata (OAMD/JOC) is timed and
interpolated across a full six-block frame, and extending it to a shorter one is unstarted work,
not merely unexposed.

## The `vbr` token (`eac3-encode` only)

```text
vbr (eac3-encode only): off | q:0..1[,min:kbps][,max:kbps]
                            | avg:kbps[,win:frames][,min:kbps][,max:kbps] - E-AC-3 only
        quality is encoder-relative, not a fixed target — bit cost rises
        steeply above roughly half the range, so a high quality with no
        max bound will often refuse real programme material outright;
        bitrate_kbps still matters in vbr mode — it feeds the same
        coupling/spx frequency defaults it always has, not a target rate
        avg:kbps instead of q: is average-rate (ABR) mode: the encoder
        steers the SNR offset to hold that long-run average, with a
        sliding-window bit reservoir underneath it, so a quiet frame
        stays cheap and a busy one may cost more. It takes no quality —
        that is the knob it exists to move — so q: and avg: are refused
        together. win:frames sets the window (32 frames by default,
        about a second at 48 kHz); min:/max: still bound each frame.
```

There are two rate controls here, named by the leading token.

**`q:` — plain VBR.** `ac3cli eac3-encode in.wav out.ec3 192 none stereo q:0.4,max:320` encodes at
quality 0.4, capped at 320 kbps whenever the content would otherwise ask for more; `bitrate_kbps`
(192 here) still drives the coupling/spx band-edge defaults the way it always has, since VBR has
no fixed target rate to hand them. What quality 0.4 actually costs, and whether it beats CBR at
that cost, is measured in
[E-AC-3 rate control: what VBR and ABR are worth](../concepts/ac3-eac3.md#e-ac-3-rate-control-what-vbr-and-abr-are-worth).

**`avg:` — average-rate (ABR).** `ac3cli eac3-encode in.wav out.ec3 192 none stereo avg:192`
delivers a stream that averages 192 kbps over the long run while each frame's size still follows
the content. The encoder holds one SNR offset across frames and steers it — up while the stream is
running under its target, down while it is over — with a sliding-window reservoir underneath as a
hard ceiling, so no window of `win:` frames can overrun its pooled budget.

`avg:` takes no quality, and passing both (`q:0.5,avg:192`) is refused rather than one half
silently winning: quality fixes the SNR offset, and moving that offset is the whole of what ABR
does. The stream's first frame seeds the offset from its own budget search, so there is no
starting value to supply.

`win:` sets the window in frames — 32 by default, about a second at 48 kHz. Shorter holds the
average more tightly but gives the reservoir less to carry between a quiet passage and the loud
one that could use its savings; longer does the opposite. `min:`/`max:` bound each individual
frame and compose with the average, but a `min:` above the average (or a `max:` below it) makes
the average unreachable by construction and is refused.

Omit `vbr` (or pass `off`) for ordinary CBR — the default. AC-3 (`encode`) is CBR-only because
`frmsizecod` indexes a fixed frame-size table; `eac3-silence` and `eac3-sine` are E-AC-3
(`frmsiz`) and are CBR-only simply because their argument lists have no `vbr` slot.

## The `layout` grammar

```text
layout: mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714
        AC-3 carries only mono | stereo | 1+1 | 51 — everything wider needs the dependent
        substreams that only E-AC-3 has.
        71 renders 8 speakers from 10 coded channels
        714 renders 12 speakers from 14 coded channels
        For 'sine' and 'eac3-sine' each speaker gets its own tone; append
        'c' to a 'sine' layout (stereoc, 51c) to enable channel coupling.
        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a
        source narrower than it leaves the channels it lacks silent, and
        a wider one folds down per §7.8 using cmixlev/surmixlev.

        [layout] also takes a comma-separated Table E2.5 location list
        instead of one of the names above, for anything Annex E allows
        that has no preset: e.g. L,C,R,LFE,Vhl,Vhr or L,C,R,LFE,LFE2,Vhc.
        AC-3 accepts one too, as long as it needs no dependent substream
        (e.g. L,R,Cs or L,C,R,Cs - Table 5.8 modes no preset names).
        Locations: L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr
        Vhc Lts Rts LFE2 LFE - a paired location (Lc/Rc, Lrs/Rrs, Lsd/Rsd,
        Lw/Rw, Vhl/Vhr, Lts/Rts) must be given both halves.
```

`71` and `714` render fewer speakers than they code because, per §E3.8.2, a dependent
substream's channels replace some of the bed's rather than adding to it — see
[Wide layouts](../library/encoding-eac3.md) for the encoder-side mechanics behind that.

`1+1` is not a speaker layout at all — two independent, single-channel programmes sharing one
syncframe (§5.4.2's "1+1 dual mono") — so it's never inferred from a source's channel count the
way `mono`/`stereo`/`51`/etc. are; it has to be named explicitly. `encode`/`eac3-encode` take its
two channels either as one two-channel WAV (channel 0 = Ch1, channel 1 = Ch2) or as two mono WAV
files (`in.wav` = Ch1, the trailing `in2.wav` positional = Ch2) — see [Commands](commands.md) for
both forms. `dialnorm2=`/`drc2=`/`heavy2` above set Ch2's own dialnorm/DRC/heavy compression;
none of the three is inherited from Ch1's — a stream that wants both programmes treated alike sets
both explicitly. `decode` writes Ch1/Ch2 back out in that same order, and `levels` names them
`Ch1`/`Ch2` rather than a speaker position that would not apply.

## Source options (`encode`/`eac3-encode`/`atmos-encode`): `src=`, `map=` and `offset=`

```text
source options (encode/eac3-encode/atmos-encode/live; any order, after the positional
arguments):
  src=<path>        an additional input source; repeat for more than one
  map=<spec>        <source>.<channel>[-<channel2>]:<dest>[@<trim>][,...] - dest is a channel
                     name, obj, objm, p1, p2 or none; a channel range is only legal with obj,
                     objm or none, and folds to one mono object with objm; trim is an optional
                     signed dB gain in [-24,24] on dest, e.g. L@-3.5
                     once given, every loaded channel must appear - explicit 'none' silences
                     the goes-nowhere warning without giving it anywhere to go
  offset=<sourceIndex>:<seconds>   leading silence ahead of that source's own channels
                     (seconds >= 0), same 0-based numbering as src=
                     the programme is still as long as the longest one once every offset is
                     applied
```

`src=` loads another WAV alongside `in.wav` (source index 0), in the order given — `src=a.wav
src=b.wav` makes `a.wav` source 1 and `b.wav` source 2. Every source must share `in.wav`'s sample
rate. On `live mode=atmos` the "sources" are the capture devices instead: source `0` is
`capture_device`, source `1` is `capture2=` when present, and `map=` binds their channels to
object slots rather than to file channels (see
[`objects=` and `map=`](#objects-and-map-modeatmos)).

With exactly one source (no `src=` at all), `map=` is not needed: the existing automatic
single-source panning applies, byte-identical to a plain `ac3cli encode`/`eac3-encode` invocation
that predates this option — omitting `map=` is defined to behave exactly as if `src=`/`map=` did
not exist. With more than one source, `map=` becomes mandatory: automatic panning has no defined
meaning across several files, so every loaded channel needs an explicit entry (or an explicit
`none`) before the encode will run.

Each `map=` entry is `<source>.<channel>:<dest>`, comma-separated, `<channel>` 0-indexed. A
channel *range* (`<channel>-<channel2>`) is only legal when `<dest>` is `obj`, `objm` or `none` —
a location or a programme names exactly one channel, so a range there would be ambiguous about
which one it means. `objm` folds the whole range into ONE mono object (equal-weight sum, scaled by
`1/n` so several full-range channels summed together don't clip past what one alone would) rather
than one object per channel the way a plain `obj` range does. Two entries naming the same location,
or more than one entry per dual-mono programme, is refused.

The `obj`/`objm` destinations are real on the two object-capable commands (roadmap IO9):
`atmos-encode` and `live mode=atmos` both assemble them. Each `obj` row becomes its own dynamic
object; a contiguous `objm` range folds to a single mono object (equal-weight sum, scaled by
`1/n`); the objects appear in `map=` order — every `obj` row first, in source-then-channel order,
then each `objm` group. `atmos-encode` honours `src=`/`map=` the same way `encode`/`eac3-encode`
do, so a GUI assignment is reproducible headlessly, which is the point of the two front ends
sharing one grammar (see
[GUI → Multi-source & assignment](../gui/source-assignment.md)).

```bash
ac3cli atmos-encode stems.wav out.ec3 448 src=vo.wav \
    map=0.0:obj,0.1:obj@-3,0.2-3:objm,1.0:obj,1.1:none
```

On `encode`/`eac3-encode` they remain what they always were: those two commands have no object
layer to put an object in, so the routing skips every non-location row (also true of a stray
`p1`/`p2` row on a target that isn't dual mono) and audio mapped onto an object destination is
discarded — not silently, though: both print a warning naming the source/channel and destination
for each row that resolves to nothing.

Any `<dest>` may carry an optional trailing `@<trim>` — a signed decibel gain in `[-24, 24]`,
snapped to a tenth of a dB (`L@-3.5`, `obj@2`) — applied as linear gain wherever that channel's
content reaches the stream: folded into the routing matrix for a bed position or a dual-mono
programme, or, for `obj`/`objm`, into that object's own plane as it is assembled (by
`atmos-encode`, `live mode=atmos` or the GUI — see the note above). Omitted (no `@`) means no
trim, the same as an explicit `@0`.

```bash
ac3cli eac3-encode roundtrip-stereo.wav out.ec3 384 none 51 \
    src=roundtrip-51.wav \
    map=0.0:C,0.1:none,1.0:L,1.1:R@-3,1.2:none,1.3:LFE,1.4:Ls,1.5:Rs \
    offset=1:2.5
```

`roundtrip-stereo.wav`'s left channel (source 0, channel 0) carries the centre; its right channel
is explicitly silenced. `roundtrip-51.wav` (source 1) fills the rest, its right channel (`1.1`)
trimmed 3 dB down, with its own centre channel (`1.2`) also sent nowhere so it doesn't collide
with the first source's. `[vbr]` and `[in2.wav]` are both skippable here even though they come
earlier in `eac3-encode`'s own positional order — the parser lifts option tokens (anything
containing `=`, plus the known bare flags) out of the argument list wherever they appear, so the
remaining positionals keep their places whether options are present or not.

`offset=1:2.5` delays `roundtrip-51.wav` (source 1) by 2.5 seconds of leading silence ahead of its
own channels — every channel that source contributes shifts together, as when it starts, not what
it contains. `offset=` applies as silence, not truncation: the programme's overall length grows to
cover whichever source ends latest once every offset is applied, not just the longest source's own
raw length, so a delayed source is never cut short to fit. `<sourceIndex>` uses the same numbering
`src=` establishes (`0` is the primary positional file, `1..N` are `src=` in the order given), and
works with a single source too — `ac3cli encode in.wav out.ac3 384 51 offset=0:2.5` needs no
`src=`/`map=` at all. Omitting `offset=` for a source (or giving it `0` seconds) behaves exactly as
it always has.

`dialnorm=auto`/`dialnorm2=auto` work alongside `src=`/`map=`: the whole programme is routed once
as a measurement pre-pass — the same BS.1770-4 gated pass the single-file path runs, over what
`map=` actually assembles (post-routing, post-trim), not each source's own raw channels — before
the real encode loop routes it again to encode it. A `1+1` target measures Ch1/Ch2 independently,
same as the single-file case; every other target gets one whole-programme measurement over the
routed bed. A trim on `map=` (e.g. `L@-6`) is measured on the trimmed signal, since that is what
actually reaches the stream.

In the GUI, a full-bandwidth channel explicitly assigned onto `LFE`/`LFE2` is sent through a
120 Hz low-pass rather than passed through untouched — an explicit assignment states raw content
for that position, and a real subwoofer (and the LFE channel's own +10 dB mixing headroom)
assumes it only ever carries deep bass. `ac3cli` does not filter: its `map=` routing is a pure
gain matrix, so content mapped onto `LFE`/`LFE2` (e.g. `1.3:LFE` above) passes through
unfiltered. Neither front end touches a source's own dedicated LFE channel reaching `LFE`
through automatic single-source routing (no `src=`/`map=` at all) — that stays bit-exact, since
nothing there claims full-bandwidth content belongs on that position.

The GUI's own multi-source Format-tab table (**Add source…** plus a per-channel assignment field)
is a direct front end over this same grammar — see
[GUI → Multi-source & assignment](../gui/source-assignment.md).

## Record/live options (`record`, `live`): `container=`, `layout=`, `codec=`, `watchdog=`

```text
record/live options (record, live; any order, after the positional arguments):
  container=raw     the bare elementary stream (the default)
  container=mkv     Matroska, written incrementally as the take runs
                    (container=matroska is an accepted alias)
  container=ts      an MPEG-2 Transport Stream, same DVB profile as 'ts'
  container=spdif   an IEC 61937 WAV carrier, same bursts as 'spdif'
  container=fmp4    a DIRECTORY of fragmented MP4/CMAF segments plus live
                    HLS playlists and a dynamic DASH MPD, updated as the
                    session runs - the output path names the folder
                    (container=cmaf is an accepted alias)
  fmp4-window=<n>   container=fmp4 only: keep only the last <n> segments in
                    the playlist/MPD (a rolling live window); 0, the
                    default, keeps every segment
  layout=<name>     the encoded layout (default stereo); anything wider
                    than AC-3 carries promotes the stream to E-AC-3
  codec=ac3|eac3    force the codec instead of deriving it from layout=
  watchdog=<sec>    stop the session if capture delivers nothing for this
                    long (default 3, 0 disables)
```

### `container=`

All five values write the take **incrementally, as it is captured** — the same `RecordingSink`
the GUI's own takes go through (`apps/common/recording_sink.hpp`), shared verbatim between the two
front ends rather than reimplemented. Two consequences worth stating: a take of any length costs
one frame of memory rather than the whole session, and a crash an hour in leaves an hour of
playable file rather than nothing.

| Value | What it writes | Byte-identical to |
|---|---|---|
| `raw` (default) | The bare `.ac3`/`.ec3` elementary stream | the frames, concatenated |
| `mkv` (alias `matroska`) | Matroska, via `matroska::Writer`'s unknown-size Segment | `mkv` over the same frames, modulo the streaming Segment header |
| `ts` (alias `mpegts`) | MPEG-2 Transport Stream, DVB profile | `ts` over the same frames |
| `spdif` | IEC 61937 bursts inside a PCM16 WAV carrier | `spdif` over the same frames |
| `fmp4` (alias `cmaf`) | A directory of fragmented MP4/CMAF segments plus HLS/DASH manifests, via `Fmp4FolderWriter` (`apps/common/fmp4_folder_writer.hpp`) | see below |

Plain `mp4` is deliberately absent: `moov`/`stco` need every frame's final offset and the
`dac3`/`dec3` box needs a bitstream scan, so it cannot be written before the take ends — the
standalone [`mp4` command](commands.md#containers) wraps an already-finished file instead.
Fragmented MP4 has no such constraint, which is exactly why `container=fmp4` can stream
incrementally like the other four. Any other value is refused rather than silently ignored, the
same rule every option on this page follows.

`container=fmp4` (alias `container=cmaf`) is the same choice for fragmented MP4/CMAF, with one
shape difference the format forces: the output path names a **folder**, not a file, because the
container is a set of files. It is written incrementally — `init.mp4` at the first frame, a
`segment*.m4s` each time a fragment closes, and `audio.m3u8`/`master.m3u8`/`manifest.mpd`
rewritten alongside it — so the folder is a servable live origin *while the session is still
running*: the playlist carries no `#EXT-X-ENDLIST` and the MPD is `type="dynamic"` with an
`availabilityStartTime` until the session stops, at which point the trailing partial fragment is
flushed and both close to their VOD/static forms. `live` never accumulates the take in memory on
this path at all; `record`, which encodes to a fixed length up front, pushes its frames through
the same writer so the two leave identical folders for the same take.

`fmp4-window=<n>` bounds what the manifests list to the last *n* segments — `#EXT-X-MEDIA-SEQUENCE`
and the MPD's `@startNumber`/`SegmentTimeline` advance with the window, which is what a real
origin deleting segments behind itself needs. The segments themselves are still written; only the
manifests roll. RFC 8216 §6.2.2 wants a live playlist to hold at least three target durations of
media, so an `<n>` below 3 is accepted but not something a player will enjoy. The default, 0,
lists every segment — right for a session whose folder will be served whole afterwards.

```bash
ac3cli record out.mkv 30 192 0 container=mkv
ac3cli record out.ts  30 448 0 container=ts layout=51
ac3cli live out.wav 0 30 448 -2 -2 atmos container=spdif
ac3cli live out.mkv 0 30 448 -2 -2 atmos container=mkv
ac3cli live out_dir 0 30 448 -2 -2 atmos container=fmp4 fmp4-window=20
```

### `layout=` and `codec=`

`layout=` takes the same grammar as every other layout argument on this page — a named layout or a
comma-separated Table E2.5 location list (see [The `layout` grammar](#the-layout-grammar)) — and
defaults to `stereo`, which is what `record` and `live mode=channels` encoded before they could be
told otherwise. The captured channels are placed onto it **by direction**, not by index: a
two-channel microphone recorded onto `51` fills L/R and leaves the rest silent, and a source wider
than the target folds down per §7.8 using `cmixlev=`/`surmixlev=` — exactly what `encode` does with
a file.

The codec follows from the layout: anything wider than AC-3 can carry needs the dependent
substreams only E-AC-3 has, so it promotes the stream automatically. `codec=eac3` forces E-AC-3 for
a narrow layout too (for a receiver you want to exercise as Dolby Digital Plus); `codec=ac3`
alongside a layout AC-3 cannot carry is refused with the layout named.

`layout=`/`codec=` describe a **channel** session. `live mode=atmos` always encodes the TS 103 420
shape — a 5.1 E-AC-3 bed plus its object layer — so passing either alongside it is refused rather
than silently ignored.

```bash
ac3cli record out.ec3 30 448 0 layout=51
ac3cli record out.ec3 30 384 0 layout=stereo codec=eac3
ac3cli live out.ec3 0 30 448 -1 -1 channels layout=714
```

### `watchdog=`

A capture device that vanishes — unplugged, disabled, or torn down under the session — delivers
nothing but zero-byte reads for as long as you let it, which a plain "wait for more samples" loop
cannot tell from a device that is briefly starved. Without a watchdog the session just sits there
looking healthy with no audio coming in. `watchdog=<seconds>` is how long that may go on before the
session stops as a failure (default 3, matching the GUI's own
[device-drop detection](../gui/live-session.md#device-drop-detection)); `watchdog=0` turns it off
for a device that legitimately goes quiet for longer.

When it fires, everything already captured stays on disk — that is what streaming the take buys —
and the command exits `5` (runtime), naming the device that stopped. `live capture2=` gets its own
watchdog on the same timeout, so a dropped slave is reported as the slave.

## Live options (`live`): `capture2=`, `objects=`, `downmix=`

```text
live options (live; any order, after the positional arguments):
  capture2=<index>  a second capture device, clock-conformed to the first (see 'devices')
  objects=<N>       the object-slot budget for mode=atmos (1..15)
  downmix=off       refuse an AC-3-only passthrough endpoint instead of running
                    the parallel 5.1 AC-3 leg
```

### `capture2=`

`capture2=<index>` names a second capture device — same 0-based numbering `devices` prints and the
`capture_device` positional already uses — that joins the session alongside the master. The master
(`capture_device`) still paces the session exactly as it always has: frame timing, target frame
count and every other positional argument mean what they meant before this option existed.
`capture2`'s own sample rate does not need to match the master's, only be a legal AC-3 rate itself
(32, 44.1 or 48 kHz) — a drift-tracking resampler continuously conforms the slave's stream to the
master's pacing, correcting both the nominal rate conversion and whatever free-running clock drift
the two devices accumulate against each other. The slave's channels are appended after the
master's own, at new, higher channel indices, in the same interleaved per-frame block that feeds
the encoder — so a two-device `atmos` session simply has more capture channels to bind object slots
to. The measured drift is printed once, in signed parts-per-million, when the session ends.

```bash
ac3cli live out.ec3 0 30 448 -2 -2 atmos capture2=1
```

Captures 30 seconds of Atmos-mode E-AC-3 from device 0 (the clock master) plus device 1
(clock-conformed to device 0), no monitor or passthrough, writing `out.ec3`.

### `objects=` and `map=` (`mode=atmos`)

The object-slot budget is allocated **once, at session start**, and never changes: a decoder reads
the object count from the first access unit's OAMD and does not re-read it, so a slot bound
half-way through a session cannot be allowed to change how many objects the stream declares. That
is the same rule the GUI's live object room follows — its **Add object** chips allocate against a
fixed budget, and reassigning a chip rebinds a slot rather than creating one.

Without either option, `live mode=atmos` allocates one slot per captured channel (capped at 15,
since the bed's LFE is the 16th object and TS 103 420 §8.3.2.2 caps the total at 16) and binds them
one-to-one — exactly what it always did. `objects=<N>` sets the budget explicitly; slots past the
captured channel count are allocated but unbound, and carried silent.

`map=` binds capture channels to slots by name, using the same grammar `encode`/`eac3-encode` take
for files (see [Source options](#source-options-encodeeac3-encodeatmos-encode-src-map-and-offset)) with the
capture devices as sources: source `0` is `capture_device`, source `1` is `capture2=` when present.
`obj` makes a channel its own object; `objm` over a contiguous range folds it to one mono object;
`none` leaves a channel out. Objects appear in `map=` order — every `obj` row first, then each
`objm` group. A `@<trim>` on either is applied to that channel's contribution.

```bash
# Four objects: capture channels 0 and 1 on their own, 2-3 folded to one, and
# capture2's first channel as the fourth. Two more slots are allocated and left
# silent, so the stream declares six objects throughout.
ac3cli live out.ec3 0 60 448 -2 -2 atmos capture2=1 objects=6 \
    map=0.0:obj,0.1:obj@-3,0.2-0.3:objm,0.4:none,0.5:none,1.0:obj,1.1:none
```

`map=` is refused on `mode=channels`: a channel session places its channels by direction onto
`layout=`, and there are no object slots to bind. A budget smaller than what `map=` assigns is
refused rather than silently truncated.

### `downmix=`

When the session's stream needs E-AC-3 — an object session, or a channel layout wider than AC-3 can
carry — but the chosen `passthrough_device` only bitstreams plain AC-3, `live` runs a **parallel
5.1 AC-3 leg**: an independent AC-3 encode of the bed the main plan has already computed, sent to
that receiver, while the file and the monitor still carry the full stream. The bed is already a
self-sufficient fold-down of the whole programme, so there is no second §7.8 fold to compute; the
receiver simply hears a capped downmix instead of a refusal. This is the CLI half of the GUI's
[parallel downmix leg](../gui/live-session.md#parallel-downmix-receiver-leg).

`downmix=off` refuses instead — the plain "does not accept E-AC-3 over IEC 61937" warning and a
file-only session, which is what `live` did before. A receiver that accepts neither format is a
genuine refusal either way.

```bash
ac3cli live out.ec3 0 30 448 -2 1 channels layout=714      # capped 5.1 AC-3 to receiver 1
ac3cli live out.ec3 0 30 448 -2 1 channels layout=714 downmix=off
```

## Play options (`play`): `follow=`

```text
play options (play; after the positional arguments):
  follow=off        refuse a sink that rejects the source format instead of
                    transcoding to AC-3 or falling back to decoded PCM
```

### `follow=`

Roadmap UX9. When `play` is given a `device_index`, it asks what that sink actually accepts —
its own EDID/ELD-carried Short Audio Descriptors where a backend can read them (real today only
on ALSA; see [Linux](../platforms/linux.md)), the same live probe `outputs` uses everywhere
else. By default (`follow=on`, the implicit default), a source format the sink rejects gets an
automatic fallback:

- E-AC-3 on an AC-3-only sink is transcoded to AC-3 first (`transcode`, roadmap DC9, through a
  temporary file — dialnorm, `compr` and the mix metadata carry across exactly as a direct
  `ac3cli transcode` call would), then plays as AC-3. This is the "no 5.1 PCM over optical"
  case: an optical link can carry compressed AC-3 but never multichannel PCM, so a 5.1 E-AC-3
  source has no other way through.
- A sink that bitstreams neither format falls back to decoded PCM (`monitor`'s own §7.8-aware
  path, so a wide programme is folded to the endpoint's real width rather than left to a
  shared-mode mixer to average down however it sees fit).

`follow=off` disables both and restores the plain refusal `play` always gave — the escape hatch
for a script that wants a hard failure rather than an automatic re-encode. The default endpoint
(no `device_index` given) is unaffected either way: its capabilities were never probed before
this option existed either, and it is taken at its word exactly as before.

```bash
ac3cli play programme.ec3 2                # follows the sink (the default)
ac3cli play programme.ec3 2 follow=off     # the old plain refusal instead
```

## Common options (every command): `quiet`, `verbose`

```text
common options (every command; any order, after the positional arguments):
  quiet             no status output at all - errors on stderr and, for a '-'
                    output, the payload on stdout. Nothing else is printed.
  verbose           print the stderr progress line whatever the run's length,
                    and name every source/routing decision as it is made.
  --help, -h        this command's own help
```

`quiet` silences the *status* output — the meters, the routing summary, the `dialnorm=auto`
measurement line, the "wrote N frames" summary — while leaving errors on stderr and, for a `-`
output, the payload on stdout untouched. It does **not** silence a reporting command's report:
`levels`, `loudness`, `qc`, `devices` and `outputs` print their answer regardless, because that
answer is the command's output rather than commentary on it.

`verbose` turns the stderr progress line on whatever the run's length. Without either token, a run
long enough to be worth watching (500 frames or access units — about 16 seconds of audio) prints
that one line on stderr and nothing else new. It is always stderr, never stdout: a `-` output owns
stdout, and a progress line in the middle of a piped elementary stream would corrupt whatever is
reading it.

```bash
ac3cli encode in.wav out.ac3 448 51 quiet && echo "encoded"
ac3cli decode long.ec3 - verbose > out.wav
```

## Exit codes

Every command returns one of eight codes, so a script can tell *why* something failed rather than
only *that* it did. `ac3cli help exit-codes` prints the same table.

| Code | Meaning |
|---|---|
| `0` | Success. |
| `1` | Usage — a bad or missing argument, an unknown command or option, or a configuration the encoder cannot express (an illegal bitrate for a layout, more objects than a stream can carry). Retrying the same command line cannot help. |
| `2` | Input — unreadable, absent, or not a valid AC-3/E-AC-3/WAV/ADM file, or a stream that stopped decoding part-way. |
| `3` | Output — the destination could not be created, written or finalized. |
| `4` | Unavailable here — this build or this machine cannot run the command at all (no audio backend, no capture/render endpoint, an endpoint that refuses the format, a library this build was not configured with). The same command line may well succeed elsewhere. |
| `5` | Runtime — the run started and then failed for none of the above reasons: a capture device that stopped delivering audio (the `record`/`live` watchdog), a loudness measurement with nothing above the gate, a signing pass that could not complete. |
| `6` | A QC gate failed. Distinct from `2` so a CI step can tell "the stream is out of spec" (a result) from "`qc` could not read the file" (a fault). |
| `7` | Internal — an exception escaped a command. Never expected. |

`qc`'s long-standing contract is unchanged: exit `0` only when the file decodes cleanly **and**
every requested gate passes. What is new is the non-zero half being named.

```bash
ac3cli qc out.ec3 preset=ebu-r128-s2
case $? in
  0) echo "in spec" ;;
  6) echo "out of spec" ;;
  *) echo "qc could not run" ;;
esac
```

## Programme options (`eac3-encode`): `programme2=`

```text
programme options (eac3-encode; any order, after the positional arguments):
  programme2=<path> author a SECOND programme into the same stream, as a second independent
                    substream (§E2.3.1.2's I1)
  programme2-layout=<name>   its layout; omitted follows its own source
  programme2-bitrate=<kbps>  its own rate, spent ON TOP of the primary's; omitted is half
  programme2-dialnorm=<1..31>  its own dialnorm (§5.4.2.8, default 31)
```

A/52 Annex E allows eight *independent* substreams (I0–I7) in one elementary stream. That is a
different thing from the dependent substreams a wide layout uses: a dependent extends one
programme's soundfield, while a second independent substream is a whole second programme with its
own layout, rate and level. Broadcast DD+ uses them for the services §5.4.2.2 names — a second
language, an audio description, a commentary — so one stream carries the main mix and the
alternatives, and a receiver plays one of them.

```bash
ac3cli eac3-encode film51.wav out.ec3 448 none 51 off     programme2=commentary.wav programme2-layout=mono     programme2-bitrate=96 programme2-dialnorm=20
```

That writes I0 as 5.1 at 448 kbit/s and I1 as mono at 96 kbit/s, one access unit of each per
frame period. The rates **add**: substreams share a frame period rather than a frame, so the file
above runs at 544 kbit/s, not 448.

`programme2-dialnorm=` is deliberately not inherited from `dialnorm=`. A commentary or
description track is levelled independently of the mix it plays against — that independence is
the reason it is carried as a separate programme at all — so leaving it unset gives it the
default 31 rather than quietly copying the main programme's measurement onto it.

Not yet supported, and refused rather than ignored:

- `programme2-layout=1+1`. 1+1 already carries two programmes in one substream, levelled by
  `dialnorm`/`dialnorm2`; nesting it inside a second independent substream would mean three
  programmes described two different ways with only one of the levels reachable. Use 1+1 on the
  primary programme instead, or give `programme2` a layout of its own.
- `programme2=` together with `src=`/`map=`. The multi-source router assigns channels to one
  programme.
- Labelling a programme as a service (`bsmod`) or supplying the mixing metadata a receiver would
  use to mix an associated service against the main one — that is roadmap `DC3`/`DC4`, and this
  is the structural half.

One thing worth knowing before shipping such a stream: **FFmpeg refuses it outright**, and not
only the second programme — see
[Validation → Where the oracles don't reach](../verification.md#where-the-oracles-dont-reach).

## Programme options (`decode`, `qc`, `levels`): `programme=`

```text
programme options (decode, qc, levels; any order, after the positional arguments):
  programme=<0..7>  which programme of a multi-programme stream to work on, by the §E2.3.1.2
                    substreamid of its independent substream; omitted takes the first the
                    stream carries
```

The decode-side half of `programme2=` above. All three commands work on exactly one programme —
never a fold of several, since two programmes are alternatives rather than layers and mixing them
would give a WAV that splices unrelated audio, or a loudness figure neither programme has.

```bash
ac3cli decode out.ec3 main.wav                # programme 0
ac3cli decode out.ec3 commentary.wav programme=1
ac3cli levels out.ec3 programme=1
ac3cli qc out.ec3 programme=1 preset=atsc-a85
```

Omitting it takes the first programme the stream carries. When there is more than one, each
command says which it picked and what else was there (`programme 0 of 2 (0, 1)`), so a
multi-programme stream is never handled silently. Asking for a programme the stream does not
carry is an error that lists the ones it does. Ignored for AC-3, which has no substream layer.

## Stream-tool options (`transcode`, `metadata`)

| Token | Command | Meaning |
|---|---|---|
| `codec=ac3` / `codec=eac3` | `transcode` | The output codec, when the output name's own suffix cannot say it — stdout (`-`), or a file named something other than `.ac3`/`.ec3`. Without it, an unrecognisable name is refused rather than guessed |
| `compr=<dB>` | `metadata` | Stamp §7.7.2's compression word onto an existing stream, as the 8-bit wire value that gain implies |
| `compr2=<dB>` | `metadata` | The same for Ch2 of a 1+1 dual-mono stream |
| `bsmod=<0..7>` | `metadata` | Table 5.5's service type (0 = complete main, 1 = music and effects, 2 = visually impaired, …) |
| `dsurmod=<0..3>` | `metadata` | Table 5.11's Dolby Surround mode. Coding mode 2/0 only — §5.4.2.7 transmits it nowhere else |

`compr=`/`compr2=` round **down** to the nearest representable word, not to nearest: §7.7.2
exists to give "an assured upper limit of instantaneous peak reproduced signal level", and a
ceiling exceeded by half a step is not assured. That is the same rule
`ac3::meta::encode_compr_at_most` applies on the encode side.

`compr=` is a different thing from the `heavy`/`ceiling=`/`dialogue=` group above. Those ask an
*encoder* to derive a compression word from the signal it is coding; `compr=` names the word
outright, because a metadata rewrite has no signal to derive from — only bits to overwrite, and
only where the stream already carries a `compr` word. Asking for one where it does not is
refused, not invented.

`dialnorm=` and `dialnorm2=` work on `metadata` too, but only with an explicit `1..31` value:
`dialnorm=auto` needs a measurement, which is what `ac3cli normalize` is. On `transcode` they
override the value carried from the source, and `dialnorm=auto` measures the *source* the same
way an encode from a WAV would.

## Qc options (`qc`): `preset=`, `layout=`, `objects=`

```text
qc options (qc; any order, after the positional arguments):
  preset=<name>     gate the measurement against a named delivery spec
                    ebu-r128-s2 | atsc-a85 | atsc-a85-streaming | netflix | apple-music-atmos
  preset=all        gate against every preset above
                    omitted: measure and report only, no gate
  layout=bed        the default - meter the independent substream's own
                    Table 5.8 bed (BS.1770 Annex 1's basic algorithm)
  layout=rendered   meter the whole assembled program instead, every
                    dependent substream's height/wide/rear channels
                    included (BS.1770-5 Annex 3's extended algorithm)
  objects=<layout>  re-render dynamic objects by their own OAMD position
                    onto <layout> (71|512|514|714) and meter that too
                    (BS.1770-5 Annex 4); dynamic-object-only programmes
                    only
```

`preset=<name>` checks `qc`'s BS.1770-4 measurement against one named delivery-loudness gate instead of just
reporting it; `preset=all` checks every one below in a single run. Each preset states a target integrated
loudness, a symmetric tolerance around it (in LU) and a true-peak ceiling (a one-sided limit, never exceeded —
not a tolerance band). The numbers are defined in `ac3::meta::qc_preset()`
([`ac3/meta/qc.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/forge/include/ac3/meta/qc.hpp)), each
read directly from its own primary source rather than recalled from memory:

| Preset | Loudness | Max true peak | Source (version, date) |
|---|---|---|---|
| `ebu-r128-s2` | −23.0 LUFS ±1.0 LU | −1.0 dBTP | EBU R 128 s2 "Loudness in Streaming" (Geneva, **November 2023, v3**) recommendation (e) — programmes "should be streamed unchanged, that is at −23.0 LUFS" — which itself defers tolerance/true-peak to EBU R 128 (Geneva, **November 2023, v5**) recommendations (h) and (m) |
| `atsc-a85` | −24.0 LKFS ±2.0 dB | −2.0 dBTP | ATSC **A/85:2026-07** (approved **8 July 2026**) §6 "Target Loudness and True Peak Levels for Content Delivery or Exchange" |
| `atsc-a85-streaming` | −25.0 LKFS ±2.0 LU | −2.0 dBTP | ATSC **A/85:2026-07** (approved **8 July 2026**) Annex L.5 — "Selecting a Loudness value between −23 and −27 LKFS is recommended", restated in Annex M's Table M.1 |
| `netflix` | −27.0 LKFS ±2.0 LU | −2.0 dBTP | Netflix "Sound Mix Specifications & Best Practices" **v1.6**, Near-field Audio Prerequisites for Mix Facilities; Netflix "Dolby Atmos Home Mix Deliverable Requirements" **v2.3** states the same numbers for an Atmos deliverable |
| `apple-music-atmos` | **≤ −18.0 LKFS** (a ceiling, not a band) | −1.0 dBTP | Apple "Immersive Audio Source Profile" (Apple Video and Audio Asset Guide), Dolby Atmos music deliverables — "should not exceed −18 LKFS measured as per ITU-R BS. 1770-4" |

Two of these need a word of explanation.

`atsc-a85-streaming` carries a **band**, not a point. Annex L.5 asks a streaming service to pick "only one
specific and consistent Target Loudness" somewhere between −23 and −27 LKFS; −25.0 ±2.0 reproduces those two
edges exactly. The −25.0 midpoint is an artefact of how this table is shaped and is *not* a level the Annex
asks anyone to aim for — it names −23, −24 and −27 as the values real operators actually use.

`apple-music-atmos` is the one preset whose loudness figure is a **ceiling** rather than a band: Apple's clause
is "should not exceed", so a quieter master is compliant however quiet it is. Gating that as a ±band would fail
material the specification accepts, so `qc` prints it as `limit <= -18.0 LKFS` and passes anything at or under
it. True peak is always a ceiling, for every preset.

### Specifications deliberately not given a preset

Adding these would mean shipping a second name for a verdict already on offer, so they are documented here
instead:

- **EBU R 128 s4** "Loudness Normalisation of Cinematic Content" (November 2023). Recommendation (m) normalises
  Programme Loudness to "a Target Level of −23.0 LUFS" and (l) repeats the −1 dBTP ceiling — numerically
  identical to `ebu-r128-s2`. What s4 adds is recommendation (j), a Loudness-to-Dialogue Ratio not exceeding
  5 LU; that is a Programme-minus-Dialogue figure, and `LoudnessMeter` has no dialogue gate, so the single
  clause that would distinguish an s4 preset is also the one this meter cannot evaluate.
- **Netflix Dolby Atmos Home Mix Deliverable Requirements v2.3**. Same three numbers as `netflix`. What it adds
  is scope rather than numbers — "Loudness and peaks should be measured via a 5.1 rerender" — which is a
  `layout=` choice, not a gate.
- **Amazon.** Prime Video figures are widely repeated at −24 LKFS/−2 dBTP, but every source found for them is a
  third-party summary and Amazon's own delivery specifications sit behind a partner portal. Nothing in this
  table is cited to a document that was not read, so the row is absent rather than guessed — and −24/±2/−2
  would in any case restate `atsc-a85`.

### `layout=bed` (default) and `layout=rendered`

`layout=` chooses *which soundfield* is metered, and with it which of BS.1770's two algorithms does the
metering:

| | What is measured | Algorithm |
|---|---|---|
| `layout=bed` (default) | The independent substream's own Table 5.8 bed | BS.1770 Annex 1's basic algorithm — Table 3 weights, keyed on `acmod` |
| `layout=rendered` | The whole assembled program, every dependent substream's channels laid over the bed in Table E2.5 order | BS.1770-5 (11/2023) Annex 3's extended algorithm — Table 4 weights, keyed on each channel's position |

The default is `bed`, which is what `qc` has always measured. On a stream that carries dependent substreams,
`bed` now says so explicitly rather than silently reporting the 5.1 as if it were the whole programme:

```text
qc: atmos.ec3 (E-AC-3, 3/2 + LFE, 48000 Hz, 62 access unit(s), 1.98 s)
  layout=bed  (BS.1770 Annex 1, Table 3 weights over the Table 5.8 bed)
  note: this stream carries dependent substreams whose channels (height, wide, rear)
        are NOT in the figures above - layout=rendered measures them as well
```

`layout=rendered` is what makes 7.1, 5.1.2, 5.1.4 and 7.1.4 measurable at all, since none of those channels is
a member of Table 5.8. Annex 3's Table 4 weights a channel by where it sits: **1.41 (+1.5 dB)** for anything
between 60° and 120° azimuth below 30° elevation, **1.00** everywhere else. Applied to Table E2.5's locations
that gives:

| Weight | Locations | BS.2051 label |
|---|---|---|
| 1.41 (+1.5 dB) | `Ls` `Rs` `Lsd` `Rsd` `Lw` `Rw` | M±110, M±090, M±060 |
| 1.00 (0 dB) | `L` `C` `R` `Lc` `Rc` | M+000, M±030, M±SC |
| 1.00 (0 dB) | `Lrs` `Rrs` `Cs` | M±135, M+180 |
| 1.00 (0 dB) | `Vhl` `Vhr` `Vhc` `Lts` `Rts` `Ts` | every U/T position |
| excluded | `LFE` `LFE2` | — |

Two results there are worth reading twice, because reasoning from the channel *names* gets both wrong: a 7.1
layout's rear pair is **not** surround-weighted (M±135 is past the 120° edge), and **no** height channel is
either (Table 4's elevation row simply does not cover the upper layer). The wides *are*, sitting exactly on the
inclusive 60° edge.

That first one is where other meters differ. ffmpeg's `ebur128`, probed one channel at a time, weights a 7.1
layout's back surrounds at 1.41 just like its side surrounds — it generalises Annex 1's Table 3 by channel
*name*, so anything called a surround gets +1.5 dB. Annex 3's Table 4 and Table 5 both put M±135 at 1.00, and
`layout=rendered` follows the standard, so expect a 1.5 dB disagreement on exactly those two channels. On 5.1
the two agree to within 0.02 dB, which is why `ebur128` is a good cross-check for `layout=bed` and not for
`layout=rendered`.

For a plain 5.1 stream the two algorithms are the same function — `Ls`/`Rs` are M±110, inside Table 4's +1.5 dB
sector, which is where Annex 1's Table 3 got its 1.41 — so `layout=` changes nothing there. The one Table 5.8
layout where they genuinely differ is 2/1 and 3/1: Annex 1 has no Table 3 entry for a lone surround and this
meter reads it as the surround field collapsed to one channel (+1.5 dB), while Annex 3 sees Table E2.5's `Cs`,
a rear centre at M+180, at unity.

### `objects=<layout>`

`layout=` above meters *channels* — the bed, or the whole assembled program with every dependent's channels
laid over it. Neither sees a dynamic object's own position: this project's own encoder pans every object onto
the flat 5.1 ring at encode time (`ac3::spatial::pan_room`'s own "a raised object folds onto the ring... at
full level"), so an object authored directly overhead measures no differently from one on the ring — the bed
a legacy 5.1 decoder plays has nowhere else to put it.

ITU-R BS.1770-5 (11/2023) Annex 4 covers this: it defines no weighting table of its own, and instead says to
render object-based (or combined channel- and object-based) audio to a real loudspeaker configuration first,
then meter *that* through Annexes 1/3 — and to report which configuration and rendering algorithm did the
rendering, since two reasonable choices can legitimately disagree by several LU (Annex 4's own worked example,
its Table 6, does exactly that across renderers and layouts). `objects=<layout>` is that: it re-renders each
dynamic object by its own OAMD position — including height — onto the named layout (`51`, `71`, `512`, `514`
or `714`) via `ac3::spatial`'s direction-based panner (the same height-aware geometry `ac3::plan`'s
layout-to-layout channel renderer uses internally), then meters the result through the Annex 3 algorithm
`layout=rendered` already implements:

```text
$ ac3cli qc atmos.ec3 objects=514
qc: atmos.ec3 (E-AC-3, 3/2 + LFE, 48000 Hz, 62 access unit(s), 1.98 s)
  layout=bed  (BS.1770 Annex 1, Table 3 weights over the Table 5.8 bed)
  ...
  objects=5.1.4  (BS.1770-5 Annex 4: objects re-rendered by their own OAMD
  position, via ac3::spatial's direction panner, then Annex 3)
  objects: measured (BS.1770-4 gated / EBU Tech 3342 / BS.1770-4 Annex 2):
  ...
```

`layout=`/`layout=rendered` and `objects=<layout>` are independent switches — `objects=` adds a third,
separately-labelled report rather than replacing either of the other two.

Restricted to a **dynamic-object-only** programme — the only shape `atmos-encode` produces, and what Dolby's
own reference JOC test streams declare. For that shape the decoded bed already IS the objects' 5.1 VBAP fold
(`oba/oamd.hpp`'s own comment: "the 5.1 downmix IS the objects' VBAP render"), so `objects=` starts every
full-bandwidth target channel at silence and sums each object's own recovered audio
(`DecodedAccessUnit::object_audio`) into it, rather than adding to a bed that already carries it and
double-counting. A bed-and-objects programme — third-party content whose bed may carry independent,
non-object material this decoder cannot separate back out — is refused rather than risk silently doubling or
dropping content:

```text
$ ac3cli qc plain_channel_based.ec3 objects=514
error: programme 0 carries no dynamic-object-only OAMD - objects= needs OAMD dynamic
objects (try layout=rendered or layout=bed instead)
```

Omitting `preset=` entirely leaves `qc` in measure-and-report mode — every number is still printed, there is
just no PASS/FAIL verdict and nothing to gate on. See [Commands → `qc`](commands.md#decoding-inspection) for the
full report format and the exit-code convention this drives.

## Probe options (`probe`): `json=`, `detail=`

```text
probe options (probe; any order, after the positional arguments):
  json=1            emit the JSON document instead of the human table
                    (schema ac3forge.probe/1 - docs/cli/commands.md)
  detail=frames     add a per-access-unit dump: offsets, sizes, CRC,
                    substream headers and each frame's object layer
  detail=blocks     the same, plus every block's coding tools and
                    exponent strategies - what a codec bug report needs
```

`json=1` is a value token rather than a bare word (unlike `couple` or `heavy`) because `probe` is
the first command here whose *output form* is a choice: `json=0` is accepted and means the table,
so a script building its command line programmatically (`json=$want`) never has to omit the token
to turn it off. The document it emits is a versioned contract — see
[Commands → `probe`](commands.md#json-output-json1) for the schema, the compatibility rules and
the units each field is in.

`detail=` is independent of `json=`: all four combinations work, and the two detail levels add to
the stream summary rather than replacing it — the same summary comes out either way.

| `detail=` | What it adds |
|---|---|
| *(omitted)* | Nothing. The stream summary alone, which is what a pipeline or a CI gate wants |
| `frames` | One entry per access unit: byte offset, size, start time, and each syncframe's own header, CRC state, authenticity tag and object layer |
| `blocks` | The same, plus per syncframe: Table E1.3's frame-level tool gates, and per block which coding tools were in force and what exponent strategy each coded stream carried |

A dump of a long file is a lot of output, which is why neither is on by default — but it costs no
extra memory, because each access unit is written as the walk reaches it rather than collected
first. See [Commands → `probe`](commands.md#per-frame-and-per-block-detail) for what the block dump
looks like and why it exists.


## Defaults

Optional positional arguments, when omitted:

- `silence`, `sine`, `eac3-silence`, `eac3-sine` — 5 s at 192 kbps; the tone commands default to
  1000 Hz at 50% amplitude, and the three that take `[layout]` default to `stereo`.
- `orbit` — 8 s at 448 kbps, 4 s per orbit.
- `atmos` — 8 s at 448 kbps, 4 objects, 6 s per orbit.
- `record` — 5 s at 192 kbps from device 0.
- `live` — 10 s at 192 kbps.
- `play`, `monitor` — device `-1`, the default output.
- `transcode` — 448 kbps, and the source's own layout (folded to 5.1 when AC-3 cannot code it).
- `cut` — from 0 s to the end of the stream.

## What the encoder accepts

- WAV sample rates: AC-3 takes 32, 44.1 or 48 kHz (Table 5.6); E-AC-3 additionally takes the
  Annex E `fscod2` half rates 16, 22.05 and 24 kHz.
- The bit rate must be one of the 19 nominal AC-3 rates (Table 5.18), 32 through 640 kbps.
- `record` (and `live mode=channels`) encodes onto `layout=` — `stereo` by default, anything up
  to 7.1.4 — and places the endpoint's channels onto it by direction, not by index: a device
  narrower than the layout leaves the rest silent, a wider one folds down per §7.8. See
  [`layout=` and `codec=`](#layout-and-codec).
- The Atmos commands take 1 to 15 objects — the bed's LFE is the 16th, and TS 103 420 §8.3.2.2
  caps the total at 16.

## Command-specific notes

- **`transcode`/`metadata`/`normalize`/`cut`/`cat`** work on an already-encoded stream rather
  than on PCM — see [Commands → Stream tools](commands.md#stream-tools-an-encoded-stream-in-an-encoded-stream-out)
  for what each carries across and what it deliberately does not. Only `transcode` re-encodes.
  Every metadata option above that a stream tool does not name is ignored by it, the same way
  `mkv` ignores all of them.
- **`mkv`** reads format, packet boundaries, sample rate and channel count from the bitstream
  itself, so it cannot be told the wrong ones. E-AC-3 dependent substreams are grouped into their
  access unit and counted as the channels they render.
- **`spdif`** wraps an already-encoded AC-3 or E-AC-3 file's IEC 61937 bursts as a 2-channel
  16-bit PCM WAV, playable bit-exactly (100% volume, no mixing) into an S/PDIF or HDMI output to
  light up a receiver's Dolby Digital indicator. Detects AC-3 vs. E-AC-3 from the stream itself
  (`bsid`); E-AC-3's carrier runs at four times the content sample rate (WASAPI's own
  `make_eac3_format` convention), which is legal though unusual for a plain PCM16 file. The GUI's
  S/PDIF container option is this same command, run automatically as the second half of a
  two-command encode — see [GUI → Format & channels](../gui/format-and-channels.md).
- **`atmos`** encodes objects orbiting the room at different heights and rates as a 5.1 E-AC-3 bed
  plus JOC + OAMD side data (TS 103 420); FFmpeg reports the result as "Dolby Digital Plus + Dolby
  Atmos". **`atmos-encode`** does the same but makes each channel of a real source file an object
  instead of synthesizing motion. Its optional `[paths.txt]` (same format `atmos-path` reads)
  authors that motion instead of the default static, fanned-out placement — keyed by WAV channel
  index, so an object index the file doesn't mention keeps its default placement unchanged.
- **`atmos` mode**: `objects` (default) writes the JOC+OAMD container; `bed51` omits it so the
  5.1 bed still plays on a decoder that would otherwise refuse an object container it can't
  validate, instead of falling back to the bed on its own. `bed51` drops the TS 103 420 §8.3.1
  `addbsi` object marker with it, so a `bed51` stream reads as ordinary 5.1 E-AC-3 all the way
  out: no `Atmos complexity` line from `scan`, no Atmos extension in the `dec3` box `fmp4`
  builds, no `CHANNELS="<N>/JOC"` in its playlists, and no "+ Dolby Atmos" from FFmpeg. See
  [Atmos & JOC](../concepts/atmos-joc.md) for why a decoder can tell the difference at all.
- **`sign-objects`** (with **`signing-key=<path>`**): signs the object container's EMDF protection
  tag so a validating decoder reconstructs the objects instead of playing the bed. Honored by all
  three Atmos commands (`atmos`, `atmos-path`, `atmos-encode`). Off unless you pass both —
  `sign-objects` alone with no key is an error. The key may also come from
  `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` instead of `signing-key=`. The key is never
  stored by the tool; the algorithm is in-tree but the key is yours to provision. Full details in
  [Object signing](../concepts/object-signing.md).
- **`fast-mdct=off`**: every encode runs the §7.9.4 fast forward MDCT by default (the quality
  evidence that made it the default: ~3e-12 max relative coefficient error against the direct
  form, 331 dB direct-vs-fast end-to-end SNR, 0.000 dB SNR delta against an independent oracle
  at 192–448 kbps — see `tools/ci/quality_race.py fast-mdct`). `fast-mdct=off` forces the direct
  §8.2.3.2 reference form — the validation oracle — wherever the command encodes, including the
  `atmos*`, `record`, `live`, `eac3-sine` and `eac3-encode` (both its single-source and its
  `src=`/`map=` multi-source path). `eac3-encode` also has a second spelling: the bare
  `nofastmdct` token inside its `[tools]` positional argument reaches the same field, and wins
  over `fast-mdct=off` if both are given on the same command line. Typing `tools=nofastmdct` as
  if it were a trailing option does not parse, since any `=` token goes to the options parser,
  which has no `tools` key. The fast MDCT is not a coding tool, so `none`/`all` leave it alone.
  `eac3-silence` has no use for either spelling: it builds a silent access unit directly, with no
  forward transform in the loop to choose a path for. The bare `fast-mdct` word and the
  `fastmdct` tool token — the opt-in spellings from when this defaulted off — still parse and now
  name what already happens.
- **`fast-imdct=off`**: the decode-side mirror. `decode` runs §7.9.4 step 3 — the inverse
  transform's one O(N²) part — through an FFT by default. The kernel
  (`src/forge/src/core/fft_kernel.hpp`) is radix-4 throughout, ending on a single radix-2 stage
  only where log2(P) is odd — P = 128 and P = 512, not P = 64. The quality evidence that made it
  the default: 7.8e-14 max peak-normalized relative error against the direct evaluation at the
  transform level, and over 180-second real-material decodes 214.9 dB SNR agreement for AC-3 /
  284.7 dB for E-AC-3, with decodes 4.5–4.7× faster. `fast-imdct=off` forces the pseudocode's
  own direct sum — the reference form, and the oracle the fast path's tests validate against.
  Applies to both codecs; the `qc`, `levels` and playback decoders stay on the library default,
  where a ~1e-12 difference cannot move a reported figure. Encoded output never depends on this
  switch: the encoder's own internal inverse-transform uses are pinned to the direct form
  regardless. The bare `fast-imdct` word — the opt-in spelling from when this defaulted off —
  still parses and now names what already happens.
- **`mode=performance|reference`**: the two switches above as one statement of intent.
  `mode=reference` turns **both** fast paths off — the forward MDCT wherever the command encodes
  and the step-3 inverse in `decode` — so the whole run uses the spec's own direct evaluations:
  the forms the standard states, the forms every fast-path test validates against, and the forms
  to reach for when two runs must agree bit-for-bit with the spec's stated arithmetic (comparing
  against an external reference decoder sample-for-sample, regenerating validation fixtures,
  chasing a suspected transform defect). `mode=performance` — the default state, so passing it
  changes nothing — names the fast paths: same streams to within ~1e-12, encodes measurably
  faster and decodes 4.5–4.7× faster. Tokens apply in order, so
  `mode=reference fast-mdct=off` is redundant but harmless, and `mode=performance fast-imdct=off`
  runs a fast encode with a reference decode. `eac3-encode`'s `[tools]` positional still wins
  the forward-MDCT half if both are given, exactly as it does against `fast-mdct=off`.
- **`joc-domain=qmf|mdct`**: which domain JOC's reconstruction matrix is estimated in (on
  `atmos`, `atmos-path` and `atmos-encode`) and applied in (on `decode`). `qmf` — the default —
  is TS 103 420 §7.1's 64-subband complex filterbank, which is what §6.6.6 describes and what a
  licensed decoder runs. `mdct` selects the 256-bin MDCT approximation this project used before
  it had a filterbank: cheaper on the encode side, but about 5 dB worse per object (22.8 dB
  against 27.7–28.6 dB mean per-object SNR over four placements; 20.2 dB against 26.5 dB on
  moving objects), and correct only against a decoder given the same token. Use it to reproduce
  output from before 0.9.0, not for new material. Unlike `fast-mdct=off` / `fast-imdct=off` this
  is **not** part of `mode=` in either direction: those two are the same answer computed two
  ways, agreeing to ~1e-12, while these are different answers — see
  [Atmos & JOC](../concepts/atmos-joc.md#which-domain-the-matrix-lives-in). Note that the two
  domains do not have the same latency, so a `decode` writing objects with `objects_dir=` gets
  them 576 samples behind the bed under `qmf` and 256 behind under `mdct`.
- **`verify`**: `eac3-encode` only. Runs the encoder/decoder mirror self-check (`ac3::verify`,
  see [Validation](../verification.md#six-independent-checks)) over every access unit the command
  emits: each one is decoded with this project's own decoder as soon as it is encoded, and the
  decoder's model of it — per-substream, per-block bit offsets, decoded exponents, `bap`, delta
  correction, AHT gain mode and gains, and the coupling, enhanced-coupling and
  spectral-extension coordinates — is diffed against the encoder's own. The first disagreement
  refuses the run (exit 1) and names where the two sides parted company, down to the substream,
  block, coded stream and bin:

  ```
  error: verify: the encoder and decoder disagree about access unit 0
  frame 0 substream 0 block 2 channel 1: bap[10] encoder=8 decoder=9
  ```

  A clean run prints one extra line beside the usual summary and writes exactly the stream it
  would have written anyway — the check reads state the encoder already has and never steers a
  decision. Off by default because it decodes everything it encodes, which roughly doubles the
  work. What it buys is the class of defect a round trip cannot see: the two sides differing in
  a way the audio survives. That matters most for `ecpl`, `tpn`, `fscod2` and `714`, which have
  no external decoder to check against at all — see
  [Validation → where the oracles don't reach](../verification.md#where-the-oracles-dont-reach).
  `encode` (AC-3) has no equivalent token yet; its half of the same facility is library-only
  (`ac3::verify::MirrorEncoder`).
- **`keep-partial`**: `encode`, `eac3-encode` and `atmos-encode` refuse a frame that cannot fit the
  configuration mid-run just as they always have, but with `keep-partial` given, whatever frames
  were already encoded before that point are written to `<name>.partial.<ext>` (`out.ec3` →
  `out.partial.ec3`) rather than discarded — the run still exits non-zero and prints the same
  error either way, only what happens to the frames already produced changes. Off by default, the
  same as every other bare token here; a plain invocation with no `keep-partial` behaves exactly
  as it always has. Mirrors the GUI's own keep-partial-output preference (see
  [GUI → window layout](../gui/index.md#preferences)) rather than a separate idea — the same
  `.partial.` naming either way, so a file produced by either front end is named alike.

## Next

[Concepts → Atmos & JOC](../concepts/atmos-joc.md) if any of `JOC`, `OAMD`, or the object/bed
relationship above are unfamiliar; [Spatial & Atmos objects](../library/spatial-and-atmos.md) for
the library-level API these commands are thin wrappers over.
