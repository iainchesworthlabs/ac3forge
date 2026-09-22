# Programme mixing metadata (mixmdate, Table E1.2)

!!! note "Status as of 2026-09-22: plan, nothing built yet"
    Written 2026-09-22 after a full review of the current implementation. The headline finding:
    the wire format itself is **already correct and complete**, on both the encoder and the
    decoder, including every `mixdef` variant, pan and per-block mixing configuration. Nothing in
    this plan touches `ac3/meta/mixing.hpp`'s shape, `eac3_frame.cpp`'s encoder, or
    `eac3_decoder.cpp`'s reader — Phase 5 adds one new file that reads what the decoder already
    produces.

    What is missing is everything built *on top of* that correct core: CLI/JSON reporting stops
    partway through the struct, the C API and Python bindings expose none of it (already
    self-documented as a known gap in `docs/library/c-api.md`), the GUI exposes three fields out
    of roughly thirty, test coverage has specific, named holes, and — the one genuinely new
    feature here — nothing anywhere *uses* the decoded values to actually combine a main
    programme with an associated service's audio.

    This work was scoped against two sibling efforts running at the same time, coordinated
    directly session-to-session: **"EAC3 multi-program authoring implementation"** is generalizing
    `programme2=` to `programmeN=` (CLI authoring of *multiple* programmes) and **"EAC3 Annex E
    associated service semantics"** is container-level `mainid`/`asvc` identification in MPEG-TS
    (`dec3` descriptor). Neither touches decode-time audio combination; both confirmed that
    directly. This plan's Phase 5 is that piece.

## What already exists

| Layer | State | Evidence |
|---|---|---|
| Data model | Complete | `ac3::meta::MixMetadata`/`MixingParameters`/`ExternalScales`/`SpeechEnhancement`/`PanInfo` (`src/forge/include/ac3/meta/mixing.hpp:254-320`) model every field Table E1.2 defines. |
| Encoder | Complete; one bug found and fixed during Phase 2 | `eac3_frame.cpp:1267-1600` writes every field/variant, including the `strmtyp` gate (`!dependent`, i.e. `kIndependent` **or** `kConvertible` — deliberately, not `kDependent` alone; see below). Writing the Phase 2 `blkmixcfginfo`×`numblkscod==0x0` test (below) surfaced a real one: the encoder always wrote `blkmixcfginfo`'s six-block wire form regardless of `numblkscod`, while the decoder (and the independent Python reference parser, `tools/references/eac3_parse.py`) already correctly implemented §E2.3.1.60's one-block special case — so the two went out of sync whenever a caller combined `numblkscod=0` with `blkmixcfginfo`, and the encoder's own stream failed its own decoder. Fixed in the same phase; see the Phase 2 write-up. |
| Decoder | Complete, correct | `eac3_decoder.cpp:128-286` (`read_mixing_parameters`/`read_mixing_metadata`) reads the full group symmetrically, including `mixdef==3`'s length-prefixed skip-forward for sub-fields this build doesn't otherwise need. |
| CLI authoring (primary programme) | Complete, under-documented | `apps/cli/support.cpp:1136-1267` implements `pgmscl=`/`pgmscl2=`/`extpgmscl=`/`mixdef=`/`premixcmp=`/`mixdata=`/`extmix=`/`auxmix=`/`speechmix=`/`paninfo=`/`paninfo2=`/`blkmixcfg=` in full, documented in `docs/forge/cli/metadata-options.md:68-85,210-235` — but absent from `ac3cli --help` (`apps/cli/usage.cpp`). |
| Round-trip tests | Real, but incomplete | `tests/meta/test_bsi.cpp`'s "DC4" section (lines 435-767) round-trips most of the struct through real encode→decode. Specific gaps below. |
| Docs (C++ API) | Accurate | `docs/library/metadata.md:116-160` describes the full struct correctly, with a working example. |

**A correctness question worth recording as answered, not open**: the encoder writes the
programme-scaling/mixdef/pan/blkmixcfginfo group whenever `strmtyp != kDependent`, which includes
both `kIndependent` (0) and `kConvertible` (2, "independent, and previously coded as AC-3").
Several comments in the codebase shorthand this as "Table E1.2's `strmtyp == 0x0` gate," which
reads like it excludes `kConvertible`. It does not — `src/forge/src/emdf/frame_layout.cpp:158-163`
spells out why a third, independent bit-walker treats the two identically, and encoder/decoder
agree with it. No bug; the loose phrasing in other comments is the only imprecision.

