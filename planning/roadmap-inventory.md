# Roadmap inventory (codebase reconciliation)

Working document for the `feature/roadmap-simplify` rewrite. **Not published to the
documentation site.** Reconciles `ROADMAP.md`, `planning/*.md`, product index pages, and the
tree as of **2026-09-17** on `main` (`a7d3bd56`).

## Purpose

Before replacing `ROADMAP.md` with a slim status board, every non-shipped item is checked against
code. Each row records what is **done**, what is **not done**, whether existing docs are
**accurate**, and the **plain-English line** the new roadmap should carry.

## How to read the columns

| Column | Meaning |
|---|---|
| **Status** | Shipped · In progress · Partial · Proposed · Blocked · Out of scope |
| **Doc accuracy** | Accurate · Misleading · Missing · Stale |
| **New roadmap line** | Draft one-liner for the slim `ROADMAP.md` (no new IDs) |

**Verification rules used here:**

- **Shipped** — tests or CI exercise it; recorded in `CHANGELOG.md` and/or `capabilities.md`.
- **Partial** — boundary named in code (header comment, CMake gate, or test scope).
- **Missing** — merged or in-flight on `main` but absent from `ROADMAP.md` structured sections.
- **Stale** — `ROADMAP.md` contradicts product pages or the tree.

---

## Summary counts

| Category | Count |
|---|---|
| Active ROADMAP items (in progress / considering) | 19 IDs |
| Missing from ROADMAP (material on `main`) | 8 themes |
| Stale or misleading ROADMAP entries | 3 |
| Planning-only proposed work (no ROADMAP ID) | 4 |
| Shipped items in current ROADMAP (to remove from slim file) | ~81 |

---

## A. Missing from `ROADMAP.md`

These are the largest gaps. Substantial code exists; the current roadmap has no Hearth theme and
no AC-4 decode entry.

| Plain-English name | Doc sources | Code anchors | Done | Not done | Status | Doc accuracy | New roadmap line |
|---|---|---|---|---|---|---|---|
| **Hearth desktop window** | `hearth-reference-player.md` A5; `docs/hearth/index.md` | No `apps/hearth/ui/`; engine in `apps/hearth/engine/`; 17 `tests/hearth/*.cpp` files | Engine, transport, gapless, passthrough, meters, settings model, diagnostics | Qt window, drag/drop queue UI, A6 network UI, A7 packaging, A8 user guide | **In progress** | **Missing** (planning only) | Desktop player engine is in CI; the application window and packaging are not started. |
| **Hearth Sendspin server in app** | `hearth-reference-player.md` A4 | `pairing_store.*` in engine; `src/sendspin/`; `tests/hearth/test_group.cpp` uses `ServerHost` directly; `apps/hearth/engine/CMakeLists.txt`: "ac3::sendspin joins when the network output lands" | Protocol library, testserver, testsink, group/pairing tests, aiosendspin CI exit | `ServerHost` wired into `ac3hearth_engine` for network output | **Partial** | **Misleading** if A4 read as fully merged in app | Sendspin library and test tools work; the desktop app does not host a server yet. |
| **Hearth UI design (A0)** | `hearth-reference-player.md` A0 | No signed design record in repo | Decision log in planning page | Published design round sign-off before A5 | **Proposed** | **Missing** | UI design must be signed off before the window phase starts. |
| **Hearth ESP32-C6 sink** | `hearth-reference-player.md` C3 | C6 decoder/build (`esp-idf/…`); no C6 sink guide; no Sendspin CI for C6 | C1–C2 bring-up | Sendspin sink firmware and guide (follows S3 sink) | **Not started** | **Missing** | C6 Sendspin sink follows the S3 sink pattern. |
| **AC-4 audio decode** | Chip D in hearth plan; IM4 (inspect only) | `src/ac4/` inspect; `src/ac4dec/` syntax trace (`decoder.hpp`: "produces no audio yet"); `tests/ac4dec/`, `tests/golden/ac4dec/*.tsv` | TOC/framing (shared with inspect); channel-coded syntax transcription; DRC metadata parsing in syntax layer | PCM output; speech frontend; object/immersive substreams; 96/192 kHz; dialogue enhancement decode | **Partial** | **Misleading** (IM4 "complete" reads as full AC-4) | AC-4 container inspect is shipped; audio decode is syntax-only so far. |
| **QC delivery report file** | `planning/qc-report.md`; `planning/README.md` | `ac3cli qc` prints to stdout only | QC analysis in CLI/GUI | File-shaped delivery report on disk | **Proposed** | **Missing** | A file export for QC results is proposed, not started. |
| **DAW/NLE host plugin study** | `planning/host-plugin.md` | No plugin code | Feasibility study | Any product decision or implementation | **Proposed** | **Missing** (AP10 is GStreamer/FFmpeg only) | Whether a host plugin is worth building is an open study. |
| **Topology / transport roles** | `planning/topology.md` | Decision 1 taken; no transport implementation | Source/transport/sink vocabulary | HLS/CMAF transport for non-Hearth members (Hearth uses Sendspin) | **Proposed / mostly superseded** | **Missing** | Shared transport vocabulary is decided; Hearth sinks use Sendspin instead of HLS. |

