# A host plugin: what one could do, which formats are open, and what shipping one takes

!!! note "Status: study and plan, written 2026-09-07. Nothing here is decided."
    The library has C, Python, Rust and npm bindings and no form a DAW, an NLE or a media
    pipeline can load. This page asks first whether a useful plugin is possible at all
    ([Part 1](#part-1-the-capability-study)), and then plans the one Part 1 recommends
    ([Part 2](#part-2-the-plan)). It keeps the shape of
    [the family recasting](recasting.md) and [the Crucible promotion](../crucible/promotion.md):
    design sections saying what changes and why, phases carrying an exit criterion and how it is
    verified, a [Decisions](#decisions) section listing what only the owner can settle, and a
    [What cannot be verified](#what-cannot-be-verified-and-why) table written before the work
    rather than after it.

    Part 1's answer is **a metering and QC plugin, as CLAP and VST3, and not yet an encoder**.
    The reasons are the object-metadata wall ([here](#object-audio-is-where-it-stops)) and one
    unmeasured number ([here](#real-time-safety)).

The project is unaffiliated with Dolby Laboratories, Steinberg, Avid and Apple. "Dolby",
"Dolby Digital" and "Dolby Atmos" appear below only as format names, and the marks of the
plugin-format owners only to name their formats and tools.

## Part 1 — the capability study

### What the library could put in a host

Four candidates, evaluated rather than assumed. The verdict column is what the tree supports
**today**, by file path.

| Candidate | What it would do | Verdict |
|---|---|---|
| **Metering and QC** | BS.1770 loudness, true peak, LRA and broadcast-preset pass/fail over a bus | **Supported today.** The one candidate needing no encoder |
| **Encoder for monitoring** | render a bus to AC-3 / E-AC-3 / E-AC-3 with JOC, to hear what a coded delivery sounds like | **Possible, needs work.** One missing API and one unmeasured number |
| **Decoder on a timeline** | play coded material in place | **API ready, use case thin.** The best-supported entry points in the tree, with nothing to feed them |
| **Object panner writing ADM** | author positions, emit object metadata | **Blocked by the formats**, not by the library |

**Metering and QC.** `ac3::meta::LoudnessMeter`
(`src/forge/include/ac3/meta/loudness.hpp:76`) is already shaped like a plugin: a `push()` that
takes any number of samples, and const getters for momentary, short-term, integrated, loudness
range and true peak. It has two constructors — BS.1770 Annex 1 keyed on `acmod`, and Annex 3
over a rendered wide layout — so 5.1.4 and 7.1.4 are measurable, not just 5.1.
`src/forge/include/ac3/meta/qc.hpp:60` adds named broadcast limits with pass/fail and margins.
Nothing in this path encodes anything, so none of the encoder's constraints below apply to it.

**Encoder for monitoring.** All three encoders take the same shape: pimpl'd, scratch allocated
once in the constructor, spans in, one owning buffer out —
`ac3::FrameEncoder::encode_frame` (`src/forge/include/ac3/encoder/encoder.hpp:187`),
`ac3::eac3::FrameEncoder::encode_frame` (`src/forge/include/ac3/encoder/eac3_frame.hpp:560`)
and `ac3::oba::AtmosEncoder::encode_frame` (`src/forge/include/ac3/oba/atmos.hpp:167`). Each
reports a `LatencyBudget` (roadmap PF6), so the number a plugin must declare to its host is
already computable rather than guessed. What is missing is in
[Real-time safety](#real-time-safety).

**Decoder on a timeline.** `ac3forge_decoder_decode_frame_into()`
(`src/capi/include/ac3forge_c/ac3forge.h:366`) and
`ac3forge_eac3_decoder_decode_access_unit_into()` (`:664`) are the only codec entry points in
the tree that write into caller-owned memory, and their own comments say they exist "for a
realtime embedder". The API side of a decode plugin is done. The use case is the weak half: a
DAW decodes coded material at import and puts PCM on the timeline, so a decode plugin sitting
in an insert slot has no coded input to receive. A host would have to hand a plugin an
elementary stream, and no format below has a way to do that.

**Object panner.** The library has the machinery — `src/ac3adm` for BW64/ADM,
`src/forge/include/ac3/oba/oamd.hpp`, `scene.hpp` and `motion.hpp` for object metadata and
trajectories, `src/admbridge` for coordinate conversion. The wall is the formats, and it is the
next section.

### The formats, and what they can express

| | VST3 | AU (v2 / v3) | AAX | CLAP | LV2 |
|---|---|---|---|---|---|
| Owner | Steinberg | Apple | Avid | free-audio (Bitwig, u-he) | community |
| Platforms | Win, macOS, Linux | macOS (and iOS for v3) | Win, macOS | Win, macOS, Linux | mostly Linux |
| Channel beds | `SpeakerArrangement` bitset | `AudioChannelLayout` | channel-count based | `clap.surround/4` channel mask | port groups |
| Height channels | yes — `k51_4`, `k71_4`, `k91_6` | yes | yes | yes — 9 top positions | yes |
| Ambisonics | yes — 1st to 7th order ACN/SN3D | via layout tags | no | via channel mask | via extensions |
| **Object metadata** | **no** | **no** | **no** | **no** | **no** |
| Validator | Steinberg's `validator` | `auval` | Avid's, behind the agreement | `clap-validator` | `lv2lint` |
| SDK licence | MIT | part of the macOS SDK | click-through agreement | MIT | ISC |

VST3's arrangements are the concrete case. `Steinberg::Vst::SpeakerArr` defines `k51_4`,
`k71_4` and `k91_6` alongside `kAmbi1stOrderACN` through `kAmbi7thOrderACN`, and it defines
`k91Atmos` — which is an alias for `k71_2`. That name is worth reading carefully: it labels a
**fixed twelve-channel arrangement**, not an object bed. Everything in that namespace is a
bitset of speaker positions. There is no per-object position, no object count, no metadata
channel.

CLAP is the same answer arrived at independently. Its surround extension defines twenty speaker
positions including nine height positions, and nothing else; the identifier is `clap.surround/4`,
with the `clap.surround.draft/4` compatibility alias marked for removal during 2026, so the
extension is settled rather than in flux.

### Object audio is where it stops

A plugin in any of the five formats sees a fixed arrangement of channels. It cannot be told that
channel 7 is an object at azimuth 30°, and it has no way to say so either.

This is not an omission the project could work around, because it is how object production is
actually wired. Dolby's own Music Panner ships as a VST3, AU and AAX plugin, and it does **not**
carry object metadata through the plugin API — it sends positions out-of-band to the Dolby Atmos
Renderer, over a private path between the two. Where a DAW has object panning built in (Nuendo's
VST MultiPanner, Pro Tools' own panner), "object" is a **host** concept: the host routes an
object track to its renderer and its own panner writes the metadata. A third-party plugin is not
in that conversation, and the plugin formats give it no way to join.

Two consequences follow, and they set the shape of everything below.

- **An object panner is not possible in any open plugin format.** The blocker is not GPL-3.0,
  not the SDK, and not anything in `src/ac3adm` or `src/forge/include/ac3/oba/`. It is that the
  formats carry no object metadata and the renderer path is private.
- **A bed is a different question, and the answer there is yes.** 5.1.4 and 7.1.4 arrive at a
  plugin as ordinary channel arrangements every format above can express, and
  `LoudnessMeter`'s Annex 3 constructor already measures exactly those layouts. Metering a bed,
  and encoding one, are both reachable. Authoring objects is not.

### Real-time safety

A plugin's `process()` **is** the host's audio callback. This is the difference between a plugin
and everything the project has built so far, and it is worth stating plainly: Crucible does not
run the encoder on a callback. `apps/crucible/engine/engine.hpp:20` says "One thread runs the
frame loop", commands are applied at frame boundaries, and `output_stage.hpp` splits endpoint
work into a "slow half, safe on any thread" and a "fast half, frame thread only" — the comment
there records that the slow half, run on the frame thread, once cost seven seconds. Crucible's
architecture is a decoupled frame thread feeding a sink, and it does not transfer to a plugin
unchanged; a plugin would need the same decoupling built inside it.

**What the tree already gets right.** A scan of `src/forge/src/{encoder,oba,spatial,quality,meta,core,dsp}`
for `std::mutex`, `lock_guard`, `std::cout`, `std::cerr`, `fopen`, `ofstream`,
`std::this_thread`, `condition_variable` and bare `malloc` returns **zero hits in all seven
directories**. There is no locking, no I/O, no logging and no sleeping anywhere in the encode,
object, spatial, quality, metadata, core or DSP paths. The per-frame scratch — MDCT history,
exponent plans, coupling buffers, the bit-allocation working set — lives in the pimpl and is
sized once; the `resize()` calls inside `encode_frame` are on those persistent members and
become no-ops once the stream count settles.

**The one structural allocation.** Every encode returns an owning buffer, and the bit writer is
emptied to produce it: `BitWriter::take()` is `std::exchange(bytes_, {})`
(`src/forge/include/ac3/core/bitwriter.hpp:65-68`), so the writer starts each frame with no
capacity and grows again, and `encoder.cpp:2500` then returns that buffer by value. That is
heap traffic on every frame, on what would be the audio thread. The fix is already modelled in
the tree: the decode side has `..._into()` entry points that write into caller memory
(`ac3forge.h:366`, `:664`); the encode side has no counterpart. **An `encode_frame_into()` — and
a `BitWriter` that keeps its capacity — is the single missing library API an encode plugin
needs.**

**What is bounded, and what is not.** The SNR-offset search is a bounded binary search:
`search_max_fitting` (`src/forge/src/encoder/snr_search.hpp:33`) probes over `[0, limit]`, warm-
started from the previous frame's answer, and falls back to a plain binary search when the hint
misses. So the probe count has a ceiling. The cost per probe is a full bit allocation over every
stream, and the hint misses precisely at transients and scene changes — which is to say, the
worst case is correlated with the content a monitoring plugin exists to check.

**What would have to be measured, and how.** The WASM demo's 385× / 120× / 82× real-time figures
for AC-3, E-AC-3 and four-object Atmos are throughput averages over a file. An audio thread is
not governed by an average; it is governed by the worst block. The measurement that decides
whether an encode plugin is possible is:

1. **Worst-case wall time for one `encode_frame`**, as a distribution rather than a mean —
   maximum, 99.9th and 99th percentiles over a corpus that includes transients and scene changes,
   since those are where the search hint misses. The gate is the host's block deadline: at 48 kHz
   a 256-sample block is 5.33 ms, and one 1536-sample frame
   (`src/forge/include/ac3/core/tables.hpp:22`) spans six of them, so a frame's work must fit
   inside roughly 32 ms of budget shared with every other plugin in the session.
2. **Allocation count and bytes per `encode_frame` in steady state**, captured by an
   instrumented global allocator around the call, to confirm the only remaining traffic is the
   returned buffer and to prove the scratch resizes have gone quiet.
3. **The same two, per configuration** — AC-3, E-AC-3, and Atmos with JOC — because the object
   path adds a reconstruction-matrix solve the bed path does not have.

The project already has the harness shape for this: `tools/checks/` and the existing performance
gates measure paired medians on a shared host. What does not exist is a worst-case block-time
probe, and that is Phase 0 of the encoder work rather than something to assume either way.

A metering plugin needs none of this. It runs no encoder, so it has no search, no bit writer and
no returned buffer — its own real-time issue is smaller and named in
[What it builds on](#4-what-it-builds-on).

### Licensing, per format

Every claim below was checked against the format owner's own current terms on **2026-09-07**,
rather than taken from prior knowledge. One of them had changed.

| Format | SDK licence, as of 2026-09-07 | Workable under GPL-3.0? | Source |
|---|---|---|---|
| **VST3** | **MIT** | **yes** | [Steinberg VST 3 licensing FAQ](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Licensing.html) |
| **CLAP** | **MIT** (Alexandre BIQUE, 2021) | **yes** | [free-audio/clap LICENSE](https://github.com/free-audio/clap/blob/main/LICENSE) |
| **LV2** | ISC | yes | [lv2/lv2](https://github.com/lv2/lv2) |
| **AU** | part of the macOS SDK; no separate plugin-SDK licence | yes, but see signing | [Apple Developer](https://developer.apple.com/) |
| **AAX** | click-through agreement; Avid account; iLok account and USB key for the signing process; commercial tooling by arrangement with Avid | **no**, without a dual-licence decision | [developer.avid.com/aax](https://developer.avid.com/aax/) |

**VST3 is no longer dual-licensed, and the GPLv3 arm is no longer the relevant one.** Steinberg
moved the VST 3 SDK to the MIT licence with VST 3.8 in late 2025; its own licensing FAQ now
states MIT, notes that MIT "does not require you to disclose your source code", and describes
following the VST usage guidelines as "best practice, but it is optional". The previous
proprietary-or-GPLv3 dual model is gone. For this project the practical effect is that VST3 has
become as licence-cheap as CLAP, and the calculation that would have made CLAP the obvious sole
entry no longer holds. (Separately, Steinberg moved the ASIO SDK to GPLv3 in the same change;
nothing here uses ASIO.)

**AAX is the one format GPL-3.0 closes.** Avid's own page requires a click-through licence
agreement, an Avid account, and an iLok account, and states that commercial AAX development
"requires an iLok USB key as part of the AAX digital signing process", with the tools and licence
obtained by contacting Avid directly. Distributing a GPL-3.0 binary that only functions when
signed by a third party under a non-disclosable agreement conflicts with GPL-3.0's terms on
additional restrictions and on the information needed to install a modified version. Iain
Chesworth holds the copyright, so **dual-licensing is a decision available to him** rather than
an impossibility — but it is his decision, it is the only route to AAX, and it should be taken
with legal advice rather than on the strength of this page. See
[decision 5](#decisions).

**A GPL-3.0 plugin loaded by a proprietary DAW.** This is settled by practice rather than by
anything novel here: GPL-licensed plugins ship as VST3 and LV2 and are loaded by proprietary
hosts today. The plugin is a separate work, distributed on its own terms; the user performs the
combination at runtime, and running a program is not a restricted act under GPL-3.0. What
mattered was the SDK licence, and for VST3, CLAP and LV2 that is now permissive. This paragraph
is a reading of the licences and not legal advice; the AAX row is the one where the reading is
contested enough to warrant more than a reading.

### What the study concludes

**A metering and QC plugin is possible now. An encoder plugin is possible after one API addition
and one measurement. An object panner is not possible in any open format, and no amount of work
in this repository changes that.**

The recommendation is therefore:

- **Type: metering and QC.** It is the only candidate the library supports today, it needs no
  encoder on the audio thread, and it is the one whose value does not depend on a format feature
  that does not exist. It also puts the project's loudness and QC instruments — which are
  unusually complete, including BS.1770-5 Annex 3 wide layouts and named broadcast presets — in
  front of people who cannot use a CLI.
- **Formats: CLAP first, VST3 alongside it.** Both are MIT as of 2026-09-07, both have a
  command-line validator that can run in CI, and both build on all three platforms the project
  already targets. CLAP is first because `clap-validator` is the easier CI citizen and the
  format has no trademark guidelines to observe; VST3 follows immediately because it is what
  most hosts actually load.
- **Not AAX**, unless [decision 5](#decisions) goes the other way. **Not AU in the first
  release**: it is macOS-only and it lands squarely on DR6, which is unresolved
  ([Signing and install](#14-signing-and-install)). **Not LV2** in the first release — it adds a
  third packaging shape for an audience the first two mostly cover, and it is the cheapest to add
  later.
- **The encoder plugin is "not yet"**, and the conditions that change that are named in
  [Phase 4](#phase-4-the-encoder-question-decides-itself) rather than deferred to judgement.

## Part 2 — the plan

### 1. The name

The plugin measures whether a mix meets a spec. The family's metaphor is metalworking: the
library is a **forge**, the desktop application a **crucible**.

An **assay** is the test that determines a metal's purity and composition. It is what this
plugin does, it sits inside the metaphor without straining it, it is short enough for a plugin
list, and it is not in use as an audio plugin name — a search of plugin listings on 2026-09-07
for a loudness or metering plugin called "Assay" returned none, against a field that includes
Youlean Loudness Meter, LCAST and Loud-A.

| Option | Prose name | Identifiers | Cost |
|---|---|---|---|
| **N1, recommended** | **AC3Forge Assay** | `ac3assay`, `ac3::assay`, `AC3FORGE_BUILD_ASSAY` | one more word the family has to teach; "assay" is unfamiliar to some readers as a verb |
| N2 | AC3Forge Gauge | `ac3gauge`, `ac3::gauge` | plain and instantly understood, but generic — heavily used across software, and weak in a trademark sense |
| N3 | AC3Forge Touchstone | `ac3touchstone`, `ac3::touchstone` | a touchstone is literally the stone used to assay gold, so it is the most exact word of the three; long for a plugin list, and it carries a strong everyday meaning that competes with the technical one |
| N4 | AC3Forge Meter | `ac3meter`, `ac3::meter` | says what it is and nothing else; unregistrable, and it forecloses the plugin ever growing past metering |

**Recommend N1.** It follows the settled spelling rule from
[the recasting](recasting.md#the-name): "AC3Forge Assay" in prose, every identifier lowercase and
`ac3`-prefixed, and the string "AC3Forge Forge" is still never written.

**The name is Iain's decision, not this page's** — see [decision 1](#decisions).

**Identifiers that freeze on first release.** Plugin formats carry identities of their own, and
these cannot change afterwards without breaking every session that loaded the plugin. A host
stores them in the project file; changing one makes an old session load an empty slot.

| Format | Frozen identifier | Proposed value | Consequence of changing it later |
|---|---|---|---|
| CLAP | plugin ID (reverse-DNS string) | `com.iainchesworthlabs.ac3forge.assay` | every saved project loses the plugin |
| VST3 | class UID — a 128-bit FUID | generated once, recorded in the repository | same, and the UID must be generated randomly rather than derived from the name |
| VST3 | subCategory | `Analyzer` | cosmetic; hosts use it for filtering |
| AU (if ever) | type / subtype / manufacturer — three four-character codes | `aufx` / a subtype chosen once / a manufacturer code chosen once | same, and the manufacturer code should be unique across vendors by convention |

The reverse-DNS root follows
[recasting decision 12](recasting.md#decisions): `com.iainchesworthlabs.*`. The VST3 FUID must be
generated and then committed as a literal — deriving it from the name would tie the identity to
a string that [decision 1](#decisions) may still change.

### 2. Which member it belongs to

**A fourth member: AC3Forge Assay.**

The recasting deliberately ruled a fourth member out of its own scope, and its
["Deliberately not in scope"](recasting.md#deliberately-not-in-scope) says so: the Shield demo and
the browser demos stay demos of the library. This plan argues for one anyway, and the argument
has to clear that bar rather than ignore it.

The three existing members divide by **what owns the audio**. The library is called by someone
else's program. Forge is a pair of applications the user runs and points at files. Crucible is an
application that owns a live audio path end to end. A plugin is none of these: it is a
**loadable object that a foreign process owns**, whose lifetime, threading, buffer sizes and UI
surface are dictated by a host the project does not control. That is a different distribution
shape (per-format system directories, not `bin/`), a different test story (a validator, not a
CLI invocation), a different signing story (several hosts refuse to load unsigned), and a
different release cadence (identifiers frozen on first publish).

The alternatives were considered and are weaker:

- **Under the library** — the library is what other code links; a plugin is a terminal artefact
  with a UI, and putting it there makes `find_package(ac3forge)` ambiguous about what it offers.
- **Under Forge** — Forge is "the `ac3cli` + `ac3gui` pair" by
  [recasting decision 1](recasting.md#decisions), a boundary the build, install rules, packaging
  and docs already draw. A plugin joins none of those.
- **Under Crucible** — Crucible owns a device; the plugin owns nothing. They share the metering
  code and no infrastructure.

Rejecting a fourth member entirely would mean filing the plugin under a member whose boundary it
breaks, and the cost of that shows up in exactly the places the recasting worked to make clean.
The fourth member is [decision 2](#decisions), and it is the decision this plan is least
confident should be taken without argument — the recasting closed the question deliberately, and
reopening it is a real cost.

### 3. Scope

**What it does.** One effect plugin, present as CLAP and VST3, that sits on a bus and reports
what the project's instruments measure:

- Momentary, short-term and integrated loudness, loudness range and true peak, from
  `ac3::meta::LoudnessMeter`, for mono through 7.1.4.
- Pass/fail and margin against a named broadcast preset, from `ac3::meta::qc`.
- A dialnorm suggestion derived from integrated loudness — the number the encoder would put on
  a stream, shown before anyone encodes anything.
- A UI showing all of the above, and a reset.

It is an **analyser**: audio passes through unchanged.

**What it does not do.**

- **Encode anything.** No AC-3, E-AC-3 or JOC on the audio thread. That is
  [Phase 4](#phase-4-the-encoder-question-decides-itself), gated on a measurement.
- **Author or read object metadata**, or pan anything. Not possible in any open format
  ([above](#object-audio-is-where-it-stops)).
- **Write ADM, BW64 or any file.** No file I/O from a plugin.
- **Decode coded material.** No host hands a plugin an elementary stream.
- **Ship as AAX**, unless [decision 5](#decisions) says otherwise.
- **Ship as AU or LV2 in the first release.**
- **Replace `ac3cli`'s QC reporting**, which stays the reference and the thing the plugin is
  checked against.
- **Talk to Crucible**, or to any renderer.

### 4. What it builds on

| Need | Where it exists | State |
|---|---|---|
| Loudness, true peak, LRA | `src/forge/include/ac3/meta/loudness.hpp:76` | complete; see the gap below |
| Broadcast presets, pass/fail | `src/forge/include/ac3/meta/qc.hpp:60` | complete |
| Wide-layout loudness (5.1.4, 7.1.4) | `LoudnessMeter`'s Annex 3 constructor | complete |
| Level analysis | `src/forge/include/ac3/analysis/levels.hpp` | complete |
| Qt Quick UI patterns, theming, accessibility | `apps/gui`, `apps/crucible/ui` | complete, but see [10](#10-localisation-and-accessibility) |
| Notices composition | `apps/notices/notices.cmake` | complete |
| Packaging components | `cmake/Packaging.cmake:454` | needs one component added |

**What is missing in the library, and it is one thing.** `LoudnessMeter` is built for a file: a
finite programme, measured once, reported once. Two consequences make it wrong for a plugin left
open on a mixer for hours, and both are in `src/forge/src/meta/loudness.cpp`:

- **The history grows without bound.** `push_block()` appends one double per 100 ms to
  `block_power_` (`:477`) and one to `short_term_power_history_` (`:500`). The header comment is
  accurate that this is "one double per 100 ms of programme" — over an eight-hour session that
  is roughly 4.6 MB and, more importantly, a `push_back` on the audio thread whose reallocations
  are unbounded in the worst case.
- **The getters are O(n) and allocate.** `integrated_lkfs()` builds a local vector over the whole
  history (`:527`), and `loudness_range()` does the same and then sorts (`:552`). A UI polling
  those at a refresh rate re-scans the entire session every frame, and the cost grows for as long
  as the plugin is open.

Neither is a defect for the CLI, where the programme is bounded and the answer is wanted once.
Both need addressing before a plugin, and the fix is a library change rather than a plugin
workaround, because the correct answer — a bounded reservoir or a streaming gate that keeps the
statistic without keeping every block — belongs next to the algorithm. This is
[Phase 1](#phase-1-the-library-gap) and it is the only library work the metering plugin needs.

For the encoder plugin, the missing API is `encode_frame_into()` plus a capacity-preserving
`BitWriter`, described in [Real-time safety](#real-time-safety).

### 5. Build identity

| | Value | Following |
|---|---|---|
| Targets | `ac3assay_clap`, `ac3assay_vst3`, and `ac3assay_core` holding the format-independent half | `ac3crucible` / `ac3crucible_engine`'s split |
| Namespace | `ac3::assay` | Crucible's `ac3::crucible` |
| CMake option | `AC3FORGE_BUILD_ASSAY`, default `OFF` | `AC3FORGE_BUILD_CRUCIBLE` (`CMakeLists.txt:138`), also `OFF` |
| Per-format options | `AC3FORGE_ASSAY_CLAP`, `AC3FORGE_ASSAY_VST3`, both `ON` when the parent is on | new; a builder without the VST3 SDK still gets CLAP |
| CPack component | `assay`, appended to `CPACK_COMPONENTS_ALL` (`cmake/Packaging.cmake:454`) only when built | the `crucible` component's conditional append (`:464`) |
| QML URI | `Ac3ForgeAssay` | `Ac3ForgeCrucible` |
| Catch2 tag | `[assay]` | tags become ctest labels (`tests/CMakeLists.txt:617`) |

**Plugins do not install to `bin/`.** Each format has a system path, and the bundle layout is
part of the format rather than a choice:

| Format | Windows | macOS | Linux |
|---|---|---|---|
| CLAP | `%COMMONPROGRAMFILES%\CLAP\` | `/Library/Audio/Plug-Ins/CLAP/` | `/usr/lib/clap/` |
| VST3 | `%COMMONPROGRAMFILES%\VST3\` | `/Library/Audio/Plug-Ins/VST3/` | `/usr/lib/vst3/` |
| AU (if ever) | — | `/Library/Audio/Plug-Ins/Components/` | — |

A CLAP plugin is a single shared object named `.clap` on Windows and Linux, and a bundle on
macOS. A VST3 is a **directory** ending in `.vst3` on every platform, with the binary under
`Contents/<arch>-<os>/` — so the install rules are directory installs, not file installs, and
`GNUInstallDirs` does not describe any of these paths. The per-platform prefixes have to be
written out, and a user-level alternative (`~/.clap`, `~/.vst3`,
`~/Library/Audio/Plug-Ins/`) offered for anyone who cannot write to a system directory.

### 6. Tests

**It joins `ac3tests`.** That binary is single by design and the recasting keeps it that way; a
per-member binary was considered there and rejected. Assay's cases compile into it, tagged
`[assay]`, which `catch_discover_tests(ac3tests ADD_TAGS_AS_LABELS)`
(`tests/CMakeLists.txt:617`) turns into the ctest label `assay`. The UI tests follow Crucible's
pattern: a separate `ac3assay_qmltests` executable, one `add_test` per `tst_*.qml`
(`apps/crucible/ui/tests/CMakeLists.txt:147`), label `assay-ui`, and the environment those tests
need — `QT_QPA_PLATFORM=offscreen` **with** `QT_QUICK_BACKEND=software`, because offscreen alone
leaves the scene graph asking for a GPU context and hanging on a runner with no display server.

**How a plugin is tested without a host**, in three layers:

1. **The core, directly.** `ac3assay_core` has no format dependency: samples in, measurements
   out. Its cases are ordinary `ac3tests` cases, and they are where correctness is actually
   established — including the one that matters most, that the plugin's numbers agree with
   `ac3cli`'s on the same input.
2. **The format wrappers, through a validator.** Each format ships one, and this is what replaces
   a host:

    | Validator | Format | Runs in CI? |
    |---|---|---|
    | `clap-validator` | CLAP | **yes** — a standalone binary, no DAW, no display |
    | Steinberg's `validator` | VST3 | **yes** — built from the SDK the plugin already vendors |
    | `auval` | AU | only on a macOS runner, and only if AU is ever built |
    | Avid's | AAX | **no** — behind the developer agreement |

    Both validators the first release needs are CI-runnable. That is a large part of why CLAP and
    VST3 are the recommended pair.

3. **In a real DAW — by hand, and it does not run in CI.** No hosted runner has a DAW licence.
   Which hosts were tried, on what versions and on what dates goes in the docs the way
   Crucible's platform table does, and the [What cannot be verified](#what-cannot-be-verified-and-why)
   table says plainly that this is unautomated.

A validator confirms a plugin is well-formed. It does not confirm it is correct, and the
distinction should stay visible in how results are reported.

### 7. CI

The eleven `_build.yml` matrix legs today are Windows MSVC, Windows LLVM (clang-cl), Windows
MSVC arm64, Linux GCC, Linux LLVM, Linux GCC arm64, Linux LLVM arm64, Linux LLVM ASan+UBSan,
Linux LLVM TSan, macOS LLVM and macOS LLVM x64.

**Proposed: an `assay: true` flag on four legs**, matching how `crucible: true` is placed
(`_build.yml:329,342,430,487`):

| Leg | Why |
|---|---|
| Windows MSVC | the platform most plugin users are on, and the compiler the VST3 SDK is most exercised with |
| Linux LLVM | the sanitiser-adjacent toolchain, and where a plugin's threading assumptions break first |
| macOS LLVM (arm64) | the platform where the bundle layout differs and where AU would later live |
| Linux LLVM ASan+UBSan | a plugin runs inside a foreign process; a use-after-free here is someone else's crash |

Flags: `-DAC3FORGE_BUILD_ASSAY=ON`, with `-DAC3FORGE_ASSAY_VST3=ON` only where the SDK is
fetched. The validators run as a post-test step on those four legs.

**Cost.** Four legs of eleven gain a target and two validator runs. The plugin's own compile is
small — the core is a thin layer over library code that leg already builds — so the added time is
mostly the VST3 SDK's own build, once per leg, cacheable. The estimate is single-digit minutes
per affected leg, and it should be **measured on the first CI run and written into this page**
rather than left as an estimate.

**The branch-protection contract.** `.github/branch-protection.md:29` records that `CI Status`
aggregates every required job and is the one required check. Adding `assay: true` to existing
legs changes no job name, so **no branch-protection change is needed** — which is the point of
putting the flag on existing legs rather than adding new ones. If a separate job is ever added,
its name enters `CI Status`'s `needs` list and the `name:` string becomes a frozen contract.

### 8. Packaging and release identity

**Component `assay`**, appended to `CPACK_COMPONENTS_ALL` only when the plugin is built, the way
`crucible` is (`cmake/Packaging.cmake:464`).

| Platform | Shape |
|---|---|
| Windows | `ac3forge-assay-<full>-win64.zip`, plus the plugin directories inside an NSIS installer offering the system and per-user locations |
| macOS | `ac3forge-assay-<full>-Darwin.tar.gz`; a `.pkg` is the right installer for plugin paths later, since a `.dmg` cannot place files in `/Library/Audio/Plug-Ins/` |
| Linux | `ac3forge-assay-<full>-Linux.tar.gz`, plus DEB/RPM `ac3forge-assay` installing to `/usr/lib/{clap,vst3}/` |

**The release route.** `release.yml:278` collects release assets by the `packages-*` artifact
name pattern, with no filter downstream — so the upload step must be named `packages-assay-<preset>`
to be collected, and must produce a `.sha512` beside each archive and set
`DERIVED_VERSION_OVERRIDE`, the same three requirements the Crucible packaging step landed with
([recasting decision 13](recasting.md#decisions)). Getting the artifact name wrong produces a
release with the plugin silently missing, which is the failure mode that decision was written
about.

**Version.** One tag stream, as [recasting decision 15](recasting.md#decisions) settled. The
plugin's own frozen identifiers ([section 1](#1-the-name)) are independent of the version and
never change with it.

### 9. Documentation

A new **Assay** tab, making eight, placed after Crucible so the nav reads library → tools →
application → plugin:

| Page | Contents |
|---|---|
| `docs/assay/index.md` | what it is, what it measures, what it does not do |
| `docs/assay/install.md` | per-platform paths, system vs per-user, and how to verify a host found it |
| `docs/assay/hosts.md` | per-host setup notes, and which host/version/date each claim was checked on |
| `docs/assay/measurements.md` | what each number means, which standard it comes from, and how it corresponds to `ac3cli`'s |
| `docs/assay/plugin-formats.md` | why CLAP and VST3, why not AAX, and the object-metadata wall — the durable half of Part 1 |
| `docs/family/host-plugin.md` | this page, under Project |

**`docs/assay/hosts.md` is the page that needs the most care**, because installing a plugin
differs by DAW in ways that generate most support questions: where each host scans, whether it
caches a plugin list and how to make it rescan, whether it sandboxes or validates on first load,
and — for the metering case specifically — that a plugin on a bus sees the bus, so where it is
inserted changes what it measures. Each host's row carries the version and the date it was
checked, and hosts nobody has tried are listed as untried rather than omitted.

**`README.md`** gains an Assay row in the layout block and a line in the family statement.

**`docs/index.md`'s "The three members"** becomes "The four members" — one heading, one
paragraph, and the [family recasting](recasting.md) page gains a note that its
"no fourth member" scope line was revisited by this plan and why. That page should not be
silently contradicted; if [decision 2](#decisions) goes this way, the recasting page says so and
links here.

### 10. Localisation and accessibility

**What the project does today.** Six languages plus a pseudo-locale: `ar`, `de`, `es`, `fr`,
`he`, `yi` and `xx`. Three of the six — Arabic, Hebrew and Yiddish — are right-to-left, so RTL is
exercised by half the catalogue rather than being theoretical. Assay ships
`ac3assay_{ar,de,es,fr,he,yi,xx}.ts`, and CI gains the plugin to its "translations are up to
date" check (`_build.yml:1199`).

A gap worth fixing while here: `apps/gui/translations/` has `ac3gui_xx.ts` and
`apps/crucible/translations/` has **no** `ac3crucible_xx.ts`. Crucible was promoted without the
pseudo-locale that catches untranslated strings and layout overflow. Assay should ship `xx` from
its first commit, and Crucible's absence is worth a separate issue rather than being fixed here.

**Accessible roles and names on every control**, matching `apps/gui` and `apps/crucible`: each
meter, readout, preset selector and reset control gets an accessible name and role, and the
numbers are readable as text rather than only as bar geometry — which matters more for a meter
than for most UI, since the entire content is numeric.

**What a plugin host makes impossible**, and this is the part that differs from a standalone
application:

- **The plugin cannot choose its own language.** It inherits the host process's locale. A host
  running under one locale with a user who wants another gives the plugin no way to know, and no
  standard plugin API asks the host what UI language to use. A manual override in the plugin's
  own settings is the only answer, and it is a workaround.
- **The plugin does not own its window.** The host owns the frame, decides resizing (CLAP and
  VST3 both negotiate size, and hosts vary in how well they honour it), and supplies the parent
  handle. A plugin cannot set a window title, and it cannot control where its window opens.
- **Screen-reader support depends on the host's window hierarchy.** A plugin UI is a child of a
  foreign window; whether an accessibility tree reaches it is the host's behaviour, not the
  plugin's. Qt's accessibility works within the plugin's own hierarchy, and whether a screen
  reader traverses into it from a given DAW cannot be established without that DAW. Roles and
  names are still set — the cost of setting them is small and the benefit is host-dependent
  rather than absent.
- **RTL mirroring is the plugin's own**, since it controls its layout — this one is unaffected by
  the host.
- **Keyboard focus is contested.** Hosts commonly grab keys for transport control, so a plugin
  cannot rely on receiving any particular key. Nothing in the UI should be keyboard-only, which
  for a meter is easy and for a text-entry-heavy UI would not be.

### 11. Identity assets

Assay needs an icon for the installer, the DEB/RPM, and any host that shows one.

**The family-wide gap this runs into.** `apps/crucible/CMakeLists.txt:659-664` installs
`apps/gui/icons/ac3forge-256.png` and `ac3forge-32.png` renamed to `ac3crucible.png` — Crucible
ships Forge's icon under its own name. **No member of the family has a mark of its own.** Adding
a fourth member makes that more visible rather than less: three products sharing one icon is
already awkward, and four is worse, particularly in a plugin list where the icon is often all
the user sees.

This plan does not solve it. It notes that the right fix is a family mark with per-member
variants, that it is a design task rather than a code one, and that Assay should not paper over
it by inventing a fourth unrelated icon. Until it is resolved, Assay reuses the family icon the
way Crucible does, which ships a known-wrong asset — worth naming here rather than leaving for a
user to notice. This is [decision 6](#decisions).

**Per-format branding.** CLAP has no branding requirement. Steinberg's VST usage guidelines are,
per its own licensing FAQ, "best practice" rather than mandatory under MIT — but the **VST word
mark and logo remain Steinberg's trademarks**, and MIT licensing of the SDK does not grant
trademark rights. The safe course is to describe the plugin as "VST3-compatible" or to state the
format factually, follow the usage guidelines, and not use the logo as product branding. The
same care applies to any Avid or Apple mark if those formats are ever added.

### 12. Third-party notices

`apps/notices/notices.cmake` composes per-platform notices from fragments in
`apps/notices/fragments/` — today `header.txt`, `qt-linux.txt`, `qt-macos.txt`,
`qt-windows.txt` and `windows-runtime.txt`. Assay adds:

| Fragment | For | Required by |
|---|---|---|
| `clap.txt` | the CLAP headers | MIT — copyright notice and licence text must appear in distributions |
| `vst3.txt` | the VST3 SDK | MIT — same |

Both are MIT, so both require the copyright notice and licence text to travel with any binary
distribution, and both fragments are short. The plugin's notices are installed beside the binary
per platform and, on Linux, once more as `copyright` under `share/doc/`, following the pattern
`apps/crucible/CMakeLists.txt` already uses for the DEB.

Two things to watch. **Qt's fragments already exist but Assay's linkage differs**: a plugin
links Qt into a foreign process, and if Qt is used under the LGPL the relinking obligation
applies to a plugin binary as much as to an application — the existing Qt fragments cover the
notice, not the obligation, and which Qt licence the project ships under governs. **An SDK's
own notice requirements can exceed its licence**: MIT's is the notice above, but if AAX is ever
added its agreement carries requirements that are not public, which is one more reason
[decision 5](#decisions) is a decision rather than a preference.

### 13. Licensing

*The section a reader checks before contributing.*

**The plugin is GPL-3.0, like the rest of the repository.** Iain Chesworth holds the copyright.

**The SDKs it vendors are MIT** — CLAP and VST3, both verified against their owners' current
terms on 2026-09-07 ([the table above](#licensing-per-format)). MIT is GPL-3.0-compatible, so
combining them raises no conflict; the combined work is distributed under GPL-3.0 and the MIT
notices travel with it ([section 12](#12-third-party-notices)).

**VST3's licence changed, and older guidance is wrong.** Until late 2025 the VST3 SDK was
dual-licensed, proprietary or GPLv3, and much of the advice online still says so. It is MIT as of
VST 3.8. Anything a contributor reads that describes a GPLv3 obligation flowing from the VST3 SDK
predates that change.

**A GPL-3.0 plugin in a proprietary host is normal and is how GPL plugins ship today.** The
plugin is distributed on its own terms; the user combines it with a host at runtime.

**AAX is closed to this project as licensed.** It requires a click-through agreement, an Avid
account, an iLok account and — for commercial development — an iLok USB key as part of Avid's
signing process, with tooling obtained by direct arrangement. A GPL-3.0 binary that functions
only when signed under a non-disclosable agreement conflicts with GPL-3.0's terms on additional
restrictions and on installation information. The only route is dual-licensing, which is the
copyright holder's to decide and should be taken with legal advice
([decision 5](#decisions)).

**Trademarks are separate from licences.** MIT on the VST3 SDK grants no rights in Steinberg's
VST mark. "Dolby", "Dolby Digital" and "Dolby Atmos" are Dolby Laboratories' marks and appear in
this project only as format names. The project is unaffiliated with any of them.

**None of the above is legal advice.** It is a reading of published terms on a stated date, with
links, so that a reader can check it rather than trust it.

### 14. Signing and install

**DR6 is unresolved.** `ROADMAP.md:2909` records it as Developer ID and notarisation for macOS,
Authenticode for Windows, blocked on certificates rather than on code, and a Known gap in every
release since 0.8.0-beta.2.

**For a plugin, DR6 is closer to a prerequisite than it is for an application**, and the reason
is that a user can talk their way past Gatekeeper for an application they downloaded
deliberately, while a plugin is loaded by a host that makes that decision for them.

| Format | Behaviour unsigned | Verdict |
|---|---|---|
| **AU on macOS** | AUv3 is an app extension registered by `pluginkit`; distribution outside the App Store requires Developer ID signing, and hosts validate on load | **hard blocker.** DR6 must land before AU is attempted at all |
| **VST3 / CLAP on macOS** | Gatekeeper blocks unsigned, unnotarised bundles from a download; a host may refuse or silently skip the plugin, and some hosts run plugins in a sandbox that requires a valid signature | **hard blocker for macOS distribution.** Buildable and testable locally; not shippable |
| **VST3 / CLAP on Windows** | loads unsigned; SmartScreen warns on the installer | **soft blocker.** Ships, with a warning that costs trust |
| **VST3 / CLAP on Linux** | loads unsigned; no signing infrastructure exists | **not a blocker** |

**The conclusion is that DR6 is a hard prerequisite for the macOS half and not for the rest.**
That is a usable split rather than a dead end: a first release can be Windows and Linux, with
macOS following DR6, and the phases below are ordered so that macOS work is written and compiled
but not published until then. What should **not** happen is a macOS release that tells users to
right-click-open a plugin, because for a plugin the host — not the user — decides whether to load
it.

This raises DR6's priority. It already gates Crucible's macOS half and the tap's consent prompt;
a plugin makes it gate a third thing. That is an argument for resolving it, and the argument
belongs in the roadmap rather than in this page's phases.

### 15. Phases

Each phase has an exit criterion and says how it is verified.

#### Phase 0: the decisions

Nothing is built until [Decisions](#decisions) 1, 2 and 3 are taken — the name, whether there is
a fourth member, and which formats. Every identifier in
[section 1](#1-the-name) freezes on first release and several are derived from the name.

**Exit:** the decisions are recorded on this page, as the recasting records its own.
**Verified by:** this page saying which option was taken and on what date.

#### Phase 1: the library gap

Bound `LoudnessMeter`'s history and make its getters cheap
([section 4](#4-what-it-builds-on)): a fixed-cost integrated statistic and a bounded structure
for loudness range, with no `push_back` on the caller's thread in steady state.

**Exit:** `push()` and every getter perform no allocation after warm-up, and the getters are
O(1) in session length.
**Verified by:** new `ac3tests` cases under `[loudness]` asserting the existing published numbers
are unchanged on the current fixtures — this is a performance change that must not move a single
measurement — plus an allocation-counting test around a long synthetic push, and a run against a
multi-hour input confirming memory is flat.

#### Phase 2: the core, with no plugin in sight

`ac3assay_core`: samples in, measurements out, no format dependency and no UI.

**Exit:** the core's numbers match `ac3cli`'s on the same fixtures, to the last digit either
reports.
**Verified by:** `ac3tests` cases tagged `[assay]` that run both paths over the shared fixture
corpus and compare, so a divergence fails CI rather than being discovered in a DAW.

#### Phase 3: CLAP, then VST3

The two wrappers, and the UI. CLAP first because its validator is the simpler CI citizen.

**Exit:** `clap-validator` and Steinberg's `validator` both pass on Windows and Linux in CI, and
the plugin has been loaded by hand in at least two hosts with the host, version and date written
into `docs/assay/hosts.md`.
**Verified by:** the validator steps on the four `assay: true` legs, plus the hand-checked table
— which is stated as hand-checked, since no CI job can produce it.

#### Phase 4: the encoder question decides itself

Not "build an encode plugin". **Measure**, and let the numbers answer:

1. Build the worst-case block-time probe described in
   [Real-time safety](#real-time-safety) — a distribution, not a mean, over a corpus containing
   transients and scene changes.
2. Add `encode_frame_into()` and a capacity-preserving `BitWriter`, mirroring the decode side's
   existing `..._into()` entry points.
3. Re-measure allocations per call, which should then be zero in steady state.

**Exit:** a table of worst-case frame times per configuration against the block deadline, and a
recommendation — build it, or do not — supported by that table.
**Verified by:** the measurement harness in `tools/checks/`, run on a quiet machine with paired
medians the way the existing performance gates work, with the numbers written into this page.
**An outcome of "the worst case does not fit, so no encode plugin" is a successful phase**, and
the measurement is worth having either way: it is the same number Crucible's frame loop is
implicitly relying on and has never had stated.

#### Phase 5: macOS, when DR6 allows

The macOS bundle layouts, and AU only if DR6 has landed and
[decision 4](#decisions) says to.

**Exit:** signed, notarised bundles that a host loads on a Mac.
**Verified by:** a real Mac. Per
[What cannot be verified](#what-cannot-be-verified-and-why), this cannot be established on CI,
and the phase does not close on a green build.

## The proposed ROADMAP entry

Roadmap ID allocation is Iain's, so this is text to place rather than an edit to make. `ROADMAP.md`
is deliberately untouched by this plan.

> **UX13 (L, needs decisions, blocked on DR6 for macOS)** — AC3Forge Assay, a metering and QC
> host plugin — CLAP and VST3, an analyser over the library's BS.1770 loudness, true-peak, LRA
> and broadcast-preset instruments, for mono through 7.1.4. Study and design record in
> `docs/family/host-plugin.md`, which concludes that an object panner is not possible in any open
> plugin format and that an encode plugin waits on a worst-case block-time measurement. Needs
> `LoudnessMeter` bounded for continuous operation first. macOS distribution is blocked on DR6.

## What cannot be verified, and why

Stated before the work rather than after it, because most of these will still be true when the
code is finished.

| Claim | Can it be verified | Blocker |
|---|---|---|
| CLAP and VST3 SDK licences are MIT | **verified 2026-09-07** against both owners' own pages | none |
| VST3 expresses no object metadata | **verified 2026-09-07** from Steinberg's `SpeakerArr` reference | none |
| CLAP expresses no object metadata | **verified 2026-09-07** from `clap/ext/surround.h` | none |
| The encode path takes no lock and does no I/O | **verified 2026-09-07** — zero hits across seven `src/forge/src/` directories | none |
| `LoudnessMeter`'s history is unbounded | **verified 2026-09-07** at `loudness.cpp:477,500` | none |
| Worst-case `encode_frame` time fits an audio block | **no** | the probe does not exist; it is [Phase 4](#phase-4-the-encoder-question-decides-itself) |
| No DAW would ever expose objects to a third-party plugin | **no** — only that none does through a published API today | host-private renderer paths are undocumented and can change |
| The plugin works in any specific DAW | **no**, not in CI | no hosted runner has a DAW licence; hand-checked per host, with dates |
| Screen readers reach the plugin UI in a given host | **no** | depends on the host's window hierarchy; needs that host and that screen reader |
| AAX's full terms | **no** | behind a click-through agreement and a non-disclosable signing arrangement |
| macOS behaviour of any kind | **no** | DR6 for signing, and a Mac to run it — the same row the [Crucible plan](../crucible/promotion.md#what-cannot-be-verified-and-why) carries |
| "Assay" is free as a trademark | **no** | product listings were searched on 2026-09-07; trademark registers were not, and that is a search for a professional |
| CI cost of the four extra legs | not yet | estimated as single-digit minutes; to be measured on the first run and written here |

## Deliberately not in scope

- **An object panner, in any format.** Not deferred — not possible
  ([above](#object-audio-is-where-it-stops)).
- **Talking to any renderer**, Dolby's or otherwise, over any private protocol.
- **A decode plugin.** The API is ready and no host has a way to hand a plugin coded data.
- **AAX**, absent [decision 5](#decisions).
- **AU and LV2 in the first release.** AU waits on DR6; LV2 is cheap to add later.
- **An encode plugin**, until [Phase 4](#phase-4-the-encoder-question-decides-itself) measures
  the number that decides it.
- **File I/O from the plugin** — no ADM, no BW64, no reports written from an audio process.
- **Replacing `ac3cli`'s QC output**, which stays the reference the plugin is checked against.
- **Changing any existing member's identifiers**, packages or paths.
- **Solving the family icon gap** ([section 11](#11-identity-assets)), which is named here and
  fixed elsewhere.
- **Editing `ROADMAP.md` or `CHANGELOG.md`.** The roadmap entry is
  [proposed as text](#the-proposed-roadmap-entry); the ID is Iain's.

## Decisions

Only what the owner has to decide. Each carries options, a recommendation and the cost of taking
it. **None of these is taken as this page is written.**

1. **The name.** (a) AC3Forge Assay; (b) AC3Forge Gauge; (c) AC3Forge Touchstone; (d) AC3Forge
   Meter. **Recommend (a)** — it names the test for a metal's purity, sits in the forge and
   crucible metaphor, is short in a plugin list, and is not in use as an audio plugin name as of
   2026-09-07. Cost: one more word the family has to teach, and every identifier in
   [section 1](#1-the-name) freezes on first release, so this decision cannot be revisited after
   publication without breaking saved sessions.

2. **A fourth member.** (a) a fourth member, AC3Forge Assay; (b) under the library; (c) under
   Forge; (d) under Crucible; (e) do not build it. **Recommend (a)** — a plugin's distribution,
   testing, signing and release-identity story matches no existing member, and filing it under one
   breaks a boundary the recasting drew deliberately. Cost: this reopens a question the
   [family recasting](recasting.md#deliberately-not-in-scope) closed on 2026-09-05, and that page
   must be amended rather than quietly contradicted. It is the decision this plan is least
   confident should be taken without argument.

3. **Which formats first.** (a) CLAP and VST3; (b) CLAP alone; (c) VST3 alone; (d) all of CLAP,
   VST3 and LV2. **Recommend (a)** — both are MIT as of 2026-09-07, both have a CI-runnable
   validator, and VST3 is what most hosts load. Cost: two packaging shapes and two sets of frozen
   identifiers from the first release rather than one.

4. **AU, and when.** (a) after DR6; (b) never; (c) before DR6, unsigned. **Recommend (a)**.
   (c) is not viable — AUv3 is an app extension and hosts validate signatures on load. Cost:
   macOS users get CLAP and VST3 first and AU later, and AU adds three four-character codes that
   freeze on its own first release.

5. **AAX, and therefore dual-licensing.** (a) no AAX, stay GPL-3.0 only; (b) dual-license so AAX
   becomes possible; (c) revisit if Pro Tools users ask. **Recommend (a)** for now — AAX requires
   an agreement, an iLok account and a USB key in the signing path, and a GPL-3.0 binary that runs
   only when signed under a non-disclosable arrangement conflicts with GPL-3.0. Cost: Pro Tools
   users cannot use the plugin, and Pro Tools is heavily represented in exactly the broadcast and
   post-production work this plugin measures for. **(b) is available** — the copyright is Iain's —
   and it should be taken with legal advice rather than on this page's reading.

6. **The icon.** (a) reuse the family icon as Crucible does, and fix the family-wide gap
   separately; (b) commission a family mark with per-member variants before Assay ships; (c) give
   Assay its own unrelated icon. **Recommend (a)**, with (b) raised as its own piece of work.
   Cost: (a) ships a fourth product wearing Forge's icon, which is a known-wrong asset and gets
   more visible with each member added. (c) is worse — it fragments the family further.

7. **Whether to build any of it.** "Not yet" is an acceptable answer to the whole plan. The
   study stands on its own: the object-metadata wall and the licence table are worth having
   written down whether or not a line of plugin code is ever compiled, and
   [Phase 1](#phase-1-the-library-gap) and
   [Phase 4](#phase-4-the-encoder-question-decides-itself) are both worth doing for the library's
   own sake. Cost of deciding not to build: none that this page can see, which is itself a
   reason to take the decision deliberately rather than by default.
