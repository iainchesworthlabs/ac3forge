# ac3forge

A clean-room AC-3 and E-AC-3 encoder and decoder in C++23, implemented from the published
standards. It turns PCM — or mono sources placed and moved in 3D space — into AC-3, E-AC-3,
or E-AC-3 with Joint Object Coding elementary streams, and reads those streams back.

Nothing here links FFmpeg or any other codec library. The FFmpeg command-line tools are used
during development as an independent decoder to check output against; the build does not
depend on them.

!!! warning "Standards and trademarks"
    "Dolby", "Dolby Digital" and "Dolby Atmos" are trademarks of Dolby Laboratories. This
    project implements the openly published standards — ATSC A/52:2018 (of which E-AC-3 is
    normative Annex E), ETSI TS 102 366 and ETSI TS 103 420 — and is not affiliated with,
    endorsed by, or certified by Dolby Laboratories. Code and documentation use the technical
    names AC-3 and E-AC-3. Whether the patents reading on these formats matter for your use is
    your problem to assess, not something this project resolves.

!!! note "Status"
    The API is not stable — releases so far are 0.x betas; the
    [changelog](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) records what
    each contains. Green and required in CI on Windows (MSVC, clang-cl), Linux (GCC and Clang,
    x64 and arm64) and macOS (Homebrew LLVM) — CLI and GUI alike on every platform — plus an
    ASan+UBSan leg, clang-tidy static analysis, a line/branch
    coverage gate over the library, a per-platform gold-reference *quality* gate, dedicated
    Linux FFmpeg- and ADM-validation legs checking output *correctness*, and a required Android
    build leg. No leg remains experimental. See [building.md](building.md) for exact toolchain
    versions and what each CI leg covers.

## What it does

### Encoding

| | AC-3 (bsid 8) | E-AC-3 (bsid 16) |
|---|---|---|
| Coding modes | 1+1 dual mono, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2, each with or without LFE (1+1 never carries one) | the same, plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams |
| Programmes per stream | one | up to eight, through independent substreams (§E2.3.1.2's I0–I7) — the multi-language / associated-service shape of broadcast DD+. Each has its own layout, rate and dialnorm; labelling one as a service (`bsmod`) and mixing it against another is not there yet |
| Sample rates | 48, 44.1, 32 kHz | 48, 44.1, 32 kHz, plus the `fscod2` half rates 24, 22.05, 16 kHz (Annex E only) |
| Bit rates | CBR only — the 19 nominal rates of Table 5.18, 32–640 kbps | CBR (the same 19, per substream) or VBR — a quality target with optional min/max kbps bounds, per substream |
| Transform | long (512-point) or short (2x256-point) blocks, KBD window, chosen per block per channel by a §8.2.2 transient detector | same |
| Exponents | D15 / D25 / D45, strategy chosen per block from the reuse span (§8.2.8) | the same span rule, per channel per frame, written either as a Table E2.10 code (`expstre` 0) or as per-block strategies (`expstre` 1) — whichever the plan needs |
| Coupling | yes (§7.4), begin and end frequencies auto or pinned | yes (§E3.3) |
| Tool selection | coupling/rematrixing/delta always automatic, no toggle | `auto` picks coupling, spectral extension and AHT per frame from the per-channel rate **and** the frame's own spectrum — see [Encoding E-AC-3](library/encoding-eac3.md#how-auto-chooses) |
| Bit allocation parameters | §8.2.12's basic-encoder set with one measured departure, `dbpbcod` 3 | the same set, transmitted rather than inherited (`bamode` 1, 17 bits a frame) — Table E1.4's own defaults differ from §8.2.12's |
| Delta bit allocation | automatic (§7.2.2.6), like rematrixing below — no toggle | automatic, same as AC-3 |
| Dither substitution | `dithflag` decided per channel per block from content (§7.3.4) | the same, except in a frame using spectral extension |
| Rematrixing | yes, 2/0 (§7.5.3 minimum-power rule) | yes, 2/0 — the same rule, over Table 7.25's bands clamped to wherever coupling or spectral extension takes over |
| Annex E tools | — | spectral extension (§E3.6), enhanced coupling (§E3.5), adaptive hybrid transform with GAQ (§E3.4), transient pre-noise processing (§3.7) |
| Objects | panned to a 5.1 bed (no metadata survives) | OAMD + JOC in an EMDF container (TS 103 420) |

At 44.1 kHz, CBR needs non-integral frame sizes; the AC-3 encoder alternates between the two
Table 5.18 lengths on a Bresenham accumulator so the long-run rate is exact. E-AC-3 signals
`frmsiz` directly and needs no such alternation.

**Block switching's scope**: a §8.2.2 transient detector (cascaded biquad 8 kHz high-pass, a
256/128/64-sample peak-ratio tree) runs per full-bandwidth channel per block; a channel that
switches anywhere in the frame is excluded from coupling for that whole frame. On AC-3 that
exclusion is per channel — `chincpl` is a per-channel field, so the rest of the frame still
couples — while E-AC-3's coupling decision, and AHT on both, remain frame-wide all-or-nothing.
The LFE never switches (§8.2.2 defines the detector over full-bandwidth channels only).