---

## B. Active `ROADMAP.md` items (reconciled)

### B1. In progress

| ID | Plain-English name | Code anchors | Done | Not done | Status | Doc accuracy | New roadmap line |
|---|---|---|---|---|---|---|---|
| **IM5** | TrueHD experimental module | No `AC3FORGE_BUILD_MLP` on `main`; work on `feature/truehd-atmos-support` branch per ROADMAP | Internal codec on branch | Rebase, gate as `ac3::mlp`, label output, merge to `main` | **In progress** (off main) | Accurate | Land TrueHD/MLP as an experimental, accurately labelled module from its long-lived branch. |
| **VX9** | Listening test session | `tools/listening/`; `tools/listening/responses/README.md`: "No listening session has been run yet" | Blind stimulus generator, scorer, protocol on `docs/landscape.md` | Human MUSHRA/ABX session over real programme material | **Partial** | Accurate | Listening-test apparatus is merged; the session itself has not been run. |
| **VX12** | Cross-toolchain bitstream reproducibility | ROADMAP self-labels PARTIAL; `tests/encoder/test_coupling.cpp` (`ilogb` fix); `tests/encoder/test_transient.cpp` all six rates | Audit complete; one libm fix landed; transient detector proven bit-identical on five toolchain legs | Fixed-point transient port; re-validation of coupling-fit/AHT/rematrix thresholds; cross-leg gold gate blocked on VX11 arm64 root cause | **Partial** | Accurate | Encoder bit-cost decisions are mostly audited; perceptual re-validation and a few FP comparisons remain open. |
| **AP1** | API freeze → v1.0.0 | `docs/library/api-stability.md`; version macros in `ac3forge.h`; `.github/workflows/_ci-core.yml` `ABI_ENFORCE: 'false'`; `SOVERSION "${PROJECT_VERSION}"` in library CMake | Tiering doc, SemVer policy, release criteria, C version macros | Flip `SOVERSION` to major; `inline namespace v1`; make ABI gate **required** | **In progress** | Accurate | v1.0 policy is written; SOVERSION and a enforcing ABI gate wait for the v1.0.0 cut. |
| **UX12** | Crucible promotion | `apps/crucible/`; CI `crucible: true` on Windows/Linux legs; `docs/crucible/index.md` | Rename; Windows + Linux verified on hardware (Pi); engine, window, packages on Linux; macOS platform code | macOS interactive run; native-speaker translation review (→ CR1); attestation-signed Windows driver | **Partial** | **Stale** in ROADMAP summary (see below) | Crucible ships on Windows and Linux; macOS compiles in CI but has never run with audio on a Mac. |
| **UX7** | macOS process tap | `src/audio/src/backend/macos/process_tap.mm`; `audio_backend.cpp` refuses `process_loopback` unless `AC3FORGE_MACOS_PROCESS_TAP` | Tap code compiles; device watcher registers; opt-in env var | `AudioDeviceCreateIOProcID` hang; path disabled by default; no successful capture on Mac | **Partial** | Accurate (2026-09-06 update) | macOS process tap code exists but is refused by default until the HAL hang is resolved. |
| **DR9** | Hardware confirmation per backend | `docs/platforms/raspberry-pi.md`, `windows.md`; `docs/crucible/design/promotion.md` | Linux/ALSA passthrough; PipeWire on Pi; Windows WASAPI exclusive to Onkyo AVR | CoreAudio tap on real Mac; Crucible on Mac; Pi 5; second Android TV | **Partial** | Accurate | ALSA, PipeWire and WASAPI passthrough are confirmed; CoreAudio and desktop Mac runs are not. |

#### UX12 stale text (must fix when rewriting)

`ROADMAP.md` lines ~2598–2602 still say macOS "none of it has been compiled" and "CI has not
reported back." **`docs/crucible/index.md`** (2026-09-06+) and **DR9** record macOS CI compile,
link, and headless QML suite runs. The accurate gap is **no interactive Mac session with audio**,
not "does not compile."

### B2. Considering / proposed / blocked

