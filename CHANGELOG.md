# Changelog

*For end users tracking what has shipped. How releases and version numbers are cut lives in
[docs/releasing.md](docs/releasing.md); the project overview is in [README.md](README.md).*

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [docs/releasing.md](docs/releasing.md) for how releases and version numbers are cut.

## [Unreleased]

### Added

- **Third-party Atmos streams decode** (roadmap `DC6`). The object layer used to recognise only
  the shapes this project's own encoder writes and refuse everything else, which is most of what
  real content carries. OAMD now reads any number of metadata update blocks at any sample offset
  and ramp duration, object size, zone constraints, elevation gating, channel lock, screen
  reference, distance, explicit priority and gain reuse, positions coded differentially against
  the previous block, inactive objects, several bed instances (standard or non-standard), bed
  channel distribution, programmes carrying an intermediate spatial format, alternate object
  data, the `trim_element` and the `extended_object_element` — and an `oa_element` whose id it
  does not know is now skipped by its own size and reported on `DecodedProgram::skipped_elements`
  rather than costing the whole payload. JOC reads all five of Table 47's downmix configurations,
  any clip gain, and per-object band count, quantizer, sparse-or-whole-matrix mode, interpolation
  slope and data-point count; `joc::reconstruct` implements the whole of §6.6.5, keeping
  `joc_mix_mtx_prev` per QMF subband as the clause does. The EMDF reader parses the whole of
  §H.2.1.3's payload configuration onto `DecodedPayload::config` instead of insisting on TS 103
  420 Table 56's one shape, and handles the payload-id extension escape.
- **JOC reconstruction for bed programmes.** Object audio was only reconstructed for a
  dynamic-object-only programme — the one shape `AtmosEncoder` writes. It now covers bed
  programmes too, which is what channel-based-immersive third-party content is, so a 7.1.4 bed
  carried in a 5.1 downmix exports its eleven non-LFE channels. `DecodedSubstream::object_indices`
  says which programme object each `object_audio` entry is, `oba::bed_labels()` turns a bed
  channel into a speaker label, and `ac3cli decode`'s report and `objects_dir` export, the GUI
  object inspector and the WASM demo all follow.
- **A committed third-party fixture.** `tests/golden/object-fixture/dee_joc_514.ec3` is a DD+ JOC
  stream produced by the Dolby Encoding Engine from a synthetic 5.1.4 tone bed
  (`tools/generators/gen_object_fixture.py`, local-only — DEE is licensed and never runs in CI).
  It is the only Atmos stream here this project's encoder did not make. Because each source
  channel carries a different tone, identifying each reconstructed object by which tone dominates
  it independently confirms both the reconstruction and the order a bed's channels occupy — the
  order TS 103 420 §5.6.1.1.4 states backwards.
- **Object extent, channel lock and zone constraints on the encode side** (roadmap `DC7`).
  `ac3::oba::ObjectPlacement` and `Keyframe` carry §5.6.1's `size` (width/depth/height),
  `snap`, `zone` and `enable_elevation`; `oba::build_payload` writes all four, and `KeyframePath`
  interpolates size between keyframes while holding the three discrete flags. The ADM bridge maps
  BS.2076-2 `width`/`height`/`depth` onto `ObjectSize` and `channelLock` onto `snap`; `diffuse`,
  `zoneExclusion` and `objectDivergence` remain unmapped, and `docs/library/adm-bridge.md` now
  gives each one its own reason rather than one blanket paragraph.
- **SIMD kernels, selected by CMake rather than by `#ifdef`** (roadmap `PF5`). The codec's hot
  kernels now run through 128-bit vector types supplied by one of
  `src/forge/src/internal/arch/{generic,x86_64,aarch64}/`, each carrying an identically-pathed
  `ac3/internal/arch/simd.hpp` that `src/forge/CMakeLists.txt` puts on the include path — the same
  mechanism the profiling seam and the audio backend tree already use, and the reason no
  translation unit in the codec has to ask what it is being compiled for. `AC3FORGE_SIMD` forces a
  directory (`generic` is a complete scalar implementation and what a reproducibility comparison
  should reach for); the resolved value appears in the configure summary and in
  `ac3cli --version`. Vectorised: the DCT-IV pre/post twiddles every fast MDCT and fast IMDCT is
  built on, analysis windowing, both inverses' twiddle stages, `dft512`'s normalisation,
  §7.2.2.2's exponent-to-PSD conversion, and a batched `to_fixed25`. The FFT/DCT-IV core itself
  (`fft_kernel.hpp`) is `PF4`'s own radix-4 restructuring, an algorithmic change rather than a
  wider-lane one, and is not part of this seam; the pre/post-twiddle loops around it were
  adapted to gather from and scatter to that kernel's digit-reversed layout rather than to
  sequential slots, which is why the scatter/gather ends of those loops stay scalar and only the
  arithmetic between them is vectorised.
  Only SSE2 and base ARMv8-A Advanced SIMD are used — both part of their architecture rather than
  optional features — so there is no `-march=` flag and no runtime dispatch, and 128 bits is the
  native width of the platforms this was done for anyway (Raspberry Pi, the Shield's Tegra X1,
  WASM). **Encoded output is unchanged, bit for bit**: every seam operation is exactly one
  IEEE-754 add, subtract or multiply per lane, `tests/core/test_simd_kernels.cpp` holds each
  primitive to bit-for-bit equality with a scalar reference in the same binary (the kernels built
  from them are composition, not new arithmetic, so they inherit rather than need their own
  bit-exact test), and the full `run_codec_matrix.sh` corpus — 93 streams, 272 output files across
  every layout, Annex E tool token and metadata option — hashes identically between this build, a
  `-DAC3FORGE_SIMD=generic` build, and the previous release. Decoded audio is likewise
  bit-identical, which is a stronger guarantee than the fast-IMDCT work's own 7.8e-14 / 215–285 dB
  standard. See [docs/building.md](docs/building.md).
- **The encoder/decoder mirror self-check now covers E-AC-3** (`ac3::verify`, roadmap `VX2`).
  The AC-3 half has decoded every frame the encoder emitted and diffed the decoder's model
  against the encoder's own since 0.7.0; Annex E was explicitly out of scope, because its
  dependent-substream and transient-pre-noise machinery needed its own instrumentation design.
  It has one now. `Eac3MirrorEncoder` compares, per substream of an access unit and per block:
  the bit offset at each block boundary, the decoded exponents, `bap`, the delta correction in
  force, the adaptive-hybrid-transform gain mode and its per-bin gains, and the coupling,
  enhanced-coupling and spectral-extension coordinates — across an independent substream and
  both of its dependents at 7.1.4. The `§3.7` hold-back turns out not to need special handling:
  the trace is written while a frame is parsed rather than when its audio is released, so a
  held-back frame is compared in the call that decoded it like any other.

  This matters more for E-AC-3 than it did for AC-3 because Annex E has weaker oracles, not
  stronger ones: `docs/verification.md` records that 7.1.4 has no external oracle at all, that
  enhanced coupling and transient pre-noise processing have none "not even the partial one 7.1.4
  gets", and that `fscod2` audio is refused by FFmpeg *and* by Dolby's own Reference Player. For
  those, the in-repo round trip was the only check there was. What the mirror adds over it is the
  case where the two sides differ but the audio survives — a gain one side recovered differently,
  a coordinate quantized against a different band structure — which a round trip passes and a
  third-party decoder would nevertheless render differently. What it still cannot see is a
  misreading the two sides make identically, in code they share; `docs/verification.md` is
  explicit about that residue rather than claiming the gap is closed.

  Off by default and free when off, exactly as the AC-3 half is: `eac3::FrameConfig::trace` and
  `DecoderConfig::eac3_trace` are null pointers, costing one branch per block and no allocation,
  and attaching one never changes a single emitted bit. `ac3cli eac3-encode … verify` runs it
  over a whole file and refuses the run at the first disagreement, naming the substream, block,
  coded stream and bin; `tools/ci/run_codec_matrix.sh` runs it on the sanitizer leg over the
  whole Annex E tool matrix, every layout including 7.1.4, all three `fscod2` rates and VBR. It
  found no disagreement on any stream this encoder currently produces.
- **QMF-domain JOC** (roadmap DC10). TS 103 420 puts the object reconstruction in a 64-subband
  complex QMF; this tree had no filterbank, and estimated and applied the matrix over 256 MDCT
  bins instead. `ac3::dsp::QmfAnalysis` / `QmfSynthesis` (`ac3/dsp/qmf.hpp`) is that filterbank
  — 640-tap prototype designed in-tree for exact perfect reconstruction, a 128-point FFT on the
  same radix-2 core the fast MDCT already uses. `joc::Domain` selects where the matrix is
  estimated (`AtmosConfig::joc_domain`) and applied (`DecoderConfig::joc_domain`), and the CLI
  spells it `joc-domain=qmf|mdct` on the `atmos*` commands and `decode`.
