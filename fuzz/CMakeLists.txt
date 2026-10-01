# ---------------------------------------------------------------------------
# libFuzzer harnesses over iclforge::ac3's untrusted-input entry points.
#
# libFuzzer (-fsanitize=fuzzer) is an LLVM built-in, unavailable under GCC or
# MSVC, so this whole directory requires upstream Clang - not clang-cl, whose
# libFuzzer support this project has never exercised. Configure through
# fuzz/run.sh rather than by hand; it sets up a dedicated build/fuzz directory
# with the right compiler and none of ICLFORGE_BUILD_CLI/GUI/TESTS, which this
# needs neither of and which would otherwise drag in vcpkg/Qt/Catch2 for
# nothing this directory uses.
# ---------------------------------------------------------------------------

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "ICLFORGE_BUILD_FUZZERS needs upstream Clang: libFuzzer (-fsanitize=fuzzer) is an "
        "LLVM built-in, not available under GCC or MSVC. Configure with "
        "-DCMAKE_CXX_COMPILER=clang++, or run fuzz/run.sh, which does this for you.")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "ICLFORGE_BUILD_FUZZERS targets upstream Clang on Linux/macOS (the linux-llvm / "
        "macos-llvm toolchain), not clang-cl: this project has never verified libFuzzer "
        "under the MSVC-ABI frontend, and the fuzzing task this serves asks for Linux-or-"
        "macOS specifically.")
endif()

