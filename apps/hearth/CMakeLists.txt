# Hearth's applications (planning/hearth-reference-player.md). So far the test sink and the test
# server, which need src/sendspin's full player and server halves, so an
# AC3FORGE_SENDSPIN_CORE_ONLY build leaves them out, and the notices for what those halves link.
if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    add_subdirectory(testsink)
    add_subdirectory(testserver)
    include("${CMAKE_CURRENT_SOURCE_DIR}/notices/notices.cmake")
endif()
