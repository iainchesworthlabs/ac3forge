# ---------------------------------------------------------------------------
# libFuzzer harnesses over ac3::forge's untrusted-input entry points.
#
# libFuzzer (-fsanitize=fuzzer) is an LLVM built-in, unavailable under GCC or
# MSVC, so this whole directory requires upstream Clang - not clang-cl, whose
# libFuzzer support this project has never exercised. Configure through
# fuzz/run.sh rather than by hand; it sets up a dedicated build/fuzz directory
# with the right compiler and none of AC3FORGE_BUILD_CLI/GUI/TESTS, which this
# needs neither of and which would otherwise drag in vcpkg/Qt/Catch2 for
# nothing this directory uses.
# ---------------------------------------------------------------------------

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "AC3FORGE_BUILD_FUZZERS needs upstream Clang: libFuzzer (-fsanitize=fuzzer) is an "
        "LLVM built-in, not available under GCC or MSVC. Configure with "
        "-DCMAKE_CXX_COMPILER=clang++, or run fuzz/run.sh, which does this for you.")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "AC3FORGE_BUILD_FUZZERS targets upstream Clang on Linux/macOS (the linux-llvm / "
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
# flags automatically through target_link_libraries(... ac3::forge). -fno-
# sanitize-recover=undefined turns a UBSan report into a process abort
# libFuzzer can catch and minimize, rather than a warning it would otherwise
# print and run straight past.
#
# Set on forge_objects, not the forge_static/forge_shared wrapper
# targets: the actual codec sources compile there (see src/forge/CMakeLists.txt),
# and PUBLIC on an OBJECT library's usage requirements still propagates
# through PUBLIC target_link_libraries(forge_static PUBLIC forge_objects)
# to whatever finally links ac3::forge - here, each fuzz harness below.
#
# signing_objects gets the identical treatment, for the identical reason:
# fuzz_signing_verify drives ac3::signing's own frame walk and HMAC
# construction (src/signing/src/emdf_atmos_signer.cpp), which is a separate
# target from ac3::forge (see src/signing/CMakeLists.txt) and would otherwise
# be the one library in this build linked into a fuzzer without coverage
# counters or sanitizer instrumentation of its own - so a report would only
# fire on the ASan runtime's own interceptors, and the mutation engine would
# be blind to every branch inside it.
function(ac3forge_instrument_for_fuzzing target)
    target_compile_options(${target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined,fuzzer-no-link)
endfunction()

ac3forge_instrument_for_fuzzing(forge_objects)
ac3forge_instrument_for_fuzzing(signing_objects)

# The container libraries need the identical treatment: they parse
# self-declared lengths straight off untrusted input, which is exactly the
# class of bug ASan catches and exactly what the harnesses below mutate. An
# uninstrumented library would still crash on a genuine out-of-bounds read,
# but only by luck, and the mutation engine would be walking blind - the
# coverage counters are what steer it into the branches that matter.
#
# Same PUBLIC-on-the-OBJECT-library reasoning as forge_objects above: the
# usage requirements propagate through the static/shared wrappers to
# whatever finally links matroska::matroska - here, each harness.
foreach(container_target IN ITEMS matroska_objects mp4_objects)
    target_compile_options(${container_target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link
        -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer
        -g)
    target_link_options(${container_target} PUBLIC
        -fsanitize=address,undefined,fuzzer-no-link)
endforeach()

add_custom_target(ac3forge_fuzzers)

# One executable per untrusted-input entry point. Each adds -fsanitize=fuzzer
# on top of what it already inherited from ac3::forge above: that flag links
# libFuzzer's runtime and its LLVMFuzzerTestOneInput-driven main, which must
# exist exactly once per executable and never inside the library itself.
#
# ac3::warnings is named explicitly rather than inherited: ac3forge links it
# PRIVATE, as every first-party target does, so it governs that library's own
# translation units and stops there. Without this the harness sources would be
# the one corner of the project compiling without the strict set - and a
# harness is exactly where a sloppy size_t-to-int narrowing would quietly
# reshape the input a fuzzer thinks it is passing in.
#
# Anything after `name` is an extra library that harness needs beyond
# ac3::forge - ac3::signing for fuzz_signing_verify, ac3adm::ac3adm for
# fuzz_adm_parse. Both are separate targets rather than part of ac3::forge
# (see src/signing/CMakeLists.txt and src/ac3adm/CMakeLists.txt for why), and
# neither is needed by the six harnesses that only call into the codec.
function(ac3forge_add_fuzzer name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE ac3::forge ac3::warnings ${ARGN})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(ac3forge_fuzzers ${name})
endfunction()

# Same, for a harness whose subject is a container library rather than the
# codec: everything about the flags is identical, only the linked library
# differs. Kept as its own function rather than a parameter on the one above
# because every EXISTING harness takes ac3::forge and would gain a noise
# argument for it.
function(ac3forge_add_container_fuzzer name library)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE ${library} ac3::warnings)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_dependencies(ac3forge_fuzzers ${name})
endfunction()

# ac3::io::scan: format-sniffing over an unidentified byte stream, before any
# decoder commits to a layout (src/forge/src/io/elementary.cpp).
ac3forge_add_fuzzer(fuzz_scan)

# matroska::demux + matroska::Reader: the EBML walk over a container that came
# from a disc rip, a broadcast capture or a download - every length in it
# self-declared (src/matroska/src/reader.cpp).
ac3forge_add_container_fuzzer(fuzz_matroska_demux matroska::matroska)

# mp4::demux + mp4::Reader: the box walk, and the sample table it resolves
# against the file - an INDEX of self-declared offsets and sizes, which is a
# wider attack surface than Matroska's in-line framing
# (src/mp4/src/reader.cpp).
ac3forge_add_container_fuzzer(fuzz_mp4_demux mp4::mp4)

# split_frames + FrameDecoder::decode_frame, driven the way ac3cli's 'decode'
# drives them (src/forge/src/decoder/decoder.cpp).
ac3forge_add_fuzzer(fuzz_ac3_decode)

# split_access_units + Eac3Decoder::decode_access_unit (which calls
# decode_substream internally), driven the way ac3cli's 'decode' drives them
# for E-AC-3 (src/forge/src/decoder/eac3_decoder.cpp).
ac3forge_add_fuzzer(fuzz_eac3_decode)

# ac3::io::read_wav: a realistic input too, not only an adversarial one - a
# truncated or hand-edited WAV is exactly what a user hands the CLI
# (src/forge/src/io/wav.cpp).
ac3forge_add_fuzzer(fuzz_wav_read)

# ac3::iec61937::BurstReader: IEC 61937 burst de-framing, driven the way
# ac3cli's 'unspdif' drives it (src/forge/src/sinks/iec61937.cpp). The one
# entry point here whose input is by definition off a wire - an S/PDIF or
# HDMI capture - and whose Pd word is an attacker-chosen length.
ac3forge_add_fuzzer(fuzz_iec61937_unwrap)

# --- Differential mode (roadmap G3) -----------------------------------------
# Same decode paths as fuzz_ac3_decode/fuzz_eac3_decode above, but instead of
# only checking for a crash/sanitizer trip, these also decode the same
# mutated bytes with FFmpeg and diff the PCM (fuzz/differential_oracle.hpp
# has the full reasoning for what counts as a reportable divergence). Needs
# `ffmpeg` on PATH at RUN time only - the build itself has no new dependency,
# since differential_oracle.hpp shells out rather than linking anything.
ac3forge_add_fuzzer(fuzz_differential_ac3_decode)
ac3forge_add_fuzzer(fuzz_differential_eac3_decode)

# --- The object and metadata layer (roadmap VX3) -----------------------------
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

# ac3::emdf::parse_container: ETSI TS 102 366 Annex H's container, located by
# a bit-by-bit sync scan and sized by its own 16-bit length field
# (src/forge/src/emdf/emdf.cpp).
ac3forge_add_fuzzer(fuzz_emdf_parse)

# ac3::oba::parse_payload: TS 103 420 §5's object_audio_metadata_payload
# (src/forge/src/oba/oamd.cpp).
ac3forge_add_fuzzer(fuzz_oamd_parse)

# ac3::joc::parse_payload: TS 103 420 §6's joc() payload, the one metadata
# parser here that Huffman-decodes and that sizes a matrix from the stream's
# own numbers (src/forge/src/oba/joc.cpp).
ac3forge_add_fuzzer(fuzz_joc_parse)

# ac3::signing::verify_atmos_stream/verify_atmos_frame: the only signing
# operation that runs on a stream its caller did not produce
# (src/signing/src/emdf_atmos_signer.cpp). Links ac3::signing on top of
# ac3::forge; the key is part of the fuzzed input, and nothing here needs or
# derives a real one - see the harness's own header comment.
ac3forge_add_fuzzer(fuzz_signing_verify ac3::signing)

# ac3adm::parse_bw64: BW64/RF64 chunks plus an arbitrary ADM XML document
# (src/ac3adm/src/adm.cpp). Gated on the same AC3FORGE_BUILD_ADM the library
# itself is - that option is OFF by default and additionally needs vcpkg's
# "adm" feature for libadm's Boost headers, which the rest of this build
# deliberately has no vcpkg dependency at all (see fuzz/run.sh's own
# -DAC3FORGE_BUILD_* list). fuzz/run.sh turns it on via AC3FORGE_FUZZ_ADM=1;
# without that this harness simply is not built, exactly as ac3adm itself is
# not.
if(AC3FORGE_BUILD_ADM)
    ac3forge_add_fuzzer(fuzz_adm_parse ac3adm::ac3adm)
endif()