# ASan+UBSan+SanitizerCoverage on the LIBRARY, not only the harness main: a
# fuzzer only earns its keep against undefined behaviour (the class of bug
# 8386c8f fixed - a malformed differential exponent chain walking the
# reconstruction outside the range it assumed) if the code being mutated is
# instrumented, not just the driver calling it. fuzzer-no-link adds the
# coverage counters libFuzzer's mutation engine reads without pulling in
# libFuzzer's own main, which a library must not carry.
#
# PUBLIC so every harness executable below inherits the same compile AND link
# flags automatically through target_link_libraries(... iclforge::ac3). -fno-
# sanitize-recover=undefined turns a UBSan report into a process abort
# libFuzzer can catch and minimize, rather than a warning it would otherwise
# print and run straight past.
#
# Set on iclforge_ac3_objects, not the iclforge_ac3_static/iclforge_ac3_shared wrapper
# targets: the actual codec sources compile there (see src/ac3/CMakeLists.txt),
# and PUBLIC on an OBJECT library's usage requirements still propagates
# through PUBLIC target_link_libraries(iclforge_ac3_static PUBLIC iclforge_ac3_objects)
# to whatever finally links iclforge::ac3 - here, each fuzz harness below.
#
# iclforge_signing_objects gets the identical treatment, for the identical reason:
# fuzz_signing_verify drives iclforge::signing's own frame walk and HMAC
# construction (src/signing/src/emdf_atmos_signer.cpp), which is a separate
# target from iclforge::ac3 (see src/signing/CMakeLists.txt) and would otherwise
# be the one library in this build linked into a fuzzer without coverage
# counters or sanitizer instrumentation of its own - so a report would only
# fire on the ASan runtime's own interceptors, and the mutation engine would
# be blind to every branch inside it.
function(iclforge_instrument_for_fuzzing target)
    target_compile_options(${target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined,fuzzer-no-link)
endfunction()

iclforge_instrument_for_fuzzing(iclforge_ac3_objects)
iclforge_instrument_for_fuzzing(iclforge_signing_objects)

# The container libraries need the identical treatment: they parse
# self-declared lengths straight off untrusted input, which is exactly the
# class of bug ASan catches and exactly what the harnesses below mutate. An
# uninstrumented library would still crash on a genuine out-of-bounds read,
# but only by luck, and the mutation engine would be walking blind - the
# coverage counters are what steer it into the branches that matter.
#
# Same PUBLIC-on-the-OBJECT-library reasoning as iclforge_ac3_objects above: the
# usage requirements propagate through the static/shared wrappers to
# whatever finally links iclforge::matroska - here, each harness.
foreach(container_target IN ITEMS iclforge_matroska_objects iclforge_mp4_objects iclforge_mpegts_objects)
    target_compile_options(${container_target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${container_target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link)
endforeach()

# iclforge_ac4_objects and iclforge_iab_objects, for the same reason: fuzz_ac4_parse and
# fuzz_iab_parse below mutate parsers that take their counts and lengths from
# the file. Each is a library of its own rather than part of iclforge::ac3 (see
# src/ac4/ and src/iab/), so nothing iclforge::ac3 carries reaches their
# sources, and until these calls existed both harnesses ran against
# uninstrumented parsers. A harness executable still carries the ASan
# runtime, so a segfault, a timeout or an allocation over -rss_limit_mb
# stopped those runs; an out-of-bounds read or undefined behaviour inside the
# parser did not, and the mutation engine got no coverage from it.
#
# iclforge_adm_objects, which fuzz_adm_parse links, was the last one left out, and
# for a reason the other two did not have: libbw64 is header-only, so
# instrumenting iclforge::adm instruments the libbw64 code it compiles too. UBSan then
# stopped the harness within a few hundred executions inside libbw64's
# UnknownChunk constructor, which takes &data_[0] of an empty vector for a
# zero-length chunk, and does the same in Bw64Reader::read() for a <data> chunk
# of zero length. Both are patched in the dependency itself at populate time
# (src/adm/patch_libbw64.cmake, wired up in src/adm/CMakeLists.txt), which
# is what let this call join the set.
#
# An ignorelist scoped to libbw64 was the alternative, and would have hidden
# more than it suppressed: a <fmt > whose channel count and width overflow
# libbw64's uint16_t block alignment gets its read buffer sized from the wrapped
# value and decoded against the real one, which ASan reports as a heap overread
# against iclforge::adm's own read_pcm. That one is guarded in src/adm/src/adm.cpp,
# and an uninstrumented iclforge::adm reads it as a clean execution.
#
# Each call is guarded on the option that decides whether its library exists,
# and each harness below sits under the same guard, so turning one of those
# options off drops its harness instead of failing the configure.
if(ICLFORGE_BUILD_AC4)
    iclforge_instrument_for_fuzzing(iclforge_ac4_objects)
endif()
if(ICLFORGE_BUILD_IAB)
    iclforge_instrument_for_fuzzing(iclforge_iab_objects)
endif()
if(ICLFORGE_BUILD_ADM)
    iclforge_instrument_for_fuzzing(iclforge_adm_objects)
endif()

add_custom_target(iclforge_fuzzers)

# One executable per untrusted-input entry point. Each adds -fsanitize=fuzzer
# on top of what it already inherited from iclforge::ac3 above: that flag links
# libFuzzer's runtime and its LLVMFuzzerTestOneInput-driven main, which must
# exist exactly once per executable and never inside the library itself.
#
# iclforge::warnings is named explicitly rather than inherited: iclforge links it
# PRIVATE, as every first-party target does, so it governs that library's own
# translation units and stops there. Without this the harness sources would be
# the one corner of the project compiling without the strict set - and a
# harness is exactly where a sloppy size_t-to-int narrowing would quietly
# reshape the input a fuzzer thinks it is passing in.
#
# Anything after `name` is an extra library that harness needs beyond
# iclforge::ac3 - iclforge::signing for fuzz_signing_verify, iclforge::iab for
# fuzz_iab_parse, iclforge::ac4 for fuzz_ac4_parse, iclforge::adm for
# fuzz_adm_parse. Each is a separate target rather than part of iclforge::ac3
# (see its own CMakeLists.txt for why), and none is needed by the harnesses
# that only call into the codec. Naming a library here does not instrument
# it: its OBJECT library needs its own iclforge_instrument_for_fuzzing() call
# above, or the harness links it without coverage counters or sanitizer
# checks.
function(iclforge_add_fuzzer name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE iclforge::ac3 iclforge::warnings ${ARGN})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(iclforge_fuzzers ${name})
endfunction()

# Same, for a harness whose subject is a container library rather than the
# codec: everything about the flags is identical, only the linked library
# differs. Kept as its own function rather than a parameter on the one above
# because every EXISTING harness takes iclforge::ac3 and would gain a noise
# argument for it.
function(iclforge_add_container_fuzzer name library)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE ${library} iclforge::warnings)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(iclforge_fuzzers ${name})
endfunction()

# iclforge::ac3::io::scan: format-sniffing over an unidentified byte stream, before any
# decoder commits to a layout (src/ac3/src/io/elementary.cpp).
iclforge_add_fuzzer(fuzz_scan)

# iclforge::matroska::demux + iclforge::matroska::Reader: the EBML walk over a container that came
# from a disc rip, a broadcast capture or a download - every length in it
# self-declared (src/matroska/src/reader.cpp).
iclforge_add_container_fuzzer(fuzz_matroska_demux iclforge::matroska)

# iclforge::mp4::demux + iclforge::mp4::Reader: the box walk, and the sample table it resolves
# against the file - an INDEX of self-declared offsets and sizes, which is a
# wider attack surface than Matroska's in-line framing
# (src/mp4/src/reader.cpp).
iclforge_add_container_fuzzer(fuzz_mp4_demux iclforge::mp4)

# iclforge::mpegts::demux + iclforge::mpegts::Reader: sync search, PSI section reassembly and PES
# reassembly, all loops driven by self-declared lengths over a format that is
# EXPECTED to arrive damaged - the harness most likely to find a hang rather
# than a crash (src/mpegts/src/reader.cpp).
iclforge_add_container_fuzzer(fuzz_mpegts_demux iclforge::mpegts)

# --- The other two Dolby bitstreams this repo reads --------------------------
#
# iclforge::iab and ac4 are here for the same reason every harness above is: both
# exist only to read files this project did not write, and both size their
# loops from numbers the file chose. They are the two such parsers that had no
# harness at all until now.
#
# iclforge::iab::parse_iabitstream + parse_mxf_iab + parse_iaframe: §7's
# Preamble+IAFrame run, the SMPTE ST 2067-201 KLV wrapper an IAB track file
# clip-wraps it in, and §9.1's single extracted frame (src/iab/). One
# harness over all three because they are three framings of the same bytes and
# a shared corpus serves them better than three split ones. Includes
# BitReader::read_plex, whose §5.2 escape bound is exactly the sort of clause
# worth pressing on.
#
# Its first instrumented run, 1.67 million executions in, found parse_mxf_iab's
# KLV walk looping forever on a Length that wrapped back to offset 0 - an
# input the uninstrumented runs never reached. Fixed, with the input under
# fuzz/regressions/fuzz_iab_parse/.
if(ICLFORGE_BUILD_IAB)
    iclforge_add_fuzzer(fuzz_iab_parse iclforge::iab)
endif()

# iclforge::ac4::scan + iclforge::ac4::parse_raw_frame: sync search, then the TOC - a presentation
# list whose count, per-presentation substream group counts and
# substream_index_table sizes are all read from the bitstream, after which
# parse_raw_frame locates payloads by those declared sizes rather than by
# parsing through audio_data (src/ac4/src/ac4.cpp). parse_raw_frame is called
# on the raw input as well as reached through scan(), so the TOC parser is
# pressed without a valid 0xAC40/0xAC41 syncframe having to be guessed first.
#
# Its first runs found a null dereference and five loops that ran on an
# attacker's count after the data behind it was gone; all are fixed. The six
# million clean executions reported after those fixes ran before iclforge_ac4_objects
# was instrumented (see above), so they could catch a segfault, a timeout or
# an oversized allocation, and nothing the parser did within its own memory.
# Instrumented, the committed regression corpus read past a six-entry count
# table with no mutation at all, and 8,606 executions in,
# presentation_config_ext_info() overflowed an int. Both are fixed, with
# reproducers under fuzz/regressions/fuzz_ac4_parse/.
if(ICLFORGE_BUILD_AC4)
    iclforge_add_fuzzer(fuzz_ac4_parse iclforge::ac4)
endif()

# split_frames + FrameDecoder::decode_frame, driven the way forge's 'decode'
# drives them (src/ac3/src/decoder/decoder.cpp).
iclforge_add_fuzzer(fuzz_ac3_decode)

# split_access_units + Eac3Decoder::decode_access_unit (which calls
# decode_substream internally), driven the way forge's 'decode' drives them
# for E-AC-3 (src/ac3/src/decoder/eac3_decoder.cpp).
iclforge_add_fuzzer(fuzz_eac3_decode)

# iclforge::ac3::io::read_wav: a realistic input too, not only an adversarial one - a
# truncated or hand-edited WAV is exactly what a user hands the CLI
# (src/ac3/src/io/wav.cpp).
iclforge_add_fuzzer(fuzz_wav_read)

# iclforge::iec61937::BurstReader: IEC 61937 burst de-framing, driven the way
# forge's 'unspdif' drives it (src/iec61937/src/iec61937.cpp). The one
# entry point here whose input is by definition off a wire - an S/PDIF or
# HDMI capture - and whose Pd word is an attacker-chosen length.
iclforge_add_fuzzer(fuzz_iec61937_unwrap)

# --- Differential mode (differential decoder fuzzing) -----------------------------------------
# Same decode paths as fuzz_ac3_decode/fuzz_eac3_decode above, but instead of
# only checking for a crash/sanitizer trip, these also decode the same
# mutated bytes with FFmpeg and diff the PCM (fuzz/differential_oracle.hpp
# has the full reasoning for what counts as a reportable divergence). Needs
# `ffmpeg` on PATH at RUN time only - the build itself has no new dependency,
# since differential_oracle.hpp shells out rather than linking anything.
iclforge_add_fuzzer(fuzz_differential_ac3_decode)
iclforge_add_fuzzer(fuzz_differential_eac3_decode)

# --- The object and metadata layer (signing-verify fuzz walk) -----------------------------
# The three parsers below sit behind a skip field in every Atmos frame, which
# is where this codec's differentiating feature - object audio - actually
# lives, and they are the deepest attacker-controlled bytes in the tree: a
# decoder reaches emdf::parse_container only after crc2 has passed, and the
# two payload parsers only after the container has handed them a length and a
# blob. Before this they were fuzzed only indirectly, through
# fuzz_eac3_decode, and only for the vanishing fraction of mutations that
# still had a valid CRC (the reason crc_mutator.hpp now exists). Seeded from
# the real payloads inside the Atmos streams generate-seeds.sh already builds
# - see fuzz/extract-metadata-seeds.py.

# iclforge::emdf::parse_container: ETSI TS 102 366 Annex H's container, located by
# a bit-by-bit sync scan and sized by its own 16-bit length field
# (src/objects/src/emdf.cpp).
iclforge_add_fuzzer(fuzz_emdf_parse)

# iclforge::oba::parse_payload: TS 103 420 §5's object_audio_metadata_payload
# (src/objects/src/oamd.cpp).
iclforge_add_fuzzer(fuzz_oamd_parse)

# iclforge::ac3::oba::joc::parse_payload: TS 103 420 §6's joc() payload, the one metadata
# parser here that Huffman-decodes and that sizes a matrix from the stream's
# own numbers (src/ac3/src/oba/joc.cpp).
iclforge_add_fuzzer(fuzz_joc_parse)

# --- Live position input (live OSC object positions) ---------------------------------------
# iclforge::oba::parse_osc_packet: the OSC 1.0 wire form of a live scene update
# (src/objects/src/scene_osc.cpp). Not behind the bitstream at all - this
# is the project's first NETWORK-facing parser, reached straight from a UDP
# datagram by iclforge::audio::LivePositionSource whenever a live session opts
# into positions=osc:<port>, with no CRC or container ahead of it the way
# every other untrusted parser above has.
iclforge_add_fuzzer(fuzz_osc_parse)

# iclforge::signing::verify_atmos_stream/verify_atmos_frame: the only signing
# operation that runs on a stream its caller did not produce
# (src/signing/src/emdf_atmos_signer.cpp). Links iclforge::signing on top of
# iclforge::ac3; the key is part of the fuzzed input, and nothing here needs or
# derives a real one - see the harness's own header comment.
iclforge_add_fuzzer(fuzz_signing_verify iclforge::signing)

# iclforge::adm::parse_bw64: BW64/RF64 chunks plus an arbitrary ADM XML document
# (src/adm/src/adm.cpp). Gated on the same ICLFORGE_BUILD_ADM the library
# itself is - that option is OFF by default and additionally needs vcpkg's
# "adm" feature for libadm's Boost headers, which the rest of this build
# deliberately has no vcpkg dependency at all (see fuzz/run.sh's own
# -DICLFORGE_BUILD_* list). fuzz/run.sh turns it on via ICLFORGE_FUZZ_ADM=1;
# without that this harness simply is not built, exactly as iclforge::adm itself is
# not.
if(ICLFORGE_BUILD_ADM)
    iclforge_add_fuzzer(fuzz_adm_parse iclforge::adm)
endif()

# iclforge::ac4::Decoder::parse: every substream of an AC-4 frame below the table of
# contents fuzz_ac4_parse presses (src/ac4dec) - section lengths, Huffman
# codewords, A-SPX envelope counts, DRC gain sets and EMDF payloads, each
# counted by the stream - and, from D2, the reconstruction of what they
# decode to. iclforge_ac4dec_objects is a library of its own, so it needs its own
# instrumentation call, as fuzz_ac4_parse's parser does, and so does the
# shared core it links (src/ac4core: the tables and transforms). Its seeds
# are fuzz_ac4_parse's (fuzz/run.sh's seed_source_for).
#
# Its first run from short seeds (one to three frames of the committed DEE
# streams) stopped 2,690 executions in: ext_code's escape, whose leading ones
# Pseudocode 20 does not bound, shifted a 32-bit value by 32. Table 40 limits
# the escape to 21 bits, which the decoder now enforces; the input is under
# fuzz/regressions/fuzz_ac4_decode/.
if(ICLFORGE_BUILD_AC4)
    iclforge_instrument_for_fuzzing(iclforge_ac4core)
    iclforge_instrument_for_fuzzing(iclforge_ac4dec_objects)
    iclforge_add_fuzzer(fuzz_ac4_decode iclforge::ac4dec)
endif()

# iclforge::ac4::Encoder (src/ac4enc): configurations and samples from the input, each
# frame read back by the decoder, whose trace must be the encoder's, decoded
# to finite PCM, and written again byte for byte by a second encoder
# (planning/ac4.md, the encoder's ladder, items 1 and 8). The core and the
# decoder are instrumented by the block above.
if(ICLFORGE_BUILD_AC4)
    iclforge_instrument_for_fuzzing(iclforge_ac4enc_objects)
    iclforge_add_fuzzer(fuzz_ac4_encode iclforge::ac4enc iclforge::ac4dec)
endif()

# --- Sendspin (planning/hearth-reference-player.md, A4) ----------------------
# iclforge::sendspin's parsers are the first code a network peer's bytes reach, on
# hearth's server and on a sink. Gated on ICLFORGE_BUILD_HEARTH like the
# library; fuzz/run.sh turns it on with ICLFORGE_SENDSPIN_CORE_ONLY, which
# builds the library's dependency-free core and keeps this build free of vcpkg.
if(TARGET iclforge_sendspin)
    iclforge_instrument_for_fuzzing(iclforge_sendspin)

    # iclforge::sendspin::json::Document::parse, with a write-and-reparse check
    # (src/sendspin/src/json.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_json iclforge::sendspin)

    # iclforge::sendspin::Reassembler in both fragment forms, and the player@v1 and
    # _iclforge_player@v1 chunk parsers (src/sendspin/src/frames.cpp, chunks.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_frames iclforge::sendspin)

    # The handshake phase's messages - client/init, server/init, server/error,
    # noise/handshake and the two Noise payloads - read and written back
    # (src/sendspin/src/handshake.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_handshake iclforge::sendspin)

    # The core and pairing messages after the handshake, every reader in both dialects, with a
    # check that what is written back reads and writes back to itself
    # (src/sendspin/src/messages.cpp, pairing_messages.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_messages iclforge::sendspin)
endif()