| ID | Plain-English name | Code anchors | Status | Doc accuracy | New roadmap line |
|---|---|---|---|---|---|
| **EQ2** | Per-channel/per-block SNR offsets | Swept and declined per ROADMAP full record | **Out of scope** (tried, declined) | Accurate | Declined after measurement; reference encoders agree with shipped behaviour. |
| **EQ14** | Perceptual criterion calibration | `kPerceptual` in encoder search; open threads from EQ13 | **Proposed** | Accurate | Calibrate the perceptual encoder criterion (EQ13 follow-on). |
| **IM6** | TrueHD interoperability | Blocked on non-public MLP reference and clean-room ruling | **Blocked** | Accurate | Real TrueHD interoperability stays blocked on sources and clean-room policy. |
| **VX5** | Dolby Reference Player wider CI | Licensed player wired locally; crosscheck loop narrow | **Proposed** | Accurate | Widen Reference Player crosschecks (ecpl, tpn, 7.1.4, compr) and run in CI. |
| **AP8** | Generated API reference / versioned docs | No Doxygen/mike pipeline | **Proposed** | Accurate | Generated API reference and versioned docs are not started. |
| **AP10** | GStreamer / FFmpeg encode wrapper | Needs C API (AP5 shipped); no out-of-tree element | **Proposed** | Accurate | Out-of-tree GStreamer/FFmpeg wrapper for >5.1 and JOC encode. |
| **UX10** | TrueHD Forge front ends | Rides IM5 branch GUI/CLI | **Proposed** | Accurate | TrueHD GUI/CLI front ends follow the experimental TrueHD module (IM5). |
| **CR1** | Crucible translation reading | Six `ac3crucible_*.ts` at 385 messages, 0 unfinished | **Partial** | Accurate | Mechanically translated Crucible strings need native-speaker review. |
| **DR3** | vcpkg git registry | Registry not published | **Proposed** | Accurate | Publish a vcpkg git registry for `vcpkg install ac3forge`. |
| **DR4** | winget / ConanCenter | Manifests staged; CLA/submission pending | **Proposed** | Accurate | winget and ConanCenter submissions wait on paperwork. |
| **DR6** | Code signing | Known gap since 0.8.0-beta.2; test-signed Windows driver only | **Blocked** (certificates) | Accurate | macOS notarisation and Windows Authenticode wait on certificates. |

---

## C. Shipped items misread as roadmap work

These are **shipped** but easy to misread without a done/not-done split. They must **not** appear
as active roadmap rows; the new roadmap may mention them only in a legacy index or link to
`CHANGELOG.md`.

| Topic | ROADMAP | Code / docs truth | Inventory note |
|---|---|---|---|
| **AC-4 inspect** | IM4 Shipped | `src/ac4/ac4.hpp`: "INSPECTOR, not a decoder"; `capabilities.md` AC-4 row | Shipped scope = parse/inspect + carriage in MP4/TS |
| **AC-4 decode** | *(absent)* | `src/ac4dec/`: syntax only | **Partial** — separate row in section A |
| **IAMF** | IM3 Shipped (phase 1) | `iamf/iamf.hpp`: "phase 1 of 3"; phases 2–3 wait on IAMF v2.0 final | Shipped = channel-based writer; object elements **blocked** |
| **IAB reader** | IM1 Shipped | `ac3iab`: full header parse; **AudioDataDLC** opaque bytes only | Shipped = reader; Annex B DLC decode is **follow-on**, not ID'd |
| **Multi-programme E-AC-3** | *(capabilities wording)* | `encode.cpp` `programme2=` works; `capabilities.md` L24: **`bsmod` / programme mixing metadata not there yet** | **Partial** — authoring yes, receiver-side mix metadata no |
| **E-AC-3 programme decode** | DC5 Shipped (structural) | `DecoderConfig::programme` selects one programme at a time | Shipped = structural; mixing metadata decode per DC4 |

---

## D. Hearth chip scorecard (planning vs tree)

From `planning/hearth-reference-player.md` status block, verified against `main`.

| Chip | Planning claim | Tree check | Inventory status |
|---|---|---|---|
| **A0** | Design round published, awaits review | No standalone design doc in repo | **Proposed** — gate for A5 |
| **A1** | Merged | `src/forge/include/ac3/render/*` (Experimental tier in api-stability) | **Shipped** |
| **A2** | Merged | `src/audio/` backends, routing, device watch | **Shipped** (hardware identify tone checks still open per plan) |
| **A3** | Merged | `apps/hearth/engine/*`, 17 hearth test files | **Shipped** |
| **A4** | Merged | Sendspin lib + testserver + tests; **engine lacks ServerHost** | **Partial** |
| **A5** | Not started | No `apps/hearth/ui/` | **Not started** |
| **A6–A8** | Follow A5 | — | **Not started** |
| **B1–B5** | Merged | `hearth_sink` example, Sendspin CI (`hearth-esp32s3` job) | **Partial** — firmware done; TDM DAC + Music Assistant on real MA open |
| **C1–C2** | Merged | ESP32-C6 fixed-point path | **Shipped** |
| **C3** | Follows B | No C6 sink guide/CI | **Not started** |
| **D** | First code merged | `src/ac4dec` syntax only | **Partial** — see section A |

