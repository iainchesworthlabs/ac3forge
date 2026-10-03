# AC-4: a decoder, an encoder and the applications

!!! note "Status as of 2026-09-30: built except I6, D14c and D14d; N1 has been built since, on 2026-10-01"
    The decoder (D1 to D11), the encoder (E1 to E10), the application phases (I1 to I5b), the golden
    masters (G0, G1) and the first two parts of D14, the ESP32 work (D14a on the host, D14b on the
    ESP32-P4), are merged; [State on 2026-09-30](#state-on-2026-09-30) lists each phase with its pull
    request. Not built: the ESP32 sinks taking AC-4 in a Sendspin group (I6) and the S3 and C6 parts
    of D14 (D14c, D14d). N1, the program names and the layout of `src/`, was built after that date:
    [N1](#n1-the-names) says what ran, in which pull requests, and what is left. The design
    sections below say what was proposed and why, and where the built code differs from a sketch
    they say so, and they keep the names of the time they were written (`ac3cli`, `ac3::forge`,
    `ac3forge`: [Renamed](../docs/renamed.md) puts each beside its new name).

    Written on 2026-09-15 as phase D0 of chip D in the Hearth plan, as a plan for a decoder.
    Extended on 2026-09-24, when the user widened the scope to a decoder, an encoder and their
    integration into the applications: [What encoding AC-4 involves](#what-encoding-ac-4-involves),
    the encoder's design, [The oracles](#the-oracles), the encoder's ladder, the
    [encoder phases](#encoder-phases), the [application phases](#application-phases) and
    decisions 13 to 23 were added then. The page was kept on a local branch, `feature/ac4-decoder-plan`
    ([decision 11](#decisions)), until the user moved it to main as `planning/ac4.md`
    ([decision 22](#decisions-for-the-encoder-and-the-applications), #1003).

    D1, the library and the channel-coded syntax, merged as #700 on 2026-09-16, with the readings
    settled in #712 and #715. The inspector gained `oamd_common_data()` and the HSF extension's
    index in #739 and #744, and the decoder the HSF extension's content in #786.

    The twelve decisions under [Decisions](#decisions) were put to the user on 2026-09-15 and
    answered the same day. Five answers went against the recommendation and two were given in the
    user's own words; both kinds are marked. The phases followed the answers: D1 to D10 were approved,
    D11 waited only for IEC 61937-14:2017, 61937-1:2021 and 61937-2:2026, which the user supplied on
    2026-09-15, and D12 and D13, the `float` and fixed-point tiers, were to be confirmed with the user
    when D10 landed (D14 took their work on 2026-09-25). Decisions 13 to 23, which the wider scope
    raised, were put to the user on 2026-09-24 and answered the same day: the four put as questions
    (13, 14, 20 and 22) took the recommendation, and the recommendations stated for the rest were
    taken without objection.

    On 2026-09-25, with D2 to D5 and E1 to E4 merged, the user asked for the remaining phases to be
    built in parallel, D11 and the ESP32 phases included, and answered four more questions: DEE's
    licence will not be renewed, so G1 made the golden masters the remaining phases needed before it
    ended ([decision 23](#decisions-for-the-encoder-and-the-applications)); the ESP32-P4, the family's
    part with the most CPU and memory, is AC-4's first ESP32 target, with the S3 and the C6 to follow
    as the decoder is optimised, which [D14](#d14-ac-4-on-the-esp32s) plans in place of D12 and D13;
    AC-4 follows AC-3's and E-AC-3's arithmetic, `double`, `float` and fixed point by target; and the
    programs named `ac3` are renamed, since `ac3cli` doing AC-4 reads wrongly. The family's name and
    the library's identifiers were to stay; on 2026-09-29 the user widened the ask to the whole tree,
    which [N1](#n1-the-names) records. Decisions 24 to 35, under
    [Decisions of 2026-09-25](#decisions-of-2026-09-25), record the 2026-09-25 answers, and decisions
    36 to 39, under [Decisions of 2026-09-29](#decisions-of-2026-09-29), those of 2026-09-29.

    Shape follows the Hearth plan: design sections say what is proposed and why, each phase carries
    an exit criterion and how it is verified, [Decisions](#decisions) gives the options with a
    recommendation and the cost of each, and
    [What cannot be verified, and why](#what-cannot-be-verified-and-why) says where the evidence
    stops. The reading behind the decoder (both specifications, a census of 100 DEE encodes, the
    public record on conformance material, IEC 61937 and other decoders) was done on 2026-09-15; the
    reading behind the encoder and the applications (the specifications again, the tools installed
    here, the AC-3 and E-AC-3 validation machinery and each application's code) on 2026-09-24.

## State on 2026-09-30

Each phase with its pull request and where it stands. Dates are UTC, as GitHub records a merge.
"Exit met" is as the phase's pull request reports it, and any exit criterion not met in the pull
request is named in the last column; the phase's own section below says what it built and how it was
checked. The table is of `main` at `5ef9eeafc`.

| Phase | Builds | Pull request | State |
|---|---|---|---|
| G0 | the gold set: DEE streams that can be scored against their sources | #1006, 2026-09-25 | merged; exit met |
| G1 | golden masters for the phases still to come, before DEE's licence ends | #1051, 2026-09-25 | merged; exit met. G2 (#1054, 2026-09-26) made the same for DEE's AC-3, E-AC-3, E-AC-3 JOC and TrueHD encoders |
| D1 | the decoder library and the channel-coded syntax | #700, 2026-09-16 (readings #712, #715; #739, #744; HSF content #786) | merged; exit met |
| D2 | waveform-coded stereo to PCM; the shared core | #1009, 2026-09-25 | merged; exit met |
| D3 | the QMF domain and A-SPX | #1012, 2026-09-25 | merged; exit met |
| D4 | the 5.X element (and 3.0, 7.X) | #1014, 2026-09-25 | merged; exit met |
| D5 | A-CPL | #1027, 2026-09-25 | merged; exit met |
| D6 | output processing: every frame rate, DRC, dialogue enhancement, downmix, concealment | #1037, 2026-09-25 | merged; exit met |
| D7 | presentations | #1055, 2026-09-26 | merged; exit met |
| D8 | the decoder's API, CLI options, media information, packaging | #1059, 2026-09-26 | merged; exit met |
| D9 | channel-based immersive, in full and core decoding | #1057, 2026-09-26 | merged; exit met, the delivery kit's 5.1.4 streams checked in #1069 |
| D10 | A-JOC and direct-coded objects | #1060, 2026-09-26 | merged; exit met but the listening, which is the user's; the DEE criterion did not apply |
| D11 | AC-4 over IEC 61937 | #1052, 2026-09-26 | merged; exit met; no device here accepts AC-4 |
| D12, D13 | the `float` and fixed-point tiers | | folded into D14 |
| D14a | the scalar and the decoder's size, on the host | #1096, #1102, #1123, 2026-09-29 | merged; exit met |
| D14b | AC-4 on the ESP32-P4 | #1118, 2026-09-29 | merged; the P4 decodes 2.0 in real time and nothing wider; decision 26's identical output does not hold on the five plays with companding |
| D14a4 | libm parity and the frame-rate converter at `float` | open, 2026-09-30 | exit met: the `float` PCM equal on the host, the Cortex-M3 leg and the P4 for D14b's twenty plays; the converter takes 7.3 ms a frame at 24 and 25 fps, and 1001/960 still has a 5.9 s first frame |
| D14a5 | the converter's `float` tables at compile time | open, 2026-10-01 | exit met: the P4's first frame at 1001/960 takes 0.31 s from 5.9 s and the converter 16.5 ms a frame at 23.976 fps from 22.4; no `float` pin moved and the `double` output is byte-identical |
| D14a6 | the P4's low-power SRAM out of the heap | open, 2026-10-02 | exit met: the 29.97 fps play's converter takes 12.0 ms a frame under ESP-IDF's default allocation policy from 93.8, the play 1.00 of real time from 3.51, and every other play 3 to 24% less time a frame; no `float` pin moved |
| D14c | the S3 | | not built |
| D14d | the C6, fixed point | open, 2026-10-02 | the host and QEMU part's exit met: the fixed decode within 105.7 to 132.1 dB of `double` below A-SPX's crossover and 34.2 to 97.2 above, the probe's hashes equal on x86-64, the Cortex-M3 and RV32IMC, `double` and `float` unchanged; nothing fits a C6 beside WiFi; the board not yet run |
| D14f | the decoder's memory | open, 2026-10-03 | measured: 2.0 peaks at 286,365 bytes on a 32-bit core from 429,667 (fixed) and 413,611 (`float`), 5.1 at 704,311, 5.1.4 at 1,502,903; every PCM pin unmoved and `double` byte-identical to `main` |
| E1 | the encoder library, the frame writer, SIMPLE mono and stereo | #1011, 2026-09-25 | merged; exit met |
| E2 | A-SPX and companding | #1013, 2026-09-25 | merged; exit met |
| E3 | the 5.X element | #1025, 2026-09-25 | merged; exit met |
| E4 | A-CPL | #1029, 2026-09-25 | merged; exit met |
| E5 | metadata, frame rates, I-frames | #1046, 2026-09-26 | merged; exit met |
| E6 | presentations and several substreams | #1058, 2026-09-26 | merged; exit met |
| E7 | the encoder's API, CLI options, packaging | #1061, 2026-09-26 | merged; exit met |
| E8 | channel-based immersive | #1071, 2026-09-26 | merged; exit met; the sweep legs' gap to DEE was closed by E10 |
| E9 | A-JOC and direct-coded objects | #1082, 2026-09-29 | merged; exit met, MediaInfo's object count and bed run afterwards (#1103); the listening is the user's; no race, since DEE refuses the masters |
| E10 | A-SPX noise floors on sweeps | #1113, 2026-09-29 | merged; exit met |
| I1 | the rest of `ac3cli` | #1070, 2026-09-26 | merged; exit met |
| I2 | Hearth desktop | #1068, 2026-09-26 | merged; exit met |
| I3 | Forge GUI | #1084, 2026-09-29 | merged; exit met |
| I4 | the C API, Python, Rust, WebAssembly | #1094, 2026-09-29 | merged; exit met; the WebAssembly module's C++ side and the Android NDK build are checked by CI alone |
| I4b | the object encoder in those | #1119, 2026-09-29 | merged; exit met; no WebAssembly demo page |
| I5 | immersive and object content in the applications | #1100, 2026-09-29 | merged; exit met, apart from two failures in suites it did not touch (fixed since) |
| I5b | the encoder page's AC-4 objects | #1117, 2026-09-29 | merged; exit met, with the ADM master's audio as the page's source and its scene authored there: the page has no ADM or IAB reader |
| I6 | the ESP32 sinks | | not built |
| N1 | the program names and the layout of `src/` | study #1122; S2 #1160; S3 #1161; S4 #1162; N1A #1164; S5; S6 | built in the repository, 2026-09-30 and 2026-10-01; the owner's renames and U1 are left (see N1); the PyPI project and the Homebrew names are decided (40, 41) and the open vcpkg pull request is not |

Not built, beside those: a quality series for the encoder (the decoder's has one, `ac4-quality-main.jsonl`;
the encoder's scores and the race against DEE are held to pinned floors and keep no history), Pro
Logic II from Hearth's Decoder page, an AC-4 live session in the Forge GUI, and AC-4 in Matroska,
which registers no codec ID for it. AC-4's speed and memory are measured and gated: `ac3perf` holds
AC-4's stereo and 5.1 encode and decode to real time in the gate's build legs, the speed, kernel and
memory benchmarks carry AC-4's rows (and the transforms of `src/ac4core`), and since #1132
(2026-09-29) a merge queue entry that changes `src/` fails if any workload, AC-4's included, takes
twice as long as at the commit it is queued on, or doubles its heap churn.

Left to the user, each with its options in the pull request or section named: N1's steps outside
the repository and what to do with the open vcpkg pull request ([N1](#n1-the-names)); D14b's four, of which the libm change and the frame-rate converter were taken on
2026-09-30 and are D14a4, the allocation policy was taken on 2026-10-01 and is D14a6, and what comes next on the P4 is
still open (#1118);
D14a4's, of which the 1001/960 table was taken on 2026-09-30 and is D14a5;
I5's two, whether to spend a check on the `zone_mask` reading and a native
check of the Arabic, Hebrew and Yiddish strings (#1100); I5b's four, which are an ADM or IAB master
as a source on the encoder page, static beds for channels assigned to speakers, a Preview from a
decode of the AC-4 stream and AC-4 in Guided's Movement step (#1117); and the listening for D10 and
E9.

## What is asked

The Hearth desktop application was designed with AC-4 pages that stay disabled until a decoder
exists: presentation selection, main and associated mixing, dialogue enhancement, DRC and downmix.
This page first planned that decoder. On 2026-09-24 the user asked for more: a feature-complete
decoder producing PCM, a feature-complete encoder, and both in the applications once the library is
complete, validated the way AC-3 and E-AC-3 are, with DEE's encodes as the gold standard and a
second decoder checking what the encoder writes. The limits the decisions set stand, such as the
speech frontend waiting for a stream that uses it, and so does
[Deliberately not in scope](#deliberately-not-in-scope). Both are to be:

- written clean-room from ETSI TS 103 190-1 V1.4.1 and ETSI TS 103 190-2 V1.3.1 (both 2025-07),
  which ETSI publishes free with their table attachments;
- libraries beside `src/ac4` that do not depend on `ac3::forge`, as the inspector does not;
- free of FFmpeg and every other codec library, with other codecs used only as separate programs
  whose output is compared, and never read;
- callable from the applications through APIs that mirror `ac3::forge`'s where the concepts match,
  so an application can show one control for both formats;
- verified without a decode reference, by the means [Verification](#verification) sets out.

The user has cleared the patent position. It is recorded here and does not gate any phase.

## Where things stand

The inspector and the applications are described as they are on 2026-09-30. The decoder after D1
is the position the decoder phases started from, and the specifications, the material and what DEE
writes are what the phases worked from, with what the phases added to them.

### The inspector

`src/ac4` (`ac4::ac4`, roadmap IM4, PR #442) reads sync frames and their CRC (Annex G), the table
of contents, presentation information v0 and v1, substream group information for channel-coded,
A-JOC, direct-coded object and OAMD substreams, and the substream index table. It builds `dac4`,
reports samples per frame and writes the RFC 6381 codec string. It reports `audio_data()` and
`metadata()` as byte ranges. Since #739 it reads the `oamd_common_data()` an
`ac4_substream_info_ajoc()` embeds, and since #744 each substream's `hsf_ext_substream_index`. The
decoder phases added to it, and what the decoder and the encoder meet in it now:

- The library is installed and exported (D8): `ac4::ac4_static` and `ac4::ac4_shared`, and
  pkg-config's `ac4`, with the decoder and the encoder in one export set
  (`cmake/InstallLibrary.cmake`).
- `ac4::scan` stops at a partial frame, and `ac4::SyncFrameSplitter` holds one between reads for
  input that arrives in pieces, in the pattern of `io::AccessUnitAccumulator` (D8).
- `ac3cli probe` writes `presentations_v1` and the selected presentation's metadata beside
  `presentations_v0` (D8), and the `oamd_common_data()` of an A-JOC substream and of a group's own
  OAMD substream (I5, #1114). Every stream DEE writes has v1 presentations.
- `ac4::build_dac4()` and `ac4::rfc6381_codec_string()` take an `ac4::Toc`, so they describe a
  stream the encoder writes as well as one the inspector reads. `ac4::dac4_refusal()` and
  `ac4::cmaf_refusal()` name what a `dac4` box or a CMAF track cannot carry (E7).
- The ESP-IDF component leaves `AC3FORGE_BUILD_AC4` off, since that option also builds the
  encoder, the applications and the tests, and the root `CMakeLists.txt` refuses it in the minimal
  profile. `CONFIG_AC3FORGE_AC4` (D14b) builds the inspector, the core and the decoder into the
  component through `AC3FORGE_MINIMAL_AC4`.
- The two defects found while planning are fixed: `fuzz_ac4_parse` links an instrumented
  inspector (`fuzz/CMakeLists.txt`), and an EMDF-only presentation reads its EMDF substream
  list on both table-of-contents paths, checked by frames built in both languages.

The Python reference parser `tools/references/ac4_parse.py` reads the same framing, transcribed
independently; CI runs it through `ac4_syntax.py`'s digest test. `tests/ac4/test_ac4.cpp` checks one
committed DEE stream, `tests/golden/external-baseline/ac4-stereo-64/dee.ac4`, against MediaInfo's
reading of it; fifteen more committed DEE streams sit beside it (G0 and G1 added the later ones).

### The decoder after D1

The position when D1 merged (2026-09-16), which the decoder phases started from. `src/ac4dec`
(`ac4::decoder`) read every syntax element of a channel-coded frame and produced no audio: the
presentation substream, the Part 1 channel elements with ASF, stereo processing, companding, A-SPX
and A-CPL data, `metadata()` with DRC and dialogue enhancement, EMDF payload substreams, and a
channel-coded substream's HSF extension (#786). It refused, by name, the speech frontend, the
immersive and 22.2 elements, object substreams, and the efficient high frame rate mode. Since then
D2 to D10 made it decode all of them but the speech frontend, the 9.X.4 and 22.2 layouts and the
efficient high frame rate mode, which it still refuses; the header
`src/ac4dec/include/iclforge/ac4dec/decoder.hpp` says what it decodes and what it refuses.
`src/ac4dec/ERRATA.md` records every reading taken where the text is ambiguous or defective, with
its evidence.

The syntax is transcribed twice, in C++ and in `tools/references/ac4_syntax.py`, and the two traces
must agree record for record: `tests/golden/ac4dec/*.tsv` holds the Python parser's digests of the
committed streams, `tests/ac4dec/test_ac4dec_syntax.cpp` holds the decoder to them in `ac3tests`,
which every build leg runs, and `tools/checks/test_ac4_syntax_digests.py` holds the Python parser
to the same files. Over the local census (107 DEE streams, 50,728 frames) and 6,670 frames of
public streams from other Dolby encoders, every digest agreed when D1 merged.
`tools/checks/ac4_syntax_differential.py` compares the two on mutated, synthetic and fuzzed
streams; it runs nightly in the SonarCloud workflow, and does not gate. `fuzz_ac4_decode` is
instrumented and has regression inputs.

Five contract items from the review of #700 were open after D1, and D8 closed them: `DecoderConfig`
stored a `SyntaxSink` that its comment gave a call-scoped lifetime, so a temporary lambda dangled
once the decoder copied the configuration; an ASF Huffman miss reported `kInvalidStream` where
A-SPX, A-CPL and metadata reported `kTruncated` for the same failure; an HSF extension substream
that nothing claimed got no report, although the header said HSF was refused; there was no
`tools/ci/abi-allowlist/libiclforge_ac4dec.so.txt`; and the Android, WASM and Python wheel configurations
compiled both AC-4 libraries without linking them, since `AC3FORGE_BUILD_AC4` is on by default and
their targets are not `EXCLUDE_FROM_ALL`.

### The applications today

Status board: [ROADMAP.md](../ROADMAP.md). This section records what is on `main` on 2026-09-30;
phase numbers below are the plan's, not the roadmap's.

**`ac3cli` (I1, I5).** Reads and writes AC-4 in `probe`, `decode`, `transcode`, `monitor`, `play`,
`qc`, `levels`, `loudness`, `spdif`, `unspdif`, `record`, `live`, `mp4`, `ts`, `fmp4` and `demux`;
`ac4-encode` writes mono, stereo, 5.0, 5.1, 5.0.4 and 5.1.4; `atmos-encode`, `atmos-adm` and
`atmos-iab` write AC-4 objects with `codec=ac4`, and `decode` exports objects (`objects_dir`,
`adm_out`); `mkv` refuses AC-4 (Matroska registers no codec ID for it). The banner names AC-4; the
man page's name and description lines still name AC-3 and E-AC-3 only (ROADMAP, Proposed).

**Hearth desktop (I2, I5).** The engine plays AC-4 through `ac4::Decoder`'s public API: channel-based
content up to 7.1.4, and A-JOC and direct-coded objects rendered through the layout renderer the CLI
plays them with, in full or core decoding and in an immersive layout the Decoder page's controls
choose; the Decoder page's AC-4 tab and the Media page's AC-4 information are live. A network group
gets the stream as IEC 61937-14 bursts for members on the extension role that list `"ac4"`; only the
development test sink decodes those bursts, and the ESP32 sinks advertise AC-3 and E-AC-3 only (I6).
The Decoder page follows the stream's preferred downmix and cannot ask for Pro Logic II.

**Forge GUI (I3, I5, I5b).** Encodes one source in its own layout, mono to 5.1, to a raw AC-4 stream
or an MP4 file, and authors AC-4 objects (A-JOC, or direct-coded) on the encoder page; each echoes the
`ac3cli` command that writes the same bytes. QC, the stream player and the object inspector read
AC-4, and the player exports its objects. A live session under AC-4 is refused.

**The C API and the bindings (I4, I4b).** `ac3forge_ac4_*` decoder and encoder functions, a Rust
`ac3forge::ac4` module, a Python `ac4` submodule, and a WebAssembly module with a JavaScript wrapper
(`js/src/ac4.ts`, the package's `./ac4` export), each covering channel-based and channel-based
immersive content and the encoder's object substream. Android's CMake builds the AC-4 libraries and
nothing in that app links them.

**The ESP32s (D14b).** With `CONFIG_AC3FORGE_AC4` the P4's component decodes AC-4 and `hearth_sink`
plays it from its HTTP source; Hearth does not send AC-4 to a board (I6).

**Not in the applications.** Crucible, which takes no AC-4 (decision 20); an AC-4 live session in the
Forge GUI; AC-4 for the boards of a Sendspin group (I6); Pro Logic II from Hearth's Decoder page.
`docs/assets/data/support-catalogue.json` and `docs-snippets/generated/application-capabilities.md`
record each application's rows.

### The specifications

Part 1 has 318 pages and Part 2 has 254. Local copies and their text conversions are in the main
checkout's gitignored `docs/spec/`. Each part comes with a C source file of tables:

- `ts_103190_tables.c` (Part 1, dated 2013-12-18): the codeword lengths and values of every
  Huffman codebook (ASF, A-SPX, A-CPL, dialogue enhancement, DRC), the speech spectral frontend's
  tables, `ASPX_NOISE`, and `QWIN`, the 640-tap QMF window. `QWIN` exists only in this file; Annex
  D.3 of the PDF refers to it.
- `ts_103190_tables_part2.c` (2015-07-10): the A-JOC and A-JCC codebooks and the ISF rendering
  matrices.

Everything else is transcribed from the PDF: codebook offsets and dimensions (Annex A), the scale
factor band tables (Annex B, about 885 offsets and 872 `max_sfb_master` values), the A-CPL
decorrelator coefficients (Part 1 Tables 199 to 201), and the dialogue enhancement, DRC and A-SPX
tables printed in the body.

Neither part defines what a correct decoder output is. There is no conformance clause, tolerance,
reference decoder or test vector. Pseudocode is read as exact arithmetic (Part 1 3.4). The only
statements about precision are that the speech spectral frontend's arithmetic decoder is specified
in integer arithmetic (Part 1 5.2.2, 5.2.8.2) and that a fixed-point overlap-add saturates (Part 1
5.5.2.2).

Reading both parts for this plan turned up about seventy places where pseudocode, a formula and a
table disagree, or where a case is left open. Some of them change the output if transcribed
literally:

- the QMF synthesis modulation offset: the formula on Part 1 page 196 gives 257, Pseudocode 66
  gives 255, and only 255 reconstructs (78 dB against 43 dB, measured with `QWIN`);
- a stray semicolon in A-SPX's patch table (Pseudocode 72), and a cast that truncates the tone
  generator's position to zero (Pseudocode 92);
- the speech frontend's coefficient symbol search, which as written cannot decode a negative value
  (Pseudocode 50);
- Part 2's substream mixer, whose equation divides by the number of substreams while its prose and
  Part 1's mixer sum them (Part 2 4.8.4);
- A-JOC's differential decoding, which writes the wrong wet-matrix index (Part 2 5.7.3.2).

The implementation keeps a register of them: for each, the clause, the reading taken, and the
evidence for that reading. `src/ac4dec/ERRATA.md` has 165 entries today and `src/ac4enc/ERRATA.md`,
for the readings only the writer needs, 48.

### Material

- **Encoders.** The local, licensed Dolby Encoding Engine 6.5.4 (`6.5.4-dme+b56bc97e`) has
  `dee_ac4_encoder` and `dee_ac4ims_encoder`. Its `dee_ac4ajoc_encoder` accepts only an Atmos
  master. DEE's ADM BWF input refused the master this project authored for its E-AC-3 JOC fixture,
  on provenance rather than syntax (`docs/verification.md`), and G0 and G1 found it refuses the ADM
  BWF masters `ac3cli decode` writes on the same check ([G0](#g0-the-gold-set)). What the first two
  write is in [What DEE writes](#what-dee-writes).
  - **The licence runs out on 2026-11-06** (`dee_ac4ajoc_encoder --morehelp license`; the plain
    encoder has no licence topic, and all three are one install). After that date nothing here
    encodes AC-4 with DEE: the user said on 2026-09-25 that the licence will not be renewed
    ([decision 23](#decisions-for-the-encoder-and-the-applications)).
  - **What it can be asked for.** `dee_ac4_encoder` takes 48 kHz WAV, per-channel WAVs, 5.1.4 as
    `cbi_wav` or raw PCM, and writes 2.0 (48 to 768 kbps), 5.1 (96 to 768) or 5.1.4 (192 to 768),
    with no 7.1 output and no frame rate option. It sets a DRC profile per decoder mode, the I-frame
    interval (11 to 1,000 frames, or one second by default, and forced positions from a list), the
    preferred downmix and its mix levels, the height downmix for 5.1.4, and loudness measured and
    corrected (the default) or measured only. `dee_ac4ims_encoder` takes 5.1 WAV or an Atmos master,
    at 64 to 320 kbps, at 23.976, 24, 25 or 29.97 fps or the native rate, in a general or a music
    mode (music sends no dialogue enhancement), with DRC profiles including none and a language
    tag.
    `dee_ac4ajoc_encoder` writes level 3 (320, 448 or 768 kbps) or level 4 (128 to 1,500). None of
    them can be asked for several presentations, associated audio, dialogue enhancement parameters,
    the CRC or the bitstream version. `--temp-dir` defaults to the install's own directory, so every
    run passes one.
- **Conformance material.** None. ETSI and ATSC publish no AC-4 conformance bitstreams and no
  reference output; ATSC A/342-2 constrains what a broadcaster sends. Public AC-4 streams exist,
  none with decoded output or checksums:
  - DASH-IF's test vectors (`dash.akamaized.net/dash264/TestCasesDolby/`, 2020): 2.0 at 96 kbps
    and 5.1 at 192 and 320 kbps, at 25 and 29.97 fps. No licence is stated.
  - CTA WAVE's `ca4s` sets (2023): 2.0 at 64 kbps, 30 fps, from pseudo-noise, licensed for WAVE
    testing under CC BY 4.0.
  - DVB's DASH test streams: two stereo AC-4 services.
  - Dolby's AC-4 Online Delivery Kit 1.5: 2.0, 5.1, immersive stereo and 5.1.4, at 25 and
    29.97 fps. The download page states no licence.
  - Chromium's `media/test/data`: raw streams made by Dolby, `ac4-ajoc.ac4` (A-JOC, level 3),
    `ac4-channel-based-coding.ac4` and `ac4-ims.ac4`.
  - AndroidX Media3's test data: a stereo clip and a 21-channel level 4 clip in MP4.
- **Other decoders.** Only librempeg's runs here, and it decodes part of what the phases needed:
  - FFmpeg has an AC-4 demuxer and muxer (since 6.1) and no decoder; a 2020 decoder patch was not
    merged. The FFmpeg installed here, 8.0.1 (gyan.dev's full build, and Ubuntu's in WSL), lists
    `ac4` as a codec with no decoder or encoder. Its raw demuxer finds each sync frame and reports
    no sample rate or channels; its mov demuxer reads an AC-4 track's rate and channels from MP4; it
    cannot write AC-4 into MP4; and its raw muxer's `write_crc` option writes a CRC that differs
    from DEE's in every frame of `ac4-stereo-64`, which the inspector checks against Annex G.
  - librempeg, a fork of FFmpeg, gained an experimental AC-4 decoder on 2025-06-02, under the GPL
    version 3 or later and partly derived from Emby's code. It publishes no releases or binaries,
    and the tags in its repository (`github.com/librempeg/librempeg`, about 159 MB packed) are
    FFmpeg's from 2010, so a build pins a commit.
    Emby's, Kodi's and NextPVR's AC-4 decoding come from the same code. It is built in WSL under
    `D:\ac3bld\librempeg` from commit `5854b48e` (git 2026-09-24), called by its full path, and is
    the second decoder of [The oracles](#the-oracles). Its output is 736 samples earlier than this
    project's decoder's and agrees with it to 77 to 93 dB on SIMPLE streams and, below A-SPX's
    crossover, to 83 dB where companding is off and 33 to 36 dB where it is on (D2 to D4). The
    phases record where it stops: A-CPL's rebuilt channels, which it leaves silent (D5), anything
    but a presentation's first substream group, tables of contents of more than 16 presentations
    and `bitstream_version` 1 (D7), the immersive element (D9) and object coding (D10).
  - The Dolby Reference Player's `dlbac4dec` returned no samples for any frame when it was tried
    for IM4 (`docs/verification.md`, AC-4 section). Dolby's release notes for the player say an
    install may lack the AC-4 decoder, which Dolby supplies on request; the player is now sold, with
    a trial. It is not pursued ([decision 5](#decisions)).
  - The DEE install's own tools are its encoders, MP4 tools, `atmos_info`, MediaInfo and
    `dee_convert_sample_rate`, which converts only 22.05 to 96 kHz input to 48 kHz. None decodes
    AC-4.
  - Windows ships no AC-4 decoder (a Microsoft Store decoder for PC makers is reported, not
    confirmed). This machine has no Media Foundation transform for AC-4 and no AC-4 package. Apple
    has none, GStreamer's is commercial (Fluendo), VLC has none, and Android's are on devices.
- **Readers.** MediaInfo (MediaInfoLib 26.05), bundled with the Dolby install and nowhere else here,
  reports presentation, channel, loudness, DRC, dialogue enhancement and downmix facts. With
  `--Details=1` it prints a trace of every frame: the table of contents, the presentation substream
  with its loudness and DRC fields, each substream's `metadata()` and dialogue enhancement
  configuration, the EMDF payloads and `crc_word`. It does not decode `audio_data()`. That trace
  makes it a third reader of those elements, independent of both transcriptions. DEE's MP4 muxer
  (`dee_mp4muxer`, built on Bento4) takes AC-4 elementary streams and writes their MP4 sample entry,
  and its demuxer lists AC-4 tracks.

### What DEE writes

Measured on 2026-09-15. DEE was asked for 119 encodes of recorded music and speech (from the
committed programme fixtures) and produced 100 streams. An extended copy of the Python reference
parser read every frame of every stream to its declared size, and MediaInfo agreed with every
metadata value it reports.

The codec mode of the one audio substream, by layout and data rate in kbps (constant over every
frame, and unchanged by the content tried):

| Encoder and layout | 48 | 64 | 96 | 128 | 144 | 192 | 256 | 288 | 320 | 384 | 448 | 512 | 768 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `dee_ac4_encoder`, 2.0 | ASPX | ASPX | ASPX | ASPX | ASPX | SIMPLE | SIMPLE | SIMPLE | SIMPLE | SIMPLE | SIMPLE | SIMPLE | SIMPLE |
| `dee_ac4_encoder`, 5.1 | refused | refused | ASPX_ACPL_3 | ASPX_ACPL_2 | ASPX_ACPL_2 | ASPX | ASPX | ASPX | ASPX | SIMPLE | SIMPLE | SIMPLE | SIMPLE |
| `dee_ac4_encoder`, 5.1.4 | | | | | refused | ASPX_ACPL_2 | ASPX_ACPL_2 | ASPX_ACPL_2 | ASPX_ACPL_2 | ASPX_ACPL_2 | ASPX_ACPL_2 | ASPX_SCPL | SCPL |
| `dee_ac4ims_encoder`, from 5.1 | | ASPX | ASPX | ASPX | ASPX | refused | ASPX | | ASPX | | | | |

- **Never written:** ASPX_ACPL_1, A-JCC, the 7.X element, mono, 3.0, 22.2, the HSF extension,
  44.1 kHz, more than one presentation, substream group or audio substream, and objects. There is
  no 7.1: eight-channel input is downmixed to 5.1 without an error, and 7.1.4 input is refused.
- **5.1.4** is coded as 7.X.4 with the back pair absent and three top channels present, as Part 2
  requires.
- **The speech frontend** never appears where it can be seen. Every 2.0 and IMS frame enables MDCT
  stereo processing, which makes both channels ASF. In 5.1 and 5.1.4 the centre's frontend bit
  follows the LFE's Huffman-coded spectrum, so it was not read.
- **Frames.** `dee_ac4_encoder` always writes `frame_rate_index` 13 (2,048 samples at 48 kHz) and
  has no option to change it. Its frame 0 is a priming frame, frames 0 and 1 are both I-frames, and
  I-frames then fall every 23 or 24 frames, or every 11 to 1,000 frames by option.
  `dee_ac4ims_encoder` writes index 13, or 23.976 and 24 fps (1,920 samples), 25 fps (2,048) and
  29.97 fps (1,536). It is the only local source of frames that need the sample rate converter.
  Nothing writes a frame shorter than 1,536 samples.
- **Transforms.** Every stream switches blocks, and all four transform-length indices occur.
  Companding is on in stereo at 48 to 96 kbps and off at 128 and 144.
- **Metadata.** Bitstream version 2, with dialnorm and DRC in the presentation substream. Dialnorm
  is −24 dB by default and follows the loudness options.
  - DRC appears only in I-frames, always for all four decoder modes, and always as default profiles
    or compression curves; transmitted gains never appear.
  - Dialogue enhancement is present in every frame of every stream, music included: always the
    channel-independent method with a 9 dB cap, with its channels following detected dialogue
    (none, L and R, or C). IMS in its general mode adds advanced dialogue enhancement data.
  - 5.1 and 5.1.4 streams carry downmix gains and custom downmix data.
  - EMDF payloads with ids 18 and 20, which Part 1 leaves to an external registry.
- **Immersive stereo.** One presentation with a single A-SPX stereo substream at every rate. It
  signals `presentation_version` 2, which V1.3.1 names without giving it any syntax, and a
  `channel_mode` code that Part 2 Table 56 maps to 7.0 (3/4/0). Read as 7.0 every I-frame fails;
  read as stereo, every frame of all 23 IMS streams ends exactly, and MediaInfo reports stereo.
  `b_pre_virtualized` is 0 in every stream, IMS included.

What that leaves each tool to be tested on:

| Tool | Streams DEE writes | Not available from DEE |
|---|---|---|
| ASF, stereo processing, block switching | 2.0 at 192 to 768 kbps | SSF, mono, stereo without MDCT stereo processing |
| A-SPX and companding | 2.0 at 48 to 144 kbps; every IMS stream | |
| The 5.X element | 5.1, SIMPLE at 384 to 768 and A-SPX at 192 to 320 | 5.0, 3.0, 7.X |
| A-CPL | ASPX_ACPL_2 in 5.1 at 128 and 144 and 5.1.4 at 192 to 448; ASPX_ACPL_3 in 5.1 at 96 | ASPX_ACPL_1; A-CPL in a channel pair |
| The immersive element | 5.1.4: ASPX_ACPL_2, ASPX_SCPL at 512, SCPL at 768 | ASPX_AJCC; 7.X.4 and 9.X.4 with every channel present |
| Sample rate converter; 1,920 and 1,536-sample frames | IMS at 23.976, 24, 25 and 29.97 fps | frames shorter than 1,536 samples |
| DRC | default profiles and curves, every stream | transmitted gains; decoder modes above 3 |
| Dialogue enhancement | the channel-independent method on L and R, or C | the other three methods |
| Presentations | one per stream | several presentations; M&E and dialogue; main and associated |
| Objects | none | A-JOC, direct-coded objects, OAMD |

## What decoding AC-4 involves

### The chain

For a channel-coded substream (Part 1 Figure 9 and clause 6, Part 2 4.8.3):

1. The spectral frontend: the audio spectral frontend (ASF), or the speech spectral frontend (SSF)
   for tracks that select it. SSF can be selected only for a centre `mono_data`, a `stereo_data`
   pair, and the two A-CPL stereo modes; the LFE and every multichannel data element are ASF.
2. Stereo and multichannel processing in the MDCT domain: M/S, prediction by `alpha_q`, and the
   multichannel matrices. All of it is real-valued.
3. The inverse MDCT with KBD windows and overlap-add, then the frame alignment delay (Part 1 5.6).
4. QMF analysis.
5. Companding, A-SPX and A-CPL, in that order. Immersive elements use S-CPL, A-CPL for immersive
   or A-JCC in place of A-CPL (Part 2 5.3 to 5.6), and A-JOC substreams use A-JOC (Part 2 5.7).
6. Dialogue enhancement, then DRC with its gain towards the output level, then QMF synthesis.
7. The sample rate converter.

Then, per presentation: mixing its substreams (Part 1 6.2.16, Part 2 4.8.4), loudness correction
(Part 2 4.8.5), and rendering to the output layout (Part 1 6.2.17, Part 2 5.10).

### The size of each part

Pages include syntax, semantics and tables.

| Tool | Clauses | Pages | State carried between frames | Needed for |
|---|---|---|---|---|
| Channel element syntax | P1 4.2.5-4.2.7, 4.3.5 | 12 | configuration from I-frames | every stream |
| ASF | P1 4.2.8, 4.3.6, 5.1, A.1, B | 36 | none; the noise fill reseeds from `sequence_counter` | every stream |
| SSF | P1 4.2.9, 4.3.7, 5.2, C | 35 | envelope, predictor and spectrum history; two generators | tracks that select it |
| Stereo and multichannel processing | P1 4.2.10, 4.3.8, 5.3 | 11 | none | 2.0 and wider |
| IMDCT, windows, block switching, frame alignment | P1 5.5, 5.6 | 8 | overlap buffer; alignment delay line | every stream |
| QMF analysis and synthesis | P1 5.7.1-5.7.4, D.3 | 5 | filter states of 640 and 1,280 samples; control data held one to four frames | every codec mode except SIMPLE, and DRC and dialogue enhancement |
| Companding | P1 4.2.11, 4.3.9, 5.7.5 | 4 | none | A-SPX modes |
| A-SPX | P1 4.2.12, 4.3.10, 5.7.6, A.2, D.2 | 50 | delay lines, previous envelopes, generator indices | every codec mode except SIMPLE |
| A-CPL | P1 4.2.13, 4.3.11, 5.7.7, A.3 | 23 | three decorrelators per QMF band, the transient ducker, interpolation history | A-CPL modes; A-JCC and A-JOC reuse its decorrelators |
| Dialogue enhancement | P1 4.2.14.11-13, 4.3.14, 5.7.8, A.4 | 13 | previous parameters and matrix | streams that carry it |
| DRC | P1 4.2.14.5-10, 4.3.13, 5.7.9, A.5 | 17 | configuration from I-frames; smoothing per channel and band | every stream, for the output level gain |
| Metadata | P1 4.2.14, 4.3.12; P2 6.2.7, 6.3.8 | 18 | downmix and mixing values persist | every stream |
| Sample rate converter | P1 6.2.15; P2 5.11 | 1 | filter state and phase | every frame rate except index 13 |
| Mixing and rendering | P1 6.2.16-6.2.17; P2 4.8.4-4.8.5, 5.10.2 | 25 | persistent gains | presentations of more than one substream; any output other than the coded layout |
| Presentation substream and presentation data | P2 6.2.2.3, 6.2.9, 6.3.3.1, 6.3.10 | 20 | group gains, names | bitstream version 2 |
| Immersive element, S-CPL, A-CPL for immersive | P2 5.2-5.5, 6.2.4 | 15 | as A-CPL | 7.X.4 and 9.X.4 |
| A-JCC | P2 5.6, 6.2.6, 6.3.7, A.1.2 | 17 | history per derived array; four to eight decorrelators | immersive elements coded ASPX_AJCC |
| A-JOC | P2 5.7, 6.2.5, 6.3.6, A.1.1 | 18 | dry and wet history, ramps across frames, seven decorrelators | A-JOC substreams |
| Object audio metadata | P2 5.9, 6.2.8, 6.3.9 | 33 | previous blocks and defaults | object substreams |
| ISF renderer | P2 5.10.3 | 2 | none | ISF object substreams |
| Efficient high frame rate mode | P2 5.1.3 | 2.5 | a queue of fragments | streams with `frame_rate_fraction` above 1 |

### Facts that shape the design

**Most frame rates decode at an internal rate.** Part 1 Table 83 codes every video frame rate at a
frame length of 2,048, 1,920 or 1,536 samples, or a half or a quarter of one, at an internal rate
of 46,080 Hz, 46,033.97 Hz or 51,200 Hz. A sample rate converter at 25/24, 1001/1000 × 25/24 or
15/16 then produces 48 kHz (6.2.15). Only `frame_rate_index` 13, 2,048 samples at 48 kHz, needs
none. The converter's filter is a recommendation, so at every other index two conforming decoders
need not agree sample for sample. At the 1000/1001 rates a frame is not a whole number of output
samples, and Part 2 5.11 locks the converter's phase to `sequence_counter` so the output sample
count comes out exact.

**Transform lengths have factors of three and five.** At 48 kHz internal the inverse MDCT runs at
fifteen lengths from 96 to 2,048 (Part 1 Tables 99 to 105). Ten of them are 96 · 2^k or 120 · 2^k,
not powers of two. `ac3::forge`'s FFT kernel takes powers of two only
(`src/dsp/include/iclforge/dsp/detail/fft_kernel.hpp`), and its public MDCT is fixed at 512 and 256 samples.

**The QMF bank has a published window.** AC-4's filterbank has the structure of the one
`ac3::forge` built for E-AC-3 JOC (`src/dsp/include/iclforge/dsp/qmf.hpp`): 64 complex subbands, a
hop of 64 samples and a 640-tap window. ETSI TS 103 420 does not publish that window, so the
repository designed its own. AC-4 publishes `QWIN`, which carries the alternating sign in the table
itself, uses a different phase convention, reconstructs to about 78 dB, and delays by 577 samples.
Given `QWIN` shifted by one sample, a fixed phase per band and one more sample of delay, the forge
bank's structure reproduces AC-4's bank to floating-point precision (checked numerically for this
plan). The designed window cannot stand in for `QWIN`: A-SPX's patching and its tone generator
depend on `QWIN`'s response and phase. Several QMF-domain tools are not scale-invariant (the
companding exponent, A-SPX's envelope estimate, DRC levels), and Part 1 does not state its
full-scale convention. Phase D3 settled the first two against DEE's streams: A-SPX's envelopes read at
the inverse transform's scale, full scale 2^15, and companding measures its levels against full scale
1.0 (`src/ac4dec/ERRATA.md`).

**QMF-domain parameters arrive ahead of their audio.** The inverse MDCT output is delayed so that
the control data of A-SPX, A-CPL, dialogue enhancement and DRC applies one, two or four frames
after it is parsed (Part 1 5.6, 5.7.2, Table 188).

**Configuration comes in I-frames.** A SIMPLE-mode substream decodes from any frame after one frame
of overlap. Every other codec mode needs `aspx_config` and `acpl_config`, which only I-frames
carry, and SSF can start only at an SSF I-granule. A switch at an I-frame boundary shall be
seamless; anywhere else, output shall be restored by the next I-frame. `sequence_counter` 0 marks
a splice (Part 1 6.2.19).

**Loudness and DRC follow a different model from AC-3.** The DRC tool takes dialnorm (0 to
−31.75 dBFS in quarter-dB steps) to an output level `Lout` that the system supplies, with a gain of
`2^((Lout − dialnorm)/6)`, which can boost as well as cut (Part 1 5.7.9.3.3). There are no line or
RF modes and no cut or boost scale factors. A stream carries up to eight DRC decoder modes, four
with defined meanings: home theatre (−31 to −27 dBFS), flat panel TV (−26 to −17), and portable
speakers and portable headphones (both −16 to 0) (Table 161). Each mode repeats another, uses a
default profile, sends a compression curve that the decoder evaluates with a level detector the
specification leaves open, or sends gains per channel group, band and subframe. Part 1 gives no
default `Lout`, and its ranges are strict inequalities, so a literal reading selects no mode at
exactly −31.

**Downmix is a set of plain matrices.** Lo/Ro and two Lt/Rt methods are built from the stream's
mixing gains. Lt/Rt has no 90-degree phase shift, because `phase90_info` describes processing done
before encoding. The LFE is always mixed in at `lfe_mixgain`, and mono is L + R without scaling
(Part 1 6.2.17). A listener's choice of method overrides `preferred_dmx_method`.

**5.1.4 is coded as 7.X.4.** It has no channel mode of its own: it is `channel_mode` 12 with
`b_4_back_channels_present` clear and `top_channels_present` 3 (Part 2 Tables 56, 57, 59 and
A.28), so a 5.1.4 decoder is a 7.X.4 decoder. An immersive element is coded in one of five modes,
signalled in every frame: SCPL and ASPX_SCPL (eleven waveform tracks and a fixed matrix),
ASPX_ACPL_1 and ASPX_ACPL_2 (seven tracks and A-CPL), and ASPX_AJCC (five tracks and A-JCC)
(Part 2 Table 73).

**A decoder decodes fully, or decodes the core.** A decoder shall support at least one of full
decoding, in which A-CPL, A-JCC and A-JOC reconstruct every channel and object, and core decoding,
which skips or simplifies those tools for low-complexity platforms (Part 2 4.7).

**Decoder levels.** `md_compat` bounds what a presentation needs: level 0 two tracks, 1 six, 2 nine
(7.1.4 input allowed), 3 eleven, and 7 unrestricted (Part 2 Table 55; Part 1 Table 86 for version 0
presentations). A decoder of level n shall not select a presentation above n. ATSC 3.0 requires a
presentation of level 3 or below in every primary stream, and DVB's receiver guidelines require
levels up to 3 and decoding of bitstream versions 0 and 2, dialogue enhancement and DRC.

**Objects are rendered outside the specification.** Rendering dynamic and bed objects to
loudspeakers is not normative. ETSI TS 103 448 (V1.1.1, 2016) is an informative reference, and
informative Annex F lists what a decoder hands a renderer: position, gain, size, zones, divergence,
snap, timing and trim. The channel renderer (Part 2 5.10.2) and the ISF renderer (5.10.3) are
normative.

**"Immersive stereo" is not a term in either part.** The nearest mechanism is `b_pre_virtualized`,
a presentation flag saying the content was rendered for headphones before encoding (Part 1
4.3.3.3.5). It changes nothing in decoding. The headphone fields in object metadata describe
intent, and no headphone rendering is specified. DEE's IMS streams signal a presentation version
that V1.3.1 does not define ([What DEE writes](#what-dee-writes)).

## What encoding AC-4 involves

Part 1's introduction says the encoding process is not normative (pp. 18 and 19). Neither part has
an encoder annex, a forward transform, a psychoacoustic model or a method for estimating any
parameter: they define the bitstream and what a decoder does with it. An encoder is correct when a
conforming decoder reads its streams and reproduces the source closely enough, and the rest is its
own design, made against the decoder's equations.

### The chain, in reverse

For a channel-coded substream the encoder runs the decoder's chain backwards, choosing at each stage
what the decoder will be told:

1. At every `frame_rate_index` but 13, a sample rate converter from 48 kHz to the internal rate Part
   1 Table 83 implies: 46,080 Hz at 24, 30, 48, 60 and 120 fps, that rate divided by 1.001 at the
   1000/1001 rates, and 51,200 Hz at 25, 50 and 100 fps. The texts describe only the decoder's
   converter (Part 1 6.2.15; Part 2 5.11); the encoder's being its inverse is an inference.
2. QMF analysis of every channel with `QWIN`, since every QMF-domain parameter is defined on that
   bank's output: companding, A-SPX, A-CPL, A-JOC, dialogue enhancement and DRC gains. The analysis
   is aligned to the delays the decoder applies (Part 1 Tables 188 and 192), so that the parameters
   estimated for a frame land on the audio they describe.
3. The parametric tools: A-JOC's matrices from the objects and their downmix; A-CPL's and S-CPL's
   parameters from the channels they rebuild, and the downmix that is coded; A-SPX's envelopes,
   noise floors and tones from the source above the crossover; and companding's gains, applied in
   the QMF domain and synthesised back before the transform.
4. The forward MDCT with KBD windows, its transform lengths chosen per frame within Part 1 Table
   187's splits.
5. Stereo and multichannel processing per band: M/S, prediction by `alpha_q`, and the 5.X and 7.X
   matrices.
6. Quantisation: scale factors, quantised spectra, sections and codebooks, and noise fill, within
   the frame's bits.
7. Metadata: loudness, DRC configuration or gains, dialogue enhancement parameters and downmix
   values.
8. The table of contents, the presentation substream, the substream index table and the sync frame.

### What the texts give an encoder, tool by tool

| Tool | What the texts define | What the encoder devises |
|---|---|---|
| Transform | the IMDCT, KBD windows by transform length (Part 1 Table 186) and the allowed splits (Table 187); switching on transients, informatively (p. 189) | the forward MDCT and its scaling against the IMDCT's (Pseudocode 64 and the worked example differ by a factor of two, pp. 190 and 191), and the transient detector |
| ASF quantisation | reconstruction only: the dequantiser and the scale factor gains, scale factors coded differentially from a reference, quantised values within ±8191 and scale factors within 0 to 255 (Part 1 5.1.3, pp. 141 and 142) | the psychoacoustic model, the scale factors, sections and codebooks, and the rate loop |
| Noise fill | whole zero bands only, at the previous coded band's power plus a delta in 3 dB steps (pp. 143 to 145) | when to fill, and at what level |
| Stereo processing | M/S, and prediction L = (1 + α)M + S, R = (1 − α)M − S with α = 0.1 · `alpha_q` for each pair of bands (5.3.2, p. 174) | the choice per band, and α |
| Companding | the decoder's expander (5.7.5, pp. 197 to 199); the encoder's side is one sentence | the compressor, as the expander's inverse |
| A-SPX | the decoder's regeneration in full: patching, the limiter, what an envelope, a noise value and a tone mean (pp. 206 to 226); the intent of variable borders (p. 209) and of interleaved waveform coding (pp. 230 and 231) | framing, envelope, noise and tone estimation, and when to interleave. The regeneration is deterministic, so the encoder can run it to see what a decoder will produce |
| A-CPL | the upmix; the downmix it implies, x0 = (L + R)/2, and the scalings of the 5.X and 7.X modes (Pseudocodes 115 to 117, pp. 238 to 241) | the parameters |
| S-CPL | fixed matrices (Part 2 5.3) | nothing; the matrices invert exactly |
| A-JCC | the decoder's reconstruction (Part 2 5.6) | the parameters |
| A-JOC | z = C_dry · x + C_wet · decorr(D · x), with the matrices' ranges (Part 2 5.7, pp. 83 to 91); the downmix is not specified, and may be a static 5.0 or 5.1 bed (p. 161) | the downmix, and the matrices |
| Dialogue enhancement | the three methods, the cap and eight parameter bands (Part 1 5.7.8); its data is recommended wherever dialogue is present (p. 131) | the parameters, which need to know where the dialogue is |
| DRC | the decoder's evaluation of default profiles and compression curves, with a level detector the text leaves open, and its application of transmitted gains (5.7.9, pp. 255 to 258) | in curve modes, the configuration; in gain modes, the gains |
| Loudness | the fields and the measurements they carry, by reference to BS.1770-3, BS.1771-1 and EBU Tech 3342 (pp. 108 to 118) | the measurement |
| Downmix | the matrices (Part 1 6.2.17) and the values that drive them | the values |
| Object metadata | the syntax and the meaning of each property (Part 2 5.9 and Annex F); rendering is not specified | the values, from the scene |

### Facts that shape the encoder

**ASF's allocation is the encoder's.** In AC-3 a decoder recomputes the bit allocation from
transmitted exponents with a normative model, and an encoder tunes a few of its parameters. ASF
sends scale factors and codebooks directly, as MPEG AAC does, so every choice of precision is the
encoder's, and coding quality rests on its psychoacoustic model and its rate loop. The nearest thing
in the tree is `ac3::quality`'s model (`src/ac3/include/iclforge/ac3/quality/perceptual.hpp`): Johnston's
perceptual entropy with the tonality measure of ISO/IEC 11172-3 Annex D.2 and Schroeder's spreading,
over A/52's 50 bands. It prices AC-3's allocation parameters and has never driven an allocation.

**The parametric tools are estimated against the decoder.** A-SPX, A-CPL, A-JCC and A-JOC send
descriptions a decoder synthesises from. That synthesis is fully specified (the HF generator and its
patching, which depend on `QWIN`; the decorrelators; the matrices), so the encoder can run it on its
own candidate parameters and measure what a decoder will produce. This plan has it do so.

**Rate.** `wait_frames` 0 signals a constant bit rate, 1 to 6 an average bit rate with the decoder
waiting 0 to 5 frames before it starts (0 to 10 at indices 10 to 12), and 7 a variable bit rate
(Part 1 Table 81, p. 73). The decoder's input buffer holds six frames at the stream's rate, twelve
above 60 fps (6.2.4 and Table 211, pp. 263 and 264), and only a variable bit rate stream's frames
may exceed it. No other size is capped: `frame_size` escapes to 24 bits (Annex G). MPEG-2 TS
carriage sets its own buffer (Part 2 Annex D.2).

**Configuration and I-frames.** A-SPX, A-CPL and DRC configuration is sent only in I-frames, and
dialogue enhancement's in I-frames or where `de_config_flag` says. Nothing in the texts mandates an
interval; the containers do, since the first sample of each movie fragment (Part 1 Annex E.5) and of
each CMAF fragment (Part 2 Annex E.3) shall be a sync sample. An I-frame must not predict across
time. Part 2 has flags that say a substream does not (`b_audio_ndot` and its siblings); Part 1
leaves it to the encoder.

**`sequence_counter` carries four things.** It counts 1 to 1020 and wraps to 1, with 0 marking a
splice (Part 1 4.3.3.2.2), and the first sample of an ISOBMFF file should carry 0 (Annex E.1). It
detects splices, seeds noise fill's generator (Pseudocode 24), sets the phase of the decoder's
converter at the 1000/1001 rates (Part 2 5.11) and groups the fragments of the efficient high frame
rate mode (Part 2 5.1.3). 1020 is divisible by 4 and by 5, so the wrap keeps the converter's phase
and the grouping.

**Limits a stream keeps.** Huffman codebooks 12 to 15 are not used. Mono and 3.0 elements code
SIMPLE or ASPX only, and the 7.X element has no ASPX_ACPL_3; Part 1 Tables 212 to 214 fix which
channels companding, A-SPX and A-CPL process in each element and mode. A-SPX has at most five noise
groups and five patches, and four envelopes in FIXFIX or five otherwise, and an interval class that
ends in FIX is followed by one that starts with FIX (p. 209). A 3.0 substream is only a dialogue
enhancement or dialogue signal (p. 77), and dialogue and associated substreams add no channel the
main one lacks, except mono (p. 268). `md_compat` caps each level's channels and objects (Part 1
Table 86, Part 2 Table 55). A stream with the HSF extension uses no QMF-domain tool (Part 1 5.4),
and 44.1 kHz exists only at index 13 (Table 84). CMAF adds rules of its own (Part 2 Annex H):
bitstream version 2, presentation version 1, at most 64 presentations, a `presentation_id` in every
sample and the same table of contents configuration in every sample.

**A decoder may decode fully or only the core** (Part 2 4.7), so an immersive or object stream has
to work in both; the encoder's streams are scored in both modes.

**DEE writes a subset.** DEE's streams exercise the tools [What DEE writes](#what-dee-writes) lists,
and none of its encoders takes several presentations, associated audio or dialogue enhancement
parameters. Syntax outside that set has no other encoder's stream to pin its readings, so an encoder
that writes it rests on the two transcriptions, MediaInfo's trace where MediaInfo reads the element,
and a second decoder.

## Design

### Where the libraries live

Three libraries beside the inspector, none linking `ac3::forge`
([decision 15](#decisions-for-the-encoder-and-the-applications)):

- **The decoder**, `src/ac4dec/` (`ac4::decoder`), as D1 built it: an OBJECT library with static
  and shared wrappers, headers under `include/ac4dec/`, types in namespace `ac4`, linking `ac4::ac4`
  for the table of contents and presentation types.
- **The encoder**, `src/ac4enc/` (`ac4::encoder`), in the same pattern, with headers under
  `include/ac4enc/`. It links `ac4::ac4`, whose `Toc` describes what it writes and whose
  `build_dac4()` and `rfc6381_codec_string()` it reuses.
- **The shared core**, `src/ac4core/` (`ac4::core`): what both directions compute, built and tested
  once ([DSP](#dsp)). It is a static library of position-independent code with hidden symbols,
  linked privately by the other two, so each shared library carries the part it uses, a static
  build links it once, and it has no public headers and no ABI of its own. D2 created it and moved
  D1's generated tables into it. The bit reader and the Huffman decoder stayed in `src/ac4dec`,
  since they carry the syntax trace and its sink, and the encoder has a bit writer of its own in
  `src/ac4enc`.

The syntax stays apart. The decoder's reader and the encoder's writer are separate transcriptions of
the syntax tables, in opposite directions, and the Python reference stays independent of both. A
stream the encoder writes is therefore read by the decoder, which shares only the core's tables and
kernels with it, and by the Python parser, which shares nothing with it, down to its own tables.

`AC3FORGE_BUILD_AC4` gates all of them, and is on by default. The ESP-IDF component leaves it off and
the minimal profile refuses it: `CONFIG_AC3FORGE_AC4` (D14b) builds the inspector, the core and the
decoder into the component through `AC3FORGE_MINIMAL_AC4`, and the encoder never goes there. Each was
added where the peer libraries are: its own `CMakeLists.txt`, the root option and subdirectory,
`tests/CMakeLists.txt`, an instrumented fuzz target, an ABI allowlist, the coverage table,
`docs/building.md`, `docs/library/` and `docs/verification.md`. D8 and E7 installed and exported
them in `cmake/InstallLibrary.cmake` and `cmake/iclforgeConfig.cmake.in`, since an exported target
cannot link one that is not exported.

### What the inspector grows

- The fields of the table of contents the decoder needs: `add_ch_base`, the per-instance
  `b_iframe`/`b_audio_ndot` flags, `presentation_id`, `b_pre_virtualized`, `b_alternative`,
  `b_pres_ndot`, the presentation substream's index and the EMDF payload substreams' indices (D1).
- A splitter that holds a partial frame between reads, for streams that arrive in pieces
  (`ac4::SyncFrameSplitter`, D8).
- `presentations_v1` and the metadata listed under [Media information](#media-information) in
  `ac3cli probe`'s JSON, added under `ac3forge.probe/1` (D8).

As built in D1, `ac4_presentation_substream()` (Part 2 6.2.2.3), `presentation_version` 2 as DEE's
IMS streams use it ([decision 10](#decisions)), and the position where `audio_data()` starts are
read by the decoder, with its own bit reader, which carries the syntax trace; the inspector stays
the table of contents and the substream framing. Media information takes presentation names from
the decoder's reading in D8.

Since #739 the inspector reads the `oamd_common_data()` of an `ac4_substream_info_ajoc()`. The OAMD
substream's own content, which can carry a second one, is the decoder's, read in D10
(`SubstreamReport::oamd_common_data`).

### DSP

The two directions carry their own ([decision 7](#decisions)), in the shared core:

- an FFT for lengths of the form 2^a · 3^b · 5^c, and the MDCT and inverse MDCT on it, each tested
  against a direct evaluation of its formula (Part 1 5.5.2 for the inverse; its adjoint for the
  forward transform);
- KBD windows computed from their formula, with the alphas of Part 1 Table 186;
- the QMF analysis and synthesis banks with `QWIN`;
- the kernels of reconstruction an encoder also runs, to see what a decoder will produce. As built,
  the core holds A-SPX's subband tables and HF generator, A-CPL's parameter bands, tables,
  interpolation, three decorrelators and transient ducker, and A-JCC's and A-JOC's reconstruction,
  which reuse the decorrelators (`src/ac4core/include/iclforge/ac4core/aspx`, `acpl`, `ajcc` and `ajoc`).
  Dequantisation, the stereo and multichannel matrices, companding and A-SPX's envelope adjustment
  stayed in the decoder (`src/ac4dec/src/pcm`), and the encoder has the matrices' inverses of its own
  (`src/ac4enc/src/asf`);
- the sample rate converters in both directions: from the internal rate to 48 kHz for the decoder,
  and from 48 kHz to the internal rate for the encoder;
- the tables: the Huffman codebooks, in the decoder's order and in the index order an encoder looks
  codewords up by, generated together from the attachment; the scale factor band tables; and the
  QMF window, the noise tables and the ISF rendering matrices. The dialogue enhancement and DRC
  values are not shared: each side holds what it applies or writes.

`ac3::forge`'s FFT kernel and QMF bank are the pattern for the transforms and the QMF bank, and are
not linked.

### Arithmetic

The reference is `double`. Neither part asks for bit-exact output, and the converter rules it out
at most frame rates, so each further tier promises what `ac3::forge`'s tiers promise: a stated
agreement with the `double` build, measured.

The ESP32-S3 has a floating-point unit and uses `float`; the ESP32-C6 has none and needs fixed
point ([decision 8](#decisions)). So from D2 the DSP is written against a scalar type that both a
floating type and a fixed-point type can instantiate, in the pattern of forge's decode path
(`planning/arithmetic-tiers.md`): arithmetic through the type's operators, functions through
overload sets, and a block exponent carried with each block of coefficients where a fixed store
would otherwise lose a small value's bits. `double` and `float` are built (D14a and D14b), and
fixed point, for the C6, in D14d. The QMF-domain tools make the fixed tier harder
than forge's was: every codec mode except SIMPLE runs a complex filterbank, A-SPX's envelope
estimates and, in the A-CPL modes, IIR decorrelators on every channel, and forge's own JOC
reconstruction still runs in `float` in every build because of the same filterbank. The speech
frontend's arithmetic decoder uses integer arithmetic in every build, as the text specifies.

As D6 left it, the pattern held for about a third of the DSP: `src/ac4core`'s kernels were
templates on `Real`, instantiated only at `double` and over `std::complex<Real>`, which a
fixed-point type cannot instantiate, and the reconstruction in `src/ac4dec/src/pcm` spelled `double`
directly. On 2026-09-25 the user asked that AC-4 follow AC-3's and E-AC-3's approach, `double`,
`float` and fixed point by target ([decision 25](#decisions-of-2026-09-25)), and D14a did it: the
kernels and the reconstruction are templates on `Real` over `dsp::Complex<Real>`, a complex type of
the project's own, and AC-4 joined `AC3FORGE_DECODE_SCALAR`, so one source builds the `double` and
the `float` decoder. The ESP-IDF component builds the `float` one behind `CONFIG_AC3FORGE_AC4`,
offered only on a part with a floating-point unit (D14b). With `AC3FORGE_DECODE_SCALAR=fixed` the core
still builds at `double`, since the fixed tier has not instantiated these kernels. The rules for new
code stand: kernels are templates on `Real`, and no phase adds a function-local static, a stack
object over 4 KiB, or a heap allocation each frame where a member buffer would do.

The encoder runs on computers only. It is built at `double`, on the same scalar type as the core it
shares, with no `float` or fixed-point tier and no place in the ESP32 builds; the core's kernels it
calls are also instantiated at `double` in a `float` build (`AC4CORE_ALSO_AT_DOUBLE`). Its output is
deterministic for one toolchain, as `ac3::forge`'s encoders' is, and is not promised byte-identical
across toolchains.

### The API

The decoder takes one `raw_ac4_frame` at a time and returns PCM for one presentation. This is the
sketch the design started from; `src/ac4dec/include/iclforge/ac4dec/decoder.hpp` is the API as built, and the
notes after the sketch say where it differs:

```cpp
namespace ac4 {

enum class DownmixTarget : std::uint8_t { kAsCoded, kLoRo, kLtRt, kMono };  // as ac3::DownmixTarget
enum class DrcMode : std::uint8_t {
    kOff, kDefault, kHomeTheatre, kFlatPanelTv, kPortableSpeakers, kPortableHeadphones,
};

struct OutputConfig {
    DownmixTarget target = DownmixTarget::kAsCoded;  // Part 1 6.2.17
    std::optional<ChannelLayout> layout;             // any other output layout, Part 2 5.10.2
    double output_level_dbfs = -31.0;                // Lout, Part 1 5.7.9.3.3
    DrcMode drc = DrcMode::kDefault;                 // the mode Table 161 selects for Lout
    double dialogue_enhancement_db = 0.0;            // G_DE, capped by the stream
    double dialogue_gain_db = 0.0;                   // g_dialog, M&E and dialogue
    double associated_gain_db = 0.0;                 // g_assoc, main and associated
    bool mix_lfe = true;                             // the specification always mixes it
    int sample_rate_hz = 48000;                      // 48, 96 or 192 kHz; 44.1 kHz at index 13
};

struct DecoderConfig {
    PresentationChoice presentation{};  // an id, an index, or preferences
    int level = 3;                      // the md_compat level the decoder claims
    DecodingMode mode = DecodingMode::kFull;  // or kCore, Part 2 4.7
    OutputConfig output{};
    ConcealmentPolicy concealment = ConcealmentPolicy::kNone;
    bool skip_reconstruction = false;   // parse and report, render nothing
    DiagnosticSink diagnostics = nullptr;
    void* diagnostics_context = nullptr;
};

class Decoder {  // a Pimpl, as the library's other stateful classes are
   public:
    explicit Decoder(const DecoderConfig& config);
    std::expected<std::optional<DecodedFrame>, DecodeError> decode(std::span<const std::byte> frame);
    std::expected<FrameInfo, DecodeError> decode_by_block(std::span<const std::byte> frame,
                                                          BlockSink sink);
    void set_output(const OutputConfig& output);  // from the next frame, gains ramped
    std::span<const PresentationInfo> presentations() const;
    int latency_samples() const;
    void reset();
};

}  // namespace ac4
```

- **Frames and samples.** A decoded frame carries its output sample rate and sample count, which
  alternates at the 1000/1001 frame rates, its channel layout as a list of speaker locations, the
  presentation it came from, and the metadata below. `decode()` returns nothing for a frame that
  produces no output, such as the frames of a stream joined before its first I-frame.
- **Blocks.** `decode_by_block` hands over the output in blocks of 256 samples, the size forge's
  `BlockSink` uses, through a sink type of the decoder's own, since `ac3::BlockSink` is typed in
  `ac3::`.
- **Objects.** From the object phase, a frame also carries each reconstructed object's PCM and its
  properties in Annex F's terms, and the application renders them. The decoder does not depend on
  the renderer that Hearth's phase A1 moves into `ac3::forge`.
- **Errors.** A frame that cannot be decoded returns an error, or, under a concealment policy, a
  frame of concealed output and a record of what was done, as forge's decoders do.
- **The syntax trace.** D8 settled the sink's lifetime, which `DecoderConfig` had wrong, by
  ownership: the configuration owns a copy of the callable, and the encoder's write trace follows the
  same rule.
- **Hearth.** The engine's `StreamDecoder` (`apps/hearth/engine/stream_decoder.hpp`) decodes a unit
  into blocks rendered onto a `render::OutputLayout`; `decode_by_block` and the frame's layout are
  what an adapter there needs, and the adapter lives in the engine, since the decoder does not link
  `ac3::forge`'s renderer.

As built, the header differs from the sketch in more places than
[D8](#d8-the-api-the-cli-media-information-and-packaging) lists. `OutputConfig::output_level_dbfs` is a `std::optional<double>`, and unset, its default, leaves
the stream at its coded level and compresses nothing, since Part 1 gives `Lout` no default (D6);
`headphones` chooses between the two portable DRC modes; `downmix` names one of the layouts the
renderer gives (as coded, 5.X, stereo, Lo/Ro, Lt/Rt, mono, and the immersive 7.X.4, 7.X.2, 7.X.0,
5.X.4 and 5.X.2) and replaces `target` and `layout` (D9); and there is no `sample_rate_hz`: the
decoder outputs 48 kHz, or 44.1 kHz at index 13, and refuses a 96 or 192 kHz substream, whose HSF
extension it reads and does not decode.

### The encoder's API

The encoder takes PCM at 48 kHz, or at 44.1 kHz for `frame_rate_index` 13, the one index Part 1
Table 84 defines at that rate, in blocks of any length, and returns each `raw_ac4_frame` as it
completes. This is the design's sketch; `src/ac4enc/include/iclforge/ac4enc/encoder.hpp` is the API as built,
and the notes after the sketch say where it differs:

```cpp
namespace ac4 {

enum class CodecMode : std::uint8_t {
    kAuto, kSimple, kAspx, kAspxAcpl1, kAspxAcpl2, kAspxAcpl3,  // Part 1 elements
    kScpl, kAspxScpl, kAspxAjcc,                                 // Part 2 immersive element
};
enum class RateControl : std::uint8_t { kConstant, kAverage, kVariable };  // wait_frames 0, 1-6, 7

struct SubstreamInput {
    ChannelLayout layout;                 // mono to 7.1.4; A-JOC objects from E9
    CodecMode mode = CodecMode::kAuto;
    ContentClassifier role = ContentClassifier::kCompleteMain;  // M&E, dialogue, associated, ...
    std::optional<DialogueSource> dialogue;  // a stem or marked channels, for dialogue enhancement
};

struct PresentationConfig {
    std::vector<int> substreams;          // indices into EncoderConfig::substreams
    std::string name;
    std::string language;                 // as the syntax carries it
    Loudness loudness;                    // dialnorm and the further loudness values
    DrcConfig drc;                        // per decoder mode: a profile, a curve, a repeat or gains
    DownmixConfig downmix;                // mixing gains, preferred method, custom data
};

struct EncoderConfig {
    FrameRate frame_rate = FrameRate::kIndex13;  // or 23.976 to 120 fps, at an internal rate
    int bitrate_kbps = 128;               // the stream's total
    RateControl rate = RateControl::kConstant;
    int iframe_interval = 24;             // frames; positions can also be forced
    bool crc = false;                     // sync word 0xAC41 and Annex G's CRC
    bool experimental_tools = false;      // decision 16
    std::vector<SubstreamInput> substreams;
    std::vector<PresentationConfig> presentations;
};

class Encoder {  // a Pimpl
   public:
    explicit Encoder(const EncoderConfig& config);
    // Samples for every input channel, any count; the frames this completes.
    std::expected<std::vector<EncodedFrame>, EncodeError> encode(
        std::span<const std::span<const float>> channels);
    std::expected<std::vector<EncodedFrame>, EncodeError> flush();
    const Toc& toc() const;               // what the stream carries; build_dac4() takes it
    LatencyBudget latency() const;        // as ac3::forge's encoders report theirs
};

}  // namespace ac4
```

- **Frames.** An encoded frame carries its `raw_ac4_frame`, which an MP4 sample holds as it is, its
  sample count and whether it is an I-frame. A function beside the encoder wraps one in a sync
  frame, with or without the CRC, for raw `.ac4` files and MPEG-2 TS.
- **Configuration.** One configuration builds every substream and presentation the stream will
  carry; the table of contents does not change within a stream, as CMAF requires. Values that
  change during a stream, such as object positions, arrive with the samples.
- **Loudness** is supplied, not measured: `ac3cli` measures it with the BS.1770 meter it has, so
  the library carries no second meter and does not link `ac3::forge` for one.
- **Objects**, from E9: each object's PCM and its metadata in Annex F's terms, which the
  applications convert from ADM BWF, IAB or `ObjectScene`.
- **The trace.** The encoder writes a `SyntaxRecord` for each element it writes, in the shape the
  decoder reads, under the lifetime rule D8 settled.

As built, `Encoder::create()` returns a `std::expected` in place of a constructor, and
`Encoder::refusal_reason()` names the rule a configuration breaks (E7). `RateControl` is `RateMode`.
`EncoderConfig` gained `channels`, `sample_rate_hz`, `iframes`, `fragment_starts`, `dialnorm_db`,
`loudness`, `drc`, `downmix` and `dialogue`, its substreams are `SubstreamConfig`s, each
`PresentationConfig` names one of Part 2 Table 53's configurations, and `experimental` is a struct of
flags in place of `experimental_tools`: `aspx_balance`, `aspx_varvar`, `aspx_interleave`,
`coding_configs`, `seven_x`, `acpl`, `back_pair`, `ajcc`, `drc_gains`, `three_zero` and `objects`. The
CRC is an argument of `sync_frame()`, `latency()` is `delay_samples()` and `decoder_delay_samples()`,
and `encode()` has overloads for a dialogue stem and for the metadata updates of objects.

### Rate control and the psychoacoustic model

- **The model.** Per scale factor band of the transform in use: a masking threshold from the band's
  energy, a tonality estimate, spreading across bands and a floor, and a perceptual entropy per
  block. It follows the published sources `ac3::quality`'s model cites, written anew for AC-4's bands
  and its fifteen transform lengths, and does not link it
  ([decision 17](#decisions-for-the-encoder-and-the-applications)). Phase E1 took out the threshold
  in quiet the plan first named: in the race it removed the top octave of speech and music that
  DEE's streams keep (`docs/verification.md`, "The encoder").
- **The loop.** Each frame gets a budget from the bit rate and, at an average rate, the buffer's
  fill, shared across the substream's channels, the parametric tools' data and the metadata. Scale
  factors and quantisation are chosen so each band's noise sits under its threshold where the budget
  allows, with the shortfall spread by perceptual entropy where it does not; sections and codebooks
  are chosen by their exact Huffman cost. Bits beyond what the thresholds need go first where the
  noise is loudest (E1).
- **The three rates.** A constant rate fills each frame to its size with fill bits; an average rate
  carries unused bits forward within the buffer Part 1 6.2.4 sets, and signals the wait
  `wait_frames` needs; a variable rate is an average rate without the buffer, in which frames lend
  each other up to two seconds' share of the rate (`wait_frames` 7).
- **The closed loop.** The rate loop is open: it estimates each band's noise from the quantiser's
  step (`src/ac4enc/src/asf/psycho.hpp`) and does not reconstruct the frame to measure it. The
  encoder runs the core's kernels where a tool's choice depends on what the decoder will make of it:
  A-SPX's envelopes, noise floors and tones against the HF generator (E2, E10), and A-JOC's dry
  matrices against its reconstruction (E9).

### What the encoder writes by default

Each tool's syntax falls in one of two sets. The first is the syntax DEE's streams exercise, which
the decoder's readings have been checked against on another encoder's output: SIMPLE and ASPX with
MDCT stereo processing and block switching, companding, FIXFIX, FIXVAR and VARFIX framing, the 5.X
element, ASPX_ACPL_2 and ASPX_ACPL_3, 5.1.4 in SCPL, ASPX_SCPL and ASPX_ACPL_2, DRC profiles and
curves, the channel-independent dialogue enhancement method, and one presentation. The second is the
rest, which only this project's transcriptions have read: noise fill, VARVAR framing, interleaved
waveform coding, ASPX_ACPL_1, A-CPL in a channel pair, the mono, 3.0 and 7.X elements, 7.1.4, A-JCC,
transmitted DRC gains, the other dialogue enhancement methods, several presentations and substreams,
and objects.

By default the encoder writes the first set only. The second is available behind options that name
it experimental, and each tool leaves that list when a reader independent of this project agrees
with the encoder's use of it: MediaInfo's trace for the table of contents, the presentation
substream and `metadata()`, and a second decoder for audio data
([decision 16](#decisions-for-the-encoder-and-the-applications)). Where a reading is settled that
way, the errata register records it.

As built, the second set is `EncoderConfig::experimental` (`ac3cli ac4-encode experimental=`):
balance, VARVAR framing and frequency-interleaved waveform coding in A-SPX (`aspx_balance`,
`aspx_varvar`, `aspx_interleave`), the 5.X element's other coding configurations (`coding_configs`),
7.0 and 7.1 (`seven_x`), ASPX_ACPL_1 and A-CPL in stereo (`acpl`), 7.0.4 and 7.1.4 (`back_pair`),
A-JCC (`ajcc`), transmitted DRC gains (`drc_gains`), a 3.0 substream (`three_zero`) and objects
(`objects`). Mono, several presentations and substreams, and the Mid and cross-channel dialogue
enhancement methods are options without the flag. Noise fill and time-interleaved waveform coding
are not written at all.

### One control for both formats

Where an E-AC-3 control and an AC-4 control are the same idea, Hearth shows one control. The
reading of both specifications gives this mapping. Dynamic range and the output level are shown
separately for each format ([decision 12](#decisions)), because AC-4's decoder modes and its
boosting output level are a different model from E-AC-3's line and RF modes.

| Control | E-AC-3 in the library today | AC-4 | Shown as |
|---|---|---|---|
| Programme | `DecoderConfig::programme` | presentation (id, name, language, content classifier, level) | one picker |
| Stereo downmix | `DownmixTarget` Lo/Ro, Lt/Rt with an optional 90-degree shift, mono | Lo/Ro, Lt/Rt (two methods, no shift), mono; the stream's preferred method | one control; the phase shift applies to E-AC-3 only |
| LFE in the downmix | `mix_lfe`, off by default | always mixed at `lfe_mixgain` | one toggle, on by default for AC-4; off drops the LFE term, outside the specification |
| Output layout | the renderer's layout | the channel renderer's output configurations | the renderer's control for both |
| Dynamic range | `OperatingMode` line, RF or custom; `drc_scale`; `heavy_compression` | DRC decoder mode (home theatre, flat panel TV, portable speakers, portable headphones), chosen by `Lout` or by the listener | separate controls for each format |
| Dialogue level | dialnorm normalisation to −31 dBFS, attenuation only | `Lout`, any level, boost allowed | separate controls for each format |
| Objects | `skip_object_reconstruction`, the player's auto, never and always policy | full or core decoding | one control: auto, never and always over full and core decoding |
| Concealment | `ConcealmentPolicy` | the same policies; output restored by the next I-frame | one control |
| Dialogue enhancement | none | `G_DE`, 0 dB up to the stream's cap of 3, 6, 9 or 12 dB | AC-4 only |
| Associated mix level | associated programmes are separate programmes, not mixed | `g_assoc`, −∞ to 0 dB | AC-4 only |
| Dialogue gain | none | `g_dialog`, −∞ to the stream's maximum | AC-4 only |
| Dual mono | the engine selects channel 1, 2 or both | none | E-AC-3 only |

### Media information

For each stream: the frame rate, internal and output sample rates, bitrate, I-frame interval and
splices. For each presentation: its index, id, name, language, content classifier, `md_compat`
level, channel layout, `b_pre_virtualized`, alternative and enabled flags, and substream groups. For
the selected presentation: dialnorm and the further loudness values; the DRC modes carried and how
(repeat, default profile, curve or transmitted gains); dialogue enhancement's method, channels and
cap; the downmix gains and preferred method; and, from the object phase, the objects and their
properties. `ac3cli probe json=1` writes the same fields.

### IEC 61937 and the extension role

- **The standard.** IEC 61937-14:2017 (edition 1.0, 26 pages) carries AC-4 over IEC 60958. IEC
  61937-2 Amendment 2 (2018) assigned `Pc` data type 24, with subdata types 0 (AC-4), 1 (HBR4),
  2 (HBR16) and 3 (LD), and not the extended data type mechanism. The base type fits a two-channel
  48 kHz link. The repetition periods and the AC-4 fields in `Pc` bits 8 to 11 are in Part 14's
  tables. ATSC A/342-2's maximum frame sizes match one sync frame per burst, repeating once per
  audio frame, exactly at every frame rate; D11 read Part 14, which says so (Annex A, and the
  repetition periods of Tables 7, 8, 13, 14, 19, 20 and 25).
- **Devices.** No receiver, soundbar or processor was found that accepts an AC-4 bitstream. Dolby
  wrote in 2021 that none existed, and that televisions and set-top boxes decode AC-4 and send PCM
  or Dolby MAT onward. No IEC 61937 AC-4 format exists in FFmpeg's S/PDIF muxer, Android's S/PDIF
  encoder, Linux's HDMI header or Windows' compressed-audio subformats. CTA-861 signals AC-4 as
  audio coding extension type 12.
- **In this project.** IEC 61937-14 is implemented from its text ([decision 9](#decisions)), which
  the user supplied on 2026-09-15. D11 gave `ac3::iec61937` the AC-4 burst types with their
  repetition periods and `Pc` fields, `PassthroughSink` an AC-4 format on the backends whose
  operating system has a way to send it (ALSA and Android), and the extension role
  `_ac3forge_player@v1` an AC-4 data type. The extension role is this project's own protocol, whose
  bursts drop IEC 61937's sync words and stuffing, and its receivers decode with this library. No
  device here can check passthrough.

### The ESP32

D14, after D10: the P4 first, in `float`, since it has the most CPU and memory of the family's
parts; then the S3 in `float` and the C6 in fixed point, as the decoder's optimisation lets them
([decision 24](#decisions-of-2026-09-25)). The ESP-IDF component builds AC-4 only with
`CONFIG_AC3FORGE_AC4` (D14b); the minimal profile carries the decoder for the Cortex-M3 probe
(D14a). The parts decode only; the encoder is built for none of them
([decision 34](#decisions-of-2026-09-25)). D14a and D14b are built, D14d is built short of its
board run, and D14c is not built.

- **What they face.** Every AC-4 codec mode, SIMPLE included, runs QMF analysis and synthesis on
  each channel (D3), and the A-CPL modes add three decorrelators per band. Read from the code as
  D6 left it, the QMF pair was about three quarters of a frame's arithmetic at 2.0 and 5.1, and a
  decoder held about 1 MB at 2.0 and 2 MB at 5.1 in `double`, where the S3's probe allows a heap
  of 245,000 bytes; D14a's figures for what it did about both are in its section. In E-AC-3,
  forge's JOC reconstruction through its QMF bank peaked at 449,826 bytes on the ESP32-S3, where a
  decode leaves a largest free block of 116,736 (`docs/platforms/bare-metal/esp32-s3.md`). A frame
  at index 13 lasts 42.7 ms.
- **What AC-3 and E-AC-3 manage on each part is the guide** to what AC-4 aims for there, less what
  AC-4's heavier frame costs. The aims below were read from the code before D14's work on it; the
  boards measure, and the last column has what they have measured.

| Part | Arithmetic | AC-3 and E-AC-3 on the board, per 32 ms frame | AC-4's aim | Measured |
|---|---|---|---|---|
| **P4**: 2 × RV32 at 360 MHz on this board's v1.3 silicon, single-precision FPU, 768 KB SRAM, 32 MB PSRAM | `float` | every fixture in real time: E-AC-3 5.1 at 0.18 of real time, 7.1.4 at 0.43 | 2.0, 5.1 and 5.1.4 in full decoding in real time, first | D14b: 2.0 in real time (SIMPLE at 0.53 of a frame, A-SPX at 0.74); 5.1 at 1.4 to 4.1 and 5.1.4 in full decoding at 2.8 to 3.7 |
| **S3**: 2 × LX7 at 240 MHz, single-precision FPU, 512 KB SRAM, 8 MB PSRAM | `float` | every fixture in real time: E-AC-3 5.1 at 0.34, 7.1.4 at 0.90 | 2.0 and 5.1; 5.1.4 measured, heard through core decoding or folded to 2.0 | not built (D14c) |
| **C6**: 1 × RV32 at 160 MHz, no FPU, 512 KB SRAM shared with WiFi, no PSRAM | fixed point | 5.1 with WiFi at 0.82 (AC-3) and 0.96 (E-AC-3); 7.1.4 misses, at 1.88 | 2.0 in core decoding if it keeps up with WiFi; otherwise Hearth sends it PCM | D14d and D14f: the decoder peaks at 286,365 bytes at 2.0 on a 32-bit core (429,667 at D14d), level with the about 285,000 free beside WiFi with WiFi's code in flash and below the 383,416 free with no network; the board not yet run |

- **The P4 here** is pre-production silicon: 400 MHz takes its CPLL down, and its I2S has no PLL
  clock, so it plays one or two channels over standard I2S and no TDM. On this board AC-4's
  multichannel decodes are heard folded to 2.0 until later silicon arrives.

## Verification

No reference output exists for AC-4, so correctness rests on several checks, each weaker than a
comparison with a reference decoder would be. AC-3 and E-AC-3 are checked against FFmpeg's decoder
and Dolby's encoder; AC-4 has Dolby's encoder here and no Dolby decoder, and FFmpeg does not decode
it. This section names what each outside program can check, then the two ladders: the decoder's,
and the encoder's.

### The oracles

| Oracle | What it checks | For the decoder | For the encoder | Where it runs |
|---|---|---|---|---|
| The two transcriptions, C++ and `ac4_syntax.py` | every syntax element, and the size invariants | DEE's, third-party and constructed streams | every stream it writes, against its own write trace as well | CI |
| DEE | encodes of known sources | its streams, scored against their source | the race: the same sources at the same settings | locally, while its licence runs ([decision 23](#decisions-for-the-encoder-and-the-applications)) |
| librempeg | PCM from a second decoder | its decode of the same streams | its decode of the encoder's streams | locally ([decision 13](#decisions-for-the-encoder-and-the-applications)) |
| MediaInfo | the table of contents, the presentation substream, `metadata()`, EMDF payloads and `crc_word`, frame by frame | DEE's and third-party streams | each value the encoder was asked to write | locally |
| FFmpeg 8.0.1 | sync frames in raw AC-4; the AC-4 track of an MP4 file | framing only | framing of raw output and of MP4 and TS carriage | CI, in FFmpeg Validate, which runs in the nightly tier ([the CI stages](../docs/ci-agentic.md#the-tiers)) |
| DEE's MP4 muxer | reads an AC-4 elementary stream to write its sample entry | | the encoder's raw output muxes, and the `dac4` it writes equals the encoder's | locally |
| The decoder | PCM | | every stream the encoder writes | CI |
| Listening | rendering and objects, where numbers do not reach | D10 | E9 | locally, by the user: not done, and each phase's pull request gives the streams and the commands |

**FFmpeg cannot be the second decoder.** For AC-3 and E-AC-3, FFmpeg's decoder is what the gold
gate, the FATE interoperability run and the encoder-space harnesses compare against. FFmpeg has no
AC-4 decoder or encoder ([Material](#material)), so for AC-4 it checks framing: its raw demuxer must
find every sync frame the encoder writes, at the size written, and its mov demuxer must read the
AC-4 track of the encoder's MP4 output. Its raw muxer's CRC is not a reference.

**librempeg is the second decoder** ([decision 6](#decisions), confirmed in decision 13). It is
built in WSL under `D:\ac3bld\librempeg` from a pinned commit, with the compiler, `make` and
`pkg-config` already there; `nasm` is not, so it is built without assembly for a generic
architecture, which costs speed and nothing else (its x86 build without assembly does not link). Its
`ffmpeg` program builds only with `--enable-agpl`, which puts that program under the AGPL; it runs
here as a separate tool and is never linked or distributed. Its binary is named `ffmpeg`, and the
gold gate, `quality_race.py`, `fuzz/differential_oracle.hpp` and most other scripts call FFmpeg by
name from `PATH`, so it is only ever called by its full path, never put on `PATH`, where it would
replace the pinned FFmpeg 8.0.1. Before its output counts as evidence for a tool, it decodes the
committed and census DEE streams and is scored against their sources as the decoder is; that
measurement says which tools it is a second decoder for. Its source is never read, its tag and
commit are recorded with each comparison, agreement with it is evidence, and a disagreement is
settled from the text.

**MediaInfo's trace is a third reader.** A script compares MediaInfo's `--Details=1` values, frame
by frame, with the decoder's trace of the same elements. On DEE's streams that checks the decoder's
reading; on the encoder's it checks the encoder's writing against a reader outside this project,
which is what moves a tool out of the encoder's experimental set
([decision 16](#decisions-for-the-encoder-and-the-applications)). It runs locally, from DEE's
install.

**DEE's licence runs out on 2026-11-06.** DEE is the gold standard in both directions, so phases
G0 and G1 ([G0](#g0-the-gold-set)) made every DEE stream the later phases need before that date,
and G2 (#1054) the same for its AC-3, E-AC-3, E-AC-3 JOC and TrueHD encoders; the licence is not
renewed.

The Dolby Reference Player's `dlbac4dec` stays installed and unused ([decision 5](#decisions)).

### The decoder's ladder

The checks are listed from the one that proves most to the one that proves least; each phase names
the ones it uses.

1. **Two transcriptions of the syntax agree on encoded streams.** The C++ decoder and the Python
   reference parser, each transcribed from the text separately, write a field-by-field dump of
   every frame, and the dumps must be identical over every stream in the set. Two invariants back
   this without relying on either transcription: every `audio_data()` ends inside its `audio_size`
   with only fill and alignment bits left, and every `metadata()` and `drc_frame()` ends where
   `tools_metadata_size`, `drc_metadata_size` and the substream size say. IM4 found three
   field-order bugs this way, and the census walk held these invariants on 100 streams. A reading
   the two transcriptions share passes, as the `presentation_config` 6 early return did.
2. **Each fast DSP path equals the clause's own formula.** The inverse MDCT at every length and
   window transition, the QMF analysis and synthesis, the KBD windows and the decorrelators are
   tested against a direct evaluation of the formula in the text, to 1e-12 relative. The QMF pair
   with `QWIN` reconstructs to about 78 dB with a delay of 577 samples.
3. **Decoded DEE streams are scored against their source.** DEE encodes known WAVs, from the
   committed programme fixtures and from synthetic signals, with its audio-altering defaults off:
   the committed streams were made with its default loudness correction to −24 LKFS, a gain applied
   to the audio, so G0 makes them again with loudness measured only. The decoded output is aligned
   by cross-correlation and fitted with a least-squares gain. The gain must be within 0.2 dB of what
   dialnorm and `Lout` predict, which settles Part 1's unstated full-scale convention and a factor
   of two between its IMDCT pseudocode and its informative example. Then:
   - waveform-coded channels and bands (SIMPLE mode, and below A-SPX's crossover): per-channel SNR
     against the source, with floors pinned at the first measurement less 1 dB, as
     `tools/checks/derive_channel_floors.py` derives E-AC-3's;
   - A-SPX bands: the decoded energy in each envelope's time and frequency tile against the
     source's, within the envelope quantiser's step;
   - A-CPL and S-CPL channels: per parameter band, the decoded level difference and correlation of
     each reconstructed pair against the source's;
   - whole signals: Bark-band log-spectral distance and ViSQOL MOS-LQO through
     `tools/ci/quality_race.py`, pinned per stream;
   - routing: one tone per channel, the method that settled DEE's `cbi_wav` channel order.

   These measure closeness to the source. They catch a wrong channel, level, delay, polarity, band
   or envelope, and a hole in the spectrum. An error that keeps the output close to the source,
   such as a small misreading in a decorrelator or an interpolation, can pass them.
4. **Metadata-driven gains match their formulas.** Dialogue enhancement, DRC's output level gain,
   transmitted DRC gains, downmix matrices and mixing gains are measured on decoded output, with
   one tone per channel, against the formula applied to the parsed values, to 0.01 dB.
5. **Third-party streams decode, and their metadata matches MediaInfo's trace.** DASH-IF's, CTA
   WAVE's, DVB's, Dolby's delivery kit's and Chromium's streams come from encoders other than DEE
   6.5.4, and from frame rates the plain encoder does not write. Without a source to score against,
   they are checked by the invariants in 1, by MediaInfo's trace field by field, and by listening
   ([decision 4](#decisions)).
6. **Constructed streams reach what no encoder here writes.** A test multiplexer builds
   presentations of several substreams from the substreams of separate DEE encodes, rewriting their
   `extended_metadata`. The encoder's writer, driven directly by the tests
   (`tests/ac4dec/ac4dec_constructed.cpp`), builds syntax no encoder here writes by default: A-CPL
   mode 1, A-JCC, transmitted DRC gains, and dialogue enhancement methods 1 to 3. The encoder's own
   streams reach it too. These check parsing, the invariants, and gains on known input; the writer,
   the encoder and the decoder can share a misreading.
7. **A second decoder.** librempeg's experimental AC-4 decoder is built outside the tree and its
   output compared on the same streams ([decision 6](#decisions)), as [The oracles](#the-oracles)
   sets out; the Reference Player is not pursued ([decision 5](#decisions)). At
   `frame_rate_index` 13, which needs no converter, and with DRC off, two correct decoders should
   agree closely; at other rates their converters differ. librempeg is experimental, so a
   comparison is recorded in each phase's pull request and does not gate it, and a disagreement is
   settled from the text, never from librempeg's source, which is not read.
8. **Robustness.** A fuzz target over the decoder, instrumented; truncated, corrupt and spliced
   streams; the address, undefined-behaviour and thread sanitizer legs.

### The encoder's ladder

Every stream the encoder writes goes through the decoder's checks and through these, from the one
that proves most to the one that proves least:

1. **Three transcriptions agree.** The encoder's own trace of what it wrote, the decoder's trace and
   the Python parser's trace of the stream must be identical, record for record, with the invariants
   of the decoder's item 1 holding. The writer, the decoder's reader and the Python parser are
   transcribed separately; a reading all three share still passes, which is what items 3 to 5 are
   for.
2. **Its DSP equals the formulas.** The forward MDCT against a direct evaluation, and the forward
   and inverse transforms together reconstructing to 1e-12 relative at every length and split; the
   QMF analysis as in the decoder's item 2; and the encoder's converter followed by the decoder's
   returning the input's sample count exactly over 100,000 frames at every frame rate.
3. **Readers outside the project read it as configured.** MediaInfo's trace of each encoded stream
   matches the configuration the encoder was given, field by field: the table of contents,
   presentations, loudness, DRC, dialogue enhancement, downmix, EMDF payloads and the CRC. FFmpeg's
   demuxers read the raw and MP4 output. DEE's MP4 muxer takes the raw output and writes a `dac4`
   equal to the encoder's.
4. **Decoded, it scores against its source.** The decoder decodes every stream, and librempeg
   decodes those whose tools it was found to decode; the scoring of the decoder's item 3 applies:
   per-channel SNR below A-SPX's crossover, A-SPX tile energies, A-CPL level differences and
   correlations per band, log-spectral distance and ViSQOL, and one tone per channel, each pinned at
   the first measurement.
5. **The race.** The same sources at the same layouts, rates and frame rates are encoded by DEE,
   with its audio-altering defaults off, and by the encoder; both are decoded by the decoder and by
   librempeg, and scored against the source as in item 4. The encoder's scores and its gap to DEE's
   are recorded per leg and pinned against regression
   ([decision 19](#decisions-for-the-encoder-and-the-applications)). As built, `score_ac4_encode.py`
   holds the encoder's scores to pinned floors in FFmpeg Validate, which runs nightly, and its
   `--gold` race against DEE runs locally, with DEE's streams and ViSQOL; `quality_race.py` has no
   AC-4 legs. The decoder's scores have a trend series of 10 legs, which the nightly run writes
   ([Quality trend](../docs/quality-trend.md#ac-4-decode-quality)); the encoder's scores, a
   comparison with other tools and AC-4's objects have none. AC-4 does have speed, allocation and
   kernel series ([Performance trend](../docs/performance-trend.md)).
6. **Metadata gains match their formulas.** Through the decoder's output processing (D6), the
   encoder's streams give the dialogue enhancement, output level, downmix and mixing gains their
   configuration asks for, to 0.01 dB, with one tone per channel.
7. **The encoder space.** A harness in the pattern of `tools/ci/fuzz_eac3_encoder_space.py` draws
   configurations (layout, rate, frame rate, codec mode, tools, presentations and metadata) and
   short signals, encodes them, and requires items 1 and 3's FFmpeg framing and a clean decode.
   Seeds that once failed are replayed as regressions; it runs for 120 seconds in FFmpeg Validate
   and 900 in the nightly fuzz workflow, as E-AC-3's does. MediaInfo's readings come from
   `tools/checks/check_ac4_encode_readers.py`, which runs locally, from DEE's install.
8. **Robustness and determinism.** An instrumented fuzz target over the encoder's configuration and
   input (silence, DC, full-scale square waves, clipped signals, and non-finite samples, which are
   refused); the sanitizer legs; and the same bytes from the same input and configuration on one
   toolchain. As built, that is held for the committed streams the encoder wrote, which the tests
   rebuild and compare byte for byte; `tests/golden/bitstream-hashes.json` has no AC-4 entries.

**What goes in the tree.** Short DEE streams (about five seconds each) under `tests/golden/`, with a
generator that makes them from committed sources as `tools/generators/gen_ac4_baseline.py` does,
and never runs in CI. As built, each stream's manifest entry records its source's SHA-256 and the
properties its trace shows, and the scorers pin the scores. The encoder's streams are committed
only as cases for syntax that no DEE stream reaches (`tests/golden/ac4dec/constructed`,
`presentations` and `objects`, with the Python parser's digests), which the tests rebuild byte for
byte; CI makes the rest, and no hashes of them are pinned. The full census, the gold set (G0, G1)
and librempeg's outputs stay local on `D:`, and so do the third-party streams: none is committed.

## Phases

### Order

G0 came first, because DEE's licence runs out on 2026-11-06. After it, each encoder phase followed
the decoder phase that decodes what it writes, so that what the two share is built once, with both
directions' tests, and each encoder phase is checked by a decoder the phase before checked on DEE's
streams ([decision 14](#decisions-for-the-encoder-and-the-applications)). The applications follow
the channel-based library. The table is the order the phases were planned in, with what each needs
and, in the last column, where it stands; [State on 2026-09-30](#state-on-2026-09-30) has the dates.

| # | Phase | What it builds | Needs | State |
|---|---|---|---|---|
| 1 | [G0](#g0-the-gold-set) | the gold set: every DEE stream the phases need | | merged, #1006 |
| 2 | [D2](#d2-waveform-coded-stereo-to-pcm) | waveform-coded stereo to PCM, and the shared core | D1 | merged, #1009 |
| 3 | [E1](#e1-the-encoder-library-the-frame-writer-and-simple-mono-and-stereo) | the encoder library, the frame writer, SIMPLE mono and stereo | D2 | merged, #1011 |
| 4 | [D3](#d3-the-qmf-domain-and-a-spx) | the QMF domain and A-SPX | D2 | merged, #1012 |
| 5 | [E2](#e2-a-spx-and-companding) | A-SPX and companding | D3, E1 | merged, #1013 |
| 6 | [D4](#d4-the-5x-element) | the 5.X element | D3 | merged, #1014 |
| 7 | [E3](#e3-the-5x-element) | the 5.X element | D4, E2 | merged, #1025 |
| 8 | [D5](#d5-a-cpl) | A-CPL | D4 | merged, #1027 |
| 9 | [E4](#e4-a-cpl) | A-CPL | D5, E3 | merged, #1029 |
| 10 | [D6](#d6-output-processing) | output processing | D5 | merged, #1037 |
| 11 | [E5](#e5-metadata-frame-rates-and-i-frames) | metadata, frame rates and I-frames | D6, E4 | merged, #1046 |
| 12 | [D7](#d7-presentations) | presentations | D6 | merged, #1055 |
| 13 | [E6](#e6-presentations-and-several-substreams) | presentations and several substreams | D7, E5 | merged, #1058 |
| 14 | [D8](#d8-the-api-the-cli-media-information-and-packaging) | the decoder's API, CLI, media information and packaging | D7 | merged, #1059 |
| 15 | [E7](#e7-the-encoders-api-the-cli-and-packaging) | the encoder's API, CLI and packaging | D8, E6 | merged, #1061 |
| 16 | [I1](#i1-the-rest-of-ac3cli) to [I4](#i4-the-c-api-python-rust-and-webassembly) | the applications, for channel-based content | D8, E7 | merged, #1070, #1068, #1084, #1094 |
| 17 | [D9](#d9-channel-based-immersive) | channel-based immersive | D8 | merged, #1057 |
| 18 | [E8](#e8-channel-based-immersive) | channel-based immersive | D9, E7 | merged, #1071 |
| 19 | [D10](#d10-a-joc-objects) | A-JOC objects | D9 | merged, #1060 |
| 20 | [E9](#e9-a-joc-objects) | A-JOC objects | D10, E8 | merged, #1082 |
| 21 | [I5](#i5-immersive-and-object-content-in-the-applications) | immersive and object content in the applications | D10, E9 | merged, #1100 |
| | [I4b](#i4b-the-object-encoder-in-the-c-api-python-rust-and-webassembly) | the object encoder in the bindings, and I4's leftovers | I4, E9 | merged, #1119 |
| 21b | [I5b](#i5b-the-encoder-pages-ac-4-objects) | the encoder page's AC-4 objects | I3, I5 | merged, #1117 |
| 22 | [D14](#d14-ac-4-on-the-esp32s) | AC-4 on the ESP32s: the P4 first, then the S3 and the C6 | D10 | D14a and D14b merged (#1096, #1102, #1123, #1118); D14d and D14f open, the board run to come; D14c not built |
| | [D14a4](#d14a4-libm-parity-and-the-converter-at-float) | libm parity and the frame-rate converter at `float`: what D14b's board work left in D14a's build | D14a, D14b | open |
| 23 | [I6](#i6-the-esp32-sinks) | the ESP32 sinks | each part's D14 figures | not built |
| 24 | [N1](#n1-the-names) | the names | I5 | built in the repository, 2026-09-30 and 2026-10-01 (#1160 to #1164, S5 and S6); the owner's renames are left |
| | [G1](#g0-the-gold-set) | the golden masters, extending G0's set | any time before DEE's licence ends on 2026-11-06 | merged, #1051 |
| | [D11](#d11-ac-4-over-iec-61937) | AC-4 over IEC 61937 | D1; any time | merged, #1052 |
| | [E10](#e10-a-spx-noise-floors-on-sweeps) | A-SPX noise floors on sweeps: the gap E8 left to DEE | E2, E8 | merged, #1113 |

Hearth's AC-4 pages activate for channel-based content in I2, for immersive content and objects in
I5, and are live.

How each phase is run:

- Before each phase: `gh pr list`, and `ListAgents` for a session already on it; the collision a
  pull request list misses is two sessions in the middle of the same work.
- One pull request per phase, branched from main, named `feature/` or `bugfix/`, with no
  attribution lines; the pull request links its phase on this page and states the exit criterion
  and what verified it.
- From D2 on, each phase's pull request records the comparison with librempeg; from E1 on, each
  encoder phase's pull request records the race against DEE.
- Every reading taken where the text is ambiguous goes into `src/ac4dec/ERRATA.md`, and both
  transcriptions take it. A reading only the writer needs, such as the value of a field decoders
  ignore or an order the text leaves open, goes into `src/ac4enc/ERRATA.md`, which points at the
  decoder's entry wherever both depend on one reading.
- Each phase updates what it changes of the support catalogue, the status table, the capabilities
  and validation pages, CHANGELOG and ROADMAP.
- Before pushing: the MSVC `/W4 /WX` build, the touched translation units under clang-cl, and the
  WSL GCC and Clang `-Werror` gates; for documentation, `tools/checks/check_doc_paths.py` and
  `mkdocs build --strict`.
- From 2026-09-25 the phases left ran in parallel, as agents under one session that coordinated
  them, each in a worktree of its own made at its base and working from a common brief of these
  rules. A phase whose predecessor was still open branched from it and said so at the top of its
  pull request; a phase built beside the one the order puts first (D9 beside D7) took that phase
  in by merging. Pull requests were merged when their checks passed, the lowest of a stack first.

### G0: the gold set

**Status:** merged as #1006 on 2026-09-25. Exit met: 137 streams (45.5 MB, 32,787 frames, from 10 s
sources) in `D:\ac3bld\ac4-gold`, both transcriptions reading every substream to its end but the
immersive element of the 22 5.1.4 streams, which D9 took on, and MediaInfo's trace beside each.
`dee_ac4ajoc_encoder` and `dee_ac4ims_encoder` refused the ADM master, so the set has no A-JOC
stream.

Every DEE stream the later phases need, made while DEE's licence runs, and kept on `D:`. The
generator is `tools/generators/gen_ac4_baseline.py`, extended; it runs locally and never in CI.

- **Sources** rebuilt by the generator from committed material, so that any stream can be scored
  again later: the programme fixtures, synthetic signals (one tone per channel, sweeps, noise, a
  silence-then-transient), and the 5.1 and 5.1.4 mixes the generator already builds.
- **Scoring legs**: every layout and rate DEE writes, from 2.0 at 48 kbps to 5.1.4 at 768, with
  loudness measured only, so that a decode can be scored against its source; the immersive stereo
  encoder at each of its frame rates.
- **Race legs**: the programme fixtures and synthetic signals at the layouts and rates the encoder
  phases race at, with the same settings.
- **Metadata legs**: each DRC profile in each decoder mode, each preferred downmix and mix level,
  the height downmix, the loudness presets, the general and music modes, and the I-frame intervals.
- **Objects**: one attempt with `dee_ac4ajoc_encoder` and `dee_ac4ims_encoder` on the ADM BWF
  masters `ac3cli decode` writes (roadmap IM2). DEE refused an earlier master of this project's on
  provenance, so a refusal is the likely result, and it is recorded with DEE's message. If either
  accepts one, A-JOC streams at levels 3 and 4, and immersive stereo from objects, join the set.
- **A manifest** of every stream: its source, the command, DEE's log and version, and MediaInfo's
  trace.

The committed streams (about five seconds each) are the scoring legs D2 to D6 use, replacing or
beside the eleven made with DEE's default loudness correction; changed streams take new digests in
`tests/golden/ac4dec/`, from the Python parser.

**Exit:** every stream in the manifest is on `D:` with its source rebuildable, both transcriptions
read each one to the end of every substream (5.1.4's audio refused, as now), and MediaInfo's trace
is saved beside each.

**Verified by:** the generator's own checks; the census comparison in `test_ac4dec_syntax.cpp` over
the set.

**G1, the golden masters (merged as #1051 on 2026-09-25; exit met under G0's criterion).** On
2026-09-25 the user said DEE's licence will not be renewed and asked for golden masters to test
against now. G1 listed 439 legs beside G0's in `D:\ac3bld\ac4-gold` under the manifest's `g1_legs`
(gold version 4): 427 made (433 streams, 188 MB, 133,163 frames) and 12 attempts DEE refuses, kept
with its messages. G0's legs, sources and files are as G0 left them, and the scorers that read
`legs` fail a leg they have not pinned, so a phase takes G1's legs into its scorer as it pins
them. `gen_ac4_baseline.py` makes each leg from committed material and groups them by the phases
they serve:

- D2 to D5 and E1 to E4: sweeps, pink noise and silence-then-transient at every 2.0 and 5.1 rate.
- D9 and E8: film, speech, sweeps, noise and transients at every 5.1.4 rate; stepped tones under
  each DRC profile, the preferred downmixes, and every mix level and every height downmix mode and
  gain on one tone per channel; the loudness presets and I-frame settings. In all 40 streams of the
  five sources, the immersive codec mode follows the rate alone: ASPX_ACPL_2 from 192 to 448 kbps,
  ASPX_SCPL at 512 and SCPL at 768.
- D6, E5 and D11: immersive stereo at every rate at each frame rate, from music, film, tones,
  sweeps, noise and transients, with the I-frame intervals each frame rate allows; and one
  programme through the immersive stereo encoder's gapless encoding, in three parts at the native
  rate, 25 and 29.97 fps, whose streams meet at splices DEE made.
- D6 and E5: stepped tones under each DRC profile at 2.0 and 5.1; loudness corrected to targets
  from −31 to −10, where dialnorm equals the target; the four loudness presets, dialogue
  intelligence and the speech threshold; I-frame intervals and forced I-frames; every mix level at
  5.1; and immersive stereo's DRC profiles, music mode and loudness presets.
- D7 and E6: substreams for the test multiplexer, a dialogue tone, an associated tone, and dialogue
  and associated speech in 2.0, on the I-frame grid of G0's 2.0 and 5.1 legs; immersive stereo with
  seven language tags, on six sources.
- D12, D13 (whose work D14 took) and I6: 60 s programmes, 2.0 from 48 to 256 kbps, 5.1 from 96 to
  448, 5.1.4 at 192, 256, 512 and 768, and immersive stereo at five frame rates.
- I1 and I5: E-AC-3 and AC-3 from `dee_ddp_encoder`, and E-AC-3 JOC from `dee_ddpjoc_encoder`'s
  channel-based immersive input at 5.1.4, 7.1.4 and 9.1.6, of the same sources; 7.1 input; and
  DEE's own downmix of 5.1 and 7.1 input to 2.0.

Each leg keeps DEE's command and log, MediaInfo's trace with a table of contents for every frame
(`--ParseSpeed=1`) and its summary, and DEE's MP4 of the stream; the manifest holds the SHA-256s
and what main's `ac3cli` made of each stream, and G0's streams have their MP4s under `mp4\`. Both
transcriptions agree on all 396 AC-4 streams (the census comparison). On the day G1 merged main's
decoder decoded each 2.0 and 5.1 stream at index 13 and refused the 5.1.4 streams' immersive
element (D9) and the other frame rates (D6), which is where those phases started. Three 5 s streams
of one tone per channel at 5.1.4, one in each immersive codec mode, are committed; their digests
came with D9.

What DEE could not be made to write:

- **Objects.** `dee_ac4ajoc_encoder` and `dee_ac4ims_encoder` take objects only in an Atmos master.
  With #1007's `bitDepth`, the masters `ac3cli` writes pass the check G0 failed, and
  `dee_ac4ajoc_encoder` at both levels, `dee_ac4ims_encoder`, `dee_ddpjoc_encoder` and
  `atmos_info` all stop at the next: "Content was not authored with Dolby tools"
  (`dlb::isAtmosMezzFile`), for G0's music master and for the committed reference objects at their
  authored positions alike; a bare IAB file gets "ATMOS_STORAGE_RES_UNSUPPORTED_MASTER_TYPE". The
  check is on provenance and is not worked around, and DEE reports nothing about a master past it,
  so no defect of the ADM writer is known. D10 and E9 rest on the evidence their sections name;
  the object-coded material DEE does write is E-AC-3 JOC from channel beds, a bed of 12 objects
  from 5.1.4 or 7.1.4 and of 16 from 9.1.6, with no dynamic objects.
- **7.1 and above.** `dee_ac4_encoder` writes 7.1 input as 5.1, each surround the average of its
  side and back channels (−6 dB each), L, R and C unchanged; it refuses 7.1.4 and 9.1.6
  (`cbi_wav` of 12 and 16 channels) and input of 1, 3, 4 or 5 channels, so mono, 3.0 and 5.0 are
  not available either. The 7.X element stays with the encoder's constructed streams (D4, E3).
- **Frame rates.** Only the immersive stereo encoder writes a rate other than index 13, indices 0
  to 3, and its music mode refuses them ("Music mode requires native frame rate").
- **Presentations.** No encoder, option or template writes several presentations, music and
  effects with dialogue, associated audio or a dialogue enhancement substream. The immersive stereo
  encoder's language tag is the one presentation field a caller sets: it sends the whole tag in
  I-frames and its primary subtag in the others (`fr` in the frames between `fr-CA`), and writes
  `eng` as `en`.

Worth knowing from the same runs: the immersive stereo encoder levels input it measures above
about −16 LKFS, so G1's sources for it are 6 dB below the plain encoder's; a forced I-frame lands
one frame after the index its list names, since DEE's frame 0 is a priming frame; under
`measure_only` with a loudness preset, the plain encoder writes the preset's target as dialnorm
(−24, or −23 for R128) whatever the measurement (−20.9 on the 2.0 music), while the immersive
stereo encoder writes its measurement and A/85's practice whatever the preset; and DEE's
encode-time downmix of 5.1 to 2.0 is Lo/Ro at the default −3 dB levels with the LFE dropped.

A phase after 2026-11-06 cannot get a DEE stream of any configuration outside the set, DEE's MP4
muxer's `dac4` for the encoder's own streams if the muxer stops with the licence (the encoder's
ladder item 3; the set holds DEE's MP4 of each DEE stream, so a configuration's `dac4` can still be
compared), or new E-AC-3 and AC-3 encodes for `gen_external_baseline.py`, whose encoders are the
same install. G2 (#1054, 2026-09-26) made the last for AC-3, E-AC-3, E-AC-3 JOC and TrueHD with
`tools/generators/gen_dee_gold.py`: 1,111 legs (1,086 streams, 986 MB) in `D:\ac3bld\dee-gold`.

### Decoder phases

#### D1: the library and the channel-coded syntax

**Status:** merged as #700 on 2026-09-16, with the readings of #712 and #715; the HSF extension's
content followed in #786. Exit met: every criterion below, the third-party streams included.

- `src/ac4dec/` with its CMake, tests and an instrumented fuzz target, and the inspector additions
  under [What the inspector grows](#what-the-inspector-grows).
- The whole syntax of channel-coded substreams in the Part 1 channel elements, from Part 1 clause 4
  and Part 2 clause 6: ASF section, spectral, scale factor and noise fill data decoded to quantised
  values; stereo processing parameters; companding; A-SPX and A-CPL data; `basic_metadata`,
  `further_loudness_info`, `extended_metadata`, `drc_frame` and `dialog_enhancement` in full;
  EMDF payload configuration; and the presentation substream. SSF is detected and refused with a
  named error ([decision 2](#decisions)).
- Tables from the attachments, and from the PDF with each transcribed table checked against a
  rendering of its page.
- The Python reference parser grows the same syntax, transcribed separately from the text and not
  from the C++.
- The errata register (`src/ac4dec/ERRATA.md`), and the generator for the committed streams.
- As built, also: a differential check (`tools/checks/ac4_syntax_differential.py`) that compares
  the two transcriptions' traces on mutated DEE frames, synthetic tables of contents and fuzzing
  corpora, where no encoded stream reaches the syntax.

**Exit:** every frame of every 2.0, 5.1 and IMS stream in the census set, and of the third-party
channel-based streams, parses in both implementations with the invariants of
[Verification](#verification) item 1 holding, and the two dumps are identical. Every Huffman
codebook is a complete prefix code. The fuzz target runs its configured budget clean.

**Verified by:** `ac3tests` on every leg; the dump comparison in CI over the committed streams and
locally over the whole set; the fuzz target.

#### D2: waveform-coded stereo to PCM

**Status:** merged as #1009 on 2026-09-25. Exit met.

- The shared core, `src/ac4core/` ([Where the libraries live](#where-the-libraries-live)): D1's
  generated tables moved into it, and this phase's transforms are written there, the forward MDCT
  beside the inverse, so that E1 finds them built and tested. The bit reader and the Huffman
  decoder stayed in `src/ac4dec`, since they carry the syntax trace and its sink.
- ASF reconstruction: scale factors, dequantisation, noise fill, ungrouping.
- Stereo processing: M/S and prediction.
- The inverse MDCT on the FFT for 2^a · 3^b · 5^c, the direct form it is tested against, KBD
  windows, block switching, overlap-add and the frame alignment delay.
- Mono and stereo in SIMPLE mode. `ac3cli decode` reads AC-4 and writes the coded channels, and
  `tools/checks/score_ac4_decode.py` scores the decoder's output against the sources with
  `quality_race.py`'s metrics. Frame rates other than index 13 were refused until D6.
- librempeg, built as [The oracles](#the-oracles) describes (G0 had built it, once the user had
  confirmed it, decision 13), and measured on the committed and census streams.

**Exit:**

- The fast inverse MDCT and the forward MDCT each equal their direct forms to 1e-12 relative at
  every length and every block transition Part 1 Table 187 allows, and the windows reconstruct
  perfectly across them.
- Every DEE 2.0 stream from 192 to 768 kbps in G0's set, music and speech, decodes with its level
  within 0.2 dB of the prediction, its per-channel SNR at or above its pinned floor, its alignment
  at the computed latency, and each channel's tone on its own channel.

**Verified by:** `ac3tests`; `score_ac4_decode.py` over the committed streams in FFmpeg Validate,
and over the full set locally.

#### D3: the QMF domain and A-SPX

**Status:** merged as #1012 on 2026-09-25. Exit met.

- The QMF bank with `QWIN`, following Pseudocode 66's modulation offset; control data held for its
  one, two or four frames. Every codec mode passes through the banks, SIMPLE included, as Part 1
  Figure 9 draws it, which gives the decoder one delay, 1,313 samples at index 13: DEE's SIMPLE and
  ASPX streams and librempeg's decodes of them each show one delay for both modes (D3).
- Companding.
- A-SPX in full: the subband group tables, framing, envelope decoding, the HF generator,
  envelope adjustment with its limiter, the noise and tone generators, and interleaved waveform
  coding.
- The ASPX mode for mono and stereo.

**Exit:**

- The QMF pair and the direct formula agree to 1e-12 relative, and the pair reconstructs to 78 dB
  at 577 samples.
- Every DEE 2.0 stream from 48 to 144 kbps (companding on and off) and every IMS stream at index 13
  decode. Below each stream's crossover, per-channel SNR at or above the pinned floor. Above it,
  each A-SPX tile's energy within the quantiser step of the source's, plus a tolerance fixed at the
  first measurement. Log-spectral distance and ViSQOL at or above their pinned floors.
- The IMS streams, made from 5.1, have no stereo source. Their decode is scored against the
  source's Lo/Ro and Lt/Rt downmixes after a gain fit; if neither correlates, they are held to the
  invariants and to librempeg's decode, and the reason is recorded. D3 found them a frame earlier
  than the AC-4 encoder's streams and correlating with Lo/Ro at 0.98.

**Verified by:** as D2.

#### D4: the 5.X element

**Status:** merged as #1014 on 2026-09-25. Exit met.

- `5_X_channel_element` in SIMPLE and ASPX modes: the LFE path, the multichannel matrices (Part 1
  Tables 178 to 185), `2ch_mode`, and A-SPX's channel pairing. The 3.0 and 7.X elements in the same
  modes, tested on constructed streams.
- D4 found that DEE's 5.1 streams use one form of the element, `coding_config` 0 with `2ch_mode` 0.
  The other forms, and the 3.0 and 7.X elements, are tested on streams built with the encoder's writer,
  whose tracks are the channels through the inverse of the printed matrices; twelve are committed with
  the Python parser's digests.
- DEE low-passes the LFE before it codes it, with the phase of a filter near 120 Hz, which librempeg's
  decode shows as well. The LFE is held to its level from 20 to 100 Hz and to librempeg's decode, and
  its SNR against the source is pinned as measured.
- The 5.1 centre's A-SPX band exposed D3's pre-flattening: as printed, the patch takes the inverse of
  the gain that flattens the low band's slope, which doubles the slope, and the top group of film's
  centre came out 4.6 dB under the source at 256 kbps, lost to the limiter, and 10.9 dB under at 192.
  D4 flattens the patch, in the decoder and in the encoder's analysis, and measures both scorers' ASPX
  legs again (`src/ac4dec/ERRATA.md`, "Pre-flattening's direction").

**Exit:** every DEE 5.1 stream from 192 to 768 kbps decodes with the checks of D2 and D3 on each
channel, and one tone per channel lands on its own channel, the LFE included. Constructed 3.0 and
7.X streams parse in both implementations.

**Verified by:** as D2.

#### D5: A-CPL

**Status:** merged as #1027 on 2026-09-25. Exit met.

- The three decorrelators (Part 1 Tables 199 to 201), the transient ducker, interpolation and
  dequantisation.
- ASPX_ACPL_2 and ASPX_ACPL_3 in the 5.X element. ASPX_ACPL_1, A-CPL in a channel pair, and the
  7.X modes, tested on constructed streams.
- D5 found that DEE's 5.1 streams send 15 parameter bands at fine quantisation and one parameter set
  a frame, interpolated smoothly in all but a few frames, and difference along frequency in every
  I-frame. librempeg puts out their coded pair as L and R and leaves Ls and Rs silent, so the source
  is the only reference. The decorrelators, the ducker, interpolation and the dequantisation tables
  are in `src/ac4core` for E4, and the encoder's writer writes A-CPL's syntax for the constructed
  streams, eight of them committed; E4 builds its parameter extraction on both.
- Scored against the source, DEE's streams settled two readings the text leaves open: a frame's
  parameters apply d_ctrl frames later, with its A-SPX data (a frame early or late takes the level
  difference's distance from the source from 2.6 dB to 3.8 and 3.6), and the transient ducker weighs
  its own input, the decorrelator's output, though it barely moves these measures
  (`src/ac4dec/ERRATA.md`, "A-CPL").

**Exit:**

- Each decorrelator's impulse response equals its difference equation, and its magnitude response
  is flat to 1e-9.
- Every DEE 5.1 stream at 96, 128 and 144 kbps decodes. For each A-CPL parameter band and each
  reconstructed pair, the level difference and correlation within a tolerance of the source's
  fixed at the first measurement. The waveform-coded channels meet D2's and D3's checks.

**Verified by:** as D2, with the per-band script.

#### D6: output processing

**Status:** merged as #1037 on 2026-09-25. Exit met.

- DRC: default profiles and compression curves as DEE writes them, with a level detector chosen
  and documented, since Part 1 leaves it open; transmitted gains on constructed streams; the output
  level gain; mode selection.
- Dialogue enhancement: the channel-independent method on DEE streams, the other three on
  constructed streams.
- Loudness correction and the downmix: the matrices of Part 1 6.2.17, custom downmix data, and
  downmix loudness correction.
- The sample rate converter for every frame rate, with Part 2 5.11's phase lock, and in the shared
  core beside it the encoder's converter in the other direction, which E5 uses.
- Start-up at I-frames, DEE's priming frame, splices, and the concealment policies.
- As built, the pull request recorded four choices. The library's default output level is none:
  Part 1 leaves `Lout` to the system, so `OutputConfig::output_level_dbfs` unset leaves the stream
  at its coded level and compresses nothing, where the API sketch had −31 dBFS. A change of source
  forgets what was read from the stream and keeps the signal, so a splice or a switch at an
  I-frame is seamless (Part 1 6.2.19). A frame whose table of contents does not read counts as
  the frame the stream expected. A decode begun at an I-frame is right from the next frame's
  audio, except that A-SPX's noise and tone indices run from the decoder's first frame and A-CPL's
  decorrelators settle within four frames (`src/ac4dec/ERRATA.md`, "What an I-frame does not
  restore").

**Exit:**

- The output level gain equals `2^((Lout − dialnorm)/6)` to 0.01 dB for dialnorms from −31 to −17,
  from DEE's loudness options.
- Each DRC decoder mode's static curve, measured with stepped tones at steady state, is within
  0.5 dB of the curve its profile defines, and the mode selected follows Table 161 as the register
  reads it.
- Dialogue enhancement at 0 dB leaves the output identical to the output with the tool bypassed,
  and at its cap applies the gains the parsed parameters give to 0.01 dB, measured on known input.
- Every downmix matrix, measured with one tone per channel, equals its formula with the stream's
  gains to 0.01 dB.
- The converter's output sample count is exact over 100,000 frames at every frame rate, and its
  passband ripple and stopband attenuation are stated and tested. IMS streams at 23.976, 24, 25
  and 29.97 fps and the third-party 25, 29.97 and 30 fps streams decode.
- Decoding from any I-frame gives correct output from the following frame, and a splice resets
  state.

**Verified by:** `ac3tests`; the scoring and gain scripts over the committed and full sets.

#### D7: presentations

**Status:** merged as #1055 on 2026-09-26. Exit met.

- Selection: level, enabled flag, language, content classifier, associated types and
  `b_pre_virtualized`, for version 0 and version 1 presentations.
- Presentations of several substreams: M&E with dialogue, main with associated, and main with a
  dialogue enhancement substream for the hybrid methods.
- Mixing (Part 1 6.2.16; Part 2 4.8.4 with substream group gains and the mixer's division as the
  register reads it), and the dialogue and associated gains.
- The test multiplexer that builds such presentations from DEE substreams, writing their tables of
  contents with E1's frame writer, which E6 extends to the encoder's own presentations.
- D7 found the mixer's sum and the pan law settled by Part 1: Part 2 4.8.4's equation divides by the
  number of substreams where its prose and Part 1 6.2.16 add, and Table 216's 0.5 to each of L and R
  at 0 degrees fixes a law linear between neighbouring channels. The pan clause puts L and R at 330
  and 30 degrees, clockwise, and Table D.1 at 45 degrees, counting the other way; the pan takes the
  clause's angles for L, C and R and D.1's, turned clockwise, for the rest. Configuration 1 sends no substream group gains, since
  n_substream_groups is 1, and configuration 4 two for three groups, the dialogue enhancement group
  taking the main one's. A version 0 presentation takes Table 16's dialnorm and levels its associated
  audio from its own, in dB2 as the output level gain is, so that the audio description plays as it
  does alone at any output level.
- DEE writes no presentation of several substreams, so the multiplexer takes the substreams of DEE's
  tone legs and of six encoder streams by the offsets the decoder's syntax trace gives, rewrites
  their mixing fields and dialogue enhancement, and writes version 0 presentations as well, at sus_ver
  0. librempeg decodes a presentation's first substream alone, refuses a table of contents of more
  than 16 presentations, and refuses `bitstream_version` 1. MediaInfo reads the streams' tables of
  contents, and a second parameter set after `de_ms_proc_flag`, which E5's Mid streams and the
  multiplexer's show as "NOK: tools_metadata"; the text sends one (`src/ac4dec/ERRATA.md`,
  "Presentations" and "de_ms_proc_flag leaves one parameter set").

**Exit:** a table of constructed tables of contents selects as Part 2 4.8.2 requires; every mix,
measured with one tone per substream, equals its formula to 0.01 dB; associated audio pans at the
three angles Part 1 Table 216 defines; the multiplexed streams parse in both implementations.

**Verified by:** `ac3tests`; the gain scripts.

#### D8: the API, the CLI, media information and packaging

**Status:** merged as #1059 on 2026-09-26. Exit met.

- `DecoderConfig`, `OutputConfig` and `Decoder` in their final form, with the controls of
  [One control for both formats](#one-control-for-both-formats).
- `ac3cli decode` options for the presentation, DRC mode, output level, downmix, dialogue
  enhancement and the associated mix; `ac3cli probe json=1` with v1 presentations and the metadata.
- The inspector, the decoder and the core installed and exported, with ABI allowlists
  (`libac4dec.so.txt` among them) and the package check; the documentation pages; CHANGELOG and a
  ROADMAP entry.
- The contract items open since the review of #700 ([The decoder after D1](#the-decoder-after-d1)):
  the syntax sink's lifetime; one error for a Huffman miss at the end of a substream, whichever tool
  reads it; a report for an HSF extension substream nothing claims; and the Android, WASM and Python
  wheel configurations no longer compiling AC-4 libraries they do not link, until I4 binds them.
- D8 settled the syntax trace's lifetime by ownership. `DecoderConfig::syntax` and
  `EncoderConfig::trace` are `ac4::SyntaxTrace`, a `std::function` the configuration owns and the
  decoder or encoder copies, and `ac4::SyntaxSink`, the reference the readers and writers hold while
  they run, binds a named callable only. The change showed a second way to dangle: `sink_of()`, taking
  a `const SyntaxTrace&`, accepted the old reference type by converting it into a temporary
  `std::function` around a null reference, and the encoder's tests crashed on it until `sink_of()`
  took a `SyntaxTrace` alone.
- The API differs from the sketch under [The API](#the-api) where the sketch left room. The
  presentation changes through `set_presentation()` beside `set_output()`; `decode_by_block()`
  returns a `FrameInfo`, a decoded frame's description without its samples, or nothing for a frame
  without output, and `flush()` hands over the samples short of a block at the end; `parse()` reads
  a frame without decoding it, in place of `skip_reconstruction`; and the syntax trace and
  `refusal_reason()` stand in for a diagnostic sink. The output layouts of Part 2 5.10.2 and core
  decoding are D9's, as fields after the existing ones.
- A decoder built afresh when the listener changes a setting waits for the stream's next I-frame,
  so Hearth's rebuild-and-prime, which serves AC-3 and E-AC-3, cannot serve AC-4. `set_output()`
  takes the new values from the next frame instead: nothing is lost, and from two frames on, once
  the control data has reached the QMF domain, the output is the new configuration's sample for
  sample. Every presentation's substreams are read in every frame whichever is decoded, so
  `set_presentation()` needs no I-frame either, and `presentations()`, `metadata()` and
  `ac3cli probe` report a stream without decoding its audio.
- Part 2 6.3.3.1.4 sends an alternative presentation's name whole or in chunks, one a frame, and
  leaves open which bytes of a chunk are the name's and where a decoder joining in the middle begins.
  The register reads a chunk before the last as name bytes throughout and the last as two bytes
  shorter, and takes the name once as many consecutive chunks as the count says have arrived
  (`src/ac4dec/ERRATA.md`, "A presentation name in chunks"); the Python reference takes the same
  reading on 14 cases. No stream here names a presentation.
- DEE's 5.1 streams send no `lfe_mixgain`, so the LFE's place in a two-channel downmix is tested on
  an encoder stream written with `lfemix=-4.5`.
- The static decoder calls into the core without containing it, as no archive contains what it
  links, so the package installs `libac4core_static.a` beside `libac4dec_static.a` and names it in
  the decoder's exported target and in `ac4dec.pc`'s `Requires.private`. The decoder's OBJECT library
  used to link the inspector and the core itself, which put the core's archive on the link line of
  everything that linked `libac4dec.so`, and in a static build the inspector's archive as well; each
  library now links the inspector of its own kind. The member functions of `ac4::Decoder::Impl`
  were exported from `libac4dec.so` with the class they are nested in; they are hidden now, and
  `tools/ci/abi-allowlist/libiclforge_ac4dec.so.txt` lists the header's API alone.
- Over DEE's local set the test standing in for Hearth's engine decoded 406 of 533 streams when D8
  merged, and refused the 127 5.1.4 streams by the immersive channel element's name; over the 13
  third-party streams it decoded 12, and refused Chromium's A-JOC stream naming the substream it
  did not decode. D9 and D10 took those on.

**Exit:** the Hearth engine, or a test standing in for it, decodes every stream in the set through
the public API alone; the CLI tests cover every option; the packages contain the libraries; each
contract item has a test that failed before it was fixed; `mkdocs build --strict` and
`tools/checks/check_doc_paths.py` pass.

**Verified by:** `ac3tests` and the CLI tests on every leg; the package check; the documentation
gates.

#### D9: channel-based immersive

**Status:** merged as #1057 on 2026-09-26. Exit met: the delivery kit's 5.1.4 streams were not on
the machine when the pull request merged, and #1069 (2026-09-26) recorded that they decode in full
and core decoding.

- The immersive element and `immers_cfg`, in both transcriptions; Part 2 5.2's track assignment;
  S-CPL; A-SPX's immersive pairing and gains; A-CPL for immersive; A-JCC in full decoding, on
  constructed streams.
- Part 2's channel renderer (5.10.2) with custom downmix data and loudness correction, and Part 1's
  cascade for stereo and mono output; DRC's immersive channel groups (Part 2 Table 69).
- Core decoding as well as full ([decision 3](#decisions)): S-CPL's seven core outputs, A-SPX's
  core pairing and its post-processing (Part 2 5.4), A-CPL for immersive replaced by its core gain,
  A-JCC's core modules, the core renderer (Part 2 Tables 44 to 46) and the core loudness
  correction. Part 2 4.8.3.1 says gain factors replace A-CPL in core decoding for every element but
  specifies them only for the immersive element; the register records the reading taken for the
  others.
- 7.1.4 with every channel present, which DEE cannot write: the same element with the back channels
  present, tested on constructed streams and then on E8's.
- 22.2 and 9.X.4 are refused.
- D9 found the element's text at odds with itself in three places, each read by the rest of the
  text: Table 19's track numbers are names, not the order the syntax reads the elements in; the
  EXAMPLE after it assigns the first pair of grouping 1 against the table; step 4's NOTE 2 names F'
  where it means H'. Table 20's prediction gains, which the text does not say how to extract, are
  read as Pseudocode 59's `sap_gain`. The core's top pair is Tsl and Tsr, which Tables 45 and 46 index,
  and Table 59 prints a .2 source's pair as carried "in Tbr, Tbl", read left to left. Tables 38 to
  43 have rows no channel mode names, so the renderer's input configuration is the channel mode
  narrowed by the presence flags; custom downmix data hold from the frame that sends them until
  another does, since DEE sends them in I-frames alone; a render takes its output's loudness
  correction only where it downmixes, and two channels the Lo/Ro or Lt/Rt correction alone.
- DEE writes the element in three codec modes, by rate alone: ASPX_ACPL_2 from 192 to 448 kbps,
  ASPX_SCPL at 512 and SCPL at 768, always with the back channels silent
  (`b_4_back_channels_present` 0), so its 5.1.4 comes out as 5.1.4. It writes neither ASPX_ACPL_1
  nor ASPX_AJCC, and no 7.1.4 with backs, so those, every `core_5ch_grouping` and `2ch_mode`, and
  step 4's and Table 20's parameters are read from streams the encoder's writer builds, five of
  them committed with their digests. In ASPX_ACPL_2 DEE codes each top pair's sum and A-CPL makes
  the pair, so a top tone comes out across its pair, the sum at its level. librempeg does not decode
  the element: its L, R and C come out 6 to 9 dB down, its surrounds 12 to 15 dB down, all four top
  tones in its Lb and its top channels silent. The Dolby AC-4 Online Delivery Kit 1.5's two 5.1.4
  streams (ASPX_ACPL_2 at 192 kbps, 25 and 29.97 fps, local only) decode in full and core decoding
  with `ac3cli`: every frame (800 and 960) with no error, ten channels as coded and eight in core
  decoding, 1,920 samples a frame and 1,601 or 1,602.

**Exit:**

- Every DEE 5.1.4 stream from 192 to 768 kbps (ASPX_ACPL_2, ASPX_SCPL, SCPL) decodes in full
  decoding with each of the ten channels' tones on its own channel and the per-channel checks of D2
  to D5, and in core decoding with each channel's tone on the core layout's speaker the text assigns
  it and the level the core gains give, to 0.2 dB.
- Renders to 5.1 and 2.0 match their matrices to 0.01 dB in both modes.
- The Dolby delivery kit's 5.1.4 stream decodes in both modes with the invariants holding.
- Constructed A-JCC streams parse in both implementations, and A-JCC's full and core
  reconstructions equal their formulas on known QMF-domain input.

**Verified by:** as D2 to D6.

#### D10: A-JOC objects

**Status:** merged as #1060 on 2026-09-26. Exit met, with two exceptions. The DEE criterion did not
apply (G0 and G1 found no master DEE's A-JOC encoder accepts), and the listening, that the objects
move as their metadata says, is the user's to do; the numbers hold in `test_ac4dec_object_render.cpp`.

- `audio_data_ajoc`, `var_channel_element`, A-JOC in full decoding, object audio metadata (common,
  timing and dynamic data), dialogue enhancement for A-JOC (Part 2 5.8.2.3), and the ISF renderer.
- Core decoding: the downmix signals or static bed as the objects, the first object metadata portion
  (Part 2 4.8.3.4.2), and dialogue enhancement with A-JOC's interpolated dry matrix (5.8.2.4).
- Direct-coded object substreams (`ac4_substream_info_obj`), which use the Part 1 elements.
- Objects and their Annex F properties on the API; the OAMD substream's content, with the second
  `oamd_common_data()` it can carry.
- Built in D10: both transcriptions read the object syntax, and the encoder's writers gained it for
  the constructed streams; A-JOC's reconstruction lives in `src/ac4core`, templated on `Real`, and
  the decoder hands each object over with its Annex F properties and the sample of each update. The
  intermediate spatial format is the one kind of object the decoder renders itself, by the
  attachment's matrices; `ac3cli decode` renders the others through the layout renderer Hearth plays
  E-AC-3's objects with (`apps/common/ac4_object_render.hpp`).
- Chromium's `ac4-ajoc.ac4` is the only encoded A-JOC stream here: ten downmix signals in a SIMPLE
  `var_channel_element()`, seventeen objects, no LFE and no decorrelators, one metadata block a
  frame, and every object at X 0.5, Y 0 and Z −1, the front of the room on the floor, in all 64
  frames. It decodes in both modes with the invariants holding and its objects as its table of
  contents lists them; rendered, it all comes from the centre speaker. The rest is read from the
  encoder's writer: eight committed constructed streams, whose objects each carry a known sum of
  tones, and the differential check's mutations of them.
- The text needed readings (`src/ac4dec/ERRATA.md`): A-JOC's ramp advances once a slot and stops
  on its target (Pseudocode 17 as printed would pass it by a step); the decorrelation input matrix
  is taken subband by subband for objects of different band counts; Pseudocode 22's `de_gain > 1`
  test belongs to full decoding, since core decoding's `de_gain` is 10^(G/20) − 1; H'_M is kept by
  object; and Annex A.2.1's tables name the 5.x, 7.x and 9.x matrices `_to_50`, `_to_70` and
  `_to_90`, which the attachment calls `_to_5`, `_to_7` and `_to_9`, and give no column order,
  which the values settle as Table A.27's.
- DEE writes no A-JOC from this project's masters (G1), so the DEE criterion below does not apply;
  librempeg refuses object coding, so there is no second decode to compare with. Whether the
  objects move as their metadata says is the user's to hear.

**Exit:**

- Chromium's `ac4-ajoc.ac4` (fetched under [decision 4](#decisions)) decodes in full and core
  decoding with the invariants holding, and its object count and bed assignment match its table of
  contents.
- If G0 found a master DEE's A-JOC encoder accepts, its streams decode in both modes and are scored
  against the master's objects and bed, object by object.
- Constructed A-JOC and direct-coded object streams parse in both implementations, and A-JOC's
  matrices equal their formulas on known input.
- Rendered through Hearth's renderer, the objects move as their metadata says, by listening, in both
  modes.

**Verified by:** `ac3tests`; listening and the librempeg comparison, recorded in the pull request.

#### D11: AC-4 over IEC 61937

**Status:** merged as #1052 on 2026-09-26. Exit met; no device here accepts AC-4, so passthrough to
one is not checked.

From IEC 61937-14:2017, with 61937-1:2021 and 61937-2:2026 for the burst format they extend
([decision 9](#decisions)); the user supplied all three on 2026-09-15, read in place and not copied
into the tree.

- The four AC-4 burst types (`Pc` data type 24, subdata types 0 to 3) in `ac3::iec61937`, with
  Part 14's repetition periods, burst sequences at the 1000/1001 frame rates, maximum burst lengths
  and the AC-4 fields of `Pc` bits 8 to 11; `BurstReader` recognising them. `Ac4BurstPacker` packs
  one sync frame to a burst in any of the four types, with Part 14's tables transcribed as data and
  checked against renders of its pages; `ac4_burst_type_for()` picks the smallest type a stream's
  largest frame fits, since the type sets the link's rate. The reader checks an AC-4 burst's `Pd`
  against the sync frame's own size, and now reads all seven data-type bits of `Pc`, so data type
  1 with a subdata type is no longer taken for AC-3.
- An AC-4 format in `PassthroughSink`, on the backends whose operating system has a way to send it.
  D11 found two that do: ALSA and Android take IEC 61937 bursts as opaque two-channel data with the
  non-audio flag set, and send AC-4 on a link at the content rate and AC-4 HBR4 at four times it.
  WASAPI names a codec's subformat and the Windows SDK has none for AC-4, PipeWire's IEC 958 codecs
  have no AC-4, and Core Audio has no AC-4 format ID, so those three refuse it with
  `kUnsupportedFormat`; so does AC-4 HBR16 everywhere, whose eight-channel link no backend opens.
- The AC-4 data type on the extension page Hearth's phase A4 writes, and in the test sink: `"ac4"`,
  a burst chunk of one sync frame, which the test sink decodes with `ac4::Decoder` and renders.
- Part 14 leaves two choices, recorded in `iec61937.cpp` and the pull request. It numbers the five
  bursts of a sequence without saying which frame is data-burst 0; the packer takes the frame's
  phase in the five-frame cycle TS 103 190-2 clause 5.11 locks the decoder's converter to, set by
  `sequence_counter`. Part 14's sequences start at the same frame of that cycle, so each burst
  starts within a sample of its frame's first decoded sample, and a stream packed from any frame
  gives each frame the same period. And it gives `Pd` in bits for AC-4 and AC-4 LD where IEC
  61937-2 Table 2 says bytes; the packer writes bits, as the part that defines the data-burst says,
  and the reader takes either. Its AC-4 LD rate of 187.5 fps has no `frame_rate_index` in
  TS 103 190-1 V1.4.1, so no frame reaches that row.

**Exit:** for every frame rate, a stream packed and read back returns every frame unchanged, with
each burst's repetition period, sequence and `Pc` fields as Part 14's tables give them; the test
sink decodes an AC-4 stream sent through the extension role.

**Verified by:** `ac3tests`, with the tables' cases transcribed from the standard; the loopback test
of Hearth's phase A4. No device here accepts AC-4, so passthrough to one is not checked.

#### D12 and D13: taken into D14

Planned on 2026-09-15 as the `float` tier on the ESP32-S3 (D12) and the fixed-point tier on the
ESP32-C6 (D13). The P4 joined the family's sinks on 2026-09-21, after them, and on 2026-09-25 the
user made it AC-4's first ESP32 target, with the S3 and the C6 to follow as the decoder is
optimised ([decision 24](#decisions-of-2026-09-25)). D14 carries both phases' work in that order.

#### D14: AC-4 on the ESP32s

**Status on 2026-09-30:** D14a merged as #1096, #1102 and #1123 and D14b as #1118, all on
2026-09-29; D14c and D14d are not built.

After D10. One implementation with three scalars, as `ac3::forge`'s decoder has
(`planning/arithmetic-tiers.md`, [decision 25](#decisions-of-2026-09-25)): `double` on computers,
`float` on parts with a floating-point unit, fixed point on parts without, chosen by
`AC3FORGE_DECODE_SCALAR` and, in the ESP-IDF component, by `CONFIG_SOC_CPU_HAS_FPU`. The P4 comes
first; the S3 and the C6 follow in the phase's later parts. What AC-3 and E-AC-3 manage on each part
([The ESP32](#the-esp32)) sets what AC-4 aims for there. One pull request per part.

- **D14a, the scalar and the decoder's size, on the host** (merged as #1096, #1102 and #1123).
  - `src/ac4dec/src/pcm` on the decoder's scalar, as `src/ac4core` is, with a complex type of the
    project's own in place of `std::complex<Real>`; AC-4 in `AC3FORGE_DECODE_SCALAR`, the `float`
    build compiling with `-Wdouble-promotion` as an error; `ac3::forge`'s `Fixed32`, vector types
    and `float` functions shared through a header-only target
    ([decision 31](#decisions-of-2026-09-25)).
  - The QMF bank, three quarters of a frame's arithmetic, with real and imaginary parts in separate
    planes, delay lines that move an index rather than their contents, and a 64-point transform;
    held, as D3's pair is, to the direct formula and to its reconstruction.
  - What a decoder holds, about 1 MB at 2.0 and 2 MB at 5.1 in `double` as D6 left it: one
    transform scratch per substream, tables in flash, the A-CPL and decorrelator state only in the
    modes that use it, no guarded function-local static and no stack object over 4 KiB.
  - A cached bit reader and a table-driven Huffman decoder, with every syntax digest unchanged.
  - Vector kernels on the host (`f32x4`, `f64x2`) on the split planes, each identical bit for bit
    to the loop it replaces.

  **Built in D14a.** Decision 31's header-only target (`src/arithmetic`) moved `Fixed32`
  and the float scalar functions out of `ac3::forge`'s own tree with no copy; both `ac3::forge` and
  `src/ac4core` link it. `src/ac4core`'s own QMF-domain kernels - the analysis/synthesis pair, the
  FFT and MDCT, A-SPX's high-frequency generator, A-CPL's decorrelators and ducker, A-JOC's
  reconstruction, A-JCC's accumulator - take a complex type of the project's own (`dsp::Complex<Real>`)
  in place of `std::complex<Real>`, joined `AC3FORGE_DECODE_SCALAR` through the directory-selection
  mechanism `ac3::forge`'s own `decode_scalar_t` uses, and the float build compiles `src/ac4core`
  alone with `-Wdouble-promotion` as an error: clean, confirmed by building that one target
  (0 warnings). `QmfValue` (`pcm/aspx.hpp`) and `QmfSample` (`aspx_encoder.hpp`) take the same
  complex type, since both cross straight into these kernels at `double`; the double build's output
  is unchanged bit for bit as far as the full test suite can tell (2 358 cases, 11 168 384
  assertions, no hash re-pin needed). Three of the memory findings are fixed on their own, apart
  from the scalar work: `bitrate_kbps()`'s guarded hash map, the |q|^(4/3) table's guarded lazy
  init, and `stereo_parameters()`'s 32 KiB return.

  `src/ac4dec/src/pcm` is templated on `Real` too, now: the same double-to-`Real` and
  `std::complex`-to-`dsp::Complex` change this phase's first part made in `src/ac4core`, across all
  of its modules - A-SPX, A-CPL, A-JCC, A-JOC, companding, dialogue enhancement, DRC, the downmix,
  S-CPL, stereo and multichannel processing, and the substream orchestrator itself - checked the
  same way. `ac3cli` and the whole test suite build, link and pass at `float` on the host: 2 389
  cases, 11 171 235 assertions, identical on both scalars, the `double` build's output not moved by
  a bit despite decision 25's own cost estimate above. A handful of values set once a frame or a
  configuration rather than once a QMF sample - a downmix or DRC gain matrix, A-CPL's and A-JCC's
  own coefficients from `acpl::interpolate()`, deliberately left untouched - keep `double`, narrowed
  once where they multiply a `Real` or `QmfValue`, in the shape the QMF banks' own twiddle factors
  already used. `hf_generator.cpp`'s dB gains, which called `std::log10`/`std::pow` directly at
  every scalar, now go through a new `scalar_exp2` (`src/arithmetic`, beside the existing
  `scalar_log2`/`scalar_exp`) at `float` only; the `double` path still calls them directly,
  unchanged. `src/ac4enc` calls several of `src/ac4core`'s kernels at a literal `double`, since the
  encoder has no `float` tier of its own ([decision 34](#decisions-of-2026-09-25)) and `ac4core` is
  one shared library rather than `ac3::forge`'s separately-compiled encoder and decoder DSP; those
  kernels, and a few others `ac4core`'s own tests exercise directly at `double`, now also explicitly
  instantiate `<double>` when `Real` is not already `double` (`AC4CORE_ALSO_AT_DOUBLE`, defined by
  the per-scalar `real.hpp`), adding nothing to a `double`-configured build - the encoder's own tests, part of the unmoved whole suite above,
  hold on that path unchanged.

  **The rest of D14a** (the third pull request) does what the first two left: the QMF banks, the
  memory of the decoder, the bit reader and the Huffman decoder, the host's vector kernels, and the
  probe's AC-4 rows.

  The QMF banks are each one 64-point complex transform between a rotation that packs pairs of
  samples into complex values and a butterfly that pairs subband k with 63 - k, which is
  Pseudocode 65 and 66 reduced algebraically (the derivation is in `src/ac4core/include/iclforge/ac4core/dsp/qmf.hpp`),
  on separate real and imaginary planes, with ten-block delay lines that move an index. Every
  twiddle factor is a `constexpr` array built by integer angle arithmetic from one generated
  quarter-wave cosine table, the `Real` nearest its exact value, and the banks share them. They are
  held to Pseudocodes 65 and 66 as printed, to the 78 dB reconstruction and to a direct 128-point
  sum, at both scalars. A slot takes 0.77 us in analysis and 0.83 us in synthesis at `double`, from
  3.15 and 3.3. The vector kernels (`qmf_vector.hpp`) put the seven steps of a slot on `f32x4` and
  `f64x2`, each tested equal bit for bit to the scalar loop it replaces, at both scalars, and the
  banks to those loops composed. The SIMD seam moved from `src/ac3` to `src/arithmetic` for them
  (`ac3::arithmetic` now carries the architecture directory): on the x86-64 seam a slot is 2.2 to
  3.9 times faster at `float` and 0.9 to 1.6 times at `double`. On the Cortex-M3 leg the seam is the
  generic directory, and the same kernels run 0.2 to 0.3% fewer instructions and add 4.2 KB to the image
  against the scalar loops, so they are used on every part; a part whose measurement says otherwise
  can take the scalar kernels, which stay as the reference.

  What the decoder holds: `SubstreamPcm` is 10.9 KB at `double` (9.3 KB at `float`), from 299 KB
  (177 KB), because A-CPL, A-JCC and A-JOC make their decorrelators when the first frame that
  applies them arrives (A-JOC's reconstruction alone was 150 KB) and one transform scratch per
  substream serves every channel's transforms. The |q|^(4/3) table is `constexpr` and in flash,
  exact to the `double` nearest each power in place of 8 192 calls filling 64 KB of RAM before
  `main` (`std::pow(m, 4.0 / 3.0)` is up to 6.7e-16 relative off it, since 4.0 / 3.0 is short of
  4/3 by 7.4e-17). No guarded function-local static remains in `src/ac4core`, `src/ac4dec` or
  `src/ac4`, and every object the decode path built on the stack to reset or return is built in
  place, a member, or handed a scratch: `SubstreamPcm::decode`'s frame fell from 15.1 KB to
  3.4 KB and `decode_aspx`'s from 11.7 KB to 3.4 KB. Seven frames are still over 4 KiB in the
  `float` build, each a sum of smaller locals: the A-CPL coupling parameters' (12.4 KB),
  `Decoder::Impl::read`'s (11.6 KB), `decode_into`'s (7.1 KB), `conceal_or`'s (4.6 KB),
  `acpl_values`' (4.4 KB), `stereo_parameters`' (4.2 KB) and `parse_sf_data`'s (4.2 KB); a decode's
  stack read by painting is 18.3 to 19.5 KB on the Cortex-M3 and 24.7 to 26.0 KB on x86-64. Two
  costs are left open: the syntax layer builds its element vectors afresh each frame, so a frame
  allocates 50 to 191 times in the steady state and 69 to 673 KB in bytes, and the transforms'
  and windows' tables are built by the first frame from `libm` (their values are the same on the
  host and on the Cortex-M3 to the bit, as the hashes below show, but nothing but that
  measurement says they must be).

  The bit reader reads through a 64-bit cache, so a peek of up to 32 bits is a shift, and every
  Huffman codebook carries a 256-entry table of its codewords of 8 bits or fewer, built at compile
  time (2 bytes an entry, 43 KB of flash over the 84 codebooks); a longer codeword takes the search
  by length as before. Every syntax digest is unchanged, and the two transcriptions agree over
  600 streams.

  The probe (`apps/baremetal/ac4_probe.cpp`) is a third probe beside the AC-3 and E-AC-3 decoder's
  and the encoders': the AC-4 libraries build in the decode profile (static, without exceptions,
  the encoder not built), and `run_baremetal_probe.sh --ac4` decodes five committed streams (2.0
  and 5.1, with and without A-CPL, and DEE's 5.1.4 tones) through `Decoder::decode_by_block`,
  checking each channel's level against `apps/baremetal/ac4_fixture.hpp`. On the Cortex-M3 leg,
  in `float`, with GCC 14.2.1 at `-Os`:

  | Fixture | Frames | Instructions a frame | Peak heap | Allocations a frame, steady | Stack |
  |---|---:|---:|---:|---:|---:|
  | `ac4_20_music`, 2.0 | 3 | 54,530,000 | 431,805 | 52 | 18,288 |
  | `ac4_20_acpl`, 2.0 A-CPL | 4 | 57,883,000 | 626,008 | 50 | 19,456 |
  | `ac4_51_music`, 5.1 | 3 | 117,580,000 | 996,954 | 153 | 19,456 |
  | `ac4_51_acpl`, 5.1 A-CPL | 4 | 124,489,000 | 1,205,460 | 90 | 19,456 |
  | `ac4_514_tones`, 5.1.4 | 2 | 205,781,000 | 1,931,680 | 191 | 19,456 |

  The image is 486,192 bytes and nothing is retained after the decoders are destroyed. The PCM of
  each fixture is bit-identical between the x86-64 host (GCC 16, SSE seam) and the Cortex-M3
  (soft float, generic seam), so decision 26's claim holds for these five, and the hashes are pinned
  (`tests/golden/ac4-probe-pcm-hashes.json`). The peak is what D14c meets: 2.0 needs 432 KB where the
  S3's probe allows 245,000, so 2.0 in internal RAM on the S3 needs the decoder's allocations halved
  again, or PSRAM.

  **Exit, as measured.** (a) The `float` decode against the `double` one on every committed
  stream (67 of them, `tools/checks/check_ac4_decode_scalar_snr.py`), the worst channel's SNR in
  half-overlapped Hann frames of 2 048 samples: below the lowest crossover 109.4 to 136.0 dB (132.1
  to 136.0 on DEE's), and above the highest 37.5 to 102.1 dB where a stream has A-SPX (47.2 to
  102.1 on DEE's; the constructed streams, whose payloads are random, and the A-SPX object streams
  are the low end, 37.5 to 46.8). The high
  band is where `float` and `double` part, and the cause is open. Accumulating Pseudocode 86's
  covariances and solving Pseudocode 87 in `double` in a `float` build moved no figure by 0.1 dB on
  eight of the streams, so the prediction is not it; the worst frames of a channel are far below
  its aggregate (39 dB in a frame of a channel whose figure is 75), which points at a few decisions
  or gains that flip or move and not at a general loss of precision. In absolute terms, against
  the energy of a full-scale sine, the loudest channel's difference is no louder than -149.1 dBFS
  on DEE's streams, -143.9 on the object streams and -96.8 on the loudest constructed one.
  MSVC, GCC 16 and Clang 22 agree to 0.1 dB;
  the floors are pinned in `tests/golden/ac4dec/scalar-agreement.json`, 3 dB under the figures.
  (b) `score_ac4_decode.py` and `score_ac4_encode.py` hold their pins with a `float` CLI and with a
  `double` one, on the committed legs and, with `--gold`, on the local gold set. (c) The probe's rows above are pinned in
  `run_baremetal_probe.sh`, each about a tenth over its figure. (d) The `double` output moves:
  61 of the 66 streams under `tests/golden` decode with a few samples different in the float32
  output, by at most 2.3e-10 (about -193 dBFS); the encoder's output is byte-identical on the five
  encodes checked, so nothing of the encoder's is re-pinned, and both scorers hold at `double`.

  **Exit:** on every committed stream, the `float` build's agreement with the `double` build
  stated below and above the crossover and pinned; the scorers at their pins with a `float` CLI;
  the probe's AC-4 rows with peak heap, allocations per frame, stack and the Cortex-M3 leg's
  instruction counts pinned. The `double` output moves in its last bits, and the encoder's with it:
  both are scored again. Met; the figures are above.

  **Verified by:** `ac3tests`; the `float` gate and the scorers in CI; `run_baremetal_probe.sh`.
- **D14b, the P4** (merged as #1118). The component builds the inspector, the core and the decoder
  in `float`, behind a switch off by default. On the board, at 360 MHz with the network up: 2.0,
  5.1 and 5.1.4 in full and core decoding, in each mode DEE writes, and the converter's three
  ratios, with stage timers per part of the decode. The `float` output is identical on the host,
  the Cortex-M3 leg and the P4 ([decision 26](#decisions-of-2026-09-25)).

  **Built in D14b.** `CONFIG_AC3FORGE_AC4` builds the inspector, the core and the decoder into the
  component in `float`, behind a switch that is off by default and offered only on a part with a
  floating-point unit. `AC3FORGE_MINIMAL_AC4` in the root CMake gives `src/ac4`, `src/ac4core` and
  `src/ac4dec` the minimum-footprint profile's compile options as static archives, with no encoder
  and no shared library, and the packer's `--with-ac4` puts their sources in the component archive.
  With the switch off the component's objects disassemble identically to those of the tree before
  it (35 of 35). The player reads a stream that opens with an AC-4 sync word through the same ring,
  renderer and sinks, and `hearth_sink` plays it over Wi-Fi and ends a play with its time, heap,
  stack and PCM hash and, under `AC3FORGE_STAGE_TIMERS`, the time in each part of the decode:
  markers (`AC4_ZONE_SCOPED_N`, empty without the timers) in the QMF banks, the inverse transform,
  A-SPX, A-CPL, the converter, and the syntax and reconstruction around them. The compile errors
  that only the RISC-V compiler found (a `std::min` on a `long`, two integer-to-`float`
  conversions) were fixed the same way on main by D14a's third part.

  On the board, at 360 MHz with the network up, twenty plays of DEE's streams cover 2.0, 5.1 and
  5.1.4 in full decoding, the three 5.1.4 modes in core decoding, and the converter's ratios (24,
  23.976, 25 and 29.97 fps), with D14a's third part in the decoder and, as the baseline it is
  measured against, without it. The P4 decodes 2.0 in SIMPLE mode in real time, at 0.53 of a frame,
  and 2.0 in A-SPX mode at 0.74, and nothing wider: 5.1 takes 1.4 to 4.1, 5.1.4 in full decoding 2.8
  to 3.7 (2.3 to 3.1 in core decoding) and the converter's frame rates 5.6 to 6.6, where AC-3 and
  E-AC-3 through the same image take 0.20 for 5.1 and 0.38 for 7.1.4. Before the third part the
  figures were 0.86 and 1.04, 3.5 to 6.7 and 5.4 to 6.1: it made a frame 1.1 to 2.5 times faster,
  cut the decode task's stack from 49-50 KB to 20-24 and moved the decoder's large blocks to PSRAM.
  Four findings say where the time goes now. The QMF banks and the inverse transform are 22 to 29%
  of a frame, and A-CPL mode 3 two thirds of its play's. The converter runs in `double` on an FPU
  that is single precision: 185 to 231 ms a frame, five times real time by itself and 84 to 88% of
  the frame, and 5.5 s more than an ordinary first frame to design its table at 1001/960. ESP-IDF's
  default allocation policy fills internal RAM, and before the third part it cost the decoder up to
  1.9 times and after it 1.0 to 1.2, while a policy that suits AC-4 costs the AC-3 and E-AC-3
  decoders 1.1 to 1.7 times, so `sdkconfig.ac4` keeps the default; why it costs anything is not
  established. The `float` output equals the pinned hashes of the probe's five fixtures, and the
  host's on 15 of the 20 plays and all six core plays, and differs on the five with companding,
  because `std::pow` and `std::exp2` at `float` in `pcm/companding.cpp` and `pcm/aspx.cpp` give a
  different last bit in each C library; routed through `scalar_exp2` and `scalar_log2` in a scratch
  copy, before the third part, the host, a Cortex-M3 program and the board gave the same hashes on
  four cuts. **Of the exit criteria: the per-stream time against real time, the heap and the stack,
  and the statement beside AC-3 and E-AC-3 hold, and decision 26's identical output holds on the
  probe's pinned fixtures and on every stream without companding; the change that would make the
  companding streams agree is not in this phase.** D14a4, below, made it. The options, a
  recommendation and their cost are in the pull request's report.
  [ESP32-P4](../docs/platforms/bare-metal/esp32-p4.md#ac-4) has the tables.

  **Exit:** per stream, the time per frame against real time, peak heap and stack left, and a
  statement of what the P4 decodes in real time, in which mode and to which layout, beside what
  AC-3 and E-AC-3 do there.

  **Verified by:** the gates in CI; the board (no QEMU runs the P4).
- **D14c, the S3.** Not built. The same build under the S3's limits: 2.0 in internal RAM, checked
  under QEMU in CI within the probe's limits; 5.1 and wider with PSRAM, on the board
  ([decision 29](#decisions-of-2026-09-25)). 5.1.4 in full decoding is measured, not aimed at
  ([decision 28](#decisions-of-2026-09-25)). A PIE kernel, integer and so fixed point inside the
  `float` decode, only where the board's timers show one kernel holding back a stream
  ([decision 30](#decisions-of-2026-09-25)).

  **Exit and verified by:** as D14b for the S3, with the QEMU rows in CI.
- **D14d, the C6.** Built on the host and under QEMU, the board not yet run:
  [D14d](#d14d-the-c6-fixed-point) below, and the decoder's memory after it in
  [D14f](#d14f-the-decoders-memory). `Fixed32` with a block exponent per QMF slot and per transform
  block; A-SPX's energies, gains and limiter and the decorrelators' energies as a mantissa and a
  power of two; the decorrelators and the converter's taps on 64-bit accumulators; AC-4 rows in the
  fixed-point hashes, identical on x86, the Cortex-M3 leg and the C3 under QEMU. On the board with
  WiFi, core decoding first; where 2.0 does not keep up, Hearth sends the C6 PCM
  ([decision 32](#decisions-of-2026-09-25)). Follows Hearth's phase C1 bring-up.

  **Exit:** the fixed tier's agreement with `double` stated; hashes equal on every architecture;
  which streams fit the C6 and keep up with WiFi, and how the rest reach a C6 sink.

  **Verified by:** the fixed gate and the C3 probe in CI; the board.

#### D14a4: libm parity and the converter at float

After D14a and D14b. D14b's board work found two things in D14a's float build. The board's PCM equalled
the host's on the probe's five pinned fixtures and on fifteen of the twenty plays and differed on the five
with companding, because `std::pow` and `std::exp2` at `float` give a different last bit in each C
library; and the frame-rate converter ran in `double` on a part whose FPU is single precision, 185 to
231 ms a frame and 84 to 88% of those plays' frames. The user took both on 2026-09-30, the second with the
table designed once in `double`. One pull request.

- **Libm parity.** Where the float decoder calls libm at `float` and the answer reaches the output, it calls
  the project's own functions: companding's gain L^((1 - alpha) / alpha) is 2^(e log2 L) through
  `ac3::internal::scalar_exp2` and `scalar_log2`, G = 2^(1 / alpha) and A-SPX's `exp2` go through
  `scalar_exp2`, and Pseudocode 87's limit |alpha| >= 4 compares the squared magnitude with 16 in place of
  `hypotf`. At `double` the calls are libm's as they were (`scalar_exp2` is `std::exp2` there), so the
  `double` output does not move. What remains of libm in the float decode, read from the symbols of the
  Cortex-M3 image, is `sqrtf` and `floorf`, which are exact, and calls at `double` whose results are rounded
  to `float` once.
- **The converter.** `BasicResamplerFilter<Coefficient>` keeps the table in the scalar the converter runs at:
  every phase is designed and normalised in `double` and rounded once, `ResamplerFilter` stays the `double`
  one the encoder and the tests take, and `Resampler<Real>` keeps its history in `Real`. At `float` an output
  is a sum of `float` products over four lanes of the seam's `f32x4` in an order fixed in
  `dsp/resampler_vector.hpp` (lane j takes taps j, j + 4, ...; the lanes are added (0 + 1) + (2 + 3); the
  taps left over follow), the same float on the x86-64 seam and the generic one. A sequential `float` sum
  on the P4 is a chain of dependent adds. At `double` the sum is the loop it was.
- **A probe fixture with companding.** None of the probe's five had it, so its Cortex-M3 and host legs could
  not see what the C libraries disagreed on: a sixth, DEE's 2.0 at 48 kbit/s (a committed fuzz seed), and a
  test that pins the bits of the converter's `float` table at the three ratios.

**Built in D14a4.** The changes above, in `pcm/companding.cpp`, `pcm/aspx.cpp`, `aspx/hf_generator.cpp`,
`dsp/resampler.{hpp,cpp}` and the new `dsp/real_functions.hpp` and `dsp/resampler_vector.hpp` of
`src/ac4core`; `src/ac4dec` reaches the arithmetic through the first, as the layering table has it, and
compiles with `ac3::arithmetic`'s include directory. The five probe fixtures' hashes did not move; the sixth, `ac4_20_companding`, is
`5b93c61c57566b0c` on the Cortex-M3 under QEMU, on the x86-64 host and on the board, at 58.6 M instructions
a frame, 466,163 bytes of peak heap and 75 allocations a frame. The `float` decode of every committed
stream still agrees with the `double` one to the floors of `scalar-agreement.json`, except that the three
IMS streams, whose converter now rounds to `float`, sit 1.4 to 3.3 dB lower above their crossover than
before (91.4, 60.9 and 94.9 dB), and their floors are pinned again.

**Exit, as measured.** (a) The `float` PCM equal on the host (MSVC, GCC 16 and Clang 22), the Cortex-M3
leg and the P4 for D14b's twenty plays and six core plays, on the M3 for the 24-frame cut of each, for
the probe's six fixtures and for D14b's four cuts: 84 plays and cuts compared across the five, under ESP-IDF's
default allocation policy and under the 512-byte one, none different; before, the five plays with
companding differed. (b) The `double` output byte-identical to main: 360 decodes (120 committed and
played streams, as coded, folded and in core decoding) and 6 encodes, at index 13 and at 24, 25, 23.976,
29.97 and 30 fps, with the whole of `ac3tests` passing at both scalars with no `double` pin moved. (c)
`score_ac4_decode.py` (15 legs) and `score_ac4_encode.py` (72) hold their pins with the `float` CLI. (d)
The converter on the P4, per frame of two channels, before and after: 204,951 to 7,321 us at 25/24,
208,009 to 7,169 at 15/16, 230,897 to 22,395 at 1001/960 and 23.976 fps and 184,609 to 98,547 at 29.97
fps, where the default policy's slow stage falls on it (17,454 under the 512-byte policy). The
frame is 0.92 and 0.82 of its duration at 24 and 25 fps, from 5.64 and 5.92, and 1.25 and 3.65 (1.21 and
1.26) at the 1001/960 rates. On the host, in one channel's frame, the `double` sum takes 119 to 143 us
and the `float` lanes 41 to 54 (2.6 to 3.1 times, MSVC). The first frame at 1001/960 still takes 5.9 s,
5.5 s more than an ordinary one, the table being designed in `double`.
[ESP32-P4](../docs/platforms/bare-metal/esp32-p4.md#ac-4) has the tables.

**Exit:** the `float` PCM equal on the host, the Cortex-M3 leg and the P4 for D14b's twenty plays, where
it differed on five; the `double` output byte-identical to main; the scorers at their pins with the `float`
CLI; the converter's time on the P4 before and after. Met.

**Verified by:** `ac3tests` at both scalars on MSVC; the WSL GCC 16 and Clang 22 gates at both scalars and the
assertion-checked Clang tree; `run_baremetal_probe.sh --ac4` on the Cortex-M3 and on the host; the board.

#### D14a5: the converter's float tables at compile time

After D14a4. D14a4 left the frame-rate converter's table at 1001/960, 94,094 coefficients each a Kaiser window and a
sinc, to be designed in `double` on the P4's soft-float routines at the first frame, which took 5.9 s, and read from
PSRAM, which cost the converter 22 to 98 ms a frame. The user took it on 2026-09-30: generate the `float` table at
compile time, with C++23 `constexpr` and `consteval`, "idiomatic". One pull request.

- **The design as a constexpr function.** `dsp/resampler_design.hpp` holds the design once (the filter's length from
  Kaiser's estimate, the window, the sinc, each phase's normalisation) over a `Math` that supplies `ceil`, `sqrt`,
  `sin` and the window's I0. The `double` filter, the encoder's and the default build's decoder's, instantiates it
  with the C library's functions, so its table keeps the bytes it had; the `float` tables are instantiated with
  `PortableMath`.
- **Portable math.** `dsp/portable_math.hpp` has `ceil` by truncation; `sqrt` by Heron's iteration from half the
  exponent and a step of Newton's method on the residual, which Dekker's product computes exactly (the double nearest
  the root on every argument tried, the arguments one unit below a power of four, where a root lies within 2^-56 of a
  midpoint, included); and `sin` and `cos` by a Cody-Waite reduction with pi / 2 in three pieces and Taylor series to
  r^19 and r^18 (within three units in the last place of the C library's over |x| <= 2^20). `bessel_i0` moved to
  `dsp/kbd.hpp` as a `constexpr` function with the same statements. Plain `double` operations in a fixed order, so the
  compiler's evaluation, an x86-64 host, a Cortex-M3 and an ESP32's soft-float `double` give the same bits (the build
  pins `-ffp-contract=off`).
- **The tables.** 25/24, 15/16 and 1001/960 are `constexpr` variable templates built by a `consteval` function,
  evaluated when `dsp/resampler.cpp` is compiled at the `float` scalar; a `double` build names none and evaluates nothing. A table keeps phases 0 to up / 2: phase
  up - p is phase p read from its last coefficient to its first, since the window and the sinc are even, so 1001/960 is
  188 KB where it was 376 KB; `dot_four_lanes_reversed()` is the four-lane sum of a phase read backwards, bit for bit what
  `dot_four_lanes()` gives on a copy written out. A float filter of any other ratio is designed when it is made, with
  the same functions.
- **A copy.** The constants are in flash, which on the board is DIO at 80 MHz behind a 128 KB cache, where the PSRAM
  is hex at 200 MHz. Read in place the half table took the converter 60.5 ms a frame at 23.976 fps (the whole 376 KB
  table 110 ms), against the 22.4 ms of the table designed at run time and read from PSRAM; the filter copies the
  table into its own memory when it is made, and the converter takes 16.5 ms.
- **Limits.** One translation unit evaluates the tables, only in a `float` build, and `src/ac4core/CMakeLists.txt`
  raises the constant evaluator's limit for it by compiler: `/constexpr:steps` for MSVC, `/clang:-fconstexpr-steps`
  for clang-cl (which accepts and ignores `/constexpr:steps`), `-fconstexpr-steps` for Clang and
  `-fconstexpr-ops-limit` for GCC, which ESP-IDF's component takes through the same file.

**Built in D14a5.** `dsp/portable_math.hpp` and `dsp/resampler_design.hpp` in `src/ac4core`, `bessel_i0` in
`dsp/kbd.hpp`, the filter and kernel in `dsp/resampler.{hpp,cpp}` and `dsp/resampler_vector.hpp`, the compile limits,
and `tests/ac4core/test_ac4core_portable_math.cpp` with the converter's tests (the table's FNV-1a image three ways, the
compiler's evaluation against the machine's, the mirrored phases, the reversed kernel). The three tables equal the C
library's design rounded to `float` in every coefficient, so no `float` PCM hash moved; the tests pin the FNV-1a image
of each table, which is new here. The Cortex-M3 probe's image is 683,448 bytes from 485,032, which its ceiling follows (750,000).

**Exit, as measured.** On the P4, in the default allocation policy, the four IMS streams of D14b, with D14a4's tree built and measured the same
way in the same session beside D14a5's image (the converter's microseconds a frame, D14a4's then D14a5's): 7,283 and
7,389 at 24 fps (25/24), 7,184 and 7,285 at 25 fps (15/16), 22,369 and 16,520 at 23.976 fps (1001/960) and 98,508 and
93,761 at 29.97 fps (1001/960). The frame is 1.11 of its duration at 23.976 fps, from 1.25, and 3.51 at 29.97 fps, from
3.65; at 24 and 25 fps it is 0.91 and 0.82, as before. The first frame takes 0.31 s at 24, 25 and 23.976 fps and 0.42 s at
29.97 fps, from 0.43, 0.40, 5.87 and 5.97 s, and the 23.976 fps play's peak of PSRAM falls from 632 to 435 KB. Under the
512-byte policy the converter takes 7.5, 7.3, 15.3 and 12.2 ms. The `float` PCM hashes of the 84 plays and cuts of D14a4's
lists are the same on the host (MSVC, GCC 16 and Clang 22), in the Cortex-M3 program and on the board, and equal to
D14a4's, so no pin moved; the `double` output is byte-identical (360 decodes and 6 encodes). The image grows by 199,696
bytes, 196,464 of them the tables, the Cortex-M3 probe's by 198,416 bytes, and `dsp/resampler.cpp` takes 5 to 14 s
longer to compile at `float`, by compiler (MSVC 14.7 s from 0.7, GCC 16 6.0 s from 0.8, ESP-IDF's RISC-V GCC 8.4 s from 1.5). The 29.97 fps play's
converter still takes 77 ms more than the 23.976 fps play's with the same table and code, and the allocation policy's
open question stood until D14a6.

**Exit:** the P4's first frame at 1001/960 from 5.9 s to the 0.31 s of the first frame at 24 and 25 fps, no run-time
design on any target, no change to the `double` output, and the `float` PCM equal across the host, the Cortex-M3 leg
and the board. Met.

**Verified by:** the whole `iclforge-tests` at both scalars on MSVC, clang-cl, GCC 16 and Clang 22 (at `float` one
Hearth test, `stream decoder: fast inverse transform reaches the decoder, closely matching the reference transform`,
fails on all four, and on main's tree with GCC 16); the `double` comparison with main's build; the probe on the Cortex-M3
and the host; the board; the full-matrix `ci.yml` and `pr-gate.yml` dispatches on the branch.

#### D14a6: the P4's low-power SRAM out of the heap

After D14a5. D14b and D14a5 left one thing open on the P4: under ESP-IDF's default allocation policy the 29.97 fps play's
frame-rate converter took 93.8 ms a frame and under the 512-byte policy 12.2, from the same code and the same table, and a
few stages of other plays (QMF synthesis, the inverse transform) were slow in the same way. The user took the allocation
policy on 2026-10-01, to run once N1 and D14a5 had merged: find the cause, and give the default policy the fast time
through the allocator's placement and not by moving the image to the 512-byte policy. One pull request, one line of
configuration.

- **The cause.** ESP-IDF puts the ESP32-P4's 32 KB of low-power SRAM (0x50108000) in the heap as internal memory of the
  lowest priority (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`, on by default, whose help says the memory "does not have
  much performance impact"). The HP cores reach it over the LP bus with no cache: a loop in a scratch image took 174 cycles
  for a load the next depends on, 195 for a load in sequence and 173 for a store, against 6 to 7 from main RAM and 18 a word
  for a cold sequential read of PSRAM. The decoder takes nearly all of main RAM (343 to 350 KB free when a play starts, 1 to
  11 KB at its least), so what it allocates late is served from the SRAM before PSRAM is tried, and whether a hot buffer is
  among it depends on how full main RAM is at that moment, which is why a play was slow after some plays and not after
  others and why two images of the same code differed. In the 29.97 fps play of a scratch image that prints the converter's
  addresses, one channel's history was at 0x50108364 and the output vector the channels share at 0x5010ad6c, and the
  converter took 93.6 ms a frame; the same image built without the region had both in PSRAM and the converter took 11.9.
  At the most 25.3 to 31.5 KB of the SRAM was in use in each of the four converter plays.
- **The fix.** `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n` in `esp-idf/iclforge/examples/hearth_sink/sdkconfig.p4`, with
  the reasons in a comment beside it. The heap is 32,640 bytes smaller, what spilled into the SRAM goes to PSRAM behind the
  cache, and the allocation policy stays ESP-IDF's default. Nothing in the component asks for `MALLOC_CAP_RTCRAM`, so the
  option is the only way a buffer reached that memory.
- **Ruled out on the way.** The image's layout (the code's addresses were the same in a slow image and a fast one); the
  heap's free and largest block at the start of the play; the order of the plays alone (a fresh boot played the slow play
  slow in the image that printed the addresses); and the L1 data cache's two ways, to which a model of the converter's two
  streams gave no sensitivity to placement.

**Built in D14a6.** The line above, the ESP32-P4 page's tables, stage table and a new section,
[The low-power SRAM](../docs/platforms/bare-metal/esp32-p4.md#the-low-power-sram), and this plan's lines. No source file changed,
so no `float` PCM hash moved: the board's hash of 56 plays and cuts on the default image equals D14a5's, so do the 26 plays
of the 512-byte image, and the probe's six fixtures equal their pins.

**Exit, as measured.** On the P4 with the network up and the default allocation policy, the converter's microseconds a frame,
D14a5's then D14a6's: 7,389 and 7,397 at 24 fps (25/24), 7,285 and 7,280 at 25 fps (15/16), 16,520 and 15,150 at 23.976 fps
(1001/960) and 93,761 and 11,978 at 29.97 fps (1001/960; under the 512-byte policy 17,454 on D14a4's tree, 12,180 on
D14a5's and 12,240 on D14a6's). The frames are 0.79, 0.80, 0.98 and 1.00 of their duration, from 0.91, 0.82, 1.11 and 3.51. The other
plays of the twenty take 3 to 24% less time a frame and the six core-decoding plays 11 to 19% less; synthesis in
`51-music-96` at 2.0 takes 1.3 ms a channel from 13.6, `514-music-768`'s inverse transform 1.0 ms a call from 2.3, and the
inverse transform at 5.1 takes 7.1 to 7.3 ms a frame, from 13.1 to 13.4. The default policy is now the faster of the two on all
twenty-six plays (the 512-byte policy takes 1.04 to 1.16 times as long on the twenty and 1.05 to 1.08 on the six), and the
AC-3 and E-AC-3 decoders take 0.19 (5.1), 0.19 (5.1) and 0.38 (7.1.4) of real time under it from 0.20, 0.20 and 0.38, and
0.21, 0.31 and 0.65 under the 512-byte policy. The peak heap is as before (0.60 to 2.2 MB), PSRAM's peak 0.02 to 0.04 MB
higher and internal RAM's least free 1 to 11 KB (2 to 14 before). Not measured: the 8 KB of tightly coupled memory at 0x30100000, which is
in the heap at the same low priority; the same option on the S3 and the C6; and the tree before D14a's third part.

**Exit:** the 29.97 fps converter at the speed the 512-byte policy gave it (17 ms on D14a4's tree, 12 with D14a5's tables) under
the default policy, the slow QMF stage gone, and the `float` PCM unchanged. Met: 12.0 ms, and the stages are at 1.1 to 1.4 ms a
channel-frame in every play.

**Verified by:** the board (the twenty plays, the six core plays, their cuts, D14b's four cuts and the probe's six fixtures
on the default image, the twenty and the six on the 512-byte image, and the AC-3 and E-AC-3 plays on both); both images built
from the branch tree with the committed configuration; `tools/ci/precheck.py`; `mkdocs build --strict`; the packer's
`--with-ac4 --verify --verify-targets esp32p4`; and, since no source file changed, the host builds and tests (MSVC, GCC 16,
Clang 22) of the base commit, whose code this branch does not touch.

#### D14e: the P4 decodes 5.1 in real time

After D14a6. On the P4 at D14a6 a 5.1 frame took 1.17, 1.52, 1.94 and 3.86 times its duration in SIMPLE mode, A-SPX, A-SPX with A-CPL mode 2 and A-CPL mode 3.
The user took it on 2026-10-01, to run once N1 and D14a5 had merged: decode 5.1 SIMPLE in real time, then A-SPX and, where it can be reached, the A-CPL
modes, at 1.0 times a frame's duration or less at 360 MHz with Wi-Fi up and ESP-IDF's default allocation policy, with the `float` PCM equal to the host's and
the Cortex-M3's, starting from the board's own stage timers; the levers named were A-CPL mode 3, the inverse transform at 5.1, the QMF banks, the vector
extension, where the allocator puts things, the L2 cache's size and a paced I2S; the 29.97 fps converter was D14a6's, and 5.1.4 was to be measured and not chased.
One pull request.

- **Where the time went.** At D14a6 a 5.1 SIMPLE frame took 49.9 ms: parse 13.0, reconstruction 13.7, the inverse transform 7.5, the QMF banks 8.3 and 7.4;
  A-SPX added 15.9, A-CPL mode 2 28.4 and mode 3 116.8. The cache controller's access counters, read in a scratch image, gave about 2,300 64-byte lines of
  code a frame from flash (1,000 of them in parse), 1,700 to 2,600 lines of data from PSRAM in each stage and about 0.7 MB of data a frame through the 128 KB
  L2 cache; a loop of `float` operations on the in-order core ran 2 to 3 times slower as written than unrolled with independent work (a scratch `membench`),
  and a profile by caller found 48,000 calls a frame into the compiler's soft-float `double` routines in A-CPL mode 3, 43,000 of them in its coupling.
- **The transforms and the banks in fewer passes** (`perf(ac4core)`). The FFT's passes are a function for each radix and direction
  (`dsp/fft_kernels.hpp`) in place of a generic butterfly; `Imdct::inverse_overlap()` takes a full-length block after a full-length block from the spectrum to
  the PCM in one pass (the pre-twiddle is the first pass's read, the post-twiddle, the unfolding, the window and the overlap-add one loop); and the A-SPX cubic
  fit's four orthonormal polynomials, which depend on the number of points alone, are made once for a channel and not at every frame in `double`.
- **A-CPL's interpolation once for each run of subbands** (`perf(ac4)`). `acpl::Interpolator` evaluates Pseudocode 109 operation for operation, from the values
  the subbands of a parameter band that share `acpl_param_prev` share, with the ramp's `(ts + 1)` a table entry and the division by 16 or 32 slots a
  multiplication by the exact reciprocal; `AcplStage` forms each slot's coefficients once for a run and narrows them to `Real` once, and `Decorrelator`
  narrows Tables 199 to 201 once. 35,000 evaluations a frame at 5.1 in mode 3 become 8,000.
- **The reconstruction and the output without their copies** (`perf(ac4dec)`). A frame that SIMPLE mode passes through whole is read from `ext` by the
  synthesis where nothing else takes its matrix, and not copied to `out` first; a frame of one long block is ungrouped by swapping buffers; the 256 scale
  factor gains are a table and not a `std::pow` for each band; the concealment spectra change places with the frame's in place of a 48 KB copy; the delay queue
  moves each sample once; an element's tracks (15 KB each) are reserved and not moved 63 times a frame; and the two or four lines of a Huffman codeword are
  stored by name, where GCC made a call into ROM's `memcpy` of them.
- **The compiler and the flash** (`build(esp32p4)`). The kernels of `src/ac4core` at `-O3` and thirteen of `src/ac4dec`'s translation units at `-O2` under
  `ICLFORGE_MINIMAL_HOT_O2`, and `sdkconfig.p4` reading the flash in QIO mode. The mode is the second stage bootloader's, and the application image carries
  none of it.

**What each part bought**, 5.1 on the board, the decoder's microseconds a frame without the first frame and times a frame's duration, D14a6's image and then
the same tree with the parts added, one image each, flashed with its own bootloader and played in the same session
([the P4 page](../docs/platforms/bare-metal/esp32-p4.md#what-d14e-changed) has the stages):

| | SIMPLE | A-SPX | A-SPX, A-CPL 2 | A-SPX, A-CPL 3 |
|---|---:|---:|---:|---:|
| D14a6 | 52,937 (1.24) | 65,797 (1.54) | 82,756 (1.94) | 164,627 (3.86) |
| and the three code commits | 41,390 (0.97) | 48,144 (1.13) | 50,994 (1.20) | 60,398 (1.42) |
| and `-O3` and `-O2` | 35,026 (0.82) | 42,921 (1.00) | 46,329 (1.08) | 56,153 (1.32) |
| and QIO flash | 27,399 (0.64) | 35,621 (0.83) | 38,932 (0.91) | 48,255 (1.13) |
| with the decoder's files at `-Os` | 31,217 (0.73) | 37,496 (0.88) | 40,502 (0.95) | 50,175 (1.18) |

- **Found on the way.** An extra component that includes the decoder's headers and is built without `-ffp-contract=off` changes the PCM on the board: the linker
  keeps one copy of each weak symbol for the whole program, and the profiling component's copy of the QMF kernels, compiled at GCC's default of `fast`,
  contained fused multiply-adds that the decoder then ran (every hash moved and the QMF stages ran slower; the host, which never links such a component, did
  not move). Compare the board's PCM hash after any change to an image's components or flags. A play's time moves with the boot: ten boots of one image gave 5.1
  SIMPLE 28.4 to 29.9 ms and 5.1 A-SPX 36.7 to 37.5 ms a frame (the average with the first frame), and the D14a6 image's SIMPLE play 49.9 and 52.9 ms in two
  sessions. The one-in-twenty slow QMF and transform stage of D14b was the low-power SRAM of D14a6: no stage was slow in the twenty plays of the final image or the
  ten boots.
- **Tried and not taken.** `-O3` on the decoder's files (no gain over `-O2` there, 73 KB more flash); `-funroll-loops` and an instruction-scheduling flag on the
  kernels (1.3 to 3.2 ms a frame for 21 to 31 KB, no more than `-O3`'s 2.7 to 2.9 for 9 KB); a 256 KB L2 cache (2.6 to 3.7 ms a frame, for 128 KB of internal RAM,
  1 KB of it free at the end of a play); a 128-byte L2 line (1.3 to 2.2 ms on three 5.1 plays, 0.6 ms more on another, and three plays of nine stopped on an image
  whose `esp_hosted` buffers are PSRAM that its DMA reads: not traced); the cache's autoload prefetcher, which the ROM has and ESP-IDF does not use (enabling
  it from a scratch image panicked the board, and the setting survived the CPU reset into a boot loop in PSRAM initialisation until a watchdog reset cleared it);
  a 120 MHz flash (`IDF_EXPERIMENTAL_FEATURES` for this part, with the flash's high-performance mode: not tried). Not attempted: the second core, which is idle
  (87% of its samples in a profile) but would take a job hook in the library and a second set of working buffers, and the vector extension, which works on
  integers and so cannot form the `float` kernels' bits.
- **A-CPL mode 3 is not reached** (1.14). Its stage takes 19.9 ms of the 48.7. Some 7 to 11 ms of that is the compiler's soft-float `double` routines at 60 to 100
  cycles a call, which Pseudocode 109's interpolation needs for its bits (8,000 evaluations of three operations and 11,500 narrowings a frame, in a profile of
  the rework's image); the decorrelators, the ducker and the loops over the subbands are `float`, and the rest is passes over 16 KB buffers in PSRAM. A ramp
  scaled by the reciprocal of the slot count would save one multiplication in three of the evaluations, about 2 ms of the 6.0 the mode is over by.
- **The output side.** The measurements use a null sink. Through the example's I2S sink at 2.0, with its default queue of 4 descriptors of 240 frames (21 ms), a
  stream whose frame takes more than the queue holds to decode plays slowly: 5.1 A-SPX folded to 2.0, 35 ms a frame, took 13.5 s for 10.1 s of audio, where a 2.0
  SIMPLE stream (13 ms a frame) took 10.0 s. With 12 descriptors of 256 frames (64 ms, 24 KB of internal RAM at 2.0), which `sdkconfig.p4` now sets, the same stream took 10.0 s with one underrun of 11 ms,
  and 5.1 SIMPLE and A-SPX with A-CPL mode 2 folded to 2.0 played in 10.0 s with at most three underruns and 11 ms of silence in all; A-CPL mode 3 ran dry in every frame (11.4 s).
  The sink's own count is `sink.underruns` and `sink.dry_ms`. The wide TDM sink does not start on this chip revision (`sink/i2s_wide/audio_sink.cpp` has why), so 5.1 itself
  could not be played through a sink, and a wide TDM frame (64 bytes, 196 KB for a 64 ms queue) wants the decoded PCM in a PSRAM ring in front of the sink instead.

**Built in D14e.** `src/ac4core`'s `dsp/fft_kernels.hpp`, `Imdct::inverse_overlap()`, `CubicBasis`, `acpl::Interpolator` and the decorrelator's narrowed tables; in `src/ac4dec`
`SubstreamPcm`'s lazy pass-through (`materialize_out()` and `shift_history()`), `ungroup_in_place()`, `scale_factor_gains()` and the cheaper `parse_sf_data()` store
and `ElementParser::begin()`; the compiler options of both libraries' `CMakeLists.txt`; `sdkconfig.p4`'s QIO line; the page's tables and its Flash mode and What D14e
changed sections. Four test files hold the changes to the old code, to the bit: `test_ac4core_dsp_exact.cpp` (the passes, the transform and the synthesis against
verbatim copies, at the decoder's scalar and at `double`, on dense data and on spectra with runs of zeros of either sign), `test_ac4core_acpl_exact.cpp`,
`test_ac4dec_acpl_exact.cpp` (the stage against a copy of the old one, over several frames that carry state) and `test_ac4dec_reconstruct_exact.cpp`.

**Exit, as measured.** On the board with the network up and the default allocation policy, the decoder's microseconds a frame and times a frame's duration, D14a6's and then D14e's
(`ICLFORGE_EXAMPLE_AC4_PCM_HASH` on, a null sink): 5.1 SIMPLE 49,913 (1.17) and 27,400 (0.64), A-SPX 64,836 (1.52) and 35,589 (0.83), A-SPX with A-CPL mode 2 82,568 (1.94) and 38,823
(0.91), A-CPL mode 3 164,748 (3.86) and 48,689 (1.14); the 5.1 streams folded to 2.0 1.07, 1.46, 1.85 and 3.77 and 0.60, 0.76, 0.83 and 1.06; the 2.0 streams 0.52 and 0.66 and 0.28 and 0.37;
the converter's four streams 0.97, 0.79, 0.80 and 1.00 and 0.68, 0.51, 0.53 and 0.69; 5.1.4 in full decoding 2.4 to 3.4 and 1.57 to 1.90, in core decoding 1.9 to 2.7 and 1.24 to 1.58. The 512-byte
policy takes 1.08 to 1.24 times as long over the twenty plays. The PCM did not move: the host's hash of each of the 84 plays and cuts (MSVC, GCC 16 and Clang 22 at `float`) equals D14a5's, the board's
hash of 52 plays (the twenty, the six core plays, their cuts and the probe's six fixtures) equals the host's, and the six fixtures equal their pins.

**Exit:** 5.1 in SIMPLE mode, A-SPX and, if reached, A-CPL at 1.0 or less with the `float` PCM unchanged, and the exit's figures. Met for SIMPLE (0.64), A-SPX (0.83) and A-SPX with A-CPL
mode 2 (0.91), which also keep up folded to 2.0 (0.60, 0.76 and 0.83); not met for A-CPL mode 3 (1.14), which was named "if reachable"; 5.1.4 is 1.57 to 1.90 in full
decoding and 1.24 to 1.58 in core decoding, measured only.

**Verified by:** the board (the 26 plays and their cuts under the default policy and the 26 plays under the 512-byte one, the control plays with the hash off, AC-3 and E-AC-3, the probe's
six fixtures, and the six quick plays of every image in the table above); `iclforge-tests` whole at `double` on MSVC with every option on, and its AC-4 tests at `float` on MSVC; the WSL GCC 16 and Clang 22
`-Werror` gates at both scalars; GCC 14 with the wheel's flags over the 75 AC-4 translation units; clang-cl over the touched ones; `run_baremetal_probe.sh --ac4 --icount` on the Cortex-M3 under QEMU and `--host` (the six pinned hashes and every ceiling); the packer's `--verify-targets esp32p4`;
`tools/ci/precheck.py`; `mkdocs build --strict`.

#### D14d: the C6, fixed point

After D14e. The user took it on 2026-10-02 as one branch, the host and QEMU first and the board last: `Fixed32` with a block
exponent per QMF slot and per transform block; A-SPX's energies, gains and limiter and the decorrelators' energies as a mantissa
and a power of two; the decorrelators and the converter's taps on 64-bit accumulators; the AC-4 rows of the fixed-point hashes
equal on x86, the Cortex-M3 and the C3; the `double` output and the `float` pins unchanged; the fixed tier's agreement with
`double` stated and pinned; the C6 and C3 images sized; and on the C6, with WiFi up, core decoding first.

- **The tier.** `ICLFORGE_DECODE_SCALAR=fixed` selects `src/ac4core/variants/scalar-fixed/`, whose `Real` is `Fixed32` (Q7.24 in
  an `int32_t`, `src/arithmetic`). The code that differs by tier is chosen by `if constexpr` on `dsp::kFixed<Real>`
  (`dsp/scalar_traits.hpp`), so a `double` or `float` build compiles what it compiled before. In `src/ac4dec`'s files that are not
  templates the fixed branches are templates on the scalar or generic lambdas, for the same reason.
- **Where the values sit.** A sample of PCM has full scale at 8.0 and a QMF sample sits at 2^-16 of the `double` decoder's, two
  shifts chosen from probes of each domain's smallest and largest values. Each track's spectrum carries an exponent: the
  dequantisation puts the track's largest line in [1/2, 1), the tracks a matrix combines are aligned to the largest of them, and
  the inverse transform normalises each block to a largest line in [4, 8). The FFT sheds a bit before a pass wherever a part
  reaches 2^28, and the block's exponent carries every shift out to the PCM. A QMF analysis slot sums its window products in 64
  bits with a Q30 window and normalises its 128 values; a synthesis slot is normalised by the sum of its parts' magnitudes, which
  keeps the bits of a sparse slot that a normalisation by its largest part lost.
- **Energies as a mantissa and a power of two.** `MantExp` (`src/arithmetic/include/iclforge/arithmetic/mant_exp.hpp`: a 30-bit
  mantissa and an exponent, with integer `log2`, `exp2` and square root) holds A-SPX's envelopes, noise floors, gains, limiter and preflattening
  gains, the HF generator's covariances and its solve of Pseudocode 87, the A-CPL transient ducker's energies, companding's levels
  and gains and DRC's slot levels. A gain reaches a sample as one 64-bit product and a shift. At `double` and `float` the same
  code's `Energy<Real>` is `Real`.
- **64-bit accumulators.** The decorrelators' taps are Q30 with `int64_t` sums, rounded once; the converter's taps are Q1.30 with
  64-bit sums.
- **The converter's tables.** Q1.30, built by the compiler as D14a5's `float` ones are: 3,200 bytes at 15/16, 4,888 at 25/24 and
  188,376 at 1001/960. The filter copies a table of up to 16,384 bytes (`kResamplerCopyLimit`) and reads a larger one in place
  from flash, so the 1001/960 table costs no RAM: a C6 has no PSRAM, and 188 KB is most of what it has free with WiFi up. A fixed
  image links these three tables and no `float` one.

**Built in D14d.** In `src/arithmetic`, `mant_exp.hpp`; in `src/ac4core`, the `scalar-fixed` variant, `dsp/scalar_traits.hpp`,
`dsp/qmf_fixed.hpp` and the generated `tables/qmf_tables_fixed.hpp` (`tools/generators/gen_ac4_fixed_tables.py`, with `--check`),
`Fft::inverse_scaled()`, the IMDCT's and the synthesis's overloads that carry a block's exponent, and the fixed branches of
`aspx/hf_generator.cpp`, `acpl/acpl.cpp` and `dsp/resampler.cpp`; in `src/ac4dec`, the per-track exponent of
`pcm/asf_reconstruct.cpp` and `pcm/substream_pcm.cpp`, and the `Energy` types of `pcm/aspx.cpp`, `pcm/companding.cpp` and
`pcm/drc.cpp`. `tests/ac4core/test_ac4core_mant_exp.cpp` is new; the AC-4 tests take their units from
`tests/ac4dec/ac4dec_units.hpp` and pass at every scalar, and D14e's exact tests, which hold the floating paths to their verbatim
copies, run where the scalar is floating. `check_ac4_decode_scalar_snr.py --fixed-cli` measures the tier against `double`;
`run_baremetal_probe.sh --ac4 --scalar=fixed` builds the probe at the tier, with its own instruction and image ceilings. CI runs
both, the scorers with a fixed CLI and the probe's hashes on the Linux host and the Cortex-M3 leg. `CONFIG_ICLFORGE_AC4` no longer
depends on an FPU.

**Exit, as measured.** (a) The fixed decode against the `double` one on every committed stream (67), the worst channel's SNR as
D14a measured the `float` one: below the lowest crossover 105.7 to 132.1 dB (111.3 to 132.1 on DEE's), and above the highest 34.2
to 97.2 dB where a stream has A-SPX (39.2 to 97.2 on DEE's, 35.0 to 96.9 on the object streams, 34.2 to 43.1 on the constructed
ones). `float` measured 109.4 to 136.0 and 37.5 to 102.1. The loudest channel's difference against a full-scale sine is no louder
than -139.4 dBFS on DEE's streams, -134.1 on the object streams and -92.3 on the constructed ones. The floors are pinned in
`tests/golden/ac4dec/scalar-agreement-fixed.json`, 3 dB under the figures, and `score_ac4_decode.py` (15 legs) and
`score_ac4_encode.py` (72) hold their pins with a fixed CLI. (b) The probe's six fixtures give the same PCM hash on the x86-64 host
(GCC 14), the Cortex-M3 under QEMU (GCC 14.2.1) and RV32IMC, the C3's instruction set, built by its compiler (GCC 15.2.0,
`riscv32-esp-elf`) and run on QEMU's `virt` board; they are pinned in `tests/golden/ac4-fixed-probe-pcm-hashes.json`. The C3
machine of Espressif's QEMU has 400 KB of SRAM and no PSRAM, less than the smallest fixture's peak of 429,667 bytes, so the RV32
run was made by hand with the toolchain's semihosting library and is not in CI. (c) On the Cortex-M3 leg the tier takes 38.2 M,
34.2 M, 55.6 M, 52.5 M, 90.4 M and 39.9 M instructions a frame for the six fixtures, 0.43 to 0.70 of the `float` tier's; the peaks
are 429,667 to 1,825,056 bytes, 1.4 to 5.2% above `float`'s; the stack 21,080 bytes; the image 727,656 bytes, 35,760 more than
`float`'s. (d) The `double` and `float` outputs are byte-identical to `main` (`dd67c6461`) on every tracked AC-4 stream as coded,
folded to 2.0 and in core decoding (252 decodes, and 3 that both refuse) and on 12 encodes; no `float` pin moved. (e) The
component builds for the C3, the C6 and the S3 with AC-4 on (the packer's `--with-ac4 --verify`). A C6 image of the AC-4 decoder
alone is 1,096,256 bytes with 48,560 of static RAM, and the same image at `float` 942,464; the C3's is 1,093,920 with 47,567. (f)
On the C6 nothing fits beside WiFi: the decoder peaks at 429,667 bytes at 2.0 on a 32-bit core, and the board had about 236,000
bytes free with WiFi up (285,000 with WiFi's code kept in flash, as the C6 player's overlay keeps it). Hearth sends a C6 sink PCM
([decision 32](#decisions-of-2026-09-25)). The board has not run the tier yet.

**Verified by:** the host's `iclforge-tests` built with Clang 22 and `-Werror` at `double` and `float`, whole (2,486 cases
each), and at fixed, where every AC-4 test passes and the only failures are the 184 assertions of AC-3 and E-AC-3 tests
(`test_eac3_search.cpp`, `test_eac3_selfcheck.cpp`, `test_cli.cpp`) that `main` fails at fixed as well; GCC 14 `-Werror` with the
wheel's settings over the AC-4 libraries at `double` and at fixed; `run_baremetal_probe.sh --ac4 --scalar=fixed --icount` on the
Cortex-M3 under QEMU and the host probe at fixed; the RV32 run; `check_layering.py`; the packer's `--verify` for `esp32c3`,
`esp32c6` and `esp32s3`.

#### D14f: the decoder's memory

After D14d, before its board run. The user took it on 2026-10-03 with no fixed target: every reduction that leaves the PCM
of every tier as it was (`double` byte-identical to `main`, the `float` and fixed pins unmoved), each step measured on the
probe, and the larger redesigns decided once the smaller ones had shown where the peak was.

- **Where the peak was.** The probe now prints, for each fixture, the frame and the stage its peak fell in and the live bytes
  by size class then (`<fixture>.peak_frame`, `.peak_stage`, `.peak_live[...]`; the stage in a build with stage timers). Every
  fixture peaked in its second or third frame, once the lazily made buffers had all been made, 2.0 in the next frame's parse
  and the A-SPX streams in A-SPX.
- **The inverse transform's tables in flash** (`perf(ac4core)`). For the five block lengths of a 2048-sample frame at 44.1 and
  48 kHz, the FFT's roots, the IMDCT's pre-twiddles, the fixed tier's post-twiddles and the KBD windows are narrowed to the
  tier's scalar while `src/ac4core/src/dsp/transform_tables.cpp` compiles, from doubles `tools/generators/gen_ac4_transform_tables.py`
  computes as the runtime code does (the same operations in the same order and the same C library's `cos`, `sin` and `sqrt`,
  written as exact hexadecimal literals). `tests/ac4core/test_ac4core_transform_tables.cpp` holds every narrowed value to the
  runtime computation's bits on the compiler that runs it. `double`, and every other length, builds its tables as before.
- **A-SPX's high band assembled in place.** Pseudocode 106 reads each value of the high band only where it writes Y, so one
  buffer for an element's channels replaces the two each channel allocated.
- **Sized to the frame.** A channel element's stereo parameters hold the frame's window groups, not sixteen; the A-CPL stage
  makes each decorrelator when a module first takes it, in the state `reset()` gives (a pair takes one of five).
- **One channel's inverse transform at a time** where no S-CPL needs every channel's at once.
- **The syntax.** `quant_spec` in sixteen bits (an escape's magnitude is at most 8,191), an audio substream parsed straight
  into its capture, and a frame's tracks freed once reconstruction and the stereo and multichannel steps have read them.
  Keeping the parsed vectors' capacity across frames, which would cut the 48 to 202 allocations a frame but not the peak, is
  not done.
- **A channel's QMF matrix in its Q_low_ext.** The rolling input of eleven slots first proposed cannot be built: A-SPX's
  predictor sums its covariances over all 42 input slots (Pseudocode 86). What is built instead: A-SPX writes each slot of a
  channel's matrix from a slot of Q_low_ext at or after it, and the next frame's history is Q_low_ext's last ten slots, so the
  matrix takes Q_low_ext's first 32 slots, and the history moves to the front when the next frame's `render()` begins, after
  every stage and every substream that mixes this one has read the matrix. The stages after A-SPX, and A-JOC's inputs in
  `src/ac4core`, take the matrices as spans (`QmfMatrix`).

**What each step bought**, the fixed tier's peak heap in bytes on the x86-64 host (GCC 14), each row with the ones above it:

| | `ac4_20_music` | `ac4_20_acpl` | `ac4_51_music` | `ac4_51_acpl` | `ac4_514_tones` | `ac4_20_companding` |
|---|---:|---:|---:|---:|---:|---:|
| D14d | 438,527 | 634,760 | 988,038 | 1,190,298 | 1,848,776 | 494,531 |
| the tables in flash | 359,511 | 555,744 | 909,022 | 1,111,282 | 1,769,760 | 415,515 |
| A-SPX in place | 359,511 | 536,288 | 889,566 | 1,091,826 | 1,769,760 | 396,059 |
| sized to the frame | 347,247 | 477,580 | 881,398 | 1,028,634 | 1,747,704 | 383,795 |
| one transform buffer | 339,031 | 469,364 | 840,318 | 987,554 | 1,747,704 | 375,579 |
| the syntax's two | 331,277 | 468,820 | 830,438 | 986,138 | 1,735,648 | 372,971 |
| the tracks freed | 328,009 | 468,270 | 820,375 | 984,704 | 1,723,427 | 370,291 |
| the matrix in Q_low_ext | 295,225 | 435,486 | 722,007 | 886,336 | 1,526,819 | 337,507 |

**Built in D14f.** `src/ac4core`'s `dsp/transform_tables.{hpp,cpp}`, the generated `tables/transform_tables.hpp` and the
generator; `Fft`, `Imdct` and `TransformSet` reading the tables; A-JOC's inputs as spans. In `src/ac4dec`, `pcm/aspx.cpp`,
`pcm/stereo.{hpp,cpp}`, `pcm/acpl.{hpp,cpp}`, `pcm/substream_pcm.{hpp,cpp}` (`Channel::out()`, the shared transform buffer,
the history's move, the tracks' release), `syntax/asf.{hpp,cpp}`, `decoder.cpp` and `QmfMatrix` through A-JCC, A-JOC, dialogue
enhancement, DRC, the downmix and mixing. The probe's peak attribution (`apps/baremetal/ac4_probe.cpp`, `stage_timers.cpp`)
and its ceilings, and `run_baremetal_probe.sh --ac4 --stage-timers`.

**Exit, as measured.** (a) The PCM did not move: the probe's six hashes at `float` and fixed on the x86-64 host, the Cortex-M3
under QEMU and (fixed) RV32IMC equal their pins after every step, and the `double` and `float` decodes of every tracked AC-4
stream as coded, folded to 2.0 and in core decoding (252, and 3 both refuse) and 12 encodes are byte-identical to `main`.
(b) On the Cortex-M3 the peaks are, at fixed, 286,365, 426,918, 704,311, 868,424, 1,502,903 and 329,147 bytes for the six
fixtures (from 429,667, 626,368, 970,430, 1,172,502, 1,825,056 and 486,331), and at `float` 286,365, 418,110, 696,375,
859,616, 1,494,319 and 321,307 (from 413,611, 601,504, 946,390, 1,147,590, 1,800,312 and 462,435 at D14e): 2.0 by a third,
5.1 by 27%, 5.1.4 by 18%. (c) The first frame no longer builds the tables in software floating point, so the probe's
instructions a frame (an average over its three or four frames) fall at fixed to 6.7 M, 10.5 M, 24.0 M, 28.8 M, 43.0 M and
8.3 M from 38.2 M to 90.4 M, and at `float` to 25.2 M, 35.1 M, 87.2 M, 100.7 M, 161.8 M and 29.3 M from 54.8 M to 206.3 M.
(d) The images grow by the tables: 801,812 bytes at fixed (from 727,656) and 750,276 at `float` (from 691,896) on the
Cortex-M3, and on an ESP32-C6 an image of the decoder alone 1,164,592 (from 1,096,256) with its static RAM unchanged at
48,560. (e) On the C6, 2.0 at 286,365 bytes is level with the about 285,000 the board had free beside WiFi with WiFi's code
in flash (the probe's network image) and below the 383,416 it had with no network; the Sendspin player leaves less than either.

**Verified by:** after every step, the host probes at `float` and fixed and their hashes, the comparison with `main`, and
`iclforge-tests` built with Clang 22 and `-Werror` at `double`, `float` and fixed (2,488 cases pass at `double` and `float`; at
fixed every AC-4 test passes and only `main`'s AC-3 and E-AC-3 failures remain); at the end the Cortex-M3 legs (`--ac4`,
`--ac4 --icount`, `--ac4 --scalar=fixed --icount`) against the new ceilings, RV32IMC, GCC 14 `-Werror` with the wheel's
settings, and the packer's `--verify` for `esp32c6`.

### Encoder phases

Each encoder phase follows the decoder phase that decodes what it writes and uses what that phase
built. Its pull request records the race against DEE at the phase's layouts and rates, and
librempeg's decode of the encoder's streams, besides the exit criterion. What it writes outside
DEE's set is an experimental option
([What the encoder writes by default](#what-the-encoder-writes-by-default)), whose exit is a clean
decode with the invariants holding and MediaInfo's and librempeg's readings recorded.

#### E1: the encoder library, the frame writer, and SIMPLE mono and stereo

**Status:** merged as #1011 on 2026-09-25. Exit met.

- `src/ac4enc/`, with its CMake, tests, an instrumented fuzz target over its configuration and
  input, the write trace, and `src/ac4enc/ERRATA.md`.
- The frame writer: `ac4_toc()` at bitstream version 2 with one version 1 presentation, one
  substream group and the substream index table; the presentation substream with dialnorm; the audio
  substream's `metadata()`; the sync frame, with Annex G's CRC on request; and raw frames for MP4
  samples. The table of contents is written from an `ac4::Toc`, so `ac4::build_dac4()` and
  `ac4::rfc6381_codec_string()` describe the encoder's output as they describe a stream they read.
  `sequence_counter` starts at 0 in a file's first frame, as Part 1 Annex E.1 asks of ISOBMFF,
  counts to 1020 and wraps to 1.
- ASF: the forward MDCT from D2's core; transform lengths chosen by a transient detector within Part
  1 Table 187's splits; the psychoacoustic model; scale factors and quantisation; section and
  codebook choice; and the escape codes.
- Stereo processing: MDCT-domain M/S and prediction per band, chosen by the energy each saves.
- A constant bit rate (`wait_frames` 0) at `frame_rate_index` 13 and 48 kHz, each frame filled to
  its size, and I-frames at an interval the caller sets, which predict nothing across time.
- `ac3cli ac4-encode` writes AC-4 in these modes, raw or through the MP4 muxer, as `eac3-encode`
  writes E-AC-3.
- Mono, in the `single_channel_element`, written without a flag: MediaInfo reads it as configured.

**Exit:**

- Every stream the encoder writes, over the encoder-space harness's configurations and the race's
  legs, passes the encoder's ladder items 1 to 3: three identical traces, the invariants,
  MediaInfo's trace as configured, FFmpeg's framing, and DEE's MP4 muxer's `dac4`.
- Decoded by D2's decoder, the programme fixtures and synthetic signals score at or above floors
  pinned at the first measurement less 1 dB, per channel, with log-spectral distance and ViSQOL
  pinned the same way.
- The race at 2.0 from 192 to 768 kbps, where DEE writes SIMPLE, is recorded and pinned.

**Verified by:** `ac3tests`; the encoder-space harness in CI; the race and MediaInfo locally.

#### E2: A-SPX and companding

**Status:** merged as #1013 on 2026-09-25. Exit met.

- QMF analysis from D3's core, aligned to the decoder's delays (Part 1 Tables 188 and 192).
- The A-SPX encoder: `aspx_config` (crossover, master frequency table, noise subband groups) from
  the rate; framing from a transient detector, in FIXFIX, FIXVAR and VARFIX, with a FIX end followed
  by a FIX start; envelopes estimated against what D3's HF generator, run from the core,
  regenerates; noise floors and added tones; delta coding and codebook choice; balance coding for
  stereo; and Part 1's limits of five noise groups, five patches and four or five envelopes.
- Companding: the compressor, as the inverse of the decoder's expander, on and off by rate as DEE
  uses it.
- The ASPX codec mode for mono and stereo. VARVAR framing and interleaved waveform coding as
  options. E2 writes balance, VARVAR and frequency interleaving behind `experimental=`, and not time
  interleaving, whose slots take the spectral frontend's output across the whole band and would need
  it coded full band in two frames around each.
- E2 found the spectral frontend's rate loop of E1 unfit below 64 kbps a channel: no frame there
  holds its bands at their masking thresholds, and raising every band's noise over its threshold
  together left 6 dB of SNR in every band of music at 48 kbps, where DEE keeps 19 dB in the bass.
  Such frames now pull every band toward one level of noise, with each band's noise capped at a
  multiple of its energy, which ViSQOL rewards over leaving holes; frames that hold their thresholds
  keep E1's law, which ViSQOL prefers there.

**Exit:** decoded by D3's decoder: below each stream's crossover, per-channel SNR at or above the
pinned floor; above it, each A-SPX tile's energy within the quantiser step of the source's plus the
pinned tolerance; log-spectral distance and ViSQOL pinned. The race at 2.0 and 48, 64, 96, 128 and
144 kbps.

**Verified by:** as E1.

#### E3: the 5.X element

**Status:** merged as #1025 on 2026-09-25. Exit met.

- 5.1 and 5.0 in SIMPLE and ASPX modes: the coding configuration and matrices of Part 1 Tables 178
  to 185, chosen per frame by the energy they save; the LFE; one budget shared across the channels;
  A-SPX's channel pairing, within Tables 212 to 214.
- The 7.X element (7.1) as an option.
- Since D4 found DEE's 5.1 streams in one form, `coding_config` 0 with `2ch_mode` 0, that form is what
  the encoder writes by default, with DEE's 5.1 A-SPX configuration (a 12 kHz crossover, 12.75 kHz
  from 256 kbps, no companding) and its LFE band (three scale factor bands, to 140.6 Hz). The other
  coding configurations, chosen per frame by the bits their matrices and side information cost, and
  the 7.X element in its three layouts, are experimental options. Each pair and C switch blocks on
  their own transients; under the experimental configurations the five channels share one layout.
- E3 found librempeg reading the default form as the decoder does, and not the experimental ones:
  its matrices of three to five channels and the 3.0 element's pair come out 6 to 19 dB down, it
  refuses the 5/2/0 and 3/2/2 layouts, and it writes a 3/4/0 stream's L on every channel. DEE's
  muxer leaves 3/2/2's top front pair out of the `dac4` channel groups Part 2 Table A.27 and
  Pseudocode E.3 both give it (`src/ac4enc/ERRATA.md`).
- In the race, this encoder's SNR below the crossover trails DEE's by 7.3 to 11.6 dB at 192 kbps,
  where DEE keeps the bass clean and lets the band from 8 kHz go, with ViSQOL at or above DEE's
  there; above 288 kbps its SNR leads and its ViSQOL is up to 0.05 under DEE's on film. DEE splits a
  fifth of its frames into two blocks of 1,024 samples, where this encoder's transient detector
  splits under one in a hundred; splitting more moved ViSQOL by no more than 0.02 and was left out.
  Both are room for the encoder's tuning later.

**Exit:** one tone per channel lands on its own channel, the LFE included; each channel meets E1's
and E2's checks; the race at 5.1 from 192 to 768 kbps.

**Verified by:** as E1.

#### E4: A-CPL

**Status:** merged as #1029 on 2026-09-25. Exit met.

- Parameter extraction per parameter band and time slot, quantisation at the fine or coarse step,
  differential coding in time or frequency, and the downmix that is coded, normalised as the
  decoder's upmix expects (Part 1 Pseudocodes 115 to 117).
- ASPX_ACPL_2 and ASPX_ACPL_3 in the 5.X element. ASPX_ACPL_1 with its residuals, and A-CPL in a
  channel pair, as options.

- E4 found DEE's A-CPL streams in one configuration: 15 parameter bands at the fine step, one
  parameter set a frame, smooth interpolation in all but about one frame in a hundred, DIFF_FREQ in
  I-frames, no companding, and A-SPX
  from subband 32 to 23.25 kHz with a 12.75 kHz crossover in ASPX_ACPL_2, to 18.75 kHz from 12 kHz in
  ASPX_ACPL_3. The encoder writes that. Its ASPX_ACPL_3 gammas follow DEE's four relations, which
  DEE's streams hold in all but 37 of 7,110 bands (`src/ac4enc/ERRATA.md`, "ASPX_ACPL_3's gammas").
- Each band's parameters are estimated over 48 QMF slots centred on the frame's last, where smooth
  interpolation reaches them, from a DFT of each subband's slots, reading the bins of its own band. A
  subband's own band lies in half of its spectrum; its neighbours reach the other half through the
  prototype's transition band, and read from the whole spectrum they pulled ASPX_ACPL_3's centre
  prediction off on the 5.1 tones, whose routing margin went from -3.7 dB to 9.7 (DEE's 8.4).
- In the race, each band's level difference lands 0.02 to 0.13 dB nearer the source's than DEE's,
  the correlation within 0.007 of DEE's distance, and ViSQOL 0.01 to 0.06 above DEE's but for film
  at 128 kbps, 0.12 under. There the coded C trails DEE's by 9.5 dB of SNR below 2 kHz: DEE gives C as
  many bits as the whole A/B pair. Neither a 6 dB tighter allowance on C nor capping the TNA mode at
  1 moved C's ViSQOL by more than 0.01, and C's high band is as near the source's level as DEE's,
  so the gap is the core coder's, room for its tuning later with E3's.
- kAuto's rates make ASPX_ACPL_3 the mode from 20 kbps in 5.1, whose least frame, with eleven
  parameters a band in an I-frame, needs 25 kbps; where the rate cannot hold a mode's least frame
  kAuto takes the next of ASPX_ACPL_2 and ASPX that it can.
- The experimental options: ASPX_ACPL_1 in 5.X codes each pair's residual to 3 kHz (`acpl_qmf_band`
  8), which takes each band's level difference from 2.57 to 1.92 dB of the source's on music at 128
  kbps, and ViSQOL from 4.65 to 4.60, the residuals' bits taken from the downmixes; at 160 kbps it
  reaches 4.67. A-CPL in stereo: ASPX_ACPL_2 0.11 ahead of ASPX at 32 kbps and 0.04 under it at 48,
  ASPX_ACPL_1 0.04 over it at 64. librempeg refuses the ASPX_ACPL_1 streams, whose residuals and side send
  fewer bands than their bases, and reads D5's constructed ones, which send as many. The stereo form
  showed the decoder reading a side with fewer bands than its mid at the mid's offsets, fixed in D5
  (`src/ac4dec/ERRATA.md`, "get_max_sfb() with b_dual_maxsfb").

**Exit:** for each A-CPL parameter band and each reconstructed pair, the decoded level difference
and correlation within the tolerance fixed at the first measurement; the waveform-coded channels
meet E1's and E2's checks; the race at 5.1 and 96, 128 and 144 kbps.

**Verified by:** as E1, with D5's per-band script.

#### E5: metadata, frame rates and I-frames

**Status:** merged as #1046 on 2026-09-26. Exit met.

- Loudness: dialnorm and the further loudness values, as the caller supplies them; `ac3cli`
  measures them with the BS.1770 meter it has.
- DRC: each decoder mode configured with a default profile, a compression curve or a repeat of
  another mode, and `drc_eac3_profile` for a transcoder (Part 1 5.7.9.4); transmitted gains as an
  option, computed from a profile.
- Dialogue enhancement: the channel-independent method, with its parameters computed from a
  dialogue stem or from channels the caller marks as dialogue
  ([decision 18](#decisions-for-the-encoder-and-the-applications)), and the cap the caller sets. The
  Mid of L and R and the cross-channel method as options. The hybrid methods, 2 and 3, add a
  dialogue waveform in a substream of its own, and so go with E6's `presentation_config` 1.
- Downmix: mixing gains, the preferred method, and custom downmix data.
- Rates: an average bit rate within the buffer Part 1 6.2.4 sets, with `wait_frames` signalling the
  wait, and a variable bit rate.
- Every frame rate of Part 1 Table 83, through D6's converter in the other direction, with the
  output sample count locked to `sequence_counter` as Part 2 5.11 requires; 44.1 kHz at index 13.
- I-frames at an interval, at forced positions, and at every fragment boundary a caller names.

**Exit:**

- MediaInfo's trace shows every loudness, DRC, dialogue enhancement and downmix value as configured,
  over a table of configurations covering each field.
- Through D6's output processing, the output level gain, the dialogue enhancement gains and the
  downmix matrices measured on the encoder's streams equal their formulas to 0.01 dB.
- At every frame rate the decoded sample count is exact over 100,000 frames, and the decoded signal
  scores within a pinned allowance of the same source at index 13.
- An average-rate stream never needs more than the buffer it signals, checked frame by frame.

**Verified by:** `ac3tests`; the gain scripts; MediaInfo locally.

#### E6: presentations and several substreams

**Status:** merged as #1058 on 2026-09-26. Exit met.

- Several substreams and substream groups, and presentations of each `presentation_config` Part 1
  Table 85 lists: music and effects with dialogue (0), main with dialogue enhancement (1), whose
  dialogue substream the hybrid dialogue enhancement methods (Part 1 Table 170's 2 and 3) take, main with
  associated audio (2), music and effects with dialogue and associated audio (3), main with dialogue
  enhancement and associated audio (4) and main alone (5), and Part 2's EMDF-only presentation (6).
  Alternative presentations; names, languages, content classifiers and group gains; the `md_compat`
  level each presentation needs; EMDF payloads passed through.
- The rules a presentation keeps: dialogue and associated substreams add no channel the main one
  lacks, except mono; 3.0 carries only a dialogue enhancement signal or the dialogue of a music and
  effects presentation; and CMAF's limits hold: 64 presentations at most, a `presentation_id` in
  every sample and one table of contents configuration throughout.
- E6 found the text leaving a writer these choices (`src/ac4enc/ERRATA.md`, "Presentations"): the
  tracks `md_compat` counts, every channel but the LFE of every substream a presentation names, the
  dialogue enhancement substream's included, which DEE's levels agree with (0 in stereo, 1 in 5.1, 2
  in 5.1.4, and `presentation_id` 0 on their one presentation, which E1 to E5 left out); a name sent
  whole, at most 31 bytes; one target for an alternative presentation of channel-coded substreams,
  whose `alt_data_set_index` has no object metadata to pick; the substreams' order, presentation
  substreams, then audio, then EMDF payloads, which librempeg depends on; and what a hybrid method's
  waveform carries: each processed channel's dialogue, the Mid's sum, or the dialogue projected on its
  panning. A configuration 6 presentation has no field for the `presentation_id` CMAF asks of every
  presentation, and DEE's muxer warns of it.
- MediaInfo reads the encoder's tables of contents as configured, but reads no audio substream of a
  stream with a configuration 6 presentation, gives no language to a presentation whose one group is
  associated audio, and frames the EMDF payloads substream without detailing it. DEE's MP4 muxer
  refuses a stream of more than one presentation, and on the 15 presentations of the broadcast stream
  it hangs. librempeg decodes a presentation's first group alone, as D7 found, is silent on 15
  presentations over 22 substreams, and refused a frame whose EMDF payloads substream came before the
  audio.
- In the race against G1's legs, which DEE encoded one at a time and D7's multiplexer puts into the
  same presentations, the encoder's SNR is within 0.1 dB of DEE's or above it with music at 128 kbps
  and dialogue and associated audio at 64, and 5.5 to 6.3 dB above it at 192 and 128 kbps, ViSQOL
  within 0.03 of DEE's throughout.
- Left for E7: the options of `ac3cli ac4-encode`, and the MP4's `dac4`, which `ac4::build_dac4()`
  writes whole only for a presentation of one substream (Annex E.10 describes the others); and for
  later, transmitted DRC gains computed from a presentation's mix, where E6 takes the main
  substream's input.

**Exit:** the decoder's D7 selection and mixing on the encoder's streams give the configured
presentations, measured with one tone per substream; MediaInfo's trace lists presentations, names,
languages and levels as configured.

**Verified by:** `ac3tests`; the gain scripts; MediaInfo locally.

#### E7: the encoder's API, the CLI and packaging

**Status:** merged as #1061 on 2026-09-26. Exit met.

- `EncoderConfig` and `Encoder` in their final form, and the function that wraps a frame in a sync
  frame.
- `ac3cli ac4-encode`, which E1 to E5 grew, in its final form: options for layout, bit rate and
  rate control, frame rate, codec mode, presentations, loudness, DRC, dialogue enhancement,
  downmix, I-frames, CRC and the experimental tools.
- The encoder installed and exported beside the decoder, with an ABI allowlist and the package
  check; the documentation pages; the status table's encoder rows; CHANGELOG and a ROADMAP entry.
- E7's `Encoder::refusal_reason()` names the first rule a configuration breaks, as a string
  literal, across the substreams, the presentations, their metadata and the rate; `create()` still
  answers `kInvalidConfig` alone. Every field of the configuration's structures has a default, so a
  designated initializer names only what it sets, as D8's decoder configuration does. The members of
  `Encoder::Impl` defined out of line are hidden, and `tools/ci/abi-allowlist/libiclforge_ac4enc.so.txt` lists
  the header's API alone.
- `ac3cli ac4-encode` takes the substreams and presentations as numbered options, `substream2=` to
  `substream32=` and `presentation1=` to `presentation64=`, each with keys of its own
  (`substreamN-content=`, `presentationN-config=` and the rest), substream 1's values being the bare
  options or `substream1-` keys. Without them it builds the configuration as before, so the
  committed commands' streams do not change. `crc=off` writes sync frames without Annex G's CRC, and
  `dialogue-hybrid=` with `substreamN-enhances=` gives the hybrid methods their waveform. Every option
  has a test that reads what it writes back from the table of contents or the syntax trace, and a
  configuration the encoder refuses is refused with its reason.
- The MP4's `dac4` (Part 2 Annex E.10) describes every presentation: each group its specifiers name,
  the channel mode, core and channel groups by Pseudocodes 25, 26 and E.3 over all its substreams,
  A-JOC and direct-coded object groups, the program identifier and an alternative presentation's
  `alternative_info()`, whose name and target the decoder now reports. Where it cannot describe a
  presentation whole it writes nothing, and `dac4_refusal()` says why. DEE's muxer refuses a stream
  of more than one presentation and does not finish one of an alternative presentation, so the box
  is held to the text and to MediaInfo's trace, and to the muxer's box byte for byte for Chromium's
  A-JOC stream and DASH-IF's 5.1 test vectors. MediaInfo reads `n_targets` as `n_targets_minus1`.
  `src/ac4enc/ERRATA.md` records the readings, among them where Pseudocode E.3 leaves channel groups
  out and when `b_presentation_core_differs` is set.
- A configuration 6 presentation has no field for the `presentation_id` Annex H.1.2.1 asks of every
  presentation of a CMAF track. `ac4::cmaf_refusal()` names the rule a table of contents breaks, and
  `ac3cli fmp4`, which fragments AC-4 since I1, refuses such a stream by it; an MP4 that is not
  fragmented carries it.
- The encoder is installed and exported beside the decoder, `ac4::encoder_static` and
  `ac4::encoder_shared` with pkg-config `ac4enc`, in the shape D8 gave the decoder: each library links
  the inspector of its own kind and the core privately, and the core's archive is installed wherever
  a static decoder or encoder is. `check_install_consumer.sh` encodes a tone through each installed
  encoder by CMake and by pkg-config and reads it back with the inspector. The vcpkg port and the
  Conan recipe install the AC-4 libraries only where asked for, through an `ac4` feature and
  option that are off by default, and IAB and IAMF the same way (`iab`, `iamf`), as the user
  decided on D8's question: a curated vcpkg port's default features may enable behaviours, not
  public targets. Both pin Hearth off, which upstream refuses beside the AC-4 libraries off and
  whose dependencies neither declares. `check_packaging_versions.sh` holds the two recipes to the
  same components and fails an option upstream defaults ON that a recipe neither offers nor pins, and
  `check_install_consumer.sh` checks that a tree built without a library installs no file of it.
- The encoder-space harness draws further substreams in one case in five, in each configuration of
  Table 53, with rate shares, mixing values, ids, levels, names and payloads, through the CLI's
  options. It found three faults the tests had not: at an average rate a frame could give the
  substream taking what the others leave less than its least frame, which the encoder then could not
  write, so each substream now keeps its least frame first and the rest goes by need; a frame
  between I-frames was sized for a dialogue stem's parameters coded against the last frame's, where
  it falls back to the last frame's kept, and so found no size (E6's sizing, which it now checks
  with the fallback's metadata); and the 7.X layout's pair went to every substream, so a 7.1
  substream refused a mono one beside it.

**Exit:** every CLI option has a test; the packages contain the encoder; the documentation gates
pass.

**Verified by:** `ac3tests` and the CLI tests on every leg; the package check; the documentation
gates.

#### E8: channel-based immersive

**Status:** merged as #1071 on 2026-09-26. Exit met. The race's sweep legs stood 0.03 to 0.18 under
DEE's ViSQOL from 256 to 512 kbps, which [E10](#e10-a-spx-noise-floors-on-sweeps) closed.

- The immersive element for 5.1.4 in SCPL, ASPX_SCPL and ASPX_ACPL_2, as DEE writes it; S-CPL's
  matrices; A-SPX's immersive pairing; the height downmix values.
- ASPX_ACPL_1, 7.1.4 with every channel present, and A-JCC, as options.

**Exit:** one tone per channel on its own channel; each channel meets the checks of E1 to E4 in full
decoding, and core decoding of the encoder's streams gives what D9 checks; the race at 5.1.4 from
192 to 768 kbps, scored in full and in core decoding.

**Verified by:** as E1.

**Built** (phase E8): 5.0.4 and 5.1.4 in the immersive element as DEE writes it, `core_5ch_grouping` 0
with `2ch_mode` 0, SCPL from 640 kbps, ASPX_SCPL from 480 and ASPX_ACPL_2 below, with DEE's A-SPX
configurations by rate; each coupled pair coded as its sum and difference, the difference predicted from
the sum band by band where that costs fewer bits, which is what DEE's streams send (`sap_mode` 3, which
corrected the decoder's register); the height downmix as custom downmix data in I-frames; and 7.0.4 and
7.1.4 with the back pair, ASPX_ACPL_1 and A-JCC (`ajcc_core_mode` 0) as experimental options. Every
channel's tone decodes on its own channel in full decoding and at the core's gain in core decoding, in each
mode. Measured locally against DEE's 5.1.4 legs from 192 to 768 kbps, in full and core decoding
(`tools/checks/score_ac4_encode.py --gold`, pinned): ViSQOL within 0.035 of DEE's or over it on music, film
and speech, the SNR below the crossover up to 10.5 dB under DEE's from 192 to 320 kbps and within 1.4 dB
of it or over it from 384;
on sweeps the shared A-SPX encoder left the band above the crossover emptier than DEE's did, 0.03 to 0.18
under DEE's ViSQOL from 256 to 512 kbps, which [E10](#e10-a-spx-noise-floors-on-sweeps) closed.
librempeg does not decode the immersive element.

#### E9: A-JOC objects

**Status:** merged as #1082 on 2026-09-29. Exit met: MediaInfo's object count and bed were run
afterwards (#1103), which names a static bed and cannot tell a bed object from a dynamic one; the
listening is the user's to do; there is no race, since DEE refuses this project's masters, and
librempeg refuses object coding.

- Objects and their metadata in, converted by the applications from the scene descriptions they read
  (ADM BWF through `ac3adm`, IAB through `ac3iab`, `ObjectScene`).
- The A-JOC downmix, computed or a static 5.0 or 5.1 bed as Part 2 allows, and the dry and wet
  matrices estimated against D10's reconstruction; object audio metadata (common, timing and dynamic
  data), with the downmix signals' own metadata for core decoding; the presentation that carries
  them, within `md_compat`'s object limits. Direct-coded object substreams as an option.

**Exit:** decoded in full by D10's decoder, each object's reconstruction scores against the object
the encoder was given, with correlation and SNR pinned per object; core decoding gives the downmix
with its metadata; MediaInfo reports the object count and the bed; if G0 found a master DEE's A-JOC
encoder accepts, the race runs against its streams; the objects move as their metadata says, by
listening.

**Verified by:** `ac3tests`; listening; the librempeg comparison.

**Built** (phase E9): with `experimental.objects`, one object substream of objects and their Annex F
properties over time (`ObjectsConfig`, and `encode()` taking `ObjectMetadataUpdate`s beside the PCM;
`ObjectProperties` moved to `ac4/ac4.hpp`, where the decoder reports it). As an A-JOC substream: a
computed downmix of one to eleven signals in a `var_channel_element()`, each the sum of a run of the
objects in azimuth order and sent for core decoding at its group's centre, or a static 5.0 or 5.1 bed
the objects are panned onto, coded in SIMPLE or ASPX; the dry matrices chosen frame by frame by running
`src/ac4core`'s reconstruction on candidate fits and keeping the one that comes closest to the objects,
the decorrelators' wet matrices as an option; bed objects listed in the upmix; and the metadata's blocks
at the 32-sample step of each update, landing where its input sample comes out. Or direct-coded, the
dynamic objects in mono, stereo, 3.0 and 5.0 elements with the LFE's `mono_data(1)` before the first,
and the group's OAMD substream. One presentation of the substream alone, at md_compat 3 for A-JOC and by
its tracks direct-coded, through E6's presentation machinery. Decoded in full, each object of the
committed cases comes back at 40 to 75 dB SNR against its source, the tones each in a parameter band of
their own (pinned in `tests/ac4enc/test_ac4enc_objects.cpp`), and core decoding gives the downmix at its
metadata; the encoder's trace, the decoder's and the Python parser's agree on the committed streams
(`tests/golden/ac4dec/objects/encoder-*.ac4`) and on the encoder-space harness's object draws.
`ac3cli ac4-encode objects=` takes a scene file of the library's terms for the harness and the listening
streams; the applications' scene readers are I5's. DEE writes no A-JOC from this project's masters, so
there is no race, and librempeg refuses object coding, so there is no second decode; MediaInfo's reading
(`tools/checks/check_ac4_encode_readers.py --only objects`) needs DEE's install, and was run
afterwards (#1103): it counts the objects and names a static bed, and cannot tell a bed object from
a dynamic one. Bed objects in direct-coded substreams, objects beside channel-coded substreams, frame rates other than index 13 and
the intermediate spatial format are refused.

#### E10: A-SPX noise floors on sweeps

**Status:** merged as #1113 on 2026-09-29. Exit met.

- E8's race left the shared A-SPX encoder 0.03 to 0.18 under DEE's ViSQOL on 5.1.4's sweeps from 256 to
  512 kbps, the band above the crossover emptier than DEE's. Which layouts and rates show it, the two
  streams' A-SPX side information and decoded band compared frame by frame, and the fix in
  `src/ac4enc`'s A-SPX analysis.

**Exit:** on the sweep legs ViSQOL within 0.035 of DEE's or over it at every layout and rate where the
gap was measured, and no other pinned leg regressing beyond its own tolerance; a synthetic sweep through
the encoder and the decoder keeps each band above 16.5 kHz within a stated number of dB of the source's
energy.

**Verified by:** `tools/checks/score_ac4_encode.py --gold` over G1's sweeps at 2.0, 5.1 and 5.1.4 and
over the music, film and speech legs as controls; `ac3tests`; the WSL GCC and Clang gates. ViSQOL and DEE
are local only.

**Built** (phase E10): E8's gap is at 5.1.4, with a smaller one at stereo 48 kbps. Scored as `score_ac4_encode.py --gold` scores it (ViSQOL
of the channels' mean over the middle four seconds), G1's sweeps stood, this encoder's less DEE's: at 2.0,
from 0.00 to 0.10 over from 64 to 144 kbps and 0.04 under at 48; at 5.1, from 0.03 to 0.21 over from 96
to 320, though its A-SPX tiles were 2 to 11 dB further from the source's energy than DEE's (27.7 dB
against 16.7 at 192 kbps); at 5.1.4, 0.03 over at 192 kbps and 0.145, 0.154, 0.107, 0.071, 0.034 and 0.183
under at 256, 288, 320, 384, 448 and 512, in core decoding 0.02 to 0.04 nearer DEE's; 5.1.4's music leg at
256 kbps, the control, 0.03 over. E8 put the gap above 16.5 kHz. Taking DEE's decoded band from 10.5 kHz
up in place of this encoder's, at 256 kbps, gives DEE's ViSQOL (4.262 against DEE's 4.260 and this
encoder's 4.115), and from 16.5 kHz up alone 4.174, two fifths of the way: the gap is the whole A-SPX
band, three fifths of it below 16.5 kHz. What the decoder does with a group explains it (Pseudocodes 94
and 95): its noise is Q / (1 + Q) of the envelope whatever the patch holds, and its patch gain divides by
1 plus the patch's own energy, so a patch with nothing in it delivers nothing of the envelope. A sweep
above the crossover has nothing in the low band to copy. The two streams' A-SPX configurations are the
same (start, stop and master scale, noise groups, interpolation, pre-flattening, limiter), so are the
framing class (FIXFIX), the high frequency resolution of every envelope, the quantisation step (1.5 dB in
a frame of one envelope, 3 dB in two) and the delta direction (along time but in I-frames), and frame by
frame on the 5.1.4 sweep at 256 kbps (`tools/references/ac4_syntax.py`'s reader, on DEE's streams as
output only) what differs is these: DEE's noise floors are `qscf_noise` 7 to 17, 2^-1 to 2^-11, and this
encoder's 29, the least, in 95 to 99 % of its values (on music DEE's are 7 in nine of ten, this
encoder's 29 in 80 to 96 %); DEE inverse-filters at mode 0 in 96 to 98 % of the core channels' values,
this encoder at 0 or 3; DEE frames one frame in ten as two envelopes (one in five in the top pairs); and
this encoder adds a sinusoid to a group in one frame in twenty at most. Decoded, the tone above 16.5
kHz lands 15 to 17 dB under the source's energy in DEE's stream, which is its noise floor's share (2^-5 of
the envelope in the tone's group is -15 dB), and 32 to 61 dB under it in this encoder's at 5.1 and 48 to
61 at 5.1.4, in every channel A-SPX codes.

`AspxChannelEncoder::fill_undelivered` sends the floor the patch needs. Per noise group and interval it
measures the share of the input's energy that the decoder's generator, run on the input's low band at
the inverse filtering chosen, delivers (est / (1 + est) of each subband, a subband with a sinusoid or
coded by the spectral frontend counting as delivered whole), and where that is under three quarters
sends the floor at which (share + Q) / (1 + Q) reaches three quarters, when it is louder than the one
the tonality rule chose: `qscf_noise` 4 for an empty patch. Music, film, speech, noise, transients and
tones at 2.0 from 48 to 144 kbps, 5.1 from 96 to 320 and 5.1.4 from 192 to 512 encode to the same bytes
as before (91 streams); only the sweeps change, and the SIMPLE and SCPL rates have no A-SPX. The test
(`tests/ac4enc/test_ac4enc_encoder.cpp`) encodes a sweep from 11 to 21 kHz in one channel of a stereo and
a 5.1.4 stream and holds the energy of each band from 16.5 to 20.5 kHz to 6 dB of the source's: before,
seven of the eight came back 8 to 13 dB under it, and after all eight 2 to 4 dB under. Against DEE's
streams ViSQOL is now over DEE's on every sweep leg: 0.15 to 0.28 at 2.0, 0.05 to 0.48 at 5.1 and 0.13 to
0.66 at 5.1.4 (core decoding 0.05 to 0.65), the A-SPX tiles 3.5 to 5.7 dB nearer the source's energy than
DEE's at 5.1.4 and 4 to 7 at 2.0 and 5.1. What it costs is log-spectral distance, which the noise
raises by 0.2 to 0.9 dB on sweeps, to 0.55, 0.79 and 1.14 dB over DEE's at 2.0 and 48, 64 and 96 kbps
and 0.55 to 2.7 dB under it everywhere else. The pins moved for that reason alone: the 5.1.4 sweeps' LSD
ceilings up 0.4 to 0.7 dB, their ViSQOL floors up 0.17 to 0.63, their tile ceilings down by 7 to 19 dB, and
their SNR floors unchanged but for 0.1 dB in a channel or two at 256 to 320 kbps; the 2.0 and 5.1 sweeps
and the three A-CPL ones are pinned for the first time. `src/ac4enc/ERRATA.md` records the reading. Not
done: `choose_sinusoids` holds a group's tone to twice the group's mean energy, which no group of two
subbands can show, so sinusoids reach only the three-subband groups above 16.5 kHz and the single
subbands; changing it would change music's streams, and E10 leaves it.

### Application phases

`ac3cli` grows with the library: D2 and D8 give it AC-4 decoding and media information, and E1 and
E7 give it AC-4 encoding, because the scripts that check each phase drive it. The phases below are
the rest of each application. They start when the channel-based library is complete (D8 and E7);
immersive and object content follows in I5
([decision 20](#decisions-for-the-encoder-and-the-applications)). Each updates the support catalogue
(`docs/assets/data/support-catalogue.json`, which `tools/checks/generate_support_matrices.py` turns
into the snippets under `docs-snippets/generated/`, one of which `docs/library/application-coverage.md`
includes).

#### I1: the rest of `ac3cli`

**Status:** merged as #1070 on 2026-09-26. Exit met; `record`, `live` and `monitor` were not run
against a real device (the software-ALSA tests run on Linux CI).

- `transcode` between AC-4 and AC-3 or E-AC-3 in both directions, through PCM, carrying what maps:
  dialnorm, the downmix levels, the DRC profile Part 1 5.7.9.4 names for a transcoder, and a
  presentation to a programme.
- `monitor` and `play` decode AC-4 live to `MonitorSink`.
- `qc`, `levels` and `loudness` read AC-4, so a stream's measured loudness is checked against its
  dialnorm as E-AC-3's is.
- `fmp4`, with its HLS and DASH output, takes the encoder's output, under CMAF's rules; `mkv`
  refuses AC-4, since Matroska registers no codec ID for it; `probe` reads AC-4 inside MP4 and TS as
  well as raw.
- `spdif` and `unspdif` for AC-4, from D11; `record` and `live` encode AC-4 with `codec=ac4`.
- `ac3::plan::Codec` gains AC-4, and every helper that decides by codec becomes a switch that
  refuses a codec it does not know: they were two-way ternaries, under which a third codec read as
  E-AC-3 (`src/ac3/include/iclforge/ac3/encoder/plan.hpp`). The help topics' bitmask, which was full
  (`apps/cli/usage.hpp`), is widened.

- Built: `transcode` decodes an AC-4 presentation as coded, since 5.7.9.4 asks a transcoder for no
  DRC, and hands the AC-3 or E-AC-3 encoder the profile the stream names; 5.7.9.4 calls the field
  `drc_eac3_transcode_curve`, which Part 1 does not have, and `src/ac4dec/ERRATA.md` reads it as
  `drc_eac3_profile`. The downmix values map by linear coefficient, AC-4's half-dB LFE steps going
  half a dB up to E-AC-3's whole dB and back down the other way, so the two directions undo each
  other; a 7.X presentation keeps its pair in E-AC-3 at Table E2.5's locations and folds for AC-3
  by Table 219. `record` and `live` share one `TakeEncoder` with `transcode`; `RecordingSink`
  carries AC-4 in every container but Matroska, which has no AC-4 codec ID (FFmpeg 8.0.1 cannot
  mux one either). Checked on 2026-09-29: the Matroska registry still lists no AC-4 ID. The
  request ([issue 176](https://github.com/ietf-wg-cellar/matroska-specification/issues/176) of
  the specification's repository, opened in 2017) and a proposal for `A_AC4`
  ([pull request 874](https://github.com/ietf-wg-cellar/matroska-specification/pull/874), aimed
  at the v5 document, last changed in March 2025) are open. The refusal stays until the registry
  has an ID and a mapping. `record` no longer writes the frames of its bitstream check after the
  rest of the take. `play` decodes AC-4 to PCM, since no receiver found takes it over IEC 61937, and
  `live` sends a receiver the 5.1 AC-3 leg. `fmp4`'s CMAF track takes the readings
  `src/ac4enc/ERRATA.md` records under "Manifests and CMAF tracks".

**Exit:** every new option has a test; the codec matrix covers every AC-4 command, as
`tools/checks/check_matrix_coverage.py` requires; the man page and completions list them.

**Verified by:** `ac3tests`, the CLI tests and the codec matrix on every leg.

#### I2: Hearth desktop

**Status:** merged as #1068 on 2026-09-26. Exit met.

- An AC-4 decoder in the engine's `StreamDecoder` shape (`apps/hearth/engine/stream_decoder.hpp`),
  rendering onto `render::OutputLayout`, and `Session::open` accepting AC-4
  (`apps/hearth/engine/ac4_stream.hpp`). An item's units are its sync frames, each as long as Part 2
  Table 47 makes it for the frame's place in the `sequence_counter` cycle, so a queue's sample
  counts stay exact at the 1000/1001 rates. The decoder runs through `ac4::Decoder`'s public API
  alone, 256 samples at a time; what it holds back is flushed before a frame that waits for an
  I-frame, an error and an item's end, so each unit puts out its own length. A seek starts the
  decoder at an I-frame at least 6 144 samples before the point asked for, and what plays from that
  point equals an unbroken decode. A stream none of whose presentations the decoder decodes is
  refused when it opens, with the decoder's reason.
- `DecoderSettings` gains AC-4's controls: the presentation, by id or place, and the listener's
  language; the DRC decoder mode and the output level, with normalisation, shown apart from
  E-AC-3's ([decision 12](#decisions)); dialogue enhancement; the dialogue level; audio description
  and its level; and a fold by the stream's preferred downmix. The stereo fold, the LFE and
  concealment are E-AC-3's own settings ("One control for both formats"); the LFE is unset until
  the listener sets it, which means off for E-AC-3 and on for AC-4. A change reaches the playing
  item at its next frame in the same decoder. `DecoderAc4.qml`'s cards are live and its banner has
  gone, the Decoder tab follows the playing item's format, and the Media page shows the decoder's
  media information.
- AC-4 decodes to PCM for every output. A network group is also sent the stream as D11's bursts,
  of the type its largest frame needs and timed from the session's units, which the members on the
  extension role that list `"ac4"` take; members on player@v1 get the decoded PCM, as with any item.
  A presentation other than the one a sink chooses with no preferences is decoded here, for every
  member.
- I2 found three things. A sink on the extension role that does not list `"ac4"`, which is every
  board until I6, is sent nothing for an AC-4 item; the pull request gives the options. The page
  cannot ask for Pro Logic II, since the decoder takes Lt/Rt's Pro Logic II form only where the
  stream prefers it (Table 150), so the design's third downmix segment became "Follow the stream's
  preferred downmix". And a group item whose burst type differed from what the group carried, AC-3
  after E-AC-3 as well as AC-4 after either, was sent on the first item's stream start; the group
  now starts again.

Decided on 2026-09-29 about what I2's pull request recorded. A sink on the extension role that
does not list `"ac4"` is sent nothing for an AC-4 item until a part decodes AC-4 in a group (D14b
put the decoder on the P4, whose sink does not yet list `"ac4"`); I6 settles what such a sink is
sent. The decoder's latency, 1 313 samples at index 13, is not
trimmed at an item's start, so two AC-4 items in a queue have a gap of about 27 ms between them;
I6 trims it. The page's downmix control stays as it is, following the stream's preferred
downmix; Pro Logic II is a follow-up after I6.

**Exit:** the engine plays every committed AC-4 stream through the decoder's public API; each
control, driven from the page, changes the decoded output as its formula says, measured with tones;
the UI tests pass.

**Verified by:** the `[hearth]` tests; the QML tests (`tst_decoder_ac4.qml` measures each control's
effect tone by tone at the fake device); the gain scripts through the engine, with
`ac3hearth-render` playing an item through the engine into a WAV file for
`gain_ac4_decode.py --engine`, in the Hearth CI job.

#### I3: Forge GUI

**Status:** merged as #1084 on 2026-09-29. Exit met.

- AC-4 in the codec list, which mapped index 1 to E-AC-3 and every other index to AC-3
  (`apps/gui/encoder_controller.cpp`), with its encode options and the command line the page echoes.
- AC-4 decode in the QC, object and stream player controllers, which dispatched on `stream_bsid`.
- I3 built both. The page encodes one source in its own layout, mono to 5.1, to a raw stream or an
  MP4 file, as `ac4-encode` takes a WAV file; its AC-4 tab carries the frame rate, the rate and
  codec modes, the I-frame interval, the CRC, dialnorm, the loudness values, the DRC profile, the
  stereo downmix and dialogue enhancement, and the page echoes one `ac3cli ac4-encode` command.
  The steps that decide the bytes (the input's channel order, the BS.1770 measurement behind
  `dialnorm=auto` and `loudness=`, and the raw or MP4 packaging) moved from `ac4-encode` to
  `apps/common/ac4_encode_core.hpp`, with `ac4_channels.hpp` and `is_ac4_stream`, so the page and
  the command run the same code. QC, the player and the object page recognise AC-4 by its sync
  word; QC and the player take a presentation by position as `presentation=` does, and the object
  page shows the presentations, beds and objects the decoder reports, read-only, saying in its
  own text that exporting AC-4 objects is I5's.
- I3 left to the command line what one source in one layout cannot say: several substreams and
  presentations, dialogue stems and hybrid dialogue enhancement, a DRC profile per decoder mode,
  Lt/Rt's own levels, the LFE mix, the downmix corrections, I-frames at named frames, and the
  3.0, 7.X and immersive layouts. A live session under AC-4 is refused: the page's live path takes
  AC-3 and E-AC-3, though `ac3cli live` takes `codec=ac4` for a channel session (I1).

**Exit:** each control has a test, and the command line the page echoes, run through `ac3cli`,
writes the same bytes as the page.

**Verified by:** the GUI tests.

#### I4: the C API, Python, Rust and WebAssembly

**Status:** merged as #1094 on 2026-09-29. Exit met; the WebAssembly module's C++ side and the
Android NDK build were not built on the machine that made it, and are checked by CI.

- The C API gains `ac3forge_ac4_*` decoder and encoder functions, with their own status range,
  embedding the AC-4 libraries as it embeds `ac3::forge`
  ([decision 21](#decisions-for-the-encoder-and-the-applications)); the Rust `-sys` crate's
  allowlist picks them up, and the safe crate wraps them.
- Python gains an `ac4` submodule in the present-or-absent pattern its optional modules use.
- WebAssembly gains an AC-4 module beside its decode and encode modules, and the JavaScript package
  a wrapper for it.
- The configurations D8 stopped compiling the AC-4 libraries now link them.

- Built: the header's new section adds `ac3forge_ac4_decoder_t`/`ac3forge_ac4_encoder_t` behind
  the existing opaque-handle and `_config_init()` conventions, two new status ranges
  (`AC3FORGE_ERROR_AC4_DECODE_*` at 60–64, `AC3FORGE_ERROR_AC4_ENCODE_*` at 80–81) behind a new
  `AC3FORGE_HAS_AC4` compile-time guard (`ac3forge_c/version.h`, `#cmakedefine`'d from
  `AC3FORGE_BUILD_AC4`); `src/capi/CMakeLists.txt`'s
  `forge_c_objects`/`forge_c_static`/`forge_c_shared` targets now embed `ac4::decoder_static`/
  `ac4::encoder_static` the same way they already embedded `ac3::forge_static`, gated on the same
  option, so a package built with both the `capi` and `ac4` features/options carries the AC-4 C API
  surface with no further wiring.
- The Rust safe crate's `ac3forge::ac4` module covers `Decoder`/`Encoder`, every config and
  decoded-frame type, `Toc` and `sync_frame` — the same "core config" cut the C API itself took.
  It carries no Cargo feature of its own, unconditionally available once `-sys`'s bindgen output
  has the symbols, the same footing `atmos` already stood on; every `AC3FORGE_ERROR_AC4_*` status
  gets its own `Error` variant rather than folding into `Other`.
- Python's `ac4` submodule binds `ac4::Decoder`/`ac4::Encoder` pybind11-direct, the same subset the
  C API and Rust took; every AC-4 failure raises a plain `ValueError` (`ac4::describe()` of the
  underlying error), not the `Ac3EncodeError`/`Ac3DecodeError` hierarchy the rest of the package
  uses — a deliberate difference from the AC-3/E-AC-3 bindings, recorded rather than silently
  inconsistent.
- WebAssembly's `ac3forge_wasm_ac4` (`apps/wasm/ac4_bindings.cpp`) is one combined decode-and-encode
  Embind module, unlike the AC-3 side's decode/encode split — AC-4's decoder and encoder share one
  table-of-contents/framing library regardless, so a second executable had less to gain here.
  `js/src/ac4.ts` is a plain ES module wrapping `Ac4Decoder`/`Ac4Encoder` directly, not a Worker
  wrapper like `decoder-worker.ts`'s realtime pipeline: nothing about that protocol's shape (built
  for one decode-only class with a channels-vs-fold output choice) fits a module that covers both
  decode and encode with a wider decoder surface. There is no `ac4` directory under `apps/wasm` and
  no demo page — optional polish this phase left to a later pass — so the compiled module lands in
  its own output directory (`bin/wasm_ac4_demo`) with nothing to serve it yet; `js/src/ac4.ts` compiles
  into `js/dist/ac4.js`, which I4b added to `package.json`'s `exports` map.
- Android's CMake wrapper (`apps/android/app/src/main/cpp/CMakeLists.txt`) no longer forces
  `AC3FORGE_BUILD_AC4` off: the libraries depend on nothing outside this tree and cross-compile
  cleanly under the NDK, unlike the third-party dependencies (MbedTLS, httplib, FLAC, Opus, mdns)
  that keep Hearth off this build, so D8's "not linked, don't compile" reasoning had nothing left
  to justify leaving AC-4 off here too. Nothing in the app's own `target_link_libraries` links
  `ac4::` yet — giving the Shield app an AC-4 feature is later
  application work, not this phase's. `tools/checks/test_ac4_build_configurations.py` was rewritten
  for this shape: Android, WebAssembly and the Python wheel all build AC-4 now; WebAssembly and the
  wheel also link `ac4::` targets, Android does not yet; the ESP32 minimal-decoder/minimal-encoder
  presets are untouched, still off (D14's territory).
- Every binding covers channel-based and channel-based-immersive content only (mono, stereo, 5.0,
  5.1, 5.0.4, 5.1.4) — the encoder's own scope as of this phase, matching the C++ encoder itself;
  each decoder's object accessors read whatever object audio a stream carries regardless,
  so a stream encoded elsewhere with objects decodes through every one of these bindings even
  though this project's own encoder could not yet produce one to round-trip end to end (E9's
  A-JOC and direct-coded objects reached the bindings in I4b).

**Exit:** each binding's tests decode a committed stream and encode one that the decoder reads back;
the package checks pass. Met: the C API's `tests/capi/test_capi.cpp` adds 8 Catch2 test cases (600
assertions, MSVC-built and passing); Rust's `tests/ac4_roundtrip.rs` adds 6 integration tests,
passing alongside the crate's full existing suite with no regressions, `clippy -D warnings` clean;
Python's `test_ac4_roundtrip.py` adds 6 round-trip tests, passing alongside the full 115-test
`python/tests/` suite in an isolated venv, `ruff`-clean and `stubtest`-clean against the hand-written
`ac4` stubs; WebAssembly's `js/tests/ac4.test.js` passes under Node against a fake Embind module —
the C++ Embind side itself is unverified locally, since Emscripten is not installed in this
environment, and stays CI-only until `build-wasm` confirms it.

**Verified by:** the binding tests on their CI legs.

Left for later phases when I4 merged: E9's objects needed matching encoder-side C API and binding
work, since every binding here followed the C++ encoder's own scope (phase I4b, below); and the
WebAssembly module needs a real Emscripten build, in CI or otherwise, to confirm the C++ Embind
side beyond what `js/tests/ac4.test.js`'s fake-module harness can reach. The first was I4b's; the
second stands.

#### I4b: the object encoder in the C API, Python, Rust and WebAssembly

**Status:** merged as #1119 on 2026-09-29. Exit met; leftover (e), the WebAssembly demo page, is
not done.

After I4 and E9. I4 bound the encoder as E7 left it, for channel-based content, and its closing
paragraph left E9's objects to a later phase; I5 then drove them from the command line.

- The C API, Rust, Python and WebAssembly encoders take E9's object substream: the objects and
  their metadata, A-JOC or direct-coded, and the metadata updates given with the input.
- I4's leftovers: `js/src/ac4.ts` in the package's `exports`, typed exceptions for AC-4 in Python,
  the encoder configuration widened to the fields that cost one field and one test, and the
  decoder's update ramps in every binding.

Built: `ac3forge_ac4_encoder_config_t` takes `objects`, a pointer to
`ac3forge_ac4_objects_config_t` (an array of `ac3forge_ac4_object_config_t` and E9's `ObjectsConfig`
fields: the coding, the A-JOC downmix and its signal count, decorrelation, the parameter bands and
their quantisation, the common data), beside the fields I4 bound. An object is a bed object, a
dynamic object or the LFE, and its metadata is the `ac3forge_ac4_object_properties_t` the decoder's
accessor already returned, with an `_init()` because a zeroed struct has a depth exponent no code
holds and the encoder refuses it. `ac3forge_ac4_encoder_encode_objects()` takes the metadata
updates (`ac3forge_ac4_object_metadata_update_t`: object, input sample, ramp, properties) with the
input, as `Encoder::encode()`'s overload does, and `experimental.objects` is a field the caller
sets, as in C++, since no reader outside the project has read the object substream from this
encoder. The frame-rate constraint and the counts' limits are the encoder's, named by the new
`ac3forge_ac4_encoder_refusal_reason()` (`Encoder::refusal_reason()` for a C caller, which the first
cut left out) and pinned by `AC3FORGE_AC4_MAX_OBJECTS` and `AC3FORGE_AC4_MAX_DOWNMIX_SIGNALS`, whose
tests build an encoder of that many and refuse one more. Eight functions were added, 76 in the
header's AC-4 section, each with a stub in `ac4_absent.cpp`; no status code was needed. Each
scalar field of `ac4::EncoderConfig` that costs one field and one test joined the config: `iframes`
and `fragment_starts` (a pointer and a count), and `experimental`'s `aspx_balance`, `aspx_varvar`,
`aspx_interleave`, `coding_configs`, `seven_x`, `acpl`, `back_pair` and `ajcc`. Left out, because
each needs a nested group or the substream list: `loudness`, `drc`, `downmix`, `dialogue`,
`substreams`, `presentations`, the EMDF payloads, the trace, `encode()`'s dialogue-stem overload,
and the `drc_gains` and `three_zero` flags. The C++ `DecodedObject` carries its `updates`, so every
binding returns them: `ac3forge_ac4_decoded_frame_object_update_count()` and `_update()`, `updates`
on Rust's and Python's `DecodedObject`, and on JavaScript's objects, which now carry every field
of the properties (the first cut returned four).

Rust wraps the same as `ObjectsConfig`, `ObjectConfig`, `Experimental`, `ObjectMetadataUpdate`,
`Encoder::encode_objects()` and `Encoder::refusal_reason()`; `EncoderConfig` owns vectors now, so
it is `Clone` and no longer `Copy`, and `ObjectProperties` has a `Default` that calls `_init()`.
Python binds `ObjectsConfig`, `ObjectConfig`, `ObjectMetadataUpdate` and `Experimental` on the C++
structs, makes `ObjectProperties` settable and constructed from the encoder's defaults, gives
`EncoderConfig` an `objects` view of the one substream a stream can have (the config's
`codec_mode` is then the object substream's, as in C and JavaScript), and takes `updates=` on
`Encoder.encode()`. `Ac4Error` derives from `ValueError`, since I4 raised `ValueError` and code
that caught it should keep catching AC-4's failures, with `Ac4DecodeError` and `Ac4EncodeError`
under it, each carrying the C++ enumerator as `.error`; the three are exported and stubbed, and
`stubtest` checks them. WebAssembly's `Ac4Encoder` takes its configuration as one JS object, since
eight positional arguments cannot hold a list of objects; a field it leaves out keeps the C++
default, so `ac4.ts` no longer repeats them, an enumerator the C++ header does not define is
refused at construction, and `constructionError()` says why a configuration made no encoder.
`./ac4` is in `package.json`'s `exports`; `js/tests/package-exports.test.js` holds the map to the
files the build writes, imports the subpath through the package's own name and finds the
wrapper's exports in its declarations, which `npm run build` type-checks in the package's strict
settings.

Found: the decoder lists the objects in its own order, not the encoder's (the LFE first, then the
bed objects, then the dynamic objects, each group in the order the configuration lists it), which
the header and each binding's documentation now state. An inactive object sends none of its
metadata, so the decoder reports it with gain -infinity and priority 0. E9's `object_codes()`
wrote the screen factor and the depth exponent as one group of fields whose factor has no code for
0, so an object with a depth exponent other than 1 and a screen factor of 0 decoded with a factor of
1/8; the tests gave such an object a factor, and the encoder now refuses it, naming the reason
(`src/ac4enc/ERRATA.md`, "The screen factor and the depth exponent"). Python binds the C++
structs, so its streams are the C++ encoder's by construction: its test holds two ways of
configuring the same scene to the same bytes, and the decoder's read-back holds each field. A C
caller can store any int in an enumeration-typed field, and reading a value the enumeration does not
name is undefined in C++ (Clang's `-fsanitize=enum` reports it), so the checks on the object coding,
the downmix, a bed object's channel and the seven-channel pair read the field's bytes
(`stored_value()` in `internal_ac4.hpp`), and the tests that hand them such values write the bytes.
The AC-4 entry points' argument and error arms, and the accessors the round trips do not reach, are
held by `tests/capi/test_capi_ac4_arguments.cpp` against the C++ decoder and encoder they wrap.
`src/capi`'s branch coverage is 81.1% with them, against a floor of 74% that main's 71.1% had missed.

**Exit:** in each of the C API, Rust, Python and JavaScript, a test encodes a small object scene,
A-JOC and direct-coded, that the same binding's decoder reads back with the objects' positions and
gains within the codec's tolerance, and whose bytes equal what the C++ encoder writes for the same
scene; leftovers (a) to (d) done or listed with a reason. Met: the C API's `test_capi.cpp` encodes
an A-JOC scene and a direct-coded one through the C API and through `ac4::Encoder`, and the two
streams are the same bytes; it decodes the C API's stream and reads every object back within what
each field's code can hold (X and Y to half a step of 1/62, the gain to 0.5 dB, the widths to half
a step of 1/31), with its own
tone (correlation above 0.98, and below 0.5 against another object's), and the moved object's
update at its input sample plus the two delays to within 32 samples, with its 1 024-sample ramp.
Rust's `ac4_objects.rs` reads the same scenes back from the crate's stream, which equals the raw C
API's from structs built by hand; Python's `test_ac4_objects.py` from a stream that two ways of
configuring the scene write identically; JavaScript's `ac4.test.js` from the fake Embind module's
loopback codec model, the C++ side being `build-wasm`'s. Leftovers (a) to (d) are done; (e), the
demo page, is not.

**Verified by:** the binding tests on their CI legs and locally where the tool exists (`ac3tests`,
`cargo test`, `pytest` and `stubtest`, `npm test`), `cargo fmt --check` and `cargo clippy -D
warnings`, `ruff`, `tools/checks/test_ac4_build_configurations.py`, the WSL GCC and Clang gates, the
C API's cases under AddressSanitizer, UBSan and LeakSanitizer, and the whole of `ac3tests` once.

Not done: the WebAssembly demo page; a build of `ac4_bindings.cpp` with Emscripten, which is
`build-wasm`'s to do in CI (it was compiled and run natively against a host model of
`emscripten::val`, a scratch harness kept out of the tree, and both scenes' bytes equal
`ac4::Encoder`'s); the configuration groups and flags listed above.

#### I5: immersive and object content in the applications

**Status:** merged as #1100 on 2026-09-29. Exit met, apart from two failures in suites it did not
touch, which its pull request named: a Hearth diagnostics test, since fixed, and the Qt teardown
crash of the GUI's AC-4 decode suite, fixed in #1109.

After D9, E8, D10 and E9.

- `ac3cli`: encoding 5.1.4 and 7.1.4, and objects from ADM BWF and IAB as `atmos-adm` and
  `atmos-iab` do for E-AC-3; decoding objects to an ADM BWF master as `decode` does for E-AC-3 JOC;
  object metadata in `probe`.
- Hearth: immersive layouts through its renderer, objects through the renderer its phase A1 moved,
  and full or core decoding under its objects control.
- Forge GUI: AC-4 in its object pages.

**Exit and verified by:** as I1 to I3, for this content.

**Built (phase I5):** `atmos-adm` and `atmos-iab` take `codec=ac4`: every bed/object channel the ADM
or IAB source resolves becomes an AC-4 dynamic object (A-JOC by default, `coding=direct` for
direct-coded object substreams), its position sampled once a frame - the object substream is
frame_rate_index 13 only (ac4enc/encoder.hpp) - and fed to E9's own writer; the ADM/IAB readers
themselves needed no change. 5.1.4 and 7.1.4 channel-based-immersive encoding already existed by E8
(`ac4-encode` takes any channel count E8 writes); this phase's own new coverage is the objects path
and the matrix legs exercising 5.1.4 and 7.1.4 through the rest of `ac3cli`. `decode`'s `objects_dir`
and `adm_out` now read D10's own decoded objects the way they already read E-AC-3 JOC's:
`objects_dir` streams each object's PCM to its own WAV, and `adm_out` (needs
`-DAC3FORGE_BUILD_ADM=ON`) accumulates every bed and dynamic object's decoded Annex F properties
into the same ADM BWF writer E-AC-3's own IM2 item built, through a new conversion onto
`ac3::oba::DynamicObject` (position and gain carry over directly, TS 103 190-2 Annex F and TS 103
420 §5.6.1 sharing one room and one dB convention). `probe`'s JSON gains an `oamd_common_data`
object on an A-JOC substream's own entry, additive, the schema unchanged.

The round trip the exit criterion names - an ADM master from the fixture `tests/cli/
test_cli_atmos_adm.cpp` already commits (two bed channels and one dynamic object jumping position at
a known time), encoded to AC-4 by `atmos-adm`, decoded with `objects_dir` and `adm_out`, and the
written master re-parsed through `ac3adm`/`ac3::admbridge` - matches the original's own automation,
object for object (matched by which tone each carries, not by index), to within AC-4's own
quantization once the encoder's and the decoder's combined delay (reported on `atmos-adm`'s own
status line, and pinned in the test rather than hardcoded) is accounted for: position within 0.06 in
each axis, gain within 2 dB, and the moving object's jump is still there rather than the whole
reading being flat (`tests/cli/test_cli_atmos_adm_ac4.cpp`).

Hearth's engine now reads a whole AC-4 frame through `ac4::Decoder::decode()` in place of
`decode_by_block()`, so a presentation with objects renders through `Ac4ObjectRenderer` (the same
class `ac3cli decode` plays them with) beside its channels, delivered a 256-sample block at a time as
before; the only rate an object substream can carry (frame_rate_index 13, 2 048 samples, an exact
multiple of 256) makes this change in delivery mechanism invisible to every existing, committed
channel-only stream - confirmed by extending the existing full-committed-stream comparison test's own
independently-computed reference to render objects too, rather than dropping them as it silently did
before. `DecoderSettings::Ac4Settings` gains `immersive_layout` (the same six layouts `decode`'s
`speakers=` takes, reached once the configured output layout does not itself ask for a stereo or mono
fold - that always wins) and `core_decoding`, both reachable from a new "Immersive and objects" card
on `DecoderAc4.qml` and `HearthController`'s JSON bridge. The support catalogue's Hearth row for AC-4
decode no longer says "objects not yet".

Forge GUI's `StreamPlayerController` now fills `has_objects`/`object_count`/`object_audio` for an
AC-4 stream from D10's own `DecodedFrame::objects`, the way it already does for E-AC-3 JOC - the
existing "Export objects…" button and its one-WAV-a-decoded-object export function are already
codec-agnostic and needed no change. Doing so found a bug that predates this phase: the same
function's `order.empty()` doubled as its "has the first frame been read" flag, and a presentation
of objects alone has no channels or speakers to make `order` non-empty with, so it reported "no
frame decoded" for every such stream despite decoding it correctly; fixed with an explicit flag,
the pattern `ObjectDecodeController::measure_ac4_objects()` already used. The object inspector's
own read-only listing already covered AC-4 before this phase. The encoder page's Atmos/object
authoring UI was E-AC-3-only when this phase merged (`EncoderController::setAtmosEnabled` forced
`codec_` away from `kAc4`); [I5b](#i5b-the-encoder-pages-ac-4-objects) gave it an AC-4 path.

Checks: `tests/cli/test_cli_atmos_adm_ac4.cpp` (new), the extended `tests/cli/test_cli_ac4_decode.cpp`,
`tests/hearth/test_ac4_engine.cpp` and `tests/hearth/test_decoder_settings.cpp` (new cases), the
extended `tests/hearth/test_diagnostics.cpp`, `apps/gui/tests/qml/tst_e2e_inspect.qml` (new case),
`tools/ci/run_codec_matrix.sh`'s new AC-4 legs (5.1.4, objects both codings, both Atmos-ingest
commands' `codec=ac4`), and the whole of `ac3tests` once, at the end (a full run's own numbers are in
the phase's report rather than repeated here, since a later merge would make them stale immediately).
Not done: `zone_mask`'s mapping onto
`ac3::oba::ZoneConstraint` is a reading, not independently checked against the spec text (neither
library's syntax, so not an ERRATA entry). A direct-coded group's own separate `oamd_substream`,
which this phase left out of `probe`'s JSON, landed afterwards: `ac4::SubstreamReport` holds the
substream's `oamd_common_data()`, and `probe` writes the first one as `oamd_common_data` on the
group's `oamd` member, in the shape of the A-JOC substream's. The GUI's encoder-page AC-4 object
path, which this list named when the phase merged, landed as I5b, below.

#### I5b: the encoder page's AC-4 objects

**Status:** merged as #1117 on 2026-09-29. Exit met, with the ADM master's audio as the page's
source and its scene authored on the page, which has no ADM or IAB reader.

After I3 and I5. I5 left the encoder page's Atmos and object authoring to E-AC-3 and gave the
options in its report; the user asked for the page to author AC-4 objects.

- The page's Atmos switch and its object pages work with AC-4: what the page offers for E-AC-3 it
  offers for AC-4, A-JOC by default and a control for direct-coded object substreams, to a raw
  stream or an MP4 file. The limits of E9's writer (frame_rate_index 13 alone, 64 objects at most,
  one of them the LFE) are in the page's own text, and what cannot apply is named and, where a
  control can be, disabled, before Encode is pressed.
- The page echoes one `ac3cli` command that writes the same bytes. The object steps both need move
  into `apps/common`; the command line's behaviour and its tests stay as they are.

**Exit:** each new control has a test; the command line the page echoes, run through `ac3cli`,
writes the same bytes as the page; an ADM master (the fixture I5's round trip commits: two bed
channels and one dynamic object that jumps position) authored on the page as AC-4 decodes in the
GUI's own object decoding with its objects at their places and gains, within I5's tolerances (0.06
in each axis, 2 dB).

**Verified by:** the GUI's Qt Quick Tests and C++ tests, and the CLI matrix.

**Built (phase I5b):** `apps/common/ac4_objects_core.hpp` holds the steps `ac3cli` and the page
both run: `ObjectSlot` and `object_slots_from_assignment` (moved from `apps/cli/support.hpp`),
`location_azimuth_deg` (moved from the GUI's `channel_geometry.cpp`, which forwards to it), the
order of a stream's objects (each `obj` row, each `objm` group, each channel assigned to a speaker,
then the LFE), the audio each carries, one metadata update per object per 2 048-sample frame,
ramped over the frame and taken at its end, and the call into E9's writer. `atmos-adm` and
`atmos-iab` call it for `codec=ac4` and write the bytes they wrote. `atmos-encode` takes
`codec=ac4` beside E-AC-3: `src=`, `map=`, `offset=` and a scene file as for E-AC-3, `coding=direct`,
`dialnorm=` and, for a raw stream, `crc=off`, with an output named `.mp4`, `.m4a` or `.mov` written as
an MP4 file. `coding=` and `crc=` without `codec=ac4` are refused rather than dropped, and the
E-AC-3 command and its tests are as they were.

A channel assigned to a speaker is a dynamic object held at that speaker's place on the ring ADM's
polar coordinates give a bed channel (radius 0.5 about the room's centre), at unity, as
`atmos-adm codec=ac4` writes an ADM bed channel; one assigned to an LFE is the stream's LFE
object, and the LFE send has no AC-4 counterpart. An object with no path keeps the inverse-root
gain E-AC-3 gives it, which AC-4 codes in whole dB. Sources shorter than the longest are silent
past their end, where E-AC-3's `src=` holds the last sample.

On the page, `EncoderController::setAtmosEnabled` keeps AC-4 as the codec, the codec list greys
out AC-3 in object mode, and the AC-4 tab keeps its place beside the Objects tab. It carries what
an object stream takes: A-JOC or direct coding, a dialnorm in whole dB, and the CRC; the frame
rate and the rate mode show as fixed, and the loudness values, DRC, downmix and dialogue
enhancement, which describe channels, are off. The Objects tab gives the writer's limits under
its header and counts against 64. A request the page can tell is refused (a container other than
a raw stream or an MP4 file, more than 64 objects, a second LFE channel or no object but the LFE,
a measured or out-of-range dialnorm, sources resampled to one rate) is named there and at the top
of the AC-4 tab, in the words Encode refuses it with, before a run opens; a key outside +15 to
-49 dB or outside the room, and a bit rate the writer refuses, are refused at Encode. The echoed
command is `ac3cli atmos-encode <source> out.ac4 <kbps> <objects> <name>-paths.json [src= map=
offset=] codec=ac4 [coding=direct] [dialnorm=] [crc=off]`, with `out.mp4` for an MP4 file.
Encoding writes the scene the command reads beside the stream, as `<name>-paths.json`, and
**Export paths...** suggests the same name.

A live session refuses AC-4, since the page's live path takes AC-3 and E-AC-3 (`ac3cli live` takes
`codec=ac4` for a channel session, not for objects); Guided's Movement step writes E-AC-3 objects;
and Preview plays an AC-4 object encode through the E-AC-3 object encoder's bed, the first fifteen
objects, whichever codec is chosen. The page reads audio, with the scene authored on it, and has no
reader for ADM BWF or IAB masters, which `atmos-adm` and `atmos-iab` write to AC-4 with
`codec=ac4`. The exit's ADM master is therefore the one `tests/cli/test_cli_atmos_adm.cpp`
builds, written to `apps/gui/tests/fixtures/adm-two-beds-one-object.wav` with its `axml` and `chna`
chunks as that test builds them. The page reads its audio, and the scene is authored on the page:
the two bed channels assigned to L and R, and the third channel an object at azimuth -110 degrees
for 0.096 s and then dead ahead. The stream the page writes, read by `ObjectDecodeController` as
the object page reads it, has its three objects at those places at unity within 0.06 in each axis
and 2 dB, before the jump and after it (0.0081 and 0.000 dB at worst), and the object's jump is
there.

Checks: `tests/cli/test_cli_atmos_encode_ac4.cpp` (new, 6 Catch2 test cases: the raw and
direct-coded bytes equal the shared core's, `crc=off` and an MP4 file, `src=`, `map=` and
`offset=` with a speaker and an LFE, the default placements, the refusals with their exit codes,
and E-AC-3 unchanged); `tests/gui/test_ac4_objects_core.cpp` (new, 8); `tests/gui/
test_ac4_encode_settings.cpp` (4 new); `apps/gui/tests/qml/tst_ac4_objects.qml` (new, 10 tests, one
for each control and refusal); `tst_e2e_ac4_objects.qml` (new, 3: a raw stream and an MP4 file
equal to the echoed line run through `ac3cli`, and the ADM master); `tst_guided_wizard.qml` (1
new); and, unchanged and passing, the accessibility and channel-count suites, `tst_e2e_objects.qml`,
`tst_objects_per_source.qml` and the localisation pipeline. The whole of `ac3tests` and `ctest -L
gui` ran once at the end; their numbers are in the pull request.

Left for the user, each with its options in the pull request: an ADM or IAB master as a source
on the page; AC-4 bed objects (A-JOC's static bed) for the channels assigned to speakers, in place
of dynamic objects at ring positions; a Preview taken from a decode of the AC-4 stream; AC-4 in
Guided's Movement step. The object inspector's note on export now points to Open stream, where
I5 put the export; it had said the export would arrive in a later release.

#### I6: the ESP32 sinks

**Status:** not built. The P4's decoder is D14b's and plays from `hearth_sink`'s HTTP source; no
sink lists `"ac4"` among its Sendspin data types, and Hearth sends a board nothing for an AC-4 item.

On each part after its D14 figures: the P4 first, then the S3 and the C6.

- The ESP-IDF component carries the inspector, the decoder and the core; `hearth_sink` decodes AC-4
  on the P4 and the S3 in `float` and on the C6 in fixed point, and lists AC-4 among its Sendspin
  data types. On the P4 here, whose I2S has no clock for TDM, it plays to two channels.
- AC-4's frames last from 16 to 43 ms across its frame rates, where the sink's queue and its hold
  of the first unit were sized for E-AC-3's 32 ms.
- Two things I2 left ([I2](#i2-hearth-desktop)): what a sink that does not list `"ac4"` is sent
  for an AC-4 item, and the decoder's latency of 1 313 samples at index 13, which the engine does
  not trim at an item's start. Both are settled here.

**Exit:** D14's board figures, and a sink playing an AC-4 stream Hearth sends it.

**Verified by:** the QEMU probes in CI; the boards.

Crucible takes no AC-4. It captures applications into Atmos objects for receivers, and no receiver
accepts AC-4 ([decision 20](#decisions-for-the-encoder-and-the-applications)).

#### N1: the names

**Status on 2026-10-01:** built in the repository, in one freeze that began with #1160 on 2026-09-30 and
ends with the S5 pull request. The study (#1122, 2026-09-29) moved no source file; the user took its
recommendation on each of its 14 decisions on 2026-09-30 (L2, the root `iclforge`, flat
`src/<library>`), and the stages ran from [its plan](layout.md#f-the-migration-plan), each in a pull
request of its own with its scripts under `tools/n1b/`:

| stage | pull requests | what it did |
|---|---|---|
| S0, S1 | #1153 to #1155, #1158, #1159 | the seven cuts that let five codec-blind libraries leave `forge`, the scripts, and the hand-written part of S2 as a patch |
| S2 | #1160 | `src/` and `tests/` moved to the L2 layout: 404 renames and 22 libraries |
| S3 | #1161 | the namespace root `ac3` became `iclforge` in 1,208 files, and 294 files were wrapped again to 100 columns |
| S4 | #1162 | `ac3forge` became `iclforge` in identifiers, names and wire strings (713 files), `sendspin::ac3forge` became `sendspin::player`, and 211 package files moved |
| N1A | #1164 | `ac3cli`, `ac3gui`, `ac3hearth` and `ac3crucible` became `forge`, `forge-gui`, `hearth` and `crucible`, with what they register |
| S5 | #1165 | the pages, the addresses, the build and tool text and the install routes follow the names; [Renamed](../docs/renamed.md) is the page that puts each old name beside its new one |
| N1D | the pull request that carries this text | the Windows null-sink driver became `IclForgeNullSink` and its endpoint "Crucible Silent Output", on the owner's decision of 2026-10-01 (the driver had never been signed or installed outside the test guest); [the driver's page](../docs/platforms/windows-driver-acx.md#the-rename-2026-10-01) has the record |
| S6 | the pull request that carries this text | the AC-3 codec's C++ names are in `iclforge::ac3`, a peer of `iclforge::ac4`, and the root `iclforge` holds one namespace per library; [the library index](../docs/library/index.md) and [Renamed](../docs/renamed.md) say how, and `tools/n1b/README.md` has the recipe |

[What the runs found](layout.md#what-the-runs-found-that-the-plan-did-not) gives the counts that
differed from the study's and the hazards it did not name. **Left:** the owner renames the
repository, the Homebrew tap and the SonarCloud project key, creates the pending publisher of the
PyPI project `iclforge` and publishes the first release under the new names; and U1 revises
wording that is not a name. What follows is the plan as it was written.

Asked by the user on 2026-09-25, since libraries and programs named `ac3` now do AC-4. A survey of
the tree found `ac3forge` already the family's name for parts with no AC-3 in them (Hearth,
Sendspin, Crucible, the ADM and IAB readers), the recasting plan's decisions of 2026-09-05 keeping
the identifiers until the 1.0 freeze and the program names through 0.x
(`planning/recasting.md`, decision 6 there), and AC-4 named `ac4` on purpose
([decision 15](#decisions-for-the-encoder-and-the-applications)). The man page, the CLI's banner,
the CMake project's description (which reaches pkg-config and the Debian packages), README, the
site's description, the Homebrew formula, the desktop entries and the Windows file-type label said
AC-3 and E-AC-3 alone, and a few pages described `ac4dec::` and `ac4enc::` namespaces the code does
not have. #1076, #1077, #1078 and #1133 corrected most of that. On 2026-09-30 the man page's name
and description lines (`apps/cli/usage.cpp`) and some of the GUI's own strings (#1133 lists them)
still name AC-3 and E-AC-3 alone. The Windows file-type label (`cmake/Packaging.cmake`) names the
two extensions the installer registers, `.ac3` and `.ec3`, and is wrong only once `.ac4` is
registered. The user found a program called `ac3cli` doing AC-4 wrong in itself, and took renaming
the programs ([decision 35](#decisions-of-2026-09-25)), which the recasting plan's scheme S3 costed.

On 2026-09-29 the user extended the ask from the programs to the whole tree: "while this started out
as an AC3 or an EAC3 based application, it now supports AC4 and it's kind of weird to say it's an AC3
XYZW ... Hearth should be Hearth, not AC3 Hearth. Forge should be Forge ... it may do other things in
the future if there was like an AC5 or DTS or whatever", and to where the code sits: "the old stuff is
over here in forge and the new AC4 stuff's over here which is two folders higher, not as a sibling,
it's all kind of weirdly structured". N1 is now two tasks that share one quiet window after the
wave of phase branches has merged (it has), N1B first:

- **N1A** names the programs and what they register with the system (below).
- **N1B** names and lays out the libraries. It is studied in [The layout of `src/`](layout.md), with
  the inventory it reads from in [layout-inventory.md](layout-inventory.md); the recommendation and
  the decisions it puts to the user are there.

**N1A: the programs.**

- The programs take their members' names, which the user chose on 2026-09-26 ("I like option b for
  program names"): `ac3cli` becomes `forge`, `ac3gui` `forge-gui`, `ac3hearth` `hearth` and
  `ac3crucible` `crucible`, and Hearth's test sink and server `hearth-testsink` and
  `hearth-testserver`. `forge` is also the name of Foundry's command and of Laravel Forge's, so on
  a computer with either, the first on the path wins; the documentation says so. The internal
  programs (`ac3tests`, the benchmarks, the probe) keep their names unless the user takes decision
  13 of [the study](layout.md#i-decisions), which recommends renaming them in N1A.
- What the programs register with the system follows their names: the completions for four shells,
  the man page, winget's aliases, the desktop entries and bundle identifiers, the Windows file-type
  command lines, the firewall rules' names and the translation catalogues named after the programs,
  and the version line, which names the family as N1B names it (`ac3forge <version>` today). The
  Homebrew formula and cask take the family's name instead ([decision 41](#decisions-of-2026-09-30)).
  The old plan kept the old names working through a stated period, printing the new name. The
  decisions of 2026-09-29 drop that: the old names are not kept, and no
  launcher is written ([decision 38](#decisions-of-2026-09-29)). The pre-releases and the Homebrew
  tap ship `ac3cli` and `ac3gui` (see N1B below), so someone who installed one keeps those names at
  that version, and a release made after N1 carries the new names only.
- Wording that names the formats the family handles, AC-4 among them, wherever it says AC-3 and
  E-AC-3 alone.

At a point where few phase branches are open, since every branch touches the programs' names.

**Exit (N1A):** the programs build and install under their new names, every test and document uses
the new names, and no page or package description names AC-3 and E-AC-3 as the family's only
formats; the documentation gates pass.

**Verified by (N1A):** `ac3tests` and the CLI tests under the new names; the package checks; a search
for the old names and the stale phrases; `mkdocs build --strict` and
`tools/checks/check_doc_paths.py`.

**N1B: the libraries.**

- The family is named "ICL Forge" (organisation `iainchesworthlabs`; identifiers `iclforge`,
  `ICLFORGE_`) in place of `ac3forge`: plain `forge` is taken on PyPI, npm, crates.io and Homebrew core,
  and `iclforge` and `icl-forge` are free. The programs keep the plain names above.
- The C API prefix, the CMake package, the Kconfig prefix, the environment variables and the wire
  strings are renamed outright, with no shim ([decision 38](#decisions-of-2026-09-29)). That decision
  was taken on the statement that nothing is published, which holds in part. Checked on 2026-09-30:
  - **Published.** On GitHub, the repository, the Pages address and ten pre-releases, `v0.2.0-beta.1`
    (2026-08-10) to `v0.10.0-beta.1` (2026-09-01), whose assets carry the old names: `ac3forge-*` and
    `ac3gui-*` files, and Debian and RPM packages named `ac3forge`, `libac3forge0` and
    `libac3forge-dev` (RPM: `ac3forge-devel`). On PyPI, the project `ac3forge`, at 0.9.0b1
    (2026-08-22) and 0.10.0b1 (2026-09-01); `docs/releasing.md` says its publishing is live. In
    Homebrew, the public tap `iainchesworthlabs/homebrew-ac3forge`, with the formula `ac3forge` and
    the cask `ac3gui`.
  - **Submitted, not merged.** A vcpkg port named `ac3forge` (microsoft/vcpkg #53470, open and a
    draft since 2026-08-18) and a winget package `iainchesworthlabs.ac3forge` (microsoft/winget-pkgs
    #419594, which winget-pkgs' policy bot closed on 2026-09-29 for want of author feedback). No
    ConanCenter submission was found.
  - **Not published.** Nothing under the family's names is on npm or crates.io, and the ESP Component
    Registry has never had the component (`.github/workflows/esp-component.yml`; the registry itself
    was not queried). Sendspin and the ESP32 firmware have not left this repository, on the user's
    word.
  - **Free.** `iclforge` and `icl-forge` on PyPI, npm and crates.io.
- What that means for N1: the rename can still be outright in the repository, with no shim, and the
  stages do not change. What happens outside the repository was decided on 2026-09-30
  ([decisions 40 and 41](#decisions-of-2026-09-30)), in the study's stages S4 and S5, the publishing
  steps being the user's. `ac3forge` stays on PyPI as an old project, untouched, and `iclforge` is
  published as a new one after the repository is renamed; the old project's trusted publisher names
  the repository `ac3forge` (`docs/releasing.md`), so the new project gets its own. The Homebrew
  formula, cask and tap take the new name, with the old names mapped to it, and the `brew tap` path
  changes. Someone who installed a pre-release keeps the old program names, C API, CMake package and
  pkg-config names at that version, and meets the new ones, without a shim, when moving to a release
  made after N1.
- Open on 2026-09-30, and the user's: whether the open vcpkg pull request, which names the port
  `ac3forge`, is replaced by a port `iclforge` (the winget submission was closed unmerged).
- N1B is layout and naming only: no algorithm changes and every output byte stays the same. It covers
  where the code sits, the C++ namespaces, the header roots and the CMake target names. The duplicated
  DSP (FFT, MDCT, QMF and resampler in `src/ac3` and `src/ac4core`) is a later phase.
- The study recommends renaming `src/ac3` to `ac3`, beside the AC-4 libraries, with five
  codec-blind libraries cut out of it, in stages S0 to S6 with N1A in the same freeze. The user has not
  yet answered its 14 decisions ([its section (i)](layout.md#i-decisions)): the layout, the
  namespace root, grouping, the order with N1A, the merge method, the repository and Pages names,
  TrueHD's directory and the EMDF container among them.

**Exit (N1B):** the tree has the layout the user chose, in the stages the study sets out, each proven by
the builds, the whole `ac3tests`, the pinned bitstream hashes and the bytes of a fixed CLI corpus;
a check states the dependency direction between libraries and passes; the documentation gates pass.

**Verified by (N1B):** the prototype in [the study](layout.md#what-the-prototype-found) for the design;
for the execution, each stage's proof in [its section](layout.md#h-proof-per-stage).

## Decisions

Put to the user on 2026-09-15 and answered the same day. Each lists its options, the
recommendation, what each costs, and what was taken; the table at the end sums them up.

1. **Which phases to build now.**
   - (a) **D1 to D8**: channel-based streams up to 5.1, with metadata processing, presentations and
     the API. Immersive content and objects are decided after D8, with its measurements.
   - (b) **D1 to D9**: also 5.1.4 and the other channel-based immersive layouts.
   - (c) **D1 to D10**: also A-JOC objects.

   **Recommend (b).** Hearth's renderer and sinks are built for immersive layouts, and DEE writes
   5.1.4 in three of the five immersive modes, each of which can be scored against its source.
   A-JOC has one encoded stream here, Chromium's, and nothing to score it against. Cost: (a) eight
   pull requests; (b) one more, whose A-JCC part rests on constructed streams; (c) another, whose
   correctness rests on constructed streams, one encoded stream and listening. D11 and D12 are asked
   in decisions 9 and 8.

   **Taken: (c), D1 to D10**, against the recommendation.

2. **The speech spectral frontend.**
   - (a) **Refuse streams that select it**, with a named error, and implement it when a stream that
     uses it is available.
   - (b) Implement it now, from the text.

   **Recommend (a).** DEE never selected it where that could be observed. Five of its defects
   change how many bits its arithmetic decoder consumes, and no stream here can settle them: a
   symbol search that cannot return a negative value, a fixed-point conversion never defined, a
   loop whose counter is never reset, an undefined increment in both random generators, and a
   table whose layout contradicts its index formula. Cost of (a): a stream that uses SSF on any
   track fails to decode, since SSF data has no length field to skip. Cost of (b): about 35 pages
   and 680 lines of pseudocode, with those readings unverified.

   **Taken: (a).**

3. **Full or core decoding.**
   - (a) **Full decoding only.**
   - (b) Full and core decoding.

   **Recommend (a).** Core decoding is for low-complexity platforms, and this chip targets
   computers. Adding it roughly doubles what the immersive and object phases have to verify. Cost
   of (a): an embedded AC-4 decoder would add core decoding later, mostly as separate paths (S-CPL's
   seven outputs, A-JCC's core modules, Part 2 Tables 44 to 46).

   **Taken: (b), full and core decoding**, against the recommendation. Core decoding joined D9 and
   D10 (both built), and is the mode D14d tries first on the C6.

4. **Third-party streams.**
   - (a) **Fetch the public streams** (DASH-IF, CTA WAVE, DVB, Dolby's delivery kit, Chromium's and
     Media3's test files) into a gitignored local directory when a phase needs them, each named
     with its source and size first, and commit only what a licence allows.
   - (b) Use DEE streams only.

   **Recommend (a).** They are the only streams here from other encoders, the only channel-based
   streams at 25, 29.97 and 30 fps, and Chromium's is the only A-JOC stream. Cost: a fetch script;
   none of them has reference output; the Dolby kit and DASH-IF state no licence, so they stay out
   of the tree.

   **Taken: (a).**

5. **The Dolby Reference Player's AC-4 decoder.**
   - (a) **The user asks Dolby, or whoever administers the licence**, whether this install is
     entitled to AC-4 decoding, and if it is, the decoder is used as a program whose output is
     compared.
   - (b) Debug the player's pipeline here first, stopping at any sign of a licence check.
   - (c) Leave it.

   **Recommend (a).** Dolby's release notes say an install may lack the AC-4 decoder and that Dolby
   supplies it on request, which fits a player that parses AC-4 and produces no samples. With it,
   Verification item 3 becomes a comparison with Dolby's own decoder at index 13. Cost: the user's
   correspondence, and possibly a fee. (b) spends time on what may be a missing component.

   **Taken, in the user's words:** DEE is installed and licensed locally, and it can generate the
   reference samples. The Reference Player is not pursued. The DEE install has no AC-4 decoder (its
   tools were listed on 2026-09-15), so its references are its encodes of known sources, scored as
   Verification item 3 describes.

6. **librempeg as a second decoder.**
   - (a) **Build librempeg's experimental AC-4 decoder outside the tree**, in WSL on `D:`, and
     compare its output as the FFmpeg CLI's is compared: never linked, its source never read, its
     version recorded with each comparison.
   - (b) Do not use it.

   **Recommend (a).** It is the only other decoder that can run here. Agreement between two
   decoders on A-SPX, A-CPL or dialogue enhancement is evidence that closeness to the source cannot
   give. Cost: building an FFmpeg-sized tree (about 1 GB on `D:`, its build dependencies explained
   before anything is installed); a GPL program kept outside the tree; and, since it is
   experimental and partly derived from Emby's code, a disagreement proves nothing without the
   text.

   **Taken: (a).**

7. **Where the DSP comes from.**
   - (a) **The decoder carries its own**: an FFT for 2^a · 3^b · 5^c, the inverse MDCT, KBD windows,
     the `QWIN` QMF bank, the decorrelators and the converter.
   - (b) A shared DSP library, extracted from `ac3::forge` and used by both.
   - (c) The decoder links `ac3::forge`.

   **Recommend (a).** Forge's FFT takes powers of two only and its QMF bank has a different window
   and phase, so little would be shared as it stands. Cost of (a): a second FFT kernel and a second
   QMF bank, a few hundred lines each. (b) changes `ac3::forge`'s minimal profile, the ESP-IDF
   component and the ABI gate to share those lines. (c) contradicts the inspector's standalone
   design and brings `ac3::forge` into every AC-4 build.

   **Taken: (a).**

8. **Arithmetic and the ESP32.**
   - (a) `double` only, and no embedded work in this chip.
   - (b) **The DSP written against a scalar type from D2**, instantiated at `double`, with D12 (the
     `float` build, its gate and an ESP32-S3 measurement) left unscheduled.
   - (c) (b), and a fixed-point tier for the ESP32-C6.

   **Recommend (b).** Hearth's sinks carry AC-3 and E-AC-3, with AC-4 later. A scalar type costs
   little when the code is written; adding one afterwards cost `ac3::forge` a phase
   (`planning/arithmetic-tiers.md`). (c) has no basis yet: forge's JOC path through its QMF bank
   did not fit the S3's internal RAM, and every AC-4 mode except SIMPLE runs a QMF bank on every
   channel. Cost of (b): templates in the DSP, and D12's gate when it runs.

   **Taken, in the user's words:** the ESP32-S3 has an FPU and can use `float`; the C6 does not
   and needs fixed point. So the DSP's scalar type is one a fixed-point type can instantiate
   ([Arithmetic](#arithmetic)), D12 is the `float` tier on the S3 and D13 the fixed-point tier on
   the C6. Both follow D10 and are confirmed with the user then, since decision 1 approved D1 to
   D10.

   On 2026-09-25 the user confirmed the ESP32 work, made the P4 its first target, and D14 took
   D12's and D13's work ([decisions 24 and 25](#decisions-of-2026-09-25)). D12 and D13 were not
   built under those names: D14a and D14b are built, D14c and D14d are not.

9. **AC-4 in IEC 61937 and the extension role.**
   - (a) **The extension role carries AC-4 as `Pc` data type 24 bursts of one sync frame each**,
     defined on this project's extension page, and there is no AC-4 passthrough to a device.
   - (b) Buy IEC 61937-14:2017 and implement it fully, with its repetition periods and `Pc` bits 8
     to 11, for passthrough to devices.
   - (c) Neither, until a device that accepts AC-4 exists.

   **Recommend (a).** No device was found that accepts an AC-4 bitstream, and no operating system
   has an AC-4 IEC 61937 format, so passthrough would have nothing to play to or be verified
   against. The extension role is this project's own protocol and its receivers use this library.
   Cost of (a): a data type and a packer in `ac3::iec61937`, and a paragraph on the extension page.
   Cost of (b): the standard's price, and a mode nothing here can test.

   **Taken: (b), buy IEC 61937-14 and implement it**, against the recommendation. Buying it was the
   user's step; D11 started when the text was here, and is merged (#1052).

10. **Immersive stereo.**
    - (a) **Decode DEE's IMS streams by the observed rule**: `presentation_version` 2 with that
      `channel_mode` code is a stereo substream. The rule is labelled an observation in the errata
      register, `b_pre_virtualized` is reported, and there is no headphone processing.
    - (b) Refuse `presentation_version` 2 until a specification defines it.

    **Recommend (a).** MediaInfo, which reads through Dolby's library, agrees on all 23 streams;
    music services deliver immersive stereo; and IMS is the only local source of the frame rates
    that need the converter. Chromium's `ac4-ims.ac4` is a second check. Cost: a rule the text does
    not contain, which a later version of the specification could contradict.

    **Taken: (a).**

11. **Where this plan lives.**
    - (a) **A pull request to main**, with a row in `planning/README.md`.
    - (b) Committed locally only, as the Hearth plan is.
    - (c) Committed onto the Hearth plan's local branch.

    **Recommend (a).** Each phase's pull request cites this page's phases and decisions, which needs
    the page on main. Cost: main gains a page that names a Hearth plan main does not have; it is
    named in prose, without a link.

    **Taken: (b), local only, as the Hearth plan is**, against the recommendation. Each phase's pull
    request branches from main and states its own exit criterion. **Superseded by
    [decision 22](#decisions-for-the-encoder-and-the-applications)**: the page moved to main on
    2026-09-25.

12. **Dynamic range and dialogue level in Hearth.**
    - (a) **One dynamic range control** (off, home theatre, TV, portable speakers, portable
      headphones) **and one output level.** For E-AC-3, home theatre is line mode at −31 dBFS, TV
      and both portable settings are RF mode at −20, and off is custom mode with no DRC. For AC-4
      each is the matching decoder mode, and the output level is `Lout`, defaulting inside the
      mode's range.
    - (b) Separate controls for each format.
    - (c) One output level control, and DRC controls per format.

    **Recommend (a).** The choice a listener makes, a kind of device and a level, is the same for
    both formats, and E-AC-3's line and RF levels fall in AC-4's home theatre and TV ranges. Cost:
    E-AC-3's `drc_scale` and heavy compression move under a custom setting; the portable settings
    have no A/52 meaning of their own; the register records Table 161's range edges as inclusive;
    and Hearth's design round (A0) takes the mapping in.

    **Taken: (b), separate controls for each format**, against the recommendation. Hearth's A0 draws
    E-AC-3's operating mode and scale, and AC-4's decoder mode and output level, as separate
    controls.

| # | Question | Recommended | **Taken** |
|---|---|---|---|
| 1 | Which phases to build now | D1 to D9 | **D1 to D10** ← against |
| 2 | The speech spectral frontend | Refuse until a stream uses it | **Refuse until a stream uses it** |
| 3 | Full or core decoding | Full only | **Full and core** ← against |
| 4 | Third-party streams | Fetch when a phase needs them | **Fetch when a phase needs them** |
| 5 | The Reference Player's AC-4 decoder | The user asks Dolby | **DEE, installed and licensed locally, generates the reference samples** (the user's words) |
| 6 | librempeg | Build and compare, never read | **Build and compare, never read** |
| 7 | Where the DSP comes from | The decoder's own | **The decoder's own** |
| 8 | Arithmetic and the ESP32 | A scalar type, D12 unscheduled | **The S3 has an FPU and can use `float`; the C6 needs fixed point** (the user's words) |
| 9 | AC-4 over IEC 61937 | The extension role only | **Buy IEC 61937-14 and implement it** ← against |
| 10 | Immersive stereo | The observed stereo rule | **The observed stereo rule** |
| 11 | Where this plan lives | A pull request to main | **Local only, as the Hearth plan is** ← against |
| 12 | Dynamic range in Hearth | One control | **Separate controls for each format** ← against |

### Decisions for the encoder and the applications

Put to the user on 2026-09-24, when the scope grew, and answered the same day. Decisions 1 to 12
stand. Each lists its options, the recommendation, what each costs, and what was taken.

13. **The validation oracles.**
    - (a) **librempeg as the second decoder, FFmpeg for framing, MediaInfo's trace as a third
      reader, and DEE as the gold standard in both directions**, as [The oracles](#the-oracles) sets
      out.
    - (b) (a), and a search for a Dolby AC-4 decoder on a device: Android defines an AC-4 media
      type, and some televisions and Android devices carry Dolby's decoder for it. One driven over
      `adb` by a small test app would be the first Dolby decoder this project could compare with.
    - (c) No second decoder: the transcriptions, the decoder, FFmpeg's framing, MediaInfo and DEE.

    **Recommend (a)**, and (b) if such a device is at hand. The user asked for FFmpeg as the
    parallel decoder, as for AC-3 and E-AC-3; FFmpeg has no AC-4 decoder, and librempeg, a fork of
    FFmpeg with an experimental one, is the nearest program that can take the role. Cost of (a): a
    shallow clone of one commit (the whole repository is about 159 MB packed), fetched with the
    user's go-ahead, and a build tree under 1 GB on `D:`, in the WSL distribution that has the
    compiler, with nothing installed; a GPL and AGPL program kept outside the tree; and an
    experimental decoder, measured on DEE's streams before its output counts. Cost of (b): a device,
    a small Android app, and the device's own processing (DRC, loudness, virtualisation) switched
    off or measured; nothing is attached here and `adb` is not on `PATH`. Cost of (c): the encoder's
    syntax outside DEE's set, and every decode above SIMPLE, get no second opinion.

    **Taken: (a)**, in the user's answer of 2026-09-24.

14. **The encoder's scope, and the order of the work.**
    - (a) **Everything the decoder decodes, in pairs**: each encoder phase right after its decoder
      phase, from stereo to A-JOC objects with their metadata, with 7.1.4 with every channel present
      as an experimental option in D9 and E8.
    - (b) Everything, the decoder first: D2 to D8, then E1 to E7, then the channel-based
      applications, then the immersive and object phases in pairs.
    - (c) No object encoding: as (a), with the encoder stopping at channel-based immersive; D10
      decodes objects and nothing encodes them.

    **Recommend (a).** What the two directions share (the transforms, the QMF bank, the HF
    generator, the converters, the frame writer) is built once with both directions' tests while it
    is fresh, and each encoder phase is checked by a decoder the phase before checked on DEE's
    streams. Cost of (a): Hearth's AC-4 playback waits for six encoder phases more than under (b),
    and E9 is the largest encoder phase, with weaker evidence than the others unless G0 finds a
    master DEE's A-JOC encoder accepts. (b) gets channel-based playback into Hearth sooner and comes
    back to each shared piece later. (c) drops E9, and the object encoding of I5.

    **Taken: (a)**, in the user's answer of 2026-09-24.

15. **Where the encoder lives, and what the two directions share.**
    - (a) **`src/ac4enc` beside the decoder, and a shared core, `src/ac4core`**: the tables, bit
      reading and writing, the transforms, the QMF bank, the reconstruction kernels and the
      converters, in a static library both link privately
      ([Where the libraries live](#where-the-libraries-live)).
    - (b) The encoder links the decoder, and the shared code stays in `src/ac4dec`.
    - (c) One AC-4 library holding both directions, as `ac3::forge` holds AC-3's and E-AC-3's.

    **Recommend (a).** Each direction links what it runs, the syntax stays two separate
    transcriptions, and the ESP32 builds carry no encoder. Cost of (a): D2 moves D1's tables, bit
    reader and Huffman decoder into the core, a mechanical change, and there is a third library to
    build and install. (b) either exports the decoder's internals from its shared library, widening
    its ABI, or builds the decoder into the encoder, which a static build linking both then carries
    twice. (c) puts the encoder into every decoder-only build unless an option splits it again.

    **Taken: (a)**, as recommended; the user raised no objection. As built, D2 moved the generated
    tables into the core and the transforms were written there; the bit reader and the Huffman
    decoder stayed in `src/ac4dec`, and the encoder has a bit writer of its own in `src/ac4enc`.

16. **What the encoder writes by default.**
    - (a) **The syntax DEE's streams exercise**, and everything else behind options named
      experimental, each leaving that list when a reader outside the project agrees with it
      ([What the encoder writes by default](#what-the-encoder-writes-by-default)).
    - (b) Every tool, wherever the encoder finds it helps.

    **Recommend (a).** Outside DEE's set, a reading the encoder, the decoder and the Python parser
    share passes every check this project has; only MediaInfo's trace, for the table of contents and
    metadata, or a second decoder, for audio data, can catch it. Cost: noise fill, VARVAR framing,
    interleaved waveform coding, ASPX_ACPL_1, 7.1, 7.1.4, A-JCC, transmitted DRC gains, several
    presentations and objects need the experimental option until MediaInfo or librempeg agrees, and
    quality at low rates may trail what the extra tools would give.

    **Taken: (a)**, as recommended; the user raised no objection. As built, the experimental set is
    smaller than this cost list: several presentations and substreams are written without the flag,
    since MediaInfo reads their tables of contents as configured (E6), and noise fill is not written
    at all. [What the encoder writes by default](#what-the-encoder-writes-by-default) lists the flags.

17. **The psychoacoustic model.**
    - (a) **The encoder's own**, written for AC-4's bands and transform lengths from the published
      sources `ac3::quality`'s model cites, and not linked.
    - (b) `ac3::quality`'s model moved into a library both codecs link.

    **Recommend (a)**, as decision 7 took for the DSP. Cost of (a): a second model to write and
    calibrate. (b) changes `ac3::forge`'s layout, its ABI and its minimal profile for a model built
    on A/52's 50 bands and 256-sample blocks, which AC-4's fifteen transform lengths would reshape
    anyway.

    **Taken: (a)**, as recommended; the user raised no objection.

18. **Dialogue enhancement parameters.**
    - (a) **From a dialogue stem**, when the input carries dialogue apart from music and effects, or
      from channels the caller marks as dialogue; otherwise the stream carries no dialogue
      enhancement data.
    - (b) (a), and a speech detector for mixed input, as DEE's streams suggest DEE has.

    **Recommend (a).** Cost: a mixed master gets no dialogue enhancement unless its dialogue is
    supplied apart or sits in marked channels, where Part 1 recommends the data wherever dialogue is
    present. (b) is a classifier with its own accuracy to measure and no text to follow.

    **Taken: (a)**, as recommended; the user raised no objection.

19. **The quality bar against DEE.**
    - (a) **Pinned at the first measurement, with the gap to DEE recorded** in each pull request and
      tracked in the trend series; no phase waits on closing it.
    - (b) A phase merges only within a fixed margin of DEE's ViSQOL, set now.

    **Recommend (a).** A margin fixed before any measurement is a guess; pinning stops regressions,
    and the gap shows what to work on, as it does for the E-AC-3 legs. Cost: a phase can merge
    behind DEE. (b) can hold phases on tuning.

    **Taken: (a)**, as recommended; the user raised no objection. As built, each phase's pull
    request recorded the gap to DEE, `score_ac4_encode.py` pins the scores as floors that fail
    FFmpeg Validate's nightly run, the race against DEE runs locally, and the encoder's scores have
    no trend series.

20. **The applications, and their order.**
    - (a) **`ac3cli` with each library phase; once the channel-based library is complete, the rest
      of `ac3cli`, then Hearth desktop, then Forge GUI, then the C API with Python, Rust and
      WebAssembly; immersive and object content in each after D9 to E9; the ESP32 sinks with D12 and
      D13 (D14 and I6, since decision 24); and no AC-4 in Crucible.**
    - (b) As (a), with the bindings before Forge GUI.
    - (c) No application beyond `ac3cli` until every decoder and encoder phase is done.

    **Recommend (a).** Hearth is the application the decoder was planned for, and Forge GUI the one
    people encode with; the bindings serve programs outside the project, for which a C API that
    stays put matters more, and both APIs are final by then. Crucible captures applications into
    Atmos objects for receivers, and no receiver accepts AC-4. Cost: (a) keeps AC-4 from outside
    programs until after the GUI; (c) keeps it from Hearth until the object phases land.

    **Taken: (a)**, in the user's answer of 2026-09-24.

21. **The C API.**
    - (a) **`ac3forge_ac4_*` functions in the existing C API**, with their own status range and the
      AC-4 libraries embedded as `ac3::forge` is; the Rust `-sys` crate picks them up through its
      allowlist.
    - (b) A separate C library and `-sys` crate for AC-4.

    **Recommend (a):** one C library and one crate for callers, in the pattern the C API follows.
    Cost: the C library carries AC-4 for every caller unless it is built with `AC3FORGE_BUILD_AC4`
    off. (b) doubles the packaging.

    **Taken: (a)**, as recommended; the user raised no objection.

22. **Where this plan lives, now.**
    - (a) **A pull request to main**, with a row in `planning/README.md`, and ROADMAP's AC-4 row
      pointing at it.
    - (b) Local only, as decision 11 took.

    **Recommend (a).** The plan now holds twenty-eight phases' pull requests, which other sessions
    will pick up, and ROADMAP's AC-4 row says no plan exists; on main, each phase's pull request can
    link its phase and decisions. The cost decision 11 weighed, a page naming a Hearth plan main did
    not have, is gone: that plan merged as #676.

    **Taken: (a)**, in the user's answer of 2026-09-24. The page moved to main as
    `planning/ac4.md`.

23. **DEE's licence, which runs out on 2026-11-06.** Whether it is renewed is the user's to say.
    - (a) **G0 now, whether or not the licence is renewed.**
    - (b) No G0: each phase makes its own DEE streams, which needs the licence renewed.

    **Recommend (a).** Cost of (a): some hours of DEE runs and a few gigabytes on `D:`, and race
    legs fixed before the encoder exists, so a leg added later needs the licence. (b) stakes every
    later phase's gold standard on a renewal.

    **Taken: (a)**, as recommended. On 2026-09-25 the user said the licence will not be renewed,
    and asked for golden masters to test against now: G1 extends G0's set with every stream the
    remaining phases need from DEE, made before the licence ends.

| # | Question | Recommended | **Taken** |
|---|---|---|---|
| 13 | The validation oracles | librempeg, FFmpeg for framing, MediaInfo, DEE; a device if one is at hand | **librempeg, FFmpeg for framing, MediaInfo, DEE** |
| 14 | The encoder's scope and the order | Everything, in pairs | **Everything, in pairs** |
| 15 | Where the encoder lives | `src/ac4enc` and a shared core | **`src/ac4enc` and a shared core** |
| 16 | What the encoder writes by default | DEE's set; the rest experimental | **DEE's set; the rest experimental** |
| 17 | The psychoacoustic model | The encoder's own | **The encoder's own** |
| 18 | Dialogue enhancement parameters | From a stem or marked channels | **From a stem or marked channels** |
| 19 | The quality bar against DEE | Pinned, with the gap recorded | **Pinned, with the gap recorded** |
| 20 | The applications and their order | CLI, Hearth, GUI, bindings; no Crucible | **CLI, Hearth, GUI, bindings; no Crucible** |
| 21 | The C API | Functions in the existing C API | **Functions in the existing C API** |
| 22 | Where this plan lives | A pull request to main | **A pull request to main** |
| 23 | DEE's licence | G0 now either way | **G0 now**; not renewed, so G1 before 2026-11-06 |

### Decisions of 2026-09-25

Put to the user on 2026-09-25, from a study of the AC-4 code as D6 and E5 left it and of the
AC-3 and E-AC-3 work on the ESP32s, and a survey of the names. The user answered 24 in their own
words, asked for 25 in their own words, and took the recommendations for the rest.

24. **When the ESP32 work runs, and which part first.**
    - (a) **D14, in place of D12 and D13, after D10**, with new code in D9 and D10 written so the
      later work stays small (kernels templated on `Real`, QMF samples through one alias, no
      function-local statics or large stack objects).
    - (b) D14's first part after D8 and before D9 and D10, so that their QMF-domain code is written
      on the scalar from the start.
    - (c) D12 and D13 as planned, the S3 and the C6.

    The study recommended (b); D9 was by then being built beside D7. **Recommend (a).** Cost of (a):
    D9's and D10's QMF-domain code converted in D14 with the rest.

    **Taken: (a), the P4 first**, in the user's words: "we have the P4 as the "best" which has the
    most cpu and ram so that's our ac-4 target i guess until we can optimise for the s3 and c6".

25. **One implementation or two.**
    - (a) **`ac3::forge`'s pattern, one implementation with three scalars**: the faster QMF bank and
      a complex type of the project's own go into the shared core for every tier, `double`
      included.
    - (b) Kernels for the ESP32s only, beside today's.

    **Recommend (a).** Cost of (a): the `double` output moves in its last bits, and the encoder's
    arithmetic with it, so both are scored again. Cost of (b): two QMF banks and two FFTs kept in
    step.

    **Taken: (a)**; the user asked that AC-4 follow AC-3's and E-AC-3's approach, `double`, `float`
    and fixed point by target.

26. **What the `float` tier promises.**
    - (a) **Identical output on the host, the Cortex-M3 leg, the S3 and the P4**, with fused
      multiply-add off, as `ac3::forge`'s `float` tier promises.
    - (b) An agreement in decibels only, which admits fused multiply-add kernels.

    **Recommend (a):** the emulators then stand in for the boards on correctness. Cost: the
    fastest `float` kernels stay out of the portable tier. **Taken: (a).** As measured in D14b, the
    output was identical on the host, the Cortex-M3 leg and the P4 for the probe's five fixtures and
    for every stream without companding, and differed on the five plays with companding, where
    `std::pow` and `std::exp2` at `float` give another last bit in each C library. D14a4 took those
    calls out of libm, and the output is identical for all twenty plays and for the probe's six
    fixtures.

27. **The P4's role.**
    - (a) **The `float` tier's part for 5.1 and full 5.1.4.**
    - (b) A tier of its own.
    - (c) Left until the sink tiers plan's phases for it land.

    **Recommend (a):** it needs no arithmetic of its own and has the most memory. Cost: its figures
    come from a board only, at 360 MHz on early silicon. **Taken: (a)**, and first (decision 24).
    D14b measured it: real time at 2.0, and 5.1 and 5.1.4 at 1.4 to 4.1 and 2.8 to 3.7 times a frame.

28. **Full 5.1.4 decoding on the S3.**
    - (a) **Measured and recorded only**: the S3 aims at 2.0 and 5.1, and 5.1.4 reaches it through
      core decoding or folded to 2.0.
    - (b) A real-time goal on the S3.

    **Recommend (a):** a 5.1.4 frame is about twice a 5.1 frame's work, and 5.1 is estimated at the
    line on one S3 core. Cost of (b): the decode split across two cores, PSRAM, and kernels of its
    own. **Taken: (a).**

29. **The S3's memory.**
    - (a) **2.0 in internal RAM, checked under QEMU in CI; 5.1 and wider with PSRAM, on the
      board.**
    - (b) Everything in internal RAM, which needs a QMF bank run one slot at a time.
    - (c) PSRAM for everything, which QEMU does not emulate, so CI checks nothing.

    **Taken: (a)**, as recommended.

30. **PIE, the S3's and P4's vector instructions.**
    - (a) **A kernel only where the board's timers show one holding back a stream.**
    - (b) PIE kernels for the QMF bank and the FFT now.
    - (c) Never.

    **Recommend (a).** PIE is integer only, so it serves fixed-point kernels inside a `float`
    decode, each written per part and checked on a board. **Taken: (a).**

31. **Sharing `ac3::forge`'s arithmetic.**
    - (a) **A header-only target** holding `Fixed32`, the vector types and the `float` functions,
      used by `ac3::forge` and `src/ac4core`.
    - (b) Copies in `src/ac4core`.
    - (c) `ac3::forge`'s private headers included by path.

    **Recommend (a):** decision 7 kept the DSP kernels apart, not the types, and a copied `Fixed32`
    would drift. **Taken: (a).**

32. **AC-4 on the C6.**
    - (a) **Fixed-point 2.0 measured with WiFi; where it misses, Hearth sends the C6 PCM**, which it
      plays today.
    - (b) Real-time 2.0 as a goal.
    - (c) No AC-4 decoder on the C6.

    **Recommend (a):** read from the code, 2.0 is near real time before WiFi and near the part's
    largest free block. **Taken: (a).**

33. **The frame-rate converter on the ESP32s.**
    - (a) **The host's design, its phases computed per output sample, measured**; a shorter filter
      only if a board needs one.
    - (b) A shorter filter from the start.
    - (c) Index 13 only on the ESP32s, other frame rates as PCM.

    **Recommend (a):** broadcast AC-4 at 29.97 fps needs the converter, whose phase table alone is
    752,752 bytes in `double` as D6 left it. **Taken: (a).** D14b measured the design: the converter
    runs in `double` on a single-precision FPU at 185 to 231 ms a frame, five times real time by
    itself, so the shorter filter the decision allows for is now what the board asks for. D14a4
    ran the dot product in `float` with the table designed once in `double`, which takes 7.3 ms a
    frame at 25/24 and 15/16; what is left at 1001/960 is the first frame's 5.9 s and a table read
    from PSRAM (22 to 98 ms a frame), and a shorter filter is one of the options D14a4's pull
    request puts. D14a5 builds the table at compile time and the converter copies the 188 KB of it
    that it keeps into PSRAM: no first-frame stall, 16.5 ms a frame at 23.976 fps, so the shorter
    filter is not needed for either. The 29.97 fps play's 93 to 98 ms a frame was not the table: it was the heap's low-power
    SRAM, which D14a6 took out, and the converter takes 12.0 ms there.

34. **The encoder on an ESP32.**
    - (a) **Never**, as decision 15 has it.
    - (b) One measurement on the P4.
    - (c) A product.

    **Recommend (a):** nothing but this project's own sinks takes AC-4 (decision 20), and the
    encoder is `double` throughout. **Taken: (a).**

35. **The names, now that `ac3` libraries and programs do AC-4.**
    - (a) **Keep them, and correct the wording** that names AC-3 and E-AC-3 as the family's only
      formats ([N1](#n1-the-names)).
    - (b) Also rename the programs (`ac3cli`, `ac3gui` and the rest).
    - (c) Rename the family, with the old names kept as aliases.
    - (d) Rename `ac3::`, at 1.0's `inline namespace v1`.

    **Recommend (a).** `ac3forge` already names parts with no AC-3 in them, and the recasting plan
    keeps identifiers to 1.0. A rename's cost has about doubled since that plan counted it: about a
    thousand files, the published PyPI and Homebrew names, Hearth's stored settings and pairing
    keys, the ESP32 updater's check of an image's project name, and the Sendspin extension role's
    name at both ends. Cost of (b): `ac3cli` in about 355 files and `ac3gui` in 121, the
    completions, the man page, the formula and cask, winget's aliases, the desktop entries and every
    user's scripts, which the old names kept for a period soften. AC-4's own installed names follow
    decisions 15 and 21, and are harder to change once D8, E7 and I4 export them.

    **Taken: (b)**, against the recommendation, in the user's words: "Let’s take option b on the
    names. I feel that “ac3cli” doing ac4 stuff seems incorrect". Offered the family's name with
    the member (`ac3forge`, `ac3forge-gui`, ...) or the members' own (`forge`, `forge-gui`,
    `hearth`, `crucible`), whose `forge` other tools also use, the user chose the members' own
    ("I like option b for program names"); N1 does the rest. On 2026-09-29 the user widened the
    rename to the family and the layout, and dropped the aliases
    ([decisions 36 to 38](#decisions-of-2026-09-29)).

| # | Question | Recommended | **Taken** |
|---|---|---|---|
| 24 | When the ESP32 work runs | D14 after D10 | **D14 after D10, the P4 first** (the user's words) |
| 25 | One implementation or two | Three scalars, as `ac3::forge` | **Three scalars, as AC-3 and E-AC-3** (the user's words) |
| 26 | What the `float` tier promises | Identical output everywhere | **Identical output everywhere** |
| 27 | The P4's role | The `float` tier's part for 5.1 and 5.1.4 | **That, and first** |
| 28 | Full 5.1.4 on the S3 | Measured only | **Measured only** |
| 29 | The S3's memory | 2.0 internal, wider with PSRAM | **2.0 internal, wider with PSRAM** |
| 30 | PIE | Where a board's timers ask | **Where a board's timers ask** |
| 31 | Sharing forge's arithmetic | A header-only target | **A header-only target** |
| 32 | AC-4 on the C6 | Measured, with PCM where it misses | **Measured, with PCM where it misses** |
| 33 | The converter on the ESP32s | The host's design, measured | **The host's design, measured** |
| 34 | The encoder on an ESP32 | Never | **Never** |
| 35 | The names | Keep them; correct the wording | **Rename the programs: `forge`, `forge-gui`, `hearth`, `crucible`** ← against, in the user's words |

### Decisions of 2026-09-29

Made while N1 was scoped, and on what I2's pull request left open. Unlike the decisions above they
were given as directions, not put as options, so each says what was taken. They extend decision 35
and supersede two lines the plan held after it: that the old program names keep working for a stated
period, and that the family's name, the library's identifiers and the C API stay.

36. **The family's name.** **Taken, on the user's proposal: "ICL Forge"**, organisation
    `iainchesworthlabs`, identifiers `iclforge` and `ICLFORGE_`, in place of `ac3forge`, for the
    family and its packages. The programs keep the plain names of decision 35. Plain `forge` is taken
    on PyPI, npm, crates.io and Homebrew core, and `iclforge` and `icl-forge` were free when checked
    on 2026-09-29, and on PyPI, npm and crates.io still were on 2026-09-30.

37. **What N1 covers, and when.** **Taken, the defaults offered.** N1 is two tasks: N1A names the
    programs and what they register with the system, and N1B lays out and names the libraries,
    including the C++ namespaces, the header roots and the CMake target names. N1B is layout and
    naming only: no algorithm changes, and the duplicated DSP is a later phase. A study came first
    ([the layout study](layout.md), #1122), and the execution is one quiet window after the wave of
    phase branches has merged, N1B first.

38. **Shims for the old names.** **Taken: none.** The user's premise was that "no package has been
    published nor has the sendspin/esp32 been published outside this repo". Package names, the C API
    prefix, the CMake package, the Kconfig prefix, the environment variables and the wire strings are
    renamed outright, with no shim, and no launcher is written for an old program name. The costs
    outside the repository were taken to be GitHub's: its name, the Pages address, existing release
    tags and asset names.

    **The premise held in part** when checked on 2026-09-30. Nothing is on npm, crates.io or the ESP
    Component Registry, and Sendspin and the ESP32 firmware are unpublished, but ten pre-releases are
    on GitHub, the project `ac3forge` is on PyPI (0.9.0b1 and 0.10.0b1), a public Homebrew tap holds a
    formula and a cask, and a vcpkg port is open upstream. The decision is unchanged, since it asks for
    no shim and the rename can still be outright. What follows was decided the next day
    ([decisions 40 and 41](#decisions-of-2026-09-30)): `ac3forge` stays on PyPI as an old project and
    `iclforge` is published as a new one, the tap's names and its `brew tap` path change, and someone
    on a pre-release meets the new names without a shim.

39. **What I2 left open.** **Taken:** a sink on the extension role that does not list `"ac4"` is sent
    nothing for an AC-4 item until a part decodes AC-4 in a group, and I6 settles what it is sent;
    I6 trims the decoder's latency at an item's start; the Decoder page's downmix control stays as
    it is, following the stream's preferred downmix, and Pro Logic II is a follow-up after I6.

| # | Question | **Taken** |
|---|---|---|
| 36 | The family's name | **ICL Forge (`iclforge`), on the user's proposal** |
| 37 | What N1 covers, and when | **N1A the programs and N1B the libraries; a study, then one quiet window, N1B first** |
| 38 | Shims for the old names | **None: renamed outright, no launchers.** The premise, that nothing is published, held in part |
| 39 | What I2 left open | **I6 settles the unlisted sink and trims the latency; Pro Logic II after I6** |

### Decisions of 2026-09-30

Made once the names outside the repository had been checked ([N1B](#n1-the-names)), and like those
of the day before given as directions. Both are carried out in the study's stages S4 and S5, and the
publishing steps are the user's. Neither changes decision 38: the rename inside the repository stays
outright, with no compatibility shim.

40. **The PyPI project.** **Taken:** `ac3forge` stays as it is, untouched, at 0.9.0b1 and 0.10.0b1,
    and the new project's description says "formerly ac3forge". `iclforge` is published as a new
    project after the repository is renamed. The user creates its pending trusted publisher: owner
    `iainchesworthlabs`, repository `iclforge`, workflow `wheels.yml`, environment `pypi`. The
    repository name is the one the study recommends ([its decision 11](layout.md#i-decisions)).

41. **The Homebrew names.** **Taken:** the formula and the cask are renamed to `iclforge`, a
    `tap_migrations.json` in the tap maps the old names to the new, and the tap repository is renamed
    to `homebrew-iclforge`, which GitHub redirects from the old path.

| # | Question | **Taken** |
|---|---|---|
| 40 | The PyPI project | **`ac3forge` untouched; `iclforge` a new project, published after the repository rename** |
| 41 | The Homebrew names | **Formula and cask `iclforge`, the old names mapped in the tap; the tap becomes `homebrew-iclforge`** |

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| The decoder's output agrees with Dolby's decoder | **no** | No Dolby AC-4 decoder runs here and the Reference Player is not pursued (decision 5); at every frame rate but index 13 the converter is the implementer's choice |
| The decoder's output agrees with another decoder | **partly** | librempeg is experimental and partly derived from Emby's code, so agreement with it is evidence and disagreement is settled from the text; it does not decode A-CPL's rebuilt channels, the immersive element, objects, or a presentation's second and later substreams |
| A-SPX, A-CPL and S-CPL reconstruct what the encoder intended | **partly** | Scored against the source; an error that stays close to the source passes |
| A-JCC, A-CPL mode 1, SSF, transmitted DRC gains, dialogue enhancement methods 1 to 3 | **syntax, and gains on known input** | No other encoder here writes them; constructed streams and the encoder's own share this project's readings |
| Presentations of several substreams as an encoder writes them | **partly** | DEE writes one presentation; streams multiplexed from DEE substreams stand in, and the encoder's own, which MediaInfo's trace reads |
| A-JOC objects | **partly** | One encoded stream (Chromium's) and no source; object rendering is not normative, and the listening that would stand in for it has not been done |
| Absolute output level | **partly** | Part 1 states no full-scale convention; it is settled against DEE's source, which assumes DEE's input convention is the decoder's |
| Compression-curve DRC agrees with another decoder | **no** | The level detector is not normative |
| The readings chosen for the errata | **partly** | A reading is checked where an encoder's stream exercises the tool; elsewhere it rests on the text |
| Immersive stereo (`presentation_version` 2) | **by observation** | V1.3.1 names the version without defining it |
| Core decoding matches what a core decoder of Dolby's produces | **partly** | Checked against the text's matrices and gains on DEE and constructed streams; nothing here decodes the core otherwise |
| AC-4 over IEC 61937 to a device | **no** | No device found that accepts it; D11 is checked against IEC 61937-14's tables only |
| Real time on the ESP32-P4 and S3 in `float` and the C6 in fixed point | **on boards, in D14** | QEMU has no cache model and a fabricated clock, and none runs the P4 or the C6; the boards measure time. D14b measured the P4 (real time at 2.0 only, and no CI leg runs it); the S3 and the C6 are not built |
| The encoder's streams decode as a Dolby decoder would decode them | **no** | No Dolby AC-4 decoder runs here; the decoder and librempeg stand in, and the decoder is checked on DEE's streams first |
| The encoder's syntax outside DEE's set is read as Dolby reads it | **partly** | MediaInfo's trace covers the table of contents, the presentation substream and `metadata()`; audio data has only librempeg, which is experimental |
| The encoder's quality against DEE's | **yes, through the decoder, and librempeg where it decodes the tool** | The decoder's reading of the tools concerned is checked on DEE's own streams first; the race runs locally, while DEE's licence does |
| Objects the encoder writes render as Dolby's renderer would render them | **no** | Object rendering is not normative; listening stands in |
| Compression-curve DRC from the encoder's streams behaves alike in every decoder | **no** | The level detector is the decoder's own |
| AC-4 in Matroska | **no** | Matroska registers no `A_AC4` codec ID (issue 176 and pull request 874 of its specification are open, checked 2026-09-29); `ac3cli mkv` refuses AC-4 until the registry has an ID and a mapping |
| The WebAssembly module's C++ side and the Android build of the AC-4 libraries | **by CI only** | No Emscripten or NDK on the machine that made I4; I4b compiled `ac4_bindings.cpp` natively against a host model of `emscripten::val` |
| Any DEE stream after 2026-11-06 | **no** | The licence is not renewed; G0, G1 and G2 made the sets before then (decision 23) |
| What is published under the family's names | **partly** | Checked by hand on 2026-09-30 on GitHub, PyPI, npm, crates.io, the Homebrew tap and the two upstream pull requests. Not checked: the ESP Component Registry (its site was not queried), ConanCenter beyond a search of its pull requests, and the user's statement that Sendspin and the ESP32 firmware are unpublished |

## Coordination

- Branch names are `<type>/<kebab-name>` with the type `feature`, `bugfix`, `hotfix`, `docs` or
  `chore`, which CI's branch-name gate (`pr-gate.yml`) requires; the phases' branches are `feature/`
  or `bugfix/`.
- Before each of the phases that remain, `gh pr list` and `ListAgents`: the phases so far were built
  by sessions working in parallel under one that coordinated them (see [How each phase is
  run](#order)), and more than one session may take a phase.
- Hearth: phase A0 drew the AC-4 pages with dynamic range and output level separate from E-AC-3's
  (decision 12), and I2 enabled them through the engine; the extension role has the AC-4 data type
  from D11; the renderer that D10 and I5 hand objects to is the one Hearth's phase A1 moved into
  `ac3::forge`.
- Files several phases edit, where merges conflict first: the root `CMakeLists.txt`,
  `tests/CMakeLists.txt`, `fuzz/CMakeLists.txt`, `cmake/InstallLibrary.cmake`,
  `tools/ci/classify_changes.py`, `CHANGELOG.md`, `docs/verification.md`,
  `tools/checks/check_doc_paths.py`, `tools/ci/run_codec_matrix.sh`, `.github/workflows/_ci-core.yml`
  (FFmpeg Validate), `apps/cli/main.cpp`'s command table, `ac3::plan::Codec`,
  `docs/assets/data/support-catalogue.json`, `src/capi/`, `python/`, `rust/` and `apps/wasm/`.
- `tools/ci/classify_changes.py` has no lane of its own for AC-4 (`src/ac4*` is in the core lane),
  and `tools/generators/` and `tools/references/` match no lane, so every lane runs when they
  change. No phase gave AC-4 a lane. The ESP32 lane lights after a merge for `src/ac3/`,
  `src/arithmetic/` and `cmake/` (#1131) and leaves the AC-4 trees, which the component's pack stages
  only for `--with-ac4`, to the nightly run.
- librempeg's binary is named `ffmpeg`; nothing puts it on `PATH` ([The oracles](#the-oracles)).
- D11's change sits in `ac3::iec61937`, inside `ac3::forge`, beside the passthrough work of
  Hearth's A3.

## Deliberately not in scope

- Transcoding without decoding, from AC-4 to AC-3 or E-AC-3 or back; `ac3cli transcode` goes through
  PCM (I1), carrying the DRC profile Part 1 5.7.9.4 names for a transcoder.
- Linking FFmpeg, librempeg or any codec library, or reading another decoder's or encoder's source.
- Rendering objects inside the decoder (bar the intermediate spatial format, whose renderer Part 2
  specifies), headphone virtualisation and head tracking.
- 96 and 192 kHz, the HSF extension, beyond its syntax: the decoder reads it (D1) and refuses to
  decode such a substream, so its output is 48 kHz, or 44.1 kHz at index 13 (a 48 kHz decoder may
  ignore the extension, Part 1 4.2.4.3 and 5.4), and the encoder takes 48 and 44.1 kHz input alone.
- 22.2 and 9.X.4, until a stream exists.
- Presentations spread over several elementary streams (Part 2 5.1.2), and the efficient high frame
  rate mode (Part 2 5.1.3), in either direction, until a stream uses them.
- The speech spectral frontend: in the decoder until a stream uses it (decision 2); in the encoder
  at all, since an encoder for it contains its decoder, with the five defects decision 2 lists.
- Writing immersive stereo (`presentation_version` 2): V1.3.1 names the version without defining it,
  so the encoder does not copy DEE's use of it, while the decoder reads it by the observed rule
  (decision 10).
- Protected (encrypted) AC-4 tracks.
- MPEG-2 TS under ATSC A/342-2's profile, which `ac3cli ts` refuses; DVB's is carried.
- AC-4 on the ESP32-C3 and the other ESP32 parts; D14 plans the P4, the S3 and the C6, and has built
  the P4. The encoder on any ESP32.
- AC-4 in Crucible (decision 20).
- AC-4 in Matroska, until the registry has a codec ID (`ac3cli mkv` refuses it).
