# ---------------------------------------------------------------------------
# The Shield app (apps/android) is a full Gradle/NDK project outside the
# iclforge CMake configure entirely, reached from the other direction (its own
# CMakeLists.txt add_subdirectory()s the repo root). This platform doesn't
# need that indirection - there's no non-CMake build system in the way, so
# it's a normal add_subdirectory() from root CMakeLists.txt, gated on
# EMSCRIPTEN there rather than needing a whole separate entry point.
#
# WASM browser demo: a browser demo that decodes a real E-AC-3/Atmos-in-DD+
# elementary stream via iclforge::ac3 compiled to WASM, plays the decoded bed
# audio through the Web Audio API, and visualizes real per-channel energy in
# time with playback. See docs/platforms/wasm.md.
#
# Produces a self-contained, servable directory at
# ${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/: the compiled .js/.wasm, the
# static page and its JS, and the bundled demo bitstream. `python3 -m
# http.server` from that directory (or any static file server - ES modules
# and fetch() need http(s), not file://) is the entire "run the demo" story.
# ---------------------------------------------------------------------------

add_executable(iclforge_wasm_decode decoder_bindings.cpp)
target_link_libraries(iclforge_wasm_decode PRIVATE iclforge::ac3 iclforge::warnings)

# --bind: Embind, the JS<->C++ binding generator decoder_bindings.cpp's
# EMSCRIPTEN_BINDINGS macro uses. MODULARIZE+EXPORT_NAME: the loader becomes a
# plain factory function (createIclForgeModule()) rather than a global that
# would collide with a second WASM module on the same page - see index.html's
# import. ALLOW_MEMORY_GROWTH: an uploaded stream's decoded PCM size is not
# known ahead of time, unlike the bundled demo fixture. ENVIRONMENT=web: this
# is a browser page, not also a Node/worker target, so the smaller web-only
# runtime is enough. -fexceptions (compile AND link): iclforge::ac3 itself never
# throws (std::expected throughout, matching every other target in this
# project), but transitively-called std library paths (bad_alloc, vector::at)
# still can - and Emscripten's default of exceptions compiled out turns any
# such throw into an immediate hard abort rather than the ordinary unwind a
# desktop build gets, which is a worse failure mode for a page a stranger's
# browser is running than the modest code-size/perf cost of leaving them on.
target_compile_options(iclforge_wasm_decode PRIVATE "-fexceptions")
target_link_options(iclforge_wasm_decode PRIVATE
    "-fexceptions"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createIclForgeModule"
    "-sALLOW_MEMORY_GROWTH=1"
    # Growth needs a ceiling: without MAXIMUM_MEMORY the heap can climb to
    # wasm32's ~2 GB limit and the failure mode is the browser killing the
    # tab. With one, exhaustion is a std::bad_alloc (-fexceptions above)
    # that decoder_bindings.cpp turns into the demo's normal error path.
    # 1 GB comfortably holds the largest input demo.js accepts (see its
    # MAX_UPLOAD_BYTES); 64 MB up front covers the module and small clips
    # without a resize.
    "-sINITIAL_MEMORY=67108864"
    "-sMAXIMUM_MEMORY=1073741824"
    "-sENVIRONMENT=web"
    "-sNO_EXIT_RUNTIME=1")