## What's missing

| Gap | Where | Detail |
|---|---|---|
| CLI decode reporting stops early | `apps/cli/commands/decode.cpp:263-291` (`print_mix_summary`) | Prints `pgmscl`/`extpgmscl` in full, `mixdef` as a bare number with coarse sub-field presence only, `pan.panmean` only. Never prints `pgmscl2`, `pan2`, `pan.paninfo`, any `mixdef` variant's actual values, or `blkmixcfginfo`'s six words. |
| CLI docs overstate this | `docs/forge/cli/metadata-options.md:233-235` | Claims decode prints "any... programme-mixing field the stream actually carries" — false against the code above. |
| Hearth JSON stops earlier | `apps/hearth/engine/media_info.cpp:355-394` (`write_mix`) | Reports `dmixmod`, the four fold levels, `lfemixlevcod`, `pgmscl`/`pgmscl2`/`extpgmscl`. `mixdef`, `pan`/`pan2`, `blkmixcfginfo` are absent entirely. |
| `ac3cli probe` structurally cannot report most of it | `ac3/io/probe.hpp:153-229`, `ac3/io/elementary.hpp:163-198` | `ProbeReport`/`FrameHeader` carry exactly one mixing field, `dmixmod`. This is a missing-struct-member gap, not a missing-print-statement gap. |
| GUI exposes 3 of ~30 fields | `apps/gui/encoder_controller.hpp:380-384`, `qml/Main.qml:4112-4167` | `mixmeta` toggle, `dmixIndex`, `lfeMix`. Zero references anywhere in `apps/gui/` to `mixdepth`/`pgmscl`/`mixdef`/`paninfo`/`blkmixcfg`. |
| C API: no surface at all | `src/capi/include/ac3forge_c/ac3forge.h:427-437` | Self-documented: *"Not mirrored here: the `mixmdate`/`infomdat` metadata groups... see docs/library/c-api.md's 'What is deliberately out of scope'."* Confirmed in `docs/library/c-api.md:312-324`. |
| Python bindings: no surface at all | `python/src/ac3forge_ext/bindings.cpp:1487-1490` | Self-documented as *"a real gap, not a stable design decision."* |
| Test coverage holes | `tests/meta/test_bsi.cpp` | `mixdef==kNone` has no dedicated round-trip assertion; the `addche` auxiliary pair is tested only as one-set-one-absent, never both-set or fully cleared; `SpeechEnhancement`'s two shallower legal nesting states are untested (only the fully-nested case is); `blkmixcfginfo` is never tested under `numblkscod==0x0` (§E2.3.1.60's one-block-inferred case — no test sets `numblkscod` at all); `valid_mix_metadata()` is exercised for exactly one branch (`dmixmod` reserved). `tests/meta/test_mixing.cpp` tests none of this despite its name — it covers only §7.8 downmix math. `tests/encoder/test_eac3.cpp`/`test_plan.cpp` have zero references to any of this. |
| Nothing *uses* the decoded values | `src/forge/src/decoder/output.cpp:566-583` (`mix_levels`) | Reads only the 4 fold levels + `dmixmod` + `lfemixlevcod`. `pgmscl`/`pgmscl2`/`extpgmscl`, all of `mixdef`, `pan`/`pan2`, `blkmixcfginfo` have zero effect on decoded audio anywhere in the codebase (confirmed also in `stream_tools.cpp`'s transcode path and `metadata_edit.cpp`, both of which explicitly skip these fields by design). |
~~`programme2=` can't set its own mixing metadata~~ | ~~`apps/cli/commands/encode.cpp:197-200`~~ | **Resolved outside this plan, 2026-09-22.** Was self-documented: *"a second programme's DRC profile, mix metadata and downmix levels are its own, and this first cut does not offer a way to say what they are."* "EAC3 multi-program authoring implementation" generalized this to `programmeN=`/`programmeN-<field>=` for N=2..8 with full per-programme metadata parity (incl. `dialnorm=auto` measurement), verified live via an 8-programme CLI round-trip. Kept here as a resolved record, not a current gap. |

## Phases

Ordered lowest-risk/most-mechanical first, matching this project's usual pattern for multi-phase
work (see the Annex E coding-tools initiative: coupling → spectral extension → AHT → cleanup).
Each phase is its own branch off a freshly-fetched `main` and its own PR.

### Phase 1 — Reporting completeness

Make every already-decoded field visible somewhere. No new struct fields needed except in probe
(see Decision 2).

- Extend `print_mix_summary()` (`decode.cpp:263-291`): add `pgmscl2`, `pan2`, `pan.paninfo`; for
  `mixdef`, print the actual sub-field values per variant — the premix triple for `kPremix`, the
  twelve reserved bits (hex) for `kReserved`, and for `kExtended` every set external-scale value
  (`left`/`centre`/`right`/`left_surround`/`right_surround`/`lfe`/`dmixscl`, the `auxiliary` pair)
  plus the full `SpeechEnhancement` tree when present; print `blkmixcfginfo`'s six words (or "—"
  for an absent one), not just a presence flag.
