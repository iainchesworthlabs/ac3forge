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

The section below is the original four harnesses' measurement; the roadmap
VX3 harnesses have their own, further down under "Status: the VX3 harnesses",
along with what they found.

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
two passes; `fuzz/regressions/` was empty at that commit (it is not any
more - see "Status: the VX3 harnesses"). `fuzz_scan`'s exec
count dwarfs the decode harnesses' because a format-sniff is orders of
magnitude cheaper than a real IMDCT-and-bit-allocation decode - expected, not
a sign anything is under-tested relative to its own cost.

## Status: the VX3 harnesses

Same caveat as the section above - a point-in-time result, not a standing
guarantee. Measured on WSL2 Ubuntu 26.04, Clang 21, `RelWithDebInfo` +
ASan/UBSan, 300 s per harness from the committed seed corpus:

| Harness              | Executions | exec/s  | cov  | Result |
|-----------------------|-----------:|--------:|-----:|--------|
| `fuzz_emdf_parse`     |  4,982,134 |  16,551 |  120 | clean  |
| `fuzz_oamd_parse`     | 49,730,651 | 165,218 |  173 | clean  |
| `fuzz_joc_parse`      | 12,319,988 |  40,930 |   99 | clean  |
| `fuzz_signing_verify` |    ~70,000 |     n/a | 870+ | **UBSan report at exec ~70k** |
| `fuzz_adm_parse`      |     ~1,000 |     n/a |  n/a | **out-of-memory in the first minute** |

`fuzz_oamd_parse`'s exec rate is two orders of magnitude above
`fuzz_signing_verify`'s for the obvious reason: an OAMD payload is tens of
bytes and its parse is a bit walk, while a signing verification reconstructs
a whole frame's message A bit by bit and runs HMAC-SHA-256 over it.

### What they found

Three real defects, all fixed in the same change, all with a committed
reproducer under `fuzz/regressions/`:

1. **`compute_bit_allocation` indexed `kMaskTab` at `SIZE_MAX`** on an empty
   allocation region. §7.2.2.4's band walk ends at `kMaskTab[end - 1]`, and
   `end - 1` on `end == 0` is `-1`. The contract was stated only by a debug
   `assert`, so the shipped NDEBUG build had nothing between a hostile frame
   and the pointer overflow - the same shape as `8386c8f`, the bug this
   directory was created for. Reached through `ac3::signing`'s own frame
   walk, but the guard is in the library, so the decoder is covered too.
2. **`ac3adm::read_pcm` sized its buffer from the DECLARED `<data>` chunk
   size**: a 104-byte file claiming ~4 GB of PCM allocated ~4 GB
   (`malloc(8321498636)`). Bounded by the real file size now, which for a
   well-formed file never binds.
3. **`verify_atmos_frame` inherited the SIGNER's debug subset assertion**, so
   a Debug build aborted on `ac3cli decode <plain stereo>.ec3 out.wav
   verify-objects` - an ordinary input for an operation whose whole job is
   checking streams its caller did not produce. Found while writing the
   harness rather than by it: these builds are NDEBUG, so the harness could
   not have caught this one itself.

### What the mutator bought

`fuzz_ac3_decode` and `fuzz_eac3_decode`, 300 s each from an identical fresh
corpus and the same `-seed`, once with the custom mutator and once with it
compiled out. Both grown corpora were then replayed through the SAME binary
with `-runs=0`, so the coverage figures are comparable rather than each
being read off its own build:

| Harness            | Mutator | Executions | Corpus | `cov` | `ft` |
|---------------------|---------|-----------:|-------:|------:|-----:|
| `fuzz_eac3_decode` | off     |      3,366 |    271 |  1151 | 3851 |
| `fuzz_eac3_decode` | on      |      4,546 |    233 |  **1284** | **4147** |
| `fuzz_ac3_decode`  | off     |     12,739 |    283 |   713 | 2384 |
| `fuzz_ac3_decode`  | on      |     12,860 |    295 |  **787** | **2723** |

