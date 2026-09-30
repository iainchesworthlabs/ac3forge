# ac3hearth_engine (planning/hearth-reference-player.md, A3): the player's
# engine, with no Qt in it. The application's window (A5) will be a client of
# this library rather than the place any of it lives, the same split
# apps/crucible/{engine,ui} already keeps - and the same reason: everything
# here can then be tested by ac3tests on every CI leg, including the legs
# with no display and no sound card.
add_library(ac3hearth_engine STATIC
    ac3_transcoder.cpp
    ac4_stream.cpp
    decoder_settings.cpp
    device_sink.cpp
    diagnostic_log.cpp
    diagnostics_report.cpp
    engine_thread.cpp
    media_info.cpp
    media_inspector.cpp
    network_view.cpp
    output_decision.cpp
    output_selector.cpp
    passthrough_sink.cpp
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

# native_log_sink.hpp's one function, exactly one platform/<os>/*.cpp
# defining it, no #ifdef selecting between them - the same shape
# apps/crucible/engine/platform_services.hpp's own comment states for
# platform_session_monitor(). Windows' is the only one that needs
# <windows.h>, hence the same WIN32_LEAN_AND_MEAN/NOMINMAX pair Crucible's
# own WIN32 block already sets.
if(WIN32)
    target_sources(ac3hearth_engine PRIVATE platform/windows/native_log_sink.cpp)
    target_compile_definitions(ac3hearth_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_sources(ac3hearth_engine PRIVATE platform/posix/native_log_sink.cpp)
endif()

# The pairing records are a key ring for src/sendspin's server sessions, which
# a core-only build leaves out, so they are left out with them, and the server
# identity those records are bound to with them. NetworkSinks (discovery and
# pairing, A6) needs the same server half - ServerHost and the computer's own
# mdns backend - plus PairingStore itself. network_group_sink.cpp is the same
# reason: it wraps ac3::sendspin::Group.
if(NOT ICLFORGE_SENDSPIN_CORE_ONLY)
    target_sources(ac3hearth_engine PRIVATE pairing_store.cpp server_identity.cpp network_sinks.cpp
                                            network_group_sink.cpp)
    # A Hearth sink's firmware (planning/esp32-ota.md, O5): the board's own HTTP routes over
    # cpp-httplib, configured as src/sendspin configures it, and the board's own headers for what
    # GET /firmware says and what an image must be (esp-idf/iclforge/include, free of ESP-IDF,
    # as ac3tests already reads them), so that this app holds an image to the rules the board
    # holds it to.
    target_sources(ac3hearth_engine PRIVATE sink_firmware.cpp sink_firmware_view.cpp)
    target_include_directories(ac3hearth_engine PUBLIC "${PROJECT_SOURCE_DIR}/esp-idf/iclforge/include")
    # The opt-in loopback diagnostics endpoint (diagnostics_server.hpp): the
    # same httplib already linked in for sink_firmware.cpp above, so it adds
    # nothing new to a core-only build to leave out.
    target_sources(ac3hearth_engine PRIVATE diagnostics_server.cpp)
    target_link_libraries(ac3hearth_engine PRIVATE iclforge_sendspin_httplib)
endif()

# From apps/common, which has no library target: the container facts an item's
# loader hands over (container_input.hpp, whose functions the application's
# loader compiles), probe's JSON stream summary, which the media information
# writes as ac3cli probe does, and Ac4ObjectRenderer (planning/ac4.md, I5),
# the same ac3::render::LayoutRenderer-based panner ac3cli's own 'decode'
# plays AC-4 objects through, compiled in here so the engine's AC-4 path can
# take it up too.
target_sources(ac3hearth_engine PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/common/probe_json.cpp"
    "${PROJECT_SOURCE_DIR}/apps/common/ac4_object_render.cpp")
target_include_directories(ac3hearth_engine PUBLIC "${PROJECT_SOURCE_DIR}/apps/common")

# iclforge::audio for the output side (PcmOutput, PassthroughSink, the device
# records the output decision reads), iclforge::ac3 for the decoder and the
# renderer, iclforge::ac4 for an AC-4 item's table of contents and iclforge::ac4dec for
# its presentations and metadata (probe_json's AC-4 walk), and iclforge::sendspin
# for the JSON writer the media information is written with, the key ring the
# pairing records implement (pairing_store.hpp), discovery and pairing
# themselves (network_sinks.hpp), and Group, the burst/PCM streaming path a
# network output plays through (network_group_sink.hpp). iclforge::signing asks
# whether frames carry an authenticity tag.
target_link_libraries(ac3hearth_engine PUBLIC iclforge::audio iclforge::ac3 iclforge::ac4 iclforge::ac4dec iclforge::sendspin)
# iclforge::coverage (coverage floors, tools/checks/coverage_report.sh's "apps/hearth" row) instruments
# this target's OWN sources - apps/cli's own comment on its matching link explains why the
# instrumented EXECUTABLE that eventually links this library is not enough on its own: --coverage
# is a compile-time flag, so without this a coverage build's .cpp files here would produce no
# .gcno at all and read as no data rather than as low coverage.
target_link_libraries(ac3hearth_engine PRIVATE iclforge::warnings iclforge::fmt iclforge::signing iclforge::coverage)
# The engine thread (engine_thread.cpp).
find_package(Threads REQUIRED)
target_link_libraries(ac3hearth_engine PUBLIC Threads::Threads)

# The profiling header pair, PUBLIC so the window (ui/, linked PRIVATE below
# it in ../ui/CMakeLists.txt) and ac3hearth_qmltests (which links this same
# library) both resolve every `#include "iclforge/base/detail/profiling.hpp"` they
# carry - apps/crucible/CMakeLists.txt's own ac3crucible_engine target sets up
# the identical pair, for the identical reason (its own comment there has the
# history of why this lives at the engine level rather than per-platform or
# per-executable).
target_link_libraries(ac3hearth_engine PUBLIC iclforge::tracy)
target_include_directories(ac3hearth_engine PUBLIC
    "$<IF:$<BOOL:${ICLFORGE_ENABLE_TRACY}>,${CMAKE_SOURCE_DIR}/src/base/variants/profiling-tracy_enabled,${CMAKE_SOURCE_DIR}/src/base/variants/profiling-tracy_disabled>")
