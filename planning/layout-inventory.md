# The layout study: inventory

!!! note "Facts of `main` at 4ca84d66d, 2026-09-30"
    Every table below is computed from the tracked files of that commit by the scripts the study
    kept (`tools/n1b/` when the execution phase lands them), so a later `main` regenerates it.
    Counts are directives, files or lines as the column says. The proposal that reads these tables is
    [the layout study](layout.md).

## A. The libraries in `src/`

| directory | purpose | files | pub hdr / priv hdr / src | C/C++ lines | aliases | header root | namespaces (most files first) | includes (library, directives) | included by (libraries) |
| --- | --- | ---: | ---: | ---: | --- | --- | --- | --- | --- |
| `src/ac3adm` | BW64/RF64 and ADM reader | 7 | 2/1/2 | 1783 | ac3adm::ac3adm, ac3adm::ac3adm_shared, ac3adm::ac3adm_static | `ac3adm/` | `ac3adm`, `ac3adm::detail`, `adm` | - | admbridge 3 |
| `src/ac3iab` | SMPTE ST 2098-2 IAB reader, MXF | 9 | 3/1/4 | 1856 | ac3iab::ac3iab, ac3iab::ac3iab_shared, ac3iab::ac3iab_static | `ac3iab/` | `ac3iab`, `ac3iab::detail`, `element_id` | - | admbridge 3 |
| `src/ac4` | AC-4 inspector: table of contents, presentations, syntax types | 4 | 2/0/1 | 3651 | ac4::ac4, ac4::ac4_shared, ac4::ac4_static | `ac4/` | `ac4` | - | ac4dec 6, ac4enc 4, capi 1 |
| `src/ac4core` | AC-4 tables, transforms and reconstruction kernels shared by decoder and encoder (private static library) | 39 | 0/21/17 | 12538 | ac4::core | (src/ is the include dir) | `ac4::detail::dsp`, `ac4::detail::tables`, `ac4::detail::aspx` | arithmetic 1 | ac4dec 34, ac4enc 35 |
| `src/ac4dec` | AC-4 decoder | 68 | 1/33/32 | 21159 | ac4::decoder, ac4::decoder_shared, ac4::decoder_static | `ac4dec/` | `ac4::detail`, `ac4`, `object_layout` | ac4 6, ac4core 34 | capi 1 |
| `src/ac4enc` | AC-4 encoder | 51 | 1/23/25 | 17421 | ac4::encoder, ac4::encoder_shared, ac4::encoder_static | `ac4enc/` | `ac4::detail`, `ac4`, `immersive_mode` | ac4 4, ac4core 35 | capi 1 |
| `src/admbridge` | ADM and IAB to Atmos object mapping | 7 | 3/0/3 | 1427 | ac3::admbridge, ac3::admbridge_shared, ac3::admbridge_static | `ac3/` | `ac3::admbridge` | ac3adm 3, ac3iab 3, forge 6 | - |
| `src/arithmetic` | Fixed32 and the cross-platform float functions, header only | 3 | 2/0/0 | 564 | ac3::arithmetic | `ac3/` | `ac3::internal` | - | ac4core 1, forge 11 |
| `src/audio` | device backends (WASAPI, ALSA, PipeWire, CoreAudio, Android), capture, monitor, passthrough sink | 77 | 15/11/50 | 17913 | ac3::audio | `ac3/` | `ac3::audio`, `ac3::alsa`, `ac3::coreaudio` | forge 13 | - |
| `src/capi` | C11 API over the codecs | 18 | 2/2/13 | 5619 | ac3::forge_c, ac3::forge_c_shared, ac3::forge_c_static | `ac3forge_c/` | `ac3forge_c` | ac4 1, ac4dec 1, ac4enc 1, forge 13 | - |
| `src/forge` | AC-3 and E-AC-3 codec, Atmos in E-AC-3, DSP, WAV, loudness, layouts and rendering, IEC 61937 | 180 | 71/39/68 | 61081 | ac3::forge, ac3::forge_minimal, ac3::forge_shared, ac3::forge_static | `ac3/` | `ac3`, `ac3::internal`, `ac3::io` | arithmetic 11 | admbridge 6, audio 13, capi 13, signing 1 |
| `src/iamf` | IAMF OBU and ISOBMFF writer | 5 | 1/2/1 | 881 | iamf::iamf, iamf::iamf_shared, iamf::iamf_static | `iamf/` | `iamf`, `iamf::detail` | - | - |
| `src/matroska` | Matroska writer and reader | 6 | 2/1/2 | 1489 | matroska::matroska, matroska::matroska_shared, matroska::matroska_static | `matroska/` | `matroska`, `detail`, `matroska::detail` | - | - |
| `src/mp4` | MP4, fMP4, HLS and DASH writer; reader | 12 | 4/2/5 | 3719 | mp4::mp4, mp4::mp4_shared, mp4::mp4_static | `mp4/` | `mp4`, `detail`, `mp4::detail` | - | - |
| `src/mpegts` | MPEG-TS writer and reader | 6 | 2/1/2 | 2497 | mpegts::mpegts, mpegts::mpegts_shared, mpegts::mpegts_static | `mpegts/` | `mpegts`, `detail`, `mpegts::detail` | - | - |
| `src/sendspin` | Sendspin protocol, Hearth's player extension, discovery, pairing, transport | 89 | 33/6/45 | 20767 | ac3::sendspin | `ac3/` | `ac3::sendspin`, `ac3::sendspin::codec`, `ac3::sendspin::handshake` | - | - |
| `src/signing` | EMDF Atmos object signing (HMAC) | 9 | 2/2/4 | 950 | ac3::signing, ac3::signing_shared, ac3::signing_static | `ac3/` | `ac3::signing` | forge 1 | - |

Installed and exported (cmake/InstallLibrary.cmake): forge, signing, matroska, mp4, mpegts, ac3iab, ac3adm, admbridge, iamf, the AC-4 four (as one set), capi. Not installed: audio, sendspin, arithmetic.

Library file names today: `ac3forge`, `ac3forge_c`, `ac3signing`, `ac3adm`, `ac3iab`, `admbridge`, `mp4`, `mpegts`, `matroska`, `iamf`, `ac4`, `ac4dec`, `ac4enc`, `ac4core_static`. Debian, Arch and MSYS2 package file lists show libmatroska installing headers under `include/matroska/`, the directory name `matroska/matroska.hpp` here uses, and the library name `libmatroska`.

### A.1 What `src/forge` holds

| directory | content | destination in the recommended layout |
| --- | --- | --- |
| `core` | AC-3 and E-AC-3 tables, bit allocation, exponents, mantissas, coupling, MDCT; bit reader and writer (codec-blind); Location and Layout (codec-blind, inside eac3_tables.hpp) | ac3; bit I/O and the layout vocabulary to base; the FFT to dsp |
| `decoder` | AC-3 and E-AC-3 decoder, output stage, PcmBlock and BlockSink (a sink interface), DownmixTarget | ac3; PcmBlock to render, DownmixTarget to base |
| `encoder` | AC-3 and E-AC-3 encoder, plan, assignment | ac3 |
| `oba` | OAMD payload and object scene (codec-blind); JOC and the Atmos encoder (E-AC-3) | objects (scene, motion, oamd, placement, joc_domain); ac3 (atmos, joc) |
| `io` | elementary streams, probe, dec3, metadata edit, object strip, stream accumulator (AC-3); WAV (codec-neutral in intent, Acmod in its API) | ac3 (WAV stays: its API takes Acmod) |
| `meta` | BSI, DRC, mixing (AC-3); loudness and QC gates (BS.1770, but take SampleRate and Acmod) | ac3 (loudness and QC stay for the same reason) |
| `render` | speaker layout, routing, trim and delay, identify tone, bed and object renderer (header only); serving.hpp adapts them to DecoderConfig | render; serving.hpp to ac3 |
| `verify` | encoder/decoder mirror check for AC-3 and E-AC-3 | ac3 |
| `internal` | SIMD, CPU probe, profiling markers (codec-blind); build profile, scalar selection, AVX2 kernels (AC-3) | base (arch, cpu, profiling); ac3 (profile, scalar, avx2) |
| `iec61937` | IEC 61937 burst packing and unpacking (AC-3, E-AC-3, AC-4 packers) | iec61937 |
| `emdf` | EMDF container (codec-blind: bit reader and writer only) and E-AC-3 frame layout | objects (the container, emdf.hpp and emdf.cpp); ac3 (the frame layout) |
| `quality` | psychoacoustic model and distortion measure for the AC-3 encoder | ac3 |
| `dsp` | biquad, 64-band QMF, resampler | dsp |
| `spatial` | panning math for objects onto a speaker layout | render |
| `analysis` | per-channel level analysis (takes Acmod) | ac3 |