+11.6% and +10.4% edge coverage, +7.7% and +14.2% features, for the same
wall-clock budget - and on E-AC-3, 35% more executions as well, because a
frame that clears the CRC gets decoded rather than dropped, which is a
cheaper path per input than the corpus churn the rejected ones caused.
The mutator's re-stamping half also has its own portable unit test
(`tests/core/test_crc_mutator.cpp`), so a crc1 solved wrongly would fail the
ordinary test suite on every platform rather than only showing up as a
coverage number that quietly stopped improving.

## Entry points covered

| Harness              | Calls                                                              |
|-----------------------|--------------------------------------------------------------------|
| `fuzz_scan`            | `ac3::io::scan` - format-sniffing before any decoder commits to a layout |
| `fuzz_ac3_decode`      | `ac3::split_frames` + `ac3::FrameDecoder::decode_frame`, one decoder across all frames, the way `ac3cli decode` drives it |
| `fuzz_eac3_decode`     | `ac3::split_access_units` + `ac3::Eac3Decoder::decode_access_unit` (which calls `decode_substream` internally), the way `ac3cli decode` drives it for E-AC-3 |
| `fuzz_wav_read`        | `ac3::io::read_wav` - a realistic input too (a truncated or hand-edited WAV), not only an adversarial one |
| `fuzz_emdf_parse`      | `ac3::emdf::parse_container` - ETSI TS 102 366 Annex H's container, located by a bit-by-bit sync scan and sized by its own 16-bit length field |
| `fuzz_oamd_parse`      | `ac3::oba::parse_payload` - TS 103 420 §5's `object_audio_metadata_payload`, as recovered from an EMDF payload with id 11 |
| `fuzz_joc_parse`       | `ac3::joc::parse_payload` - TS 103 420 §6's `joc()` payload: Huffman-coded coefficients into a matrix sized from the stream's own numbers |
| `fuzz_signing_verify`  | `ac3::signing::verify_atmos_stream` + `verify_atmos_frame` - operator-supplied stream, operator-supplied key, no CRC check in front of either |
| `fuzz_adm_parse`       | `ac3adm::parse_bw64(std::istream&)` - BW64/RF64 chunks plus an arbitrary ADM XML document. Opt-in, see below |

### The object and metadata layer (roadmap VX3)

The last five rows are the parsers behind a skip field in every Atmos frame -
this project's differentiating feature, and the deepest attacker-controlled
bytes in the tree. Before them the layer was reached only indirectly, through
`fuzz_eac3_decode`, which meant it was reached only by mutations that still
had a valid CRC. That is the same blindspot the CRC mutator below exists for,
and the two changes landed together: direct harnesses so a mutation lands
inside the payload, and a mutator so the indirect path stops throwing its
inputs away at the checksum.

They are separate harnesses rather than one chained one because seeding
matters more here than reach. `fuzz/metadata-seeds.py extract` pulls the real
containers and the real OAMD/JOC payloads out of the Atmos streams
`generate-seeds.sh` has just encoded, so each harness starts from bytes its
own parser accepts; reaching the same states through a container would spend
most of the budget on container syntax instead. Nothing in `ac3cli` dumps a
raw payload, and the container is not byte-aligned inside the frame carrying
it (`put_skip_field` writes it 8 bits at a time from wherever the audio
happened to end), so the extractor locates it with the same bit-by-bit sync
scan `parse_container` itself does and repacks from that offset.

`fuzz_signing_verify`'s input is not a bare stream: it is a length byte, that
many key bytes, then the stream. The key is fuzzed because it is untrusted
too - a key file is operator-supplied and may be any length, including empty,
and `verify_atmos_stream` (unlike `sign_atmos_stream`) has no `key.empty()`
early-out - and because keying the HMAC differently is what makes `kValid`
and `kMismatch` both reachable. Nothing here derives, forges or reconstructs
a key; an arbitrary key produces an arbitrary tag, which is all verification
needs to be exercised. Signing is deliberately not fuzzed: it writes into the
caller's own buffer, and a caller signs a stream it just encoded.