set_target_properties(iclforge_wasm_decode PROPERTIES
    OUTPUT_NAME "iclforge_decode"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo")

add_custom_command(TARGET iclforge_wasm_decode POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/demo.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon-32.png"
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/demo.ec3"
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/assets"
    COMMENT "Assembling the servable wasm_decode_demo/ directory")

# ---------------------------------------------------------------------------
# in-browser encoding: the browser ENCODE page - a sibling module to the decode
# target above, not a bigger version of it. Kept as a second .wasm/.js
# rather than folded into iclforge_wasm_decode so a page that only needs one
# direction never pays for the other; see docs/platforms/wasm.md's "Encode
# module" section for the measured size/real-time numbers that motivated
# this split (a combined AC-3 + E-AC-3 + Atmos/JOC + QC module measured
# smaller raw, and about the same gzip, as the decode-only module alone).
#
# Produces ${CMAKE_BINARY_DIR}/bin/wasm_encode_demo/, the same
# "any static file server" story as wasm_decode_demo/.
# ---------------------------------------------------------------------------

add_executable(iclforge_wasm_encode encoder_bindings.cpp)
target_link_libraries(iclforge_wasm_encode PRIVATE iclforge::ac3 iclforge::warnings)

# Same reasoning as iclforge_wasm_decode's own comment on every flag below -
# -fexceptions, MODULARIZE+EXPORT_NAME (a distinct name so this module and
# the decode one can load on the same page without colliding), a memory
# ceiling turning exhaustion into an ordinary error path instead of a dead
# tab. A dropped WAV's decoded PCM (via the browser's own
# AudioContext.decodeAudioData - see encode/app.js) is bounded client-side
# before it ever reaches this module, the same way an uploaded stream is
# bounded before reaching the decode module.
target_compile_options(iclforge_wasm_encode PRIVATE "-fexceptions")
target_link_options(iclforge_wasm_encode PRIVATE
    "-fexceptions"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createIclForgeEncodeModule"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=67108864"
    "-sMAXIMUM_MEMORY=1073741824"
    "-sENVIRONMENT=web"
    "-sNO_EXIT_RUNTIME=1")

set_target_properties(iclforge_wasm_encode PROPERTIES
    OUTPUT_NAME "iclforge_encode"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/wasm_encode_demo")

# The round-trip preview (encode/app.js) loads the DECODE module too, and
# wasm_encode_demo/ has to be independently servable ("cd .../wasm_encode_demo
# && python3 -m http.server", the same story wasm_decode_demo/ has) rather
# than assuming a sibling directory exists - so iclforge_decode.js/.wasm are
# copied in here as well, not referenced across directories. That means this
# target's POST_BUILD step depends on iclforge_wasm_decode's own output
# already existing, hence the explicit add_dependencies: Ninja would
# otherwise happily run both targets' link steps in parallel with no
# ordering guarantee.
add_dependencies(iclforge_wasm_encode iclforge_wasm_decode)

add_custom_command(TARGET iclforge_wasm_encode POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/encode/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/encode/app.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon-32.png"
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/iclforge_decode.js"
        "${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/iclforge_decode.wasm"
        "${CMAKE_BINARY_DIR}/bin/wasm_encode_demo"
    COMMENT "Assembling the servable wasm_encode_demo/ directory")

# The Atmos object-authoring page (apps/wasm/atmos/) ships as a SUBDIRECTORY
# of the encode demo rather than a third top-level demo: it loads the very
# same iclforge_encode.js/.wasm via "../", so wasm_encode_demo/ stays the one
# directory that is copied around (docs site, local serving) with the
# authoring page inside it for free.
add_custom_command(TARGET iclforge_wasm_encode POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_BINARY_DIR}/bin/wasm_encode_demo/atmos"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/atmos/index.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/atmos/app.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/favicon-32.png"
        "${CMAKE_BINARY_DIR}/bin/wasm_encode_demo/atmos"
    COMMENT "Assembling the wasm_encode_demo/atmos/ authoring page")

# ---------------------------------------------------------------------------
# AC-4 (src/ac4, src/ac4dec, src/ac4enc): one combined decode+encode embind
# module, unlike the AC-3 decode/encode split above - see ac4_bindings.cpp's
# own header comment for why one module suits AC-4 here. Guarded on
# ICLFORGE_BUILD_AC4 (root CMakeLists.txt option(), default ON; the
# wasm-emscripten preset no longer forces it OFF - CMakePresets.json) so a
# future minimal-footprint WASM variant that turns AC-4 back off still
# configures cleanly.
#
# No servable demo page is assembled here (unlike wasm_decode_demo/ and
# wasm_encode_demo/ above): apps/wasm/ac4/ does not exist yet, and a minimal
# AC-4 demo page is optional polish this task's brief left to a later pass -
# the compiled iclforge_ac4.js/.wasm still land in their own output
# directory below, ready for one for whoever adds it.
# ---------------------------------------------------------------------------

if(ICLFORGE_BUILD_AC4)
    add_executable(iclforge_wasm_ac4 ac4_bindings.cpp)
    target_link_libraries(iclforge_wasm_ac4 PRIVATE iclforge::ac4 iclforge::ac4dec iclforge::ac4enc iclforge::warnings)

    # Same reasoning as iclforge_wasm_decode's own comment on every flag
    # below - -fexceptions, MODULARIZE+EXPORT_NAME (a third distinct name so
    # this module and the two AC-3 ones can all load on the same page
    # without colliding), a memory ceiling turning exhaustion into an
    # ordinary error path instead of a dead tab.
    target_compile_options(iclforge_wasm_ac4 PRIVATE "-fexceptions")
    target_link_options(iclforge_wasm_ac4 PRIVATE
        "-fexceptions"
        "--bind"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createIclForgeAc4Module"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=67108864"
        "-sMAXIMUM_MEMORY=1073741824"
        "-sENVIRONMENT=web"
        "-sNO_EXIT_RUNTIME=1")

    set_target_properties(iclforge_wasm_ac4 PROPERTIES
        OUTPUT_NAME "iclforge_ac4"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/wasm_ac4_demo")
endif()