## B. The include graph

Built from every `#include` of 1300 C/C++ files, each resolved as the compiler would (quote-relative, then the header spellings the build puts on the path).

### B.1 Library to library

| from | to | directives | files | headers | into a private header | from a public header |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| ac4core | arithmetic | 1 | 1 | 1 | 0 | 0 |
| ac4dec | ac4 | 6 | 5 | 2 | 0 | 2 |
| ac4dec | ac4core | 34 | 25 | 16 | 34 | 0 |
| ac4enc | ac4 | 4 | 3 | 2 | 0 | 2 |
| ac4enc | ac4core | 35 | 18 | 15 | 35 | 0 |
| admbridge | ac3adm | 3 | 3 | 2 | 0 | 2 |
| admbridge | ac3iab | 3 | 3 | 2 | 0 | 2 |
| admbridge | forge | 6 | 5 | 2 | 0 | 4 |
| audio | forge | 13 | 12 | 6 | 0 | 5 |
| capi | ac4 | 1 | 1 | 1 | 0 | 0 |
| capi | ac4dec | 1 | 1 | 1 | 0 | 0 |
| capi | ac4enc | 1 | 1 | 1 | 0 | 0 |
| capi | forge | 13 | 3 | 12 | 0 | 0 |
| forge | arithmetic | 11 | 11 | 2 | 0 | 0 |
| signing | forge | 1 | 1 | 1 | 0 | 0 |

Cycles between libraries: none. `ac4core`, `ac4dec`, `ac4enc` and `ac4` include nothing from `forge`; `sendspin` and the containers include nothing from any other library.

### B.2 Consumers to libraries

| consumer | ac3adm | ac3iab | ac4 | ac4core | ac4dec | ac4enc | admbridge | arithmetic | audio | capi | forge | iamf | matroska | mp4 | mpegts | sendspin | signing |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| apps/android |  |  |  |  |  |  |  |  | 4 |  | 13 |  |  |  |  |  | 2 |
| apps/baremetal |  |  |  |  |  |  |  |  |  |  | 9 |  |  |  |  |  |  |
| apps/cli | 2 | 2 | 10 |  | 6 | 6 | 3 |  | 19 |  | 167 |  | 3 | 7 | 2 |  | 7 |
| apps/common |  |  | 2 |  | 4 | 1 |  |  |  |  | 31 |  | 2 | 6 | 2 |  |  |
| apps/crucible |  |  |  |  |  |  |  |  | 18 |  | 21 |  |  |  |  |  | 4 |
| apps/gui |  |  | 4 |  | 4 | 2 |  |  | 8 |  | 47 |  | 1 | 3 | 1 |  |  |
| apps/hearth |  |  | 4 |  | 5 | 1 |  |  | 18 |  | 90 |  |  |  |  | 83 | 1 |
| apps/wasm |  |  | 1 |  | 1 | 1 |  |  |  |  | 15 |  |  |  |  |  |  |
| esp-idf |  |  |  |  |  |  |  |  |  |  | 38 |  |  |  |  | 37 |  |
| esphome |  |  |  |  |  |  |  |  |  |  | 2 |  |  |  |  |  |  |
| examples | 2 | 3 | 1 |  | 1 |  | 2 |  |  | 2 | 89 | 1 | 1 | 4 | 1 |  | 2 |
| fuzz | 1 | 2 | 4 |  | 2 | 1 |  |  |  |  | 18 |  | 1 | 1 | 1 | 9 | 2 |
| packaging |  |  |  |  |  |  |  |  |  |  | 1 |  |  |  |  |  |  |
| python |  |  | 1 |  | 1 | 1 |  |  |  |  | 16 |  | 2 | 2 | 2 |  | 2 |
| tests | 5 | 7 | 46 | 35 | 87 | 58 | 7 | 5 | 40 | 1 | 591 | 1 | 7 | 12 | 6 | 177 | 6 |
| tools |  |  | 3 |  | 2 | 1 |  |  | 4 | 1 | 2 |  |  |  |  |  |  |

### B.3 Inside `src/forge`: directory to directory

| from | to | directives | files |
| --- | --- | ---: | ---: |
| (root) | core | 1 | 1 |
| (root) | internal | 1 | 1 |
| analysis | core | 2 | 2 |
| analysis | spatial | 1 | 1 |
| core | internal | 11 | 7 |
| core | meta | 1 | 1 |
| decoder | (root) | 1 | 1 |
| decoder | core | 37 | 11 |
| decoder | emdf | 1 | 1 |
| decoder | internal | 12 | 6 |
| decoder | meta | 14 | 6 |
| decoder | oba | 4 | 2 |
| decoder | verify | 3 | 2 |
| dsp | core | 1 | 1 |
| emdf | core | 8 | 3 |
| encoder | (root) | 3 | 3 |
| encoder | core | 46 | 15 |
| encoder | internal | 5 | 3 |
| encoder | io | 1 | 1 |
| encoder | meta | 18 | 6 |
| encoder | quality | 6 | 5 |
| encoder | spatial | 1 | 1 |
| encoder | verify | 4 | 4 |
| iec61937 | core | 1 | 1 |
| internal | core | 3 | 2 |
| io | core | 22 | 11 |
| io | decoder | 4 | 2 |
| io | emdf | 2 | 2 |
| io | meta | 6 | 6 |
| io | oba | 2 | 2 |
| meta | core | 10 | 8 |
| oba | (root) | 2 | 2 |
| oba | core | 10 | 4 |
| oba | dsp | 4 | 4 |
| oba | emdf | 1 | 1 |
| oba | encoder | 3 | 2 |
| oba | internal | 3 | 2 |
| oba | spatial | 2 | 2 |
| quality | core | 6 | 3 |
| quality | internal | 2 | 2 |
| render | core | 2 | 2 |
| render | decoder | 4 | 3 |
| render | oba | 2 | 1 |
| render | spatial | 2 | 2 |
| spatial | core | 2 | 2 |
| verify | core | 12 | 6 |
| verify | decoder | 4 | 4 |
| verify | encoder | 7 | 4 |

Strongly connected sets of directories: [['core', 'internal', 'meta'], ['decoder', 'encoder', 'io', 'oba', 'verify']].

### B.4 The cuts: includes that stop `forge` splitting

Under the split in [the layout study](layout.md) (base, dsp, render, objects, iec61937 and ac3), the include directives that point from a codec-blind library into the codec:

```text
18 violating include directives in 14 files

## audio -> ac3: 2
  src/audio/include/ac3/audio/speakers.hpp -> ac3/core/eac3_tables.hpp
  src/audio/src/speakers.cpp -> ac3/core/eac3_tables.hpp

## iec61937 -> ac3: 1
  src/iec61937/iec61937.cpp -> ac3/core/eac3_tables.hpp

## objects -> ac3: 6
  include/ac3/oba/motion.hpp -> ac3/oba/atmos.hpp
  include/ac3/oba/scene.hpp -> ac3/oba/atmos.hpp
  include/ac3/oba/scene_osc.hpp -> ac3/oba/atmos.hpp
  src/oba/motion.cpp -> ac3/oba/atmos.hpp
  src/oba/scene.cpp -> ac3/oba/atmos.hpp
  src/oba/scene_osc.cpp -> ac3/oba/atmos.hpp

## render -> ac3: 9
  include/ac3/render/layout.hpp -> ac3/core/eac3_tables.hpp
  include/ac3/render/layout.hpp -> ac3/decoder/output.hpp
  include/ac3/render/render.hpp -> ac3/core/eac3_tables.hpp
  include/ac3/render/render.hpp -> ac3/decoder/decoder.hpp
  include/ac3/render/render.hpp -> ac3/oba/joc.hpp
  include/ac3/render/serving.hpp -> ac3/decoder/decoder.hpp
  include/ac3/render/serving.hpp -> ac3/decoder/output.hpp
  include/ac3/spatial/spatial.hpp -> ac3/core/eac3_tables.hpp
  src/spatial/spatial.cpp -> ac3/core/eac3_tables.hpp
```