- Extend `write_mix()` (`media_info.cpp:355-394`) and its JSON-shape doc comment
  (`media_info.hpp:123` area) the same way, as JSON fields rather than formatted strings.
- Once both are accurate, `docs/forge/cli/metadata-options.md:233-235`'s claim becomes true;
  re-read it against the new code and adjust wording if anything still doesn't match exactly.

**Exit criterion**: `ac3cli eac3-encode` with every mixing flag set (the worked example already in
`metadata-options.md:224-226`), then `ac3cli decode --stats` (or equivalent) and Hearth's JSON
report both show every value that was set, not just presence.

### Phase 2 — Test coverage completion

Close the five named holes in `tests/meta/test_bsi.cpp`'s DC4 section:

1. An explicit `mixdef==kNone` round-trip (confirms `mixdefe`... no, confirms no mixdef bits ride
   the wire beyond the 2-bit selector, and that the struct's default state round-trips cleanly).
2. `addche` with both halves set, and with the whole `auxiliary` optional cleared while other
   external scales are present.
3. `SpeechEnhancement`'s two shallower nesting states: `spchdat` alone (`additional` absent), and
   `spchdat`+`additional` with `more` absent.
4. `blkmixcfginfo` under `numblkscod==0x0` — the one-block-inferred case where the per-block flag
   is never on the wire and the single word is unconditional.
5. Direct `valid_mix_metadata()` coverage for `mixdef`/`pan`/`blkmixcfginfo` range violations, not
   only `dmixmod`.

Add baseline unit coverage to `tests/encoder/test_eac3.cpp`/`test_plan.cpp` and
`tests/decoder/test_eac3_decoder.cpp`, which currently have none. Leave `test_mixing.cpp`'s name
as-is but note the mismatch isn't this plan's to fix (it tests real, correctly-scoped §7.8 downmix
math — Phase 5 adds its own new pure-function tests there, see below).

**Exit criterion**: the five gaps above each have a passing, real encode→decode assertion; full
suite still green.

**Result, 2026-09-22**: writing gap 4's test (`blkmixcfginfo` at `numblkscod==0x0`) failed on
first run with `DecodeError::kInvalidStream` — not a test-construction mistake. The encoder
(`eac3_frame.cpp`'s `blkmixcfginfo`/`frmmixcfginfoe` emission) always wrote the six-block wire
form no matter what `numblkscod` was, while the decoder already correctly implemented §E2.3.1.60's
one-block special case (confirmed against a third independent implementation, the Python reference
parser `tools/references/eac3_parse.py`, which also has it right). The two desynced whenever a
caller combined `numblkscod=0` with `blkmixcfginfo` — the encoder's own output failed its own
decoder. Fixed by adding the same `numblkscod==0x0` branch to the encoder's emission, mirroring
the decoder's; all five gap tests and the full `[numblkscod]`/`[bsi]` suites pass afterward. This
is the kind of defect real round-trip testing at an unusual `numblkscod` is specifically good at
catching — the project's own history has one precedent already (a comment in `eac3_parse.py`
records an earlier, different bug in that same reference parser, found the same way).

### Phase 3 — C API and Python bindings

Mirror the existing pattern used for `ac3forge_centre_mix_level_t`/`surround_mix_level_t`
(`ac3forge.h:197-207`): new C structs mirroring `MixMetadata`'s shape, accessor functions on
`ac3forge_decoded_substream_t` for decode, and setter fields on the E-AC-3 encoder config for
encode. Bind the same surface in `python/src/ac3forge_ext/bindings.cpp`, update
`python/src/ac3forge/__init__.pyi`. This is explicitly named as deferred work in both
`docs/library/c-api.md:312-324` and the bindings.cpp comment, not a fresh discovery — the shape to
mirror already exists, this is filling in a known blank.

**Exit criterion**: a C and a Python round-trip test each set the full field set on encode and read
it back correctly on decode.

### Phase 4 — GUI exposure

**Recommended scope, narrower than full parity**: add a read-only decode-side summary (reusing
Phase 1's text) so a loaded stream's mixing metadata is visible somewhere in `ac3hearth`/`ac3gui`.
Defer full encode-side authoring widgets (nested editors for `mixdef==kExtended`'s external scales
and speech enhancement, `blkmixcfginfo`'s six-word grid) — CLI already covers this completely for
the audience who sets these fields (broadcast/mastering engineers scripting a delivery chain), and
`docs/forge/gui/metadata.md` has always scoped the GUI narrowly on purpose. See Decision 1.

**Exit criterion**: loading a stream encoded with the full field set shows its `pgmscl`/`mixdef`
summary/`pan` position/`blkmixcfginfo` presence somewhere in the GUI, without requiring new
encode-side widgets.

### Phase 5 — Decode-time associated-service mixing

The one genuinely new feature. Full design (produced via a dedicated design pass, included in
full below the phase list) recommends a small, stateful, **post-decode, PCM-domain** component:

```cpp
// src/forge/include/ac3/decoder/associated_service.hpp
class AC3FORGE_EXPORT AssociatedServiceMixer {
   public:
    [[nodiscard]] std::expected<AssociatedServiceMixResult, MixError> mix(
        DecodedAccessUnit& main, const DecodedAccessUnit& associated,
        const AssociatedServiceMixConfig& config = {});
    void reset();
   private:
    double premix_gain_ = 1.0;  // ramped across calls, like OutputStage::protection_gain_
};
```

It sums an already-decoded, already-dialnorm-normalised **associated** programme's audio into a
**main** programme's own channels in place, reading gain/premix-compression/pan instructions from
the *associated* programme's own `MixMetadata` only (an explicit, documented design choice — Table
E1.2 doesn't define a merge rule for two independently-authored copies of this group, so the
design doesn't invent one). It is a new sibling of `OutputStage`, not a change to it, and runs
before any `OutputStage` fold.

Key formulas (all cite existing spec sections already used elsewhere in this codebase):

- **Gain**: `pgm_scale_gain(code)`/new helper beside `pgm_scale_db`, same one-line pattern as
  `lfe_mix_gain`. `this_gain` from `pgmscl`, `external_gain` from `extpgmscl` (absent = code 51 =
  0 dB).
- **Premix compression** (mixdef 0x1, or 0x3's `mixdata2e` triple): selects `dynrng` or `compr`
  from whichever substream `drcsrc` names, raises that word's gain to the `premixcmpscl/6` power
  (Table E2.7's "0%..100% in sixths" — the existing partial-compression pattern in
  `src/forge/src/decoder/gain.hpp:61-90`), applied as a ramp across each 256-sample block using the
  same click-avoidance technique `output.cpp:462-508`'s RF-mode protection gain already uses.
- **Pan**: reuses `eac3_seat_fold.hpp`'s seat grouping (five horizontal positions any layout
  reduces to) but excludes height locations outright rather than folding them; a standard
  constant-power pan law across the ring of the main programme's *occupied* seats, azimuths from
  ITU-R BS.775 speaker positions.
- **Explicitly not applied, by design**: `dmixscl` (targets a downmix that doesn't exist at this
  stage), `auxiliary`/`addche` (no channel destination in this codebase's `chanmap` model),
  `blkmixcfginfo`, `mixdef==kReserved`'s bits, and `SpeechEnhancement` (all "placeholders for as
  yet undefined data" per `mixing.hpp`'s own characterisation) — all remain decoded and exposed,
  none silently affect audio. A test asserts a mix with any of these set is bit-identical to one
  without.
- **Explicitly excluded from this phase**: CLI wiring to actually invoke the mixer (natural next
  increment, not folded in here); resampling between differently-rated main/associated programmes
  (mismatch is a checked error, not silently handled).

New files: `src/forge/include/ac3/decoder/associated_service.hpp`,
`src/forge/src/decoder/associated_service.cpp`,
`tests/decoder/test_associated_service_mixer.cpp`. Touched: `mixing.hpp`/`mixing.cpp` (two new
gain helpers), `CMakeLists.txt`/`tests/CMakeLists.txt` (registration), `docs/library/decoding.md`
(new subsection), `docs/library/capabilities.md` (Metadata table + Decoding section).

**Before writing the pan-law/premix-scale code**: locate the licensed spec text
(`docs/spec/A52-2018.txt`, gitignored — not present in this worktree; a prior session found a copy
in a sibling worktree, so check there first rather than re-sourcing) and confirm two specific
readings the design had to assume without it:

1. Table E2.7's `premixcmpscl` code `7` — the design clamps it to the same 100% as code 6, since
   "0%..100% in sixths" doesn't by itself define a seventh step.
2. `panmean`'s rotation handedness — the design assumes clockwise as front→right→rear→left when
   viewed from above; getting this backwards silently mirrors every pan position.

Neither blocks starting the phase — both are checkable in isolation before the pan/premix code is
written, and both are cheap to flip if wrong since they're single constants/formulas, not
structural.

**Exit criterion**: two synthetic encoded programmes (known tone/level/`pgmscl`/`extpgmscl`/`pan`/
premix settings) decoded independently, mixed, and the combined PCM matches hand-computed expected
per-channel level and position — the associated tone at the predicted level in the predicted
seats, main's own tone correctly gain-conditioned, channel count/order unchanged. Plus: the
pure-function pan-law and gain tests, the no-op confirmation for reserved/speech fields, and the
block-boundary click-avoidance check.

### Phase 6 (not this plan) — CLI wiring for the mixer

Once Phase 5 lands, a natural follow-on is `ac3cli decode associated=<id> ...` to actually invoke
it end to end. Deliberately not built here, per Phase 5's own scope boundary — keeps that PR
reviewable as one self-contained unit.

## Decisions

| # | Question | Recommendation | Cost of the alternative |
|---|---|---|---|
| 1 | GUI: read-only decode summary only (Phase 4), or full nested authoring UI for `mixdef==kExtended`'s sub-fields and `blkmixcfginfo`? | Read-only summary now; defer authoring UI | Full UI is a large, largely-unused surface (this is the deepest, least-common corner of the metadata space) for an audience CLI already serves |
| 2 | Extend `ac3cli probe`'s `ProbeReport`/`FrameHeader` structs to carry the full field set, or leave probe lightweight? | Leave it lightweight; `ac3cli decode` (post-Phase-1) and Hearth's JSON (also post-Phase-1) already cover deep inspection, including machine-parseable JSON for automation | Extending probe duplicates that reporting path in a third place for marginal benefit |
| 3 | Table E2.7 code 7 and `panmean` handedness (Phase 5) | Confirm against `docs/spec/A52-2018.txt` before writing the affected code, per the note above | Shipping the assumed reading risks a silent, hard-to-notice mirroring/clamping bug |

## What cannot be verified and why

- Phase 5's pan law and premix-compression composition can be checked internally (hand-computed
  expected levels against the design's own formulas) but not against a third-party oracle: neither
  FFmpeg nor the Dolby Reference Player — this project's usual cross-checks — decode more than one
  programme at a time, so there's no reference implementation of "combine two decoded E-AC-3
  programmes" to compare against. Confidence here rests on spec citations and internal consistency,
  not an external match, which is a genuinely different (weaker) evidence class than most of this
  codebase's other DSP claims.
- The two items in Decision 3 stay open until someone reads the primary spec text directly; this
  plan states the assumed readings rather than presenting them as settled.

## Verification

- Phases 1-4: existing test suites plus the new cases each phase adds; a manual CLI round-trip
  (encode with the full field set via existing flags, decode, confirm full reporting) after Phase
  1; a C/Python round-trip script after Phase 3.
- Phase 5: `tests/decoder/test_associated_service_mixer.cpp` end to end, plus the full suite to
  confirm no regression to `OutputStage`/`mix_levels()`, which Phase 5 does not modify.
