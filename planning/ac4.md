# AC-4: a decoder, an encoder and the applications

!!! note "Status as of 2026-09-24: D1 merged; the scope extended to an encoder and the applications"
    Written on 2026-09-15 as phase D0 of chip D in the Hearth plan, as a plan for a decoder.
    Extended on 2026-09-24, when the user widened the scope to a decoder, an encoder and their
    integration into the applications: [What encoding AC-4 involves](#what-encoding-ac-4-involves),
    the encoder's design, [The oracles](#the-oracles), the encoder's ladder, the
    [encoder phases](#encoder-phases), the [application phases](#application-phases) and
    decisions 13 to 23 are new. The page was kept on a local branch, `feature/ac4-decoder-plan`
    ([decision 11](#decisions)), until the user moved it to main as `planning/ac4.md`
    ([decision 22](#decisions-for-the-encoder-and-the-applications)).

    D1, the library and the channel-coded syntax, merged as #700 on 2026-09-16, with the readings
    settled in #712 and #715. The inspector gained `oamd_common_data()` and the HSF extension's
    index in #739 and #744, and the decoder the HSF extension's content in #786.

    The twelve decisions under [Decisions](#decisions) were put to the user on 2026-09-15 and
    answered the same day. Five answers went against the recommendation and two were given in the
    user's own words; both kinds are marked. The phases follow the answers: D1 to D10 are approved,
    D11 can start after D1 (the user supplied IEC 61937-14:2017, 61937-1:2021 and 61937-2:2026 on
    2026-09-15), and D12 and D13, the `float` and fixed-point tiers, are planned and confirmed with
    the user when D10 lands. Decisions 13 to 23, which the wider scope raises, were put to the user
    on 2026-09-24 and answered the same day: the four put as questions (13, 14, 20 and 22) took the
    recommendation, and the recommendations stated for the rest were taken without objection.
    Whether DEE's licence is renewed after 2026-11-06 is not yet known.

    Shape follows the Hearth plan: design sections say what is proposed and why, each phase carries
    an exit criterion and how it is verified, [Decisions](#decisions) gives the options with a
    recommendation and the cost of each, and
    [What cannot be verified, and why](#what-cannot-be-verified-and-why) says where the evidence
    stops. The reading behind the decoder (both specifications, a census of 100 DEE encodes, the
    public record on conformance material, IEC 61937 and other decoders) was done on 2026-09-15; the
    reading behind the encoder and the applications (the specifications again, the tools installed
    here, the AC-3 and E-AC-3 validation machinery and each application's code) on 2026-09-24.

## What is asked

The Hearth desktop application is designed with AC-4 pages that stay disabled until a decoder
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

### The inspector

`src/ac4` (`ac4::ac4`, roadmap IM4, PR #442) reads sync frames and their CRC (Annex G), the table
of contents, presentation information v0 and v1, substream group information for channel-coded,
A-JOC, direct-coded object and OAMD substreams, and the substream index table. It builds `dac4`,
reports samples per frame and writes the RFC 6381 codec string. It reports `audio_data()` and
`metadata()` as byte ranges. Since #739 it reads the `oamd_common_data()` an
`ac4_substream_info_ajoc()` embeds, and since #744 each substream's `hsf_ext_substream_index`. Other
facts the decoder and the encoder meet:

- Nothing installs or exports the library (`docs/library/index.md:10-12`), so only an in-tree
  build links it.
- Its bit reader reads one bit at a time inside an anonymous namespace
  (`src/ac4/src/ac4.cpp:24-93`). Its public types give each substream's byte offset and
  `audio_size`, but not the position where `audio_data()` starts.
- `ac4::scan` stops at a partial frame, and there is no AC-4 counterpart to
  `io::AccessUnitAccumulator` for input that arrives in pieces.
- `ac3cli probe` writes `presentations_v0` to its JSON and no v1 presentations
  (`apps/common/probe_json.cpp:622`). Every stream DEE writes has v1 presentations.
- `ac4::build_dac4()` and `ac4::rfc6381_codec_string()` take an `ac4::Toc`, so they can describe a
  stream the encoder writes as well as one the inspector reads.
- The ESP-IDF component forces `AC3FORGE_BUILD_AC4` off (`esp-idf/ac3forge/CMakeLists.txt:210`),
  and the root `CMakeLists.txt` refuses it in the minimal profile.
- The two defects found while planning are fixed: `fuzz_ac4_parse` now links an instrumented
  inspector (`fuzz/CMakeLists.txt:117-118`), and an EMDF-only presentation reads its EMDF substream
  list on both table-of-contents paths, checked by frames built in both languages.

The Python reference parser `tools/references/ac4_parse.py` reads the same framing, transcribed
independently; CI runs it through `ac4_syntax.py`'s digest test. `tests/ac4/test_ac4.cpp` checks one
committed DEE stream, `tests/golden/external-baseline/ac4-stereo-64/dee.ac4`, against MediaInfo's
reading of it; ten more committed DEE streams sit beside it.

### The decoder after D1

`src/ac4dec` (`ac4::decoder`) reads every syntax element of a channel-coded frame and produces no
audio (`src/ac4dec/include/ac4dec/decoder.hpp:23-35`): the presentation substream, the Part 1
channel elements with ASF, stereo processing, companding, A-SPX and A-CPL data, `metadata()` with
DRC and dialogue enhancement, EMDF payload substreams, and a channel-coded substream's HSF extension
(#786). It refuses, by name, the speech frontend, the immersive and 22.2 elements, object
substreams, and the efficient high frame rate mode. `src/ac4dec/ERRATA.md` records every reading
taken where the text is ambiguous or defective, with its evidence.

The syntax is transcribed twice, in C++ and in `tools/references/ac4_syntax.py`, and the two traces
must agree record for record: `tests/golden/ac4dec/*.tsv` holds the Python parser's digests of the
committed streams, `tests/ac4dec/test_ac4dec_syntax.cpp` holds the decoder to them on every CI leg,
and `tools/checks/test_ac4_syntax_digests.py` holds the Python parser to the same files. Over the
local census (107 DEE streams, 50,728 frames) and 6,670 frames of public streams from other Dolby
encoders, every digest agrees. `tools/checks/ac4_syntax_differential.py` compares the two on
mutated, synthetic and fuzzed streams; it runs nightly, and does not gate. `fuzz_ac4_decode` is
instrumented and has regression inputs.

Five contract items from the review of #700 are open, and D8 closes them: `DecoderConfig` stores a
`SyntaxSink` that its comment gives a call-scoped lifetime, so a temporary lambda dangles once the
decoder copies the configuration (`src/ac4dec/src/decoder.cpp:367-378`); an ASF Huffman miss reports
`kInvalidStream` where A-SPX, A-CPL and metadata report `kTruncated` for the same failure
(`src/ac4dec/src/syntax/asf.cpp:515-517` and six more sites); an HSF extension substream that
nothing claims gets no report, although the header says HSF is refused; there is no
`tools/ci/abi-allowlist/libac4dec.so.txt`; and the Android, WASM and Python wheel configurations
compile both AC-4 libraries without linking them, since `AC3FORGE_BUILD_AC4` is on by default and
their targets are not `EXCLUDE_FROM_ALL`.

### The applications today

AC-4 reaches the applications only through the inspector. `ac3cli` links `ac4::ac4` alone
(`apps/cli/CMakeLists.txt:101`): `probe` reads raw AC-4, and `mp4`, `ts` (the DVB profile; ATSC is
refused), `remux` and `demux` carry it. `fmp4` with its HLS and DASH output, `mkv`, `decode`,
`transcode`, `monitor`, `play`, `qc`, `levels` and `spdif` have no AC-4 path, and `encode` and
`eac3-encode` write AC-3 and E-AC-3 only. Hearth lists AC-4 items with their table of contents and
marks them unplayable, since `Session::open` needs `io::scan`; its AC-4 decoder page
(`apps/hearth/ui/qml/DecoderAc4.qml`) is drawn and disabled. Forge GUI, Crucible, the C API, the
Python and Rust bindings and the WebAssembly modules have no AC-4 at all, and
`docs/assets/data/support-catalogue.json` has no AC-4 row.

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
evidence for that reading.

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
- **Other decoders.** None runs here:
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
    Emby's, Kodi's and NextPVR's AC-4 decoding come from the same code. It is not built here.
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
(`src/forge/src/core/fft_kernel.hpp`), and its public MDCT is fixed at 512 and 256 samples.

**The QMF bank has a published window.** AC-4's filterbank has the structure of the one
`ac3::forge` built for E-AC-3 JOC (`src/forge/include/ac3/dsp/qmf.hpp`): 64 complex subbands, a
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
in the tree is `ac3::quality`'s model (`src/forge/include/ac3/quality/perceptual.hpp`): Johnston's
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
  build links it once, and it has no public headers and no ABI of its own. D2 creates it and moves
  D1's tables, bit reader and Huffman decoder into it.

The syntax stays apart. The decoder's reader and the encoder's writer are separate transcriptions of
the syntax tables, in opposite directions, and the Python reference stays independent of both. A
stream the encoder writes is therefore read by the decoder, which shares only the core's tables and
kernels with it, and by the Python parser, which shares nothing with it, down to its own tables.

`AC3FORGE_BUILD_AC4` gates all of them. They stay out of the ESP-IDF component and the minimal
profile until D12 brings the decoder and the core in; the encoder never goes there. Each is added
where the peer libraries are: its own `CMakeLists.txt`, the root option and subdirectory,
`tests/CMakeLists.txt`, an instrumented fuzz target, an ABI allowlist, the coverage table,
`docs/building.md`, `docs/library/` and `docs/verification.md`. Once their APIs are stable, D8 and
E7 install and export them in `cmake/InstallLibrary.cmake` and `cmake/ac3forgeConfig.cmake.in`,
since an exported target cannot link one that is not exported.

### What the inspector grows

- The fields of the table of contents the decoder needs: `add_ch_base`, the per-instance
  `b_iframe`/`b_audio_ndot` flags, `presentation_id`, `b_pre_virtualized`, `b_alternative`,
  `b_pres_ndot`, the presentation substream's index and the EMDF payload substreams' indices (D1).
- A splitter that holds a partial frame between reads, for streams that arrive in pieces (D8).
- `presentations_v1` and the metadata listed under [Media information](#media-information) in
  `ac3cli probe`'s JSON, added under `ac3forge.probe/1` (D8).

As built in D1, `ac4_presentation_substream()` (Part 2 6.2.2.3), `presentation_version` 2 as DEE's
IMS streams use it ([decision 10](#decisions)), and the position where `audio_data()` starts are
read by the decoder, with its own bit reader, which carries the syntax trace; the inspector stays
the table of contents and the substream framing. Media information takes presentation names from
the decoder's reading in D8.

Since #739 the inspector reads the `oamd_common_data()` of an `ac4_substream_info_ajoc()`. The OAMD
substream's own content, which can carry a second one, waits for the object phase.

### DSP

The two directions carry their own ([decision 7](#decisions)), in the shared core:

- an FFT for lengths of the form 2^a · 3^b · 5^c, and the MDCT and inverse MDCT on it, each tested
  against a direct evaluation of its formula (Part 1 5.5.2 for the inverse; its adjoint for the
  forward transform);
- KBD windows computed from their formula, with the alphas of Part 1 Table 186;
- the QMF analysis and synthesis banks with `QWIN`;
- the kernels of reconstruction an encoder also runs, to see what a decoder will produce:
  dequantisation, the stereo and multichannel matrices, companding, the A-SPX HF generator and
  envelope adjustment, and the three A-CPL decorrelators with the transient ducker, which A-JCC and
  A-JOC reuse;
- the sample rate converters in both directions: from the internal rate to 48 kHz for the decoder,
  and from 48 kHz to the internal rate for the encoder;
- the tables: the Huffman codebooks, in the decoder's order and in the index order an encoder looks
  codewords up by, generated together from the attachment; the scale factor band tables; and the
  A-SPX, A-CPL, dialogue enhancement and DRC tables.

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
would otherwise lose a small value's bits. Only `double` is built until D12 adds `float` and D13
adds fixed point. The QMF-domain tools make the fixed tier harder than forge's was: every codec
mode except SIMPLE runs a complex filterbank, A-SPX's envelope estimates and, in the A-CPL modes,
IIR decorrelators on every channel, and forge's own JOC reconstruction still runs in `float` in
every build because of the same filterbank. The speech frontend's arithmetic decoder uses integer
arithmetic in every build, as the text specifies.

The encoder runs on computers only. It is built at `double`, on the same scalar type as the core it
shares, with no `float` or fixed-point tier and no place in the ESP32 builds. Its output is
deterministic for one toolchain, as `ac3::forge`'s encoders' is, and is not promised byte-identical
across toolchains.

### The API

The decoder takes one `raw_ac4_frame` at a time and returns PCM for one presentation. A sketch; the
names are not final:

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
- **The syntax trace.** D8 settles the sink's lifetime, which `DecoderConfig` gets wrong today: the
  decoder either owns a copy of the callable or takes the sink with each call, and the encoder's
  write trace follows the same rule.
- **Hearth.** The engine's `StreamDecoder` (`apps/hearth/engine/stream_decoder.hpp`) decodes a unit
  into blocks rendered onto a `render::OutputLayout`; `decode_by_block` and the frame's layout are
  what an adapter there needs, and the adapter lives in the engine, since the decoder does not link
  `ac3::forge`'s renderer.

### The encoder's API

The encoder takes PCM at 48 kHz, or at 44.1 kHz for `frame_rate_index` 13, the one index Part 1
Table 84 defines at that rate, in blocks of any length, and returns each `raw_ac4_frame` as it
completes. A sketch; the names are not final:

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
  decoder reads, under the lifetime rule D8 settles.

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
  `wait_frames` needs; a variable rate fixes quality and lets the size follow.
- **The closed loop.** The encoder can reconstruct its own frame with the core's kernels and
  measure the error per band, as `ac3::quality`'s distortion criterion does for AC-3. It uses that
  to calibrate the model, and inside the loop only where the measurements show it pays.

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
  tables, which are not public. ATSC A/342-2's maximum frame sizes match one sync frame per burst,
  repeating once per audio frame, exactly at every frame rate; that is an inference.
- **Devices.** No receiver, soundbar or processor was found that accepts an AC-4 bitstream. Dolby
  wrote in 2021 that none existed, and that televisions and set-top boxes decode AC-4 and send PCM
  or Dolby MAT onward. No IEC 61937 AC-4 format exists in FFmpeg's S/PDIF muxer, Android's S/PDIF
  encoder, Linux's HDMI header or Windows' compressed-audio subformats. CTA-861 signals AC-4 as
  audio coding extension type 12.
- **In this project.** IEC 61937-14 is implemented from its text ([decision 9](#decisions)): the
  user buys the standard, and D11 starts when it is here. D11 gives `ac3::iec61937` the AC-4 burst
  types with their repetition periods and `Pc` fields, `PassthroughSink` an AC-4 format, and the
  extension role `_ac3forge_player@v1` an AC-4 data type. The extension role is this project's own
  protocol, whose bursts drop IEC 61937's sync words and stuffing, and its receivers decode with
  this library. No device here can check passthrough.

### The ESP32

The S3 in `float` (D12) and the C6 in fixed point (D13), after D10 and confirmed with the user
then. The ESP-IDF component and the minimal profile build without AC-4 today. Both parts decode
only; the encoder is not built for either.

- **What they face.** Every AC-4 codec mode except SIMPLE runs QMF analysis and synthesis and
  A-SPX on each channel, and the A-CPL modes add three decorrelators per band. In E-AC-3, forge's
  JOC reconstruction through its QMF bank peaked at 449,826 bytes on the ESP32-S3, where a decode
  leaves a largest free block of 116,736 (`docs/platforms/bare-metal/esp32-s3.md`). A frame at
  index 13 lasts 42.7 ms.
- **The S3** has a single-precision FPU, and 8 MB of PSRAM on the N16R8 development boards. D12
  measures decode time and peak memory for 2.0, 5.1 and 5.1.4 in full and core decoding.
- **The C6** has no FPU, no PSRAM, 512 KB of SRAM shared with WiFi, and one 160 MHz core. Core
  decoding ([decision 3](#decisions)) is the mode that part is most likely to manage. D13 measures
  what fits and what keeps up, with the network up, and states which streams do not.

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
| FFmpeg 8.0.1 | sync frames in raw AC-4; the AC-4 track of an MP4 file | framing only | framing of raw output and of MP4 and TS carriage | CI, in FFmpeg Validate |
| DEE's MP4 muxer | reads an AC-4 elementary stream to write its sample entry | | the encoder's raw output muxes, and the `dac4` it writes equals the encoder's | locally |
| The decoder | PCM | | every stream the encoder writes | CI |
| Listening | rendering and objects, where numbers do not reach | D9, D10 | E8, E9 | locally |

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
G0 and G1 ([G0](#g0-the-gold-set)) make every DEE stream the later phases need before that date;
the licence is not renewed.

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
   `extended_metadata`. An independent bit writer, as `tests/ac4/test_ac4.cpp` uses for A-JOC
   framing today, builds syntax no encoder here writes: A-CPL mode 1, A-JCC, transmitted DRC gains,
   and dialogue enhancement methods 1 to 3. From E1 on, the encoder's streams reach it too. These
   check parsing, the invariants, and gains on known input; the writer, the encoder and the decoder
   can share a misreading.
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
   ([decision 19](#decisions-for-the-encoder-and-the-applications)). `quality_race.py` gains the
   AC-4 legs, and the trend series track them as they track E-AC-3's.
6. **Metadata gains match their formulas.** Through the decoder's output processing (D6), the
   encoder's streams give the dialogue enhancement, output level, downmix and mixing gains their
   configuration asks for, to 0.01 dB, with one tone per channel.
7. **The encoder space.** A harness in the pattern of `tools/ci/fuzz_eac3_encoder_space.py` draws
   configurations (layout, rate, frame rate, codec mode, tools, presentations and metadata) and
   short signals, encodes them, and requires items 1 and 3's FFmpeg framing and a clean decode.
   Seeds that once failed are replayed as regressions; it runs for 120 seconds on each pull request
   and 900 nightly, as E-AC-3's does, and locally it adds librempeg and MediaInfo.
8. **Robustness and determinism.** An instrumented fuzz target over the encoder's configuration and
   input (silence, DC, full-scale square waves, clipped signals, and non-finite samples, which are
   refused); the sanitizer legs; and the same bytes from the same input and configuration on one
   toolchain, pinned by hashes as `tests/golden/bitstream-hashes.json` pins AC-3's and E-AC-3's.

**What goes in the tree.** Short DEE streams (about five seconds each) under `tests/golden/`, with a
generator that makes them from committed sources as `tools/generators/gen_ac4_baseline.py` does
now, and never runs in CI; each stream's manifest entry records its scores, as the AC-3 and E-AC-3
legs' `manifest.json` does. The encoder's streams are not committed: CI makes them, and their hashes
are pinned. The full census, G0's full set and librempeg's outputs stay local on `D:`. Third-party
streams are committed only where their licence allows it.

## Phases

### Order

G0 comes first, because DEE's licence runs out on 2026-11-06. After it, each encoder phase follows
the decoder phase that decodes what it writes, so that what the two share is built once, with both
directions' tests, and each encoder phase is checked by a decoder the phase before checked on DEE's
streams ([decision 14](#decisions-for-the-encoder-and-the-applications)). The applications follow
the channel-based library.

| # | Phase | What it builds | Needs |
|---|---|---|---|
| 1 | [G0](#g0-the-gold-set) | the gold set: every DEE stream the phases need | |
| 2 | [D2](#d2-waveform-coded-stereo-to-pcm) | waveform-coded stereo to PCM, and the shared core | D1 |
| 3 | [E1](#e1-the-encoder-library-the-frame-writer-and-simple-mono-and-stereo) | the encoder library, the frame writer, SIMPLE mono and stereo | D2 |
| 4 | [D3](#d3-the-qmf-domain-and-a-spx) | the QMF domain and A-SPX | D2 |
| 5 | [E2](#e2-a-spx-and-companding) | A-SPX and companding | D3, E1 |
| 6 | [D4](#d4-the-5x-element) | the 5.X element | D3 |
| 7 | [E3](#e3-the-5x-element) | the 5.X element | D4, E2 |
| 8 | [D5](#d5-a-cpl) | A-CPL | D4 |
| 9 | [E4](#e4-a-cpl) | A-CPL | D5, E3 |
| 10 | [D6](#d6-output-processing) | output processing | D5 |
| 11 | [E5](#e5-metadata-frame-rates-and-i-frames) | metadata, frame rates and I-frames | D6, E4 |
| 12 | [D7](#d7-presentations) | presentations | D6 |
| 13 | [E6](#e6-presentations-and-several-substreams) | presentations and several substreams | D7, E5 |
| 14 | [D8](#d8-the-api-the-cli-media-information-and-packaging) | the decoder's API, CLI, media information and packaging | D7 |
| 15 | [E7](#e7-the-encoders-api-the-cli-and-packaging) | the encoder's API, CLI and packaging | D8, E6 |
| 16 | [I1](#i1-the-rest-of-ac3cli) to [I4](#i4-the-c-api-python-rust-and-webassembly) | the applications, for channel-based content | D8, E7 |
| 17 | [D9](#d9-channel-based-immersive) | channel-based immersive | D8 |
| 18 | [E8](#e8-channel-based-immersive) | channel-based immersive | D9, E7 |
| 19 | [D10](#d10-a-joc-objects) | A-JOC objects | D9 |
| 20 | [E9](#e9-a-joc-objects) | A-JOC objects | D10, E8 |
| 21 | [I5](#i5-immersive-and-object-content-in-the-applications) | immersive and object content in the applications | D10, E9 |
| | [D11](#d11-ac-4-over-iec-61937) | AC-4 over IEC 61937 | D1; any time |
| | [D12](#d12-the-float-tier-and-the-esp32-s3), [D13](#d13-the-fixed-point-tier-and-the-esp32-c6), [I6](#i6-the-esp32-sinks) | the ESP32 tiers and sinks | D10, and the user's confirmation then |

Hearth's AC-4 pages activate for channel-based content in I2, for immersive content and objects in
I5.

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

### G0: the gold set

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

**G1, the golden masters (2026-09-26).** On 2026-09-25 the user said DEE's licence will not be
renewed and asked for golden masters to test against now. G1 made 439 legs beside G0's in
`D:\ac3bld\ac4-gold` (433 streams, 188 MB, gold version 4), listed under the manifest's `g1_legs`.
G0's legs, sources and files are as G0 left them, and the scorers that read `legs` fail a leg they
have not pinned, so a phase takes G1's legs into its scorer as it pins them. `gen_ac4_baseline.py`
makes each leg from committed material and groups them by the phases they serve:

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
- D12, D13 and I6: 60 s programmes, 2.0 from 48 to 256 kbps, 5.1 from 96 to 448, 5.1.4 at 192,
  256, 512 and 768, and immersive stereo at five frame rates.
- I1 and I5: E-AC-3 and AC-3 from `dee_ddp_encoder`, and E-AC-3 JOC from `dee_ddpjoc_encoder`'s
  channel-based immersive input at 5.1.4, 7.1.4 and 9.1.6, of the same sources; 7.1 input; and
  DEE's own downmix of 5.1 and 7.1 input to 2.0.

Each leg keeps DEE's command and log, MediaInfo's trace with a table of contents for every frame
(`--ParseSpeed=1`) and its summary, and DEE's MP4 of the stream; the manifest holds the SHA-256s
and what main's `ac3cli` made of each stream, and G0's streams have their MP4s under `mp4\`. Both
transcriptions agree on all 396 AC-4 streams (the census comparison), and main's decoder decodes
each 2.0 and 5.1 stream at index 13; it refuses the 5.1.4 streams' immersive element (D9) and the
other frame rates (D6, not yet on main), which is where those phases start. Three 5 s streams of
one tone per channel at 5.1.4, one in each immersive codec mode, are committed, with their digests
left to D9.

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
same install.

### Decoder phases

#### D1: the library and the channel-coded syntax

**Built:** merged as #700 on 2026-09-16, with the readings of #712 and #715; the HSF extension's
content followed in #786.

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

- The shared core, `src/ac4core/` ([Where the libraries live](#where-the-libraries-live)): D1's
  tables, bit reader and Huffman decoder move into it, and this phase's transforms are written
  there, the forward MDCT beside the inverse, so that E1 finds them built and tested.
- ASF reconstruction: scale factors, dequantisation, noise fill, ungrouping.
- Stereo processing: M/S and prediction.
- The inverse MDCT on the FFT for 2^a · 3^b · 5^c, the direct form it is tested against, KBD
  windows, block switching, overlap-add and the frame alignment delay.
- Mono and stereo in SIMPLE mode. `ac3cli decode` reads AC-4 and writes the coded channels, and
  the scripts that score E-AC-3 (`quality_race.py`'s scoring, the gold gate's comparison) take the
  decoder's output. Frame rates other than index 13 are refused until D6.
- librempeg built as [The oracles](#the-oracles) describes, once the user has confirmed it
  (decision 13), and measured on the committed and census streams.

**Exit:**

- The fast inverse MDCT and the forward MDCT each equal their direct forms to 1e-12 relative at
  every length and every block transition Part 1 Table 187 allows, and the windows reconstruct
  perfectly across them.
- Every DEE 2.0 stream from 192 to 768 kbps in G0's set, music and speech, decodes with its level
  within 0.2 dB of the prediction, its per-channel SNR at or above its pinned floor, its alignment
  at the computed latency, and each channel's tone on its own channel.

**Verified by:** `ac3tests`; a scoring script over the committed streams on the legs that run the
gold-reference gate, and over the full set locally.

#### D3: the QMF domain and A-SPX

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

- Selection: level, enabled flag, language, content classifier, associated types and
  `b_pre_virtualized`, for version 0 and version 1 presentations.
- Presentations of several substreams: M&E with dialogue, main with associated, and main with a
  dialogue enhancement substream for the hybrid methods.
- Mixing (Part 1 6.2.16; Part 2 4.8.4 with substream group gains and the mixer's division as the
  register reads it), and the dialogue and associated gains.
- The test multiplexer that builds such presentations from DEE substreams, writing their tables of
  contents with E1's frame writer, which E6 extends to the encoder's own presentations.

**Exit:** a table of constructed tables of contents selects as Part 2 4.8.2 requires; every mix,
measured with one tone per substream, equals its formula to 0.01 dB; associated audio pans at the
three angles Part 1 Table 216 defines; the multiplexed streams parse in both implementations.

**Verified by:** `ac3tests`; the gain scripts.

#### D8: the API, the CLI, media information and packaging

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

**Exit:** the Hearth engine, or a test standing in for it, decodes every stream in the set through
the public API alone; the CLI tests cover every option; the packages contain the libraries; each
contract item has a test that failed before it was fixed; `mkdocs build --strict` and
`tools/checks/check_doc_paths.py` pass.

**Verified by:** `ac3tests` and the CLI tests on every leg; the package check; the documentation
gates.

#### D9: channel-based immersive

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

- `audio_data_ajoc`, `var_channel_element`, A-JOC in full decoding, object audio metadata (common,
  timing and dynamic data), dialogue enhancement for A-JOC (Part 2 5.8.2.3), and the ISF renderer.
- Core decoding: the downmix signals or static bed as the objects, the first object metadata portion
  (Part 2 4.8.3.4.2), and dialogue enhancement with A-JOC's interpolated dry matrix (5.8.2.4).
- Direct-coded object substreams (`ac4_substream_info_obj`), which use the Part 1 elements.
- Objects and their Annex F properties on the API; the OAMD substream's content, with the second
  `oamd_common_data()` it can carry.

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

From IEC 61937-14:2017, with 61937-1:2021 and 61937-2:2026 for the burst format they extend
([decision 9](#decisions)); the user supplied all three on 2026-09-15, read in place and not copied
into the tree.

- The four AC-4 burst types (`Pc` data type 24, subdata types 0 to 3) in `ac3::iec61937`, with
  Part 14's repetition periods, burst sequences at the 1000/1001 frame rates, maximum burst lengths
  and the AC-4 fields of `Pc` bits 8 to 11; `BurstReader` recognising them.
- An AC-4 format in `PassthroughSink`, on the backends whose operating system has a way to send it.
- The AC-4 data type on the extension page Hearth's phase A4 writes, and in the test sink.

**Exit:** for every frame rate, a stream packed and read back returns every frame unchanged, with
each burst's repetition period, sequence and `Pc` fields as Part 14's tables give them; the test
sink decodes an AC-4 stream sent through the extension role.

**Verified by:** `ac3tests`, with the tables' cases transcribed from the standard; the loopback test
of Hearth's phase A4. No device here accepts AC-4, so passthrough to one is not checked.

#### D12: the `float` tier and the ESP32-S3

- The `float` build of the DSP and a gate against the `double` build.
- The decoder in the ESP-IDF component and the minimal profile for the S3, and a probe row per
  layout.
- Decode time and peak memory for 2.0, 5.1 and 5.1.4, in full and core decoding, on an ESP32-S3
  board.

**Exit:** the gate states the agreement measured on every committed stream, and the board figures
are recorded with a statement of which AC-4 streams an S3 sink decodes in real time.

**Verified by:** the gate in CI; the S3 probe under QEMU in CI; the board.

#### D13: the fixed-point tier and the ESP32-C6

- A fixed-point build of the DSP, with the block exponent where a fixed store would lose a small
  value's bits, and a gate against the `double` build.
- The decoder built for the C6, following Hearth's phase C1 bring-up.
- Decode time and peak memory, in core decoding first, with WiFi connected, on an ESP32-C6 board.

**Exit:** the gate states the agreement measured; the board figures are recorded with a statement
of which AC-4 streams, in which decoding mode, fit the C6 and keep up, and of those that do not.

**Verified by:** the gate in CI; the board.

### Encoder phases

Each encoder phase follows the decoder phase that decodes what it writes and uses what that phase
built. Its pull request records the race against DEE at the phase's layouts and rates, and
librempeg's decode of the encoder's streams, besides the exit criterion. What it writes outside
DEE's set is an experimental option
([What the encoder writes by default](#what-the-encoder-writes-by-default)), whose exit is a clean
decode with the invariants holding and MediaInfo's and librempeg's readings recorded.

#### E1: the encoder library, the frame writer, and SIMPLE mono and stereo

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
- Mono, in the `single_channel_element`, as an option.

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

- QMF analysis from D3's core, aligned to the decoder's delays (Part 1 Tables 188 and 192).
- The A-SPX encoder: `aspx_config` (crossover, master frequency table, noise subband groups) from
  the rate; framing from a transient detector, in FIXFIX, FIXVAR and VARFIX, with a FIX end followed
  by a FIX start; envelopes estimated against what D3's HF generator, run from the core,
  regenerates; noise floors and added tones; delta coding and codebook choice; balance coding for
  stereo; and Part 1's limits of five noise groups, five patches and four or five envelopes.
- Companding: the compressor, as the inverse of the decoder's expander, on and off by rate as DEE
  uses it.
- The ASPX codec mode for stereo. Mono, VARVAR framing and interleaved waveform coding as options.
  E2 writes balance, VARVAR and frequency interleaving behind `experimental=`, and not time
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

**Exit:** the decoder's D7 selection and mixing on the encoder's streams give the configured
presentations, measured with one tone per substream; MediaInfo's trace lists presentations, names,
languages and levels as configured.

**Verified by:** `ac3tests`; the gain scripts; MediaInfo locally.

#### E7: the encoder's API, the CLI and packaging

- `EncoderConfig` and `Encoder` in their final form, and the function that wraps a frame in a sync
  frame.
- `ac3cli encode` options for layout, bit rate and rate control, frame rate, codec mode,
  presentations, loudness, DRC, dialogue enhancement, downmix, I-frames, CRC and the experimental
  tools.
- The encoder installed and exported beside the decoder, with an ABI allowlist and the package
  check; the documentation pages; the status table's encoder rows; CHANGELOG and a ROADMAP entry.

**Exit:** every CLI option has a test; the packages contain the encoder; the documentation gates
pass.

**Verified by:** `ac3tests` and the CLI tests on every leg; the package check; the documentation
gates.

#### E8: channel-based immersive

- The immersive element for 5.1.4 in SCPL, ASPX_SCPL and ASPX_ACPL_2, as DEE writes it; S-CPL's
  matrices; A-SPX's immersive pairing; the height downmix values.
- ASPX_ACPL_1, 7.1.4 with every channel present, and A-JCC, as options.

**Exit:** one tone per channel on its own channel; each channel meets the checks of E1 to E4 in full
decoding, and core decoding of the encoder's streams gives what D9 checks; the race at 5.1.4 from
192 to 768 kbps, scored in full and in core decoding.

**Verified by:** as E1.

#### E9: A-JOC objects

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

### Application phases

`ac3cli` grows with the library: D2 and D8 give it AC-4 decoding and media information, and E1 and
E7 give it AC-4 encoding, because the scripts that check each phase drive it. The phases below are
the rest of each application. They start when the channel-based library is complete (D8 and E7);
immersive and object content follows in I5
([decision 20](#decisions-for-the-encoder-and-the-applications)). Each updates the support catalogue
(`docs/assets/data/support-catalogue.json`, which generates `docs/library/application-coverage.md`
through `tools/checks/generate_support_matrices.py`).

#### I1: the rest of `ac3cli`

- `transcode` between AC-4 and AC-3 or E-AC-3 in both directions, through PCM, carrying what maps:
  dialnorm, the downmix levels, the DRC profile Part 1 5.7.9.4 names for a transcoder, and a
  presentation to a programme.
- `monitor` and `play` decode AC-4 live to `MonitorSink`.
- `qc`, `levels` and `loudness` read AC-4, so a stream's measured loudness is checked against its
  dialnorm as E-AC-3's is.
- `fmp4`, with its HLS and DASH output, and `mkv` take the encoder's output, under CMAF's rules;
  `probe` reads AC-4 inside MP4, TS and Matroska as well as raw.
- `spdif` and `unspdif` for AC-4, from D11; `record` and `live` encode AC-4 with `codec=ac4`.
- `ac3::plan::Codec` gains AC-4, and every helper that decides by codec becomes a switch that
  refuses a codec it does not know: today they are two-way ternaries, under which a third codec
  reads as E-AC-3 (`src/forge/include/ac3/encoder/plan.hpp`). The help topics' bitmask, which is
  full (`apps/cli/usage.hpp`), is widened.

**Exit:** every new option has a test; the codec matrix covers every AC-4 command, as
`tools/checks/check_matrix_coverage.py` requires; the man page and completions list them.

**Verified by:** `ac3tests`, the CLI tests and the codec matrix on every leg.

#### I2: Hearth desktop

- An AC-4 decoder in the engine's `StreamDecoder` shape (`apps/hearth/engine/stream_decoder.hpp`),
  rendering onto `render::OutputLayout`, and `Session::open` accepting AC-4, which today requires
  `io::scan` and so marks AC-4 items unplayable.
- `DecoderSettings` gains AC-4's controls: presentation, DRC decoder mode and output level shown
  apart from E-AC-3's ([decision 12](#decisions)), dialogue enhancement, the associated mix and the
  downmix. `DecoderAc4.qml`'s disabled cards are enabled and its "Not in this build" banner goes;
  media information comes from the decoder.
- AC-4 decodes to PCM for every output, and is sent as a bitstream only over the extension role,
  from D11 and Hearth's A4, to sinks that decode it.

**Exit:** the engine plays every committed AC-4 stream through the decoder's public API; each
control, driven from the page, changes the decoded output as its formula says, measured with tones;
the UI tests pass.

**Verified by:** the `[hearth]` tests; the QML tests; the gain scripts through the engine.

#### I3: Forge GUI

- AC-4 in the codec list, which maps index 1 to E-AC-3 and every other index to AC-3 today
  (`apps/gui/encoder_controller.cpp`), with its encode options and the command line the page echoes.
- AC-4 decode in the QC, object and stream player controllers, which dispatch on `stream_bsid`
  today.

**Exit:** each control has a test, and the command line the page echoes, run through `ac3cli`,
writes the same bytes as the page.

**Verified by:** the GUI tests.

#### I4: the C API, Python, Rust and WebAssembly

- The C API gains `ac3forge_ac4_*` decoder and encoder functions, with their own status range,
  embedding the AC-4 libraries as it embeds `ac3::forge`
  ([decision 21](#decisions-for-the-encoder-and-the-applications)); the Rust `-sys` crate's
  allowlist picks them up, and the safe crate wraps them.
- Python gains an `ac4` submodule in the present-or-absent pattern its optional modules use.
- WebAssembly gains an AC-4 module beside its decode and encode modules, and the JavaScript package
  a wrapper for it.
- The configurations D8 stopped compiling the AC-4 libraries now link them.

**Exit:** each binding's tests decode a committed stream and encode one that the decoder reads back;
the package checks pass.

**Verified by:** the binding tests on their CI legs.

#### I5: immersive and object content in the applications

After D9, E8, D10 and E9.

- `ac3cli`: encoding 5.1.4 and 7.1.4, and objects from ADM BWF and IAB as `atmos-adm` and
  `atmos-iab` do for E-AC-3; decoding objects to an ADM BWF master as `decode` does for E-AC-3 JOC;
  object metadata in `probe`.
- Hearth: immersive layouts through its renderer, objects through the renderer its phase A1 moved,
  and full or core decoding under its objects control.
- Forge GUI: AC-4 in its object pages.

**Exit and verified by:** as I1 to I3, for this content.

#### I6: the ESP32 sinks

With D12 and D13, once the user confirms them.

- The ESP-IDF component carries the inspector, the decoder and the core; `hearth_sink` decodes AC-4
  on the S3 in `float` and on the C6 in fixed point, and lists AC-4 among its Sendspin data types.

**Exit:** D12's and D13's board figures, and a sink playing an AC-4 stream Hearth sends it.

**Verified by:** the QEMU probes in CI; the boards.

Crucible takes no AC-4. It captures applications into Atmos objects for receivers, and no receiver
accepts AC-4 ([decision 20](#decisions-for-the-encoder-and-the-applications)).

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

   **Taken: (b), full and core decoding**, against the recommendation. Core decoding joins D9 and
   D10, and is the mode D13 tries first on the C6.

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

   **Taken: (b), buy IEC 61937-14 and implement it**, against the recommendation. Buying it is the
   user's step; D11 starts when the text is here.

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
    request branches from main and states its own exit criterion.

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

    **Taken: (a)**, as recommended; the user raised no objection.

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

    **Taken: (a)**, as recommended; the user raised no objection.

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

    **Taken: (a)**, as recommended; the user raised no objection.

20. **The applications, and their order.**
    - (a) **`ac3cli` with each library phase; once the channel-based library is complete, the rest
      of `ac3cli`, then Hearth desktop, then Forge GUI, then the C API with Python, Rust and
      WebAssembly; immersive and object content in each after D9 to E9; the ESP32 sinks with D12 and
      D13; and no AC-4 in Crucible.**
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

    **Taken: (a)**, as recommended. Whether the licence is renewed is not yet known.

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
| 23 | DEE's licence | G0 now either way | **G0 now**; renewal not yet known |

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| The decoder's output agrees with Dolby's decoder | **no** | No Dolby AC-4 decoder runs here and the Reference Player is not pursued (decision 5); at every frame rate but index 13 the converter is the implementer's choice |
| The decoder's output agrees with another decoder | **partly** | librempeg is experimental and partly derived from Emby's code, so agreement with it is evidence and disagreement is settled from the text |
| A-SPX, A-CPL and S-CPL reconstruct what the encoder intended | **partly** | Scored against the source; an error that stays close to the source passes |
| A-JCC, A-CPL mode 1, SSF, transmitted DRC gains, dialogue enhancement methods 1 to 3 | **syntax, and gains on known input** | No other encoder here writes them; constructed streams and the encoder's own share this project's readings |
| Presentations of several substreams as an encoder writes them | **partly** | DEE writes one presentation; streams multiplexed from DEE substreams stand in, and the encoder's own, which MediaInfo's trace reads |
| A-JOC objects | **partly** | One encoded stream and no source; object rendering is not normative |
| Absolute output level | **partly** | Part 1 states no full-scale convention; it is settled against DEE's source, which assumes DEE's input convention is the decoder's |
| Compression-curve DRC agrees with another decoder | **no** | The level detector is not normative |
| The readings chosen for the errata | **partly** | A reading is checked where an encoder's stream exercises the tool; elsewhere it rests on the text |
| Immersive stereo (`presentation_version` 2) | **by observation** | V1.3.1 names the version without defining it |
| Core decoding matches what a core decoder of Dolby's produces | **partly** | Checked against the text's matrices and gains on DEE and constructed streams; nothing here decodes the core otherwise |
| AC-4 over IEC 61937 to a device | **no** | No device found that accepts it; D11 is checked against IEC 61937-14's tables only |
| Real time on an ESP32-S3 in `float` and an ESP32-C6 in fixed point | **on boards, in D12 and D13** | QEMU has no cache model and a fabricated clock; the boards measure time |
| The encoder's streams decode as a Dolby decoder would decode them | **no** | No Dolby AC-4 decoder runs here; the decoder and librempeg stand in, and the decoder is checked on DEE's streams first |
| The encoder's syntax outside DEE's set is read as Dolby reads it | **partly** | MediaInfo's trace covers the table of contents, the presentation substream and `metadata()`; audio data has only librempeg, which is experimental |
| The encoder's quality against DEE's | **yes, through two decoders** | Both decoders' readings of the tools concerned are checked on DEE's own streams first |
| Objects the encoder writes render as Dolby's renderer would render them | **no** | Object rendering is not normative; listening stands in |
| Compression-curve DRC from the encoder's streams behaves alike in every decoder | **no** | The level detector is the decoder's own |
| Any DEE stream after 2026-11-06 | **no, unless the licence is renewed** | G0 makes the set before then (decision 23) |

## Coordination

- Branch names start with `feature/` or `bugfix/`, which CI's branch-name gate requires.
- Before each phase, `gh pr list` and `ListAgents`: the session that planned the decoder, "04. Plan
  and build an AC-4 decoder", spawned the one that extended this page, and more sessions may take
  phases.
- Hearth: phase A0 drew the AC-4 pages with dynamic range and output level separate from E-AC-3's
  (decision 12); I2 enables them through A3's engine; A4's extension page gains the AC-4 data type
  in D11; A1 moves the renderer that D10 and I5 hand objects to; C1's bring-up of the C6 comes
  before D13 and I6.
- Files other chips edit: the root `CMakeLists.txt`, `tests/CMakeLists.txt`, `fuzz/CMakeLists.txt`,
  `cmake/InstallLibrary.cmake`, `tools/ci/classify_changes.py`, `CHANGELOG.md`,
  `docs/verification.md`, `tools/checks/check_doc_paths.py`; and, from the encoder and application
  phases, `tools/ci/quality_race.py`, `tools/checks/verify_gold_reference.sh`,
  `tools/ci/run_codec_matrix.sh`, `.github/workflows/_ci-core.yml` (FFmpeg Validate),
  `apps/cli/main.cpp`'s command table, `ac3::plan::Codec`,
  `docs/assets/data/support-catalogue.json`, `src/capi/`, `python/`, `rust/` and `apps/wasm/`.
- `tools/ci/classify_changes.py` maps no path to AC-4, and `tools/generators/` and
  `tools/references/` match no lane, so every lane runs when they change; the first encoder phase
  gives AC-4 a lane.
- librempeg's binary is named `ffmpeg`; nothing puts it on `PATH` ([The oracles](#the-oracles)).
- D11's change sits in `ac3::iec61937`, inside `ac3::forge`, beside the passthrough work of
  Hearth's A3.

## Deliberately not in scope

- Transcoding without decoding, from AC-4 to AC-3 or E-AC-3 or back; `ac3cli transcode` goes through
  PCM (I1), carrying the DRC profile Part 1 5.7.9.4 names for a transcoder.
- Linking FFmpeg, librempeg or any codec library, or reading another decoder's or encoder's source.
- Rendering objects inside the decoder, headphone virtualisation and head tracking.
- The HSF extension for 96 and 192 kHz beyond its syntax: a 48 kHz decoder may ignore it (Part 1
  4.2.4.3, 5.4), the converter provides 96 and 192 kHz output, and the encoder takes 96 and 192 kHz
  input only after converting it to 48 kHz.
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
- AC-4 on the ESP32-C3 and the other ESP32 parts; D12 and D13 cover the S3 and the C6. The encoder
  on any ESP32.
- AC-4 in Crucible (decision 20).