- **An object-reconstruction quality series** (`VX8`). Object reconstruction — the object layer
  that every other codec layer here has a per-commit trend for — was measured exactly once
  anywhere in the tree: a single `snr_db > 10.0` assertion in `tests/oba/test_atmos.cpp` against
  18–35 dB measured, so a 15 dB JOC regression passed CI and appeared on no page.
  `tools/ci/quality_race.py` grows an `objects` mode that encodes a committed five-object Atmos
  scene (`tests/golden/audio/reference_objects.wav` plus the placements beside it, both
  generated by `tools/generators/gen_object_scene_wav.py`) with `atmos-encode`, decodes it back
  to per-object WAVs, and scores each object against the channel it came from at the 512-sample
  transform delay the unit test derives. `ffmpeg-validate` computes it every run and a new
  `persist-object-quality-trend` job appends it to the `quality-history` branch, rendered on
  [Object quality trend](docs/object-quality-trend.md). LSD needed an object-specific form:
  objects are individually narrow-band, so the codec legs' 24-band measure reads 10–38 dB for a
  *healthy* reconstruction; it is now restricted to the bands an object occupies, with what
  landed outside them reported as a separate leakage figure — the object-specific failure mode.
  There is no external oracle for object decode at all (FFmpeg implements no JOC reconstruction,
  and Dolby's own decoder gates it on a key this project does not ship), so the series is
  self-consistency throughout and the page says so.
- **Listening-test apparatus** (`VX9`, partial). `tools/listening/gen_listening_stimuli.py`
  builds a blind stimulus set over the three landscape legs — hidden reference, BS.1534-3's 3.5
  and 7 kHz anchors, and one arm per encoder — with every stimulus decoded by FFmpeg so the
  decoder is a constant rather than a variable, and `score_listening_test.py` reads the answers
  back as MUSHRA means with 95% confidence intervals (after BS.1534-3 post-screening) or ABX
  proportions with Wilson intervals and an exact binomial p. The protocol is on
  [Landscape](docs/landscape.md#listening-test). **No session has been run**, so the results
  table there is empty and README's and Validation's "no listening test has been run" sentences
  are unchanged. Building the apparatus found two preconditions the committed material does not
  meet, now detected and recorded per session: `reference_51.wav` carries 0.059% of its energy
  above 3.5 kHz, so both BS.1534 anchors are inaudible on both 5.1 legs and cannot scale a
  MUSHRA session there, and the items are 1.9 s against BS.1534-3's ~10 s. Roadmap `VX7` (real
  programme material) is what fixes both.
- **`ac3::quality`: decoded-domain distortion measurement and a psychoacoustic model** (`EQ13`).
  `encoder.cpp` has recorded, since the dbpbcod/exponent-strategy work, that a per-frame search
  over the transmitted bit allocation parameters was tried twice and rejected both times: the
  only in-loop criterion available was the composite SNR offset, and that number is not
  comparable between two candidates that produce different masking curves. `ac3::quality`
  supplies the missing criterion - the error a decoder will actually reconstruct, computed
  without decoding by evaluating §7.3's quantizers in closed form on the encoder's own
  coefficients, decoded exponents and bit allocation (pinned bit-exact against the real
  quantize/dequantize pair over the whole mantissa range) - plus a tonality/masking model
  (Johnston's perceptual entropy, MPEG-1 model 2's unpredictability-based tonality, Schroeder/
  Zwicker-Terhardt spreading, a measured-and-capped absolute threshold) that prices what the
  first measure finds against what the signal can actually hide.
  See [docs/library/quality.md](docs/library/quality.md).
- **`EncoderConfig::search`**: a per-frame search over `dbpbcod`/`fgaincod` (six candidates,
  including the no-search defaults), judged by the measure above and gated by real hysteresis
  (the incumbent is the previous frame's winner, not a fixed baseline, so two near-equal
  candidates don't retrigger every frame). `kNone` by default; `kDistortion` and `kPerceptual`
  turn it on (`ac3cli encode ... search=distortion|perceptual`). AC-3 only - E-AC-3's `bamode=0`
  pins the same parameters and needs `EQ3`'s syntax work first.
  Validated on CC0/CC-BY programme material (not the checked-in band-limited fixtures) against
  FFmpeg's decode, scored by SNR/log-spectral distance/ViSQOL MOS-LQO, against the no-search
  baseline as it stood before `fgaincod_for`'s rate-adaptive curve landed alongside this
  (re-measuring against that curve is a follow-up): `kDistortion` is a real,
  repeatable win from 448 kbit/s up (SNR +0.4 to +0.8 dB, LSD and MOS improved on every material
  tested); at 192 kbit/s its own criterion still improves but trades SNR against per-band spectral
  shape, and that trade currently costs LSD and MOS more than it buys back. `kPerceptual`
  currently loses at every rate tested - real evidence that its psychoacoustic model, though
  validated in isolation (discriminates tone/noise/transient correctly, calibrated to this
  project's own transform), is not yet calibrated well enough to beat a well-tuned fixed default
  on real stereo material with rematrixing active. Both stay off by default. Validated in stereo
  only - the search's mechanism is proven correct at 3/2+LFE by the mirror self-check, but
  external-metric validation on real 5.1 material hit a measurement-harness alignment problem this
  round ran out of time to resolve; left for a follow-up. See
  [docs/library/encoding-ac3.md § Decision search](docs/library/encoding-ac3.md#decision-search)
  for the full table and the reproduction command.
- **A threat model for untrusted input** (`docs/threat-model.md`, roadmap `VX19`). What is
  treated as adversary-controlled and what is not, the memory-safety posture of the C++23 core,
  the three raw-pointer boundaries (the C API, the WASM bindings, the JNI bridge) and the
  contract each leaves to the caller, and every per-access-unit resource limit tabulated with the
  bitstream field width it comes from — including what a hostile `frmsiz` actually does. The
  limits that are *not* enforced are stated as gaps rather than omitted: there is no cap on
  stream length (the split APIs take the whole stream as one span, so memory is O(input)), no
  decode time bound, and no fuzz coverage of the opt-in ADM/BW64 path, which uses vendored
  third-party parsers. Cross-referenced from `SECURITY.md`, `README.md` and
  `docs/library/decoding.md`.
- **Published conformance vectors** (`docs/conformance-vectors.md`, roadmap `VX20`). Every
  release now carries `ac3forge-conformance-vectors-<version>.tar.gz`: 60 streams — AC-3 in every
  coding mode, E-AC-3 at every layout and with each Annex E tool, both VBR shapes, every sample
  rate including the `fscod2` half rates, and Atmos with objects — each with the PCM it was
  encoded from, its hashes, decoded per-channel levels, and a sentence naming the syntax it
  exercises. The manifest's FFmpeg-readability column is derived from
  `docs/verification.md`'s oracle table rather than typed in, so it cannot drift out of step with
  it. Generated by `tools/generators/gen_conformance_vectors.py`, which runs on the linux-gcc CI
  leg on every push and additionally re-generates and diffs the whole bundle at release time;
  the archive is written with timestamps and ownership pinned so it is byte-reproducible. Hashes
  are per-toolchain (encoded output is not yet bit-identical across compilers or architectures —
  roadmap `VX11`/`VX12`) and the source material is synthetic (roadmap `VX7`); both are stated in
  the bundle. Nothing produced by Dolby or FFmpeg is redistributed, and no stream is signed
  unless an operator supplies a key.

### Fixed

- **`audblk` skipped `cplfgaincod` and `cplfsnroffst`.** A/52 Annex E reads both ahead of the
  per-channel lists when the block couples, and the decoder read only the per-channel ones, so a
  stream that sets `frmfgaincode` or `snroffststr` 2 alongside coupling desynchronised three bits
  later and failed on the next block's exponents. No stream this project produces was affected —
  its encoder writes `frmfgaincode` 0 and `snroffststr` 0 — which is why only a real Dolby stream
  exposed it.

### Added

- **`ac3::oba::ObjectScene`, one object-scene timeline shared by every front end**
  (`ac3/oba/scene.hpp`, roadmap `IM7`). `AtmosEncoder` takes per-frame placements and nothing
  more, so `ac3cli atmos-path`, the GUI's timeline export and the station-broadcast example each
  built their own scene description. `ObjectScene` is the one they now share: named objects with
  a bed assignment, position/gain automation with per-segment interpolation (`hold`, `linear`,
  `smooth`) and ends-hold ramp semantics stated in the type, an `Orientation` that rotates a
  scene's positions as metadata before encode (never a render — a room-corrected render stays
  out of scope), and a JSON serialised form. `SceneCursor` is the live half: the authored
  timeline with per-object overrides an external source pushes in, the seam a live OSC/MIDI/
  controller source (`UX4`) lands on. JSON rather than YAML because RFC 8259 is small enough to
  implement completely in-tree where YAML 1.2 is not — {fmt} (below) formats a number, it does
  not parse or write either file format.
- **`atmos-path` and `atmos-encode` read a JSON scene as well as the keyframe columns**, told
  apart by whether the file's first non-whitespace character is `{` rather than by its suffix, so
  either form works wherever the other does. The keyframe grammar itself is unchanged, moved into
  the library so the CLI, the GUI's export and the examples share one reader and one writer; a
  file in it encodes to a byte-identical stream before and after the move, and to the same stream
  again after a JSON save/load round trip. Its only changed diagnostic is the duplicate-timestamp
  one, which now names the file and the instant.
- **The GUI's "Export paths…" writes either form**, chosen by the name saved under: a `.json`
  name writes the scene, anything else the keyframe columns it has always written.
- **Matroska container reader** (roadmap `IO2`, first of three). `matroska::demux` and
  `matroska::Reader` (`matroska/reader.hpp`) are the read side of `matroska::mux`/`Writer`, and
  codec-blind in the same way — they walk EBML, select a track and hand each frame back as
  opaque bytes. `demux` is batch and zero-copy (frames are spans into the caller's buffer);
  `Reader` is incremental, delivering frames through a callback so peak memory is one chunk plus
  one frame rather than the file. Both read shapes this project's own writer never emits,
  because a disc rip or another muxer does: all three lacing forms, `BlockGroup`-wrapped
  `Block`s, several tracks, 32-bit `SamplingFrequency`, and unknown-size clusters as well as
  unknown-size segments. A file truncated mid-cluster returns every whole frame before the cut.
  The EBML element ids now live in one shared `src/matroska/src/ebml_detail.hpp` that both sides
  read, so the reader cannot drift from the writer.
- **`ac3cli demux`** — the inverse of `ac3cli mkv`: unwraps the elementary stream a container
  carries, which is what every other command takes as input. The container is identified by its
  own magic bytes rather than by the file name, and the whole path streams, so a multi-gigabyte
  rip never lands in memory. Matroska/WebM in this release.
- **MP4 container reader** (roadmap `IO2`, second of three). `mp4::demux` and `mp4::Reader`
  (`mp4/reader.hpp`) read both layouts the writers produce and both a real muxer does: a plain
  `moov`/`mdat` file, walking `stsc`/`stsz`/`stco` (and the `stz2`/`co64` variants this project
  never writes) into sample byte ranges, and a fragmented one, taking `mvex`/`trex`'s defaults
  plus every `moof`/`traf`/`tfhd`/`trun`. 64-bit `largesize` headers and an `mdat` declared to
  run to end-of-file read normally. `demux` reads a `moov`-last file; `Reader` reports
  `kMoovAfterMdat` for it rather than guessing, since locating a sample means seeking backwards.
- **`dec3`/`dac3` parsing** — `ReadTrack::codec_config` is the read twin of
  `ac3::io::build_codec_config_box`, TS 103 420's `flag_ec3_extension_type_a`/
  `complexity_index_type_a` included. That is the Atmos/JOC marker an FFmpeg remux is known to
  drop, so reading it back is what makes the repair case possible. A box that stops before the
  extension leaves the field empty rather than reporting a confident zero.
- **`ac3cli demux` reads MP4** as well as Matroska, still by magic bytes rather than file name,
  and `fuzz_mp4_demux` joins the harness set.
- **MPEG-TS container reader** (roadmap `IO2`, third of three — all readers now land).
  `mpegts::demux` and `mpegts::Reader` (`mpegts/reader.hpp`) lock to the packet grid (188 bytes,
  M2TS's 192, or 204 with kept Reed-Solomon parity — detected, not assumed), follow PAT to PMT
  to an elementary PID and reassemble PES. Unlike the sibling readers this hands back **PES
  payloads**, not access units — a PES packet makes no such promise, and what the payloads
  concatenate to is the elementary stream `ac3::io::scan` re-frames. The writer only ever speaks
  DVB (`stream_type` 0x06 plus a descriptor); the reader recognises DVB, ATSC's own
  `stream_type` 0x81/0x87, and a `registration_descriptor`'s `'AC-3'`/`'EAC3'` identifier alike,
  reported as `ReadStream::signalling`, since a reader has to open whatever arrives. Every PSI
  section's CRC-32 is checked before it is believed — a transport stream is designed to survive
  bit errors, so a damaged PMT is the ordinary case, not the exceptional one, and believing one
  would mean locking onto the wrong PID for the rest of the file. A capture that starts
  mid-packet (the normal way one is acquired) still locks onto the grid.
- **`ac3cli demux` reads MPEG-TS** as well as Matroska and MP4, identified by its packet grid
  rather than magic bytes (a transport stream has none) or file name, and `fuzz_mpegts_demux`
  joins the harness set — the container reader most likely to find a genuine hang rather than a
  crash, since PSI and PES reassembly are loops a hostile stream can try to stall.
- **`fuzz_matroska_demux`** — a libFuzzer harness over the EBML walk, driving both `demux` and
  the chunk-boundary state machine in `Reader::push` with arbitrary bytes. Container parsing is
  untrusted-input territory (every length in an EBML file is self-declared), so
  `matroska_objects` is now instrumented with ASan/UBSan/coverage in fuzz builds the same way
  `forge_objects` already was.
- **Loudness of the rendered layout** (`IO10`). `ac3::meta::LoudnessMeter` has a second
  constructor taking an `eac3::chanmap::Layout`, which measures through **ITU-R BS.1770-5
  (11/2023) Annex 3**'s extended algorithm for advanced sound systems instead of Annex 1's basic
  one. Annex 3's Table 4 weights a channel by where it sits — 1.41 (+1.5 dB) between 60° and 120°
  azimuth below 30° elevation, 1.00 everywhere else, LFE-type channels excluded — rather than by
  its slot in a Table 5.8 `acmod`, so `Lrs`/`Rrs`, `Vhl`/`Vhr`, `Lts`/`Rts`, `Cs` and `Lw`/`Rw`
  all have a defined weight and 7.1, 5.1.2, 5.1.4 and 7.1.4 can be metered at all. A new
  `ac3::meta::position_weight()` exposes the per-location weight on its own. Two results are
  worth knowing because the channel names suggest otherwise: a 7.1 layout's rear pair is **not**
  surround-weighted (M±135 is past the sector's edge), and neither is any height channel.
- **`ac3cli qc layout=bed|rendered`** (`IO10`). `bed` — the default, and what `qc` has always
  measured — meters the independent substream's own Table 5.8 bed. `rendered` meters the whole
  assembled program from `Eac3Decoder::decode_access_unit`, every dependent substream's height,
  wide and rear channels included. For a plain 5.1 stream the two agree by construction. `bed`
  now prints a note when a stream carries dependent substreams whose channels it left out,
  instead of reporting the bed as though it were the whole programme.
- **Two new QC presets** (`IO11`): `atsc-a85-streaming`, from ATSC A/85:2026-07's new Annex L.5
  (a −23…−27 LKFS band for streaming services, with a −2 dBTP ceiling), and `apple-music-atmos`,
  from Apple's Immersive Audio Source Profile (integrated loudness **≤ −18 LKFS** and true peak
  ≤ −1 dBTP, both per BS.1770-4). Every preset now also records the document version and date it
  was read out of, and `ac3cli qc` prints that beside each verdict.
- Roadmap item `IO12`, for BS.1770-5 Annex 4's object-based loudness algorithm — the half of
  BS.1770-5 that `IO10` deliberately left out.

### Fixed

- **`ac3cli atmos ... bed51` no longer advertises an object layer it deliberately did not
  encode.** `AtmosConfig::emit_object_metadata` decided whether the EMDF container (OAMD + JOC)
  was written, but `AtmosEncoder` set TS 103 420 §8.3.1's `addbsi` object marker
  (`flag_ec3_extension_type_a` plus §8.3.2.2's `complexity_index_type_a`) unconditionally. That
  marker is the only thing any reader has to go on, so a `bed51` stream — whose whole purpose is
  to omit the container and play as a plain 5.1 bed on a decoder that would otherwise refuse an
  object container it cannot validate — still claimed objects downstream: `ac3::io::scan`
  reported an `oba_complexity_index`, `ac3::io::build_codec_config_box` wrote the `dec3` box's
  Dolby Atmos extension, `ac3cli fmp4` wrote `CHANNELS="<N>/JOC"` into its HLS playlists, and
  FFmpeg reported the profile as "Dolby Digital Plus + Dolby Atmos". The marker now follows the
  container, which is the same objects-or-nothing rule that already ruled out emitting an empty
  container. This changes `bed51` output bytes: the `addbsi` element shrinks to a single zero
  `addbsie` bit and the freed bits go back to the mantissas (the frame size is unchanged — this
  is CBR). `objects` mode is unaffected. The FFmpeg-oracle matrix now asserts the profile for
  both modes rather than only that each decodes.

### Changed

- **`atsc-a85` re-cited to ATSC A/85:2026-07** (`IO11`), approved 8 July 2026 — the first full
  revision of A/85 since 2013. Its §6 restates the target loudness, tolerance and true-peak
  ceiling unchanged (−24 LKFS, ±2 dB, −2 dBTP), so **no preset number moves**; only the citation
  does. EBU R 128 s4, Netflix's Dolby Atmos Home Mix Deliverable Requirements v2.3 and Amazon
  were each checked against their primary sources and deliberately *not* added as presets: the
  first two are numerically identical to presets already in the table (s4 differs only by a
  Loudness-to-Dialogue Ratio this meter cannot measure, and the Atmos spec only by asking for a
  5.1 re-render, which is a `layout=` choice), and no primary Amazon document was reachable to
  cite. `ac3/meta/qc.hpp` and `docs/cli/metadata-options.md` record the reasoning for each.
- `QcPreset` distinguishes a loudness **band** from a loudness **ceiling**
  (`QcLoudnessLimit`). Every preset before now stated a target with a symmetric tolerance; Apple's
  states only a level not to exceed, and gating that as a ±band would fail quiet material the
  specification actually accepts.

### Fixed

- **`dialnorm=auto` and `ac3cli loudness` mis-assigned channel weights on any layout wider than
  stereo.** `measured_dialnorm()` pushed a WAV's channels into `LoudnessMeter` in *WAV* order
  (FL, FR, FC, LFE, BL, BR) when the meter expects AC-3 *coded* order (L, C, R, Ls, Rs, LFE). For
  5.1 that put the LFE where `Ls` belongs — so BS.1770's +1.5 dB surround weight landed on the
  one channel the standard excludes outright, while a real surround landed in the excluded slot
  and was dropped entirely. Found by cross-checking against ffmpeg's `ebur128`: with signal in
  only the LFE channel, ac3forge reported −38.61 LKFS where the oracle correctly reported no
  loudness at all. `ac3cli levels` already applied the right permutation, which is why it never
  showed the fault. Measured `dialnorm` values for 3-channel-and-wider sources change as a
  result, and are now correct.

### Added

- **`ac3kernelbench` covers the fast transform paths and `dft512`.** The kernel series benched
  the direct IMDCT and not the fast one that has been the decode default since 0.9.0, and never
  benched `dft512` or the block-switched inverse at all. Four rows added
  (`imdct512_windowed_fast`, `imdct256_pair_windowed` and its fast form — the only user of the
  FFT kernel at P = 64 — and `dft512`) plus `ecpl_channel_spectrum_fast`, so each accelerated
  kernel is recorded beside its own reference form. Part of `PF1`'s decode-side gap, added here
  because `PF3`/`PF4` needed the numbers.
- **`dft512` is checked against its own O(N²) summation.** `tests/core/test_fft.cpp` had
  impulse, DC, bin-aligned-cosine and linearity properties but never compared the transform to
  the sum `fft.hpp` states it computes. Two cases now do, on six consecutive real-audio-shaped
  blocks and on genuinely complex input (what §E3.5.5.1 step 5 actually hands it), at the same
  1e-10 bound the rest of the library's fast paths are held to.
- **Streaming fMP4/CMAF fragmenter** (ROADMAP `IO4`). `mp4::FragmentWriter` is the incremental
  counterpart to `mp4::fragment`, the sibling `matroska::Writer` and `mpegts::Writer` already
  had: an initialization segment up front, then one CMAF media segment handed back each time a
  fragment closes, with `tfdt` from a running decode time. Its contract is the one
  `mpegts::Writer` already holds itself to - for the same track, options and frames the media
  segments are byte-identical to the batch form's - and the initialization segment differs in
  exactly one respect, `mvhd`/`tkhd`/`mdhd` duration 0 for a session that cannot know its own
  total, the same concession `matroska::Writer` makes with EBML's unknown-size Segment. A
  `SegmentInfo` window (`FragmentOptions::playlist_window_segments`) keeps a rolling live HLS
  playlist and DASH `SegmentTimeline` without keeping the audio, and `mp4::build_dash_mpd` -
  moved into the library from the CLI - grows a dynamic form with `availabilityStartTime`,
  `minimumUpdatePeriod` and `timeShiftBufferDepth`.
- **fMP4/CMAF as a live container** in both front ends. `ac3cli record`/`ac3cli live` take
  `container=fmp4`, which makes the output path a directory written as the session runs -
  `init.mp4`, a `segment*.m4s` per closed fragment, and playlists and MPD rewritten alongside,
  live-shaped while running and closed to VOD/static at the end - plus `fmp4-window=<n>` for a
  rolling origin. The GUI records the same way with **fragmented MP4/CMAF** selected, in both a
  live session and a recording, where before a live session fell back to writing the plain
  elementary stream and a recording accumulated the whole take before writing it. fMP4 joins
  Matroska as the second container a live session can write natively; S/PDIF, MP4 and MPEG-TS
  still fall back there.
- **DASH signalling for Dolby Atmos/JOC, and the `ceao` brand** (ROADMAP `IO5`).
  `mp4/dash.hpp` said there was no established convention to point at; DASH-IF IOP Part 8
  v5.0.0 §5.3.2 names the two supplemental descriptors ETSI TS 103 420 clause D.2 defines
  (`tag:dolby.com,2018:dash:EC3_ExtensionType:2018` with the value `JOC`, and
  `…EC3_ExtensionComplexityIndex:2018` with `complexity_index_type_a`), and §5.3.3 the `ceao`
  compatibility brand that spec's Annex E requires on an object-audio CMAF track.
  `ac3cli fmp4`, the GUI and both live paths now write all three. Every Representation also
  states its channel configuration, on either the ISO/IEC 23091-3 CICP scheme or - via the new
  `ac3::io::dash_channel_configuration`, from the channel-location word `ac3::io::scan` already
  computed and used to discard - the Dolby scheme ETSI TS 102 366 clause I.1.2.1 defines.
- **IEC 61937 de-framing, and passthrough capture** (roadmap `IO3`). The burst wrapper was
  byte-exact against FFmpeg's `spdif` muxer, but nothing in the project ever read a burst back:
  there was no round-trip test for it, and no way to recover a stream from a capture of a
  player's S/PDIF or HDMI output. `ac3::iec61937::BurstReader` now parses the `Pa`/`Pb`/`Pc`/`Pd`
  framing — data types 0x01 (AC-3) and 0x15 (E-AC-3), E-AC-3's 4× carrier and its multi-syncframe
  bursts, the stuffing between bursts, `Pd`'s two different units, and both 16-bit word orders —
  streaming, holding one burst plus the caller's chunk however long the capture runs.
  `unwrap_stream` is the batch form. A new `ac3cli unspdif <in.wav|in.raw> <out.ac3|out.ec3>`
  exposes it, reading the WAV `spdif` writes, a saved capture, or a bare dump of carrier bytes.
  Bursts written by this project *and* by FFmpeg's own muxer read back byte-exactly to the
  streams that went in, for both data types and both word orders.

  On the capture side, `ac3::iec61937::PassthroughDetector` answers whether an endpoint is
  delivering PCM or somebody else's bursts, from the same interleaved floats `ac3::audio::Capture`
  hands over. `ac3cli record` acts on it by writing the elementary stream instead of encoding the
  bursts as audio — nothing re-encoded, output bit-identical to what the source sent — and no
  longer refuses a device whose sample rate AC-3 cannot encode at until it has ruled a bitstream
  out, since 192 kHz is exactly the E-AC-3 carrier's 4×. `ac3cli live` detects the same thing and
  stops with an error naming `record` and `unspdif`, rather than encoding a whole session of
  noise. None of this is hardware-confirmed: no HDMI or S/PDIF capture device has been available,
  the same gap the passthrough output side has.

  The parser treats its input as hostile throughout — a burst carrier comes off a wire by
  definition — so a `Pd` past its data type's repetition period is refused rather than allocated,
  and a preamble not backed by a `0x0B77` syncframe is resynced past. `fuzz/fuzz_iec61937_unwrap.cpp`
  is the new libFuzzer harness over it.

- **`ac3cli probe`** (roadmap `IO1`) — what an elementary stream *declares*, without
  reconstructing its audio: `bsid`, sample rate including Annex E's `fscod2` half rates,
  `acmod`/`lfeon` and the resolved layout, `bsmod`, `chanmap`, the substream map
  (independent/dependent, ids), `numblkscod`, frame and access-unit counts, duration, measured
  bit rate with the VBR spread behind it, `dialnorm`/`compr`/`dynrng` presence and ranges, EMDF
  payload ids, OAMD/JOC with `complexity_index` and the object/bed configuration, whether an
  authenticity tag is present, CRC validity per frame, and how often each coding tool was used.
  A human-readable table by default; `json=1` emits a versioned JSON document
  (`ac3forge.probe/1`) whose schema is documented as a stable contract in
  [docs/cli/commands.md](docs/cli/commands.md). `detail=frames` adds a per-access-unit dump and
  `detail=blocks` adds every block's Annex E tools and exponent strategies — the in-repo
  counterpart of `tools/references/eac3_parse.py`, which was the only field-level dump in the
  project and shipped with nothing. The exit code is non-zero if any frame failed its CRC or the
  parser refused it, so it works as a pipeline gate without its output being read. Memory is flat
  in the length of the stream: the input is pulled through a fixed window and the per-frame dump
  is written as the walk produces it.

  It reads in two tiers, and a stream this decoder cannot decode is still described in full — the
  committed DEE-encoded E-AC-3 baseline is exactly that case, where `decode` stops at
  `decode failed (code 5)` and `probe` reports the layout, rate, duration, substream map and CRC
  state, says which 76 of 79 syncframes the parser refused and why, and notes that the stream uses
  AHT.

- **`ac3::io::read_frame_header`** — one syncframe's bit stream information, read without
  decoding it. Promotes the E-AC-3 bsi walk `scan()` already had internally to a public API and
  gives AC-3 a matching one; `scan()` now goes through the same two functions rather than keeping
  a private copy.

- **`ac3::FrameSyntax`** (`ac3/decoder/syntax_trace.hpp`) — an opt-in per-block record of which
  coding tools a syncframe used and what exponent strategy each stream carried, on the same terms
  `ac3::verify::FrameTrace` already established: a null pointer in the `DecoderConfig` costs
  nothing. Both decoders write one.

- **`DecoderConfig::skip_reconstruction`** — parse every field exactly as a full decode does, but
  stop before the inverse transform, overlap-add, JOC object reconstruction and channel
  combination. The metadata is identical; the transform, which answers none of the questions an
  inspection asks, is not paid for.

- **`ac3::signing::has_authenticity_tag`** — whether a syncframe carries an authenticity tag,
  answered **without a key**. Where the tag lives is fixed by the EMDF container's own
  protection-length codes; only whether it *matches* needs the key.

- **E-AC-3 encoder input-space fuzzing** (`tools/ci/fuzz_eac3_encoder_space.py`, roadmap `VX1`).
  The AC-3 encoder-space harness said outright where it stopped — "Scope: AC-3 only [...] E-AC-3's
  own space [...] is a real remaining gap" — and this is that gap. It draws random legal
  `eac3-encode`/`atmos-encode` configurations (Annex E tool tokens with their band-edge pins,
  `fscod2` half sample rates, CBR and VBR, every layout including the ones needing dependent
  substreams, Atmos object counts) crossed with the AC-3 harness's adversarial per-block PCM
  generator, which it imports rather than copies. Because FFmpeg does not read E-AC-3 whole — no
  second dependent substream, no model of enhanced coupling or transient pre-noise, no `fscod2`
  decode — every case is classified by which oracle can actually read it, and the cells FFmpeg
  cannot decode still get their *framing* checked: by a walk over the four fields that fix E-AC-3
  framing (syncword, `strmtyp`, `substreamid`, `frmsiz`), which shares nothing with the encoder
  and works at every layout, and by `ffprobe` wherever FFmpeg can be trusted to walk one. Bounded
  on every pull request in the ffmpeg-validate job, deeper in the nightly encoder-space job.
- **Fuzzing for the object and metadata layer** (roadmap `VX3`). Five new libFuzzer harnesses
  over the parsers that read attacker-controlled bytes out of the skip field in every Atmos
  frame, which until now were reached only indirectly through `fuzz_eac3_decode`:
  `fuzz_emdf_parse` (`emdf::parse_container`), `fuzz_oamd_parse` (`oba::parse_payload`),
  `fuzz_joc_parse` (`joc::parse_payload`), `fuzz_signing_verify`
  (`signing::verify_atmos_stream`/`verify_atmos_frame`, the one signing operation that runs on a
  stream its caller did not produce — key and stream both fuzzed), and, behind
  `-DAC3FORGE_BUILD_ADM=ON`, `fuzz_adm_parse` (`ac3adm::parse_bw64`). Each is seeded from the
  real containers and payloads inside the Atmos streams `fuzz/generate-seeds.sh` already
  encodes, extracted by a new `fuzz/metadata-seeds.py`. The first four join `fuzz/run.sh`'s
  default list and so the existing `Fuzz Regress`/`Fuzz Short`/`Fuzz Nightly` jobs; the ADM one
  is opt-in (`AC3FORGE_FUZZ_ADM=1`) and gets its own nightly job, because `ac3adm` is the one
  library here with a third-party dependency footprint.
- **A CRC-repairing custom mutator** for `fuzz_ac3_decode` and `fuzz_eac3_decode`
  (`fuzz/crc_mutator.hpp`). Both decoders check their CRC words before reading a single bit of
  the frame behind them, so a mutation landing in a skip field — where the EMDF container, and
  therefore all object metadata, lives — was rejected two orders of magnitude before the parser
  it was aimed at. The mutator re-stamps crc1 and crc2 after mutating, crc1 through the same
  GF(2) polynomial inverse the encoder uses (it precedes the region it protects, so it is solved
  for rather than recomputed), and deliberately leaves one mutation in four unrepaired so the
  bad-CRC rejection path stays reachable.

- **Real programme material in the fixture corpus** (`VX7`). Every landscape and trend number
  this project has published rested on 2.5–3 s of `sin()`, pseudo-random noise and FIR
  smoothing. Both synthetic fixtures carry a flat noise plateau from 12 kHz to Nyquist at
  roughly the level of the content below it — a shape no real programme material has, and the
  one that made narrowing the encoder's bandwidth to 14.7 kHz look like a 2.1 dB win when it
  would have made a 448 kbit/s encoder plainly worse to listen to. Two 30 s CC0 fixtures now sit
  beside them: unscripted connected speech and orchestral music, both natively 48 kHz and
  losslessly sourced, both rolling off monotonically instead of plateauing. The synthetic
  fixtures are kept — the published series are only comparable because the material under them
  never moved — so the programme material runs as its own legs rather than replacing anything.
  `quality_race.py` also takes `--material speech|music` on every mode except `ci`, `trend` and
  `eac3-51`, so an encoder policy can be re-measured on material that is not band-limited.
- **The fixture corpus is versioned and hash-enforced.** `tests/golden/audio/corpus.json`
  records every fixture's format, duration, SHA-256 and — for the programme ones — its upstream
  source, that source's own SHA-256, its licence and the exact excerpt window;
  `tools/checks/check_corpus.py` fails if any committed fixture stops matching. A regenerated
  fixture is otherwise close to invisible: it still decodes, still has the right duration, still
  produces a plausible SNR, and simply puts a step in every published series that reads as an
  encoder change. `tools/generators/README.md` documents the corpus, the licences and the
  measured spectra.
- **Five new landscape/trend legs.** Four of them sit at rates where the Annex E tools actually
  run. `auto` enables coupling below 12 + 14n kbit/s per channel and spectral extension below
  56, and the only stereo leg sat at 96 per channel — above both — so every published stereo
  comparison was of an encoder that had chosen no tools at all. The new stereo legs at 96 and
  64 kbit/s bracket both crossovers, on synthetic and on programme material.

### Fixed

- **The MOS column carries real numbers** (`VX6`). `visqol-python` was deliberately not
  installed in CI, so all ~3,758 rows on the `quality-history` branch carried `mos_lqo: null`
  and every MOS cell on the landscape and tool-comparison pages rendered `n/a` — the perceptual
  half of this project's own published comparison did not exist. It is now hash-pinned in
  `requirements-ffmpeg-validate` like every other Python dependency (seven added packages,
  nothing already pinned moved) and installed on the `ffmpeg-validate` leg. Scoring is capped to
  a fixed 4 s window because ViSQOL's patch matching is super-linear — measured 3.9 s of compute
  for 3 s of audio and 127.8 s for 30 s — which keeps the whole column at about seven minutes on
  a job ~15 minutes off the critical path. The history appender gained a soft MOS regression
  tier (warning only, never fails a run).
- **Both 5.1 legs have a DEE number again.** The installed DEE build drops the surround-left
  channel when 5.1 arrives as one discrete 6-channel file, which is why `ac3-51-448` and
  `eac3-51-256` were marked unverified and their `vs DEE` cells read `n/a`. DEE's other
  documented input path — one mono WAV per channel, `--input-format wav_list` — does not,
  confirmed by per-channel RMS through a full encode and decode. That path's channel order is
  also this project's own WAV order, so the SMPTE permutation the single-file path needed (and
  which had already cost ~15 dB on both legs once when it was missing) is gone. No leg is
  unverified at baseline version 2.
- **`dialnorm=auto` and `ac3cli loudness` mis-assigned channel weights on any layout wider than
  stereo.** `measured_dialnorm()` pushed a WAV's channels into `LoudnessMeter` in *WAV* order
  (FL, FR, FC, LFE, BL, BR) when the meter expects AC-3 *coded* order (L, C, R, Ls, Rs, LFE). For
  5.1 that put the LFE where `Ls` belongs — so BS.1770's +1.5 dB surround weight landed on the
  one channel the standard excludes outright, while a real surround landed in the excluded slot
  and was dropped entirely. Found by cross-checking against ffmpeg's `ebur128`: with signal in
  only the LFE channel, ac3forge reported −38.61 LKFS where the oracle correctly reported no
  loudness at all. `ac3cli levels` already applied the right permutation, which is why it never
  showed the fault. Measured `dialnorm` values for 3-channel-and-wider sources change as a
  result, and are now correct.
- **`ac3::io::read_wav` read past the end of its buffer** on a WAV whose `fmt ` or `data` chunk
  tag sits within a few bytes of the end of the file. Both tags are located by searching the
  whole buffer and then read at fixed offsets past; only the 44-byte minimum length was checked,
  which a file can clear while still putting a tag in its last four bytes. A debug build caught
  it as a span bounds violation; a release build did not. `WavStreamReader::open`, written later
  against the same field layout, already carried the guard — this is the whole-file parser's
  missing half, using the same bounds and the same refusal. Found while writing the threat model.
- **Seven reports at three sites, all turned up by the new harnesses**, each with a reproducer
  under `fuzz/regressions/`. `compute_bit_allocation` walked off its own arrays on regions whose
  shape only a debug `assert` had ever constrained — an empty region indexed its 256-entry band
  table at `SIZE_MAX`, a region longer than the 253-mantissa ceiling wrote one element past the
  `psd` array, and `ac3::signing`'s per-channel tally handed it a span built by violating
  `subspan`'s precondition (a null data pointer with a non-zero size). `ac3adm::parse_bw64`
  allocated from declared chunk sizes rather than from what the file holds, so a 104-byte file
  claiming 4 GB of audio allocated 4 GB, and any other over-claiming chunk did the same one
  layer down inside libbw64. And `signing::verify_atmos_frame` inherited the *signer's* debug
  assertion that the frame is in the ac3forge Atmos subset, so a Debug build aborted on
  `ac3cli decode <plain stereo>.ec3 out.wav verify-objects`; verification runs on streams its
  caller did not produce, so that assertion is now signing-only.
- **Third-party decode interop gates** (roadmap `VX4`). The gold-reference gate now decodes all
  six committed external-baseline bitstreams with `ac3cli` on every leg and diffs each against
  FFmpeg's own decode, with per-fixture floors quoted beside their measured numbers; the one
  fixture FFmpeg cannot decode cleanly is scored against the source WAV instead. The same six streams seed
  the decoder fuzzers, so mutation starts from real third-party structure rather than only from
  this project's own encoder output. A new nightly `Interop` workflow widens the corpus to eight
  SHA-256-pinned FFmpeg FATE samples — commercially mastered material exercising spectral
  extension, 1536 kbit/s, a commentary track, dither, the 3/1 acmod nothing in this tree can
  encode, and (see below) an A/52 Annex E §E2.3.1.2 legacy-core delivery — fetched at run time
  rather than committed. `compare_wav.py` gains `--max-diff-dbfs` for near-silent material,
  where an SNR ratio cannot distinguish an inaudible disagreement from a defect.
- **AC-3 core plus E-AC-3 dependent decode support** (A/52 Annex E §E2.3.1.2). One of the
  fetched FATE samples, `the_great_wall_7.1.eac3`, turned out not to be a gap in `decode` but a
  real arrangement the standard sanctions: "If an AC-3 bit stream is present in the E-AC-3 bit
  stream, then the AC-3 bit stream shall be processed as an independent substream assigned
  substream ID 0." `ac3::io::scan` recognises the alternating AC-3-core/E-AC-3-dependent pattern
  as one access unit (a new `StreamKind::kAc3CoreEac3Extension`), and `ac3cli decode` routes such
  a stream to `Eac3Decoder`, which reads the core through a private `FrameDecoder` and combines
  it with its dependent exactly as §E3.8.2 combines an ordinary independent-plus-dependent pair.
  Verified against FFmpeg's own decode of the real FATE sample: 41.69 dB on the worst of the
  eight rendered channels. No ISOBMFF codec-config box is defined for the arrangement, so
  `dac3`/`dec3` muxing refuses it explicitly rather than emit a header that contradicts its own
  `mdat`.
- **A reference-mode end-to-end gate** (roadmap `VX10`). Since 0.9.0 flipped both transform
  defaults to the fast paths, every CI gate that touched a real stream ran in performance mode
  and the normative direct forms — the oracle each fast path is validated against — were covered
  only by transform-level unit tests. `verify_gold_reference.sh` now takes
  `TRANSFORM_MODE=reference` and the `linux-gcc` leg runs it a second time that way, and the
  codec matrix gains `fast-imdct=off` decode rows beside its existing `fast-mdct=off` encode
  row.
- **`dither=off` / `nodither`** (`EncoderConfig::dither`, `eac3::FrameConfig::dither`,
  `plan::Tools::dither`) pins §7.3.4 `dithflag` at 0 unconditionally, the deterministic behaviour
  from before content-decided dither existed. Real dither values are decoder-defined - two
  independent, spec-correct decoders given the same dithered stream diverge in the dithered bins
  by design - so this exists for the one caller that needs bit-for-bit agreement between two
  decodes of the same bitstream more than it needs dither's own perceptual benefit:
  `tools/checks/verify_gold_reference.sh`, whose 55 dB decoder-agreement gate content-decided
  dither would otherwise fail on legitimate, spec-permitted divergence rather than a real bug.
  AC-3 has no `tools=` string, so `dither=off` is the CLI option surface `fast-mdct=off` already
  established there; E-AC-3's `tools=` string takes the equivalent bare `nodither` token.

- **Five E-AC-3 decoder defects, all of them syntax only a third-party encoder produces.** The
  six real Dolby Encoding Engine and FFmpeg bitstreams in `tests/golden/external-baseline` had
  been in the repository since 2026-08-12 with nothing in `tests/` or `src/` reading them;
  pointing the in-repo decoder at them for the first time found that four of the six did not
  decode at all. Each defect desynchronised the bit reader outright rather than merely losing
  fidelity, and each is in syntax this project's own encoder and FFmpeg's both happen never to
  emit: the three AHT-in-use flags read unconditionally instead of only where a stream's
  exponents are transmitted once in the frame (Table E1.2); the coupling channel's own fast gain
  and fine SNR offset (`cplfgaincod`, `cplfsnroffst`) not read at all; the default coupling,
  spectral-extension and enhanced-coupling band-structure tables applied in every block whose
  exist flag was clear, where §E2.3.3.7/.15/.18 use the default only in the first block using
  that tool and the previous block's structure in every later one; `firstcplcos[ch]`,
  `firstspxcos[ch]` and `firstcplleak` treated as "block 0" rather than the per-frame,
  per-channel state §E2.3.2.28-30 defines; and a block declaring "no coupling" failing to reset
  the coupling state. All six fixtures decode now. On DEE's stereo E-AC-3 stream — where FFmpeg
  8.0.1 fails frame 0 from cold (`exponent 25 is out-of-range`) and conceals it by repeating
  block 0 — the in-repo decoder scores 33.72 dB against the source, where FFmpeg's own decode
  scores 14.30 dB.
- **`eac3-encode` aborted instead of reporting an error** when the bitrate was above what
  §E2.3.1.3's 11-bit `frmsiz` word count can signal at the chosen sample rate — every layout,
  reachable by typing two ordinary numbers, since both a nominal Table 5.18 bitrate and an Annex E
  half sample rate are legal on their own and nothing in the CLI's grammar marks the pair. In
  practice: above 320 kbps at 16 kHz, above 448 at 22.05 kHz, above 512 at 24 kHz. A release build
  refused it without naming a cause; any build with assertions live aborted. It now reports the
  limit, the word count needed and the way out. `atmos-encode` was never affected. Found by the
  new E-AC-3 encoder-space harness above.
- **E-AC-3 `snroffststr` 0x2 read the wrong fields.** The per-channel fine-offset strategy's
  parse consumed one value per coded channel and none for the coupling channel, so a conforming
  stream using it would have desynced whenever coupling was on, and the shared channel would have
  allocated against an offset nobody sent. §7.2.2.1.1's all-zero test now includes
  `cplfsnroffst` too. Nothing emits this strategy - not this project's encoder, not FFmpeg's, not
  Dolby's - so the correction is spec-derived rather than measured against a real stream; see the
  note at the code and roadmap `EQ2`.

- **`eac3-sine` had the identical gap**, one command over from the fix above — the same
  unreachable `frmsiz` ceiling, met with the same `assert()` (`nchans == tone_hz.size()`) rather
  than a diagnosis, since it builds an `AccessUnitEncoder` from a plan the same way `eac3-encode`
  does. `ac3::plan::validate()` now carries a check of its own for this: the E-AC-3 counterpart
  of the Table 5.18 check it already made for AC-3 (`PlanError::kBitrateNotFramable`), checked
  against the per-substream configs `eac3_config()` really builds rather than the plan's own
  `bitrate_kbps` — a layout with dependent substreams gives each of them half the rate, so a
  config can be unframable there even when the plan's own number fits. `eac3-sine` asks it
  before building an encoder, the way `run_encode`'s AC-3 path always has. VBR is exempt, as it
  is inside the frame encoder: there the content decides the word count and `bitrate_kbps` only
  steers the tool frequency defaults.

### Changed

- **Floating-point contraction is pinned off** (`-ffp-contract=off`, `/fp:precise` on MSVC,
  `/clang:-ffp-contract=off` on clang-cl), project-wide — for the SIMD seam's bit-exactness
  argument, which needs the compiler not to silently re-fuse a vector operation into an FMA the
  seam's intrinsics cannot express. No measurable cost on x86-64, where the flag is a no-op —
  proven by the corpus comparison above being byte-identical against a build without it.
  **Tested and ruled out as the explanation for roadmap `VX11`'s gold-reference gap**: the leading
  hypothesis for why `linux-gcc-arm64`, `linux-llvm-arm64` and `macos-llvm` score 6.02 dB (exactly
  one AC-3 exponent step) below every x86 leg was FMA contraction, since it is architecture-
  dependent in exactly that pattern. With the flag pinned on every leg, the gap is unchanged —
  those three legs still measure ~61.8 dB against x86's ~67.8 dB. `docs/building.md`'s
  "Floating-point contraction" section carries the measurement and the surviving hypothesis (all
  three low-scoring legs are aarch64, which points at libm's own architecture-specific
  `sin`/`cos` in the transform twiddle tables); `VX11` stays open.
- **`ac3kernelbench` gained the fast inverse transforms.** `imdct512_windowed_fast`,
  `imdct256_pair_windowed` and `imdct256_pair_windowed_fast` join the per-kernel trend series; the
  bench previously timed only the direct inverse, which has not been the default since 0.9.0, so
  the whole decode side of a transform change was invisible to
  [docs/performance-trend.md](docs/performance-trend.md).
- **JOC now runs in the QMF domain by default**, encoder and decoder. Mean per-object SNR over
  four placements goes from 22.8 dB to 28.6 dB; against a decoder reconstructing in the QMF
  domain — which is what every licensed decoder does, and which the old encoder had no way to
  target — from 23.5 dB to 28.6 dB. On moving objects, 20.2 dB to 26.5 dB. Encoding costs
  0.62 → 0.74 ms/frame of a 32 ms budget; decoding gets cheaper, 0.88 → 0.70 ms/frame. Memory is
  a one-off setup cost (encoder +20 KB, decoder +44 KB on the first JOC frame) with per-frame
  churn unchanged. `joc-domain=mdct` restores the previous behaviour on both sides.
- **Reconstructed object audio now lags the bed by 576 samples rather than 256.** That is the
  QMF pair's own algorithmic delay and cannot be shortened. Code comparing `object_audio`
  against a known source must shift by `joc::reconstruction_delay(domain)` rather than a
  hard-coded 256 — including anything reading `decode`'s `objects_dir=` output.

- **`atsc-a85` re-cited to ATSC A/85:2026-07** (`IO11`), approved 8 July 2026 — the first full
  revision of A/85 since 2013. Its §6 restates the target loudness, tolerance and true-peak
  ceiling unchanged (−24 LKFS, ±2 dB, −2 dBTP), so **no preset number moves**; only the citation
  does. EBU R 128 s4, Netflix's Dolby Atmos Home Mix Deliverable Requirements v2.3 and Amazon
  were each checked against their primary sources and deliberately *not* added as presets: the
  first two are numerically identical to presets already in the table (s4 differs only by a
  Loudness-to-Dialogue Ratio this meter cannot measure, and the Atmos spec only by asking for a
  5.1 re-render, which is a `layout=` choice), and no primary Amazon document was reachable to
  cite. `ac3/meta/qc.hpp` and `docs/cli/metadata-options.md` record the reasoning for each.
- `QcPreset` distinguishes a loudness **band** from a loudness **ceiling**
  (`QcLoudnessLimit`). Every preset before now stated a target with a symmetric tolerance; Apple's
  states only a level not to exceed, and gating that as a ±band would fail quiet material the
  specification actually accepts.
- **Coded bandwidth is decided from the content, not the bit rate alone** (`EQ7`, `EQ8`). AC-3
  chose `chbwcod` from a per-channel-kbit/s curve; E-AC-3 never chose at all, sending a fixed 60
  — the whole 23.7 kHz — even at 96 kbit/s per channel, where neither coupling nor spectral
  extension runs and the frame has about two bits per mantissa to spread across 253 of them.
  Both encoders now take that rate curve as a ceiling and put the frame's own spectrum under it,
  testing each band's §7.2.2.3 banded PSD against Table 7.15's absolute hearing threshold — the
  same `hth` the allocator already floors its masking curve with. Above 128 kbit/s per channel
  the content is not consulted: reclaimed bits are only worth having while the rest of the
  spectrum is short of them. Measured on real programme material (CC0/public-domain piano,
  thunderstorm, church bells, speech and samba, sourced locally — `VX7` has not landed), E-AC-3
  stereo at 192 kbit/s gains 1.2–2.7 dB SNR and up to +0.034 ViSQOL MOS, with the high-band
  energy ratio improving alongside; AC-3 5.1 at 448 gains 0.4 dB. Nothing at or above
  128 kbit/s per channel changes, and a pinned `chbwcod` still overrides both halves.

- **AC-3's `fgaincod` is rate-dependent** (`EQ7`). §8.2.12's fixed 4 becomes a line from 7 at
  38 kbit/s per channel to 0 at 128, measured over the whole 0–7 range on real programme
  material and decided on ViSQOL — waveform SNR prefers 7 at every rate on every material and
  so distinguishes nothing. Worth +0.099 MOS at 192 kbit/s 5.1 and +0.158 at 640, and the same
  shape as the SNR-only sweep `encoder.cpp` had already recorded in the other direction. The
  new `EncoderConfig::fgaincod` (−1 = auto, 0–7 pins) overrides it. Verified over 25 (leg, rate)
  cells: mean +0.098 MOS and +0.54 dB SNR, worst cell −0.016 MOS.
- **The FFT core is radix-4, specialised per size** (`PF4`). The generic iterative radix-2
  decimation-in-time core every transform in the library shared — an explicit bit-reversal pass
  followed by log2(P) stages of two-point butterflies, each loading a twiddle and doing a full
  complex multiply against it — is replaced by compile-time-specialised kernels for the three
  sizes the codec actually uses (P = 64, 128, 512): radix-4 stages with a trailing radix-2 stage
  where log2(P) is odd, the first stage's unit twiddles eliminated (a quarter of the butterflies
  at P = 512 were multiplying by a tabulated 1.0), and the digit-reversal permutation folded
  into each caller's own input-producing loop — the DCT-IV quarter-split, the §7.9.4.1 step-2
  pre-twiddle, `dft512`'s input copy — rather than run as a pass of its own. Measured on
  linux-gcc, median of ten interleaved base/branch runs: `mdct512_forward_fast` 2535 → 1415
  ns/call (1.81×), `dft512` 8036 → 4361 (1.86×), `imdct512_windowed_fast` 2889 → 1842 (1.56×),
  `mdct256_pair_fast` 2510 → 1698 (1.56×), `imdct256_pair_windowed_fast` 3147 → 2262 (1.34×),
  `band_energy_fast` 27.9 → 21.9 µs (1.24×), against a 0.90–1.07× spread on the kernels this
  did not touch. End to end over 180 seconds of real 5.1: a 5.1 AC-3 decode 4.19 → 2.92 s and
  an E-AC-3 enhanced-coupling decode 5.84 → 3.24 s. Every stream in the 40-encode corpus is byte-identical to before, and the direct-form
  evaluations the fast paths are validated against are untouched — `mode=reference` still runs
  the spec's own arithmetic end to end. Accuracy is unchanged to three significant figures on
  every transform and slightly better for `dft512` against its own O(N²) summation (1.9e-15 →
  1.7e-15).
- **The fast IMDCT reaches enhanced coupling and JOC object reconstruction** (`PF3`). The two
  §7.9.4.1 call sites the 0.9.0 fast-inverse work fenced out were still evaluating step 3 as the
  pseudocode's direct O(N²) sum: `eac3::ecpl_channel_spectrum` (three 512-point inverses per
  coupled channel per block, on both the encode and the decode side) and `joc::reconstruct` (one
  per object per block — 90 in a 15-object frame). Both now forward a `fast` flag; a decoder
  passes `DecoderConfig::fast_imdct` for both, and the encoder-internal `ecpl` use passes
  `eac3::FrameConfig::fast_mdct`, which makes that field the encoder's fast-transform switch in
  both directions and keeps `mode=reference` direct end to end. `ecpl_channel_spectrum` is 4.4×
  faster on its own (74.8 → 17.1 µs, before `PF4` compounds it to 6.9× — 73.4 → 10.6 µs), and a
  30-second 15-object decode drops from 6.47 s to 4.83 s. Encoder output is byte-identical across the
  corpus, so this is a speed choice and not an output one.
- **The external baseline's DEE stereo score is explained rather than re-measured.** The
  33.32 dB recorded for `eac3-stereo-192`'s DEE leg reproduces exactly against the same FFmpeg
  8.0.1 build, and is now corroborated to 0.005 dB by this project's own decoder, so the number
  and its `"decoded_with": "ffmpeg"` label both stand. What was missing is that FFmpeg fails that
  stream's first frame from cold and `score_fixed`'s 0.2 s skip puts that frame outside the
  scored window - across the whole file FFmpeg's decode is 14.30 dB. A `decoder_note` on that
  entry now records it, emitted by `gen_external_baseline.py` so a regenerated baseline keeps it.
  Separately, the Dolby Reference Player's long-standing "decodes DEE's own stereo output to
  garbage" note is resolved: the player applies dialnorm, DEE writes a measured dialnorm of 12,
  and the 19 dB attenuation that follows was being charged to the decode. Compensated, the same
  decode scores 32.19 dB, which makes the player usable as an oracle again (`VX5`).
- `ac3::signing`'s frame walk now reports "no container" for a syncframe outside the subset it
  supports, where it previously asserted. That was sound while signing and verifying were its
  only callers — each already knew what it was handing over — but `has_authenticity_tag` is asked
  of every syncframe of an arbitrary stream, where an ordinary non-Atmos frame is not an error and
  a debug build must not abort on one. Release behaviour is unchanged: it already declined to sign
  these, just without saying so.
- `DecodedFrame` and `DecodedSubstream` now report `bsid` and `bsmod`, which both decoders
  already read past and discarded.
- **E-AC-3 `auto` chooses its Annex E tools from the frame, not just the bitrate** (`EQ9`). The
  tool set used to follow from the per-channel rate alone. Two measures taken from the MDCT
  coefficients the transform has already produced now decide with it: how much of the coupling
  region survives the decoder's own reconstruction of it (the shared channel is the coefficient
  sum and the coordinate restores each band's energy, so the residual against that rank-one shape
  *is* what coupling costs — evaluated, not estimated), and how much of the frame's energy sits
  above the extension frequency. Spectral extension's ceiling now runs from 110 kbit/s per
  channel where the top end is nearly empty down to 55 where it carries real content, instead of
  a fixed 56; coupling now needs both a near-exact fit and a region at least four sub-bands wide,
  which it rarely has once §E3.3.1 has derived its end frequency from `spxbegf`.

  Measured on real programme material rather than the checked-in fixtures — six 12 s excerpts of
  a 5.1 theatrical mix, six (layout, rate) points from 32 to 96 kbit/s per channel, scored
  through this project's own decoder with ViSQOL MOS-LQO beside SNR — `auto` comes out +0.11
  MOS-LQO and +0.36 dB SNR against the rate-only policy, better in 19 of 36 cells and worse in
  six, none of those six by more than 0.03 except one cell that moves the same way with or
  without this change. No (layout, rate) point regresses: the two biggest gains are +0.32
  MOS-LQO at 128 kbit/s stereo and +0.16 at 384 kbit/s 5.1. The fixed 56 it replaces was measured
  as marginal SNR on fixtures carrying 99.9% of their energy below 8.1 kHz, which is below where
  either tool operates; those fixtures' own landscape numbers are unchanged by this (32.05 dB at
  192 kbit/s stereo against 31.98 before, 31.61 at 256 kbit/s 5.1 against 31.63).

  `tools=auto` output stays decodable by FFmpeg, and the tool tokens (`cpl`, `spx`, `aht`,
  `cpl+ecpl`, `tpn`, …) are unchanged — a caller who names a tool set still gets exactly it.

### Documented

- **Enhanced coupling and transient pre-noise, measured and labelled** (`EQ10`). Both are fully
  implemented and decode correctly; neither is in `auto`'s set, for opposite reasons, and both
  are now written down with their numbers in `docs/concepts/ac3-eac3.md` and
  `docs/library/encoding-eac3.md`.

  Enhanced coupling is the better-sounding of the two coupling reconstructions on real material —
  ahead of standard coupling on MOS-LQO at every (layout, rate) point tried, by +0.54 MOS-LQO at
  96 kbit/s stereo, +0.31 at 128 and +0.18 at 192, and +0.78 / +0.55 / +0.16 at 192 / 256 /
  384 kbit/s 5.1. Every trend row records it as a net loss because every trend row is SNR, and a
  phase-restoring reconstruction built on a full DFT does not preserve the waveform. What keeps it
  out of `auto` is that FFmpeg's Annex E parser has no model of §E3.5 and misreads such a stream as
  a corrupt frame; `cpl+ecpl` still asks for it, and on a pipeline whose decoder reads it the
  measurements say to.

  Transient pre-noise processing does not pay. Over exactly the samples it touches it lands
  6.5–24 dB further from the original than leaving the audio alone, at every bitrate measured,
  and the gap widens with rate: the substitution's error is a property of the material (flat at
  20.7–22.5 dB whatever the bitrate) while the coder's own error over those samples falls from
  11.9 dB at 96 kbit/s stereo to −3.3 dB at 256. It is not a bit-allocation effect — outside its
  own footprint the two decodes are bit-identical — and perceptually it is a no-op, matching the
  untreated encode to within 0.01 MOS-LQO in every row. Block switching gets there first: the
  correction is gated on the same transient detector that shortens the transform, so it
  substitutes earlier audio for audio the short transforms had already protected.