### The ADM harness is opt-in

`fuzz_adm_parse` is the one harness here not built by default, and not in
`fuzz/run.sh`'s default target list. `ac3adm` is the one library in this
project with a third-party dependency footprint: `AC3FORGE_BUILD_ADM` is OFF
by default, and turning it on additionally needs vcpkg's `adm` feature for
libadm's Boost headers plus network access for the `FetchContent` pulls of
libbw64 and libadm themselves - none of which anything else in this build
touches. `AC3FORGE_FUZZ_ADM=1 VCPKG_ROOT=... fuzz/run.sh` turns all of that
on and appends the harness to the default list.

It is also the one harness whose reports may not land in ac3forge's own code:
BW64 chunk-walking is libbw64's and ADM XML is libadm's. That is worth
knowing either way - the bytes reach them through an `ac3adm::` API this
project ships - but it changes what "fix it" means for a finding here.

`matroska::` was checked and has no read/demux path - `matroska::mux()` only
ever produces bytes from frames already in hand, so there is nothing to fuzz
there. `ac3::io::read_wav` takes a path rather than a byte span, so
`fuzz_wav_read` round-trips libFuzzer's buffer through a scratch file
(`/dev/shm` when available) before calling it - the one unavoidable step
beyond calling the real function directly, since there is no in-memory
overload to call instead.

## The CRC-repairing mutator (roadmap VX3)

"Differential mode" below records the problem in passing: "the overwhelming
majority of mutations get rejected immediately by this project's own decoder
(bad sync word, bad CRC, a reserved field)". Of those three, bad CRC is the
one that is pure loss. A bad sync word or a reserved value at least exercises
the rejection path it names. A bad CRC rejects an input whose ONLY defect is
the checksum, throwing away whatever the mutation did to the fields behind
it - and `decode_frame` checks crc1 and crc2 before reading one bit of bsi,
`decode_substream` checks crc2 before reading one bit of the audio blocks. So
every mutation that lands in a skip field, which is where the EMDF container
and therefore all of the object metadata lives, died two orders of magnitude
before the parser it was aimed at.

`fuzz_ac3_decode` and `fuzz_eac3_decode` now define an
`LLVMFuzzerCustomMutator` (`fuzz/crc_mutator.hpp`): run libFuzzer's own
mutation first, then walk the result as a concatenation of syncframes -
same bsid-at-bit-40 test and same two size derivations `ac3::split_frames`
uses - and rewrite each frame's CRC words in place.

Re-stamping is not a naive recompute. crc2 is an ordinary trailing CRC, but
crc1 **precedes** the region it protects: A/52 §7.10.1 requires the register
to read zero after the first 5/8 of the syncframe has been shifted through,
and says outright that crc1 is not the CRC of that region. It has to be
solved for, through the GF(2) polynomial inverse `ac3::solve_leading_crc`
implements - the same call `src/forge/src/encoder/encoder.cpp` makes, down to
its crc2 == `kSyncWord` avoidance step (a crc2 that happens to equal 0x0B77
would make the frame's own tail look like the start of the next syncframe, so
the encoder flips crcrsv and recomputes; a mutator skipping that would hand
the splitter a frame boundary that is not there).

Two deliberate limits:

- **Nothing else is repaired.** Frame lengths, reserved fields and the
  bsi/audblk syntax are left exactly as the mutation left them. The goal is
  to stop losing inputs at the checksum, not to constrain the engine to valid
  streams.
- **One mutation in four is left unrepaired**, keyed off libFuzzer's own
  `Seed` argument. Always repairing would make a bad CRC unreachable by
  mutation, and the decoders' behaviour on a frame whose checksum is the only
  thing wrong with it is exactly what a re-stamping mutator would stop anyone
  from ever checking again.

