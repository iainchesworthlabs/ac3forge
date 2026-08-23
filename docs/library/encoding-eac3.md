# Encoding E-AC-3

## E-AC-3: `ac3::eac3::FrameEncoder`

`ac3/encoder/eac3_frame.hpp`. Same shape, different container. E-AC-3 is not an AC-3 variant:
no `crc1`, an arbitrary 11-bit `frmsiz` instead of a size table (so the 44.1 kHz padding
alternation disappears), and exponent strategies for all six blocks hoisted into a frame-level
`audfrm`.

```cpp
// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
// state (PREfast's C6262).
auto encoder = std::make_unique<ac3::eac3::FrameEncoder>(ac3::eac3::FrameConfig{
    .bitrate_kbps = 192,
    .acmod = ac3::Acmod::k2_0,
});

std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
const auto views = views_of(pcm);

std::vector<std::byte> stream;
for (int frame = 0; frame < 31; ++frame) {
    fill_tones(pcm, tones, frame, 48000.0);
    const auto encoded = encoder->encode_frame(views);
    if (!encoded) {
        return 1;
    }
    stream.insert(stream.end(), encoded->begin(), encoded->end());
}
```

`FrameConfig` carries nearly everything `EncoderConfig` does, plus the Annex E tools. Two AC-3
fields do not carry over as-is: there is no `cplendf` — the coupling end frequency is derived
(the top of the coded spectrum, or from `spxbegf` when spectral extension is on, §E3.3.1) —
and `chbwcod` defaults to a fixed 60 rather than AC-3's auto-from-bitrate −1.

One field widens instead: `FrameConfig::sample_rate` also accepts the three Annex E half rates —
`k24000`, `k22050`, `k16000` (24/22.05/16 kHz). For those the encoder writes `fscod2` in place
of `numblkscod` (§E2.3.1.3, the block count is then implicitly six), and the reduced rate reuses
its double-rate parent's bit-allocation tables (§E2.3.1.4). The CLI maps the plain rate numbers
onto them. Classic AC-3 has no `frmsizecod` row for a reduced rate, so `ac3::FrameEncoder`
rejects them outright.

