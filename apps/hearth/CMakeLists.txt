# Hearth's applications (planning/hearth-reference-player.md).
#
# The engine is unconditional: it is the player itself - the queue, the
# transport, a session per item, the output decision - and it needs nothing
# from src/sendspin's player half, so a core-only build still builds and
# tests it. The test sink does need that half, so an
# AC3FORGE_SENDSPIN_CORE_ONLY build leaves it out, along with the notices for
# what it links.
add_subdirectory(engine)

if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    add_subdirectory(testsink)
    include("${CMAKE_CURRENT_SOURCE_DIR}/notices/notices.cmake")
endif()