- **E-AC-3 transmits its bit allocation parameters** (`bamode` 1, roadmap `EQ3`) instead of
  inheriting Table E1.4's `bamode == 0` defaults - which are not §8.2.12's basic-encoder set, and
  in particular pinned `dbpbcod` at 2. The frame now states `{sdcycod 2, fdcycod 1, sgaincod 1,
  dbpbcod 3, floorcod 7}`, at a cost of 17 bits a frame. `dbpbcod` 3 is the departure the AC-3
  encoder measured its way to in 0.7.0, and it carries over: swept 0-3 across 96/128/192 kbit/s
  stereo and 192/256/384/640 kbit/s 5.1, it wins every cell by +1.2 to +3.0 dB SNR against the
  old value, with ViSQOL MOS up in every cell too. `floorcod` was swept as well and stays at 7 -
  the lowest of the eight, so the floor never binds. Both reference encoders in
  `tests/golden/external-baseline/` emit exactly this set.
- **`dithflag` is decided from content** in both encoders (roadmap `EQ4`), per full-bandwidth
  channel per block, where it was previously written as a fixed 0. §7.3.4's dither exists to fill
  the bins the allocator gave no bits to; the encoders now compare the energy the decoder will
  not receive against the energy the dither would put there instead, and set the flag only where
  the first is at least as large as the second. Digital silence always reads clear. A
  block-switched channel never dithers - two interleaved half transforms share one coefficient
  set there - which is also exactly what Dolby's own encoder does in the reference stream. On
  E-AC-3 dither is additionally held off for any frame using spectral extension, whose
  copy-source reconstruction the encoder could not otherwise mirror. The flag is transmitted
  either way, so none of this costs bits; it trades a little waveform SNR for perceptual quality,
  which is what the tool is for.

  Measured on `tools/ci/quality_race.py`'s own material, decoded by FFmpeg 8.0.1, ViSQOL in audio
  mode. The E-AC-3 rows carry both changes; the AC-3 rows carry only the dither one. Full tables,
  every rate and tool set, are in the pull request.

  | leg | before | after |
  |---|---|---|
  | AC-3 stereo 192 | 41.82 dB / 4.246 MOS | 41.12 dB / 4.344 MOS |
  | AC-3 5.1 448 | 21.96 dB / 3.956 MOS | 20.91 dB / 4.268 MOS |
  | AC-3 5.1 448, committed fixture | 39.95 dB / 3.670 MOS | 38.92 dB / 3.913 MOS |
  | E-AC-3 stereo 96, no tools | 25.79 dB / 3.799 MOS | 26.75 dB / 4.307 MOS |
  | E-AC-3 stereo 192, `auto` | 40.42 dB / 4.395 MOS | 40.98 dB / 4.414 MOS |
  | E-AC-3 5.1 192, no tools | 10.02 dB / 1.326 MOS | 12.14 dB / 2.340 MOS |
  | E-AC-3 5.1 256, `cpl` | 15.44 dB / 2.364 MOS | 15.36 dB / 2.875 MOS |
