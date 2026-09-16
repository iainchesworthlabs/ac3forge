# ac3hearth_engine (planning/hearth-reference-player.md, A3): the player's
# engine, with no Qt in it. The application's window (A5) will be a client of
# this library rather than the place any of it lives, the same split
# apps/crucible/{engine,ui} already keeps - and the same reason: everything
# here can then be tested by ac3tests on every CI leg, including the legs
# with no display and no sound card.
add_library(ac3hearth_engine STATIC
    device_sink.cpp
    output_decision.cpp
    player.cpp
    queue.cpp
    session.cpp
    stream_decoder.cpp
    transport.cpp)
target_include_directories(ac3hearth_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ac3hearth_engine PUBLIC cxx_std_23)

# ac3::audio for the output side (PcmOutput, PassthroughSink, the device
# records the output decision reads), ac3::forge for the decoder and the
# renderer. ac3::sendspin joins when the network output lands.
target_link_libraries(ac3hearth_engine PUBLIC ac3::audio ac3::forge)
target_link_libraries(ac3hearth_engine PRIVATE ac3::warnings ac3::fmt)
