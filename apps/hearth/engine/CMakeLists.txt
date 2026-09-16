# ac3hearth_engine (planning/hearth-reference-player.md, A3): the player's
# engine, with no Qt in it. The application's window (A5) will be a client of
# this library rather than the place any of it lives, the same split
# apps/crucible/{engine,ui} already keeps - and the same reason: everything
# here can then be tested by ac3tests on every CI leg, including the legs
# with no display and no sound card.
add_library(ac3hearth_engine STATIC
    decoder_settings.cpp
    device_sink.cpp
    diagnostic_log.cpp
    diagnostics_report.cpp
    engine_thread.cpp
    media_info.cpp
    media_inspector.cpp
    output_decision.cpp
    play_meters.cpp
    player.cpp
    queue.cpp
    session.cpp
    settings_model.cpp
    stream_decoder.cpp
    transport.cpp
    unit_reports.cpp)
target_include_directories(ac3hearth_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ac3hearth_engine PUBLIC cxx_std_23)

# The pairing records are a key ring for src/sendspin's server sessions, which
# a core-only build leaves out, so they are left out with them.
if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    target_sources(ac3hearth_engine PRIVATE pairing_store.cpp)
endif()

# From apps/common, which has no library target: the container facts an item's
# loader hands over (container_input.hpp, whose functions the application's
# loader compiles), and probe's JSON stream summary, which the media
# information writes as ac3cli probe does and so is compiled in here.
target_sources(ac3hearth_engine PRIVATE "${PROJECT_SOURCE_DIR}/apps/common/probe_json.cpp")
target_include_directories(ac3hearth_engine PUBLIC "${PROJECT_SOURCE_DIR}/apps/common")

# ac3::audio for the output side (PcmOutput, PassthroughSink, the device
# records the output decision reads), ac3::forge for the decoder and the
# renderer, ac4::ac4 for an AC-4 item's table of contents, and ac3::sendspin
# for the JSON writer the media information is written with and the key ring
# the pairing records implement (pairing_store.hpp); the rest of
# ac3::sendspin joins when the network output lands. ac3::signing asks
# whether frames carry an authenticity tag.
target_link_libraries(ac3hearth_engine PUBLIC ac3::audio ac3::forge ac4::ac4 ac3::sendspin)
target_link_libraries(ac3hearth_engine PRIVATE ac3::warnings ac3::fmt ac3::signing)
# The engine thread (engine_thread.cpp).
find_package(Threads REQUIRED)
target_link_libraries(ac3hearth_engine PUBLIC Threads::Threads)
