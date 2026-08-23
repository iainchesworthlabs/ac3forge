# Fuzzing

libFuzzer harnesses over every place ac3forge parses externally-supplied
binary data. This is the codec's natural attack surface: its whole job is
decoding bitstreams whose structure it cannot control, and the project has
already had one real bug in this class - commit `8386c8f` fixed a decoder
that shifted by an unvalidated exponent, walked outside the range the
reconstruction code assumed by a malformed differential chain, and hit
undefined behaviour on hostile input. It was found by a one-off manual
adversarial audit; this directory makes that kind of input-shape exploration
continuous and automatic instead.

## Why Clang only

libFuzzer (`-fsanitize=fuzzer`) is an LLVM built-in. GCC and MSVC do not ship
it, so everything here requires upstream Clang - specifically the
`linux-llvm` / `macos-llvm` toolchain this project already has presets for
(`windows-llvm` is clang-cl, whose libFuzzer support on Windows this project
has never exercised, so it is deliberately out of scope; see
`fuzz/CMakeLists.txt`'s `CMAKE_CXX_COMPILER_FRONTEND_VARIANT` guard).

`.github/toolchain/03-llvm-toolchain.sh` installs `libclang-rt-<ver>-dev` (the
ASan/UBSan/libFuzzer runtime archives) unconditionally alongside the Clang
compiler itself, even though none of `ci.yml`'s other Clang legs link a
sanitizer - simplest to always have it rather than fork the install for the
sanitizer/fuzzing presets alone. `fuzz.yml` needs no separate step for it; a
local Debian/Ubuntu run needs `apt-get install libclang-rt-22-dev` (or your
distro's equivalent) before `fuzz/run.sh` will link.

## `-Werror` is on for this build too

This build once opted out of `ac3::warnings`: `AC3FORGE_BUILD_FUZZERS` skipped
linking it into `ac3forge`, on the stated assumption that the codebase carried
roughly sixteen sign-conversion and double-promotion sites that only the
Windows MSVC leg had ever been held to, and that clearing them belonged to the
cross-platform porting task rather than to fuzzing.

That number was never measured, and it was wrong. Building the harnesses with
`ac3::warnings` linked in, under Clang 21 with the full set
(`-Werror -Wconversion -Wsign-conversion -Wdouble-promotion -Wold-style-cast`
and the rest) alongside ASan/UBSan/libFuzzer, produces **zero** warnings - the
other legs had gone green in the meantime and taken the debt with them. The
exemption was removed rather than re-justified, so `ac3forge` now compiles
under one warning set in every configuration, this one included.

All harness executables link `ac3::warnings` too, and had no warnings of
their own either. They need to name it explicitly: `ac3forge` links it
`PRIVATE`, so the flags govern the library's own sources and do not propagate
to anything downstream of it.

That the set is genuinely live, and not merely listed on the command line, was
checked twice - once by injecting a deliberate sign-conversion and double-
promotion into a library source, once into a harness source - confirming the
fuzz build fails on both in each case.

## Status at the commit that added this

Like `ci.yml`'s own leg-status table, this is a point-in-time result, not a
standing guarantee - re-run it yourself rather than trusting an old number.

Two full bounded passes ran locally before this landed (Docker: `ubuntu:26.04`
+ LLVM 21, matching CI's `linux-llvm` leg, since this was developed on a
Windows host with no native libFuzzer). The first pass used a Debug build and
was clean but showed pathologically low throughput on the decode harnesses
(2-3 exec/s); switching to `RelWithDebInfo` - libFuzzer's own advice, build
with optimizations on even under sanitizers - fixed that. Numbers below are
the second pass, 180s/harness:

| Harness             | Executions | Corpus grown to  | Result |
|----------------------|-----------:|------------------|--------|
| `fuzz_scan`          |      ~25.9M | 116 files / 1.0MB | clean  |
| `fuzz_ac3_decode`    |       3,011 | 184 files / 5.8MB | clean  |
| `fuzz_eac3_decode`   |       2,796 | 166 files / 7.6MB | clean  |
| `fuzz_wav_read`      |      13,370 | 56 files / 14MB   | clean  |

No crash, hang, or sanitizer report across ~45M total executions between the
two passes; `fuzz/regressions/` is empty as of this commit. `fuzz_scan`'s exec
count dwarfs the decode harnesses' because a format-sniff is orders of
magnitude cheaper than a real IMDCT-and-bit-allocation decode - expected, not
a sign anything is under-tested relative to its own cost.

## Entry points covered

| Harness              | Calls                                                              |
|-----------------------|--------------------------------------------------------------------|
| `fuzz_scan`            | `ac3::io::scan` - format-sniffing before any decoder commits to a layout |
| `fuzz_ac3_decode`      | `ac3::split_frames` + `ac3::FrameDecoder::decode_frame`, one decoder across all frames, the way `ac3cli decode` drives it |
| `fuzz_eac3_decode`     | `ac3::split_access_units` + `ac3::Eac3Decoder::decode_access_unit` (which calls `decode_substream` internally), the way `ac3cli decode` drives it for E-AC-3 |
| `fuzz_wav_read`        | `ac3::io::read_wav` - a realistic input too (a truncated or hand-edited WAV), not only an adversarial one |
| `fuzz_iec61937_unwrap` | `ac3::iec61937::BurstReader` + `unwrap_stream` - IEC 61937 burst de-framing, driven the way `ac3cli unspdif` drives it. The input is by definition off a wire (an S/PDIF or HDMI capture), and `Pd` states a length the parser must not believe past its data type's repetition period. Pushed as two chunks split at a mutation-chosen point, so the state machine's carry-across-a-chunk-boundary paths are reachable |

`matroska::` was checked and has no read/demux path - `matroska::mux()` only
ever produces bytes from frames already in hand, so there is nothing to fuzz
there. `ac3::io::read_wav` takes a path rather than a byte span, so
`fuzz_wav_read` round-trips libFuzzer's buffer through a scratch file
(`/dev/shm` when available) before calling it - the one unavoidable step
beyond calling the real function directly, since there is no in-memory
overload to call instead.

## Differential mode (roadmap G3)

`fuzz_differential_ac3_decode` and `fuzz_differential_eac3_decode` drive the
exact same decode paths as `fuzz_ac3_decode`/`fuzz_eac3_decode` above, but
instead of (in addition to - a crash is still a crash) only checking for a
crash or sanitizer trip, they decode the SAME mutated bytes a second time
with FFmpeg and diff the resulting PCM against this project's own decode.
`fuzz/differential_oracle.hpp` has the full mechanism and reasoning; the
short version:

- Both decoders have to accept the ENTIRE input - every frame/access unit,
  one unchanging acmod/sample rate throughout - before FFmpeg is even
  invoked. The overwhelming majority of mutations get rejected immediately
  by this project's own decoder (bad sync word, bad CRC, a reserved field),
  and none of those are worth a real FFmpeg process.
- A **PCM mismatch is only reported as a divergence when both decoders
  accepted the input and produced comparably-shaped audio.** FFmpeg's own
  error-concealment on a mutated (i.e. potentially malformed) frame can
  legitimately differ from this project's spec-strict decode - that proves
  nothing about which one is right, so it is treated as "no oracle for this
  one," the same stance `tools/ci/run_codec_matrix.sh` already takes for the
  Annex E tool combinations FFmpeg has no reading of at all (enhanced
  coupling, transient pre-noise processing, a second dependent substream/
  7.1.4 - see `docs/verification.md`'s "Where the oracles don't reach").
- Where a comparison IS eligible, the floor - `kMinAgreementDb = 6.0` in
  `fuzz/differential_oracle.hpp` - is deliberately loose relative to what a
  clean, non-fuzzed stream actually measures at (`docs/verification.md`:
  float32-precision parity for the plain path, 98+ dB for coupling/spectral
  extension, 62-89 dB for AHT). It started from
  `tools/checks/verify_gold_reference.sh`'s own `CPLBNDSTRCE0_MIN_SNR_DB=15`
  precedent - this project's one existing floor for "two decodes of a
  bitstream neither side controls" - and was then calibrated down to 6 dB
  after `fuzz/measure-agreement.sh` found committed seeds that legitimately
  measure below 15 dB (real, unmutated content whose bap-0 reconstruction
  FFmpeg dithers and this decoder zeros).
- `fuzz/measure-agreement.sh` is the calibration method behind that floor:
  it runs every committed seed through the differential harnesses in
  measure-only mode and reports the worst-channel SNR each one lands on.
  Re-run it after adding seed content, and after any change to
  `compare_pcm`'s own alignment/silence-skip logic - a new corner of
  legitimate decoder disagreement needs the floor reconsidered, not
  assumed.

Because every comparable input spawns a real FFmpeg process, these two
harnesses are much slower per-exec than every other harness here and are
NOT in `fuzz/run.sh`'s default target list or in the `fuzz-regress`/
`fuzz-short`/`fuzz-nightly` CI jobs - they get their own job,
`fuzz-differential` (see the CI section below), and need `ffmpeg` on PATH to
compare anything at all (silently a no-op otherwise, same as running without
`ffmpeg` installed locally). They share their crash-only siblings' seed
corpora rather than duplicating those files (`fuzz/run.sh`'s
`seed_source_for`) - same bytes, same decode path, just with an extra
comparison bolted on.

## The other direction: the encoder's input space

Everything above mutates an already-encoded bitstream. That answers "does the
DECODER survive corrupt input", and it is the whole of what this directory
covered for a long time. The mirror-image question - "does the ENCODER, driven
across its own legal configuration space by adversarial but perfectly valid
audio, ever emit a stream a decoder refuses" - is
**`tools/ci/fuzz_encoder_space.py`**, and nothing here asks it.

It is not a libFuzzer target and not part of `fuzz/run.sh`: it drives the real
`ac3cli`, so it needs the ordinary CLI build rather than this directory's
sanitizer/libFuzzer toolchain, and its failure signal is a decoder refusing a
stream rather than a sanitizer report. Per case it draws a random legal
encoder configuration (layout, bitrate, coupling, DRC, heavy compression,
dialnorm, downmix levels, forward-MDCT path), draws adversarial PCM built per
256-sample BLOCK so a frame's character can change part-way through it,
encodes, and then decodes the result with BOTH `ac3cli decode` and FFmpeg's
strict decode - the same invocation `tools/ci/run_codec_matrix.sh` uses. A
refusal from either fails the case, with one arbitrated exception: when only
FFmpeg's default invocation refuses and the same bytes decode cleanly under
`-f ac3` with every error check kept, libavformat's container *guess* failed
rather than the stream, and the case counts as "misprobed" instead - measured
and explained in the script's note above `MIN_STREAM_BYTES` (large syncframes
can lose FFmpeg's probe-window race to the MPEG-PS prober no matter how long
the stream is).

Why it exists: PR #186 fixed an encoder defect (`deltbaie == 0` means "retain
the previous block's delta bit allocation", not "no delta") that produced
streams both decoders reject, and it escaped ctest, the codec matrix, the
gold-reference gate and every job in this file. Reaching it needs dense
harmonic content followed by digital silence inside one frame - an input
SHAPE, not an option combination, which is why enumerating options more
thoroughly would never have found it. The harness finds it in seconds; that
was verified by reverting the fix and running it (see the file's own header).

```bash
AC3CLI=build/config-linux-llvm/bin/ac3cli python3 tools/ci/fuzz_encoder_space.py --seconds 120
python3 tools/ci/fuzz_encoder_space.py --check-envelope      # re-measure the rate floors it draws from
python3 tools/ci/fuzz_encoder_space.py --replay <case-seed>  # rerun one exact failing case
python3 tools/ci/fuzz_encoder_space.py --regressions         # replay every recorded past failure
```

Every case is a pure function of one 64-bit case seed, printed beside any
failure, so a random run stays fully reproducible after the fact. Failing
inputs are kept under `fuzz-encoder-artifacts/` (gitignored, and regenerable
from the seed).

Scope: AC-3 `encode` only.

### The E-AC-3 half

E-AC-3's own configuration space is **`tools/ci/fuzz_eac3_encoder_space.py`**
(roadmap VX1), which the file above used to name as its own remaining gap. It
asks the same question of `eac3-encode` and `atmos-encode`, over the part of
the space that is E-AC-3's alone: Annex E tool tokens with their band-edge
pins (`cpl`, `ecpl`, `spx`, `aht`, `tpn`, `auto`), the `fscod2` half sample
rates, CBR and VBR, every layout including the ones that need dependent
substreams, and Atmos object counts. It imports the AC-3 harness's PCM
generator rather than copying it, so `cliff` and the rest of the adversarial
material are one implementation serving both.

Two things about it are genuinely different, and both come from E-AC-3 rather
than from a preference:

**The oracle is not one oracle.** FFmpeg reads AC-3 whole; it does not read
E-AC-3 whole, and what it cannot read is exactly what this covers. It refuses
a second dependent substream (`substreamid != 0`, which 7.1.4 needs), has no
model at all of enhanced coupling or transient pre-noise processing, and
refuses `fscod2` audio outright - as does Dolby's own Reference Player. So
every case is classified before it runs, and each class is checked as hard as
something external still can:

| class | what runs |
|---|---|
| `full` | FFmpeg strict decode, `run_codec_matrix.sh`'s exact invocation, plus both framing checks below |
| `header` | no FFmpeg decode, but the framing is still checked - twice |
| `none` | nothing at all - empty, kept so a future cell that escapes even the independent walk is reported rather than silently passed |

The `header` class is the "no oracle" cell class, and it is deliberately not
an empty gesture, because framing can be checked without decoding anything.
Two things do it:

- **`syncframe_walk()`**, the harness's own walk over the four fields that
  decide E-AC-3's framing - syncword, `strmtyp`, `substreamid`, `frmsiz`, all
  at fixed bit offsets right after the syncword. No tables, no coding tools,
  nothing shared with the encoder, and it works at **every** layout. If a
  `frmsiz` does not describe its own syncframe, the next read lands somewhere
  that is not a syncword and it says so - which is exactly what a bit-offset
  defect produces, and the shape of the `deltbaie` bug that motivated the AC-3
  harness.
- **`ffprobe`**, where FFmpeg can be trusted to walk one: access-unit count,
  exact byte tiling, sample rate. It is *not* asked about a layout needing two
  dependent substreams, and that is measurement rather than caution - see
  below.

What the class does *not* prove is stated in the script too: a misreading of
the spec shared by this project's encoder and its decoder would survive it,
and so would one shared by the encoder and the field layout the walk reads.
That is the same limitation `docs/verification.md` records for the CI gate
covering `ecpl`/`tpn` today. `--check-oracles` re-measures the whole table
against the installed FFmpeg, so a cell wrongly listed as a gap cannot quietly
stop being tested.

The 7.1.4 exclusion was itself a harness finding. On case seed
`4765573204069690189` - a 7.1.4 VBR stream at 32 kHz - `ffprobe` reported 19
access units where the encoder wrote 18, splitting one 1329/207 at an offset
that is not a syncframe boundary at all. The independent walk found all 54
syncframes forming 18 access units of exactly 1536 bytes, tiling the file with
no slack: the stream was correct, and FFmpeg's demuxer had lost sync inside
the second dependent substream `ff_ac3_parse_header` refuses to parse, then
resynced on an ordinary byte pattern. Asserting its packet count there was
asserting FFmpeg's limitation, not the stream - the same trap already avoided
for `sample_rate`, which it reports as 0 on those streams for the same reason.
The seed is kept in `REGRESSION_SEEDS`.

**The acceptance envelope has a ceiling as well as a floor.** AC-3's
`frmsizcod` indexes Table 5.18, so its frame size follows from the rate pair.
E-AC-3 signals the size directly in `frmsiz`, which is 11 bits - a hard
2048-word cap on any syncframe. At 48 kHz that binds nowhere near the top of
the rate list; at the Annex E half rates it binds *inside* it, at 320 kbit/s
for 16 kHz, 448 for 22.05 kHz and 512 for 24 kHz. `--check-envelope` measures
the per-layout rate floors and that ceiling together.

What it found on its first sweep: `eac3-encode` **aborted on an assertion**
for any rate above that ceiling, at every layout. Both halves of such a pair
are legal on their own and nothing in the CLI's grammar marks the combination,
so it took two ordinary numbers to reach. Nothing else could have seen it -
`run_codec_matrix.sh`'s only WAV source is 48 kHz, and `eac3-sine`/
`eac3-silence` have no sample-rate argument at all. It now reports the actual
limit, `--check-envelope` gates the refusal staying a clean exit 1, and
`tests/cli/test_cli.cpp`'s `[frmsiz]` case pins the message.

```bash
AC3CLI=build/config-linux-llvm/bin/ac3cli python3 tools/ci/fuzz_eac3_encoder_space.py --seconds 120
python3 tools/ci/fuzz_eac3_encoder_space.py --check-envelope   # rate floors + the frmsiz ceiling
python3 tools/ci/fuzz_eac3_encoder_space.py --check-oracles    # what this FFmpeg can actually read
python3 tools/ci/fuzz_eac3_encoder_space.py --replay <case-seed>
python3 tools/ci/fuzz_eac3_encoder_space.py --regressions
```

Failing inputs are kept under `fuzz-eac3-encoder-artifacts/` (gitignored, and
regenerable from the seed). Both harnesses run bounded in `ci.yml`'s
`ffmpeg-validate` job on every pull request, and deeper in `fuzz.yml`'s
`encoder-space-nightly` job, which has a separate dispatch budget for each.

## Running locally

```bash
# One-time: any Clang 18+ with libFuzzer works; CI pins LLVM 22 the same way
# ci.yml's linux-llvm leg does (.github/toolchain/03-llvm-toolchain.sh).
fuzz/run.sh                    # build, then run every default-list harness for 60s each
fuzz/run.sh fuzz_scan          # just one harness
AC3FORGE_FUZZ_SECONDS=600 fuzz/run.sh   # a deeper local run
fuzz/run.sh regress            # replay seeds + regressions, no mutation (fast)
fuzz/run.sh minimize fuzz_scan fuzz/artifacts/fuzz_scan-crash-<hash>

# Differential harnesses need `ffmpeg` on PATH and are named explicitly -
# see "Differential mode" above for why they're not in the default list.
fuzz/run.sh run fuzz_differential_ac3_decode fuzz_differential_eac3_decode
```

On Windows, run this from WSL or inside a Linux container - there is no
libFuzzer under MSVC or clang-cl here. The commands used to develop this
directory ran inside `docker run ubuntu:26.04` with the repo bind-mounted,
which is exactly what `.github/workflows/fuzz.yml`'s containers do.

See `fuzz/run.sh --help`-equivalent (its own header comment) for the full
environment-variable list.

## Seed corpus

`fuzz/seeds/<harness>/` is a curated, committed bootstrap corpus generated
from ac3forge's own valid output - `fuzz/generate-seeds.sh` drives `ac3cli`
across the layout/codec/Annex-E-tool matrix this project already supports
(every layout token, every tool combination, both codecs, silence and real
audio, coupled and uncoupled, Atmos objects and the bed51 fallback) and
collects the resulting streams. Starting mutation from real, self-consistent
streams is what lets a fuzzer's mutations explore "almost valid" input
instead of spending its budget on bytes that get rejected before line one of
the parser.

Regenerate it with:

```bash
AC3CLI_BIN=build/config-windows-msvc-debug/bin/ac3cli.exe fuzz/generate-seeds.sh
```

(Any *working* `ac3cli` build does - this only needs it to produce valid
streams, not to run instrumented. The MSVC leg is the practical choice on a
Windows host simply because it is the one already built there.)

`fuzz/seeds/` is intentionally small (a few MB) and committed to the repo.
`fuzz/corpus/` - what a real mutation run *grows* into over its time budget -
is not: it is regenerable from the seeds plus a mutation budget, and libFuzzer
corpora can reach hundreds of MB, which does not belong in git history. It is
gitignored; `fuzz/run.sh` creates it on demand.

## When a fuzzer finds something

1. libFuzzer minimizes automatically (or run `fuzz/run.sh minimize <target>
   <path>` on a saved artifact).
2. The minimized input is added to `fuzz/regressions/<harness>/` and
   committed - `fuzz/run.sh regress` (and `fuzz-regress` in CI) replays every
   file there on every run, so a fixed bug can never silently regress.
3. The underlying bug gets a real, spec-grounded fix in the library - never
   just enough to make the fuzzer stop finding it.
4. Before considering it fixed: check out the pre-fix commit and confirm the
   *original* minimized input actually reproduces the crash there. A
   regression test that was never shown to fail is not proof of anything.

## CI

`.github/workflows/fuzz.yml`:

- `fuzz-regress` - replays `fuzz/seeds/` + `fuzz/regressions/` with no
  mutation, on every push/PR to `main`/`develop`. Seconds, not minutes, and
  not marked experimental: a failure here means a previously-fixed bug came
  back, which should always be loud.
- `fuzz-short` - a 60-second-per-harness mutation budget over the crash-only
  harnesses, push only (not pull_request, to keep PR turnaround unaffected).
- `fuzz-differential` - the same 60-second-per-harness mutation budget, push
  only, but over ONLY the two differential harnesses (see "Differential
  mode" above) - a separate job rather than folded into `fuzz-short` because
  it needs `ffmpeg` installed and is much slower per-exec (a real FFmpeg
  process per comparable input), so sharing a budget with the crash-only
  harnesses would have starved them of iterations. Also replays
  `fuzz/regressions/fuzz_differential_*` first, same shape as `fuzz-regress`.
- `fuzz-nightly` - a 10-minute-per-harness mutation budget on a daily
  schedule, plus `workflow_dispatch` with a configurable budget for an
  on-demand deeper run. Crash-only harnesses only - see `fuzz-differential`.
- `encoder-space-nightly` - the encoder input-space searches above, both of
  them, on the same daily schedule and `workflow_dispatch`, with a 15-minute
  default budget each (`encoder_space_seconds` and
  `eac3_encoder_space_seconds`). Shares none of the machinery of the other
  four (no libFuzzer, no sanitizer runtime, not in `fuzz/run.sh`): it builds
  the plain `linux-llvm` CLI with vcpkg and a pinned `ffmpeg`, the way
  `ci.yml`'s `ffmpeg-validate` job does. The E-AC-3 half runs its
  `--check-oracles` and `--check-envelope` gates first, for the same reason
  the AC-3 envelope check runs first: a search whose acceptance table or
  oracle table has gone stale comes back green having checked less than it
  claims.

The bounded per-PR counterpart to `encoder-space-nightly` is a step in
`ci.yml`'s `ffmpeg-validate` job (~2 minutes, plus the envelope check), not a
job in this file - everything it needs is already built and pinned there, and
unlike `fuzz-short` it runs on pull requests rather than push only, because it
is cheap enough relative to the job it rides on.

`fuzz-short`, `fuzz-differential` and `fuzz-nightly` run with
`continue-on-error: true`, the same convention `ci.yml` uses for its other
unproven legs: none has multiple clean fuzzing runs behind it yet. That is a
question of track record, and it is not settled by the build being
warnings-clean - a bounded mutation run can still surface something on any
given night. `fuzz-regress` is cheap enough to make a required
branch-protection check once it has proven itself - that is a repository
setting this file cannot declare on its own.

This is a bounded, time-boxed run, not continuous (OSS-Fuzz-style) fuzzing
infrastructure. That is a deliberate scope decision, not a limitation
somebody forgot to lift.
