# Hearth's applications (planning/hearth-reference-player.md). So far the test sink, which needs
# src/sendspin's full player half, so an AC3FORGE_SENDSPIN_CORE_ONLY build leaves it out.
if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    add_subdirectory(testsink)
endif()