- **`std::format`/`std::print`/`std::printf` replaced with {fmt}'s `fmt::format`/`fmt::print`/
  `fmt::printf` everywhere.** NDK r26's bundled libc++ has no `<format>` at all unless the
  compiler is invoked with `-fexperimental-library`, which the Android build never does; {fmt}
  (the library `std::format` was standardized from) has no such gap, so the codebase now uses it
  uniformly instead of avoiding standard formatting file by file. The handful of pre-existing
  `%`-specifier call sites moved to `fmt::printf` with their format strings unchanged, rather than
  being rewritten to `{}`-style. `fmt` is a new base vcpkg dependency (falls back to
  `FetchContent` when no local copy is found — see `cmake/Fmt.cmake`); no public API is affected,
  since every use is confined to implementation files.
- **Lint and security analysis for the non-C++ code** (roadmap VX14). A new `Script Lint` CI job
  runs `ruff` over every Python file in the tree, `shellcheck` over every shell script, and
  `actionlint` over the workflows — the last of these with `shellcheck` wired in, so the shell
  inside every workflow `run:` block is checked too. All three are hash-pinned in
  `requirements/requirements-lint.txt`, and ruff's rule set is curated explicitly in a root
  `ruff.toml` rather than left at a default that moves between releases. This matters more here
  than lint hygiene usually does: about thirty of those Python files *are* this project's oracles
  and CI gates, so a swallowed subprocess failure in one of them turns a check green without
  checking anything. Its first run found exactly that class of thing — 14 `subprocess.run()` calls
  with no explicit `check=`, four length-tolerant `zip()`s inside the gold-reference gate's own
  comparator, a closure over an unbound loop variable in the E-AC-3 reference parser, two unquoted
  shell globs, and a `continue-on-error` reading a matrix property no matrix entry defined.
  CodeQL also becomes a language matrix, adding `python` and `javascript-typescript` beside `cpp`.
- **A ThreadSanitizer CI leg** (roadmap VX16). `src/audio` is a lock-free SPSC ring, a silence
  watchdog and a clock-drift servo shared between a real-time callback thread and the encoder
  thread, and the only sanitizer leg was ASan+UBSan, which cannot see a data race. A new
  `config-linux-llvm-tsan` preset and matrix entry run a `concurrency` ctest label — `tests/audio/`
  plus a new `tests/cli/test_cli_live.cpp` covering the `devices`/`outputs`/`record`/`live`/
  `monitor` commands, which nothing tested before. The label comes from the Catch2 tags themselves
  (`ADD_TAGS_AS_LABELS`), so `ctest -L ring`, `-L encoder` and the rest now work as well. The
  first run was clean, and `tsan.supp` is checked in near-empty with a note saying it should stay
  that way.
- **A performance comparison on every pull request** (roadmap VX17). The performance trend is only
  recorded on pushes to `develop`/`main`, and `ac3perf`'s absolute real-time budget has enough
  headroom that a change could double ms/frame and still pass it — so a PR that halved the
  encoder's speed went green everywhere and surfaced only after merging. A `performance-compare`
  job now builds `ac3bench`/`ac3kernelbench` at the merge base and at the PR head on one runner,
  runs each three times, and posts a table of per-workload and per-kernel deltas to the job
  summary, using the same soft/hard tiers the trend job applies on merge. Informational only: it
  never fails a build, and the trend-branch append stays push-only.

### Changed

