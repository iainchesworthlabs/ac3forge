# ---------------------------------------------------------------------------
# hearth - the desktop reference player's window
# (planning/hearth-reference-player.md, A5). Qt is a PREBUILT dependency
# discovered by cmake/FindQt6.cmake, never vcpkg (qt-integration-convention),
# the same as apps/gui and apps/crucible. QUIET, and a warning rather than a
# hard failure, so a core-only or Hearth-engine-only build still configures
# on a machine with no Qt kit - apps/crucible/CMakeLists.txt's own arm is
# the precedent this follows.
#
# included from ../CMakeLists.txt, itself only reached under the root's
# `ICLFORGE_BUILD_HEARTH AND NOT ICLFORGE_SENDSPIN_PLAYER_ONLY` guard - which
# the ESP-IDF component never satisfies (feedback-root-cmake-guards-need-
# esp-idf-build: CONFIG_ICLFORGE_SENDSPIN pre-seeds ICLFORGE_BUILD_HEARTH=ON
# together with ICLFORGE_SENDSPIN_PLAYER_ONLY=ON, and every other board shape
# leaves ICLFORGE_BUILD_HEARTH at its default OFF). So this file is never
# entered by a board configure, and needs no guard of its own for that - see
# this change's PR for the WSL configure-only check that confirmed it.
# ---------------------------------------------------------------------------
# LinguistTools used to be left out of this call deliberately, because this
# file had no qt_add_translations() call, no translations/ directory and no
# .ts files - Hearth's window had no localisation pipeline at all - so the
# component was pure copy-paste ballast that only made Qt6_FOUND more
# fragile than it needed to be. That cost real build coverage (issue #890):
# a WSL image with
# qt6-base-dev and qt6-declarative-dev installed, but not qt6-tools-dev (the
# package that actually ships Qt6LinguistToolsConfig.cmake - see
# .github/workflows/_ci-linux.yml's "Install Qt6 (Linux GUI)" step comment
# for the full qt6-l10n-tools/qt6-tools-dev split), left Qt6_FOUND false for
# no reason a syntax-only build cares about, which silently dropped the
# entire hearth target - and every apps/hearth/ui/*.cpp compile_commands.json
# entry along with it - behind the message(WARNING) below, with nothing
# louder to catch it.
#
# It is back now, in exactly the commit that comment asked for: the one that
# gives Hearth its first qt_add_translations() call, below. The package that
# was missing in #890 (qt6-tools-dev, which is where
# Qt6LinguistToolsConfig.cmake actually ships - qt6-l10n-tools carries only
# the binaries) is installed by both Linux legs of the workflow today; see
# .github/workflows/_ci-linux.yml's own "Install Qt6 (Linux GUI)" step, whose
# comment spells out the same split.
find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Qml Quick QuickControls2 LinguistTools)

# Exported so apps/hearth/notices/notices.cmake (include()d from
# apps/hearth/CMakeLists.txt, one directory up) can tell whether the hearth
# package's NOTICES.txt needs a Qt section, on the platforms that bundle Qt
# into it (Windows, macOS - the install() rules below). Set unconditionally,
# not only in the Qt6_FOUND branch, so that file can tell "Qt6 not found
# here" (this defined and false) apart from "apps/hearth/CMakeLists.txt
# hasn't add_subdirectory()'d this directory yet" (not defined at all) - the
# same PARENT_SCOPE-across-add_subdirectory() reason apps/gui/CMakeLists.txt
# exports ICLFORGE_GUI_QT_VERSION for apps/notices/notices.cmake.
set(ICLFORGE_HEARTH_UI_QT_FOUND "${Qt6_FOUND}" PARENT_SCOPE)

