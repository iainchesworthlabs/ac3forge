# Copies the family's shared QML components (apps/gui/qml/*.qml: Theme, Card,
# SectionHeader, StatTile, AppButton, AppCheckBox, IconButton, AppSlider, AppTextField, AppComboBox, RailBlock,
# SegmentedControl, FocusRing) into a Qt
# application's own qml/shared/ directory, rewriting `import Ac3Forge` to that
# application's own module URI - generated INTO the source tree (ignored by
# git) rather than the
# build tree, because the QML ahead-of-time compiler names its cache files
# after the path relative to the source directory, and a build-tree path
# produces an unusable name with a drive letter in the middle of it.
#
# apps/crucible/CMakeLists.txt and apps/hearth/ui/CMakeLists.txt both call
# this instead of each running its own copy of the loop, which is how this
# file came to exist: each used to compute its own source path
# (${CMAKE_CURRENT_SOURCE_DIR}/../gui/qml/<name> and .../../../gui/qml/<name>
# respectively) and register it via set_property(DIRECTORY APPEND PROPERTY
# CMAKE_CONFIGURE_DEPENDS ...) in its own directory scope. Both spellings
# resolve to the same file, but CMake tracks CMAKE_CONFIGURE_DEPENDS entries
# by their literal, unnormalised string, so both landed - unmerged - in
# Ninja's single generated "re-run CMake if these change" phony-output edge.
# Ninja canonicalises paths when it loads the graph, finds that one file
# declared as that edge's output twice, and refuses to load build.ninja -
# a configure-time success that only fails at the next `ninja` invocation,
# with the duplicate hidden inside one very long generated line (a plain text
# search of build.ninja for the file name is not enough to find it - the
# giveaway is running the actual combined build).
#
# Routing every caller through one function, keyed off CMAKE_SOURCE_DIR
# (constant everywhere) rather than any caller's own CMAKE_CURRENT_SOURCE_DIR,
# and tracking already-registered sources in a GLOBAL property so a second
# caller skips the set_property call entirely, is what actually prevents the
# collision - a future third app copying the old inline loop would
# reintroduce it. Registering the dependency from whichever caller runs first
# is enough: CMAKE_CONFIGURE_DEPENDS's effect (mark the whole build for
# reconfiguration when the file changes) is project-wide regardless of which
# directory's scope it was registered from.
#
# module_uri:    this application's `import <uri>` replacement, e.g. Ac3ForgeCrucible
# out_dir:       where the rewritten copies are written, e.g. .../ui/qml/shared
# out_files_var: name of a variable (in the caller's scope) to receive the
#                list of generated file paths
function(ac3forge_stage_shared_qml module_uri out_dir out_files_var)
    set(names Theme.qml Card.qml SectionHeader.qml StatTile.qml AppButton.qml AppCheckBox.qml
              IconButton.qml AppSlider.qml AppTextField.qml AppComboBox.qml RailBlock.qml SegmentedControl.qml FocusRing.qml)
    file(MAKE_DIRECTORY "${out_dir}")
    get_property(tracked GLOBAL PROPERTY AC3FORGE_SHARED_QML_CONFIGURE_DEPENDS)
    set(generated)
    foreach(name IN LISTS names)
        set(src "${CMAKE_SOURCE_DIR}/apps/gui/qml/${name}")
        set(dst "${out_dir}/${name}")
        file(READ "${src}" contents)
        string(REPLACE "import Ac3Forge\n" "import ${module_uri}\n" contents "${contents}")
        file(WRITE "${dst}" "${contents}")
        if(NOT "${src}" IN_LIST tracked)
            # Re-run configure when the GUI's file changes, so the rewrite
            # follows it - registered at most once across every caller (see
            # this file's header for why that matters here).
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
            list(APPEND tracked "${src}")
            set_property(GLOBAL PROPERTY AC3FORGE_SHARED_QML_CONFIGURE_DEPENDS "${tracked}")
        endif()
        list(APPEND generated "${dst}")
    endforeach()
    set("${out_files_var}" "${generated}" PARENT_SCOPE)
endfunction()