- **The coverage gate now covers `apps/cli` and `python/`, not just `src/`** (roadmap VX15). All
  nine library components had a line and a branch floor while `apps/` had none — despite `apps/cli`
  being about 6,500 lines, the executable the codec matrix, the gold-reference gate and the
  encoder-space fuzzer all drive, and the place both CLI bugs this project has shipped actually
  lived. `apps/cli` is gated at line 40 / branch 34 against a measured 54.0 / 46.5, with a
  per-command breakdown printed below the gate — reported, not gated — so a thin command module
  shows as thin instead of averaging away. `wheels.yml` gains a `python-coverage` job running
  `pytest --cov` against the built wheel. Reaching a number at all required fixing something
  quietly wrong: `ac3cli` linked an instrumented library but never linked `ac3::coverage` itself,
  and because `--coverage` is target-scoped at compile time its sources were compiling
  uninstrumented and emitting no coverage data at all. `apps/gui` stays out of scope — its C++
  needs a Qt kit on the coverage leg that no Linux CI leg installs.

- **ROADMAP.md rebuilt** at v0.9.0-beta.1. The 2026-08-15 list was 25/32 checked off; the seven
  open items (`B2`, `B3`, `D1`, `D4`, `E3`, `F4`, `F5`) are carried into a new nine-theme list
  (`EQ`/`DC`/`IO`/`IM`/`VX`/`PF`/`AP`/`UX`/`DR`, 100 items) with their real current state - `E3`
  is already confirmed on Linux/ALSA against a real AVR, PyPI and the Homebrew tap are live, the
  vcpkg port is policy-blocked until about 2027-02 - and the retired single-letter IDs are kept
  in a ledger so older references still resolve. The DAMF reader (`B2`) moves to "Deliberately
  not on the list" (no public specification); an IAB (SMPTE ST 2098-2) reader replaces it now
  that SMPTE's catalogue is free.

## [0.9.0-beta.1] - 2026-08-22

Ninth tagged release. The headline is the memory-usage optimization programme landing in full:
per-frame codec allocation churn down 54–88%, every CLI command and GUI recording streaming
instead of buffering, and a new memory trend that gates regressions the same way the timing
series always has — alongside a default-on fast inverse transform (4.5–4.7× faster decodes), a
whole-library per-component coverage gate, `ac3::signing` joining the installed/exported library
surface, and continued `apps/cli` command-group extraction.

### Added

- **Performance and reference transform modes.** The decoder's inverse transform joins the
  forward MDCT in having a fast path: §7.9.4 step 3 — the one O(N²) part of the normative
  inverse — now runs through the same radix-2 FFT core the fast forward fold uses, and after
  its evidence was reviewed (worst transform-level relative error 7.8e-14 against the direct
  form; 214.9 dB SNR agreement for AC-3 and 284.7 dB for E-AC-3 over 180 seconds of real 5.1
  material) it became the default: **decodes run 4.5–4.7× faster** (a 180-second decode drops
  from ~3.5 s to ~0.8 s), and the direct form's 320 KiB of tabulated matrices are no longer
  built at all on the default path. The pair is exposed as one intent-level switch:
  `mode=reference` runs every transform in a command on the spec's own direct evaluations —
  the forms the fast paths are validated against, for fixture regeneration or sample-for-sample
  comparison against an external decoder — and `mode=performance` (the default state) names the
  fast paths; `fast-mdct=off` / `fast-imdct=off` still adjust one half at a time. Encoded
  output never depends on the decode-side switch. See
  [Validation → Performance and reference modes](docs/verification.md#performance-and-reference-modes).
- **Span-output decode forms.** `FrameDecoder::decode_frame_into` and
  `Eac3Decoder::decode_access_unit_into` decode into caller-owned planar storage rather than
  allocating a fresh vector per call, with the same results as the value forms, pinned by
  lockstep equivalence tests. The E-AC-3 form keeps
  §3.7's transient-pre-noise hold-back semantics exactly: a held-back frame leaves the caller's
  spans untouched and is copied out at release.
- **Streaming I/O for unbounded sessions.** `ac3::io::WavStreamReader` (block-at-a-time WAV
  reading with the same parsing and sample conversion as the whole-file reader),
  `ac3::io::WavPcm16StreamWriter` (the incremental sibling of the one-shot PCM16 writer, for
  IEC 61937 carriers whose length isn't known up front), and `mpegts::Writer` (incremental
  transport-stream muxing whose output is byte-identical to `mpegts::mux()` — that equality is
  its contract and its test). Matroska already had its incremental `Writer`; MP4 deliberately
  does not get one — `moov`/`stco` need every frame's final offset, and `fragment()` (fMP4) is
  that format's streaming shape.
- **A memory trend beside the timing trends.** `ac3membench` counts heap allocations and
  allocator traffic per frame, live-byte drift and peak RSS across the encoder configurations
  *and* the decode paths the timing benches never covered; every `develop`/`main` push appends
  to the same `quality-history` series the CPU numbers use, rendered on
  [docs/performance-trend.md](docs/performance-trend.md) with the same trailing-baseline gates
  (either churn metric regressing flags the row) plus an absolute leak check that applies
  regardless of the trailing baseline.
- **`ac3::signing` is now an installed, exported library component** (repo-structure review D6),
  restructured into the same OBJECT+STATIC+SHARED shape `ac3::forge` itself uses
  (`ac3::signing_static`/`ac3::signing_shared`, `AC3SIGNING_EXPORT`-annotated) instead of a
  single internal-only `STATIC` target with no `install()` at all. `signing_static`/
  `signing_shared` each publicly link their own matching `forge_static`/`forge_shared`,
  preserving today's `PUBLIC ac3::forge` propagation; a real standalone
  `find_package(ac3forge CONFIG REQUIRED)` consumer linking `ac3::signing_static` now builds and
  runs across the installed-package boundary.

### Fixed

- **The encoder input-space fuzz no longer reports FFmpeg container-probe misses as encoder
  failures.** Case seed 1124127684685913171 (stereo at 512 kbit/s, 48 kHz) produced a fully
  valid stream — every syncframe on its exact 2048-byte boundary, both CRC words of every frame
  good, a clean strict decode under `-f ac3` — that FFmpeg 8.0's auto-detection nonetheless
  handed to its MPEG-PS demuxer: with frames that large, ffmpeg's AC-3 prober cannot clear its
  own accept threshold inside the 8 KiB probe window (it wants seven consecutive syncframes),
  while three start-code-shaped byte patterns inside ordinary quantized mantissas were enough
  for the MPEG-PS prober to win that window outright, and no amount of appended audio can win it
  back. `tools/ci/fuzz_encoder_space.py` now arbitrates any FFmpeg refusal by rerunning with
  `-f ac3` forced and every error check kept — a clean forced decode classifies the case as
  "misprobed" (counted and reported, never failing), a refused one still fails with the real
  decode error. The seed is recorded in the script's new `--regressions` replay list, which CI
  gates on before each unseeded search, and `fuzz/seeds/` gained a 512 kbit/s stereo stream so
  decoder-side fuzzing mutates from the big-frame corner too.
- **Installed packages now actually export `ac3::forge_c_static`/`ac3::forge_c_shared`**, matching
  what [docs/library/c-api.md](docs/library/c-api.md) and the in-tree `ALIAS` targets always
  documented. The raw CMake targets were previously `capi_static`/`capi_shared` under the `ac3::`
  namespace with no matching alias, so an installed package actually provided `ac3::capi_static` —
  a name nothing in the documented consumer surface used, and a `find_package(ac3forge)` consumer
  following the docs could not link the C API at all. Fixing the name surfaced a second, more
  serious bug: the C API's object library always privately links the static codec regardless of
  `BUILD_SHARED_LIBS` (a deliberate self-contained-ABI design), and
  `AC3FORGE_INSTALL_BOTH_LINKAGES=OFF` combined with a shared-only build used to leave that static
  target out of every export set, failing the configure step outright. That combination now
  configures, builds and installs cleanly.
- **The Conan and Winget packaging manifests are back on the real latest release** — both were
  still pinned to `0.8.0-beta.1` after `0.8.0-beta.2` shipped. `tools/checks/check_packaging_versions.sh`
  now runs in CI and fails the build if any packaging manifest's version drifts from the others
  again.
- **The hosted WASM decode demo (`docs/assets/wasm-decode-demo/`) matches the real one again** — it
  had silently fallen out of sync with `apps/wasm/`'s own copy (missing favicon links and the GPL
  footer). `docs.yml`'s docs build now byte-compares the two and fails if they drift apart again.
- **The Debian/Ubuntu package's homepage field is no longer empty** — `PROJECT_HOMEPAGE_URL` is now
  set on the root `project()` call, so `dpkg -s ac3forge` reports the real project URL instead of
  nothing.
- Fixed a stale anchor in `docs/platforms/raspberry-pi.md` pointing at a `linux.md` heading whose
  text no longer matches.
- Fixed `docs/library/index.md` and `docs/releasing.md`'s vcpkg port sections, which still blamed
  `ac3::forge_c`'s absence from the port on the installed-export-set bug fixed above — the port
  has always passed `-DAC3FORGE_BUILD_CAPI=OFF` regardless of that bug and continues to now that
  it's gone, as a deliberate scope decision pending a `capi` feature. Verified with a real
  `vcpkg install ac3forge --overlay-ports=packaging/vcpkg-port` that the port still installs no
  `ac3::forge_c` artifacts today.
- **A stack-overflow-risk PREfast finding (alert #77) is fixed**: `examples/atmos_objects.cpp`
  now heap-allocates its `Eac3Decoder` instead of stack-declaring it, the same fix already
  applied to `atmos_fallback.cpp` and `station_broadcast.cpp` for the identical scratch-state
  growth. Two duplicate false-positive `optional`-access findings (alerts #70/#71, in
  `apps/gui/qc_controller.cpp`'s and `apps/cli/main.cpp`'s `measure_qc`/`measure_eac3`) are
  documented and suppressed — a `have_first`/non-empty-stream guard already proves the meter
  optional is engaged before use, matching a pattern already fixed once elsewhere in `main.cpp`.
- **`misc-include-cleaner` findings that leaked back into `apps/cli/main.cpp` and
  `commands/analysis.cpp`** after the CLI command-group extraction (both predate that move and
  were never revisited for their own include lists) are fixed, keeping the `static-analysis` CI
  leg green.

### Changed

- **The CI coverage gate now measures the whole library, per component.** The `coverage` leg
  previously instrumented and gated the codec core (now `src/forge`) alone; it now instruments
  every library component —
  `ac3::forge`, `ac3::audio`, `ac3::signing`, the Matroska/MP4/MPEG-TS writers, the C API, and
  the opt-in ADM module plus its bridge — and gates statement (line) and branch coverage per
  component via the new `tools/checks/coverage_report.sh`, so a regression in a small module can no
  longer hide inside a blended number. `src/forge`'s own floor rose from 80%/70% line/branch to
  88%/78% to track the suite's growth, and the first whole-library measurement put honest floors
  under two thin spots — `src/audio`'s device I/O paths and the C API's E-AC-3 surface — rather
  than leaving them unmeasured. See the script's floor table for every component's numbers.
- **The C API's E-AC-3 surface is now tested, and its coverage floor raised to match.**
  `tests/test_capi.cpp` gained the E-AC-3 half it was missing: substream and access-unit round
  trips across the Annex E tool combinations, dependent-substream and dual mono metadata,
  transient pre-noise hold-back and flush, the decode/encode error mappings, and the NULL-handle
  defaults across the whole opaque-handle surface — all on real multi-frame audio. `src/capi`'s
  measurement moved from 48.4%/27.1% line/branch to 87.8%/79.2%, and its floor in
  `tools/checks/coverage_report.sh` from 42/22 to 82/72 per the table's own calibration rule.
- **Memory use no longer scales with how long a session runs.** The memory-usage optimization
  programme changed how every front end moves audio: the CLI's encode commands stream their
  input and their output (a 3-minute 5.1 encode peaked at 437.8 MiB before the programme and
  9.3 MiB after; decode 217 → 28.5 MiB; `spdif` — whose IEC 61937 payload runs at the 4×
  carrier rate — 225.7 → 18.0 MiB; an hour of `eac3-silence` 205 → 8.7 MiB), and every
  output-producing command holds keep-partial and error semantics exactly as before, verified
  byte-for-byte against pre-change binaries in every case. GUI recordings now stream to disk as
  they encode for the containers whose format permits it (elementary, Matroska, MPEG-TS, the
  IEC 61937 carrier), so a crash partway through a recording no longer loses the audio already
  captured. The WASM demo gained real memory ceilings and reports an out-of-memory error instead
  of the tab being killed.
- **The codec's own per-frame allocation churn is down 85–88 % on encode and 54–61 % on
  decode.** Working buffers that were freshly allocated every 32 ms frame — the exponent
  strategy plan, the coupling work set, the E-AC-3 encoder's whole per-(stream, block) MDCT
  spectrum set, the decoder's AHT and enhanced-coupling stores among them — are now owned,
  reused storage with an every-field reset discipline, bit-exact by construction and verified
  bit-exact in practice (AC-3 encode: 225,028 → 26,778 bytes and 286 → 86 allocations per
  frame on the measured runner). The E-AC-3 decoder's per-substream state moved from
  `std::map`s onto flat 32-slot arrays — the identity key space is exactly [0, 32) — for O(1)
  lookup and zero setup allocations. Every step is recorded on the new memory trend, which now
  gates regressions the same way the timing series always has.
- **`apps/` now holds every platform-facing target, and internal naming matches it.**
  `platform/{cli,gui,wasm,android}` moved to `apps/{cli,gui,wasm,android}`; `src/lib` (the codec
  core) is now `src/forge`; `src/adm_bridge` is now `src/admbridge`; and `ac3::audio`'s former
  three-way split (`ac3::platform`/`ac3::capture`/`ac3::sinks`) retired in favour of one
  consolidated `ac3::audio` namespace and header tree. None of this is installed/public surface
  except where called out separately below, so it only affects building from source, not an
  existing library consumer.
- **`apps/cli/main.cpp`'s single ~6,100-line file is being broken into one file per command group
  under `apps/cli/commands/`.** The shared parsing/I/O/metering support layer, the `src=`/`map=`
  multi-source subsystem, and the container-wrapping, audio-hardware, synthetic-signal-generator,
  Atmos, and real-material-encode command groups have moved out so far, each verified with a full
  rebuild and the whole test suite; `main.cpp` itself is down to 1,763 lines, with the decode and
  level/loudness/spdif command groups still to move. The command dispatch table
  (`kCommands`) — the thing that keeps an argv index from ever being silently wrong — is untouched
  throughout.
- **Build- and test-tree hygiene**: `scripts/` and `tools/` merged into one
  `tools/{checks,generators,references,ci}/` convention; the six top-level `requirements-*.{in,txt}`
  files moved into `requirements/`; `tests/` regrouped from ~53 flat files into subdirectories
  mirroring `src/forge/include/ac3/<namespace>/`'s own granularity, folding in a stalled
  platform/CRT axis split along the way; `CMakePresets.json`'s test and package presets
  deduplicated behind hidden base presets; the `examples/` target's separate output directory (and
  the DLL-copy machinery it required on Windows) removed by building examples alongside the shared
  libraries like every other target already does.
- **The installed CMake export set is now named `forgeTargets`, not `ac3forgeTargets`**, matching
  the bare-component-name convention every other export set here already uses (`matroskaTargets`,
  `mp4Targets`, `mpegtsTargets`, `capiTargets`) — it was the one export set named after the whole
  package instead of its own component. Anything referencing the old `ac3forgeTargets.cmake`
  filename directly (rather than going through `find_package(ac3forge)`, which needs no change)
  will need updating.
- **[CONTRIBUTING.md](CONTRIBUTING.md) now documents the repository's actual layout rule** — an
  `ac3/<name>/` header prefix means the component depends on `ac3::forge`, a bare `<name>/` prefix
  means it's deliberately codec-blind, and the C API is the one deliberate exception (depends on
  the codec, but isolated as a C surface) — plus the `apps/` vs `src/` split and the per-backend
  directory pattern. The docs site's nav also got a pass: the four data-trend pages now sit
  contiguously, `docs/project/history.md` moved to `docs/history.md` alongside its own nav
  siblings, and `apps/gui/icons/` gained a README marking it as generated output.
- **`static-analysis` now enforces correct header inclusion.** clang-tidy's
  `misc-include-cleaner` check joins the curated set the `static-analysis` CI leg gates: every
  symbol used in `src/forge`, `src/matroska`, and `apps/cli` must have its owning header
  `#include`d directly, not merely reachable through another header's transitive includes —
  closing the gap where a file built only because of what a sibling header happened to pull in,
  and would break the moment that sibling's own includes changed. The first run found 548
  pre-existing findings (538 missing includes, almost all standard-library facades — `<span>`,
  `<vector>`, `<expected>`, `<cstdint>`, and similar — plus a couple of `ac3::` types; 10 unused
  includes); all were fixed mechanically with `clang-tidy -fix` as part of this change and
  verified against a full rebuild plus a clean `ctest` run (615/615) before the check joined the
  enforced baseline. See `.clang-tidy`'s own header comment for the full rationale.

### Known gaps

- The macOS `ac3gui.app` is still not Apple-notarized or code-signed — unchanged from
  0.8.0-beta.2; this release signs artifacts with GPG and attests provenance via Sigstore/OIDC,
  neither of which satisfies Gatekeeper. Expect a "developer cannot be verified" prompt on first
  launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA, PipeWire or CoreAudio.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.2] - 2026-08-19

Eighth tagged release. `ac3gui` builds and packages on macOS for the first time — every
platform's release archive now carries a real GUI, not just Windows/Linux's — plus Python
bindings on PyPI and a C API over the encode/decode core.

### Added