| Field | Default | Notes |
|---|---|---|
| `spx`, `spxbegf` | `false`, -1 | Spectral extension (§E3.6). Above the extension frequency nothing is coded: the decoder copies a lower band up, blends noise, and scales to a transmitted envelope. Cheaper and cruder than coupling, so the two stack. |
| `spx_atten`, `spxattencod` | `true`, -1 | The §E3.6.4.2.3 notch across the seam. Six bits per channel per frame. |
| `aht`, `gaqmod` | `false`, -1 | Adaptive hybrid transform (§E3.4): a second 6-point DCT down each bin across the frame's six blocks. Decided per channel per frame — setting the flag permits it, not forces it. |
| `coupling`, `cplbegf` | `false`, -1 | §E3.3. With `spx` also on, §E3.3.1 derives the coupling end frequency from `spxbegf`. |
| `enhanced` | `false` | §E3.5: enhanced coupling instead of standard — 22 sub-bands, amplitude/angle/chaos-quantized coordinates and a phase-restoring reconstruction built on a full DFT, rather than a single per-band scale factor. Only meaningful with `coupling` also set (`cpl+ecpl`); combines with `spx` the same way standard coupling does. This encoder fits real amplitude/angle coordinates per band (an exact 2-variable linear least squares, since §3.5.5.4's reconstruction is linear in the complex gain the pair expresses) and chooses chaos by searching its 8 legal codes against the decoder's own deterministic de-correlation sequence. Two genuinely different channels forced into one narrow coupling band still cost quality — a single coordinate per band has a real, structural limit on what it can separate — but it is no longer the amplitude-only fit's all-or-nothing loss. |
| `transient_prenoise` | `false` | §3.7 (`tpn`): a post-IMDCT correction that overwrites the pre-echo ahead of a detected transient with a synthesized copy of the clean audio just before it. Reuses the same transient detector block switching relies on, so it only has an effect on channels/frames that also block-switch. See [Decoding](decoding.md) for the one-frame decoder-side latency this introduces and the `flush()` call it requires. |
| `fast_mdct` | `true` | The §7.9.4 fast N/4-FFT forward MDCT instead of the direct §8.2.3.2 evaluation — a performance choice, not a coding tool: nothing in the bitstream's syntax changes, only how the coefficients were computed (verified ~3e-12 max relative error against the direct form; 0.000 dB SNR delta against an independent oracle at 192–448 kbps). `false` forces the direct reference form, which stays maintained as the oracle the fast path is validated against — the CLI spells that `tools=nofastmdct`. Only the long transform accelerates today; a block-switched channel's short transforms always run direct. |
| `mixing` | none | The `mixmdate` group (Table E1.2). E-AC-3 dropped `cmixlev`/`surmixlev` from `bsi` entirely, so without this the stream carries no downmix levels at all. |
| `strmtyp`, `substreamid`, `chanmap`, `last_dependent` | independent, 0, none, false | Substream identity. Set by `AccessUnitEncoder`; you rarely touch these directly. |
| `oba_complexity_index` | none | TS 103 420 §8.3 object count in `addbsi`. This is the marker FFmpeg keys its "Dolby Digital Plus + Dolby Atmos" report off. |

> The in-repo decoder reads every one of these tools, individually or stacked together, at every
> layout including 7.1.4 — see [Decoding](decoding.md) for the decode-side picture, and the
> verification-gap table in [Validation](../verification.md#where-the-oracles-dont-reach) for
> which tools have an external oracle.

Block switching (§8.2.2/§7.9) is automatic here too — no config field. A channel that switches
anywhere in the frame is excluded from both coupling (same reasoning as AC-3's) and, for this
generation only, from AHT for that frame: AHT's own "stationary" premise (§E3.4, the opposite of
what triggered the switch) already selects against a switching channel most of the time, but the
exclusion is explicit rather than relying on that correlation.

Rematrixing (§7.5.3) is automatic too, `acmod` 2/0 only, no config field — the same minimum-power
decision AC-3's own encoder makes (see [Encoding AC-3](encoding-ac3.md#rematrixing)), over the same Table 7.25
bands. Annex E §3.3's "Modifications to Previously Defined Parameters" only changes how many of
those bands are active (`nrematbd`, accounting for coupling, enhanced coupling and spectral
extension all separately taking over the top of the spectrum) — the boundaries and the decision
rule are untouched, so nothing here needed reinventing beyond that band count and clamping the
last active band to wherever this channel's own coding actually stops.

The bit allocation parameters are transmitted rather than inherited (`bamode` 1). Table E1.4's
own `bamode == 0` defaults are not §8.2.12's basic-encoder set — most of the two agree, but
`floorcod` is 0x7 against §8.2.12's 4 — so "inherit" was never the same thing as "what AC-3
does". Sending them costs `baie` plus eleven bits in block 0 and one bit in each of the other
five, 17 a frame; it buys `dbpbcod` 3, the one departure the AC-3 encoder measured its way to
(see [Encoding AC-3](encoding-ac3.md)), which lifts the masking curve over bands quieter than
`dbknee` and sends their bits to bands that hold energy. `floorcod` stays at 0x7: it is the
lowest of the eight and never binds, which the same sweep confirmed here as it had for AC-3.

Dither substitution (§7.3.4) works exactly as it does on AC-3 — see
[Encoding AC-3](encoding-ac3.md#dither-substitution) for the rule — with `dithflage` set so the
per-channel flags are transmitted rather than defaulting to on. One narrowing is specific to this
generation: dither is held off entirely for any frame that uses spectral extension. The encoder
holds a reconstruction of what the decoder will produce there, because the extension bands are
scaled to match the copy source's own energy, and the decoder's dither values are not
reproducible from the encoder's side — the sequence a bin receives depends on how many zero-bit
bins the decoder walked before it, across every stream and block. Rather than shadow that
traversal order, the tool that would disturb it is switched off. An AHT channel is left out of
the judgement too: its zero-`hebap` bins reconstruct as literal zero whatever the flag says.

`FrameConfig::dialnorm2` (see "Dual mono" in [Encoding AC-3](encoding-ac3.md)) works exactly
the same way here: set it alongside `dialnorm` when `acmod` is `kDualMono`. Dual mono is always a
lone independent substream with no dependents — 1+1 has no bed/dependent split to make — so
`AccessUnitEncoder` gives Ch2 its own `RangeController`/`HeavyCompressor` too, measured on the
independent substream's own two channels the same way it measures Ch1's. It has no VBR
implications either: dual mono is orthogonal to CBR/VBR, since `vbr` only changes how the frame's
*size* is decided, not how many programmes it carries.

## Variable bit rate: `FrameConfig::vbr`

E-AC-3's `frmsiz` states the frame's word count directly rather than indexing a table, so unlike
AC-3 a frame is free to be a different size than the one before it. Setting `vbr` switches a
`FrameConfig` from CBR (size fixed from `bitrate_kbps`) to VBR (size follows the content, at a
chosen quality):

```cpp
ac3::eac3::FrameEncoder encoder{{
    .bitrate_kbps = 192,  // not read once vbr is set — see below
    .acmod = ac3::Acmod::k2_0,
    .vbr = ac3::eac3::VbrConfig{
        .quality = 0.4,
        .max_kbps = 320,  // optional ceiling
    },
}};
```

| `VbrConfig` field | Default | Notes |
|---|---|---|
| `quality` | `0.5` | `[0, 1]`, linearly maps onto the encoder's own SNR-offset search space. Encoder-relative, not a perceptual scale — and **not linear in bit cost**: cost rises steeply in roughly the top third of the range, so a high quality with no `max_kbps` bound will often refuse ordinary multi-channel material outright (`FrameError::kInvalidBitrate`) rather than produce an oversized frame. |
| `min_kbps`, `max_kbps` | none, none | Optional hard bounds, same unit as `bitrate_kbps`. When the quality target would need more words than `max_kbps` allows, the encoder falls back to the same search CBR uses, budgeted against the ceiling — so a bounded VBR frame is never worse than the best CBR could do at that rate. `min_kbps` is a pure floor: `finish_frame`'s own padding covers the gap. |
| `nominal_kbps` | none | Drives the `cplbegf`/`spxbegf` frequency defaults in place of a fixed target rate. Defaults to `max_kbps` if set, else 192. A caller who wants today's CBR tool behaviour at some quality supplies the same number they would have passed as `bitrate_kbps`. |

`bitrate_kbps` itself is not read on the encode path at all once `vbr` is set: when neither
`nominal_kbps` nor `max_kbps` is given, the frequency defaults fall back to the fixed
`kVbrDefaultNominalKbps` (192), not to `bitrate_kbps`.

`AccessUnitConfig` needs no separate VBR field: each substream's own `FrameConfig::vbr` carries
what it needs, and `plan::eac3_config()` shares one `VbrConfig` across every substream of a
`plan::Plan`, halving `min_kbps`/`max_kbps`/`nominal_kbps` for dependents the same way it already
halves `bitrate_kbps` — substreams occupy one frame period, not one frame.

Silent frames (`build_silent_frame`) and AC-3 (`plan::Codec::kAc3`) both reject a `vbr` config
outright: silence has no content to size a quality target against, and AC-3's `frmsizecod` has no
free word count to vary in the first place.

## Wide layouts: `ac3::eac3::AccessUnitEncoder`

Anything past 5.1 rides in *dependent substreams* beside a self-sufficient 5.1 bed. Every
substream codes the same 1536 samples of the same programme; a dependent contributes only its
own channels, its `chanmap`, and its share of the bit rate.

```cpp
// The bed is self-sufficient: a decoder that reads only the independent
// substream gets a complete 5.1 programme.
ac3::eac3::AccessUnitConfig config;
config.independent = {
    .bitrate_kbps = 384,
    .acmod = ac3::Acmod::k3_2,
    .lfe = true,
};
// Each dependent gets its own slice of the rate — substreams share a frame
// period, not a frame — and a Table E2.5 chanmap naming where its channels
// belong. Per §E3.8.2 the locations that collide with the bed replace it
// and the rest extend the layout.
config.dependents.push_back({
    .bitrate_kbps = 192,
    .acmod = ac3::Acmod::k2_2,
    .chanmap = ac3::eac3::chanmap::k71Rear,  // Ls, Rs, Lrs, Rrs
});
config.dependents.push_back({
    .bitrate_kbps = 192,
    .acmod = ac3::Acmod::k2_2,
    .chanmap = ac3::eac3::chanmap::kTopQuad,  // Vhl, Vhr, Lts, Rts
});

ac3::eac3::AccessUnitEncoder encoder{config};
```

Channels are grouped by substream in transmission order: the independent's first in Table 5.8
order with LFE last, then each dependent's in the order its `chanmap` names them.
`encoder.channel_count()` is the total, and the span count `encode_access_unit` expects.

```cpp
const auto unit = encoder.encode_access_unit(views);
// unit->bytes is the wire order already; substream_bytes records the
// per-substream boundaries, which crc2 is computed over.
stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
```

Full program: [`examples/encode_eac3.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_eac3.cpp).

Constraints: at most 8 dependents; a dependent may not disagree with its parent on sample
rate; and the locations a `chanmap` names must add up to the channels its `acmod` and `lfeon`
actually code, or you get `FrameError::kInvalidChannelMap`. A single dependent codes at most 5
full-bandwidth channels, which is why 7.1.4 — six channels beyond the bed — needs two.

Useful `chanmap` constants (`ac3/core/eac3_tables.hpp`, Table E2.5):

| Constant | Channels | Gives you |
|---|---|---|
| `k71Rear` | Ls, Rs, Lrs, Rrs | 7.1 (the surrounds replace the bed's, the rears are new) |
| `k512Height` | Vhl, Vhr | 5.1.2 |
| `kTopQuad` | Vhl, Vhr, Lts, Rts | 5.1.4 |
| `k71Rear` + `kTopQuad` | both of the above | 7.1.4, in two dependents |

`chanmap::expand(map)` turns a map into a `Layout` you can iterate, and `chanmap::name`
gives each location's short name.

## More than one programme

Dependent substreams widen *one* programme. §E2.3.1.2 also allows up to eight **independent**
substreams (I0–I7) in one elementary stream, and that is a different thing entirely: each is a
self-sufficient programme with its own layout, rate and metadata, and a receiver plays one of
them. Broadcast DD+ uses it for the services A/52 §5.4.2.2 names — a second language, an audio
description, a commentary.

`AccessUnitConfig::additional` carries them. `independent`/`dependents` stay the first
programme; each entry of `additional` is a `ProgrammeConfig` with an independent substream and
dependents of its own.

```cpp
ac3::eac3::AccessUnitConfig config;
config.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2,
                      .lfe = true, .dialnorm = 27};
// I1: a mono commentary, levelled independently of the mix it plays against.
config.additional.push_back({.independent = {.bitrate_kbps = 96,
                                             .acmod = ac3::Acmod::k1_0,
                                             .dialnorm = 20}});
```

`substreamid` is assigned by position — the first programme is I0, `additional[0]` is I1 — the
same way a dependent's id comes from its position in `dependents`; `FrameConfig::substreamid` is
not read.

`encode_access_unit` takes every programme's channels in one call, the first programme's first,
in the same order the substreams go on the wire. Rates add rather than divide: substreams share
a frame period, not a frame, so `access_unit_words` is the sum across every programme.

Each programme keeps its own §7.7 measurement — its own `RangeController`/`HeavyCompressor`,
measured on its own independent substream — because dialnorm and DRC are properties of a
programme. Sharing one measurement across two would level a commentary by the main mix. Per
programme, too: the §E3.8.2 16-channel cap, and the metadata a `FrameConfig` carries.

Constraints: at most 8 programmes; every substream of every programme must agree on the sample
rate, since they all code the same frame period. An Atmos EMDF container still rides in the last
substream of the **first** programme (TS 103 420 §8.2) — the objects belong to a programme, so a
later programme's substreams are never it.

The CLI authors a second programme with `programme2=<file>` plus `programme2-layout=`,
`programme2-bitrate=` and `programme2-dialnorm=`; see
[docs/cli/commands.md](../cli/commands.md).

One caveat worth knowing before you ship such a stream: **FFmpeg cannot read it at all**, and not
just the second programme — see
[docs/verification.md](../verification.md#where-the-oracles-dont-reach).

---

See also: [Metadata](metadata.md) — mix-level and DRC fields shared with AC-3, plus the
E-AC-3-only `mixing` group; [Encoding AC-3](encoding-ac3.md) — the single-substream base case.
