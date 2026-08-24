# Threat model

What this project assumes about the bytes it is handed, what it guarantees when those bytes are
hostile, and what it does not. Written for someone deciding whether to link the decoder into a
media server, a set-top box or a browser and point it at input from the internet.

Nothing here is a promise of invulnerability. It is a statement of posture — where the trust
boundary sits, what is checked, what is only structurally bounded, and where the gaps are — so
that an embedder can reason about the remaining risk instead of guessing at it.

The reporting process is in [SECURITY.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/SECURITY.md);
[Reporting an issue](#reporting-an-issue) below adds what an embedder specifically should send.

## Trust boundary

**Untrusted.** Everything on this list is treated as adversary-controlled. Each has a parser in
this repository that must not crash, read out of bounds, or loop unboundedly on any input:

| Input | Entry point | Fuzzed |
|---|---|---|
| AC-3 elementary streams | `ac3::split_frames`, `ac3::FrameDecoder::decode_frame` | yes |
| E-AC-3 elementary streams, including dependent substreams | `ac3::split_access_units`, `ac3::Eac3Decoder::decode_access_unit` | yes |
| Format sniffing before any decoder commits | `ac3::io::scan` | yes |
| EMDF containers in a skip field (§H.2.2) | `ac3::emdf::parse_container` | reached through the E-AC-3 harnesses |
| OAMD object metadata (TS 103 420 §5.5) | `ac3::oba::parse_payload` | reached through the E-AC-3 harnesses |
| JOC payloads (TS 103 420 §6) | `ac3::joc::parse_payload` | reached through the E-AC-3 harnesses |
| WAV / RIFF headers and PCM | `ac3::io::read_wav`, `ac3::io::WavStreamReader` | yes |
| ADM XML + BW64/RF64 (opt-in build) | `ac3adm::parse_bw64`, via vendored libadm/libbw64 | **no** — see [ADM](#adm-xml-and-bw64) |
| Object authenticity tags | `ac3::signing::verify_atmos_frame` | no |
| Matroska/WebM containers | `matroska::demux`, `matroska::Reader` | yes |
| MP4/ISOBMFF containers | `mp4::demux`, `mp4::Reader` | yes |

**Trusted.** These are the caller's own inputs, and a caller that gets them wrong is a bug in the
caller, not an attack:

- Encoder configuration (`EncoderConfig`, `eac3::FrameConfig`, `AtmosEncoder` settings, the
  `ac3cli` command line). Illegal combinations are rejected with a `FrameError`, but the values
  are not assumed hostile.
- PCM handed to the encoder. Any float is legal audio; nothing about it can reach a decision the
  bitstream syntax does not already bound.
- The signing key, if an operator supplies one. It is never generated, logged or written to disk
  by this code (`ac3::signing::SigningKey` zeroizes on destruction).
- File paths, output destinations, and the caller-owned spans the `_into` decode forms write
  through — see [Raw-pointer boundaries](#raw-pointer-boundaries).

**Not a boundary this project defends.** MPEG-TS. `mpegts::Writer` is a muxer only — it turns
access units already in hand into bytes. There is no MPEG-TS demuxer, so an embedder demuxing
MPEG-TS is trusting *its own* demuxer, not this one. Matroska/WebM and MP4/ISOBMFF moved off this
list once their readers landed (`matroska::demux`/`Reader`, `mp4::demux`/`Reader`, roadmap `IO2`);
both are in the untrusted table above.

## Memory-safety posture

The codec core is C++23 with no third-party dependencies. It is not a memory-safe language, so
the posture is a set of specific properties rather than a language guarantee:

- **Bit reading cannot run off the end.** `ac3::BitReader` is the single reader for every
  bitstream in the project. Reading past the end sets a sticky `overflowed()` flag and yields
  zeros rather than touching memory; parsers check the flag at a frame or payload boundary
  instead of guarding each read. A truncated or hostile frame therefore decays into a
  `DecodeError::kTruncated` rather than an over-read.
- **Every fallible path returns a value, not an exception.** `std::expected<T, DecodeError>`
  throughout. The codec core does not throw; the only exceptions that can escape it are
  `std::bad_alloc` from an allocation it makes.
- **No owning raw pointers, no manual `new`/`delete`, no C string handling** in the codec core.
  Buffers are `std::vector`; borrowed views are `std::span` and `std::string_view`.
- **Indexed access is `std::span` and `std::vector`, which are bounds-checked only where the
  standard library's own assertions are on** — MSVC's `_STL_VERIFY` in a debug build, and
  libstdc++/libc++ only under `_GLIBCXX_ASSERTIONS`/`_LIBCPP_HARDENING_MODE`, neither of which
  this project sets. Of the CI legs only the ASan + UBSan one is a debug build, and that is also
  the leg that runs the codec matrix, so an out-of-range index there fails the job. Every other
  leg, and every shipped package, is a release build with no such net — which is why the fuzzers
  run under ASan rather than relying on the library's own checks. (The WAV over-read fixed
  alongside this document is exactly that story: caught by `_STL_VERIFY` in a debug build,
  invisible in a release one.)

What runs against it, continuously:

- **Six libFuzzer harnesses** under [`fuzz/`](https://github.com/iainchesworthlabs/ac3forge/blob/main/fuzz/README.md),
  built with ASan + UBSan and `-fno-sanitize-recover=all`. Four drive the decode and parse entry
  points for crashes and undefined behaviour; two more decode the same mutated bytes with FFmpeg
  as well and diff the PCM, so a *wrong* decode that does not crash is caught too. `Fuzz Regress`
  replays the checked-in seed and regression corpora on every push to `develop`/`main` and every
  pull request into them; `Fuzz Short` and `Fuzz Differential` add a bounded mutation budget on
  pushes, and a nightly job goes deeper.
- **An ASan + UBSan CI leg** that runs the full test suite and `tools/ci/run_codec_matrix.sh` —
  every layout, every Annex E tool token, both Atmos container modes, the metadata options —
  so the sanitizers see the real command paths rather than only unit tests.
- **CodeQL** on the `security-and-quality` suite, **MSVC PREfast** with a warnings gate, **OSV
  scanning** and **OpenSSF Scorecard**.

What is *not* covered: there is no ThreadSanitizer leg. The codec core is single-threaded and
holds no shared state, but the audio layer's lock-free SPSC ring, silence watchdog and drift
servo are shared between a real-time callback thread and an encoder thread, and neither ASan nor
UBSan can see a race there. That is roadmap VX16, and it is an open gap.

Known history in this class: one real bug of exactly this shape has been found and fixed (commit
`8386c8f` — a decoder that shifted by an unvalidated exponent and reached undefined behaviour on
a malformed differential exponent chain). It was found by a manual adversarial audit; the fuzzers
exist because that audit should not have been the thing that found it.

## Raw-pointer boundaries

Three surfaces cross out of C++ into a caller that the type system cannot help. Each has a
contract the caller must keep; none of them validate it in a release build.

### The C API (`ac3forge_c/ac3forge.h`)

The safest of the three, by design. Every handle is opaque, every fallible call returns
`ac3forge_status_t`, and **nothing is returned through a caller-supplied buffer** — a
variable-length result is an owned handle read through accessors, so no caller has to predict a
size. Every entry point is wrapped in a `noexcept` guard that turns `std::bad_alloc` into
`AC3FORGE_ERROR_OUT_OF_MEMORY` and anything else into `AC3FORGE_ERROR_INTERNAL`, because letting
a C++ exception unwind into a C (or Rust, or Python) frame is undefined behaviour.

What the caller still owns:

- **Lifetime.** Every `_create` needs its `_destroy`. Passing `NULL` to a `_destroy` is a no-op,
  matching `free()`.
- **The `const uint8_t* frame` / `size_t frame_size` pair** passed to
  `ac3forge_decoder_decode_frame`. A size larger than the buffer is a caller bug this layer
  cannot detect; the buffer must be valid for the whole call.
- **Accessor indices.** `ac3forge_decoded_frame_channel_samples(frame, channel_index)` and
  friends take an index the caller is expected to have read from
  `ac3forge_decoded_frame_channel_count` first.
- **No ABI promise before v1.0.** A newer ac3forge may need a recompile, not merely a relink.

### The `_into` decode forms

`FrameDecoder::decode_frame_into` and `Eac3Decoder::decode_access_unit_into` write PCM into
caller-owned planar spans instead of allocating. The span count and each span's length are
checked by `assert` — which means they are checked in a debug build and **not at all in a
release build**, where an undersized span is a buffer overflow in the caller's memory. The
contract is on both functions' doc comments: one span per channel the frame codes (six covers
every AC-3 layout, sixteen covers §E3.8.2's cap for E-AC-3), each holding `kSamplesPerFrame`
floats. Use the allocating forms if the sizes are not statically obvious.

### The WASM bindings and the JNI bridge

The WASM decoder (`apps/wasm/decoder_bindings.cpp`) copies the JavaScript byte array into a
`std::vector` before parsing, so the decode itself never reads JS-owned memory. What it hands
*back* is a zero-copy `typed_memory_view` into the decoder instance's own buffers: those views
are invalidated by the next `decode()` call or by the instance's destruction, and JavaScript
holding one past that point reads freed WASM heap. The module is built with
`ALLOW_MEMORY_GROWTH` under a 1 GiB `MAXIMUM_MEMORY` ceiling, which turns heap exhaustion into a
catchable `std::bad_alloc` and a readable refusal instead of a dead tab.

The Android JNI bridge (`apps/android/app/src/main/cpp/`) is demo-app scope: it returns strings
through `NewStringUTF` and does not take byte arrays across the boundary, so it has no
`GetByteArrayElements`-style pinned-buffer contract to get wrong. It is not part of the library's
supported surface.

## Resource limits

The important structural property is that **every per-access-unit allocation is bounded by a
bitstream field of fixed width**, so no single frame can be made to consume an unbounded amount
of memory or time. Decode cost is linear in the number of access units, with a bounded cost per
unit; there is no super-linear amplification and no recursive descent anywhere in the parsers.

Enforced limits, and where they come from:

| Quantity | Limit | Enforced by |
|---|---|---|
| AC-3 syncframe | 3,840 bytes | Table 5.18 (`frmsizecod` indexes a fixed table; 38–63 rejected as reserved) |
| E-AC-3 syncframe | 4,096 bytes | `frmsiz` is 11 bits; `(frmsiz + 1) * 2` |
| E-AC-3 syncframe, lower bound | 6 bytes | explicit refusal: a `frmsiz` that does not cover the header it was read from is `kInvalidStream`, not a short span someone reads past |
| Coded channels per substream | 6 (5 full-bandwidth + LFE) | `acmod`/`lfeon` are 3 + 1 bits |
| Rendered channels per access unit | 16 | §E3.8.2 |
| Substream identities held simultaneously | 32 | `strmtyp` (2 bits) × `substreamid` (3 bits); a flat 32-slot array, states lazily allocated |
| Overlap-add state per identity | 12 KiB | 6 channels × 256 doubles |
| Samples per access unit | 1,536 per channel | `kSamplesPerFrame` |
| PCM per access unit | 96 KiB | 16 channels × 1,536 × `sizeof(float)` |
| EMDF skip field | 511 bytes | `skipl` is 9 bits |
| EMDF payload | bounded against the bits actually left before allocating | explicit check in `parse_container` — a `variable_bits` size field could otherwise claim gigabytes from a few dozen bits of garbage |
| `addbsi` | 64 bytes | `addbsil` is 6 bits |
| OAMD objects | 32 | `object_count_bits` is 5 bits; §5.5.2's escape for larger counts (`0x1F` plus a 7-bit extension) is refused rather than implemented |
| JOC objects | 16 | `kMaxObjects`, checked explicitly; TS 103 420 §8.3.2.2's own cap |
| WASM demo heap | 1 GiB | `MAXIMUM_MEMORY`, with `std::bad_alloc` caught and reported |

### A hostile `frmsiz`

Worth spelling out because it is the field an attacker reaches for first. `frmsiz` says how long
an E-AC-3 syncframe is, and callers index into the spans the splitter hands back:

- A `frmsiz` **larger than the bytes remaining** is `DecodeError::kTruncated`. The splitter never
  returns a span that overruns the input.
- A `frmsiz` **smaller than the six header bytes already read** is `DecodeError::kInvalidStream` —
  refused outright rather than becoming a short span a later parser reads past.
- A `frmsiz` that is **merely wrong** — self-consistent, but not where the next syncframe
  actually is — desynchronises the split, and the following frame fails its sync-word check.
  Nothing reads outside the input span in the meantime.

The AC-3 equivalent is `frmsizecod`, which indexes a fixed table rather than carrying a length:
values 38–63 are reserved and rejected, and `fscod == 3` is rejected, so the frame size is always
one of 38 tabulated values.

### Open gaps

**No cap on stream length, and no streaming split.** `ac3::io::scan`, `ac3::split_frames` and
`ac3::split_access_units` take the whole elementary stream as one `std::span` and return one span
per access unit. Peak memory is therefore *O(input size)* — the bytes themselves plus roughly 16
bytes of index per access unit — and there is no limit at which the library refuses. `ac3cli`
inherits this: it reads the encoded input fully into memory (it streams the *decoded* PCM out, so
output is O(1)).

This is deliberate rather than overlooked: what counts as "too large" is the embedder's policy,
not the library's, and a hard cap in the library would break legitimate long-file use. The
mitigation is on the caller:

- Bound the input before calling. A length limit, a container-level packet budget, or a memory
  cgroup are all more appropriate than a constant compiled into a codec.
- Or drive the per-frame API directly. `FrameDecoder::decode_frame` and
  `Eac3Decoder::decode_substream`/`decode_access_unit` each take one unit at a time, and the
  `_into` forms write into caller-owned storage, so a caller that delimits units itself never
  needs the whole stream resident.

**No decode time bound.** Nothing in the library measures or limits wall-clock time. Because
per-unit cost is bounded and the parsers do not recurse or backtrack, total decode time is linear
in input length — there is no input that makes a *single* frame slow. Watchdog the call if the
threat is a peer feeding an endless stream; there is nothing to configure here.

**Worst-case expansion ratio.** A minimum-size access unit is 6 bytes and a maximum-size one
produces 96 KiB of PCM, so the structural upper bound on decoded-bytes-per-input-byte is roughly
16,000:1. That is an upper bound from field widths, not a measured achievable figure — a frame
short enough to hit it does not carry enough bits to code channels at all, and overflows the bit
reader into `kTruncated` first. Bound decoded *output*, not input bytes, if this matters.

**A WAV file is read whole.** `ac3::io::read_wav` reads its entire source into memory before
parsing, so memory is O(file). `ac3::io::WavStreamReader` is the block-at-a-time alternative and
reads only a fixed header window (a `data` chunk beyond that window is refused rather than
searched for). A WAV may declare up to 65,535 channels; the per-channel vector overhead that
implies (~1.5 MB) is not proportional to the file that declared it, though the sample data itself
is still clamped to the bytes actually present.

### ADM XML and BW64

`atmos-adm` and `ac3adm::` are **off by default** (`-DAC3FORGE_BUILD_ADM=ON`) and, unlike every
other parser here, are not this project's own code: the XML and BW64/RF64 reading is vendored
libadm and libbw64, plus Boost headers. That means:

- **No fuzz harness covers this path**, and the resource limits above do not apply to it. There
  is no document-size cap, no entity-expansion limit and no element-count limit; an enormous or
  deeply nested ADM document is bounded by nothing this project controls.
- The whole `axml` chunk is materialised as a string and re-parsed from an `istringstream`, so
  memory is O(document).
- Parse and graph-resolution failures do surface as real diagnostics (`ac3adm::AdmError`,
  `ac3::admbridge::BridgeError`, each with its own `describe()`) rather than a crash or a bare
  non-zero exit.

**Do not enable the ADM build for untrusted input.** It exists to ingest professional master
files an operator already trusts. Extending the threat model to cover it means fuzzing the
vendored parsers and deciding a document-size policy; neither has been done.

### Object signing is authentication, not integrity of the stream

`ac3::signing::verify_atmos_frame`/`verify_atmos_stream` check an HMAC tag over the EMDF object
container. This tells you the object metadata came from someone holding the key. It does **not**
authenticate the audio, the bed, or anything outside the container, and a stream with no
container at all has nothing to verify — see
[Object signing](concepts/object-signing.md). Verification is opt-in
(`ac3cli decode ... verify-objects`); a signed-but-unchecked stream decodes exactly like an
unsigned one. This project ships no key.

## What a decode failure looks like

Every decode entry point returns `std::expected<..., DecodeError>` and every error is one of six
values: `kTruncated`, `kBadSyncWord`, `kBadCrc`, `kReservedValue`, `kUnsupported` (legal syntax
this decoder declines to read) or `kInvalidStream`. `ac3::describe()` turns each into a sentence.
There is no concealment mode: a frame that fails produces no audio, and a caller that wants
concealment implements it above this layer.

A refusal is not a rollback, though. The decoder's own state — overlap-add delay, the §7.3.4
dither generator, JOC reconstruction — advances block by block as the frame is parsed, and a
frame refused part-way through has already advanced it as far as it got. On an `_into` form the
caller's spans are left in an unspecified state for the same reason. Neither is a memory-safety
problem (nothing is read or written outside its own buffer), but a caller that needs a clean
state after an error should construct a fresh decoder rather than continue with the one that
refused.

CRC is checked, on both generations: AC-3 checks `crc1` over the first 5/8 of the frame and
`crc2` over the whole of it, E-AC-3 checks its single CRC over everything past the sync word.
`kBadCrc` is a real refusal, not a warning.

## Reporting an issue

Use [GitHub Security Advisories](https://github.com/iainchesworthlabs/ac3forge/security/advisories/new),
privately — the process and timelines are in
[SECURITY.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/SECURITY.md).

If you found it while embedding this library, the two things that speed up a fix most are:

1. **The input.** A stream, WAV or ADM file that reproduces it, however small. If it came out of
   a fuzzer, the raw corpus file is ideal — it drops straight into
   `fuzz/regressions/<harness>/` as a permanent regression case.
2. **Which entry point you called**, and whether you were using an allocating decode form or an
   `_into` form with your own spans. Those have different contracts and a report that does not
   say which one is in play can take a while to place.

A sanitizer report (ASan/UBSan stack) is worth more than a description of the symptom, and the
build that produced it (`ac3cli --version` prints version, commit and toolchain) says whether
what you hit is already fixed.

## See also

- [Validation](verification.md) — how output correctness is checked, and where the oracles run out
- [Conformance vectors](conformance-vectors.md) — the published stream set, and what it does and does not prove
- [`fuzz/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/fuzz/README.md) — the harnesses, the differential oracle and its agreement floor
- [Decoding](library/decoding.md) — the decode API this page describes the boundaries of
- [C API](library/c-api.md) — the ownership and error conventions in full