- **Python bindings (`ac3forge` on PyPI)**, roadmap F2: a pybind11 module bound directly onto
  `ac3::FrameEncoder`, `ac3::FrameDecoder`, `ac3::Eac3Decoder` and `ac3::oba::AtmosEncoder` —
  numpy-friendly PCM, Python exceptions in place of `std::expected`. `.github/workflows/wheels.yml`
  builds wheels for Windows, macOS and Linux via `cibuildwheel`; publishing to PyPI itself is
  wired up but stays off until a maintainer provisions PyPI trusted publishing — see
  [docs/releasing.md](docs/releasing.md#publishing-to-pypi). See
  [docs/library/python-api.md](docs/library/python-api.md).
- **A C API over the encode/decode core** (roadmap F1), for consumers that can't link C++23
  directly.
- **`ac3gui` now builds, tests and packages on macOS.** The `macos-llvm` CI leg was CLI-only
  since it was promoted out of experimental; it now installs Homebrew's `qt` formula and builds
  the GUI the same opt-in way the four Linux legs do, `ac3gui_qmltests` and a headless
  `ac3gui --smoke` included, and this release's `ac3forge-0.8.0-Darwin.dmg` carries `ac3gui.app`
  for the first time. Getting there needed two real fixes for hangs under Qt's offscreen platform
  plugin, not just turning the option on — see
  [docs/platforms/macos.md](docs/platforms/macos.md#gui-on-macos).
- **A Homebrew Cask for `ac3gui`** is staged at `packaging/homebrew/Casks/ac3gui.rb`, alongside
  the existing CLI-only Formula — a Cask, not a Formula, being the right shape for a prebuilt
  `.app`. Not yet published to the `homebrew-ac3forge` tap; see
  [docs/releasing.md](docs/releasing.md#homebrew-formula-and-cask).

### Known gaps

- The macOS `ac3gui.app` is not Apple-notarized or code-signed — this release signs artifacts
  with GPG and attests provenance via Sigstore/OIDC, neither of which satisfies Gatekeeper.
  Expect a "developer cannot be verified" prompt on first launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA, PipeWire or CoreAudio.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.1] - 2026-08-17

Seventh tagged release. The repository moved from `iainchesworth/ac3forge` to
`iainchesworthlabs/ac3forge`; this release cuts over to the new location and closes out
everything left stale by that move. No AC-3/E-AC-3/Atmos codec or CLI/GUI behavior changed.

### Added

- **CI can build on a self-hosted runner when one is actually online and idle**, per OS, falling
  back to GitHub-hosted otherwise — never as an all-or-nothing switch, and never for fork PRs,
  which always stay on GitHub-hosted regardless of runner availability. See
  [docs/ci-self-hosted-runners.md](docs/ci-self-hosted-runners.md) for the live-check and
  override design.

### Fixed

- **The published docs site was about to go stale at its own URL.** GitHub's repo-transfer
  redirect covers `github.com/<owner>/<repo>` paths (blob/tree/actions/releases), but the default
  GitHub Pages URL is owner-scoped with no such redirect — `iainchesworth.github.io/ac3forge`
  would 404 once this repo's `gh-pages` branch (now under `iainchesworthlabs`) next deployed.
  Docs now publish to and link from `iainchesworthlabs.github.io/ac3forge`.
- **Dependabot auto-merge silently stopped working after the transfer.**
  `dependabot-auto-merge.yml`'s repository guard hardcoded the pre-transfer
  `iainchesworth/ac3forge` slug; since `github.repository` now reports
  `iainchesworthlabs/ac3forge`, the job's `if` condition never matched, so no Dependabot PR
  auto-merged since the move.
- Roughly 40 hardcoded `iainchesworth/ac3forge` repo-path links across docs,
  README/ROADMAP/CONTRIBUTING/SECURITY, `mkdocs.yml`, and the vcpkg portfile updated to
  `iainchesworthlabs/ac3forge`. PR/issue references that predate the transfer
  (`docs/wasm-demo.md`'s `#168`/`#169` links) were deliberately left as-is — GitHub's redirect
  still serves them, and rewriting would misrepresent when they were filed.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.7.0-beta.1] - 2026-08-17

Sixth tagged release. The main change is an AC-3 quality push: three independent fixes to the
encoder's bit allocation — weighing delta segments against their own cost at every layout, raising
`dbpbcod` past the spec's own recommendation, and giving the LFE its own fine SNR offset instead
of a shared one — move the 5.1 landscape leg at 448 kbit/s from 2.98 dB behind FFmpeg 8.0.1 to
0.72 dB ahead of it, with perceptual quality unchanged. Finding and fixing those relied on new
verification infrastructure landing alongside them: a fuzz harness over the encoder's own input
space (as opposed to only the decoder's), an opt-in encoder/decoder mirror self-check, and a codec
matrix now driven by real programme material rather than synthetic tones — which is what caught a
stale coupling-channel delta cursor and a frame-ending mid-delta bug that had escaped every
existing gate. Also landing this release: a native PipeWire audio backend for Linux, a shared app
icon and About dialogs across every GUI surface, and an E-AC-3 `auto` tool set that picks
coupling/spectral extension/AHT from the per-channel bitrate instead of taking on/off flags as
given.

### Added

- **An opt-in AC-3 encoder/decoder mirror self-check (`ac3::verify`)**, which decodes every frame
  the encoder just emitted with this project's own decoder and diffs the decoder's model against
  the encoder's own — per-block bit offset, decoded exponents, bit allocation and delta correction.
  Motivated by a bug where `deltbaie == 0` was written to mean "no delta this block" instead of
  §5.4.3.47's "keep the previous block's" — the decoder kept a stale correction, mantissa fields
  were then sized differently on each side, and the failure surfaced two blocks later as an
  exponent walking outside 0..24, misdirecting the investigation into the wrong file entirely. The
  self-check catches that class of bug structurally, at the block where the two models first part
  company, rather than at whatever `§7.10.2` guard the misaligned bits happen to trip first. Off by
  default (`EncoderConfig::trace`/`DecoderConfig::trace` are null pointers, costing one branch per
  block and no allocation); `ac3::verify::MirrorEncoder` drives the encode-decode-compare loop for
  a caller that wants it. AC-3 (`FrameEncoder`/`FrameDecoder`) only for now — E-AC-3 computes its
  delta bit allocation once per frame rather than carrying it block to block, so it is not exposed
  to this specific bug class, and Annex E's dependent-substream/transient-pre-noise holdback
  machinery would need its own instrumentation design rather than reusing this one as-is.
- **A property/fuzz harness over the AC-3 encoder's own input space**
  (`tools/fuzz_encoder_space.py`). Every fuzzing target this project had mutates an
  already-encoded bitstream, which asks whether the *decoder* survives corrupt input; the codec
  matrix walks a hand-enumerated list of command lines against one bootstrap tone. Neither has
  any notion of option *combinations*, and neither varies the input material. This one draws
  random legal encoder configurations crossed with adversarial PCM whose character can change
  part-way through a frame — which is what drives exponent-run splits, block switching and the
  delta bit allocation — then holds every resulting stream against both this project's decoder
  and FFmpeg's strict decode. Motivated by the `deltbaie` defect below, which produced streams
  both decoders reject and escaped every existing gate; reverting that fix, the harness finds
  rejected streams within seconds. Runs bounded on every pull request (in the FFmpeg-oracle
  job) and deeper nightly, mirroring how `fuzz.yml` already splits short from nightly.
- **A new `auto` E-AC-3 tool set, which picks coupling/spectral extension/AHT from the
  per-channel bitrate** instead of taking the on/off flags as given. Every Annex E tool trades
  waveform fidelity for a band it can describe more cheaply than it can code, so each is a win
  below some rate and a loss above it — `auto` applies the measured crossovers (56 kbit/s per
  channel for spectral extension; `12 + 14n` for coupling, whose saving scales with how many
  channels share the band). It still honours an explicit `cpl:N`/`spx:N`/`aht:N` band-edge pin,
  so geometry stays steerable without taking over the decision.
- **A native PipeWire audio backend for Linux** (`src/audio/src/platform/pipewire/`,
  `AC3FORGE_WITH_PIPEWIRE`), selected via pkg-config when ALSA's headers are not present.
  Live capture and monitor playback are genuine `pw_stream` PCM; IEC 61937 bitstream passthrough
  negotiates PipeWire's own compressed-format API for real
  (`SPA_MEDIA_SUBTYPE_iec958`/`spa_format_audio_iec958_build()`/`PW_STREAM_FLAG_EXCLUSIVE`), but
  depends on the target node's `iec958Codecs` having been enabled by the session manager, which
  is outside this library's control — see `src/platform/pipewire/passthrough.cpp` and
  `docs/building.md`'s "Why ALSA still comes first" for the full account, including why ALSA
  keeps precedence over PipeWire when both are present.
- **A shared app icon and About dialogs across every GUI surface.** One procedurally-generated
  mark (`assets/icon/generate_icons.py`, Pillow-based, plus a matching hand-authored SVG) now
  backs `ac3gui`'s window/taskbar icon and packaged `.exe`/`.app` icon, Shield's launcher icon and
  Android-TV Leanback banner, and the WASM demo's favicon. `ac3gui` gained an About dialog and
  Shield an About screen (reached via the TV remote's Info button), both showing real build
  version/git provenance through the existing `ac3::version_details()`, alongside a GPLv3 notice
  and font attribution.

### Changed

- **The AC-3 encoder now gives the LFE its own fine SNR offset instead of copying the one every
  other channel gets.** The bitstream carries a separate `lfefsnroffst`, but this encoder wrote
  the shared value into it, which left the LFE a price-taker in a search it cannot influence: the
  offset search picks the one value at which the frame's *total* mantissa cost fits, and that
  total is set by channels of about 250 bins each. The LFE's 7 bins are rounding error in that
  sum, so its precision was decided entirely by channels 36 times its size — and it lost
  precision at the same rate as them despite costing a fraction as much to serve. Raising only
  its own field by 4 fine steps moves about 12 bits per frame at 448 kbit/s and leaves the
  frame's total mantissa cost unchanged. Measured on two materials (the 5.1 fixture and the
  synthesized full-band decorrelated 5.1) at 192/256/320/384/448/640 kbit/s: LFE SNR up at every
  point, by as much as 5.7 dB, overall SNR never lower, ViSQOL MOS flat.
- **The AC-3 encoder now weighs delta bit allocation against what it costs at every layout, not
  only when coupling is active.** A delta segment is 12 bits of side information taken from the
  same budget that would otherwise buy a higher composite SNR offset, so the encoder already
  re-ran its offset search with delta cleared and kept whichever pass came out higher — but only
  when a coupling channel existed, because that is where a failing test first exposed it. Nothing
  in that reasoning is about coupling, and the layouts that never couple were the ones paying
  most: 5.1 at 448 kbit/s was emitting about ten segments per block, 724 bits per frame, 5% of
  the whole frame. On the 5.1 reference this is worth 0.7 dB.
- **The AC-3 encoder raises `dbpbcod` from the §8.2.12 recommendation of 2 to 3.** `dbpbcod` sets
  the knee below which §7.2.2.5 lifts a band's excitation, so raising it steers bits away from
  bands holding almost no energy and towards the ones that do. Measured on three materials
  (the 5.1 and stereo fixtures and the synthesized full-band decorrelated 5.1) at 192/256/320/
  384/448/640 kbit/s, it improves SNR in every case — by 5.9 dB at 192 kbit/s on the 5.1
  reference, where there are fewest bits to misplace — with ViSQOL MOS flat or better throughout.
  The other four parameters are unchanged: `floorcod` turns out never to bind, and `fgaincod`,
  though worth more still at high rates, regresses at 192 kbit/s.
- Together with the LFE exponent fix below, these move the AC-3 5.1 landscape leg at 448 kbit/s
  from 36.02 dB to 39.71 dB — from 2.98 dB behind FFmpeg 8.0.1 to 0.72 dB ahead of it — with MOS
  unchanged at 3.67. The three are independent and were each measured separately: the delta cost
  check and `dbpbcod` account for 39.13 dB between them, and the LFE fix adds the remaining
  0.58 dB on top.
- **Coupling is now dropped, rather than moved down in frequency, when spectral extension leaves
  it no room.** §E3.3.1 derives the coupling end frequency from `spxbegf`; when that landed below
  the requested `cplbegf` the encoder used to slide `cplbegf` down to meet it, which silently
  coupled from 8.0 kHz where the rate model had asked for 10.2 kHz and made every coefficient
  above 8.0 kHz parametric. On the stereo reference at 192 kbit/s this was worth 6.8 dB of SNR
  (21.6 → 28.5 dB with all tools forced on).
- **The landscape comparison now reports `auto` rather than a forced `all`.** The headline number
  is meant to be what a real user of this encoder gets, the same standard applied to FFmpeg's and
  DEE's own automatic choices; `all` was a configuration this encoder would never itself choose.
  Against FFmpeg 8.0.1 the E-AC-3 stereo leg moves from −11.19 dB to −0.83 dB, and the 5.1 leg is
  unchanged at +0.49 dB.
- **The landscape page shows SNR, LSD and MOS side by side, each with its own vs-FFmpeg/vs-DEE
  delta.** These tools trade waveform fidelity for banded envelope fidelity deliberately, so a
  single-metric headline reported a working tool as a straight loss.
- **The quality landscape page (`docs/landscape.md`) now shows a spectrogram alongside its
  SNR/LSD/MOS numbers** — one stacked original/ac3forge/FFmpeg/DEE image per tracked leg,
  refreshed each release promotion, so there's a visual reference next to the trend numbers, not
  only figures.
- **The CI quality gate now includes an AC-3 5.1 leg.** It was stereo-only, which left the LFE and
  the full channel count with no absolute gate — two separate faults have now shipped through that
  hole. The floor is deliberately loose: the gate decodes with FFmpeg under `-xerror`, so a
  malformed frame fails it as a hard decode error, which is the failure mode both faults had.
- **A new `tools/check_ac3_allocation.py`** reports per-channel and per-band SNR against FFmpeg at
  a matched bitrate, to say *which* part of an allocation gap is worth chasing rather than only
  that one exists. It is what found the LFE fault below.
- **The AC-3 codec matrix (`scripts/run-codec-matrix.sh`) now sweeps real programme material,
  not only synthetic tones.** A stationary sine keeps near-identical exponents in every block, so
  a defect that only appears at a mid-frame exponent-run boundary — exactly the shape of the
  `deltbaie` bug below — was structurally unreachable at any bitrate or layout. The golden
  stereo/5.1 fixtures now run the full encode sweep too, decoded by both this project's decoder
  and FFmpeg's strict decode.

### Fixed

- **AC-3 encoder: a delta bit allocation that ended part-way through a frame produced an
  undecodable stream.** `deltbaie = 0` means "keep the previous block's delta bit allocation",
  not "no delta" (A/52 §5.4.3.47), so a channel whose exponent run stopped wanting a correction
  mid-frame was never told to drop it. The decoder kept applying the stale correction, its bit
  allocation diverged from the encoder's, and every field after that point was read at the wrong
  bit offset — a stream both this project's decoder and FFmpeg reject. Real material hit this at
  several bitrates, 64 and 96 kbit/s stereo among them. E-AC-3 was unaffected.
- **AC-3 encoder: the LFE sent one exponent set per frame however much its level moved.** A
  frame's exponents are the per-bin minimum across the blocks they cover, so a single set for six
  blocks is a set chosen by the loudest of them and every quieter block was then quantized
  against a scale meant for something louder. §5.4.3.15 makes `lfeexpstr` a single bit, and the
  encoder was reading that bit as though it could only ever say "reuse". On the 5.1 reference the
  LFE moves 10–16 dB inside one frame, which cost 12 dB of LFE channel SNR — 56% of the whole
  encode's noise power, on a channel carrying a third of its signal. Worth +0.3 to +3.8 dB
  overall across 192–640 kbit/s (+1.6 at 448), for 18 bits per refresh against a 14336-bit frame.
  Stereo is unaffected, having no LFE.

- **AC-3 coupling channel: delta bit allocation could push corrections past band 50, or land
  them somewhere the decoder never reads.** `choose_delta_segments()` and
  `compute_bit_allocation()` (`src/lib/src/core/bitalloc.cpp`) both started their §7.2.2.6 delta
  band cursor at band 0 regardless of which band a channel's own allocation starts at — harmless
  for fbw/LFE (start band 0), but the coupling channel starts higher, so a literal band-0 cursor
  either overshoot band 50 or wrote corrections into mask bands the coupling channel's own
  allocation never reads. Both FFmpeg and Dolby's own reference decoder require the cursor to
  start at the channel's own start band instead; this project's decoder shared the encoder's
  reading, so the round trip never noticed. Found by the encoder input-space fuzz harness above.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire — the new PipeWire path additionally depends on the
  target node's `iec958Codecs` having been enabled by the session manager, which is outside this
  library's control.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.6.0-beta.1] - 2026-08-17

Fifth tagged release. The main change is Atmos object *decode*: earlier releases could only
encode object audio, and decoding an Atmos file just played its 5.1 bed. The E-AC-3 decoder now
reads OAMD object positions and reconstructs JOC object audio, surfaced through the CLI's
`decode`/`monitor` commands, a new GUI object inspector, and a browser-based WASM demo that
renders real decoded object motion and solos individual object audio. A companion
`verify-objects` mode checks a stream's own EMDF authenticity tag (not Dolby's proprietary
decoder gate — see Known gaps).

Also landing this release: a standalone BW64/RF64 + Audio Definition Model (ADM) reader that
drives a real professional ADM BWF master straight through to a DD+ JOC E-AC-3 stream; two new
container writers — MP4/ISOBMFF (with fragmented MP4/CMAF segmenting plus HLS/DASH signaling)
and MPEG-2 Transport Stream — alongside the existing Matroska writer; full ITU-R BS.1770/EBU
R128 loudness metering and a bitstream-aware delivery-QC command; Raspberry Pi (arm64 Linux) and
a real macOS CoreAudio backend; and the library is now installable through vcpkg.

### Atmos object decode

- **The E-AC-3 decoder reads OAMD object metadata and reconstructs JOC object audio**, closing
  the gap where only the encoder side supported objects. `ac3cli decode`/`monitor` surface the
  decoded object layer directly (including per-object WAV export via `objects_dir`).
- **A new GUI "Inspect objects…" dialog** plays back a decoded Atmos stream's object positions
  and lets you solo individual objects' audio.
- **A browser-based WASM demo** renders real decoded object motion and solo-plays real isolated
  object audio, entirely in-browser.
- **`ac3::signing` gained stream verification** (`verify_atmos_frame`/`verify_atmos_stream`, CLI
  `verify-objects`): checks a stream's own embedded EMDF authenticity tag. This is opt-in and
  separate from Dolby's own decoder gate — see Known gaps.
- **The E-AC-3 decoder now applies dynamic range control** (`drc=`/`heavy`), matching the legacy
  AC-3 decoder; previously accepted and silently ignored.

### ADM ingest

- **A standalone BW64/RF64 + Audio Definition Model parser** (`ac3adm::ac3adm`) reads a
  professional ADM BWF master's object graph into memory, and a bridging layer maps it onto the
  Atmos object encoder's input shape.
- **`ac3cli atmos-adm`** drives both together end to end: a real ADM BWF master straight to a
  DD+ JOC E-AC-3 stream. This module is opt-in (`-DAC3FORGE_BUILD_ADM=ON`, off by default) since
  it needs several Boost header libraries; see [docs/library/index.md](docs/library/index.md).

### Delivery containers

- **A standalone MP4/ISOBMFF container writer**, with a spec-correct `dec3`/`dac3` box, plus
  fragmented MP4/CMAF segmenting and HLS/DASH manifest signaling.
- **A standalone MPEG-2 Transport Stream container writer.**
- **Live capture sessions can mux straight to Matroska.** The GUI's Format tab and the CLI both
  gained the new container options.

### Loudness & delivery QC

- **Full ITU-R BS.1770-4/EBU R128 metering**: momentary and short-term loudness, loudness
  range, and true peak.
- **`dialnorm=auto` finished for multi-source assignments and dual-mono streams**, in both the
  CLI and GUI (dual-mono measures each channel independently).
- **A new CLI `qc` command and GUI QC dialog** audit an already-encoded stream's bitstream-level
  loudness against its embedded metadata and delivery gates.
- **A perceptual-quality (ViSQOL) column** sits alongside SNR in the quality-comparison tooling.

### Platform & packaging

- **Raspberry Pi (arm64 Linux)** is now a supported platform, Pi 4/5 tier (Pi 3 out of scope on
  real-time budget grounds).
- **A real macOS CoreAudio backend** for live capture/monitor playback.
- **The library is installable via vcpkg** (staged in-tree pending submission to the curated
  registry — see [docs/releasing.md](docs/releasing.md#vcpkg-port)): `ac3::forge` plus
  `matroska`/`mp4`/`mpegts` as opt-in container-writer features.

### Fixes

- **AC-3 decode's reported dynamic-range floor was wrong whenever the true minimum sample was
  exactly 0.0 dB** — an accumulator seeded at 0.0 instead of the first real sample silently
  widened the reported range.
- **A flushed E-AC-3 dependent substream (e.g. a height-only pair at end of stream) could crash
  the CLI decoder** instead of writing correct audio, when its channel layout didn't match the
  program's main substream.
- **`fast-mdct=off` is now honored consistently** across all `eac3-encode`/`eac3-encode-multi`
  commands.
- **Piping CLI output to `-` no longer risks corrupting stdout** when `dialnorm=auto` or a
  multi-source summary is printed.
- **The GUI's auto-monitor preference now actually takes effect** on the input rail's Add
  button.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware: DD+ JOC gates
  that on an authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this
  project ships no key for. `verify-objects` checks a stream against its *own* signature, not
  Dolby's gate. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.5.0-beta.1] - 2026-08-15

Fourth tagged release. The main change is a fast-transform performance initiative: an opt-in
FFT-based MDCT was introduced, taken default-on, and then progressively hardware-optimized down
through every transform kernel the encoder touches — the long transform, both block-switched
short transforms, and the opt-in enhanced-coupling tool's DFT — alongside an algorithmic
warm-start for the bit-allocation rate-control search. Measured on the same 5950X release build
throughout, default 5.1 encoding drops from 0.4.0-beta.1's ~3.0 ms/frame to ~0.47 ms/frame (about
6.4×) and 8-object Atmos from ~4.8 ms/frame to ~0.43 ms/frame (about 11×), with SNR held at
+0.000 dB against an independent FFmpeg oracle at every step along the way. Alongside the
performance work: two GUI fixes (object-drag losing its mouse grab mid-gesture, and ambiguous
plan/elevation axis labeling), a quality-trend dashboard fix, and Linux packaging now ships real
`libFOO`/`libFOO-dev`-style system packages instead of one `.deb`/`.rpm` silently bundling the
CLI together with the entire library SDK.

### Performance: fast transforms, default-on and hardware-optimized

- **A new FFT-based fast forward MDCT**, landed opt-in behind `fast_mdct` (off by default): the
  §7.9.4 N/4-FFT structure replaces the direct §8.2.3.2 O(N²) evaluation for the long transform,
  ~25× faster at the kernel level (76.8 µs → 3.1 µs/call) with the direct form kept in-tree as
  the permanent reference/validation oracle. Verified bit-identical-class agreement (peak-relative
  ~3e-15) against the direct form on goldens, random data and real audio, plus **+0.000 dB**
  through an independent FFmpeg oracle at 192–448 kbps.
- **The inverse transform and enhanced coupling's windowing step got the equivalent fix**: `std::cos`/
  `std::sin` calls inside `imdct512_windowed`, `imdct256_pair_windowed` and `ecpl_channel_spectrum`'s
  windowing loop, previously recomputed fresh every call, are now one-time tables. Bit-exact by
  construction (the naive periodic-index shortcut is provably *not* bit-exact for the IMDCT's
  un-reduced angles — documented as a trap so it isn't re-attempted). A real 5.1 E-AC-3 decode
  drops from ~640 ms to ~145 ms (~4.4×).
- **The fast MDCT is now the default everywhere**, with `band_energy` (Atmos's JOC reconstruction
  solve) wired through the same flag — the gap that had capped Atmos's win at ~2.0×. Whole-frame:
  plain 5.1 3.0 → 0.67 ms/frame (~4.5×), 8-object Atmos 4.8 → 0.64 ms/frame (**~7.6×**, up from
  ~2.0× before `band_energy` rode the flag). `fast-mdct=off` (AC-3 commands) / `tools=nofastmdct`
  (E-AC-3) force the direct form back; the old opt-in spellings still parse as no-ops so existing
  run history keeps working.
- **The fast MDCT kernel itself closed to its standalone-prototype speed** (3.09 µs → 903 ns/call,
  a further 3.4×) by moving every angle-dependent value in the §7.9.4 fold — pre/post twiddles and
  the FFT's own butterfly twiddles/bit-reversal — into one-time tables, and switching the FFT to
  split real/imaginary arrays so the auto-vectorizer can see the butterfly's independent
  multiply-add chains.
- **Both block-switched short transforms get their own fast folds**, closing the last kernels still
  running direct-form O(N²) sums under the default `fast_mdct`. Each derives to the same scaled
  DCT-IV core the long transform already uses (877 ns/call vs. 35.8 µs direct — ~41×), removing the
  worst-case real-time hazard on transient-heavy material: a fully block-switched 5.1 frame's
  transform stage drops from ~1.3 ms-class to ~32 µs-class.
- **The opt-in enhanced-coupling tool's `dft512` gets the same FFT treatment** as the long MDCT
  (both now share one `fft_radix2.hpp` core): `ecpl_channel_spectrum`, still the single most
  expensive kernel measured, drops from 277 µs to 47 µs/call (~5.9×). Not run by any default
  encode, but a real-time hazard whenever `ecpl` is enabled.
- **The bit-allocation rate-control search now warm-starts from the previous frame's converged
  offset** instead of a fixed bracket, exploiting that consecutive frames of real material converge
  to the same or a neighbouring value. A stationary frame's ~11 full bit-allocation evaluations
  drop to 2–3; whole-frame time falls a further 18% (5.1) / 11% (Atmos) on top of the kernel work
  above. Brute-force verified against the plain binary search over 4,355 monotone-predicate cases
  with zero mismatches; outputs are byte-identical on every monotone path, and the one path where
  they can legitimately differ (AHT's locally non-monotone cost function) was already
  probe-order-dependent before this change — decoded PCM agrees at 102–115 dB SNR per channel.
- **New performance observability**: Tracy zones across every previously
  unzoned encoder stage, a standalone `ac3kernelbench` micro-benchmark harness timing kernels in
  isolation against real audio, and a per-kernel trend history (non-gating, `::warning::`-only)
  alongside the existing whole-frame performance trend — see
  [docs/performance-trend.md](docs/performance-trend.md).

### Packaging

- **Linux `.deb`/`.rpm` now ship a real `libFOO`/`libFOO-dev` split** instead of one package
  silently bundling `ac3cli` together with the entire library SDK (headers, static archives, the
  CMake package config — confirmed against real `dpkg-deb -c` output, not assumed). `libac3forge0`
  carries just the versioned shared library a linked binary loads at runtime; `libac3forge-dev`/
  `ac3forge-devel` carries everything a builder needs, version-pinned to its exact matching
  `libac3forge0`. Installable with a plain `apt install`/`dnf install` rather than a manual archive
  download — see [docs/releasing.md](docs/releasing.md#what-gets-published). ZIP/TGZ downloads are
  unaffected: `library`+`libruntime` still merge into one `ac3forge-dev-*` archive, exactly as
  before.

### GUI fixes

- **Object-drag no longer loses the mouse grab mid-gesture.** The Objects tab's plan/elevation/
  live-session `MouseArea`s sit inside a `Flickable`-based `ScrollView`, which could steal the grab
  from a child `MouseArea` once movement looked flick-like — most reproducible on the elevation
  view's vertical drag, the same axis `Flickable` watches for scrolling. `preventStealing: true`
  on all five affected `MouseArea`s holds the grab for the whole gesture.
- **The plan and elevation views in the Objects tab are now labeled as what they are** — "(top-down)"
  / "(side-on)" headers, a one-line caption naming which screen axis maps to which room axis, and a
  corrected elevation hint ("drag: depth + height" rather than "drag for height", since the plan
  view's marker moves too during an elevation drag — correct behaviour, previously unexplained).

### Developer tooling

- **The quality-trend dashboard's table no longer conflates unrelated checks.** The chart already
  scoped rows by codec and `isPrimaryCheck`; the table below it rendered the raw, unfiltered record
  list, which let a steady ~25 dB interop fixture read as a crash relative to an unrelated ~68 dB
  series. The table now follows the same Codec scoping as the chart, with a `Check` column and a
  tooltipped `†` marker on non-primary checks.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.

## [0.4.0-beta.1] - 2026-08-14

Third tagged release. The GUI is rebuilt to the canon design handoff — a numbered-rail workflow,
a single assignment table driving all channel routing, an audible timeline with per-source
offsets and motion editing, live capture (including two-device parallel capture with software
clock-drift correction), per-source gain/LFE/resample controls, dual-mono independent DRC,
S/PDIF-wrapped WAV output, four selectable colour palettes with a native system-accent theme, and
a full round of dark-mode fixes. Alongside the GUI work: enhanced coupling's encoder now fits
real angle/chaos coordinates instead of sending them as zero, the decoder accepts Annex E's
default coupling band structure, the EMDF object signer is a committed clean-room library, eight
new library examples ship, an external-encoder (FFmpeg/DEE) comparison joins the quality
dashboards, and Android release builds sign with a real keystore.

### E-AC-3 encoding and decoding

- **Enhanced coupling's encoder now fits real angle and chaos coordinates**, closing the last
  known gap from 0.3.0-beta.1's enhanced coupling work — it no longer sends angle/chaos as zero.
  Amplitude and angle are solved as an exact 2-variable linear least squares per band (§3.5.5.4's
  reconstruction is linear in the complex gain a band's amplitude/angle pair expresses); chaos is
  chosen by searching its 8 legal codes directly against the decoder's own deterministic
  de-correlation sequence and keeping whichever reconstructs closest to the source, rather than
  estimated from a statistical proxy. Quality on ordinary material is unchanged (a correlated
  signal's best fit lands near angle zero anyway); the case the amplitude-only fit could not
  represent at all — two channels' different content forced into the same narrow coupling band —
  improves measurably, from a ~3 dB floor to ~6 dB, without threatening the coding tool's own
  structural limit on how much a single coordinate per band can ever separate.
- **E-AC-3 stereo (2/0) rematrixing** — the bitstream syntax and decoder undo path have existed
  since 0.2.0-beta.1; only the encoder's own §7.5.3 minimum-power decision was missing, and it
  turned out to need no new logic at all, just the same rule AC-3's own encoder already makes,
  over the same Table 7.25 bands (Annex E only changes how many of the four are active, not their
  boundaries or the rule itself).
- **The decoder now accepts Annex E's legal default coupling band structure (`cplbndstrce=0`)**
  instead of rejecting it with `DecodeError::kUnsupported`. This project's own encoder always
  transmits an explicit band structure, so the default path had only ever been exercised against
  the encoder's own output — decoding FFmpeg's E-AC-3, which legally chooses the default, failed
  immediately. The root cause was a stale assumption that Table E2.12's array needed
  relative-to-`cplbegf` indexing; cross-checked against FFmpeg's own `decode_band_structure()`,
  the table is indexed absolutely from `cplbegf == 0`. A permanent regression fixture (a real
  FFmpeg 8.0.1 encode with nonzero `cplbegf`) now covers this in the gold-reference gate.

### Atmos object signing

- **The EMDF object signer is now a committed, clean-room library (`ac3::signing`)** rather than a
  gitignored overlay. The HMAC-SHA-256 construction and the layout of what gets signed are in-tree
  and dependency-free; the **key** is the only secret and is supplied by the operator at runtime,
  never embedded and never written to disk. `ac3cli atmos` gains `sign-objects` with
  `signing-key=<path>` (or the `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` env vars);
  signing engages only when both a request and a key are present. The Shield app reads its key from
  a bundled `signing.key` asset written from the `ATMOS_SIGNING_KEY` CI secret at build
  time. See [docs/concepts/object-signing.md](docs/concepts/object-signing.md).

### GUI: canon workbench redesign

- **The desktop GUI is rebuilt to the canon design handoff**, replacing the earlier workbench that
  had drifted from it — the numbered rail (01 Input / 02 Levels / 03 Soundfield), plan strip,
  two-tier bitrate picker, routing strip and command bar now match the handoff, landed via a
  6-agent conformance sweep against the mockup (~70 fixes across CLI parity, run history, timeline
  editing, live-tab truth and guided copy).
- **A single assignment table now drives channel routing everywhere**, replacing the free-text
  token field that only appeared once a second source was loaded. Each source channel gets one
  destination dropdown (bed position / a new object / programme / nothing); sending a channel to
  an object turns object mode on, fixes the 5.1 bed, and raises the rate to ≥384 kbps atomically.
  In object mode, a channel assigned to a bed position becomes a static object pinned at that
  speaker's azimuth; unassigned channels drop with a named warning, and encoding enforces the
  sixteen-object cap over dynamic + pinned together.
- **Meter and soundfield redraws no longer tear down and rebuild ~30x/second.** The 30 Hz level
  stream previously rebuilt fresh JS arrays (and every delegate) on every tick; meter/soundfield
  models are now layout-keyed and read by index, and encode-progress/object-drag updates are
  coalesced onto the ~30–60 Hz publish cadence instead of flooding the GUI event queue per frame.
- **A real first-run screen, Preferences dialog and honest run history** round out the shell: first
  run synthesizes a bundled 5.1 test signal into a real WAV; Preferences persists via `QSettings`;
  and run history, failure-banner actions, and the live tab now reflect actual encoder/session
  state rather than mockup placeholders.

### GUI: timeline & time model

- **Timeline length is now derived, not fixed** — `max(offset + duration)` over every loaded
  source, rather than a hardcoded 8 s.
- **Each source gets an independent start offset**, settable from a rail numeric field or by
  dragging its clip band, applied as leading silence in both the channel and object encode loops
  and the meter preview — and reproducible on the command line via a new `offset=` CLI token.
- **Keyframes stay programme-absolute when a clip is dragged**; Shift-drag explicitly carries a
  source's object keyframes along by the same delta (clamped at 0), so a plain drag no longer
  silently drags authored motion with it.
- **Zoom (wheel/button, up to 40x) and snap** — ruler-tick and drag-snap tiers at 1 s / 0.1 s / a
  32 ms floor — move together as the view scales.
- **The Preview button is now audible**: it renders every object through the Atmos encoder and
  plays the 5.1 bed back live through the monitor sink, paced in real time with the playhead
  following the audio clock.
- **Object identity is now keyed by (source, channel)** instead of position in the dynamic-object
  list, so reassigning a channel or removing a non-primary source no longer silently migrates or
  destroys motion belonging to a different or surviving channel.
- **`atmos-encode` gains an optional keyframes-file argument**, matching `atmos-path`'s grammar;
  the GUI's "Export paths…" writes that exact format, closing the last gap in object-mode CLI
  reproducibility.

### GUI: live session and two-device capture

- **A live take now streams to disk incrementally** instead of buffering the whole session in RAM:
  an elementary-stream take *is* the growing output file, muxed to Matroska once at a clean stop,
  so a crash still leaves the elementary take behind. An optional raw-WAV safety copy streams the
  untouched captured PCM alongside it.
- **A silence watchdog fails a session ~3 s after a capture device goes quiet**, instead of the
  transport reading "Running" forever against a vanished device, with a "Choose another device"
  recovery action on the resulting failure banner.
- **Live Atmos sessions pre-allocate a fixed object-slot budget** rather than baking the capture
  device's channel count straight into the JOC stream, so objects can be added or reassigned to a
  different capture channel mid-session.
- **Changing the receiver — or toggling passthrough — mid-session now hot-swaps the passthrough
  sink** on the worker thread between frames, without restarting capture or encode.
- **A live session can now pace a second capture device off the first's clock in software.** The
  master device's delivery paces the frame loop as before; the second device is conformed to the
  master's clock via a streaming linear-interpolation fractional resampler and a proportional
  drift-correction servo, since there's no shared hardware clock between two independent capture
  endpoints. Available from the GUI and from `ac3cli`'s new `live capture2=<index>` token, with
  the slave device's measured drift correction visible in the chain's capture cell. A plain
  channel-mode session's bed still comes from the master device alone — there is no principled
  default position to auto-pan a second, independent device's audio into.

### GUI: source gain, metering, and format/output controls

- **Per-assignment gain/trim** on the channel routing table, applied inside the same routing
  matrix that drives encode, meter preview, and fed-channel flags.
- **Source-side metering pips**: a whole-programme, pre-routing peak/RMS reading per loaded file
  source.
- **Resample-on-load**: adding a source at a different sample rate than the primary no longer
  refuses outright — it resamples to the primary's rate via an offline windowed-sinc polyphase
  resampler and labels the row accordingly; the refusal survives only when the primary's own rate
  has no legal AC-3 target at all.
- **LFE low-pass filtering**: a full-bandwidth channel explicitly routed onto LFE through the
  assignment table now runs through a 120 Hz 4th-order Butterworth low-pass in preview and
  channel-encode. Automatic single-source routing (a file's own dedicated LFE channel) stays
  bit-exact.
- **CLIP latches per channel** in the meters — once lit, stays lit until clicked or a new
  transport starts.
- **`objm` fold-to-mono**: the range grammar (`0.1-2:objm`) can now fold a contiguous run of one
  source's channels into a single dynamic object.
- **Dual-mono programmes get independent DRC.** A/52 §7.7.1/§7.7.2.2 give 1+1's two programmes
  independent DRC curves and heavy-compression ceilings, but the encoder was building the second
  programme's controller from the first's own config. CLI gains `drc2=`/`heavy2`/`ceiling2=`/
  `dialogue2=`; GUI gains a Programme 2 DRC combo and a "Heavy compression — programme 2" card.
- **A third container option: S/PDIF-wrapped WAV**, reusing the existing IEC 61937 burst-wrapping
  machinery. Works for both codecs — E-AC-3's carrier runs at 4x rate.
- **An advisory bit-rate floor for wide layouts**: a muted hint under Bit rate when the CBR rate
  works out to fewer than ~77 kbps per full-bandwidth coded channel. A hint, not a gate.
- **Guided now applies measured loudness and film-standard DRC automatically** while it's driving
  and Loudness/Metadata is untouched this session; dual mono gets the DRC-only half of the
  contract on both programmes, since loudness measurement is refused there.

### GUI: guided-mode workflow polish

- **Finished run chips now carry their own Play action**, sending that run's own output to a
  receiver — not whatever the most recent encode happened to produce.
- **Run history now survives a restart.** The last 30 completed runs persist to Settings as JSON;
  clicking a run chip opens a details popover with status, rate, duration, size, frames, failure
  text, and the `ac3cli` command line snapshotted when that run started.
- **Guided's amp destination now auto-picks a bitstream-capable output device** — the first device
  that can carry the prospective encode plan — with a "Choose a different device" override and a
  stated reason when nothing qualifies.
- **Guided's Movement step, once object mode is on, offers two cards**: *Everything moves* (every
  loaded channel becomes an object) and *Keep the bed, add movers* (only claims still-unassigned
  channels).
- **Good/Better/Best now maps to VBR quality, not a fixed bitrate**, when a VBR default or an
  already-selected Variable rate mode applies — Guided's Quality step rate cards set a VBR quality
  target (40/75/90) instead of a CBR number.
- **Preferences defaults apply on Save to untouched fields only**, generalising the existing
  loudness-touched contract to container/rate mode/bit rate/VBR quality.
- **The guided wizard's Back/Next footer no longer disappears off-screen.** It previously shared
  the tab `StackLayout`, whose implicit height is the max over every page — inheriting the Format
  tab's height let the footer stretch a full screen below the visible content. The wizard now owns
  its own surface outside the tab stack: the step bar and footer stay pinned, only the step content
  scrolls between them.
- **The always-on `ac3cli` command bar is now a popover.** Encode runs the encoder in-process, so
  the full command line is reference material, not the primary act: a compact chip opens a popover
  with the complete line, wrapped, with Copy.
- Fixed the runs lane's empty-state text riding the top edge instead of centring in the strip.

### CLI

- **Fixed: a bare `heavy2` token was silently misparsed** as `encode`/`eac3-encode`'s optional
  `in2.wav` positional instead of enabling Ch2 heavy compression — `run_main`'s bare-token
  classifier was missing it alongside `couple`/`heavy`/`mixmeta`/`sign-objects`/`keep-partial`.
- **`keep-partial` token**: a bare trailing-options token that keeps whatever frames
  `encode`/`eac3-encode`/`atmos-encode` already produced before a failure, at
  `<name>.partial.<ext>` — mirrors the GUI's own keep-partial-output preference.

### GUI: theming

- **Four selectable colour palettes, including a native system-accent theme.** *Signal* (the
  design system's red, default), *Ink* (cool greys, cobalt accent), and *Console* (warm greys,
  studio amber) join *System* — a new `SystemTheme` singleton that reads the platform's native
  accent colour and re-announces on OS colour-scheme changes, so changing the OS accent colour
  restyles the running app live. All four are selectable in Preferences → Appearance.
- **Dark mode is now hand-tuned per palette instead of a mechanical inversion of the light ramp.**
  The previous approach turned near-white accent tints into murky red-blacks and left the
  fully-saturated accent glaring against near-black; each palette now defines both modes by hand.

### GUI: dark-mode audit fixes

- **A round of dark-mode fixes found by auditing every tab across all four palettes.** Smoke-mode
  screenshot captures are now hermetic — session restore previously ran at window creation, so a
  screenshot inherited whatever session the last run saved, and closing the smoke binary could
  clobber the user's real saved session with smoke state. The Coding tools tab now explains itself
  instead of rendering a bare void when object mode or plain AC-3 hides its contents. The runs
  lane's hard-capped height had exposed a horizontal scrollbar overlaying the chips and eating
  their clicks — the scrollbar is now off, wheel/drag still pan. The Encode button's `.ac3`/`.ec3`
  suffix no longer goes stale after the codec moves the plan between containers.

### Quality & verification tooling

- **Added an external-encoder landscape comparison against FFmpeg and Dolby DEE**, giving the
  encoder a real point of reference beyond its own gold-reference gate. A new stereo fixture
  exercises coupling, enhanced coupling, spx, AHT, transient pre-noise, and rematrixing together; a
  local-only baseline tool encodes fixed legs through FFmpeg, DEE, and `ac3cli`, while CI itself
  runs a compute-only trend mode scoring against those legs using only this project's own decoder —
  no FFmpeg or DEE invocation at CI time. Results render in two new docs pages,
  `docs/tool-comparison-trend.md` (per-commit, per-variant detail) and `docs/landscape.md`
  (release-over-release headline table). This work directly surfaced the `cplbndstrce=0` decoder
  gap fixed above, and found that the installed DEE build silently drops the Ls channel on discrete
  5.1 input — the affected rows are honestly marked `"status": "unverified"` rather than reporting
  a fabricated score.
- **The gold-reference gate now checks a real Annex E tool-enabled stream (`tools=cpl`)**, not just
  the `tools=none` baseline, at the existing 55 dB SNR floor. `spx`/`aht`/`all` are deliberately
  left off this specific check: those tools are approximate/generative reconstruction where two
  independent spec-correct decoders legitimately diverge much further, so a 55 dB floor would
  false-fail on normal divergence rather than catch a real regression.
- **The quality trend chart and tool-comparison trend chart both gained a per-series breakdown
  view** ("Worst of legs, by branch" / "By platform leg", and "By branch" / "By variant"), so one
  CI leg — or one Annex E tool-set — quietly drifting relative to its siblings is visible as a
  trend line instead of only by scanning table rows.

### Android (Shield)

- **Android release builds now sign with a real release keystore instead of the debug key**, once
  a maintainer has provisioned the `ANDROID_KEYSTORE_*` secrets per
  [docs/releasing.md](docs/releasing.md). Local dev, ordinary CI, and any release run with no
  keystore provisioned all still degrade to the debug keystore exactly as before.

### Bug fixes

- **Windows audio backends no longer list a blank row in the device picker.** A real WASAPI
  endpoint that never fills in its friendly-name property was enumerated with an empty display
  string, and both the capture and passthrough front ends put that straight into a combo box as an
  unlabeled entry. The fix resolves a display name through a fallback chain (friendly name → device
  description → an endpoint-id-carrying stand-in), and an endpoint whose id can't be read is now
  skipped entirely rather than listed.

### Library examples & documentation

- **Eight new `examples/` programs**, each a build target and `ctest` entry like every other
  example: `wav_roundtrip` (real WAV file I/O, not just in-memory PCM), `custom_layout` (a
  channel selection no named `LayoutId` covers, via `Plan::custom_locations`),
  `multi_source_assignment` (combining separate sources via `ac3::plan::Assignment`),
  `scripted_object_motion` (authored `KeyframePath`/`OrbitPath` driving `AtmosEncoder`),
  `object_signing` (`ac3::signing::sign_atmos_stream`, previously undemonstrated),
  `level_metering` (`ac3::analysis::LevelMeter`/`energy_vector`), `decode_robustness`
  (recovering from one damaged frame in an otherwise-good stream via `ac3::split_frames`), and
  `atmos_fallback` (`AtmosConfig::emit_object_metadata`'s objects-or-nothing design decision,
  side by side). Three new library reference pages —
  [Channel plans & routing](docs/library/channel-plans-and-routing.md),
  [File I/O](docs/library/file-io.md) and [Object signing](docs/library/signing.md) — and new
  sections on the existing [Spatial & Atmos objects](docs/library/spatial-and-atmos.md),
  [Decoding](docs/library/decoding.md) and [Muxing & sinks](docs/library/muxing-and-sinks.md)
  pages are written from them.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.

## [0.3.0-beta.1] - 2026-08-11

Second tagged release. Adds the two remaining Annex E coding tools (enhanced coupling,
transient pre-noise processing), a native Android app on NVIDIA Shield TV, packaged
`find_package(ac3forge)` libraries for third-party consumers, explicit multi-source channel
assignment, and a GUI tier split for first-time users through experts.

### E-AC-3 encoding and decoding

- **Enhanced coupling (§E3.5)** and **transient pre-noise processing (§3.7)**, the two Annex E
  tools the decoder previously recognised but refused (`DecodeError::kUnsupported`) — now
  implemented end to end, encoder and decoder, each behind its own tool token (`cpl+ecpl`,
  `tpn`). Enhanced coupling round-trips at the same ~20dB near-transparent bar as standard
  coupling for realistic content; transient pre-noise processing follows the spec's own
  time-scaling synthesis pseudocode, reusing the existing block-switch transient detector rather
  than a second one.
- Fixed two real conformance bugs found implementing the above: a missing §3.3.2 `nrematbd`
  formula for `ecplinu` (both encoder and decoder), and a systematic 2:1 gain error in enhanced
  coupling's FFT-based reconstruction pathway.
- `Eac3Decoder::decode_substream` now returns an optional decoded substream plus a new
  `flush()`, since transient pre-noise processing can hold a frame back until the next one
  confirms whether a correction reaches into it. Streams that never use the tool see no
  behavioural change.

### Dolby Atmos objects and multi-source encoding

- **Explicit multi-source channel assignment** alongside automatic routing — `ac3cli`'s encode
  commands take `src=`/`map=` to assign specific input files/channels to specific output
  channels and objects, instead of relying purely on automatic layout inference.
- Object mode now addresses objects by source, not a stale positional index, so multi-source
  sessions keep object identity stable as sources are added or reordered.

### GUI

- **Guided/Advanced/Expert tier split**: a real step-by-step wizard for first-time users, with
  Advanced and Expert tiers exposing the same controls power users had before.
- Multi-source input and an explicit per-channel assignment surface in the GUI, mirroring the
  CLI's `src=`/`map=`.
- **Dual mono (1+1) as a bed**, not a distinct layout — it now feeds the same object/motion
  pipeline as any other bed.
- **Variable bit rate** as a selectable GUI rate mode (a quality target with optional min/max
  kbps bounds), alongside CBR.
- Live sessions no longer clobber a file's authored objects when a live capture starts, and warn
  before silently dropping VBR settings that don't apply live.
- A Qt Quick Test harness drives the real `EncoderController` end-to-end, not a mock, for GUI
  regression coverage.

### Android (Shield) — new platform

- **ac3forge on NVIDIA Shield TV**: a native Android app (`platform/android/`) pairing
  `ac3::forge`/`ac3::audio` via JNI with a live Atmos demo — authored object trajectories,
  deflection, and ambient object motion, encoded and rendered on-device.
- HDMI receiver resilience hardening for the Shield demo, so a receiver renegotiating format
  mid-playback doesn't drop the session.
- Ships as a debug-signed `.apk` this release — see Known gaps.

### Library and packaging

- **`find_package(ac3forge)` support**: `ac3::forge` and `matroska::matroska` now build as
  proper static and shared CMake targets with `install()`/export support, so a third-party
  project can consume them without vendoring the source tree. `ac3::audio` (live capture/
  monitor/passthrough) stays CLI/GUI-internal, not part of what's installed.
- `ac3::forge` split into a platform-independent codec core plus `ac3::audio`, clearing the way
  for the library package above and for platforms — like Android — that only want the codec.

### Quality and packaging infrastructure

- Quality-trend dashboard redesign (readability, tightened gate thresholds) and a fix for CI
  concurrency dropping quality data mid-run.
- A round of security hardening prompted by OpenSSF Scorecard: hash-pinned CI tool installs,
  commit-SHA-pinned GitHub Actions (replacing tag-pinned ones), a `SECURITY.md`
  vulnerability-reporting policy, patched CVEs in docs dependencies, branch-protection scoring
  wired up, and build provenance republished as `.intoto.jsonl` for Scorecard to read.
- Several MSVC `/analyze` and clang-tidy findings fixed for real: heap-allocating large
  encoder/decoder objects out of worker-thread stacks, reusing MDCT scratch buffers instead of
  stack-declaring them per call, and a couple of static-analysis false-positive suppressions.
- macOS packaging now stays a single `.dmg` bundling both the runtime and library components,
  matching the archive packages' intent — CPack's DragNDrop generator defaulted to splitting
  per component the first time this leg actually ran on real macOS CI, caught by this release's
  own packaging dry run.

### Known gaps

- The Shield `.apk` ships debug-signed via Android's default debug keystore — no release
  keystore is provisioned in this repo yet, so it's a sideload-only build, not one suited for
  store distribution.
- Enhanced coupling's encoder always sends angle/chaos as zero (an amplitude-only fit) — quality
  degrades if two channels' content shares one narrow coupling band. Closed in
  [0.4.0-beta.1](#040-beta1---2026-08-14).
- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.

## [0.2.0-beta.1] - 2026-08-10

First tagged release. ac3forge is a clean-room AC-3 and E-AC-3 encoder and decoder in C++23,
implemented from the published standards — no FFmpeg or other codec library is linked, only
used during development as an independent oracle to check output against.

### AC-3 and E-AC-3 encoding

- Every AC-3 coding mode (1+1 dual mono, 1/0 through 3/2, each with or without LFE) at 48,
  44.1 and 32 kHz, CBR only, across all 19 nominal Table 5.18 bit rates. Exact 44.1 kHz timing
  via Bresenham alternation between the two legal frame lengths.
- E-AC-3, all of the above plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams, and
  either CBR or VBR (a quality target with optional min/max kbps bounds) per substream.
- Per-block, per-channel block switching (§8.2.2 transient detector, long 512-point vs. switched
  256-point transform pairs), automatic delta bit allocation (§7.2.2.6), and 2/0 rematrixing
  (§7.5.3) on AC-3.
- Channel coupling (§7.4 / §E3.3), spectral extension (§E3.6) and the adaptive hybrid transform
  with gain-adaptive quantization (§E3.4) on E-AC-3, each opt-in per stream.
- `fscod2`, Annex E's half sample rates (24, 22.05, 16 kHz).

### Dolby Atmos objects (Joint Object Coding)

- Mono sources placed and moved in 3D space, panned into a 5.1 bed with OAMD + JOC metadata
  carried in an EMDF container (ETSI TS 103 420) — playable as plain 5.1 by any decoder, and
  reconstructible as discrete objects by one that understands the container.
- Authored keyframe paths and closed-form orbits for object motion, both file-driven
  (`ac3cli atmos-path`) and live per-frame (`ac3cli live --atmos`).
- Syntax checked field-for-field against Dolby's own Reference Player and Dolby Media Encoder.

### Decoding

- A single in-repo decoder core shared with the encoder, reading both AC-3 and E-AC-3 —
  dependent substreams, `chanmap`, and the §E3.8.2 render — at float32-precision parity with
  FFmpeg on every layout FFmpeg itself can read.
- All three Annex E coding tools (coupling, spectral extension, AHT) decode individually or all
  stacked together, at every channel layout including 7.1.4 — the one combination FFmpeg cannot
  check at all, since its parser refuses a second dependent substream.
- Block switching and dual mono decode on both formats; decoded switch decisions are reported
  back (`DecodedFrame::blksw`), the same tier of diagnostic as `dynrng`.

### Metadata

- `dynrng` (five DRC profiles: film-standard, film-light, music-standard, music-light, speech),
  `compr` heavy compression, measured `dialnorm` (ITU-R BS.1770-4 gated loudness), and downmix
  levels (`cmixlev`/`surmixlev`, the E-AC-3 `mixmdate` group) — verified against FFmpeg applying
  the metadata, not just against the encoded bits.

### Live audio, capture and passthrough

- WASAPI (Windows) and ALSA (Linux) backends for live input/loopback capture, shared-mode
  monitor playback, and exclusive-mode S/PDIF (IEC 61937) bitstream passthrough — AC-3 and
  E-AC-3/Atmos alike.
- A lock-free SPSC ring carries samples from capture into the encoder; `ac3cli live` wires
  capture → encode → monitor/passthrough continuously.
- `MonitorSink` playback confirmed against real Windows hardware, including a live
  microphone-capture-to-monitor session; ALSA verified headless (WSL2 has no sound devices) plus
  under AddressSanitizer/UndefinedBehaviorSanitizer with leak detection.

### Tools and formats

- `ac3::io::scan`: derives stream format, access-unit boundaries and channel count directly from
  the bitstream.
- `matroska::matroska`: a standalone MKV muxer, independent of the codec library.
- `ac3::sinks::iec61937`: S/PDIF burst packing, byte-exact against FFmpeg's `spdif` muxer for
  AC-3 and independently verified against Microsoft's own IEC 61937 documentation for E-AC-3.
- `ac3::analysis`: peak/RMS/loudness metering with console ballistics and the Gerzon energy
  vector, shared by both front ends.
- `ac3cli`, a 21-command command-line front end, and `ac3gui`, a Qt Quick GUI with file and
  live-capture encoding, an object placement/motion view, and channel-level metering.

### Quality and packaging infrastructure

- CI across Windows (MSVC, clang-cl), Linux (GCC, Clang) and macOS (Homebrew LLVM) — CLI and GUI
  on Windows/Linux, CLI on macOS — plus a dedicated AddressSanitizer+UndefinedBehaviorSanitizer
  leg, clang-tidy static analysis, a coverage gate, a per-platform gold-reference quality gate,
  and an independent FFmpeg-validation leg.
- libFuzzer harnesses over every untrusted-input entry point (stream scanning, both decoders,
  WAV reading), run on every push and nightly with deeper mutation.
- Signed, attested release packages (Windows `.zip`/`.exe`, Linux `.tar.gz`/`.deb`/`.rpm`, macOS
  `.tar.gz`/`.dmg`) with SHA-512 checksums, keyless Sigstore/OIDC build provenance, and an SPDX
  SBOM — see [docs/releasing.md](docs/releasing.md).

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/project/history.md](docs/project/history.md) for how this was built.
