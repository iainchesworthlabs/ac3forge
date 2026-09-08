# Delivery QC reports

!!! warning "Status as of 2026-09-08: a plan for something that does not exist yet"
    Written 2026-09-07, nothing decided and nothing built. `ac3cli qc` prints its findings and
    exits; it writes no report file, and no option on this page is implemented. Read this as a
    design proposal rather than as documentation of a feature.

    This page plans a **delivery-shaped QC report** — the artefact someone attaches when handing
    a file to a broadcaster or a streaming platform. It keeps the shape of
    [the recasting plan](recasting.md) and
    [the promotion plan](../docs/crucible/promotion.md): design sections say what changes and why,
    each phase carries an exit criterion and says how it is verified,
    [Decisions](#decisions) lists the open questions, each with a recommendation and its
    cost, and [What cannot be verified](#what-cannot-be-verified-and-why) says where the plan
    stops. The central question — which member this belongs to — is decision 1; the name, if it
    needs one, is decision 2.

A delivery QC report answers one question for someone who did not make the file: *does this
conform to what was ordered, and against which published document was that judged?* It is read
by a delivery operator, kept as a record, and attached to a hand-off. Its shape is a file, not a
terminal.

This project already measures nearly everything such a report contains. What it does not do is
**write one down**. `ac3cli qc` prints its findings to standard output and exits; `ac3gui`'s QC
dialog draws them in a window and closes. Neither produces a file anyone can attach, archive or
diff, and neither emits anything a delivery system could parse.

So this plan is mostly about a document format and a renderer, plus the four conformance checks
the measurement side is missing. It is not a plan to build a meter.

**It is an instrument rather than a node.** A parallel plan — `docs/family/topology.md`, on PR
#539 — frames the family's other wrappers as **sources** that produce an encoded stream and
**sinks** that turn one back into sound in a room, joined by a transport. A QC report is neither:
it measures a stream without producing or rendering one. That page reaches the same placement
this one recommends, by a different route, and its own table records the reporter as "an
instrument, not a node".

## What already measures this

Enough that the gap is narrower than this page's brief assumed. Every row below is shipped and
tested today.

| Capability | Where | State |
|---|---|---|
| Integrated loudness (BS.1770-4 gated), momentary, short-term | `ac3/meta/loudness.hpp:128,136,140` (`LoudnessMeter`) | shipped |
| Loudness range (EBU Tech 3342) | `loudness.hpp:150` | shipped |
| True peak (BS.1770-4 Annex 2, oversampled) | `loudness.hpp:159` | shipped |
| Rendered-layout loudness (BS.1770-5 Annex 3, per-position weights) | `loudness.hpp:73` `position_weight()`, and `LoudnessMeter`'s second constructor | shipped |
| Object re-render loudness (BS.1770-5 Annex 4) | `qc ... objects=71\|512\|514\|714`, `apps/cli/commands/analysis.cpp:539` | shipped |
| dialnorm derivation and consistency | `loudness.hpp:186` `dialnorm_from_lkfs()`; `analysis.cpp:705-717` | shipped |
| Five delivery presets with primary-source citations | `ac3/meta/qc.hpp` — `ebu-r128-s2`, `atsc-a85`, `atsc-a85-streaming`, `netflix`, `apple-music-atmos` | shipped |
| Band-vs-ceiling gate semantics | `qc.hpp` `QcLoudnessLimit`, `evaluate_qc_gate()` | shipped |
| **An exit code a CI job can gate on** | `apps/cli/exit_codes.hpp:63` `kExitQcGate = 6`, returned at `analysis.cpp:1055` | **shipped** |
| The same gates through the C API | `ac3forge_qc_preset`, `ac3forge_evaluate_qc_gate` (`src/capi/src/qc.cpp`) | shipped |
| The same gates in a window | `apps/gui/qc_controller.cpp`, `qml/QcDialog.qml`, `qml/QcGateMeter.qml` | shipped |
| Declared stream facts — layout, substream map, OAMD counts, CRC integrity | `ac3cli probe`, and its `ac3forge.probe/1` JSON document | shipped |

The third of the brief's three candidate output forms — an exit code — therefore already exists,
and is already documented as distinguishing a failed gate from a failed read
(`apps/cli/usage.cpp:738-740`). Nothing in this plan changes it.

### Two headers that do not belong to this

[`ac3/quality/distortion.hpp`](../docs/library/quality.md) and `ac3/quality/perceptual.hpp` are named
in this page's brief as things a delivery report would build on. They answer a different
question and cannot be reached from a delivered file.

Both are **encoder in-loop** measurements. `distortion.hpp` computes the reconstruction error a
decoder will produce from three things only the encoder holds at the moment of choosing: the
pre-quantisation MDCT coefficients, the decoded exponents, and the bit allocation pointers it
has just derived. Its own header says what it is for — comparing two candidate parameter sets
inside the frame loop. `perceptual.hpp` builds the masking curve those candidates are scored
against. Given a finished `.ec3` file, the coefficients that were quantised are gone; what
survives is the quantised result, and the error is no longer recoverable from it.

`docs/library/quality.md:8` also records that nothing in that header set is on by default —
`EncoderConfig::search` and its E-AC-3 counterpart are opt-in. A report over a file this project
did not encode could not know whether they had been used.

They stay where they are, and this plan cites them once, here, to say why they are absent from
the report. The perceptual gates a delivery report *can* carry are the loudness and true-peak
ones above, plus the conformance checks below.

## What is missing

Five things, in the order they matter.

1. **No report artefact.** Nothing writes a file. `report_qc_programme()`
   (`analysis.cpp:680-781`) is a hundred lines of `fmt::println` straight to stdout, and
   `QcController` renders into QML properties. Neither can be redirected into a document without
   re-deriving it.

2. **No machine-readable form.** `probe` has `json=1` and a versioned `ac3forge.probe/1`
   contract (`docs/cli/commands.md:478-492`); `qc` has neither.

3. **The measurement result has no library type, and is duplicated.** `QcResult` and
   `QcProgrammeResult` (`analysis.cpp:54-89`) and `qc_detail::RawResult` and
   `qc_detail::RawProgramme` (`apps/gui/qc_controller.hpp:33-49`) are the same six and seven
   fields written twice. So are the measurement drivers: `qc_controller.hpp:16-18` says its
   helpers are "an anonymous namespace mirroring ac3cli's own
   `measure_qc_ac3`/`measure_qc_eac3`". Any third surface — a report writer — would be the third
   copy.

4. **Conformance is reported but never gated.** `probe` knows the rendered layout, the substream
   map, the OAMD object counts and bed mask, and the CRC/parse integrity counts. `qc` gates
   loudness and true peak alone. Nothing compares a stream against what was *ordered*: "this
   deliverable is 5.1.4", "this deliverable carries 118 objects over a 5.1.4 bed", "this
   deliverable is 48 kHz". A delivery rejection is more often a wrong layout than a wrong
   loudness.

5. **No dialogue gate.** `qc.hpp`'s own opening note records the consequence: EBU R 128 s4's
   Loudness-to-Dialogue Ratio cannot be evaluated at all, and the two Netflix presets read
   conservatively on dialogue-led material because `LoudnessMeter` measures programme loudness.
   This plan does not close that gap; it inherits it and says so in every report that cites those
   presets.

## Where it belongs

The central question. Four options, argued from the boundaries the build, packaging and docs
already draw.

### A — a new output mode on `ac3cli qc`, with the result model promoted into the library

Recommended. `qc` already takes the file, decodes it, measures it and gates it. This adds
`report=<path>` and `json=1` to the verb, and moves `QcResult` into the library as
`ac3::meta::QcReport` so the CLI, the GUI and the writers share one type instead of three.

The build already draws this boundary. `ac3::meta` is where `qc.hpp`, `loudness.hpp` and
`drc.hpp` live and is installed with the library; `apps/cli` is where the rendering of those
numbers into text already happens; `AC3FORGE_BUILD_CLI` already gates it. No new target, no new
CPack component, no new registry identifier, no new `.desktop`, no new `.ts` catalogue, no new
CI leg.

Cost: `ac3cli` gains two options on one of its forty-one verbs, and Forge's coverage floor
(`apps/cli 40 34`) has to survive a new renderer that ctest can exercise from a fixture. The
report writers live in `apps/cli/` and are reachable from the library's own consumers only
through the C API, which is where option A's limit lies — a Python caller gets the model but
renders the document itself.

### B — a page in `ac3gui`

The GUI's QC dialog is the natural place for a person to *look* at this, and it already exists.
But a delivery report is generated by a job as often as by a person, and `ac3gui` is absent from
several of the eleven CI legs and from every headless context. Making the GUI the home would put
the artefact behind Qt, behind a display, and behind `AC3FORGE_BUILD_GUI`.

This is a consumer of A rather than an alternative to it: under A, the dialog gains a **Save
report…** button that writes the same two documents. Recommended *as well*, in Phase 4.

Cost on its own: the report becomes unavailable in CI, which is where a delivery gate runs.

### C — its own named member, with its own binary

A fourth member: its own target, `ac3::<name>` namespace, `AC3FORGE_BUILD_<NAME>` option, CPack
component, DEB/RPM/archive tokens, winget identifier, Homebrew formula, `.desktop` and AppStream
entries, bundle id, icon, six `.ts` catalogues plus the `xx` pseudo-locale, a notices fragment
set, a docs tab, and a row in every table on
[the recasting page](recasting.md#what-each-member-owns) and in
[docs/index.md](../docs/index.md)'s three-members section.

[The recasting plan ruled a fourth member out of its own scope](recasting.md#deliberately-not-in-scope),
so this option has to argue past that, and the argument it would have to make is about
**audience and reach** rather than code volume. A delivery QC tool serves a delivery operator
rather than a codec engineer, and the tool such an operator wants runs over the formats they are
handed — which today would include PCM masters, and tomorrow formats this project does not
implement. A member whose scope is "audio deliverables" rather than "AC-3 and E-AC-3
deliverables" is a different product with a different scope, and it would deserve its own name.

Nothing in the tree points that way yet. The measurement is AC-3/E-AC-3-shaped end to end: the
presets are gated against a decoded elementary stream, the layout vocabulary is
`ac3::plan::LayoutId`, the object path is OAMD. Cost of taking C now: every identity above frozen
around a scope that is still one codec family's, plus [DR6](#signing-and-install) signing for a
third application before the two that exist are signed.

**External support, arrived at independently.** `docs/family/topology.md` classifies every other
wrapper in the family as a source or a sink and the QC reporter as neither, and concludes it
belongs under Forge for that reason. It was written from the family's transport gap rather than
from this page's build-boundary argument, so the two are independent readings that agree.

**When C becomes right:** when the report is asked to cover a format the library does not encode.
That is the trigger to re-open this, and it is worth writing down rather than rediscovering.

### D — a third Forge binary, `ac3qc`, sharing Forge's packaging

Between A and C: a separate executable so a delivery job installs one small thing, but inside
Forge's existing `runtime` component, archive, DEB/RPM and winget entry, so no new published
identifier appears anywhere.

Cost: a third binary in `runtime` that duplicates `ac3cli`'s argument parsing, its
`read_elementary_stream`, its programme selection and its exit-code table —
`apps/cli/support.cpp` and `apps/cli/exit_codes.hpp` are shared today because there is one
binary. It buys a shorter command name and costs a second copy of the CLI's front end.
`ac3cli qc` is already the command.

**Recommendation: A, with B in Phase 4.** It is the boundary the build already draws, it needs no
decision from any registry, and it leaves C available — promoting `QcReport` into `ac3::meta` is
the same first step under all four options, so nothing in Phases 1–3 has to be undone if the
answer later becomes C.

## The name

Under the recommendation there is **no new product name**, and one new identifier: the JSON
schema string, which follows `probe`'s convention and is `ac3forge.qc/1`.

If decision 1 takes option C or D, a name is needed. The family's names are metalworking terms —
Forge, Crucible — and the metalworking term for testing metal for purity and certifying the
result is *assay*, with *hallmark* being the mark struck on the metal once it passes. Both
describe this application precisely: one is the test, the other is the certificate.

| Option | Prose / binary / namespace / option / component | For | Cost |
|---|---|---|---|
| **Assay**, recommended if a name is needed | AC3Forge Assay · `ac3assay` · `ac3::assay` · `AC3FORGE_BUILD_ASSAY` · `assay` | The assay is the test, which is what the tool performs. Short, and unambiguous beside Forge and Crucible | The bare word is taken on PyPI and npm (below), so a bare-token package scheme is unavailable; `ac3assay` is free everywhere checked |
| **Hallmark** | AC3Forge Hallmark · `ac3hallmark` · `ac3::hallmark` · `AC3FORGE_BUILD_HALLMARK` · `hallmark` | The hallmark is the certificate, which is what the tool *emits* — closer to the artefact than to the act | Longer in every identifier; the bare word is taken on PyPI and npm, and npm's holder is an actively maintained tool (a markdown linter, v5.0.2), so the word is in current use in developer tooling |
| **Proof** | AC3Forge Proof · `ac3proof` · `ac3::proof` | Proof house, proving — the same trade vocabulary | Collides with proofreading, mathematical proof and proof-of-concept in every search; the least distinctive of the three |
| **No name** (options A/B) | none; `ac3cli qc`, schema `ac3forge.qc/1` | Nothing new is published, so nothing new is frozen | The capability has no name to point at in a release note; the docs say "`ac3cli qc`'s report" |

Spelling follows [the settled rule](recasting.md#the-name): capitalised in prose,
lowercase `ac3forge`-prefixed in identifiers, and the family named beneath the member.

### Availability, checked by hand on 2026-09-07

Checked because [the recasting plan's S2 row](recasting.md#the-name) records that a
bare generic token needs a check in seven namespaces before it can be taken, and because a
published name is frozen the moment anyone installs it.

| Namespace | `assay` | `hallmark` | `ac3assay` |
|---|---|---|---|
| PyPI | **taken** — `assay` 0.0, "Future testing framework" | **taken** — `hallmark` 0.2.0 | free (404) |
| npm | **taken** — `assay` 1.0.0, "make assertion functions into boolean tests" | **taken** — `hallmark` 5.0.2, "Markdown Style Guide, with linter and automatic fixer" | free |
| Homebrew (formula and cask) | free — 404 on both `formulae.brew.sh` API endpoints | free — 404 on both | free |
| Debian (sid) | free — "No such package" | free — "No such package" | free |
| Fedora (F43–45, EPEL, Rawhide) | free — no exact match | free — no results | free |
| winget Moniker | free — code search over `microsoft/winget-pkgs` returns 0 | free — 0 | free |

Two readings. The PyPI and npm collisions matter only for a route this plan does not propose —
neither a Python binding nor an npm package is part of it, and the library's own PyPI project is
`ac3forge`. The registries that would actually carry a member package (Homebrew, Debian, Fedora,
winget) are clear for both words. The `ac3forge`-prefixed forms are free everywhere, which is
what [decision 5 of the recasting plan](recasting.md#decisions) already settled for
every other member's package tokens.

Method and its limit: HTTP status and body checks against each registry's own API, plus a GitHub
code search for the winget Moniker. A code search indexes rather than enumerates, so a zero there
is weaker evidence than the four 404s; a winget Moniker is in any case advisory, while
`iainchesworthlabs.<name>` is the identifier that must be unique and is in a namespace this
project owns. **The name is not settled here.** This table is the evidence, not the choice.

## Scope

### What it does

- Decodes a delivered `.ac3`/`.ec3` (and, through the existing container paths, `.mkv`/`.mp4`/`.ts`)
  and measures it exactly as `ac3cli qc` does today, through the same `LoudnessMeter`.
- Reports **loudness** (integrated, LRA), **true peak**, and which BS.1770 algorithm produced
  them — `bed` (Annex 1 over the Table 5.8 bed), `rendered` (Annex 3 by channel position) or
  `objects=<layout>` (Annex 4, objects re-rendered by their own OAMD position). A figure without
  its algorithm is ambiguous, and `run_qc` already says so at `analysis.cpp:1022-1025`.
- Reports **dialnorm**: the transmitted value, the level it claims, the measured-minus-claimed
  delta, and the dialnorm the measurement itself implies — and whether `compr` is present.
- Gates against any of the five named presets, or all of them, each printed with the document
  version and date it was read out of (`QcPreset::source`).
- **New:** gates **conformance against what was ordered** — layout, sample rate, codec, object
  count, bed configuration, and stream integrity (see below).
- Writes two documents: an **`ac3forge.qc/1` JSON** document and a **single-file HTML report**.
- Exits `0`, or `6` (`kExitQcGate`) when any gate fails, or `2` when the file could not be read.
  Unchanged.

### The four conformance checks

Each compares a declared fact against an expectation the caller supplies. All four facts are
already computed; none is currently compared.

| Check | Expectation | Source of the measured fact |
|---|---|---|
| Layout | `expect-layout=51\|71\|512\|514\|714\|stereo\|mono` | the rendered program's channel order, every dependent's `chanmap` unioned in (§E3.8.2) — what `probe` reports as `rendered_channels`/`layout` |
| Sample rate and codec | `expect-rate=48000`, `expect-codec=eac3` | `bsid`, `fscod`/`fscod2` |
| Objects | `expect-objects=<n>`, `expect-bed=<layout>` | the OAMD program description `probe` already reads: `total`, `dynamic`, `bed`, `bed_mask`, `lfe` |
| Integrity | on by default | `crc_valid`, `crc_failures`, `parse_failures` from the same walk |

A mismatch fails the gate and returns `kExitQcGate`, the same as a loudness failure — from a
delivery operator's position both are "this file is not what was ordered".

### What it does not do

- **It does not measure encoder-internal quality.** `ac3/quality/distortion.hpp` and
  `perceptual.hpp` are not reachable from a delivered file, for the reason given
  [above](#two-headers-that-do-not-belong-to-this).
- **It does not gate a dialogue-gated loudness.** `LoudnessMeter` has no dialogue gate; EBU R 128
  s4's LDR clause stays unevaluable, and the report states the conservatism on the two
  Netflix-sourced presets rather than hiding it.
- **It does not add a preset.** New presets are IO11's shape and need a primary document someone
  has read; `qc.hpp` already records why EBU R 128 s4, Netflix's Atmos Home Mix v2.3 and Amazon
  are deliberately absent.
- **It does not compare against a source.** There is no reference file, no PEAQ, no ViSQOL. Those
  are [`docs/verification.md`](../docs/verification.md)'s territory and answer "did the encoder do
  well", not "is this deliverable conformant".
- **It does not render PDF itself.** See [Third-party notices](#third-party-notices).
- **It does not sign the report.** An attestable QC record is a separate question, downstream of
  [DR6](#signing-and-install) and of `src/signing`.
- **It does not batch.** One file per invocation. A delivery of twelve reels is twelve
  invocations and a shell loop; a manifest format for a whole delivery is a later question and
  would change the schema's top level.
- **It does not read AC-4.** `src/ac4` is a TOC/presentation inspector with no decoder, so there
  is nothing to meter.

## What it builds on, by path

| Path | What is taken |
|---|---|
| `src/forge/include/ac3/meta/qc.hpp` | the five presets, `QcLoudnessLimit`, `evaluate_qc_gate()`, `QcVerdict`, `parse_qc_preset()` — unchanged |
| `src/forge/include/ac3/meta/loudness.hpp` | `LoudnessMeter` (both constructors), `position_weight()`, `dialnorm_from_lkfs()` — unchanged |
| `src/forge/include/ac3/encoder/plan.hpp:64-73` | `LayoutId` and `LayoutInfo` as the layout vocabulary the conformance check compares against |
| `apps/cli/commands/analysis.cpp:54-89` | `QcResult`/`QcProgrammeResult`, **promoted** into the library |
| `apps/cli/commands/analysis.cpp:109,244,408,539` | `measure_qc_ac3`, `measure_qc_eac3_bed`, `measure_qc_eac3_rendered`, `measure_qc_eac3_objects`, **promoted** |
| `apps/cli/commands/analysis.cpp:680-781` | `report_qc_programme()`, **split** into a model pass and a text renderer |
| `apps/cli/json.hpp`, `apps/cli/json.cpp` | `JsonWriter` — the streaming, dependency-free writer `probe` uses. Reused as it stands |
| `apps/cli/commands/probe.cpp` | the shape of a `json=1` verb, and the OAMD/integrity reads the conformance checks need |
| `apps/cli/exit_codes.hpp:63` | `kExitQcGate` — unchanged |
| `apps/gui/qc_controller.{hpp,cpp}`, `qml/QcDialog.qml`, `qml/QcGateMeter.qml` | the dialog that gains **Save report…**; its duplicate model and helpers deleted in favour of the promoted ones |
| `docs/cli/commands.md:478-492` | the JSON versioning contract `ac3forge.qc/1` copies verbatim |
| `docs/gui/qc.md` | the dialog's documented behaviour, extended by one button |
| `docs/verification.md` | where the report's claims are cross-checked; the ffmpeg `ebur128` cross-check IO10 established is the loudness figures' oracle |
| `tools/checks/verify_gold_reference.sh` | the pattern for a CI check that runs a built `ac3cli` against a checked-in fixture and exits non-zero on the first failure |

### What the library is missing

Six additions, all in `ac3::meta`, all needed by any of the four placement options.

1. **`QcReport`** — the measurement result as a library type: codec, layout label, sample rate,
   unit count, duration, which BS.1770 algorithm ran, whether dependents were excluded, and a
   vector of per-programme results. Replaces the two duplicate structs. The topology plan
   sharpens the case: the sinks it describes — a playback appliance, an ESP32-S3 node, the WASM
   decode page — would each want to report what they received, so a library-level type gains a
   third and fourth *caller* where the present shape would grow a third and fourth *copy*.
2. **`QcExpectations`** — the ordered facts the conformance checks compare against, and
   `evaluate_qc_conformance()` beside `evaluate_qc_gate()`, in the same shape: a small struct in,
   a verdict out, no I/O.
3. **A measurement entry point** — `measure_qc()` over a stream span, so the CLI, the GUI and any
   binding call one function instead of mirroring four. This is where the CLI's four
   `measure_qc_*` helpers land.
4. **Stream facts on the report** — the OAMD counts and CRC/parse integrity that `probe` reads
   today inside `apps/cli`, so a QC report needs neither a second walk of the file nor a shell
   pipeline through `probe`.
5. **A loudness time series** — `LoudnessMeter` exposes `momentary_lkfs()` and
   `short_term_lkfs()` as instantaneous values (`loudness.hpp:136,140`), with nothing retained.
   An HTML report with a loudness-over-time chart needs the series. Either the caller samples on
   a cadence it chooses, or the meter retains one behind a config flag; the second is cheaper to
   get right once and is what this plan proposes, off by default so the existing memory behaviour
   is unchanged.
6. **Per-channel true peak** — `true_peak_dbtp()` returns the maximum over all channels
   (`loudness.hpp:159`). A report that says *which* channel clipped is more useful to whoever has
   to fix it, and `push_true_peak(int channel, float)` already carries the channel index.

Items 1–4 are the plan. Items 5 and 6 are what the HTML report wants and can be deferred to
Phase 5 without changing the schema, because both add members and
[the versioning rule](#ac3forgeqc1-the-machine-readable-form) permits that within a version.

## The two documents

### `ac3forge.qc/1` — the machine-readable form

Emitted by `json=1`, to stdout or to `report=<path>.json`. It adopts `ac3forge.probe/1`'s
contract without amendment, because a consumer that already parses one should not have to learn a
second set of rules:

- The top-level `schema` member names the contract. Within a version, members are only ever
  **added**; an existing member never changes type, units or meaning and never disappears.
- A member that does not apply is present and `null`/`false`/`[]`, **never omitted** — a consumer
  must not have to tell "no such key" from "no such thing".
- Units are stated in the member name (`_lkfs`, `_dbtp`, `_lu`, `_db`, `_hz`).
- A non-finite value is written `null`; `JsonWriter::value(double, int)` already does this.

Top level: `schema`, `generator` (the `ac3cli` version), `file`, `generated_at`, `stream`,
`programmes`, `conformance`, `gates`, `verdict`.

Two shape decisions worth writing down now, because changing either later costs a version:

- **`gates` is an array, one entry per preset evaluated**, even when one preset was requested.
  `preset=all` is the useful CI form and an array is the same shape for both.
- **`verdict` is the top-level boolean the exit code is derived from**, so a consumer never has to
  re-implement the AND across programmes and gates that `run_qc` performs at
  `analysis.cpp:1055`.

**Determinism.** Two runs over the same input must differ only in `generated_at`. That field
honours `SOURCE_DATE_EPOCH` when set, so a report can be checked into a delivery record and
diffed against a re-run. The test for this is a byte-comparison of two runs with the variable
pinned.

### The HTML report — the human-readable form

Emitted by `report=<path>.html`. One file, self-contained: inline CSS, inline SVG for the meters
and the loudness-over-time chart, no external stylesheet, no font download, no script from
anywhere. A delivery report is emailed and opened offline, often from a mail client's own sandbox,
and a report that renders differently depending on network access is not a record.

It carries what the terminal form carries, plus what a page can do that a terminal cannot: the
tolerance band drawn around each preset's target and the ceiling line at its true-peak limit, the
same shape `QcGateMeter.qml` already draws in the window.

Two properties it must have:

- **It prints.** `@media print` rules paginate it, because "print to PDF" in any browser is how
  the PDF in this page's brief gets made. See [Third-party notices](#third-party-notices) for why
  no PDF library is proposed.
- **It states its own limits.** Every figure names the algorithm that produced it, every preset
  names the document version and date from `QcPreset::source`, and where a preset is gated against
  programme loudness while its source says dialogue-gated, the report says so on the row. A QC
  report that overstates its own authority is worse than none.

## Build identity

Under the recommendation, one new source file pair in the library and two in the CLI. No new
target, option, component or namespace.

| | Value | Convention followed |
|---|---|---|
| Library headers | `ac3/meta/qc_report.hpp`, `src/forge/src/meta/qc_report.cpp` | beside `qc.hpp`/`qc.cpp`, in the existing `src/forge` target and its `meta` group |
| Namespace | `ac3::meta` | the namespace `qc.hpp`, `loudness.hpp` and `drc.hpp` already share |
| Export macro | `AC3FORGE_EXPORT` on every out-of-line entry point | `qc.hpp`'s own `evaluate_qc_gate` |
| CLI sources | `apps/cli/commands/qc_report_json.cpp`, `qc_report_html.cpp` | one file per renderer, beside `commands/analysis.cpp` |
| CMake option | **none** | the report is part of `qc`, which is part of `ac3cli`, which is `AC3FORGE_BUILD_CLI` (`CMakeLists.txt:136`) |
| CPack component | **none new** — `runtime` | `cmake/Packaging.cmake:454`'s `CPACK_COMPONENTS_ALL runtime library libruntime`; `ac3cli` is already in `runtime` |
| C API | `ac3forge_qc_report_*` beside the existing `ac3forge_qc_*` in `src/capi/src/qc.cpp` | the 215-symbol `ac3forge_` prefix; Phase 5, with an ABI allowlist refresh |
| Installed headers | `qc_report.hpp` follows `cmake/InstallLibrary.cmake`'s existing `ac3/meta/` glob | no new install rule |

If decision 1 takes option C instead, the identity becomes: target `ac3assay`, namespace
`ac3::assay`, option `AC3FORGE_BUILD_ASSAY` (default `OFF`, as `AC3FORGE_BUILD_CRUCIBLE` is at
`CMakeLists.txt:138`), component `assay`, packages `ac3forge-assay-<full>-<sys>` and DEB/RPM
`ac3forge-assay` — the token shape `ac3forge-crucible` established at
`cmake/Packaging.cmake:174-182`.

## Tests

`ac3tests` is one binary (`tests/CMakeLists.txt:617`,
`catch_discover_tests(ac3tests ADD_TAGS_AS_LABELS)`), so ctest labels come from Catch2 tags rather
than from CMake. The existing QC tags are `[qc]` (`tests/meta/test_meta_qc.cpp`) and `[cli][qc]`
(`tests/cli/test_cli.cpp:2008`).

| Suite | File | Tags / label | What it pins |
|---|---|---|---|
| Library | `tests/meta/test_meta_qc.cpp` (extended) | `[qc]` | `QcExpectations` and `evaluate_qc_conformance()`: every mismatch fails, every match passes, and an expectation left unset gates nothing |
| Library | `tests/meta/test_qc_report.cpp` (new) | `[qc]` | `QcReport` round-trips every field the two renderers read; a report with no measurable loudness carries `std::nullopt` rather than a sentinel |
| CLI | `tests/cli/test_cli.cpp` (extended) | `[cli][qc]` | `json=1` emits a document that parses, carries `schema == "ac3forge.qc/1"`, and has **every** documented member present — including the null ones, which is the contract's own rule and the thing a renderer regression breaks first |
| CLI | `tests/cli/test_cli.cpp` | `[cli][qc]` | determinism: two runs with `SOURCE_DATE_EPOCH` pinned are byte-identical |
| CLI | `tests/cli/test_cli.cpp` | `[cli][qc]` | the exit code stays `kExitQcGate` for a failed conformance check, and `kExitInput` for an unreadable file |
| GUI | `apps/gui/tests/qml/tst_qc_panel.qml` (extended) | `gui` | **Save report…** writes both files to a temporary path, and the dialog reports the failure when the path is unwritable |

**Coverage.** `tools/checks/coverage_report.sh:118-127` holds `src/forge 88 78` and
`apps/cli 40 34` (line, branch). The library additions sit inside `src/forge` and must clear
88/78 like everything else there — a report model and two pure evaluation functions are
straightforwardly testable from a fixture, so this is a floor to meet rather than to move.

The CLI floor is the one to watch. That table's own comment explains why `apps/cli` sits at 40 —
roughly 15% of its lines are device paths that never execute headless. Two new renderer files are
fully exercisable from a fixture stream, so they should *raise* the measured number rather than
press on the floor. **Proposal: leave both floors untouched in Phases 1–4, and re-measure before
Phase 5.** Raising a floor on the strength of one PR's additions is how a floor becomes
unmeetable later; the number moves when the measurement has settled.

## CI

**No leg is added, renamed or reflagged, and no branch-protection contract is touched.**
`CI Status`, `Branch Name` and `Scan dependency diff` are the three required checks
(`.github/branch-protection.md:27-33`), and `CI Status` aggregates by `needs` precisely so a
matrix leg can be added without editing the rule. Nothing here needs that.

| Leg | What it exercises | Flags |
|---|---|---|
| windows-msvc, windows-llvm | library + CLI, both renderers | unchanged |
| windows-msvc-arm64 | library + CLI (no GUI on this leg) | unchanged |
| linux-gcc, linux-gcc-arm64 | library + CLI + the GUI's Save button | unchanged |
| linux-llvm, linux-llvm-arm64 | the same, plus Crucible's pass | unchanged |
| linux-llvm-asan-ubsan, linux-llvm-tsan | the library additions under sanitisers — the renderers walk a model and format strings, which is where ASan finds things | unchanged |
| macos-llvm, macos-llvm-x64 | library + CLI + GUI | unchanged |
| coverage | the two floors above | unchanged |
| abi-gate | the C API additions, **Phase 5 only** | an allowlist refresh with that phase |

**Cost in matrix time: none added.** Every leg already builds `src/forge` and `ac3cli`; this adds
source files to targets that are already compiled everywhere. What it does cost is the docs-only
fast path: `ci.yml:314` classifies a PR as docs-only by a regex over `docs/`, `*.md`, `mkdocs.yml`
and `docs.yml`, so **this page** rides the fast path and every subsequent phase pays for all
eleven legs — which is the standing cost of any code change here rather than a new one.

One new check is worth considering and is **not** proposed for Phases 1–4: a CI step that runs the
built `ac3cli qc ... json=1` over a checked-in fixture and validates the document against its own
documented member list, in the shape `tools/checks/verify_gold_reference.sh` uses. It would catch
a renderer that silently drops a member, which is the failure the JSON contract is most exposed
to. It belongs after the schema has been stable for a release, and it is decision 6.

## Packaging and release identity

Under the recommendation, **nothing new is published and no token is frozen.** `ac3cli` is
already in the `runtime` component and already ships in the `ac3forge-<M.m.p>-<sys>` archives, the
NSIS installer, the DEB and RPM named `ac3forge`, the `.dmg`, the winget entry
`iainchesworthlabs.ac3forge` and the Homebrew formula `ac3forge`. A new verb option changes none
of them, and the release asset route is untouched: `release.yml` collects by the `packages-*`
artifact prefix (`release.yml:275-279`), and no new artifact appears.

The one packaged thing that changes: `apps/cli`'s shell completions and its man page list the `qc`
verb's options, so `report=` and `json=` join four completion files and `ac3cli.1`. Both are
installed under `runtime` already.

If decision 1 takes option C, the identity to reserve — and the point at which **a published name
is frozen, because a package anyone has installed cannot be renamed without a
`Replaces`/`Conflicts`/`Provides` path and a new winget identifier** — is:

| Identity | Value under option C | Frozen when |
|---|---|---|
| Component | `assay` | the first tag containing it |
| Archives | `ac3forge-assay-<full>-<sys>` zip/tgz | the first release asset |
| DEB / RPM | `ac3forge-assay` | first install |
| winget | `iainchesworthlabs.ac3assay`, Moniker `ac3assay` | first upstream submission; staged version directories are never rewritten (`docs/releasing.md`) |
| Homebrew | formula `ac3forge-assay` in the live tap | first `brew install` |
| Artifact prefix | `packages-assay-<preset>` | must match `packages-*` or `release.yml` will not collect it |

That last row is the trap
[the recasting plan recorded](recasting.md#packaging-and-release-identities) and
Crucible hit: an artifact not named `packages-*` is built, uploaded, and silently absent from the
release.

## Documentation

Six pages change and one is added. Everything sits in the seven-tab nav
[the recasting plan settled](recasting.md#the-docs); no page moves, which matters
because `mkdocs.yml` declares no redirects plugin.

| Page | Change | Nav |
|---|---|---|
| **`docs/forge/qc-report.md`** | **this page** | new, under **Forge**, after "What it is" |
| `docs/cli/commands.md` | the `qc` row gains `report=`/`json=`; a new **`ac3forge.qc/1`** section beside the `ac3forge.probe/1` one, with the full member table | unchanged |
| `docs/cli/metadata-options.md` | `report=`, `json=`, and the five `expect-*` options in the `qc` option block | unchanged |
| `docs/gui/qc.md` | the **Save report…** button, what it writes, and where | unchanged |
| `docs/verification.md` | one paragraph under **Quality**: what the report asserts, what it does not, and that its loudness figures are the ones IO10 cross-checked against ffmpeg's `ebur128` on 5.1 | unchanged |
| `docs/library/quality.md` | one sentence saying the distortion and perceptual headers are encoder in-loop and are not what a delivery report measures — the confusion this plan had to resolve, written down once | unchanged |
| `docs/index.md` | the **Forge** paragraph of "What is here" gains "and writes a delivery QC report" to its `ac3cli` sentence | unchanged |

`README.md`: the Documentation table gains a row for this page, and the Forge row of the
three-member table gains the same clause as `docs/index.md`. Nothing else — the README's
capability claims defer to `docs/index.md` and `docs/verification.md` by
`CONTRIBUTING.md:261-266`, and this plan does not change what those two say the library can do.

Under option C, add: an **Assay** tab with an index, install and report-format pages, and a row in
every member table on the recasting page and in `docs/index.md`'s three-members section — which
would become four.

## Localisation and accessibility

Under the recommendation this reaches one surface: the GUI's **Save report…** button and its error
states.

- **Catalogues.** `apps/gui/translations/` holds seven `.ts` files — `ar`, `de`, `es`, `fr`, `he`,
  `yi` and the `xx` pseudo-locale. Every new string is `qsTr()`-wrapped and `lupdate` refreshes all
  seven. `QcDialog.qml` already wraps every string it draws.
- **RTL.** `ar` and `he` are right-to-left; the button joins a dialog already laid out for both, so
  this inherits rather than adds.
- **Accessibility.** `Accessible.role`, `.name` and `.description` on the new button and on any new
  custom control, matching what `QcDialog.qml` and `QcGateMeter.qml` already carry.
  [`docs/gui/accessibility.md`](../docs/gui/accessibility.md) and
  [`docs/gui/localisation.md`](../docs/gui/localisation.md) are the pages that record this.
- **The report itself is not localised.** It is a delivery document quoting ATSC, EBU, Netflix and
  Apple specifications by their English titles and clause numbers, sent to a recipient who may not
  share the operator's locale. Its `<html lang="en">` says so. Translating the chrome around
  untranslated citations would make the document harder to check rather than easier. Worth stating
  rather than leaving as an oversight; it is decision 4.

Under option C, a new application needs its own six catalogues plus `xx`, its own RTL pass and its
own accessibility audit. Note for whoever takes that on: `apps/crucible/translations/` has `ar`,
`de`, `es`, `fr`, `he`, `yi` and **no `xx`**, so the pseudo-locale is a GUI-only practice today
rather than a family one.

## Identity assets

Under the recommendation: **none needed.** No new binary, so no icon, no `.desktop`, no AppStream
entry, no bundle id. The report is a file `ac3cli` writes.

Under option C, all four are needed, and one of them lands on a gap that already exists.

**The family-wide icon gap, recorded and not solved here.**
`apps/crucible/CMakeLists.txt:659-664` installs Forge's icon under Crucible's name:

```cmake
install(FILES "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-256.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
    RENAME "ac3crucible.png" COMPONENT crucible)
```

The same two files are also the window icon at `apps/crucible/CMakeLists.txt:456-457`, so the
substitution is not only in the install rules. **No member has a mark of its own**, and a third
application would inherit the same borrowed one. Drawing three marks is a design task with no
dependency on this plan and no reason to block it; it is worth doing before any member is
submitted to a store or a distribution that displays an icon.

Bundle id under option C would be `com.iainchesworthlabs.ac3assay`, following
[decision 12 of the recasting plan](recasting.md#decisions) and the existing
`com.iainchesworthlabs.ac3gui`. Crucible has no bundle id yet, so this would be the second
outstanding one rather than the first.

## Third-party notices

`apps/notices/` composes `NOTICES.txt` per platform from fragments — `header.txt`, `qt-linux.txt`,
`qt-macos.txt`, `qt-windows.txt`, `windows-runtime.txt` — selected by
`platform/<os>/components.cmake`. **Any new dependency needs a fragment**, and a fragment is a
licence text plus an attribution rather than a line in a list.

**This plan proposes no new dependency.** That is a design constraint, and the PDF question is
where it bites, exactly as this page's brief anticipated.

| PDF approach | Dependency | Notices | Licence under GPL-3.0 | Verdict |
|---|---|---|---|---|
| **HTML + the browser's own print-to-PDF** | none | none | n/a | **Recommended.** `@media print` rules in the report; the operator's browser makes the PDF. Zero new anything |
| Qt's `QPdfWriter` | Qt (already present) | already covered by the three `qt-*.txt` fragments | LGPL-3, compatible | Rejected: it would put Qt into `ac3cli`, which has no GUI dependency today and builds on `windows-msvc-arm64` where `ac3gui` does not |
| libharu | new | a new fragment | zlib/libpng — permissive, GPL-3-compatible | Rejected: a C PDF library, a new vcpkg port, a new fragment, and a second layout engine to maintain beside the HTML one |
| PoDoFo | new | a new fragment | **LGPL-2.1** — needs care: LGPL-2.1-**only** is incompatible with GPL-3; only the `or later` form can be taken to LGPL-3 and combined | Rejected on the dependency cost before the licence question is reached |

The JSON renderer reuses `apps/cli/json.hpp`, which is in-tree and has no dependency. So the
notices tree is untouched by this plan in full.

## Licensing

The project is **GPL-3.0** (`LICENSE`), and every application member links the library and ships
under it.

**What that permits for a dependency.** GPL-3-compatible licences only: MIT, BSD-2/3, Apache-2.0,
zlib, ISC, LGPL-2.1-**or-later**, LGPL-3, MPL-2.0, and GPL-3 itself. `{fmt}` (MIT) and the SIL OFL
typefaces already in the tree are examples.

**What it forbids.** GPL-2-**only** (incompatible with GPL-3 in the other direction),
LGPL-2.1-**only**, any CDDL or EPL component linked into the same binary, anything
source-available-but-not-free, and any licence with a field-of-use or non-commercial restriction —
GPL-3 §7 permits no added restriction of that kind. This is why the PDF table above stops at
PoDoFo's `2.1` rather than treating LGPL as uniformly safe.

**Per distribution channel.** All are already carrying GPL-3 binaries; this adds no new obligation
to any of them, and the corresponding source is the public repository at the tagged commit in
every case.

| Channel | Obligation | Met by |
|---|---|---|
| GitHub release archives, DEB, RPM, `.dmg`, NSIS, AppImage | GPL-3 §4/§6 — licence text with the binary, source offer | `apps/notices/` installs `LICENSE.txt` and `NOTICES.txt`; the source is the tagged public repository |
| Homebrew formula and cask | the formula declares the licence | already `GPL-3.0` |
| winget | the manifest declares `License` and `LicenseUrl` | already in the staged manifests |
| PyPI (`ac3forge`) | wheel metadata | unchanged — no Python surface in this plan |
| Debian / Fedora, if ever submitted | machine-readable `debian/copyright`; every dependency's licence declared | the no-new-dependency constraint above is what keeps this cheap |

The report's own output is data the operator produces about their own file, and carries no licence
claim from this project. The HTML report should say so in one line rather than leaving a reader to
wonder whether an attached QC record is encumbered.

## Signing and install

**Code signing** — Developer ID and notarisation for macOS, Authenticode for Windows — is blocked
on certificates rather than on code, and is a Known gap in every release since 0.8.0-beta.2. It
gates every application member, and this plan's dependency on it differs sharply by option.

**Under the recommendation (A/B): nothing new is required.** `ac3cli` and `ac3gui` are the binaries
that ship, they are the ones DR6 already covers, and a new verb option does not change their
signing status. The report writes a `.html` and a `.json` — neither is executable, neither triggers
Gatekeeper, SmartScreen or the macOS quarantine prompt, and neither needs to be signed to be
opened by the recipient.

One consequence is worth naming: **an unsigned QC report is evidence only to someone who trusts
the sender.** A broadcaster that wanted to verify a report had not been edited would need it
signed, and this project has the machinery — `src/signing` and the release GPG key. That is a
separate question from DR6, it is out of this plan's scope, and it is decision 7.

**Under option C:** a third signed application, which means a third Developer ID-signed and
notarised `.app` and a third Authenticode-signed Windows binary and installer, and DR6 blocks the
release the same way it blocks the other two. Adding a third unsigned application before the first
two are signed increases the surface of the same unresolved problem, which is a reason to prefer A
now and revisit C after DR6 lands.

## Phases

Each ends with something checkable and says how it is checked. Phase 1 is this page and rides the
docs-only fast path; every later phase pays for all eleven legs.

### Phase 1 — this plan

Write this page, add it to the `mkdocs.yml` nav under **Forge**, and open a PR.

**Exit criterion:** `mkdocs build --strict` is green and the page is in the nav.
**Verified by:** `python -m mkdocs build --strict` locally and the `docs` job in CI.

### Phase 2 — promote the model into the library

`ac3::meta::QcReport`, `QcExpectations`, `evaluate_qc_conformance()` and `measure_qc()` in
`ac3/meta/qc_report.hpp`. Delete the CLI's `QcResult`/`QcProgrammeResult` and the GUI's
`qc_detail::RawResult`/`RawProgramme`, and point both surfaces at the library type. No behaviour
change anywhere.

**Exit criterion:** the CLI's `qc` output is byte-identical before and after, the GUI's QC dialog
behaves identically, and the two duplicate struct definitions and the GUI's mirrored measurement
helpers are gone from the tree.
**Verified by:** a golden-output comparison of `ac3cli qc` over the checked-in fixtures on the
branch and on `main`; the existing `[cli][qc]` and `gui` suites unchanged and passing; `grep` for
the deleted symbols returning nothing.

### Phase 3 — conformance gates

`expect-layout=`, `expect-rate=`, `expect-codec=`, `expect-objects=`, `expect-bed=`, and integrity
on by default. Wire the OAMD and CRC facts into `QcReport` so no second walk of the file is needed.

**Exit criterion:** each of the five expectations fails a stream that violates it and passes one
that satisfies it, and a mismatch returns `kExitQcGate` (6) rather than `kExitInput` (2).
**Verified by:** new `[cli][qc]` cases over fixtures of each layout; an explicit case asserting the
exit code for a conformance failure, distinct from the unreadable-file case.

### Phase 4 — the two documents

`json=1` emitting `ac3forge.qc/1`; `report=<path>` emitting the JSON or the single-file HTML by
extension; **Save report…** in the GUI dialog writing both.

**Exit criterion:** the JSON parses, carries every member the documentation lists including the
null ones, and two runs with `SOURCE_DATE_EPOCH` pinned are byte-identical. The HTML opens with no
network access and prints to a paginated PDF from a browser. The GUI button writes both files and
reports an unwritable path rather than failing silently.
**Verified by:** the `[cli][qc]` parse-and-member-presence test and the determinism test above; the
extended `tst_qc_panel.qml` case; and a manual open-offline-and-print pass on one browser per
platform, which is the part CI cannot do — see
[What cannot be verified](#what-cannot-be-verified-and-why).

### Phase 5 — the bindings and the chart

`ac3forge_qc_report_*` in the C API with an ABI allowlist refresh; the retained loudness series and
per-channel true peak from [the missing library pieces](#what-the-library-is-missing); the
loudness-over-time chart in the HTML report.

**Exit criterion:** `abi-gate` is green with the refreshed allowlist; the retained series is off by
default and the existing memory behaviour of `LoudnessMeter` is unchanged when it is off; the chart
renders from the series with no new dependency.
**Verified by:** `abi-gate`; a `[qc]` case asserting the meter's allocation behaviour with the flag
off; the coverage floors re-measured and, if they have moved, raised in a change of their own
rather than in this one.

### Later, outside this plan

A batch manifest for a whole delivery; a signed report; presets that need a document nobody has
read yet; and the icon work identity assets would need under option C.

## The proposed ROADMAP entry

`ROADMAP.md` is not edited by this plan, and no roadmap ID is allocated here. The natural theme is **IO**
("Streams in and out", member "the library, Forge"), whose last three items are IO10, IO11 and
IO12, all QC and loudness; IO13 is the next free number. Proposed text, in the theme's own format:

> **IO13 (M, Forge)** — Delivery QC reports — `ac3cli qc` writes a machine-readable
> `ac3forge.qc/1` document and a self-contained HTML report, and gates layout, sample rate, codec,
> object count and stream integrity against what was ordered.
>
> C2 gave `qc` its measurement, IO10 and IO11 its rendered-layout algorithm and its cited presets,
> IO12 its object re-render, and C3 the same in a window — but nothing wrote a file. The
> measurement result existed twice, as `apps/cli`'s `QcResult` and `apps/gui`'s
> `qc_detail::RawResult`, so a third surface would have been a third copy; it is now
> `ac3::meta::QcReport` and both surfaces read it. `QcExpectations` adds the conformance half the
> loudness gates never covered: a delivery is rejected for a wrong layout more often than for a
> wrong loudness, and the layout, sample rate, codec, OAMD object count and bed configuration were
> all already computed and never compared. The JSON document follows `ac3forge.probe/1`'s contract
> unchanged — add-only within a version, a member that does not apply present and null. The HTML
> report is one file with no external resource, because a delivery report is read offline; the PDF
> in the original ask is the browser's print, and the alternative — a PDF library, a vcpkg port, a
> notices fragment and a second layout engine — is recorded in `docs/forge/qc-report.md` with the
> licence check that ruled out PoDoFo's LGPL-2.1. `ac3/quality/distortion.hpp` and `perceptual.hpp`
> are deliberately absent: both are encoder in-loop measurements over coefficients a delivered file
> no longer carries. `kExitQcGate` is unchanged, and a conformance failure returns it.

If decision 1 takes option C, this becomes a **UX** item instead — UX is the Applications theme and
holds Forge and Crucible items — and a new member would need its own theme code the way Crucible
took `CR`.

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| The HTML report renders correctly in the mail clients and browsers a broadcaster uses | **no** | no access to those environments; the mitigation is the no-external-resource rule, which removes the largest class of difference |
| Print-to-PDF paginates acceptably on every browser | partly — one browser per platform, by hand | headless CI has no print pipeline; the `@media print` rules are checked by inspection and one manual pass per platform per release |
| A broadcaster or platform would accept this document as a QC record | **no** | no delivery relationship exists to test it against; the presets are cited from primary documents, and whether a recipient accepts the *format* is a commercial question |
| The five preset numbers still match their sources | yes, by re-reading each document | `tests/meta/test_meta_qc.cpp:24` pins the numbers against the citations, but a specification revision is outside the tree — this is how IO11 found the 2026-07 A/85 revision |
| The loudness figures agree with another meter | yes, on 5.1 | IO10 established the ffmpeg `ebur128` cross-check and recorded that it disagrees by design on 7.1 rear surrounds, because ffmpeg generalises Annex 1's Table 3 by channel name while BS.1770-5 Annex 3 weights by position. So ffmpeg is an oracle for the bed path and not for the rendered one |
| The dialogue-gated presets read correctly on dialogue-led material | **no** | `LoudnessMeter` has no dialogue gate; the report states the conservatism rather than correcting it |
| The winget Moniker is free | weakly | a GitHub code search indexes rather than enumerates; the identifier that must be unique is `iainchesworthlabs.<name>`, in a namespace this project owns |
| `assay` and `hallmark` are free on Homebrew, Debian and Fedora | yes, and checked 2026-09-07 | recorded [above](#availability-checked-by-hand-on-2026-09-07); a registry can change, so a re-check belongs immediately before any submission |

## Coordination

**Open pull requests.** `gh pr list` on 2026-09-07 shows five open, four of them Crucible engine
and threading fixes (#532, #534, #535, #536) and one Dependabot Actions bump (#531). None touches
`apps/cli`, `src/forge/*/meta`, `apps/gui/qc_controller.*` or `docs/`, so Phase 1 collides with
nothing. Phase 2 edits `apps/gui`, so it should re-check the queue first.

**The GUI and the CLI in one change.** Phase 2 deletes a struct from each and is one PR rather than
two: leaving either surface on its own copy for a release would make the duplication permanent in
exactly the way it became permanent the first time.

**`docs/family/topology.md`.** *Resolved 2026-09-07.* While neither page was on `main`, each
named the other by branch URL, because a relative link to a file not yet in `docs/` aborts
`mkdocs build --strict`. #537, #538 and #540 merged first, so #539 — the last of them — converted
every such link to a relative path in one change, on both sides. Nothing here is a branch URL any
more. The rule that produced it is worth keeping for the next time two doc PRs cross:
**whichever merges last converts all of them**, and `tools/checks/check_doc_paths.py` cannot
help, because it skips http(s) targets by design (its own docstring, line 13).

**The docs-only fast path.** This page touches only `docs/` and `mkdocs.yml`, so `ci.yml:314`
classifies it as docs-only, it runs the strict docs build and skips the matrix. Phases 2–5 do not,
and should not be batched with unrelated prose.

## Deliberately not in scope

- **A fourth member**, unless decision 1 takes option C. This plan recommends against it now and
  says [when it would become right](#c-its-own-named-member-with-its-own-binary).
- **Renaming anything.** No binary, package token, registry identifier, component or namespace
  changes.
- **A PDF library.** The HTML report prints; see [the table](#third-party-notices).
- **New presets.** IO11's shape and a primary document are the bar.
- **A dialogue gate for `LoudnessMeter`.** A gap, inherited and stated, not closed here.
- **Reference-based quality measurement.** No PEAQ, no ViSQOL, no source comparison — that is
  [`docs/verification.md`](../docs/verification.md)'s territory and a different question.
- **Encoder in-loop quality in the report.** `distortion.hpp` and `perceptual.hpp` cannot be
  reached from a delivered file.
- **Batch or manifest reporting.** One file per invocation; a delivery manifest would change the
  schema's top level and should be designed once, later.
- **Signing the report.** Downstream of DR6 and of `src/signing`.
- **AC-4.** `src/ac4` has no decoder, so there is nothing to meter.
- **Editing `CHANGELOG.md` or `ROADMAP.md`.** The roadmap entry is
  [proposed as text](#the-proposed-roadmap-entry).
- **Moving any page** under `docs/`.

## Decisions

The open questions. Each carries a recommendation and the cost of taking it.
**None of these is decided.**

1. **Which member this belongs to.** (a) a new output mode on `ac3cli qc` with the model promoted
   into the library; (b) the GUI as well, in Phase 4; (c) its own named member with its own binary;
   (d) a third Forge binary `ac3qc`. **Recommend (a) with (b)**: it is the boundary the build,
   install rules, packaging and docs already draw, and it needs no decision from any registry.
   Cost: the report renderers live in `apps/cli` and are reachable from other languages only
   through the C API added in Phase 5. Taking (c) later costs nothing already spent — Phase 2's
   promotion is the first step under all four. `docs/family/topology.md` reaches the same
   conclusion from the family's source/sink/transport frame, which is independent evidence rather
   than a second statement of this page's argument.

2. **The name, if (c) or (d).** (a) **Assay** — `ac3assay`, `ac3::assay`; (b) **Hallmark** —
   `ac3hallmark`; (c) **Proof**; (d) no name, under (a)/(b) of decision 1. **Recommend (d) now, and
   Assay if a name is ever needed.** Assay is the trade term for the test itself, sits beside Forge
   and Crucible without collision, and is free on Homebrew, Debian, Fedora and winget; the bare word
   is taken on PyPI and npm, which rules out a bare-token package scheme but not the
   `ac3forge`-prefixed one every other member already uses. Cost of taking a name now: every
   identity in [Packaging and release identity](#packaging-and-release-identity) is frozen at first
   install. **This is not settled here; the table above is evidence, not a choice.**

3. **The JSON schema identifier and its shape.** `ac3forge.qc/1`, following `ac3forge.probe/1`'s
   contract unchanged, with `gates` an array even for one preset and a top-level `verdict` boolean.
   **Recommend as stated.** Cost: both shape choices are frozen for the life of version 1 —
   changing either later is `ac3forge.qc/2`.

4. **Whether the HTML report is localised.** (a) English only, `lang="en"`, because it quotes ATSC,
   EBU, Netflix and Apple documents by their English titles and clause numbers; (b) the seven
   catalogues, with citations left untranslated. **Recommend (a).** Cost: an operator working in
   Arabic or Hebrew reads an English report; the GUI chrome around it is translated either way.

5. **The coverage floors.** (a) leave `src/forge 88 78` and `apps/cli 40 34` untouched through
   Phase 4 and re-measure before Phase 5; (b) raise `apps/cli` on the strength of the new renderers.
   **Recommend (a).** Cost: the floor understates the achieved number for a release. Raising it on
   one PR's additions is how a floor becomes unmeetable later.

6. **A CI check that validates the JSON document against its documented member list.** **Recommend
   yes, after the schema has been stable for one release**, in the shape
   `tools/checks/verify_gold_reference.sh` uses. Cost: one new script and one step on one leg; it
   catches the failure the add-only contract is most exposed to, which is a renderer silently
   dropping a member.

7. **Whether a QC report should be signable.** Out of scope here, and the machinery exists
   (`src/signing`, the release GPG key). **Recommend deferring** until someone asks for it. Cost: an
   unsigned report is evidence only to a recipient who trusts the sender.

8. **When option C is re-opened.** **Recommend the trigger be written down now**: when the report is
   asked to cover a format the library does not encode. Cost: none, and it saves the argument being
   had from scratch.
