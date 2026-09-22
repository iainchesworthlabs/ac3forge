# Hearth's applications (planning/hearth-reference-player.md).
#
# The engine is unconditional: it is the player itself - the queue, the
# transport, a session per item, the output decision - and it needs nothing
# from src/sendspin's player half, so a core-only build still builds and
# tests it, less its pairing records. The test sink, the test server and the
# window all need more than that core (the window needs Qt and, once
# packaged, notices for what it bundles; A7's packaging targets neither a
# fuzz build nor AC3FORGE_SENDSPIN_CORE_ONLY has ever meant to reach), so an
# AC3FORGE_SENDSPIN_CORE_ONLY build (fuzz/run.sh, the only thing that sets it)
# leaves all three out, along with the notices for what they link.
add_subdirectory(engine)

if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    # The eventual NOTICES.txt path - defined here, not inside notices.cmake,
    # so ui/'s own install() rules (below) can reference it before that
    # file's content is actually generated. This breaks what would otherwise
    # be a real cycle: ui/CMakeLists.txt's install(FILES
    # "${AC3HEARTH_NOTICES_FILE}" ...) rules need this path, while
    # notices.cmake's Qt section needs ui/'s own PARENT_SCOPE exports
    # (AC3HEARTH_UI_QT_FOUND, AC3HEARTH_UI_QT_VERSION, set right after its
    # find_package(Qt6 ...)) - so notices.cmake has to include() AFTER
    # add_subdirectory(ui), but the path string itself depends on neither and
    # can be settled first. install() only needs the path at configure time;
    # the file it names just has to exist by the time something installs,
    # which it does once this whole configure finishes - ac3_generate_notices
    # (below, via notices.cmake) writes it with plain file(WRITE), and every
    # add_subdirectory()/include() here runs as one configure regardless of
    # order.
    set(AC3HEARTH_NOTICES_FILE "${CMAKE_BINARY_DIR}/notices/hearth/NOTICES.txt")

    # The window (A5) before notices.cmake, for the Qt-export reason above.
    # Same three platforms as apps/crucible, and for the same reason:
    # apps/crucible/CMakeLists.txt's own comment on why the guard names them
    # rather than dropping a platform with no arm here.
    if(WIN32 OR APPLE OR LINUX)
        add_subdirectory(ui)
    endif()
    add_subdirectory(testsink)
    add_subdirectory(testserver)
    include("${CMAKE_CURRENT_SOURCE_DIR}/notices/notices.cmake")
endif()
