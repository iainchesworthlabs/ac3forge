# Development status

At-a-glance implementation status for every codec, coding tool, metadata field, and related
bitstream surface this repository owns — the companion to
[Capabilities and limitations](capabilities.md).

| Page | Role |
|---|---|
| **This page** | Status of each feature: done, partial, or not started |
| [Capabilities](capabilities.md) | What each shipped feature does, with the spec sections and the limitations that shape use |
| [Validation](../verification.md) | How claims are checked, and where external oracles do not reach |
| [Roadmap](../roadmap.md) | Candidate work and history behind stable IDs (`IM5`, `EQ10`, …) |
| [Application coverage](application-coverage.md) | Which applications expose each broad library capability |

[CONTRIBUTING.md](../contributing.md) still makes [Capabilities](capabilities.md) and
[Validation](../verification.md) the authority when a capability lands or a limitation is found.
Update those first; refresh the matching row here so the status table stays a summary rather than
a second capability record.

**Legend:** 🟢 Completed • 🟡 Partial / in progress • 🔴 Not started / refused / out of scope

Statuses describe the *bitstream and API surface*, not application polish. A green row can still
carry a note about an oracle gap, an intentional `auto` exclusion, or an interoperability caveat —
those details live in [Capabilities](capabilities.md) and [Validation](../verification.md).