**Dither's scope**: a bin the allocator gave no bits to is not transmitted, so the decoder
invents it — a true zero when `dithflag` is clear, a random sample scaled to that bin's own
exponent when it is set (§7.3.4). Both are wrong, in opposite directions: a run of true zeros is
a hole in the spectrum, and dither over a bin that really was near-silent is noise added to
nothing. The encoders decide per channel per block by comparing the two — the energy the decoder
will *not* receive against the energy the dither would put there instead — and dither only where
what is being replaced was at least as loud as the substitute. Digital silence is the limiting
case and always reads clear. A block-switched channel never dithers either, on the same grounds
Dolby's own encoder appears to use (`dithflag` is exactly `!blksw` in every block of the
reference stream in `tests/golden/external-baseline/`): a switched block's coefficient set is two
interleaved half transforms, so filling a zero-bit slot spreads noise across the transient the
switch exists to resolve. On E-AC-3 dither also stays off for any frame using spectral
extension — the encoder holds a reconstruction of the decoder's output there to scale the
extension bands, and the decoder's dither sequence is not reproducible from the encoder's side.

**Delta bit allocation's scope**: the encoder compares the coarse exponent-only masking curve
§7.2.2.2-7.2.2.5 derive against one built from the real, pre-quantization coefficient magnitude,
and corrects bands where the two clearly diverge (at least a full 6 dB Table 5.17 step). It is
skipped for the LFE channel (no such field exists for it). The coupling channel is in
§7.2.2.6's scope like any full-bandwidth channel on both generations, and `cpldeltbae` is
emitted whenever correction segments exist. Coupling no longer suppresses it on E-AC-3: a
coupled channel's own below-`cplstrtmant` baseband carries corrections like any other region.
Two narrowings remain there. An AHT stream carries none — the comparison was put on the AHT
axis, where its exponents actually live, and measured worse on every AHT-carrying point, because
the transform's job is to concentrate six blocks into one coefficient and the gap that opens is
that concentration rather than quantization error. And whether corrections are sent at all is
decided per frame against the rate fit: the frame is fitted with and without them and the higher
composite SNR offset wins, which is what keeps their side information from eating a 128 kbit/s
5.1 frame whose mantissas are already down to about a quarter of it. The decoder accepts delta
bit allocation on any channel, either generation, from any encoder.

### Metadata

| Field | Section | What it does here |
|---|---|---|
| `dynrng` | §7.7.1 | Per-block dynamic range control from an RMS-detected compressor on a piecewise-linear curve. Five profiles: `film-standard`, `film-light`, `music-standard`, `music-light`, `speech`. A/52 fixes the wire format and the intent but not the curve, so the profiles are this project's, not the standard's. |
| `compr` | §7.7.2 | Heavy compression as a limiter guaranteeing a peak ceiling in the §7.8 mono downmix. Rounds down, because nearest-code rounding can overshoot a ceiling by half a step. Its peak detector includes the previous frame's MDCT overlap. |
| `dialnorm` | §5.4.2.8 | Measured with ITU-R BS.1770-4 gated loudness and negated, or set directly. A/52 predates BS.1770 and leaves the measurement open. |
| Downmix levels | Tables 5.9/5.10, E1.2, D2.2–D2.6 | `cmixlev`/`surmixlev` in AC-3's own bsi; separate Lt/Rt and Lo/Ro levels plus a preferred-downmix indication in E-AC-3's `mixmdate` and in AC-3's Annex D `xbsi1`. |
| Programme mixing | Table E1.2, §E2.3.1.12–61 | The rest of `mixmdate`, written by the independent substream: programme and external-programme scale factors, the `mixdef` mixing-parameter block (premix compression, per-channel external scales, speech enhancement data), pan position for a mono or 1+1 programme, and per-block mixing configuration. This is what a receiver mixes an audio-description or commentary service against the main programme with. |
| Service and production | §5.4.2.2–28, Table E1.2 | `bsmod` (complete main through commentary and emergency — what ATSC A/53 and DVB key associated-service handling off), `dsurmod`, `dsurexmod`, `dheadphonmod`, `adconvtyp`, `audprodie`'s mixing level and room type, `copyrightb`, `origbs`, `langcod`, the 28-bit time code, and E-AC-3's `sourcefscod`. AC-3 carries them in bsi (the Surround EX, Headphone and A/D flags only under Annex D); E-AC-3 gathers the same set into `infomdat`. |
| Annex D alternate syntax | Annex D, `bsid` 6 | AC-3's two 14-bit `timecod` fields "have never been applied for their originally anticipated purpose" (§D1), so a `bsid`-6 stream spends them on `xbsi1`/`xbsi2` instead. Encode and decode both sides; §D3.2's promise holds, in that a legacy reader takes those bits for a time code it already ignores. |