if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found: hearth is skipped; the engine and its tests still build.")
else()
    # The version, for that same Qt section. Empty here while
    # ICLFORGE_HEARTH_UI_QT_FOUND reads true would mean this export broke loose
    # from the find_package it is meant to travel with - notices.cmake
    # treats that as a configure error rather than a blank notice.
    set(ICLFORGE_HEARTH_UI_QT_VERSION "${Qt6_VERSION}" PARENT_SCOPE)

    qt_standard_project_setup(REQUIRES 6.8)

    # The shared family components, copied from apps/gui/qml and rewritten by
    # cmake/SharedFamilyQml.cmake's iclforge_stage_shared_qml() - the same
    # function apps/crucible/CMakeLists.txt calls (see that file's header for
    # why this is a shared function rather than each app running its own copy
    # of the loop). Edit apps/gui/qml/*.qml and every app that copies it follows.
    include(SharedFamilyQml)
    set(ICLFORGE_HEARTH_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qml/shared")
    iclforge_stage_shared_qml(Hearth "${ICLFORGE_HEARTH_SHARED_DIR}" ICLFORGE_HEARTH_SHARED_OUT)

    qt_add_executable(hearth
        main.cpp
        hearth_controller.cpp
        hearth_controller.hpp
        item_loader.cpp
        item_loader.hpp
        network_controller.cpp
        network_controller.hpp
        qsettings_store.hpp
        # The family's own translator installer, shared rather than copied -
        # apps/crucible/CMakeLists.txt takes it from the same place, and the
        # class's own header says it exists so a second Qt app in this
        # repository reuses it.
        "${PROJECT_SOURCE_DIR}/apps/gui/language_manager.cpp"
        "${PROJECT_SOURCE_DIR}/apps/gui/language_manager.hpp"
        "${PROJECT_SOURCE_DIR}/apps/common/container_input.cpp"
        "${PROJECT_SOURCE_DIR}/apps/common/container_input.hpp")
    target_include_directories(hearth PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/apps/gui"
        "${CMAKE_CURRENT_SOURCE_DIR}/../engine"
        "${PROJECT_SOURCE_DIR}/apps/common")
    target_link_libraries(hearth PRIVATE
        hearth_engine
        # container_input.cpp's own container readers - apps/gui/CMakeLists.txt's
        # forge-gui target links the same three for the same reason: the media
        # information this application's loader hands to describe_media() needs
        # a container's own facts (container_input.hpp, "whose functions the
        # engine does not compile" - apps/hearth/engine/session.hpp's comment).
        iclforge::matroska
        iclforge::mp4
        iclforge::mpegts
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)

    set(ICLFORGE_HEARTH_QML_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/OutputPicker.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PlayPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PlayObjectsCard.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PlaySignalPathCard.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Media.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Speakers.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/DecoderPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/DecoderEac3.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/DecoderAc4.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Network.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkList.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkPairing.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkInfo.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkSettings.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkSpeakers.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkDecoder.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkFirmware.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkReport.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkSinkOnlyOnSink.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/NetworkGroupEdit.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PlaceholderPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Settings.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/ShortcutsDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/TransportBar.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/AboutDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/LicencesDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/FirstRunDialog.qml"
        ${ICLFORGE_HEARTH_SHARED_OUT})
    foreach(qml_file IN LISTS ICLFORGE_HEARTH_QML_FILES)
        get_filename_component(qml_name "${qml_file}" NAME)
        set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
    endforeach()
    set_source_files_properties("${ICLFORGE_HEARTH_SHARED_DIR}/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

    qt_add_qml_module(hearth
        URI Hearth
        VERSION 1.0
        QML_FILES ${ICLFORGE_HEARTH_QML_FILES})

    # The family's own faces and application icon, reused rather than
    # duplicated - apps/crucible/CMakeLists.txt does the same for the same
    # reason.
    #
    # MaterialSymbolsSharp-Regular.ttf is Google's Material Symbols Sharp
    # variable font (github.com/google/material-design-icons, Apache
    # License 2.0, licence text at fonts/MaterialSymbolsSharp-Apache-2.0.txt
    # - kept beside the face, same reason Archivo's OFL.txt is), pinned to
    # its default static instance (FILL=0, GRAD=0, opsz=24, wght=400) and
    # subset to the 8 glyphs Hearth's transport/queue controls actually use
    # (play_arrow e037, pause e034, stop e047, skip_previous e045,
    # skip_next e044, create_new_folder e2cc, add e145, volume_up e050) via
    # fontTools - 2 KB rather than the upstream file's 8.46 MB. Only Hearth
    # ships this today, so it is not in apps/gui's own font resource list.
    qt_add_resources(hearth "hearth_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/fonts"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Regular.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Medium.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-SemiBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-ExtraBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/MaterialSymbolsSharp-Regular.ttf"
            # Archivo has no Arabic or Hebrew coverage, and Yiddish is
            # written in Hebrew script, so without these two faces three of
            # the six languages below render as tofu. apps/gui bundles the
            # same pair for the same reason.
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/NotoSansArabic.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/NotoSansHebrew.ttf")
    qt_add_resources(hearth "hearth_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/icons"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/icons/iclforge-32.png"
            "${CMAKE_SOURCE_DIR}/apps/gui/icons/iclforge-256.png")
    # The package's own NOTICES.txt, so the Licences dialog (About > Licences)
    # shows exactly what this build ships - the same qt_add_resources call
    # apps/crucible/CMakeLists.txt makes for crucible_notices. A
    # placeholder file exists here already (../CMakeLists.txt writes it
    # before add_subdirectory(ui), right where ICLFORGE_HEARTH_NOTICES_FILE is
    # set); notices/notices.cmake overwrites it with the real content once
    # this file's own Qt detection above has run, still well before ninja's
    # rcc step reads it at build time.
    get_filename_component(ICLFORGE_HEARTH_NOTICES_OUT_DIR "${ICLFORGE_HEARTH_NOTICES_FILE}" DIRECTORY)
    qt_add_resources(hearth "hearth_notices"
        PREFIX "/notices"
        BASE "${ICLFORGE_HEARTH_NOTICES_OUT_DIR}"
        FILES "${ICLFORGE_HEARTH_NOTICES_FILE}")

    # ---------------------------------------------------------------------
    # Localisation. The same six languages forge-gui and Crucible ship, through
    # the same LanguageManager (compiled into this target above), which loads
    # :/i18n/hearth_<code>.qm at runtime.
    #
    # The catalogues are generated, not hand-written: run
    # `cmake --build <dir> --target hearth_lupdate` to re-extract every
    # qsTr() in ICLFORGE_HEARTH_QML_FILES, and every tr() in this target's C++,
    # into them after changing a string.
    # -no-obsolete for the reason apps/gui and Crucible both give: an entry
    # whose source string is gone is kept as translation memory otherwise,
    # and git history is that memory.
    #
    # They carry every source string and NO translations yet - each entry is
    # marked unfinished, which is what Qt calls an untranslated message, and
    # what it does with one is fall back to the English source. So choosing a
    # language today changes the layout direction and the font (which is why
    # the two Noto faces are bundled above) but leaves the words in English
    # until a translator fills these in. That is a content task, not a build
    # one; the machinery is what was missing.
    #
    # Deliberately no test asserting the catalogues are complete, unlike
    # tests/crucible/test_translations.cpp: Crucible's are filled, and
    # holding these to the same bar would fail from the first commit.
    #
    # CI does check that they match the sources: the windows-msvc leg
    # reruns hearth_lupdate and fails if the result differs from what is
    # committed in anything but <location> lines
    # (.github/workflows/_ci-windows.yml, "Check the Hearth translations are
    # up to date"). So moving a string within its file needs no
    # regeneration, and adding, removing or rewording one does.
    set(ICLFORGE_HEARTH_TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/hearth_fr.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/hearth_de.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/hearth_es.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/hearth_ar.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/hearth_he.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/hearth_yi.ts")
    qt_add_translations(hearth
        IMMEDIATE_CALL
        SOURCE_TARGETS hearth
        TS_FILES ${ICLFORGE_HEARTH_TS_FILES}
        LUPDATE_OPTIONS -no-obsolete
        LUPDATE_TARGET hearth_lupdate
        LRELEASE_TARGET hearth_lrelease)

    set_target_properties(hearth PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(WIN32)
        enable_language(RC)
        set(ICLFORGE_GUI_ICON_ICO "${CMAKE_SOURCE_DIR}/apps/gui/icons/iclforge.ico")
        file(TO_CMAKE_PATH "${ICLFORGE_GUI_ICON_ICO}" ICLFORGE_GUI_ICON_ICO)
        configure_file("${CMAKE_SOURCE_DIR}/apps/gui/icons/forge-gui.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/hearth.rc" @ONLY)
        target_sources(hearth PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/hearth.rc")
        set_target_properties(hearth PROPERTIES WIN32_EXECUTABLE ON)
    elseif(APPLE)
        set(ICLFORGE_HEARTH_ICON_ICNS "${CMAKE_SOURCE_DIR}/apps/gui/icons/iclforge.icns")
        set_source_files_properties("${ICLFORGE_HEARTH_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(hearth PRIVATE "${ICLFORGE_HEARTH_ICON_ICNS}")
        set_target_properties(hearth PROPERTIES MACOSX_BUNDLE ON)

        # A custom Info.plist (Info.plist.in, beside this file), the same
        # reason and shape as apps/gui/CMakeLists.txt's own APPLE branch:
        # hearth now owns .ac3/.ec3's CFBundleDocumentTypes/
        # UTExportedTypeDeclarations (Hearth phase A7), which the default
        # template CMake would otherwise generate carries neither. These are
        # plain CMake variables, not target properties, because that is what
        # CMake's own bundle-generation logic substitutes into Info.plist.in's
        # ${VAR} tokens.
        set(MACOSX_BUNDLE_EXECUTABLE_NAME "hearth")
        set(MACOSX_BUNDLE_BUNDLE_NAME "Hearth")
        set(MACOSX_BUNDLE_INFO_STRING
            "Play AC-3, E-AC-3 and AC-4 streams on speakers, a receiver or a Sendspin network")
        set(MACOSX_BUNDLE_ICON_FILE "iclforge.icns")
        set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.iainchesworthlabs.hearth")
        set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
        set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
        set(MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION_FULL}")
        set(MACOSX_BUNDLE_COPYRIGHT "Copyright (c) Iain Chesworth")
        set_target_properties(hearth PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
    endif()

    # Qt runtime next to the executable, as apps/gui and apps/crucible do it,
    # so the window runs from the build tree. Windows only - see
    # apps/crucible/CMakeLists.txt's own comment on why a Linux build needs
    # nothing copied.
    if(WIN32)
        get_filename_component(ICLFORGE_HEARTH_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
        find_program(ICLFORGE_HEARTH_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${ICLFORGE_HEARTH_QT_BIN_DIR}")
        if(ICLFORGE_HEARTH_WINDEPLOYQT)
            add_custom_command(TARGET hearth POST_BUILD
                COMMAND "${ICLFORGE_HEARTH_WINDEPLOYQT}"
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    --qmldir "${ICLFORGE_HEARTH_SHARED_DIR}"
                    --compiler-runtime
                    "$<TARGET_FILE:hearth>"
                COMMENT "Deploying Qt runtime dependencies for hearth")
        else()
            message(WARNING "windeployqt not found next to Qt6; hearth.exe will not run standalone from the build tree.")
        endif()
    endif()

    # Four windeployqt passes already share bin/ before this target joins them
    # (apps/crucible/CMakeLists.txt's own comment: forge-gui's, forge_gui_qmltests's,
    # crucible's, crucible_qmltests's, chained one after another because
    # two of them copying the same Qt file into bin/ at once fails with
    # "Cannot create a file when that file already exists" on a real "build
    # everything" run). hearth's single pass joins the end of that chain -
    # add_dependencies is transitive for ordering, so chaining after the
    # deepest target that exists is enough; nothing needs re-checking against
    # every earlier link. hearth_qmltests (issue #886, tests/CMakeLists.txt's
    # own add_dependencies(hearth_qmltests hearth), included below) now
    # extends that same chain one more link the same way crucible_qmltests
    # chains after crucible, rather than starting a second one.
    if(WIN32 AND ICLFORGE_HEARTH_WINDEPLOYQT)
        if(TARGET crucible_qmltests)
            add_dependencies(hearth crucible_qmltests)
        elseif(TARGET crucible)
            add_dependencies(hearth crucible)
        elseif(TARGET forge_gui_qmltests)
            add_dependencies(hearth forge_gui_qmltests)
        elseif(TARGET forge-gui)
            add_dependencies(hearth forge-gui)
        endif()
    endif()

    # hearth's own Qt Quick Test suite (issue #886, A5's exit): tests/CMakeLists.txt,
    # include()d rather than add_subdirectory()'d so it sees ICLFORGE_HEARTH_QML_FILES and
    # ICLFORGE_HEARTH_SHARED_DIR as this scope set them, and so its own qt_add_executable()/
    # qt_add_qml_module() pair runs in THIS directory's scope rather than a nested one -
    # apps/gui/CMakeLists.txt's own comment on its identical include() explains why that
    # distinction is load-bearing for AUTOMOC's metatypes JSON handoff to qmltyperegistrar.
    if(ICLFORGE_BUILD_TESTS)
        include(tests/CMakeLists.txt)
    endif()

    # ---------------------------------------------------------------------
    # Install (CPack component "hearth", cmake/Packaging.cmake) - Hearth
    # phase A7. Windows (NSIS + ZIP), macOS (DragNDrop) and Linux (DEB, RPM,
    # TGZ), the same three platforms apps/crucible/CMakeLists.txt installs
    # to and for the same reason its own comment gives: the guard above
    # names them rather than dropping a platform with no arm here.
    # ---------------------------------------------------------------------
    install(TARGETS hearth
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT hearth
        BUNDLE DESTINATION "." COMPONENT hearth)

    # Only where the package carries its own Qt - the same condition and the
    # same reasoning as apps/crucible/CMakeLists.txt's own install-time
    # deployment: on Linux the system's Qt is the one the binary was built
    # against, so the package ships none.
    if(WIN32 OR APPLE)
        qt_generate_deploy_qml_app_script(
            TARGET hearth
            OUTPUT_SCRIPT ICLFORGE_HEARTH_DEPLOY_SCRIPT
            NO_UNSUPPORTED_PLATFORM_ERROR)
        install(SCRIPT "${ICLFORGE_HEARTH_DEPLOY_SCRIPT}" COMPONENT hearth)
        # Immediately after it, and only after it: that script deploys Qt's
        # test module into the package, because Qt scans this whole
        # directory for QML imports and tests/qml/tst_*.qml all `import
        # QtTest` - now that hearth_qmltests exists (the include() above),
        # hearth is no longer the exception apps/crucible/CMakeLists.txt's
        # own comment on this exact measurement described. Nothing a user
        # runs loads qml/QtTest, Qt6Test.dll, Qt6QuickTest.dll or (macOS)
        # Contents/PlugIns/libquicktestplugin.dylib - only hearth_qmltests
        # does, and that binary is never installed. cmake/StripQtTestDeployment.cmake
        # carries the whole measurement; that script needs this bundle's name
        # set in the same install pass it runs in - hence install(CODE) with
        # the SAME COMPONENT immediately before it, only on APPLE since the
        # Windows shape needs no such variable.
        if(APPLE)
            install(CODE "set(_ac3_macos_bundle_name \"hearth\")" COMPONENT hearth)
        endif()
        install(SCRIPT "${CMAKE_SOURCE_DIR}/cmake/StripQtTestDeployment.cmake"
            COMPONENT hearth)
    endif()

    if(WIN32)
        # The notices and the licence at the archive root, beside bin/ - the
        # same layout apps/crucible/CMakeLists.txt uses and for the same
        # reason (a package-shape check follows in this phase).
        install(FILES "${ICLFORGE_HEARTH_NOTICES_FILE}" DESTINATION "." COMPONENT hearth)
        install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "." RENAME "LICENSE.txt" COMPONENT hearth)
    elseif(APPLE)
        # The notices and the licence at the archive root, beside
        # hearth.app - apps/crucible/CMakeLists.txt's own comment on why
        # this is safe to share a destination with apps/notices/notices.cmake's
        # runtime-component pair and with Crucible's own pair applies here
        # too: add_subdirectory(apps/hearth) runs before
        # cmake/Packaging.cmake's include(), so whichever installs last wins
        # in the one place these could collide (the monolithic macOS .dmg),
        # and none of the three ships as that .dmg's actual product - each is
        # its own standalone archive.
        install(FILES "${ICLFORGE_HEARTH_NOTICES_FILE}" DESTINATION "." COMPONENT hearth)
        install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "." RENAME "LICENSE.txt" COMPONENT hearth)
    elseif(LINUX)
        # What a freedesktop menu needs: the launcher, the AppStream record a
        # software centre reads, and the icon in the hicolor theme at the two
        # sizes the window itself embeds - the same layout apps/gui installs
        # under `runtime` and apps/crucible/CMakeLists.txt installs under
        # `crucible`, this is `hearth` because the application is its own
        # package (cmake/Packaging.cmake). No hearth-mime.xml: the
        # audio/ac3 and audio/eac3 shared-mime-info types are already
        # declared by apps/gui/packaging/linux/forge-gui-mime.xml, which every
        # Linux build installs unconditionally
        # (apps/notices/notices.cmake's runtime component) - a second
        # <mime-type> declaring the same glob would conflict rather than add
        # anything, so this component only CLAIMS the types (.desktop's
        # MimeType=, this metainfo.xml's <provides><mediatype>), which is
        # this package's own thing to state.
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/hearth.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT hearth)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/hearth.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT hearth)
        install(FILES "${CMAKE_SOURCE_DIR}/apps/gui/icons/iclforge-256.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
            RENAME "hearth.png" COMPONENT hearth)
        install(FILES "${CMAKE_SOURCE_DIR}/apps/gui/icons/iclforge-32.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps"
            RENAME "hearth.png" COMPONENT hearth)
        # The notices and the licence under share/doc/<package>/, where a
        # .deb keeps them - apps/crucible/CMakeLists.txt's own comment on
        # the "copyright" duplicate applies here identically: it is the name
        # dpkg and lintian look for, which CPack's DEB generator never writes.
        install(FILES "${ICLFORGE_HEARTH_NOTICES_FILE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge-hearth" COMPONENT hearth)
        install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge-hearth"
            RENAME "LICENSE.txt" COMPONENT hearth)
        install(FILES "${ICLFORGE_HEARTH_NOTICES_FILE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge-hearth"
            RENAME "copyright" COMPONENT hearth)
    endif()
endif()