**Open hardware exits** (called out in planning, not in ROADMAP): Onkyo identify tone and
passthrough tests for A2/A3; TDM DAC boards for sinks; Music Assistant compatibility on real MA
(B3/A4 exits — aiosendspin 9.1.1 scripted exit passes in CI).

---

## E. Product pages vs ROADMAP (authority check)

| Product | Authoritative page | Agrees with inventory? | Action for rewrite |
|---|---|---|---|
| **Library** | `docs/library/capabilities.md` | Yes for AC-4, IAMF, IAB boundaries | Split AC-4 inspect/decode in capabilities if needed |
| **Hearth** | `docs/hearth/index.md` | Yes — "no window", sink works | Roadmap links here; add explicit AC-4 decode row |
| **Crucible** | `docs/crucible/index.md` | Yes — macOS compiles, never run with audio | Fix ROADMAP UX12 stale summary |
| **Forge** | `docs/forge/index.md` | *(not fully audited this pass)* | Spot-check when writing slim roadmap |

---

## F. CMake / CI map (build truth)

| Flag / job | Path | Default | Roadmap relevance |
|---|---|---|---|
| `AC3FORGE_BUILD_AC4` | `src/ac4`, `src/ac4dec` | ON | AC-4 inspect + syntax decoder |
| `AC3FORGE_BUILD_HEARTH` | `src/sendspin`, `apps/hearth` | OFF | Hearth engine/tests when ON |
| `AC3FORGE_BUILD_CRUCIBLE` | `apps/crucible` | OFF | UX12; CI sets ON on selected legs |
| `AC3FORGE_BUILD_IAMF` | `src/iamf` | ON | IM3 phase 1 |
| `AC3FORGE_BUILD_IAB` | `src/ac3iab` | ON | IM1 |
| `AC3FORGE_BUILD_ADM` | `src/ac3adm`, `src/admbridge` | OFF | ADM/JOC bridge optional |
| `hearth-esp32s3` CI job | `.github/workflows/_build.yml` | — | B-chip sink verification |
| `ABI_ENFORCE` | `_ci-core.yml` | `false` | AP1 deferred gate |

---

## G. Legacy ID → new plain names (for slim ROADMAP footer)

Do **not** allocate new IDs. One old ID may map to **multiple** new names when scope was conflated.

| Legacy ID | New plain name(s) | Disposition |
|---|---|---|
| IM4 | AC-4 container inspect | Shipped — leave CHANGELOG |
| *(none)* | AC-4 audio decode | Partial — new row |
| IM3 | IAMF channel-based writer | Shipped |
| *(IM3 tail)* | IAMF object elements + reader | Blocked on IAMF v2.0 |
| IM1 | IAB reader | Shipped |
| *(IM1 tail)* | IAB AudioDataDLC decode | Proposed follow-on |
| UX12 | Crucible cross-platform product | Partial |
| UX7 | macOS process tap | Partial |
| AP1 | API freeze v1.0 | In progress |
| DR9 | Hardware verification | Partial |
| A5 *(planning)* | Hearth desktop window | Not started |

---

## H. Sign-off checklist (Phases 0–2)

- [x] Every **Partial** row has explicit Done / Not done from code
- [x] Every **Missing** row has a draft new-roadmap line
- [x] **Stale** rows identified (UX12 macOS summary)
- [x] Hearth chip scorecard cross-checked
- [x] Product pages updated (`capabilities.md` multi-programme line)
- [x] New slim `ROADMAP.md` written from this inventory (Phase 1)

## I. Phase 3 — planning consolidation (2026-09-17)

- [x] [planning/README.md](README.md) reorganised (active / studies / records / superseded)
- [x] [planning/SUPERSEDED.md](SUPERSEDED.md) — index of replaced and historical plans
- [x] "Allocate a roadmap ID" / "proposed ROADMAP entry" removed from plan templates
- [x] [CONTRIBUTING.md](../CONTRIBUTING.md) — roadmap vs planning vs product page authority
- [x] [recasting.md](recasting.md) § The roadmap marked historical

## Optional follow-up

- Opportunistic cleanup of `ROADMAP XXn` comments in source (no CI gate required)