The tables below were cross-checked against the cited standards (A/52 / TS 102 366 including
Annexes D–H and F, TS 103 420, TS 103 190-1/-2, ST 2098-2, BS.2076 / BS.2088, IAMF v1.1.0, and
the carriage specs wired in-tree). Open gaps against those texts are collected again under
[Standards cross-check](#standards-cross-check).

---

## AC-3 (A/52 / TS 102 366)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Encoder** | Syncframe / BSI / audio blocks (bsid ≤ 8) | 🟢 | High | Essential | Shared tables and bit allocation with the decoder |
| | Coding modes 1+1, 1/0, 2/0, 3/2 ± LFE | 🟢 | High | Essential | Named layouts and CLI coverage |
| | Coding modes 3/0, 2/1, 3/1, 2/2 | 🟡 | Low | Optional | Decode and `FrameConfig::acmod` accept them; no named-layout / CLI encode path for every acmod |
| | Sample rates 48 / 44.1 / 32 kHz | 🟢 | High | Essential | 44.1 kHz uses Bresenham frame-size alternation |
| | CBR bit rates (Table 5.18, 32–640 kbps) | 🟢 | High | Essential | AC-3 has no VBR |
| | Block switching (§8.2.2) | 🟢 | High | Essential | Per-channel transient detector |
| | Exponents D15 / D25 / D45 | 🟢 | High | Essential | Strategy from reuse span |
| | Coupling (§7.4) | 🟢 | High | Essential | Auto begin/end; per-channel `chincpl` |
| | Rematrixing 2/0 (§7.5.3) | 🟢 | Medium | Important | Minimum-power rule |
| | Delta bit allocation (§7.2.2.6) | 🟢 | Medium | Important | Auto; skipped for LFE |
| | Dither (`dithflag`, §7.3.4) | 🟢 | Medium | Important | Content-driven per channel per block |
| | Bit allocation parameters (§8.2.12) | 🟢 | High | Essential | Basic-encoder set; `dbpbcod` 3 |
| | Objects as 5.1 bed pan | 🟢 | Low | Optional | No object metadata survives — see Atmos for JOC |
| **Metadata** | `dynrng` (§7.7.1) | 🟢 | High | Essential | Five project profiles |
| | `compr` (§7.7.2) | 🟢 | High | Essential | Mono-downmix peak ceiling |
| | `dialnorm` (§5.4.2.8) | 🟢 | High | Essential | BS.1770-4 or direct |
| | Downmix levels (`cmixlev` / `surmixlev`) | 🟢 | High | Essential | Tables 5.9 / 5.10 |
| | Service / production BSI (`bsmod`, `langcod`, timecode, `copyrightb`, `origbs`, `dsurmod`, …) | 🟢 | Medium | Important | Encode, decode, probe |
| | Annex D Surround EX / Headphone / A/D (`dsurexmod`, `dheadphonmod`, `adconvtyp`) | 🟢 | Medium | Optional | Via `xbsi2` when bsid 6; E-AC-3 `infomdat` otherwise |
| | Annex D alternate syntax (bsid 6, `xbsi1` / `xbsi2`) | 🟢 | Medium | Optional | Both ways; §D3.2 legacy-reader promise holds |
| **Decoder** | Full AC-3 reconstruction | 🟢 | High | Essential | Shared core with encoder; FFmpeg oracle |
| | CRC1 / CRC2 validation | 🟢 | High | Essential | Leading CRC1 solved on encode |
| | §7.8 / dialnorm output stage | 🟢 | High | Essential | Opt-in; Lo/Ro, Lt/Rt, mono, RF |
| | §7.10 error concealment | 🟢 | Medium | Important | Opt-in repeat / mute |
| | Consumer diagnostics sink | 🟢 | Low | Optional | CRC fail and unknown EMDF id |

---

## E-AC-3 (Annex E)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Encoder** | Independent + dependent substreams | 🟢 | High | Essential | `AccessUnitEncoder` for wide layouts |
| | Layouts through 7.1.4 (`chanmap`) | 🟢 | High | Essential | 7.1.4 = two dependents |
| | Multi-programme authoring (I0–I7) | 🟢 | Medium | Important | All eight programmes; each with own layout, dialnorm and full `mixmdate`/`bsmod` metadata — sample rate and block count shared across the stream (§E2.3.1.2 requires it) |
| | Associated-service `bsmod` / `mainid` labelling | 🟡 | Medium | Important | Written, read back and validated against the stream's own `bsmod` (`mpegts::parse_service_descriptor`); consumed by Hearth's container facts, not yet surfaced in `ac3cli probe` |
| | Sample rates + `fscod2` half rates | 🟢 | Medium | Important | Enc/dec complete; no external PCM oracle for half rates |
| | CBR and VBR (per substream) | 🟢 | High | Essential | VBR E-AC-3 only; ABR mode shipped |
| | Short syncframes (`numblkscod` 0–2) + `convsync` | 🟢 | Medium | Important | Including object layer scaling; `eac3_latency()` still ignores short frames |
| | Exponent strategies (`expstre` 0/1) | 🟢 | High | Essential | Table E2.10 or per-block |
| | Standard coupling (§E3.3) | 🟢 | High | Essential | In `auto` |
| | Spectral extension (§E3.6) | 🟢 | High | Essential | In `auto` |
| | AHT + GAQ (§E3.4) | 🟢 | High | Essential | In `auto`; delta BA suppressed on AHT streams (measured) |
| | Delta bit allocation (§7.2.2.6 under Annex E) | 🟢 | Medium | Important | Including coupling channel; closed-loop vs rate fit |
| | Enhanced coupling (§E3.5) | 🟢 | Medium | Important | Enc/dec complete; kept out of `auto` (FFmpeg cannot read); no external oracle |
| | Transient pre-noise (§3.7) | 🟢 | Low | Optional | Enc/dec complete; kept out of `auto` (measured loss); one-frame hold-back; last AU can be lost at EOF |
| | Bit allocation transmitted (`bamode` 1) | 🟢 | High | Essential | Table E1.4 defaults |
| | Closed-loop `auto` tool selection | 🟢 | High | Essential | Spectrum-aware cpl / spx / aht only |
| **Metadata** | `mixmdate` downmix levels | 🟢 | High | Essential | Tables D2.2–D2.6 |
| | Programme-mix wire format (`pgmscl`, `mixdef`, pan, `blkmixcfg`, …) | 🟢 | Medium | Important | Written and decoded (§E2.3.1.12–61); the `blkmixcfginfo` desync at `numblkscod==0x0` is fixed and round-trip gaps are closed |
| | Receiver-side programme mixer | 🔴 | Medium | Important | No runtime mix of main + AD / commentary / external programme |
| | `infomdat` service / production | 🟢 | Medium | Important | Table E1.2 |
| **Decoder** | Full E-AC-3 reconstruction | 🟢 | High | Essential | Every Annex E tool, alone or stacked |
| | Dependent render (§E3.8.2) | 🟢 | High | Essential | Including 7.1.4 |
| | Legacy AC-3 core + E-AC-3 extension (§E2.3.1.2) | 🟢 | Medium | Important | `StreamKind::kAc3CoreEac3Extension` |
| | Convertible substreams (`strmtyp` 2) | 🔴 | Low | Out-of-scope | Spec’s no-re-encode AC-3 path; encode / decode / probe / edit / split all refuse |
| | Multi-programme select | 🟢 | Medium | Important | One programme per decode (`DecoderConfig::programme`) |
| | Dependent failure → bed-only | 🟢 | Medium | Important | Concealment / soft fail path |

---

## Atmos / JOC / OAMD / EMDF (TS 103 420)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **EMDF** | Container parse / write (Annex H) | 🟢 | High | Essential | Skip-field placement verified vs DEE |
| | Reserved / unsupported EMDF variants (§H.2.2) | 🔴 | Low | Optional | e.g. `protection_length_primary` = 00, `emdf_version` ≠ 0 — refused rather than guessed |
| **OAMD** | Payload encode (project subset) | 🟢 | High | Essential | `AtmosEncoder` |
| | Payload parse (broader than encode) | 🟡 | Medium | Important | Most §5.5; commercial shapes like `sample_offset_code`, `b_object_not_active`, multi-block updates still refused by the encoder — PR #801 (open) widens it |
| | Channel-based immersive (OAMD bed, no dynamic objects) | 🟡 | Medium | Optional | Decode path exists; not a separate encode product surface |
| **JOC** | Matrix encode (5.X downmix) | 🟢 | High | Essential | 7.X configs decode-only |
| | Object reconstruction (QMF + MDCT, §6.6.6) | 🟢 | High | Essential | Default QMF domain; self-check > −20 dB |
| | `joc_clipgain` application (§6.3.3.2) | 🟡 | Medium | Important | Parsed onto `FrameParameters`; not applied in the reconstruction chain |
| | Phase-shift downmix undo (`phsflg`, Table 47 configs 2/4) | 🟡 | Low | Optional | Configs parse; reconstructed like unshifted siblings (no Hilbert undo) |
| | 7.X downmix needing dependent Lb/Rb | 🟡 | Medium | Important | Metadata/JOC parse; `object_audio` empty when bed lacks the dependent |
| **Atmos encode** | Bed + objects in E-AC-3 | 🟢 | High | Essential | `oba::AtmosEncoder` |
| | Object size / spread / zone constraints (wire) | 🟢 | Medium | Important | Transmitted in OAMD |
| | Extent / spread / zone / snap in renderer | 🟡 | Low | Optional | Spec leaves behaviour to the renderer; VBAP bed pan does not apply them |
| | Complexity index / addbsi marker (§8.3) | 🟢 | High | Essential | Probe, `dec3`, HLS `CHANNELS="…/JOC"` |
| | Scene timeline (`ObjectScene`) | 🟢 | Medium | Important | Shared by CLI / GUI / live |
| | Live OSC object positions | 🟢 | Low | Optional | Scheme-prefixed; OSC only today |
| **Signing** | EMDF protection HMAC | 🟡 | High | Essential | Mechanism ships; no project key — unsigned streams fall back to 5.1 in Dolby decoders |
| | Authenticity-tag detect / verify | 🟢 | High | Essential | Probe and `signing::verify_*` (single key) |
| | Unchecked object decode (default) | 🟢 | High | Essential | Reconstruct without MAC check — FOSS-style |
| | Multi-key verify (keyring) | 🔴 | Medium | Important | Roadmap Partial / Proposed — [Object signing](../concepts/object-signing.md#planned-decode-modes) |
| | Licensed soft-gate (bed on fail) | 🔴 | Medium | Important | AVR-like: mismatch → bed-only; decode continues |
| **Tools** | Strip object layer (bitstream) | 🟢 | Medium | Important | Bit-identical bed; no re-encode |
| | Object unlock in Dolby decoder | 🟡 | High | Essential | Spec-correct streams; proprietary key gate — see [Object signing](signing.md) |

---

## AC-4 (ETSI TS 103 190-1 / -2)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Inspector (`ac4::ac4`)** | Sync frame + Annex G CRC | 🟢 | High | Essential | vs DEE fixtures |
| | Sync frames from a stream in pieces | 🟢 | High | Essential | `SyncFrameSplitter`: the caller's storage, no allocation, a partial frame held between reads; resynchronises past bytes that are not a frame and hands a frame found that way over only once a sync word follows it; the frames `scan` finds on committed streams fed in pieces from 1 byte to 64 KiB, and against `scan` in `fuzz_ac4_parse` |
| | TOC / presentations v0–v2 | 🟢 | High | Essential | Channel-based immersive through 7.1.4 |
| | `bitstream_version` 0/1 legacy TOC | 🟡 | Low | Optional | Transcribed; no real DEE stream exercises it (all observed v2); version 0 presentations of several substreams decode and mix on the test multiplexer's streams |
| | Channel-coded substream groups | 🟢 | High | Essential | Probe / JSON contract |
| | A-JOC / object / OAMD substream info | 🟡 | Medium | Important | Parsed; real A-JOC fixtures scarce |
| | `oamd_common_data()` (TOC level, §6.2.8.1) | 🟡 | Medium | Optional | Transcribed |
| | OAMD substream DATA body (`oamd_substream()`) | 🔴 | Medium | Important | Byte-range only — not field-parsed |
| | EMDF-only presentations (config 6) | 🟢 | Low | Optional | Synthetic + dual transcription |
| | `dac4` / RFC 6381 codec string | 🟢 | Medium | Important | MP4 / TS / HLS / DASH wiring |
| | Audio PCM decode | 🔴 | High | Essential | Inspector by design — content is byte ranges only; `ac4::decoder` decodes it |
| **Decoder (`ac4::decoder`)** | Presentation + channel-coded syntax | 🟡 | High | Essential | Full syntax trace |
| | ASF / ASPX / A-CPL / metadata() | 🟡 | High | Essential | DEE digest CI; dual transcription vs Python |
| | Channel-coded paths without fixtures | 🟡 | Medium | Optional | Noise fill, VARVAR ASPX, time-interleaved ASPX, the mono element, alt presentations, transmitted DRC gains — transcribed, oracle-poor; 3.0, 7.X and every A-CPL mode read on constructed streams |
| | EMDF payload substreams (syntax) | 🟡 | Medium | Important | Syntax only |
| | Dialogue enhancement PCM apply | 🟢 | Medium | Important | Part 1 5.7.8's four methods in the QMF domain, from 0 dB to the stream's cap; 0 dB is the tool bypassed, sample for sample, and the parsed gains apply to 0.01 dB; the channel-independent method on DEE's streams, the others on constructed data; the hybrid methods take their waveform from the presentation's dialogue enhancement substream; `ac3cli decode dialogue-enhancement=` |
| | Speech spectral frontend (SSF) | 🔴 | Low | Nice-to-have | Refused `kUnsupported` |
| | Immersive channel element (7.0.4, 7.1.4) | 🟢 | High | Essential | Part 2 6.2.4 to 6.2.6 in both transcriptions; SCPL, ASPX_SCPL, ASPX_ACPL_1, ASPX_ACPL_2 and ASPX_AJCC: 5.2's track assignment, S-CPL, A-SPX's pairing and gains, A-CPL's four modules and A-JCC, in full and core decoding (`ac4::DecoderConfig::decoding`); DEE's 5.1.4 in its three modes with each tone on its own channel, the rest on constructed streams; `ac3cli decode decoding=` |
| | 9.X.4 and 22.2 channel elements | 🔴 | Low | Nice-to-have | Refused `kUnsupported` by name |
| | Object audio: A-JOC and direct-coded objects | 🟢 | Medium | Important | Part 2 6.2.3 to 6.2.8 in both transcriptions, the OAMD substream included; A-JOC's reconstruction (5.7) in full decoding and its downmix or static bed in core decoding, dialogue enhancement for objects (5.8.2.3 to 5.8.2.5), each object's Annex F properties at its update sample, and the ISF renderer (5.10.3); Chromium's `ac4-ajoc.ac4` in both modes, eight constructed streams scored tone by tone. No reference decode to compare with: librempeg refuses object coding, and DEE writes no A-JOC from this project's masters |
| | HSF / 96–192 kHz | 🟡 | Low | Nice-to-have | Extension substream content read when it resolves to its owning channel substream; an unresolved link is refused `kUnsupported` — synthetic frames only, no real HSF stream available |
| | PCM: SIMPLE mono and stereo | 🟢 | High | Essential | ASF, stereo processing, block switching, frame alignment and the QMF banks at `frame_rate_index` 13; DEE's 2.0 streams at unity gain and pinned SNR floors in CI; librempeg agrees to 83 dB or better |
| | PCM: ASPX mono and stereo | 🟢 | High | Essential | Companding and A-SPX in the QMF domain; DEE's 2.0 streams from 48 to 144 kbps at unity gain and its immersive stereo against Lo/Ro, with SNR below the crossover, A-SPX tile energies, LSD and ViSQOL pinned; interleaved waveform coding, balance and VARVAR tested on constructed data |
| | PCM: 3.0, 5.X and 7.X | 🟢 | High | Essential | SIMPLE and ASPX: the LFE, Tables 178 to 183's matrices and routing, A-SPX pairing and companding over Tables 212 and 213; DEE's 5.1 streams from 192 to 768 kbps with every channel scored and pinned, each tone on its own channel, librempeg agreeing to 83 dB; every coding_config, 2ch_mode, chel_matsel, the 3.0 element and the three 7.X modes on constructed streams |
| | PCM: A-CPL | 🟢 | High | Essential | ASPX_ACPL_1 to 3 in the pair, 5.X and 7.X elements: the decorrelators against their difference equations and flat to 1e-9, the ducker, interpolation and dequantisation; DEE's 5.1 at 96 to 144 kbps with the coded downmixes scored as waveforms and each parameter band's level difference and correlation pinned; the other modes on constructed streams. librempeg does not rebuild the surrounds |
| | PCM: other frame rates | 🟢 | High | Essential | Every `frame_rate_index` to 48 kHz through the sample rate converter (`ac4core`: Kaiser-windowed polyphase, 100 dB down from the lower Nyquist frequency), its phase locked to `sequence_counter` as Part 2 5.11 has it and Table 47's counts held; DEE's IMS streams at 23.976, 24, 25 and 29.97 fps scored in CI |
| | Output level and DRC | 🟢 | High | Essential | `Lout`, Table 161's mode selection, the default profiles, transmitted curves and gains, in dB2, with a BS.1770 K-weighted level detector; the output level gain to 0.01 dB for DEE's dialnorms from -31 to -17, each mode's static curve within 0.5 dB; `ac3cli decode output-level= drcmode=` |
| | Downmix | 🟢 | High | Essential | Part 1 6.2.17's cascade from 7.X to 5.X, two channels and mono with the stream's gains and loudness corrections: Lo/Ro, Lt/Rt and Pro Logic II, the LFE at its mix gain; one tone per channel through each matrix equals its formula to 0.01 dB; `ac3cli decode channels= downmix=` |
| | Channel renderer (immersive) | 🟢 | High | Essential | Part 2 5.10.2's Tables 38 to 43 and 45 and 46 from the source's configuration to 7.X.4, 7.X.2, 7.X.0, 5.X.4, 5.X.2 and 5.X.0, with custom downmix data (Table 130's defaults) and the output's loudness correction, and Part 1's Table 218 after 5.X.0 for two channels and mono; every table held against a second transcription; DEE's 5.1.4 legs rendered in both modes equal the matrices to 0.01 dB; DRC's Table 69 groups; `ac3cli decode speakers=` |
| | Presentations and mixing | 🟢 | High | Essential | Part 2 4.8.2's selection, by `presentation_id`, position, language, associated audio and `b_pre_virtualized`, within the decoder's level, for version 0 and 1 presentations: a table of 30 constructed tables of contents in both transcriptions. Music and effects with dialogue, main with associated audio, both, and `presentation_config` 5 mixed as Part 1 6.2.16 and Part 2 4.8.3.17 to 4.8.5 give: group gains, the main audio's scaling, g_dialog and g_assoc, pans, version 0's levelling; every mix of the test multiplexer's streams equals its formula to 0.01 dB, in CI; `ac3cli decode presentation= language= associated= dialogue-gain= associated-gain=` |
| | Start-up, splices and concealment | 🟢 | High | Essential | Decoding from any I-frame gives the whole stream's output from the frame after it, but for A-SPX's noise phase and A-CPL's decorrelators settling; a change of source keeps the signal, so a splice at an I-frame joins the streams without a gap; `ConcealmentPolicy` repeat-and-fade or mute for a frame that does not decode, `ac3cli decode conceal=` |
| | API, reports and packaging | 🟢 | High | Essential | `DecoderConfig`, `OutputConfig` and `Decoder` with every control of the plan's "One control for both formats", changed while a stream plays by `set_output()` and `set_presentation()`; output a frame or 256 samples at a time; each presentation (names sent in chunks included) and the selected one's loudness, DRC, dialogue enhancement and downmix metadata; installed and exported with the inspector and the core, and a program decodes a stream through the installed package by CMake and by pkg-config; a test standing in for the Hearth engine decodes every committed stream through the public API alone, and Hearth's engine plays each one the decoder decodes through it, sample for sample the same (I2) |
| | `ac3cli decode` and `probe` | 🟢 | High | Essential | `decode`: every control above, a presentation with objects rendered to speakers through Hearth's layout renderer, and an option of another format's named in a warning; `probe json=1`: the frame rate, bit rate, I-frames, splices, version 1 presentations, the selected presentation and its metadata |
| | Transforms (`ac4core`: FFT, MDCT pair, KBD, QMF banks) | 🟢 | High | Essential | Each against its formula to 1e-12; shared by the decoder and the encoder, with A-SPX's tables and high frequency generator and A-CPL's decorrelators, ducker and tables |
| **Encoder (`ac4::encoder`)** | SIMPLE mono and stereo | 🟢 | High | Essential | 48 and 44.1 kHz, from 8 kbps; block switching, M/S and prediction; SNR, LSD and ViSQOL floors in CI; ahead of DEE on SNR and LSD at 192 kbps |
| | Frame writer, sync frame, MP4 and `dac4` | 🟢 | High | Essential | Encoder, decoder and Python traces agree record for record (tests, `fuzz_ac4_encode`, encoder-space harness); FFmpeg frames it; MediaInfo and DEE's MP4 muxer read it as configured. The `dac4` describes every presentation (Part 2 Annex E.10: each configuration's substream groups, A-JOC and direct-coded object groups, an alternative presentation's name and target), byte for byte as DEE's muxer writes it for Chromium's A-JOC stream and DASH-IF's vectors, or `dac4_refusal()` says what it cannot; `cmaf_refusal()` names the rule of Annex H.1.2.1 a stream breaks, and `ac3cli fmp4` refuses such a stream |
| | ASPX mono and stereo | 🟢 | High | Essential | Below 96 kbps a channel: A-SPX with DEE's crossovers, FIXFIX, FIXVAR and VARFIX framing, sinusoids, companding below 64 kbps a channel; SNR below the crossover, A-SPX tiles, LSD and ViSQOL pinned in CI; ViSQOL within 0.03 of DEE's or above it from 64 to 144 kbps. Balance, VARVAR and frequency interleaving behind `experimental=` |
| | SIMPLE and ASPX 5.0 and 5.1 | 🟢 | High | Essential | The 5.X element in DEE's form: L/R and Ls/Rs pairs, C, the LFE to 140.6 Hz; ASPX below 384 kbps for 5.1, at DEE's 5.1 crossovers; each channel's tone on its own channel, the LFE's included; librempeg decodes it as the decoder does, to 82.5 dB; SNR, LSD and ViSQOL pinned in CI, and the race against DEE from 192 to 768 kbps. Coding configurations 1 to 3, `2ch_mode` 1, and 7.0 and 7.1 in the 7.X element behind `experimental=` |
| | A-CPL 5.0 and 5.1 | 🟢 | High | Essential | ASPX_ACPL_3 and ASPX_ACPL_2 at DEE's rates, with DEE's A-SPX configuration; each band's parameters from each subband's own band; each tone on its own channel; the coded downmixes, each band's level difference and correlation, LSD and ViSQOL pinned in CI, and the race against DEE at 96 to 144 kbps; MediaInfo and DEE's muxer read it, and librempeg decodes its coded channels. ASPX_ACPL_1 and A-CPL in stereo behind `experimental=acpl` |
| | Frame rates, rate modes and I-frames | 🟢 | High | Essential | Every frame rate of Part 1 Table 83 at 48 kHz, through the decoder's converter in the other direction, each frame's samples locked to `sequence_counter` and exact over 100 000 frames at each rate; LSD and ViSQOL within pinned allowances of index 13 in CI; average and variable rates, the average one within the buffer `wait_frames` signals, checked frame by frame; I-frames at an interval, at named frames and at fragment starts, an MP4's sync samples |
| | Metadata | 🟢 | High | Essential | Further loudness values, measured with the BS.1770 meter; DRC's decoder modes on the default profile, on curves of their own or repeating another; the stereo downmix's values; dialogue enhancement from marked channels or a stem, by the channel-independent, Mid and cross-channel methods. MediaInfo reads each value as written over 42 configurations, and the decoder's output level, downmixes and dialogue enhancement gains equal their formulas on the encoder's streams, in CI. Transmitted DRC gains behind `experimental=drc-gains-N` |
| | Presentations and several substreams | 🟢 | High | Essential | Part 2 Table 53's configurations over substreams in groups of their own: music and effects with dialogue, main with dialogue enhancement (the hybrid methods' waveform in a substream of its own), main with associated audio, music and effects with both, main with both, roles by classifier, and EMDF payloads alone; alternative presentations and their names, languages, classifiers, group gains, each presentation's least `md_compat`, the dialogue's and the associated audio's mixing values, EMDF payloads passed through; CMAF's limits held. D7's selection and mixing give every configured presentation to 0.01 dB on the encoder's streams, in CI; MediaInfo lists the presentations, names, languages and levels as configured. 3.0 dialogue behind the experimental options. `ac3cli ac4-encode` takes them (`substreamN=`, `presentationN=`), and the encoder-space harness draws them |
| | API, CLI and packaging | 🟢 | High | Essential | `EncoderConfig`, every field with a default for designated initializers, and `Encoder`, whose `refusal_reason()` names the rule a configuration breaks; `sync_frame()` for raw files and MPEG-2 TS; `ac3cli ac4-encode` with an option for each setting, every option tested; installed and exported with the decoder (`ac4::encoder_static`, `ac4::encoder_shared`, pkg-config `ac4enc`), and a program encodes through the installed package by CMake and by pkg-config |
| | Immersive layouts and objects | 🔴 | High | Essential | Plan phases E8 and E9 |

---

## TrueHD / MLP

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Codec** | Experimental TrueHD/MLP module | 🟡 | Medium | Important | Substantial work on `feature/truehd-atmos-support`; not on `main` (roadmap IM5) |
| | Evolution frame HMAC (authenticity) | 🔴 | Medium | Important | Distinct from DD+ EMDF `ac3::signing`; truncated HMAC-SHA-256 on Evolution frames (cf. truehdd `--evo-key`). Note on IM5 / [Object signing](../concepts/object-signing.md#sibling-truehd-evolution) |
| | Shipping TrueHD interop | 🔴 | Low | Out-of-scope | Blocked on DVD Forum reference material and clean-room ruling (IM6) |
| | Passthrough device lists | 🟡 | Low | Nice-to-have | ELD / capability enums mention TrueHD; no codec on `main` |

---

## IAB (SMPTE ST 2098-2)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Reader** | Elementary `.iab` parse | 🟢 | High | Essential | vs DTS `iab-validator` |
| | Bed / object definition tree | 🟢 | High | Essential | Positions, gains, spreads resolved on read |
| | `AudioDataPCM` | 🟢 | High | Essential | Full PCM |
| | `AudioDataDLC` (Annex B) | 🟡 | Medium | Important | Identity only — opaque bytes; lossless coder not decoded |
| | MXF Track File extract (ST 2067-201) | 🟢 | High | Essential | Minimal KLV walk |
| **Bridge** | IAB → Atmos encode (positions / gains) | 🟢 | High | Essential | `admbridge::build_iab`; `ac3cli atmos-iab` |
| | Spread + `ObjectZoneControl` → JOC | 🔴 | Medium | Important | Explicitly not bridged today |
| **Writer** | IAB encode | 🔴 | Low | Nice-to-have | Read / ingest only |

---

## ADM / BW64 (ITU-R BS.2076 / BS.2088)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Reader** | BW64/RF64 container | 🟢 | High | Essential | Opt-in (`AC3FORGE_BUILD_ADM`); Boost |
| | ADM XML — DirectSpeakers + Objects | 🟡 | High | Essential | Phase-1 scope |
| | ADM Matrix / HOA / Binaural / `zoneExclusion` / `objectDivergence` / `screenRef` | 🔴 | Low | Nice-to-have | Refused `kUnsupportedType` |
| | Common definitions (Annex A) | 🟢 | Medium | Important | Predefined formats merged |
| **Writer** | BW64 write | 🟡 | Medium | Important | 24-bit PCM; shapes matching the bridge |
| | Decode → ADM BWF (Atmos master profile) | 🟡 | Medium | Important | Dynamic-object-only programmes; cartesian |
| **Bridge** | ADM → Atmos encode | 🟡 | High | Essential | Drops width / HOA / zoneLock and similar |

---

## IAMF (AOM Immersive Audio Model and Formats)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Writer** | Channel-based 7.1.4 LPCM (v1.1.0) | 🟢 | Medium | Important | Phase 1; ISO-BMFF encapsulation |
| | IA Sequence / Codec Config / Audio Element / Mix Presentation | 🟢 | Medium | Important | Simple Profile |
| | E-AC-3 decode → IAMF round trip | 🟢 | Medium | Important | `examples/mux_iamf.cpp` |
| | Parameter Block / Temporal Delimiter / trimming OBUs | 🔴 | Low | Nice-to-have | Not required for static 7.1.4; omitted in phase 1 |
| | Object-based audio elements | 🔴 | Low | Nice-to-have | Waits on IAMF v2.0 final |
| | OBU / file reader | 🔴 | Low | Nice-to-have | Phase 3 |
| | Raw OBU stream (§5) | 🔴 | Low | Nice-to-have | §6 ISO-BMFF only today |
| | Fragmented / live writer | 🔴 | Low | Nice-to-have | Batch `iamf::mux()` only |

---

## Stream carriage and containers

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Boxes** | `dac3` / `dec3` (incl. Atmos extension, Annex F) | 🟢 | High | Essential | Built from the bitstream, not the container claim |
| | Legacy core+E-AC-3 extension sample entry | 🔴 | Medium | Important | Decode/scan via `kAc3CoreEac3Extension`; mux refuses rather than emit a contradictory box |
| | `dac4` + MPEG-TS DVB registration | 🟢 | Medium | Important | AC-4 carriage |
| | MPEG-TS AC-4 ATSC profile (A/342-2) | 🔴 | Low | Optional | Explicitly refused; DVB path only |
| **Mux** | MP4 / ISOBMFF | 🟢 | High | Essential | Mux + demux; AC-3 / E-AC-3 / AC-4 |
| | MP4 `moov`-after-`mdat` streaming demux | 🔴 | Low | Optional | Refused with explanation |
| | Fragmented MP4 / CMAF | 🟢 | High | Essential | Init + media segments |
| | Matroska | 🟢 | Medium | Important | Mux + demux |
| | MPEG-TS (DVB + ATSC for AC-3/E-AC-3) | 🟢 | High | Essential | Mux + demux; descriptors from `scan` |
| | Multi-programme container mux | 🟡 | Medium | Optional | CLI muxers warn and carry the first programme only |
| **Streaming** | HLS playlists | 🟡 | Medium | Important | Atmos `CHANNELS="N/JOC"` + 5.1 fallback; manifest semantics not player-validated |
| | DASH MPD + Dolby supplemental descriptors | 🟡 | Medium | Important | Syntactically correct; no schema / player validation |
| **Transport** | IEC 61937 burst pack (AC-3 + E-AC-3) | 🟢 | High | Essential | vs FFmpeg / MS docs |
| | IEC 61937 burst unpack (`unspdif`) | 🟢 | Medium | Optional | Inverse of pack |
| | IEC 61937-14 AC-4 burst pack + unpack | 🟢 | Medium | Important | The four burst types, their periods and sequences at every frame rate from the standard's tables, checked against a second transcription; no device here accepts AC-4 |
| **Edit** | In-place metadata rewrite | 🟡 | Medium | Optional | Existing fields only; no insert |
| | Loudness QC vs delivery specs | 🟢 | Medium | Important | BS.1770-4 vs dialnorm / R 128 / A/85 / Netflix |
| | Elementary scan / probe / split | 🟢 | High | Essential | Programme-aware access-unit walk |

---

## Cross-cutting library surface

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **API** | C++23 `ac3::forge` | 🟢 | High | Essential | Encode / decode / inspect / measure |
| | Minimum-footprint decoder (`ac3::forge_minimal`) | 🟢 | Medium | Important | Bare-metal / ESP32 profile |
| | C API (`ac3::forge_c`) | 🟢 | Medium | Important | Stable minimal surface |
| | Python / Rust / WASM bindings | 🟢 | Medium | Important | WASM decode package not yet on npm |
| **Verify** | Encoder/decoder mirror traces | 🟢 | High | Essential | AC-3 and E-AC-3 |
| | Research trace export (CSV / JSONL) | 🟢 | Low | Optional | `ac3::verify` |
| | Conformance / fuzz / quality gates | 🟢 | High | Essential | See [Validation](../verification.md) |
| | Cross-toolchain encoder bit-identical output | 🟡 | Medium | Important | Audit + `ilogb` fix done; FP thresholds / cross-leg gate still open (VX12) |
| | Listening-test apparatus (MUSHRA/ABX) | 🟡 | Low | Optional | Tools under `tools/listening/`; no human session run (VX9) |
| | Perceptual encoder criterion calibration | 🔴 | Medium | Optional | Proposed as EQ14; not wired |
| **Audio I/O** | Capture / monitor / passthrough | 🟡 | Medium | Important | `ac3::audio` in-tree only; every output backend now stops itself on device loss (Windows passthrough-unplug hardware-confirmed), but platform verification stays uneven |
| | Sink capability discovery (EDID / ELD) | 🟡 | Medium | Important | Used for passthrough negotiation; uneven across platforms |
| | AC-4 passthrough | 🟡 | Low | Optional | ALSA and Android; WASAPI, PipeWire and CoreAudio name no AC-4 format and refuse it; AC-4 HBR16's eight-channel link refused everywhere; no receiver to test |
| **Out of scope** | Headphone / binaural renderer | 🔴 | Low | Out-of-scope | Deliberate product boundary (external renderer) |

---

## Standards cross-check

Walk of each standard (or annex) this project cites against the rows above. Only **open**
items (🟡 or 🔴) are listed — green surfaces are assumed covered by the feature tables.
When a capabilities or verification page already names the bound, that page stays authoritative;
this register is the checklist that those bounds appear here too.

### ATSC A/52 / ETSI TS 102 366

| Clause / annex | Open item | Status |
|---|---|---|
| Table 5.8 acmods 3/0, 2/1, 3/1, 2/2 | Named-layout / CLI encode coverage | 🟡 |
| Annex E §E2.3.1.1 `strmtyp` 2 | Convertible substreams | 🔴 |
| Annex E §E2.3.1.2 + Annex F | Legacy core+extension `dac3`/`dec3` mux | 🔴 |
| Annex E §E2.3.1.2 I0–I7 | Associated-service labelling; receiver mixer | 🟡 / 🔴 |
| Annex E §E3.5 / §3.7 | In `auto`; external oracle; TPN EOF hold-back | 🟢 tools / 🟡 policy |
| Annex E `fscod2` | External PCM oracle | 🟢 code / validation gap |
| Annex H §H.2.2 | Reserved EMDF variants | 🔴 |

### ETSI TS 103 420 (Atmos / JOC / OAMD)

| Clause | Open item | Status |
|---|---|---|
| §5.5 / §5.6 | Commercial OAMD field shapes | 🟡 |
| §5.x renderer behaviour | Extent / spread / zone / snap apply | 🟡 |
| §6.3.3.2 | `joc_clipgain` apply | 🟡 |
| Table 47 | `phsflg` Hilbert undo; 7.X+dependent Lb/Rb | 🟡 |
| Protection / authenticity | Project key + Dolby unlock | 🟡 |

### ETSI TS 103 190-1 / -2 (AC-4)

| Clause | Open item | Status |
|---|---|---|
| Part 1 / 2 TOC legacy | `bitstream_version` 0/1 | 🟡 |
| §6.2.2.4 | OAMD substream DATA body | 🟢 |
| Channel-coded syntax without fixtures | Noise fill, VARVAR, … | 🟡 |
| SSF | Decode | 🔴 |
| §4.2.4.3 | HSF extension substream content (syntax only; synthetic frames only) | 🟡 |
| Whole codec | PCM reconstruction of the 9.X.4 and 22.2 elements | 🔴 |
| Whole codec | Encoding beyond SIMPLE, ASPX and A-CPL mono to 5.1 | 🔴 |
| §5.1.4 | Spectral noise fill: decoded, but no stream here sets it | 🟡 |

### SMPTE ST 2098-2 / ST 2067-201 (IAB)

| Clause | Open item | Status |
|---|---|---|
| Annex B | `AudioDataDLC` decode | 🟡 |
| §5.5 / §10 zone control | Spread + `ObjectZoneControl` → JOC bridge | 🔴 |
| Writer | IAB encode | 🔴 |

### ITU-R BS.2076 / BS.2088 (ADM / BW64)

| Clause | Open item | Status |
|---|---|---|
| Pack types beyond DirectSpeakers + Objects | Matrix / HOA / Binaural / zoneExclusion / … | 🔴 |
| Writer / bridge | Narrowed Atmos-master subset | 🟡 |

### AOM IAMF v1.1.0

| Clause | Open item | Status |
|---|---|---|
| §3 Parameter / trim / delimiter OBUs | Phase-1 omission | 🔴 |
| Object elements | Waits on v2.0 | 🔴 |
| §5 raw OBU; reader; fragmented writer | Not started | 🔴 |

### Carriage (Annex F, IEC 61937, MPEG-TS, HLS, DASH)

| Spec | Open item | Status |
|---|---|---|
| TS 102 366 Annex F | Legacy core+extension sample entry | 🔴 |
| ATSC A/342-2 | AC-4 ATSC TS profile | 🔴 |
| IEC 61937-14 | AC-4 to a device: no receiver accepts it, and HBR16's eight-channel link is not opened | 🟡 |
| ISO BMFF | `moov`-after-`mdat` demux | 🔴 |
| Apple HLS / Dolby DASH | Player / schema validation of Atmos signalling | 🟡 |
| Multi-programme mux | First programme only | 🟡 |

### TrueHD / MLP

| Source | Open item | Status |
|---|---|---|
| Branch `feature/truehd-atmos-support` | Land as experimental module (IM5) | 🟡 |
| Evolution frame HMAC | Authenticity policy on MLP (parallel to EMDF; not `ac3::signing`) | 🔴 |
| DVD Forum MLP reference | Shipping interop (IM6) | 🔴 |

---

## How to read a yellow or red row

- **🟡 Partial** means the feature exists with a documented bound (parser narrower than commercial streams, syntax without PCM, branch not merged, intentional `auto` exclusion called out elsewhere). The bound is the note; the prose is in [Capabilities](capabilities.md).
- **🔴 Not started / refused / out of scope** means no implementation on `main` for that specification surface, or an explicit refuse. Roadmap IDs in the notes point at [Roadmap](../roadmap.md) where one exists.
- Oracle and hardware gaps are **not** red features by themselves. A completed encoder whose only external decoder is this project's own is still 🟢, with the gap recorded under [Validation](../verification.md) and noted in the standards register where useful.