### Decoding

The in-repo decoder shares its tables, bit-allocation engine, exponent decoding and IMDCT with
the encoder. It reads AC-3 (bsid ≤ 8) and E-AC-3 (bsid 11–16), including dependent substreams,
`chanmap`, and the §E3.8.2 render that lays a dependent's channels over the bed. A stream
carrying more than one programme is decoded one programme at a time — `DecoderConfig::programme`
picks which — since independent substreams are alternatives rather than layers. Every Annex E
coding tool decodes too — standard coupling (§E3.3), enhanced coupling (§E3.5, a full FFT-based
phase-restoring reconstruction over 22 sub-bands), spectral extension (§E3.6, including the
pseudo-random noise blend the standard requires but leaves the exact generator unspecified), the
adaptive hybrid transform with GAQ (§E3.4), and transient pre-noise processing (§3.7) —
individually or all stacked together, at every channel layout including 7.1.4.

Transient pre-noise processing holds a frame back per substream that uses it (see [What it does
not do](#what-it-does-not-do)), and that holding-back is not just a `decode_substream` detail:
`Eac3Decoder::decode_access_unit` assembles a whole access unit correctly even when only some of
its substreams set the flag, queuing whichever substreams release early rather than losing or
misaligning them against the one still catching up.

Downstream of the coded channels there is an **output stage** (`ac3/decoder/output.hpp`), off by
default so the decoders stay usable as a reference: §5.4.2.8 dialnorm normalisation onto the
−31 dBFS reference, §7.8's Lo/Ro, Lt/Rt and mono downmixes driven by the stream's own
`cmixlev`/`surmixlev` or `mixmdate` levels, optional LFE mixing, and §7.7's line and RF operating
modes — RF including the overload protection a fold needs but `compr` (which is computed for the
*mono* downmix) does not provide. Lt/Rt's surround sum is genuinely phase shifted 90°. Layouts
§7.8 has no fold for, because they predate nothing that could code them, are reduced to the
nearest acmod layout first rather than having their extra channels dropped. Verified against
FFmpeg's `-ac 2` decode of the same stream.

§7.10 **error concealment** is opt-in on the same config. A frame that will not decode can be
reconstructed from the previous block's overlap — repeated and faded, or muted through the
codec's own window — instead of leaving a hard discontinuity in the PCM, with the substitution
reported on the result. For E-AC-3, an access unit whose *dependent* substream will not decode
renders its bed rather than failing outright.

`DecoderConfig::diagnostics` is a **consumer-facing diagnostic sink** — a plain function pointer,
no allocation, usable from the minimum-footprint decoder profile — for the recoverable events
that concealment's own return value does not carry: a CRC that failed (reported the moment the
check runs, whether or not concealment goes on to recover the frame) and an EMDF payload id this
decoder does not interpret (§H.2.2, skipped without failing the frame). Null by default, at the
cost of one branch per occurrence when it is not set.

### Inspection

Decoding a stream and *describing* one are different jobs. `ac3::io::probe` (`ac3cli probe`)
does the second: it reads the bitstream and reports what the stream declares — bsid, sample
rate including Annex E's `fscod2` half rates, `acmod`/`lfeon` and the resolved layout, `bsmod`,
`chanmap`, the substream map, `numblkscod`, frame and access-unit counts, duration, measured bit
rate and VBR spread, `dialnorm`/`compr`/`dynrng` presence and ranges, EMDF payload ids, OAMD/JOC
with `complexity_index` and the object/bed configuration, whether an authenticity tag is present,
CRC validity per frame, and how often each coding tool was used — without reconstructing a single
sample.

It reads in two tiers. `ac3::io::read_frame_header` answers for every syncframe whether or not
its audio is readable, so a stream this decoder refuses is still described in full; the real
decoders then run under `DecoderConfig::skip_reconstruction`, which parses every field exactly
as a full decode does but stops before the inverse transform, for everything only the bitstream
body carries. An opt-in per-block dump reports which Annex E tools each block used and what
exponent strategy each stream carried — the in-repo counterpart of
`tools/references/eac3_parse.py`, which until now was the only field-level dump in the project
and shipped with nothing.

`json=1` emits a versioned JSON document instead of the table; its schema is a stable contract,
documented in [Commands](cli/commands.md). Memory is flat in the length of the stream on both
sides — the input is pulled through a fixed window and the per-frame dump is written as the walk
produces it.

### Other

| Component | What it is |
|---|---|
| `ac3::io::scan` | Finds access-unit boundaries in a raw elementary stream and reports what it renders, without being told — grouped by programme, so a stream with two independent substreams describes both rather than one at twice the frame rate. `ac3::io::read_frame_header` is the same per-syncframe walk exposed on its own. |
| `ac3::io::probe` | The stream description above (`ac3cli probe`), as a human table or a versioned JSON contract. |
| `matroska::matroska` | A standalone MKV muxer. Links nothing from `ac3::forge` and knows nothing about AC-3. |
| `mp4::mp4` | A standalone MP4/ISOBMFF muxer, same shape as `matroska::matroska`. `ac3::io::build_codec_config_box` builds a spec-correct `dac3`/`dec3` sample-entry box (ETSI TS 102 366 Annex F), Dolby Atmos extension included, straight off the bitstream. |
| `mpegts::mpegts` | A standalone MPEG-2 Transport Stream muxer (PAT + PMT + one PES-wrapped elementary stream), identifying AC-3/E-AC-3 per either broadcast profile: DVB's ETSI EN 300 468 Annex D descriptors, or ATSC's `stream_type` 0x81/0x87 with A/52 Annex A and Annex G's own. Both descriptors' identification fields are filled in from what `ac3::io::scan` reads off the bitstream. Links nothing from `ac3::forge` beyond the A/52 field values it is handed. |
| `ac3::io::strip_objects` | Removes the JOC/OAMD object layer from a Dolby Digital Plus stream at the bitstream level — no decode, no re-encode, bit-identical bed audio — turning a DD+ JOC stream into the plain DD+ 5.1 rendition HLS delivery wants beside it. `ac3cli strip-objects`. |
| `ac3adm::ac3adm` | A standalone BW64/RF64 + Audio Definition Model reader (container and metadata parsing only — codec-blind by design). Parses the container (ITU-R BS.2088-1) and the ADM XML graph (ITU-R BS.2076-2) on top of the vendored libbw64/libadm (github.com/ebu); links nothing from `ac3::forge` and knows nothing about AC-3. `ac3::admbridge` maps its object/bed graph onto `ac3::oba::AtmosEncoder`'s input shape, driven end to end by `ac3cli atmos-adm` — see [ADM bridging](library/adm-bridge.md). Opt-in (`-DAC3FORGE_BUILD_ADM=ON`, needs Boost) — the one component in this project with a third-party dependency. |
| `mp4::fragment` + `mp4/hls.hpp` + `mp4/dash.hpp` | Fragmented MP4/CMAF segmenting (init segment + media segments, ISO/IEC 14496-12 §8.8 / ISO/IEC 23000-19) plus HLS media/master playlist and DASH `AdaptationSet` signaling helpers for the same segments — correct `CODECS`/`codecs` (RFC 6381) and, for Dolby Atmos, HLS's `CHANNELS="<N>/JOC"` (Apple's HLS Authoring Specification), with a multi-rendition master playlist for the paired 5.1 fallback that specification asks for. `ac3cli fmp4` wraps the whole thing. |
| `ac3::iec61937` | S/PDIF burst packing: AC-3 byte-exact against FFmpeg's `spdif` muxer; E-AC-3 (`Eac3BurstPacker`) verified against FFmpeg's `spdif_header_eac3` and Microsoft's own IEC 61937 documentation (both independently fetched, not recalled — see the caveats below). |
| `ac3::audio` | Live input/loopback capture — WASAPI on Windows, ALSA or PipeWire on Linux, CoreAudio on macOS (input only, no loopback) — through a lock-free SPSC ring. |
| `ac3::audio::PassthroughSink` | Exclusive-mode/direct bitstream output, AC-3 or E-AC-3 — WASAPI on Windows, ALSA or PipeWire on Linux, CoreAudio on macOS, JNI-bridged `AudioTrack` on Android. See the caveats below (Windows, Android and Raspberry Pi hardware-confirmed; the CoreAudio backend is not, and PipeWire's negotiation needs a compressed codec enabled on the target node — see [Linux audio](building.md#linux-audio)). |
| `ac3::audio::MonitorSink` | Shared-mode PCM playback — WASAPI, ALSA, PipeWire or CoreAudio: a non-bitstreamed preview/monitor path that decodes what is being encoded and plays it back on an ordinary output. Confirmed against real Windows hardware. |
| `ac3::analysis` | Peak/RMS metering with console ballistics, and the Gerzon energy vector over the BS.775 ring. |
| `ac3::meta::qc` | Bitstream-aware loudness QC (`ac3cli qc`): decodes a stream, measures it with the real BS.1770-4/EBU Tech 3342 meter, and compares against the stream's own embedded `dialnorm`/`compr` and, optionally, a named delivery-spec gate — EBU R 128 s2, ATSC A/85, or Netflix's Sound Mix Specifications, each preset's target/tolerance/true-peak ceiling cited from its own primary source. |

## What it does not do

The full picture — verification gaps, quality numbers, and exactly what has and has not been
confirmed against real hardware — is [Validation](verification.md), not here. Two gaps are
load-bearing enough to flag up front:

!!! warning "Objects will not decode as objects in Dolby's decoder"
    DD+ JOC gates object decoding on a keyed, sequence-bound HMAC-SHA-256 tag in the EMDF
    `protection` field — which the standard itself leaves "implementation dependent and not
    defined" — keyed on a secret embedded in decoder binaries. Streams from here are
    spec-correct (FFmpeg validates them, the bed decodes bit-exactly, Dolby's own parser reports
    `atmos=true`) but this project ships no key, so by default they are unsigned and Dolby's
    decoder falls back to the 5.1 bed; an operator who has a key can sign with it — see
    [Object signing](library/signing.md). The gate is authenticity, not conformance. Forging
    the tag is deliberately not attempted. What is verified about reconstruction is the
    mathematics: §6.6.6 applied per band recovers each object to better than −20 dB.

!!! warning "Linux audio output has reached a real receiver on one machine only"
    The ALSA backend was verified headless (including against ALSA's software `null` device,
    under ASan+UBSan), and has since bitstreamed to a real receiver for real on exactly one
    machine: a Raspberry Pi 4B driving an Atmos-capable AVR over HDMI, everything from plain AC-3
    through signed Atmos/JOC locking correctly (see
    [Raspberry Pi](platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver), which is
    also where the vc4-hdmi device-classifier bug that run found is written up). No other Linux
    machine, sound card or receiver has been tried, and the **PipeWire** backend has not reached a
    receiver at all — its passthrough negotiation is real but needs a compressed codec enabled on
    the target node by the session manager first. Treat the Pi result as one confirmed
    configuration, not as ALSA-on-Linux generally.

Enhanced coupling and transient pre-noise processing have no external decode oracle at all —
not even the FFmpeg-can't-but-the-in-repo-decoder-can situation 7.1.4 is in, since FFmpeg's own
Annex E parser has never read either tool's syntax — so `tools/ci/quality_race.py`'s CI gate scores
both through this project's own decoder instead (see
[Validation](verification.md#where-the-oracles-dont-reach)). That same gap is why neither is in
the `auto` tool set, though only one of them earned its way out: enhanced coupling measures
*better* than standard coupling on real programme material at every bitrate and layout tried and
is kept out purely so `auto` produces streams FFmpeg can read, while transient pre-noise
processing measured 6.5–24 dB worse than leaving the audio alone over exactly the samples it
touches, at every bitrate, with no perceptual movement either way — a reference-correctness tool
rather than a quality one. [Encoding E-AC-3](library/encoding-eac3.md#what-auto-will-not-choose)
carries both measurements. Transient pre-noise processing's
one-frame decoder buffering is an API characteristic, not a gap; [Decoding](library/decoding.md)
covers it. Variable bit rate is E-AC-3 only — AC-3's frame size indexes Table 5.18 rather than
stating a word count directly, so it has no equivalent and stays CBR.

## Where to go next

- **Getting started** — [Quick start](quickstart.md): clone to first encode in under ten
  minutes.
- **Concepts** — [Overview](concepts/index.md): AC-3, E-AC-3 and the Atmos/JOC object layer
  explained.
- **Validation** — [how output is checked](verification.md): quality numbers, oracle coverage,
  and exactly where it runs out.
- **Library** — [Conventions](library/index.md): the public C++ API, with
  [compiled examples](library/examples.md).
- **CLI reference** — [Overview](cli/index.md): `ac3cli`'s commands and option grammars.
- **GUI guide** — [Window layout](gui/index.md): `ac3gui`, the Qt Quick front end.