After the seven cuts of stage 1 the same check finds none (the prototype's build is the proof).

### B.5 Private headers reached across a boundary

```text
private headers included across libraries from src/ (excluding tests and apps):
  ac4core    src/ac4core/src/acpl/acpl.hpp  <- ac4dec (3), ac4enc (2)
  ac4core    src/ac4core/src/ajcc/ajcc.hpp  <- ac4dec (1), ac4enc (1)
  ac4core    src/ac4core/src/ajoc/ajoc.hpp  <- ac4dec (1), ac4enc (1)
  ac4core    src/ac4core/src/aspx/frequency_tables.hpp  <- ac4dec (2), ac4enc (1)
  ac4core    src/ac4core/src/aspx/hf_generator.hpp  <- ac4dec (2), ac4enc (1)
  ac4core    src/ac4core/src/dsp/complex.hpp  <- ac4dec (1), ac4enc (3)
  ac4core    src/ac4core/src/dsp/kbd.hpp  <- ac4enc (2)
  ac4core    src/ac4core/src/dsp/mdct.hpp  <- ac4enc (2)
  ac4core    src/ac4core/src/dsp/qmf.hpp  <- ac4dec (2), ac4enc (4)
  ac4core    src/ac4core/src/dsp/resampler.hpp  <- ac4dec (1), ac4enc (1)
  ac4core    src/ac4core/src/dsp/synthesis.hpp  <- ac4dec (1)
  ac4core    src/ac4core/src/huffman_codebook.hpp  <- ac4dec (2), ac4enc (1)
  ac4core    src/ac4core/src/internal/scalar/double/ac4/detail/real.hpp  <- ac4dec (3)
  ac4core    src/ac4core/src/tables/huffman_codes.hpp  <- ac4enc (7)
  ac4core    src/ac4core/src/tables/huffman_tables.hpp  <- ac4dec (6), ac4enc (6)
  ac4core    src/ac4core/src/tables/isf_tables.hpp  <- ac4dec (1)
  ac4core    src/ac4core/src/tables/noise_tables.hpp  <- ac4dec (1)
  ac4core    src/ac4core/src/tables/qmf_tables.hpp  <- ac4dec (2), ac4enc (1)
  ac4core    src/ac4core/src/tables/sfb_tables.hpp  <- ac4dec (5), ac4enc (2)
  base       src/forge/src/internal/arch/aarch64/ac3/internal/arch/simd.hpp  <- ac3 (4), dsp (1)
  base       src/forge/src/internal/cpu/cpu_features.hpp  <- ac3 (1)
  base       src/forge/src/internal/profiling/stage_timers/ac3/internal/profiling.hpp  <- ac3 (13)
  dsp        src/forge/src/core/fft_kernel.hpp  <- ac3 (5)
```

Includes that reach a header outside a library's `include/` from another tree, by including tree: tests/hearth 88, tests/ac4dec 81, src/ac4enc 35, tests/crucible 35, src/ac4dec 34, tests/ac4enc 27, apps/cli 21, apps/gui 16, tests/ac4core 16, apps/crucible 12, apps/hearth 9, tests/core 9. `tests/CMakeLists.txt` makes 26 `target_include_directories(ac3tests ...)` calls to allow them.

### B.6 What a second codec needs: the TrueHD branch

The 24 `mlp` files of `feature/truehd-atmos-support` (tip 24bac17d0, merge base with main 5991b9ef9) include these headers from outside `mlp`, with the library each lands in under L2:

| header (old spelling) | directives | library under L2 |
| --- | ---: | --- |
| `ac3/export.hpp` | 11 | its own library's export header |
| `ac3/core/bitreader.hpp` | 6 | base |
| `ac3/core/bitwriter.hpp` | 6 | base |
| `ac3/oba/oamd.hpp` | 1 | objects |
| `ac3/emdf/emdf.hpp` | 1 | objects |

Under L2 the branch's files link `base` and `objects` only if the EMDF container (`emdf.hpp`, `emdf.cpp`) sits in `objects` ([decision 14](layout.md#i-decisions)); the branch's other includes are of its own files.

## C. Names

### C.1 Tokens containing `ac3`, by class

| class | occurrences | files | distinct | most frequent |
| --- | ---: | ---: | ---: | --- |
| ac3 (namespace, path, codec word) | 23536 | 1343 | 2 | `ac3` 23532, `Ac3` 4 |
| brand: ac3forge* (package, module, crate, identifiers) | 9182 | 826 | 569 | `ac3forge` 3280, `AC3FORGEC_EXPORT` 286, `AC3Forge` 240, `_ac3forge_player` 176, `ac3forge_status_t` 126 |
| codec: E-AC-3 (eac3*) | 8822 | 644 | 515 | `eac3` 3440, `DecoderEac3` 552, `Eac3Decoder` 512, `kEac3` 348, `Eac3Field` 162 |
| brand: AC3FORGE_* (options, macros, env, Kconfig) | 4160 | 468 | 536 | `AC3FORGE_EXPORT` 355, `AC3FORGE_OK` 210, `AC3FORGE_SAMPLES_PER_FRAME` 196, `AC3FORGE_ERROR_INVALID_ARGUMENT` 135, `AC3FORGE_BUILD_ADM` 111 |
| program: ac3cli* | 2233 | 392 | 6 | `ac3cli` 2169, `AC3CLI` 53, `ac3cli_decode` 5, `ac3cli_probe` 4, `ac3cli_docs` 1 |
| codec: ac3_* / *_ac3 identifiers | 1297 | 229 | 256 | `ac3_stream` 52, `ac3_stereo` 39, `ac3_layout_for` 39, `supports_ac3_passthrough` 34, `ac3_config` 33 |
| library/target: ac3adm* | 856 | 75 | 14 | `ac3adm` 756, `ac3adm_shared` 27, `ac3adm_objects` 25, `AC3ADMBRIDGE_EXPORT` 14, `ac3adm_static` 13 |
| program: ac3gui* | 757 | 148 | 20 | `ac3gui` 646, `ac3gui_qmltests` 50, `ac3gui_lupdate` 13, `ac3gui_xx` 12, `ac3gui_` 7 |
| contains ac3 (other) | 713 | 191 | 174 | `libac3forge` 70, `dac3` 67, `DAC3FORGE_BUILD_ADM` 48, `DAC3FORGE_BUILD_GUI` 32, `libac3forge0` 26 |
| codec: Ac3* identifiers | 673 | 165 | 47 | `kAc3` 313, `Ac3Transcoder` 80, `kCodecAc3` 34, `kBitstreamAsAc3` 34, `kAc3FromJoc` 21 |
| ac3<word> (other) | 600 | 80 | 60 | `ac3f` 220, `ac3bench` 58, `ac3membench` 51, `ac3perf` 38, `ac3space` 34 |
| library/target: ac3iab* | 584 | 61 | 9 | `ac3iab` 525, `ac3iab_objects` 17, `ac3iab_shared` 15, `ac3iab_static` 14, `AC3IAB_EXPORT` 7 |
| program: ac3hearth* | 448 | 83 | 20 | `ac3hearth` 338, `ac3hearth_engine` 34, `ac3hearth_qmltests` 26, `ac3hearth_controller_tests` 11, `ac3hearth_testsink` 9 |
| codec or brand macro: AC3_ / AC3 | 433 | 74 | 52 | `AC3_ZONE_SCOPED_N` 100, `AC3_ZONE_END` 42, `AC3_ZONE_BEGIN` 40, `AC3` 36, `AC3_VCINSTALLDIR` 12 |
| program: ac3crucible* | 386 | 58 | 21 | `ac3crucible` 271, `ac3crucible_qmltests` 39, `ac3crucible_engine` 29, `ac3crucible_lupdate` 12, `ac3crucible_` 8 |
| program: ac3tests* | 322 | 83 | 4 | `ac3tests` 311, `ac3tests_sink_firmware_board` 5, `ac3tests_websocket` 3, `ac3tests_diagnostics_server` 3 |
| program: AC3<PROGRAM>_* variables | 298 | 52 | 61 | `AC3CLI_EXE` 32, `AC3GUI_LOCALE` 19, `AC3HEARTH_NOTICES_FILE` 13, `AC3CRUCIBLE_NOTICE_FRAGMENTS` 11, `AC3CLI_DOC_DIR` 10 |
| brand: ac3forge_c* (C API) | 264 | 66 | 17 | `ac3forge_c` 185, `ac3forge_command` 29, `ac3forge_centre_mix_level_t` 9, `ac3forge_commands` 8, `ac3forge_channel_level_t` 6 |
| library/target: ac3probe* | 198 | 38 | 6 | `ac3probe` 191, `ac3probe_esp32s3` 2, `ac3probe_esp32p4` 2, `ac3probe_esp32c3` 1, `ac3probe_esp32c6` 1 |
| library/target: ac3sendspin* | 47 | 8 | 4 | `ac3sendspin` 20, `ac3sendspin_httplib` 13, `ac3sendspin_time_filter` 7, `ac3sendspin_crypto` 7 |
| library/target: ac3audio* | 45 | 11 | 1 | `ac3audio` 45 |
| library/target: ac3signing* | 30 | 10 | 6 | `AC3SIGNING_EXPORT` 13, `ac3signing` 10, `ac3signing_static` 2, `AC3SIGNING_BUILDING_SHARED` 2, `AC3SIGNING_STATIC_DEFINE` 2 |
| library/target: ac3shield* | 8 | 3 | 1 | `ac3shield` 8 |
| library/target: ac3fuzz* | 8 | 4 | 1 | `ac3fuzz` 8 |
| library/target: ac3nullsink* | 6 | 3 | 1 | `ac3nullsink` 6 |
| library/target: ac3test* | 4 | 3 | 1 | `ac3test` 4 |

### C.2 Counts per name family

| name family | files | hits | files outside docs and planning | hits outside docs and planning | distinct |
| --- | ---: | ---: | ---: | ---: | ---: |
| C++ qualifier `ac3::` | 933 | 14205 | 933 | 14205 |  |
| C++ `namespace ac3` declarations | 534 | 545 | 534 | 545 |  |
| include root `ac3/` | 673 | 2501 | 670 | 2496 |  |
| include root `ac3forge_c/` | 10 | 11 | 10 | 11 |  |
| include roots `ac3iab/`, `ac3adm/` | 35 | 48 | 35 | 48 |  |
| include roots `ac4/`, `ac4dec/`, `ac4enc/` | 131 | 225 | 131 | 225 |  |
| include roots `mp4/` `mpegts/` `matroska/` `iamf/` | 50 | 100 | 50 | 100 |  |
| qualifier `ac4::` | 293 | 4318 | 293 | 4318 |  |
| qualifiers `mp4::` `mpegts::` `matroska::` `iamf::` | 64 | 1125 | 64 | 1125 |  |
| qualifiers `ac3iab::` `ac3adm::` | 45 | 440 | 45 | 440 |  |
| app namespaces `ac3cli` `ac3gui` `ac3probe` `ac3shield` `ac3fuzz` `ac3nullsink` | 71 | 143 | 71 | 143 |  |
| C API identifiers `ac3forge_*` (C, C++, Rust, Python, JS, Kotlin) | 136 | 3921 | 128 | 3911 | 472 |
| macros `AC3FORGE_*` in C and C++ | 218 | 1864 | 218 | 1864 | 248 |
| export macros `AC3*_EXPORT` | 80 | 681 | 76 | 677 | 6 |
| CMake options and variables `AC3FORGE_*` (CMake files, presets, workflows) | 79 | 1068 | 79 | 1068 | 166 |
| CMake `find_package(ac3forge` | 26 | 48 | 16 | 21 |  |
| CMake targets `ac3::*` (CMake files, workflows) | 60 | 527 | 60 | 527 | 37 |
| CMake targets `forge_objects\|static\|shared` | 22 | 141 | 22 | 141 |  |
| CMake targets `ac3audio` `ac3sendspin*` `ac3_arithmetic` | 19 | 93 | 17 | 91 |  |
| `project(ac3forge` | 10 | 11 | 8 | 8 |  |
| library file names `libac3forge*`, `ac3forge*.dll\|lib\|so\|a\|dylib` | 39 | 120 | 27 | 73 |  |
| pkg-config names `ac3forge*.pc` | 6 | 11 | 3 | 5 |  |
| Python `import ac3forge` / `from ac3forge` | 23 | 25 | 20 | 22 |  |
| Python `_ac3forge` extension module | 5 | 19 | 5 | 19 |  |
| Rust `ac3forge` and `ac3forge-sys` under rust/ | 29 | 128 | 29 | 128 |  |
| npm and wasm `Ac3Forge` factory and package names | 50 | 163 | 23 | 98 |  |
| Kconfig `CONFIG_AC3FORGE_*` | 56 | 283 | 47 | 256 | 69 |
| ESP-IDF component path `esp-idf/ac3forge` | 69 | 166 | 50 | 102 |  |
| repository `iainchesworthlabs/ac3forge` | 130 | 458 | 50 | 121 |  |
| Pages URL `iainchesworthlabs.github.io/ac3forge` | 14 | 23 | 11 | 19 |  |
| wire string `_ac3forge_player` | 65 | 170 | 55 | 134 |  |
| OTA project name `ac3forge_hearth_sink` | 25 | 81 | 23 | 77 |  |
| format and schema ids `ac3forge.<x>/<n>` | 14 | 36 | 8 | 13 |  |
| environment variables read at run time | 23 | 35 | 21 | 33 | 14 |

### C.3 Namespaces the C++ declares

| namespace | files | where (files) |
| --- | ---: | --- |
| ac3 | 33 | src/forge 33 |
| ac3::admbridge | 6 | src/admbridge 6 |
| ac3::alsa | 4 | src/audio 4 |
| ac3::analysis | 2 | src/forge 2 |
| ac3::android_audio | 1 | src/audio 1 |
| ac3::apps | 12 | apps/common 12 |
| ac3::apps::probe_json | 2 | apps/common 2 |
| ac3::audio | 67 | apps/gui 2, src/audio 65 |
| ac3::cli::platform | 6 | apps/cli 6 |
| ac3::coreaudio | 4 | src/audio 4 |
| ac3::coupling | 2 | src/forge 2 |
| ac3::crucible | 48 | apps/crucible 47, tests 1 |
| ac3::crucible::testing | 2 | tests 2 |
| ac3::crucible::ui | 11 | apps/crucible 11 |
| ac3::dsp | 7 | src/forge 7 |
| ac3::eac3 | 6 | src/forge 6 |
| ac3::eac3::chanmap | 1 | src/forge 1 |
| ac3::eac3::seat | 1 | src/forge 1 |
| ac3::emdf | 4 | src/forge 4 |
| ac3::encoder | 2 | src/forge 2 |
| ac3::encoder_detail | 1 | src/forge 1 |
| ac3::golden | 3 | tests 3 |
| ac3::hearth | 62 | apps/hearth 62 |
| ac3::hearth::testserver | 2 | apps/hearth 2 |
| ac3::hearth::testsink | 8 | apps/hearth 8 |
| ac3::hearth::ui | 11 | apps/hearth 11 |
| ac3::hearth::uitest | 2 | apps/hearth 2 |
| ac3::iec61937 | 2 | src/forge 2 |
| ac3::internal | 25 | src/arithmetic 2, src/forge 23 |
| ac3::internal::arch | 3 | src/forge 3 |
| ac3::internal::avx2 | 8 | src/forge 6, tests 2 |
| ac3::internal::cpu | 6 | src/forge 6 |
| ac3::internal::profiling | 2 | apps/baremetal 1, src/forge 1 |
| ac3::io | 16 | src/forge 16 |
| ac3::io::detail | 2 | src/forge 2 |
| ac3::meta | 11 | src/forge 11 |
| ac3::oba | 13 | src/forge 13 |
| ac3::oba::joc | 3 | src/forge 3 |
| ac3::pipewire | 1 | src/audio 1 |
| ac3::plan | 4 | src/forge 4 |
| ac3::python | 7 | python 7 |
| ac3::python::detail | 1 | python 1 |
| ac3::quality | 4 | src/forge 4 |
| ac3::render | 7 | src/forge 7 |
| ac3::sendspin | 28 | apps/hearth 2, src/sendspin 26 |
| ac3::sendspin::ac3forge | 2 | src/sendspin 2 |
| ac3::sendspin::base64 | 2 | src/sendspin 2 |
| ac3::sendspin::base64url | 2 | src/sendspin 2 |
| ac3::sendspin::codec | 6 | src/sendspin 6 |
| ac3::sendspin::cpace | 2 | src/sendspin 2 |
| ac3::sendspin::crypto | 3 | src/sendspin 3 |
| ac3::sendspin::discovery | 2 | src/sendspin 2 |
| ac3::sendspin::discovery::mdns | 2 | src/sendspin 2 |
| ac3::sendspin::discovery::mdns_packets | 2 | src/sendspin 2 |
| ac3::sendspin::discovery::mdns_platform | 3 | src/sendspin 3 |
| ac3::sendspin::field25519 | 1 | src/sendspin 1 |
| ac3::sendspin::firewall | 3 | src/sendspin 3 |
| ac3::sendspin::handshake | 5 | src/sendspin 5 |
| ac3::sendspin::json | 2 | src/sendspin 2 |
| ac3::sendspin::messages | 2 | src/sendspin 2 |
| ac3::sendspin::noise | 3 | src/sendspin 3 |
| ac3::sendspin::pairing | 3 | src/sendspin 3 |
| ac3::sendspin::pairing_flow | 2 | src/sendspin 2 |
| ac3::sendspin::pairing_messages | 2 | src/sendspin 2 |
| ac3::sendspin::test | 1 | tests 1 |
| ac3::sendspin::transport | 2 | src/sendspin 2 |
| ac3::sendspin::transport::websocket | 5 | src/sendspin 5 |
| ac3::signing | 8 | src/signing 8 |
| ac3::spatial | 2 | src/forge 2 |
| ac3::tables | 2 | src/forge 2 |
| ac3::test | 1 | tests 1 |
| ac3::test::avx2 | 2 | tests 2 |
| ac3::test::platform | 3 | tests 3 |
| ac3::verify | 12 | src/forge 12 |
| ac3::windows_audio | 1 | src/audio 1 |
| ac3adm | 3 | src/ac3adm 3 |
| ac3adm::detail | 2 | src/ac3adm 2 |
| ac3cli | 18 | apps/cli 18 |
| ac3cli::commands | 22 | apps/cli 22 |
| ac3forge | 35 | esp-idf 33, esphome 2 |
| ac3forge::fuzzdiff | 1 | fuzz 1 |
| ac3forge::improv | 1 | esp-idf 1 |
| ac3forge::tcp_arrivals | 1 | esp-idf 1 |
| ac3forge_c | 2 | src/capi 2 |
| ac3fuzz | 1 | fuzz 1 |
| ac3gui | 8 | apps/gui 8 |
| ac3iab | 6 | src/ac3iab 6 |
| ac3iab::detail | 2 | src/ac3iab 2 |
| ac3nullsink | 1 | apps/windows 1 |
| ac3probe | 17 | apps/baremetal 16, esp-idf 1 |
| ac3shield | 2 | apps/android 2 |
| ac4 | 9 | src/ac4 3, src/ac4dec 3, src/ac4enc 3 |
| ac4::detail | 113 | src/ac4core 3, src/ac4dec 64, src/ac4enc 46 |
| ac4::detail::acpl | 2 | src/ac4core 2 |
| ac4::detail::ajcc | 2 | src/ac4core 2 |
| ac4::detail::ajoc | 2 | src/ac4core 2 |
| ac4::detail::aspx | 4 | src/ac4core 4 |
| ac4::detail::dsp | 13 | src/ac4core 13 |
| ac4::detail::tables | 12 | src/ac4core 12 |
| iamf | 2 | src/iamf 2 |
| iamf::detail | 2 | src/iamf 2 |
| matroska | 4 | src/matroska 4 |
| matroska::detail | 1 | src/matroska 1 |
| mp4 | 10 | apps/common 1, src/mp4 9 |
| mp4::detail | 1 | src/mp4 1 |
| mp4::manifest_detail | 1 | src/mp4 1 |
| mpegts | 4 | src/mpegts 4 |
| mpegts::detail | 1 | src/mpegts 1 |

### C.4 Files that carry a program name, a library name, or both

| category | files scanned | program names (N1A) | library and family names (N1B) | both | either |
| --- | ---: | ---: | ---: | ---: | ---: |
| C/C++ source and headers | 1300 | 272 | 1249 | 250 | 1271 |
| apps | 346 | 66 | 232 | 48 | 250 |
| docs | 149 | 79 | 128 | 75 | 132 |
| CMake / Kconfig | 113 | 48 | 91 | 43 | 96 |
| bindings | 77 | 2 | 64 | 1 | 65 |
| check script (tools/checks) | 73 | 37 | 30 | 17 | 50 |
| CI script (tools/ci) | 68 | 25 | 29 | 12 | 42 |
| other tools | 50 | 20 | 31 | 13 | 38 |
| CI workflow / action | 43 | 15 | 23 | 13 | 25 |
| esp-idf component | 32 | 3 | 24 | 3 | 24 |
| packaging | 23 | 11 | 23 | 11 | 23 |
| planning / history (leave) | 22 | 17 | 20 | 16 | 21 |
| tests / fuzz / examples | 57 | 4 | 18 | 4 | 18 |
| repo config (root) | 26 | 2 | 8 | 1 | 9 |
| src | 6 | 2 | 3 | 2 | 3 |
| **total** | 2385 | 603 | 1973 | 509 | 2067 |

### C.5 Lines that would exceed 100 columns if `ac3::` became a longer root

lines naming ac3:: (excluding #include): 14205

| new root | chars | lines newly over 100 cols | files |
| --- | ---: | ---: | ---: |
| icl | 3 | 0 | 0 |
| forge | 5 | 192 | 94 |
| iclforge | 8 | 657 | 190 |
| ac3 (unchanged) | 3 | 0 | 0 |

An upper bound: it assumes every `ac3::` qualifier is rewritten, and `#include` lines are skipped. `.clang-format` sets `ColumnLimit: 100`.

## D. Everything keyed on a path

Files that name a directory the recommended layout moves or renames, found by `git grep` of the path followed by a non-word character (a file under the directory itself is excluded: it moves with it). One list per category; each file is followed by the keys it names.

### repo config (root): 5 files

- `.clang-tidy`: `src/forge`
- `.gitattributes`: `esp-idf/ac3forge`
- `ruff.toml`: `tests/adm`
- `sonar-project.properties`: `include/ac3`, `src/ac3adm`, `src/forge`
- `vcpkg.json`: `src/ac3adm`

### CMake / Kconfig: 49 files

- `CMakeLists.txt`: `esp-idf/ac3forge`, `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`
- `CMakePresets.json`: `src/forge`
- `apps/android/app/src/main/cpp/CMakeLists.txt`: `rust/ac3forge`, `rust/ac3forge-sys`, `src/ac4core`
- `apps/baremetal/CMakeLists.txt`: `esp-idf/ac3forge`
- `apps/baremetal/platform/esp32c3/CMakeLists.txt`: `src/forge`
- `apps/baremetal/platform/esp32c6/CMakeLists.txt`: `src/forge`
- `apps/baremetal/platform/esp32p4/CMakeLists.txt`: `esp-idf/ac3forge`, `src/forge`
- `apps/baremetal/platform/esp32s3/CMakeLists.txt`: `esp-idf/ac3forge`, `src/forge`
- `apps/cli/CMakeLists.txt`: `src/ac3adm`
- `apps/crucible/CMakeLists.txt`: `src/forge`
- `apps/gui/CMakeLists.txt`: `src/forge`
- `apps/gui/tests/CMakeLists.txt`: `src/forge`
- `apps/hearth/engine/CMakeLists.txt`: `esp-idf/ac3forge`, `src/forge`
- `cmake/CompilerWarnings.cmake`: `src/forge`
- `cmake/Coverage.cmake`: `src/ac3adm`
- `cmake/Fmt.cmake`: `src/forge`
- `cmake/GenerateVersion.cmake`: `src/forge`
- `cmake/InstallLibrary.cmake`: `include/ac3`, `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`
- `cmake/MinimalDecoder.cmake`: `src/forge`
- `cmake/Packaging.cmake`: `src/forge`
- `cmake/PkgConfig.cmake`: `src/forge`
- `cmake/Tracy.cmake`: `src/forge`
- `cmake/ac3forgeConfig.cmake.in`: `src/ac3adm`
- `esp-idf/ac3forge/CMakeLists.txt`: `src/forge`, `tests/io`
- `esp-idf/ac3forge/examples/hearth_sink/main/Kconfig.projbuild`: `include/ac3`, `src/forge`
- `esp-idf/ac3forge/idf_component.yml`: `src/forge`
- `examples/CMakeLists.txt`: `src/ac3adm`, `src/ac3iab`
- `fuzz/CMakeLists.txt`: `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`
- `python/CMakeLists.txt`: `include/ac3`, `tests/core`
- `src/ac3adm/CMakeLists.txt`: `src/forge`
- `src/ac3adm/patch_libbw64.cmake`: `tests/adm`
- `src/ac3iab/CMakeLists.txt`: `src/ac3adm`, `src/forge`
- `src/ac4/CMakeLists.txt`: `src/forge`
- `src/ac4core/CMakeLists.txt`: `src/forge`
- `src/ac4dec/CMakeLists.txt`: `src/forge`
- `src/ac4enc/CMakeLists.txt`: `src/forge`
- `src/admbridge/CMakeLists.txt`: `src/ac3adm`, `src/forge`
- `src/arithmetic/CMakeLists.txt`: `src/ac4core`, `src/forge`
- `src/audio/CMakeLists.txt`: `include/ac3`, `src/forge`
- `src/capi/CMakeLists.txt`: `src/ac3adm`, `src/forge`
- `src/forge/CMakeLists.txt`: `include/ac3`, `tests/core`
- `src/iamf/CMakeLists.txt`: `src/ac3adm`, `src/ac3iab`, `src/forge`
- `src/matroska/CMakeLists.txt`: `src/forge`
- `src/mp4/CMakeLists.txt`: `src/forge`
- `src/mpegts/CMakeLists.txt`: `src/forge`
- `src/sendspin/CMakeLists.txt`: `esp-idf/ac3forge`
- `src/signing/CMakeLists.txt`: `src/forge`
- `tests/CMakeLists.txt`: `esp-idf/ac3forge`, `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`, `tests/backend`, `tests/oba`
- `tests/performance/CMakeLists.txt`: `src/ac4core`

### CI workflow / action: 11 files

- `.github/actions/build-leg/action.yml`: `src/forge`, `tests/core`
- `.github/msvc-analysis.ruleset`: `src/forge`
- `.github/workflows/_build.yml`: `esp-idf/ac3forge`, `rust/ac3forge`, `rust/ac3forge-sys`, `tests/io`
- `.github/workflows/_ci-core.yml`: `src/ac3adm`, `src/forge`
- `.github/workflows/_ci-linux.yml`: `src/forge`
- `.github/workflows/ci.yml`: `src/forge`, `tests/backend`
- `.github/workflows/docs.yml`: `src/forge`
- `.github/workflows/esp-component.yml`: `esp-idf/ac3forge`, `src/forge`
- `.github/workflows/interop.yml`: `include/ac3`, `src/forge`
- `.github/workflows/sonarcloud.yml`: `src/ac4core`
- `.github/workflows/wheels.yml`: `src/forge`

### CI script (tools/ci): 5 files

- `tools/ci/fuzz_encoder_space.py`: `include/ac3`, `src/forge`
- `tools/ci/quality_race.py`: `include/ac3`, `src/forge`, `tests/core`, `tests/decoder`, `tests/oba`
- `tools/ci/run_codec_matrix.sh`: `src/forge`, `tests/decoder`, `tests/oba`
- `tools/ci/test_classify_changes.py`: `esp-idf/ac3forge`, `rust/ac3forge`
- `tools/ci/test_plan_gate.py`: `esp-idf/ac3forge`, `src/forge`

### check script (tools/checks): 22 files

- `tools/checks/ac4_syntax_trace.cpp`: `src/ac4core`
- `tools/checks/check_ac3_allocation.py`: `src/forge`
- `tools/checks/check_cross_platform_hash.py`: `tests/core`
- `tools/checks/check_decode_scalar_snr.py`: `src/forge`
- `tools/checks/check_doc_paths.py`: `include/ac3`, `src/forge`
- `tools/checks/check_drc.py`: `tests/meta`
- `tools/checks/check_platform_macros.ps1`: `esp-idf/ac3forge`, `tests/core`, `tests/render`
- `tools/checks/check_sendspin_levels.py`: `esp-idf/ac3forge`
- `tools/checks/check_stream_set.py`: `esp-idf/ac3forge`
- `tools/checks/compare_wav.py`: `src/forge`
- `tools/checks/coverage_crucible.ps1`: `src/forge`
- `tools/checks/coverage_report.sh`: `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`
- `tools/checks/crucible_platform_probe.cpp`: `src/forge`
- `tools/checks/improv_qemu.py`: `esp-idf/ac3forge`
- `tools/checks/passthrough_probe.cpp`: `src/forge`
- `tools/checks/run_baremetal_probe.sh`: `src/forge`
- `tools/checks/run_esp32s3_probe.sh`: `esp-idf/ac3forge`
- `tools/checks/score_ac4_decode.py`: `src/ac4core`
- `tools/checks/test_ac4_build_configurations.py`: `src/ac4core`
- `tools/checks/test_check_esp_console.py`: `esp-idf/ac3forge`
- `tools/checks/test_check_esp_efuse_free.py`: `esp-idf/ac3forge`
- `tools/checks/verify_gold_reference.sh`: `src/forge`, `tests/decoder`

### other tools: 15 files

- `tools/generators/README.md`: `src/forge`
- `tools/generators/gen_ac4_tables.py`: `src/ac4core`
- `tools/generators/gen_aht_tables.py`: `include/ac3`, `src/forge`
- `tools/generators/gen_baremetal_fixture.py`: `src/forge`
- `tools/generators/gen_bitalloc_tables.py`: `include/ac3`, `src/forge`
- `tools/generators/gen_device_streams.py`: `esp-idf/ac3forge`, `include/ac3`, `src/forge`
- `tools/generators/gen_external_baseline.py`: `src/forge`
- `tools/generators/gen_mdct_goldens.py`: `tests/core`
- `tools/generators/gen_object_fixture.py`: `tests/oba`
- `tools/generators/gen_programme_fixtures.py`: `src/forge`
- `tools/generators/gen_qmf_prototype.py`: `src/forge`
- `tools/hearth/package_firmware.py`: `esp-idf/ac3forge`
- `tools/hearth/sink_build_fixture.py`: `esp-idf/ac3forge`
- `tools/hearth/test_ota.py`: `esp-idf/ac3forge`
- `tools/packaging/pack_esp_component.py`: `esp-idf/ac3forge`, `src/ac4core`, `src/forge`

### packaging: 1 files

- `esphome/components/ac3forge/__init__.py`: `esp-idf/ac3forge`

### esp-idf component: 18 files

- `esp-idf/ac3forge/README.md`: `include/ac3`, `src/forge`, `tests/io`, `tests/render`
- `esp-idf/ac3forge/examples/hearth_sink/README.md`: `include/ac3`, `src/forge`, `tests/io`, `tests/render`
- `esp-idf/ac3forge/examples/hearth_sink/main/hearth_sink.cpp`: `include/ac3`, `src/forge`
- `esp-idf/ac3forge/examples/i2s_player/README.md`: `src/forge`
- `esp-idf/ac3forge/include/ac3forge/block_ring.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/dac_queue_model.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/firmware_image.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/firmware_status.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/firmware_trial.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/hardware_info.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/improv.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/interleave.hpp`: `src/forge`, `tests/io`
- `esp-idf/ac3forge/include/ac3forge/log_ring.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/pairing_records.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/playout.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/sink_plan.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/tcp_arrivals.hpp`: `tests/io`
- `esp-idf/ac3forge/include/ac3forge/unit_hold.hpp`: `tests/io`

### bindings: 6 files

- `python/pyproject.toml`: `src/ac4core`
- `python/src/ac3forge_ext/bindings.cpp`: `include/ac3`, `src/forge`
- `python/src/ac3forge_ext/optional_modules.hpp`: `tests/core`
- `python/tests/test_latency.py`: `tests/decoder`
- `rust/ac3forge-sys/build.rs`: `rust/ac3forge`, `src/forge`
- `rust/ac3forge/tests/atmos_meter_stream.rs`: `src/forge`

### apps: 24 files

- `apps/android/app/src/androidTest/java/com/ac3forge/shield/NativeBridgeInstrumentedTest.kt`: `tests/backend`
- `apps/android/app/src/main/cpp/live_cursor.cpp`: `include/ac3`, `src/forge`, `tests/oba`
- `apps/android/app/src/main/java/com/ac3forge/shield/NativeBridge.kt`: `tests/oba`
- `apps/baremetal/encode_probe.cpp`: `tests/encoder`
- `apps/baremetal/platform/baremetal/tls.cpp`: `src/forge`
- `apps/baremetal/probe.cpp`: `src/forge`
- `apps/baremetal/stage_timers.cpp`: `src/forge`
- `apps/baremetal/stage_timers.hpp`: `src/forge`
- `apps/cli/commands/atmos.cpp`: `tests/oba`
- `apps/cli/commands/audio_io.cpp`: `src/ac3adm`
- `apps/cli/commands/live_audio.cpp`: `esp-idf/ac3forge`
- `apps/common/container_input.cpp`: `src/forge`
- `apps/crucible/engine/platform/macos/session_monitor.cpp`: `tests/backend`
- `apps/gui/encoder_controller.cpp`: `tests/oba`
- `apps/hearth/engine/sink_firmware.hpp`: `esp-idf/ac3forge`
- `apps/hearth/ui/qml/Speakers.qml`: `src/forge`
- `apps/hearth/ui/tests/qml/tst_media_page.qml`: `esp-idf/ac3forge`
- `apps/wasm/demo.js`: `include/ac3`, `src/forge`
- `apps/wasm/tests/device-ui/board/layouts.spec.js`: `esp-idf/ac3forge`
- `apps/wasm/tests/device-ui/board/smoke.spec.js`: `esp-idf/ac3forge`
- `apps/wasm/tests/device-ui/contract.spec.js`: `esp-idf/ac3forge`
- `apps/wasm/tests/device-ui/payloads/README.md`: `esp-idf/ac3forge`
- `apps/wasm/tests/device-ui/stub.js`: `esp-idf/ac3forge`, `include/ac3`, `src/forge`
- `apps/wasm/tests/package.json`: `esp-idf/ac3forge`

### tests / fuzz / examples: 73 files

- `examples/atmos_objects.cpp`: `tests/oba`
- `examples/decode_robustness.cpp`: `tests/decoder`
- `examples/encode_adm.cpp`: `src/ac3adm`
- `examples/encode_iab.cpp`: `src/ac3iab`, `tests/ac3iab`
- `examples/read_adm.cpp`: `src/ac3adm`
- `examples/read_iab.cpp`: `src/ac3iab`
- `examples/wav_roundtrip.cpp`: `src/ac3adm`
- `fuzz/README.md`: `src/ac3adm`, `src/forge`, `tests/ac3iab`, `tests/adm`, `tests/core`
- `fuzz/crc_mutator.hpp`: `src/forge`
- `fuzz/fuzz_adm_parse.cpp`: `src/ac3adm`
- `fuzz/fuzz_emdf_parse.cpp`: `src/forge`
- `fuzz/fuzz_iab_parse.cpp`: `src/ac3iab`
- `fuzz/fuzz_iec61937_unwrap.cpp`: `src/forge`
- `fuzz/fuzz_joc_parse.cpp`: `src/forge`
- `fuzz/fuzz_oamd_parse.cpp`: `src/forge`
- `fuzz/fuzz_osc_parse.cpp`: `src/forge`
- `fuzz/fuzz_scan.cpp`: `src/forge`
- `fuzz/fuzz_wav_read.cpp`: `include/ac3`, `src/forge`
- `fuzz/metadata-seeds.py`: `src/forge`, `tests/adm`
- `tests/ac3iab/test_ac3iab.cpp`: `src/ac3iab`
- `tests/ac3iab/test_ac3iab_truncation.cpp`: `src/ac3iab`
- `tests/ac3iab/test_mxf_reader.cpp`: `src/ac3iab`
- `tests/ac4core/test_ac4core_acpl.cpp`: `src/ac4core`
- `tests/ac4core/test_ac4core_ajoc.cpp`: `src/ac4core`
- `tests/ac4core/test_ac4core_aspx.cpp`: `src/ac4core`
- `tests/ac4core/test_ac4core_dsp.cpp`: `src/ac4core`
- `tests/ac4core/test_ac4core_resampler.cpp`: `src/ac4core`
- `tests/adm/test_adm.cpp`: `src/ac3adm`
- `tests/admbridge/test_adm_bridge.cpp`: `tests/adm`, `tests/oba`
- `tests/admbridge/test_iab_bridge.cpp`: `src/ac3iab`, `tests/ac3iab`
- `tests/audio/test_pcm_output.cpp`: `tests/render`
- `tests/audio/test_playback_counter.cpp`: `include/ac3`
- `tests/capi/test_capi.cpp`: `tests/decoder`, `tests/encoder`, `tests/oba`
- `tests/cli/test_cli.cpp`: `tests/decoder`, `tests/meta`
- `tests/cli/test_cli_atmos_adm.cpp`: `tests/oba`
- `tests/cli/test_cli_atmos_cbi.cpp`: `tests/oba`
- `tests/cli/test_cli_containers.cpp`: `tests/io`
- `tests/cli/test_cli_decode_adm.cpp`: `tests/decoder`, `tests/render`
- `tests/cli/test_cli_live.cpp`: `tests/decoder`
- `tests/cli/test_cli_probe.cpp`: `tests/io`, `tests/meta`
- `tests/cli/test_cli_stream_tools.cpp`: `tests/decoder`, `tests/meta`
- `tests/core/avx2/absent/avx2_tier.cpp`: `src/forge`
- `tests/core/avx2/absent/avx2_tier.hpp`: `src/forge`
- `tests/core/avx2/present/avx2_tier.hpp`: `src/forge`
- `tests/core/test_bitalloc.cpp`: `src/forge`
- `tests/core/test_fixed32.cpp`: `src/forge`
- `tests/core/test_fixed32_ecpl.cpp`: `src/forge`
- `tests/core/test_mdct_fixed.cpp`: `src/forge`
- `tests/core/test_simd_kernels.cpp`: `src/forge`
- `tests/crucible/platform/linux/test_x11_foreground.cpp`: `tests/backend`
- `tests/decoder/test_block_norm.cpp`: `src/forge`
- `tests/decoder/test_decoder.cpp`: `src/forge`
- `tests/decoder/test_eac3_decoder.cpp`: `src/forge`, `tests/io`, `tests/meta`
- `tests/encoder/test_encoder.cpp`: `src/forge`
- `tests/encoder/test_exp_strategy.cpp`: `src/forge`
- `tests/golden/bitstream-hashes.json`: `src/forge`
- `tests/hearth/test_decoder_settings.cpp`: `tests/decoder`, `tests/meta`
- `tests/hearth/test_hearth_controller.cpp`: `include/ac3`, `src/forge`
- `tests/hearth/test_player.cpp`: `tests/render`
- `tests/hearth/test_stream_decoder.cpp`: `tests/decoder`
- `tests/io/test_elementary.cpp`: `tests/containers`
- `tests/io/test_object_strip.cpp`: `src/forge`
- `tests/io/test_playout.cpp`: `esp-idf/ac3forge`
- `tests/io/test_probe.cpp`: `tests/meta`
- `tests/io/test_stream_accumulator.cpp`: `tests/decoder`
- `tests/io/test_wav.cpp`: `src/forge`
- `tests/meta/test_mixing.cpp`: `src/forge`
- `tests/oba/test_atmos.cpp`: `tests/decoder`
- `tests/oba/test_joc_7channel.cpp`: `tests/decoder`
- `tests/oba/test_oba.cpp`: `tests/decoder`
- `tests/performance/kernel_bench.cpp`: `src/ac4core`
- `tests/performance/test_performance.cpp`: `src/forge`
- `tests/render/test_object_lfe_timing.cpp`: `tests/decoder`

### docs: 40 files

- `CONTRIBUTING.md`: `include/ac3`, `src/ac4core`, `src/forge`
- `README.md`: `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`
- `docs/assets/wasm-decode-demo/demo.js`: `include/ac3`, `src/forge`
- `docs/building.md`: `esp-idf/ac3forge`, `include/ac3`, `src/ac3adm`, `src/ac3iab`, `src/ac4core`, `src/forge`, `tests/backend`, `tests/core`
- `docs/concepts/object-signing.md`: `src/forge`
- `docs/forge/cli/metadata-options.md`: `include/ac3`, `src/forge`
- `docs/forge/gui/live-session.md`: `include/ac3`
- `docs/hearth/design/player-appliance.md`: `esp-idf/ac3forge`
- `docs/hearth/index.md`: `esp-idf/ac3forge`
- `docs/hearth/sink-esp32-c6.md`: `esp-idf/ac3forge`
- `docs/hearth/sink-esp32-s3.md`: `esp-idf/ac3forge`
- `docs/hearth/sink-firmware.md`: `esp-idf/ac3forge`
- `docs/history.md`: `src/forge`, `tests/decoder`, `tests/encoder`
- `docs/library/adm-bridge.md`: `src/ac3iab`, `src/forge`, `tests/oba`
- `docs/library/adm.md`: `src/ac3adm`
- `docs/library/api-stability.md`: `include/ac3`, `src/forge`
- `docs/library/decoding.md`: `tests/decoder`, `tests/render`
- `docs/library/encoding-ac3.md`: `tests/decoder`
- `docs/library/file-io.md`: `tests/io`
- `docs/library/iab.md`: `src/ac3iab`
- `docs/library/index.md`: `include/ac3`, `src/forge`
- `docs/library/muxing-and-sinks.md`: `tests/containers`
- `docs/library/rust-api.md`: `rust/ac3forge`
- `docs/library/spatial-and-atmos.md`: `tests/oba`
- `docs/object-quality-trend.md`: `include/ac3`, `src/forge`, `tests/oba`
- `docs/performance-trend.md`: `include/ac3`, `src/forge`
- `docs/platforms/android.md`: `src/forge`, `tests/backend`, `tests/oba`
- `docs/platforms/bare-metal/cortex-m3.md`: `src/forge`
- `docs/platforms/bare-metal/esp32-c3.md`: `esp-idf/ac3forge`
- `docs/platforms/bare-metal/esp32-c6.md`: `esp-idf/ac3forge`, `tests/core`, `tests/decoder`
- `docs/platforms/bare-metal/esp32-p4.md`: `esp-idf/ac3forge`
- `docs/platforms/bare-metal/esp32-s3.md`: `esp-idf/ac3forge`, `include/ac3`, `src/forge`, `tests/core`, `tests/encoder`, `tests/io`, `tests/oba`
- `docs/platforms/index.md`: `esp-idf/ac3forge`
- `docs/platforms/linux.md`: `tests/backend`
- `docs/platforms/macos.md`: `tests/backend`
- `docs/platforms/wasm.md`: `src/forge`
- `docs/platforms/windows-demo.md`: `include/ac3`, `src/forge`, `tests/backend`
- `docs/platforms/windows.md`: `include/ac3`
- `docs/threat-model.md`: `src/ac3adm`, `src/forge`
- `docs/verification.md`: `src/ac4core`, `src/forge`, `tests/backend`, `tests/containers`, `tests/decoder`, `tests/meta`, `tests/oba`

### src: 53 files

- `src/ac3adm/src/adm.cpp`: `tests/adm`
- `src/ac3iab/include/ac3iab/model.hpp`: `src/ac3adm`
- `src/ac4core/src/dsp/complex.hpp`: `src/forge`
- `src/ac4core/src/internal/scalar/double/ac4/detail/real.hpp`: `src/forge`
- `src/ac4dec/ERRATA.md`: `src/ac4core`
- `src/ac4dec/src/huffman.hpp`: `src/ac4core`
- `src/ac4dec/src/pcm/aspx.hpp`: `src/ac4core`
- `src/ac4dec/src/pcm/substream_pcm.hpp`: `src/ac4core`
- `src/ac4dec/src/syntax/ajoc.hpp`: `src/ac4core`
- `src/ac4enc/ERRATA.md`: `src/ac4core`
- `src/ac4enc/src/ajoc/ajoc_encoder.hpp`: `src/ac4core`
- `src/admbridge/include/ac3/admbridge/bridge.hpp`: `src/ac3adm`, `src/forge`
- `src/admbridge/include/ac3/admbridge/coordinates.hpp`: `tests/oba`
- `src/admbridge/src/bridge.cpp`: `tests/oba`
- `src/arithmetic/include/ac3/internal/fixed32.hpp`: `src/ac4core`, `src/forge`
- `src/arithmetic/include/ac3/internal/scalar_math.hpp`: `src/ac4core`, `src/forge`, `tests/encoder`
- `src/audio/include/ac3/audio/live_positions.hpp`: `src/forge`
- `src/audio/src/backend/alsa/capture.cpp`: `src/forge`
- `src/audio/src/backend/alsa/device_names.hpp`: `tests/backend`
- `src/audio/src/backend/alsa/eld_proc.hpp`: `tests/backend`
- `src/audio/src/backend/macos/coreaudio_names.hpp`: `tests/backend`
- `src/audio/src/backend/windows/capture.cpp`: `src/forge`
- `src/audio/src/backend/windows/monitor.cpp`: `src/forge`
- `src/audio/src/backend/windows/passthrough.cpp`: `src/forge`
- `src/audio/src/net/udp_socket.hpp`: `include/ac3`
- `src/capi/include/ac3forge_c/ac3forge.h`: `tests/decoder`
- `src/forge/include/ac3/core/mdct.hpp`: `tests/core`
- `src/forge/include/ac3/decoder/decoder.hpp`: `tests/oba`
- `src/forge/include/ac3/dsp/qmf.hpp`: `tests/dsp`
- `src/forge/include/ac3/dsp/resampler.hpp`: `include/ac3`
- `src/forge/include/ac3/encoder/eac3_frame.hpp`: `tests/core`, `tests/meta`
- `src/forge/include/ac3/encoder/encoder.hpp`: `tests/core`
- `src/forge/include/ac3/latency.hpp`: `tests/decoder`
- `src/forge/include/ac3/oba/joc.hpp`: `tests/oba`
- `src/forge/include/ac3/oba/oamd.hpp`: `tests/oba`
- `src/forge/include/ac3/render/identify.hpp`: `tests/render`
- `src/forge/include/ac3/render/layout.hpp`: `tests/render`
- `src/forge/include/ac3/render/render.hpp`: `tests/render`
- `src/forge/include/ac3/render/routing.hpp`: `tests/render`
- `src/forge/include/ac3/render/trim_delay.hpp`: `tests/render`
- `src/forge/src/core/exponents.cpp`: `tests/core`
- `src/forge/src/core/mdct.cpp`: `tests/core`
- `src/forge/src/core/mdct_fixed.hpp`: `tests/core`
- `src/forge/src/decoder/eac3_decoder.cpp`: `tests/oba`
- `src/forge/src/encoder/exp_strategy.hpp`: `tests/encoder`
- `src/forge/src/internal/arch/generic/ac3/internal/arch/simd.hpp`: `tests/core`
- `src/forge/src/internal/arch/x86_64/ac3/internal/arch/simd.hpp`: `tests/core`
- `src/forge/src/internal/cpu/cpu_features.hpp`: `tests/core`
- `src/forge/src/internal/scalar/float32/ac3/internal/decode_scalar.hpp`: `tests/core`
- `src/forge/src/oba/joc.cpp`: `tests/oba`
- `src/mp4/src/isobmff_detail.hpp`: `src/forge`
- `src/sendspin/include/ac3/sendspin/player_session.hpp`: `esp-idf/ac3forge`
- `src/signing/include/ac3/signing/emdf_atmos_signer.hpp`: `src/forge`

### planning / history (leave): 18 files

- `CHANGELOG.md`: `esp-idf/ac3forge`, `include/ac3`, `src/ac4core`, `src/forge`, `tests/adm`, `tests/decoder`, `tests/render`
- `ROADMAP.md`: `src/ac4core`
- `planning/ac4.md`: `esp-idf/ac3forge`, `include/ac3`, `src/ac4core`, `src/forge`
- `planning/arithmetic-tiers.md`: `include/ac3`, `src/forge`, `tests/core`
- `planning/eac3-programme-mixing-metadata.md`: `include/ac3`, `src/forge`, `tests/decoder`, `tests/encoder`, `tests/meta`
- `planning/esp32-714-realtime.md`: `include/ac3`, `src/forge`
- `planning/esp32-device-ui.md`: `esp-idf/ac3forge`, `include/ac3`, `src/forge`
- `planning/esp32-ota.md`: `esp-idf/ac3forge`, `tests/io`
- `planning/esp32-player.md`: `esp-idf/ac3forge`, `include/ac3`, `src/forge`, `tests/io`, `tests/render`
- `planning/esp32-sink-tiers.md`: `esp-idf/ac3forge`
- `planning/esp32-stream-set.md`: `esp-idf/ac3forge`, `tests/render`
- `planning/hearth-reference-player.md`: `esp-idf/ac3forge`, `include/ac3`, `src/forge`
- `planning/hearth-sendspin-extension.md`: `include/ac3`, `src/forge`
- `planning/host-plugin.md`: `include/ac3`, `src/ac3adm`, `src/forge`
- `planning/player-appliance.md`: `include/ac3`, `src/forge`
- `planning/qc-report.md`: `include/ac3`, `src/forge`, `tests/meta`
- `planning/recasting.md`: `rust/ac3forge`, `rust/ac3forge-sys`, `src/ac3adm`, `src/ac3iab`, `src/forge`
- `planning/roadmap-inventory.md`: `include/ac3`, `src/ac3adm`, `src/ac3iab`, `src/forge`, `tests/encoder`

Total: 340 files, before the files that name a program or a library only by name (section C.4).

Not present in this repository: a CODEOWNERS file and a labeler configuration. `dependabot.yml` names `/apps/wasm/tests`, `/apps/android`, `/rust` and the tool directories, none of which moves. `.clang-tidy`'s `HeaderFilterRegex` names `src/(forge|matroska)`; `sonar-project.properties` names `src`, `apps`, `examples`, `js`, `python`, `tools` and `tests` as roots and lists forty-five `src/` paths.

## E. The three layouts, dry run

|  | L1 regroup | L2 peers | L3 libs |
| --- | ---: | ---: | ---: |
| files moved (`git mv`) | 488 | 592 | 1010 |
|   under `src/` or `libs/` | 287 | 287 | 590 |
|   under `tests/` | 0 | 104 | 219 |
|   package directories (esp-idf, rust, python, packaging, cmake) | 201 | 201 | 201 |
| other files edited in place | 1545 | 1456 | 1060 |
| moved files that also change content | 431 | 535 | 944 |
| C/C++ files with rewritten includes or names | 1249 | 1250 | 1252 |
| files edited only because they name a moved path | 3 | 18 | 31 |
| deepest path under `src/` (`libs/`) before | 11 | 11 | 11 |
| deepest path after, variant trees flattened | 8 | 8 | 8 |

Text files scanned: 2430; files carrying a library or family name: 1973.

