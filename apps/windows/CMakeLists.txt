# The Windows Desktop Atmos Demo (roadmap UX11): docs/platforms/windows-demo.md.
#
# engine/ is the headless part - what the UI drives and the spikes proved -
# split in two the way the CLI's own platform/ directory is: the pure modules
# (slot plan, bed mixer, placement, output policy) have no Windows in them and
# are ALSO compiled into ac3tests on every platform (tests/CMakeLists.txt);
# the platform modules underneath talk to WASAPI, the shell and the session
# manager and only ever build here.

add_library(ac3windemo_engine STATIC
    engine/slots.cpp
    engine/bed_mixer.cpp
    engine/placement.cpp
    engine/output_policy.cpp)
add_library(ac3::windemo_engine ALIAS ac3windemo_engine)
target_include_directories(ac3windemo_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/engine")
target_compile_features(ac3windemo_engine PUBLIC cxx_std_23)
target_link_libraries(ac3windemo_engine
    PUBLIC ac3::forge ac3::audio ac3::signing
    PRIVATE ac3::warnings)