The differential harnesses do not define one. They share their crash-only
siblings' seed corpora, so they inherit the deeper inputs this finds, but
adding the mutator there would multiply the number of inputs both decoders
accept - and every one of those spawns a real FFmpeg process.

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

Scope: AC-3 `encode` only. E-AC-3's own configuration space - the Annex E tool
tokens, VBR, the wider layouts - is a real remaining gap, deliberately left
open rather than half-covered.

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
collects the resulting streams.

Three of the corpora are not raw `ac3cli` output and are built by
`fuzz/metadata-seeds.py`, which `generate-seeds.sh` calls:

- `fuzz_emdf_parse`, `fuzz_oamd_parse`, `fuzz_joc_parse` - `metadata-seeds.py
  extract` reads the Atmos streams just encoded, locates each frame's EMDF
  container by the same bit-by-bit sync scan `emdf::parse_container` does,
  repacks it from that bit offset, and writes out both the container and the
  OAMD/JOC payloads inside it. Capped at six per stream per kind:
  consecutive frames genuinely differ (an object moves, so its position
  fields and JOC coefficients change), but the fiftieth position update
  teaches the engine nothing the sixth did not.
- `fuzz_adm_parse` - `metadata-seeds.py adm` synthesises BW64/RF64 fixtures,
  because nothing `ac3cli` produces is an ADM file. They mirror the ones
  `tests/adm/test_adm.cpp` builds in memory: BS.2088-1 chunk layout,
  BS.2076-2 ADM XML, one Objects document and one DirectSpeakers document,
  plus an RF64 whose `<data>` size resolves through `<ds64>` and a file with
  no `<axml>` at all.

`fuzz_signing_verify`'s corpus is built inline in `generate-seeds.sh`, since
its input is a key-prefixed stream rather than a file `ac3cli` writes: a
throwaway 16-byte key, the same Atmos streams both signed with that key and
left unsigned, and the bed51 fallback, so all three of
`verify_atmos_frame`'s outcomes (`kValid`, `kMismatch`, `kNoContainer`) are
represented. The key is generated fresh from `/dev/urandom` at seed-generation
time and has no significance beyond making the signed copy verifiable. Starting mutation from real, self-consistent
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
- `fuzz-adm-nightly` - `fuzz_adm_parse` on its own, same daily schedule and
  `workflow_dispatch`. Its own job because it is the only harness here not
  built by default: it needs vcpkg's `adm` feature plus the `FetchContent`
  pulls of libbw64 and libadm, which nothing else in this file touches. See
  "The ADM harness is opt-in" above.
- `encoder-space-nightly` - the encoder input-space search above, on the same
  daily schedule and `workflow_dispatch`, with a 15-minute default budget.
  Shares none of the machinery of the other five (no libFuzzer, no sanitizer
  runtime, not in `fuzz/run.sh`): it builds the plain `linux-llvm` CLI with
  vcpkg and a pinned `ffmpeg`, the way `ci.yml`'s `ffmpeg-validate` job does.

The bounded per-PR counterpart to `encoder-space-nightly` is a step in
`ci.yml`'s `ffmpeg-validate` job (~2 minutes, plus the envelope check), not a
job in this file - everything it needs is already built and pinned there, and
unlike `fuzz-short` it runs on pull requests rather than push only, because it
is cheap enough relative to the job it rides on.

`fuzz-short`, `fuzz-differential`, `fuzz-nightly` and `fuzz-adm-nightly` run
with `continue-on-error: true`, the same convention `ci.yml` uses for its
other unproven legs: none has multiple clean fuzzing runs behind it yet. That is a
question of track record, and it is not settled by the build being
warnings-clean - a bounded mutation run can still surface something on any
given night. `fuzz-regress` is cheap enough to make a required
branch-protection check once it has proven itself - that is a repository
setting this file cannot declare on its own.

This is a bounded, time-boxed run, not continuous (OSS-Fuzz-style) fuzzing
infrastructure. That is a deliberate scope decision, not a limitation
somebody forgot to lift.
