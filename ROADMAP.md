# Roadmap

Candidate and in-flight work only. This is not a commitment.

For what already ships, see [CHANGELOG.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) and each product's index page. For the
library's capability record, see
[`docs/library/capabilities.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/capabilities.md).
Detailed design lives in [`planning/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/planning)
and product `design/` records — linked below, not copied here.

Last reviewed: 2026-09-21. Reconciliation source:
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
| Desktop player engine | Queue, transport, gapless, passthrough, meters, settings, diagnostics (`apps/hearth/engine/`; `[hearth]` tests) | Qt application window (planning A5); packaging and user guide (A7–A8) | [Hearth index](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/index.md) |
| Sendspin server in the app | Protocol library, testserver, testsink, group/pairing tests, aiosendspin CI exit | `ServerHost` wired into the desktop engine for network output | [Sendspin extension plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/hearth-sendspin-extension.md) |
| ESP32-S3 sink | `hearth_sink` firmware, Improv, groups, QEMU CI | TDM DAC hardware exits (ES9080 pair); Music Assistant on a real MA instance | [ESP32-S3 sink guide](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/hearth/sink-esp32-s3.md) |

**Sink module tiers (shared PCB → pair of ES9080s):** **good** C6 (5.1, one DAC) · **better**
S3 (7.1.4 without enhanced coupling, both DACs @ 16-bit) · **best** P4 (9.1.6 + full tools
desired, both DACs @ 32-bit on one I2S). Study:
[`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md).

**Proposed next (blocked on UI design sign-off):** planning A0 UI design round, then A5 window,
then A6 network UI.

**Not started:** ESP32-C6 Sendspin sink (planning C3, after the S3 sink pattern); ESP32-P4
wide sink (tier study P1+, after S3 DAC / C6 pattern as needed).

### Library — AC-4 audio decode (Partial)

| Layer | State |
|---|---|
| Container parse and inspect (`src/ac4`, CLI `probe`) | **Shipped** — see CHANGELOG and [Validation — AC-4](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/verification.md) |
| Syntax transcription (`src/ac4dec`) | **In progress** — reads channel-coded substream syntax; [`decoder.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac4dec/include/ac4dec/decoder.hpp) states it produces no audio yet |
| PCM decode, objects, speech frontend, immersive paths | **Not started** |

Hearth and Forge playback of AC-4 waits on PCM decode. Chip D in the Hearth plan; no separate
`planning/ac4-decoder.md` yet.

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
separate item (was UX10).

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
| **Multi-programme E-AC-3 encode** | `programme2=` authoring via CLI | Programme-mix metadata (`mixmdate`, `bsmod`, associated-service mixing) — see [capabilities](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/library/capabilities.md) |
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
- **APT/DNF repositories and Docker images** — not planned; see [`docs/releasing.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/releasing.md).
- **ESP32-P4 as a replacement for the S3 Wi-Fi Sendspin sink** — closed 2026-09-08 (no on-die radio; no float PIE win; S3 probe already real-time). **Complementary P4 “best” module** (Ethernet / hosted C6, dual ES9080 @ 32-bit) is Proposed — see [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md) and [`esp32-c3.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/platforms/bare-metal/esp32-c3.md#why-not-the-esp32-p4).
- **Per-channel and per-block SNR offsets** — tried and declined (EQ2); reference encoders agree with shipped behaviour.

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
