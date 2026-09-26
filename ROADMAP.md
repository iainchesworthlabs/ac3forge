# Roadmap

Candidate and in-flight work only. This is not a commitment.

For what already ships, see [CHANGELOG.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) and each product's index page. For the
library's capability record, see
[`docs/library/capabilities.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/capabilities.md).
Detailed design lives in [`planning/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/planning)
and product `design/` records — linked below, not copied here.

Last reviewed: 2026-09-26. Reconciliation source:
[`planning/roadmap-inventory.md`](https://github.com/iainchesworthlabs/ac3forge/tree/main/planning/roadmap-inventory.md).

## How to read this

| Status | Meaning |
|---|---|
| **In progress** | Active development on `main`, or on a named branch named below |
| **Partial** | Some of the scope is shipped; the row splits what is done from what is not |
| **Proposed** | Strong candidate or agreed direction; not started, or study only |
| **Blocked** | Wanted but waiting on an external dependency |
| **Out of scope** | Deliberately not doing (with reason) |

Sizes, where they still matter, are rough: **S** (an afternoon), **M** (a day or two), **L** (a
focused week), **XL** (several PRs).

---

## In progress

### Hearth

Hearth has no legacy roadmap IDs. Status is reconciled from
[`planning/hearth-reference-player.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/hearth-reference-player.md)
and [`docs/hearth/index.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/index.md).

| Feature | Done | Not done | Detail |
|---|---|---|---|
| Desktop player | Queue, transport, gapless, passthrough, meters, settings, diagnostics (`apps/hearth/engine/`; `[hearth]` tests); the Qt window (Play, Media, Speakers, Decoder, Network and Settings pages); channel-based AC-4 playback; packages for Windows, macOS and Linux | A user guide for the app; screenshots of the running app (the Hearth images in the repository are design mockups); immersive and object AC-4 in the app | [Hearth index](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/index.md) |
| Network output | Sendspin discovery, pairing, groups and playing to a group from the app; updating a sink's firmware from the app; aiosendspin CI exit | Music Assistant tested against a real instance | [Sendspin extension plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/hearth-sendspin-extension.md) |
| ESP32 sinks | `hearth_sink` on the ESP32-S3, the ESP32-C6 (stereo) and the ESP32-P4 (revision 1.x): Improv, groups, updates over the network, QEMU CI; firmware images for each, published from the next release | TDM DAC hardware exits (ES9080 pair); the wide P4 sink; AC-4 on the ESP32 parts (see the AC-4 section) | [ESP32-S3 sink guide](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/sink-esp32-s3.md), [Sink firmware](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/sink-firmware.md) |

**Sink module tiers (shared PCB → pair of ES9080s):** **good** C6 (5.1, one DAC) · **better**
S3 (7.1.4 without enhanced coupling, both DACs @ 16-bit) · **best** P4 (9.1.6 + full tools
desired, both DACs @ 32-bit on one I2S). Study:
[`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md).

**Not started:** the wide ESP32-P4 sink (tier study P2 and later: a networked shape onto TDM and
the ES9080 pair).

**Done:** ESP32-P4 probe and board timing table, no network (tier study P1) — real time on every
fixture at this board's 360 MHz. [`docs/platforms/bare-metal/esp32-p4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/platforms/bare-metal/esp32-p4.md).

### Library — AC-4 decode and encode (Partial)

| Layer | State |
|---|---|
| Container parse and inspect (`src/ac4`, CLI `probe`) | **Shipped** — see CHANGELOG and [Validation — AC-4](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/verification.md); sync frames from a stream that arrives in pieces, and `probe` reports each presentation and the metadata as the decoder reads them (D8) |
| Syntax transcription (`src/ac4dec`) | **Shipped** — reads channel-coded substream syntax, cross-checked against a second transcription |
| PCM decode | **In progress** — mono, stereo, 3.0, 5.X and 7.X decode to PCM in every codec mode Part 1 gives them, SIMPLE, ASPX and A-CPL (phases D2 to D5), through the QMF banks, companding, A-SPX and A-CPL; every frame rate, with the output level, DRC, dialogue enhancement and the downmix (D6); streams of several presentations, the presentation chosen and its substreams mixed (D7); the API in its final form for channel-based streams, with the output and the presentation changed while a stream plays, output by block, and each presentation and the metadata reported, and the inspector, decoder and core installed and exported (D8, [AC-4 decoding](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/ac4.md)); 7.0.4 and 7.1.4 in every immersive codec mode, in full and core decoding, rendered by Part 2's channel renderer (D9) |
| Immersive paths, objects | **In progress** — the channel-based immersive element decodes (D9), and object audio, A-JOC and direct-coded objects with their metadata and the intermediate spatial format (D10); the 9.X.4 and 22.2 modes are refused; the speech frontend waits for a stream that uses it |
| IEC 61937 carriage (`ac3::iec61937`, `PassthroughSink`, Hearth's extension role) | **Built** (phase D11) — the four IEC 61937-14 burst types at every frame rate, read back unchanged; passthrough on ALSA and Android, the platforms whose APIs can send AC-4; the extension role's AC-4 data type, decoded by the test sink; no receiver found accepts AC-4 |
| Encoder (`src/ac4enc`) | **In progress** — mono, stereo, 5.0 and 5.1 in the SIMPLE, ASPX and A-CPL codec modes (phases E1 to E4), A-SPX below 96 kbps a channel in mono and stereo and below 384 kbps in 5.1, companding in mono and stereo below 64 kbps a channel, ASPX_ACPL_2 and ASPX_ACPL_3 in 5.1 below 168 and 112 kbps, `ac3cli ac4-encode`; 7.X, the 5.X element's other coding configurations, ASPX_ACPL_1 and A-CPL in stereo as experimental options; every frame rate, the rate modes, I-frames on demand and the metadata (E5); several substreams in the presentations of Part 2 Table 53 (E6); the API in its final form for channel-based streams, which names the rule a configuration it refuses breaks, `ac3cli ac4-encode` with an option for each setting, the MP4's `dac4` describing every presentation, and the encoder installed and exported beside the decoder (E7, [AC-4](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/ac4.md#encoding-a-stream)); 5.0.4 and 5.1.4 in the immersive element, with the height downmix (E8); objects follow in E9 |
| Applications (`ac3cli`, Hearth, Forge GUI, bindings) | **In progress** — `ac3cli` reads and writes AC-4 in `transcode`, `record`, `live`, `monitor`, `play`, `qc`, `levels`, `loudness`, `spdif`, `probe`, `mp4`, `ts` and `fmp4`; `mkv` refuses it, since Matroska registers no codec ID (I1); Hearth's desktop player plays channel-based AC-4 through the decoder's public API, decoded for every output and sent as bursts to network sinks that take it, with the Decoder page's AC-4 controls and the Media page's AC-4 information (I2); the Forge GUI, the C API and the language bindings, immersive and object content in the applications, and the ESP32 sinks are in phases I3 to I6 |
| ESP32 (`float` on the P4, then the S3; fixed point on the C6) | **Not started** — plan phase D14, after D10: the decoder on `double`, `float` and fixed point by target, as AC-3 and E-AC-3 are, the P4 first |

`ac3cli` and Hearth's desktop player play AC-4; the Forge GUI's use of it waits on its own phase. Plan:
[`planning/ac4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md). Its
first phase made the DEE reference streams the others need; the local DEE licence ends on
2026-11-06 and will not be renewed, so G1 adds every stream the remaining phases need before then.

### Library — API freeze → v1.0.0 (was AP1, L)

**Done:** tiering and SemVer policy in
[`docs/library/api-stability.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/api-stability.md);
C version macros; release criteria written.

**Not done (deferred to the v1.0.0 cut):** flip `SOVERSION` to the major component; introduce
`inline namespace v1`; make the ABI gate **required** (today advisory — see `_ci-core.yml`
`ABI_ENFORCE`).

### Library — TrueHD experimental module (was IM5, L)

Substantial internal codec on branch `feature/truehd-atmos-support` — not on `main`. To merge:
rebase, gate as `ac3::mlp` / `AC3FORGE_BUILD_MLP`, remove non-redistributable PDFs, label output
accurately (no real TrueHD decoder reads the current block layout). Forge front ends follow as a
separate item (was UX10). Dolby Encoding Engine's TrueHD streams of known sources (2, 6 and 8
channels, 48 and 96 kHz, 16 and 24 bits, several presentations), made by
`tools/generators/gen_dee_gold.py` while DEE's licence runs (it ends on 2026-11-06), are kept
locally for what the clean-room rule below allows.

**Authenticity note (for that branch):** TrueHD carries a separate keyed check from DD+ EMDF
object signing — **Evolution frame protection**, a truncated HMAC-SHA-256 over the access unit
and the Evolution frame. Open decoders (e.g. truehdd `--evo-key`) optionally verify it with an
operator-supplied key; without a key they decode unchecked. When authenticity policy lands for
MLP (multi-key verify / unchecked / licensed soft-gate), wire it to Evolution HMAC, not to
`ac3::signing`'s EMDF path. See [Object signing](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/concepts/object-signing.md#sibling-truehd-evolution).

### Crucible — cross-platform product (was UX12, Partial)

**Done:** rename to Crucible; Windows and Linux verified on real hardware (Pi PipeWire pass,
receiver display read); Linux packages; macOS platform half **compiles and runs headless suites
in CI**.

**Not done:** launch Crucible interactively on a Mac with audio; macOS package; native-speaker
translation review (CR1); attestation-signed Windows driver (see DR6).

Detail: [`docs/crucible/design/promotion.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/crucible/design/promotion.md).
Current product status:
[`docs/crucible/index.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/crucible/index.md).

### Shared audio — macOS process tap (was UX7, Partial)

Code is in `src/audio/src/backend/macos/process_tap.mm` and compiles on macOS CI legs. The path
is **refused by default** because `AudioDeviceCreateIOProcID` hung the HAL client in Crucible's
first real run; opt-in via `AC3FORGE_MACOS_PROCESS_TAP`. No Mac has captured audio through a tap
yet.

### Shared — hardware verification (was DR9, Partial)

| Backend | State |
|---|---|
| Linux / ALSA passthrough | **Confirmed** on Raspberry Pi → AVR |
| Linux / PipeWire | **Confirmed** on Pi; Crucible live path verified |
| Windows / WASAPI exclusive | **Confirmed** on Onkyo TX-RZ740 |
| macOS / CoreAudio tap and desktop apps | **Not verified** on real Mac hardware |
| Pi 5; second Android TV | **Outstanding** |

---

## Partial tails on shipped work

These shipped but have an open follow-on. They do not belong in "In progress" as whole features.

| Topic | Shipped | Follow-on |
|---|---|---|
| **IAMF** (was IM3) | Channel-based 7.1.4 `ipcm` writer (`src/iamf`, phase 1 of 3) | Object elements and OBU reader — **blocked on IAMF v2.0 final** |
| **IAB reader** (was IM1) | Full header and PCM essence parse | Annex B **AudioDataDLC** lossless decode — opaque bytes today |
| **Object authenticity modes** | `ac3::signing` HMAC tag; `sign-objects`; single-key `verify-objects` (hard fail); default decode reconstructs objects **unchecked** (FOSS-style) | **Multi-key** verify (keyring / repeated `signing-key=`); **licensed** soft-gate (e.g. `gate-objects`: tag mismatch / unsigned → bed-only, decode continues); CLI + docs naming the three modes — [Object signing](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/concepts/object-signing.md#planned-decode-modes). TrueHD Evolution HMAC is the parallel on IM5, not EMDF |
| **Multi-programme E-AC-3 encode** | `programme2=`..`programme8=` authoring via CLI (all eight §E2.3.1.2 substreams, full per-programme `mixmdate`/`bsmod` metadata) | Receiver-side use of that metadata — actually combining an associated service with the main programme during mixdown, rather than just carrying it — see [capabilities](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/capabilities.md) |
| **Encoder reproducibility** (was VX12) | Audit done; `ilogb` fix landed | Re-validate FP-gated bit-cost thresholds; fixed-point transient port optional |
| **Listening test** (was VX9) | Apparatus in `tools/listening/` | **No session run yet** — see [`tools/listening/responses/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/listening/responses/README.md) |
| **Crucible translations** (was CR1) | Six catalogues complete (385 messages each) | Native-speaker reading of mechanical translations |

---

## Proposed

| Item | Notes | Was |
|---|---|---|
| Generated API reference and versioned docs | Doxygen into mkdocs; `latest` / `dev` branches | AP8 |
| GStreamer element or FFmpeg external encoder for >5.1 / JOC encode | Out of tree, over the C API; AP5 (C API) is done | AP10 |
| Dolby Reference Player wider CI crosscheck | Extend beyond `none/cpl/spx/aht/all`; self-hosted Windows job | VX5 |
| Perceptual encoder criterion calibration | EQ13 follow-on; `kPerceptual` | EQ14 |
| Object authenticity: multi-key + licensed gate | Completes the Partial tail above — keyring verify and AVR-like bed-only soft-gate for EMDF; Evolution HMAC for TrueHD rides IM5 | — |
| QC delivery report file | `ac3cli qc` writes stdout today | [`planning/qc-report.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/qc-report.md) |
| DAW / NLE host plugin | Feasibility study only | [`planning/host-plugin.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/host-plugin.md) |
| ESP32 sink tiers (C6 / S3 / P4) on one ES9080 PCB | Modular MCU: C6 ≤5.1 / one DAC; S3 ≤7.1.4 no ecpl / both DACs @ 16-bit; P4 ≤9.1.6 full tools desired / both DACs @ 32-bit on one I2S. P4 reopened only as best tier | [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md) |
| vcpkg git registry | Consumers install via `vcpkg install ac3forge` | DR3 |
| winget and ConanCenter | Manifests staged; CLA and submission pending | DR4 |
| AC-4 in the performance and quality reporting | The speed, memory, quality and tool-comparison series cover AC-3, E-AC-3 and Atmos only. AC-4 needs stereo and 5.1 encode and decode workloads in `ac3bench`, `ac3perf` and `ac3membench`, its transforms in `ac3kernelbench`, and a quality series in the `quality-history` branch with the regression tiers the other series use. CI holds AC-4 to pinned floors today, and those record no history. **L** | [Performance and quality](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/performance-quality.md) |
| Hearth desktop app: user guide and screenshots | The app has an index page and sink guides, and no guide of its own; the Hearth images in the repository are design mockups. `ac3hearth --shot <png> --page <name>` captures each page, but it starts network discovery, which on Windows asks to register a firewall rule, so a capture run has to expect that prompt. **M** | [Hearth index](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/index.md) |
| Forge GUI screenshots of live capture | Three of the 17 screenshots (`format-vbr`, `live-session-idle`, `live-session-vbr-note`) predate the header's Inspect objects, Open stream and About buttons; taking them again needs an open capture device. **S** | [Live capture](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/forge/gui/live-session.md) |
| Program names | `ac3cli`, `ac3gui`, `ac3hearth` and `ac3crucible` become `forge`, `forge-gui`, `hearth` and `crucible`; the library, the packages and the C API keep `ac3forge`, and the old names keep working for a stated period. **L** | [`planning/ac4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md#n1-the-names) |
| AC-4 in the published descriptions | The GitHub repository description names AC-3 and E-AC-3 only, and the repository has no topics. The CLI's banner, man page and exit-code text (`apps/cli/usage.cpp`, quoted in `docs/forge/cli/commands.md`) name AC-3 and E-AC-3 only. **S** | — |

---

## Blocked

| Item | Blocker | Was |
|---|---|---|
| TrueHD interoperability with shipping decoders | Non-public MLP reference material; clean-room policy (IM6) | IM6 |
| macOS notarisation and Windows Authenticode | Certificates and accounts, not code (Known gap since 0.8.0-beta.2) | DR6 |
| IAMF object elements and reader | IAMF v2.0 not final (IM3 phases 2–3) | IM3 tail |

---

## Out of scope

- **Forging Dolby's authenticity tag** — see [`docs/concepts/object-signing.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/concepts/object-signing.md). The signer ships; the key is the operator's. Multi-key verify and licensed soft-gate (Partial / Proposed above) use **operator-provisioned** keys only — they do not recover or invent decoder secrets.
- **AC-3 VBR** — structurally impossible; frame size indexes a fixed table.
- **Renderer and room-correction territory** — covered by [Cavern](https://github.com/VoidXH/Cavern). A headphone/binaural preview for the WASM demo stays off unless that boundary is redrawn on purpose.
- **A DAMF reader** — no public specification; IM1 / ADM BWF is the replacement.
- **TrueHD interoperability by black-box analysis of Dolby streams** — not until the clean-room rule explicitly allows it.
- **An external oracle for `fscod2` audio** — FFmpeg and Dolby Reference Player refuse it.
- **Perfect separation of co-directional objects** — property of parametric object coding.
- **Enabling PipeWire `iec958Codecs` on the user's behalf** — session-manager policy; documented instead.
- **HOA, Matrix and Binaural ADM pack types** — refused with `kUnsupportedType` until a design exists.
- **AC-4 in Matroska** — Matroska registers no codec ID for AC-4, so `ac3cli mkv` refuses it; MP4, CMAF and MPEG-TS carry it.
- **APT/DNF repositories and Docker images** — not planned; see [`docs/releasing.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/releasing.md).
- **ESP32-P4 as a replacement for the S3 Wi-Fi Sendspin sink** — closed 2026-09-08 (no on-die radio; no float PIE win; S3 probe already real-time). **Complementary P4 “best” module** (Ethernet / hosted C6, dual ES9080 @ 32-bit) is Proposed — see [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md) and [`esp32-c3.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/platforms/bare-metal/esp32-c3.md#why-not-the-esp32-p4).
- **Per-channel and per-block SNR offsets** — tried and declined (EQ2); reference encoders agree with shipped behaviour.
- **Multi-service (multi-PID) MPEG-TS authoring** — one PMT with a main-service PID plus associated-service PIDs, built in one invocation. `mpegts::mux` stays a single-elementary-stream muxer (its own header comment calls a general multiplexer out of scope); `mainid`/`asvc` describe links an operator authors across separately-muxed files, not a multiplex this tool builds for them.

---

## Legacy ID index

Old IDs (`EQ1`, `UX12`, `A1`–`G4`, planning chips) are **retired for new work**. They remain here
so old PRs and comments still resolve. Do not allocate new IDs.

<details markdown="1">
<summary>2026-08-15 roadmap → v0.9.0 carry-forward (single-letter IDs)</summary>

| ID | Item | |
|---|---|---|
| A1–A5 | Container mux / CLI streaming | merged → IO* |
| B1 | ADM BWF → JOC | merged |
| B2 | DAMF reader | out of scope → IM1 |
| B3 | IAMF | merged phase 1 → IM3 |
| C1–C4 | Metering / QC / dialnorm | merged → IO*, DC* |
| D1 | TrueHD branch | in progress → IM5 |
| D2–D4 | Decoder / AC-4 inspect | merged → EQ4, IM4 |
| E1–E4 | Audio backends / CI | merged → DR*, audio |
| F1–F6 | C API / bindings / packages / API freeze | merged or in progress → AP*, DR*, UX* |
| G1–G4 | Verification / fuzz | merged → VX* |

Full ledger text preserved in git history of this file before 2026-09-17.

</details>

<details markdown="1">
<summary>Theme IDs (EQ, DC, IO, IM, VX, PF, AP, UX, CR, DR) — disposition summary</summary>

| Theme | Shipped (removed from this file) | Still active (see sections above) |
|---|---|---|
| EQ | EQ1–EQ13 encoder quality work | EQ14 proposed; EQ2 out of scope |
| DC | DC1–DC10 decoder and stream tools | Multi-programme mix metadata tail |
| IO | IO1–IO12 containers, QC, loudness | QC report file proposed |
| IM | IM1–IM4, IM7 | IM5 in progress; IM6 blocked; IM3/IAB tails; object authenticity modes Partial |
| VX | VX1–VX23 except VX9/VX12 tails | VX9/VX12 partial; VX5 proposed |
| PF | PF1–PF8 performance | — |
| AP | AP2–AP7, AP9, AP11–AP12 | AP1 in progress; AP8/AP10 proposed |
| UX | UX1–UX6, UX8–UX9, UX11 | UX12/UX7 partial; UX10 proposed |
| CR | — | CR1 partial |
| DR | DR1–DR2, DR5, DR7–DR8 | DR9 partial; DR3/DR4 proposed; DR6 blocked |

For per-item detail on shipped work, see [CHANGELOG.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md).

</details>

---

Rebuilt 2026-09-17 from [`planning/roadmap-inventory.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/roadmap-inventory.md)
and a repository survey. Previous version (~3,100 lines, theme sections with full shipped
records) is in git history at `a7d3bd56` and earlier.
