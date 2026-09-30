# ---------------------------------------------------------------------------
# IclforgeLibrary.cmake
#
# One function for the library shape every src/<lib> shares, in place of the 40-odd lines each of
# them repeated with the name swapped:
#
#   iclforge_add_library(<name>
#       SOURCES <file>...          # relative to the calling directory
#       [DEPENDS <lib>...]         # other iclforge libraries; the objects compile against them, and
#                                  # the static and shared wrappers link the matching variant PUBLIC
#       [LINK_PRIVATE <item>...]   # extra PRIVATE links of the objects (wrap dev-only ones in BUILD_INTERFACE)
#       [PUBLIC_INCLUDES <dir>...] # extra include directories the library's users see (build tree only)
#       [PRIVATE_INCLUDES <dir>...]
#       [EXPORT_BASE <BASE>])      # generate_export_header's BASE_NAME (default ICLFORGE_<NAME>)
#
# It makes iclforge_<name>_objects (OBJECT), _static and _shared, the aliases iclforge::<name>,
# iclforge::<name>_static and iclforge::<name>_shared (the bare one follows BUILD_SHARED_LIBS), the export
# header iclforge/<name>/export.hpp, the file names iclforge_<name> and iclforge_<name>_static, and the
# properties every hand-written copy set: PIC, hidden visibility, C++23, the warning and coverage
# interface targets. iclforge_install_library(<name>) does the matching install and export rules.
# ---------------------------------------------------------------------------
include_guard(GLOBAL)
include(GenerateExportHeader)

function(iclforge_add_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "EXPORT_BASE;EXPORT_CUSTOM_CONTENT_VAR"
        "SOURCES;DEPENDS;LINK_PRIVATE;PUBLIC_INCLUDES;PRIVATE_INCLUDES")
    string(TOUPPER "${name}" upper)
    if(NOT ARG_EXPORT_BASE)
        set(ARG_EXPORT_BASE "ICLFORGE_${upper}")
    endif()
    set(objects iclforge_${name}_objects)
    set(static iclforge_${name}_static)
    set(shared iclforge_${name}_shared)

    add_library(${objects} OBJECT)
    add_library(${static} STATIC)
    add_library(${shared} SHARED)
    target_link_libraries(${static} PUBLIC ${objects})
    target_link_libraries(${shared} PUBLIC ${objects})

    add_library(iclforge::${name}_static ALIAS ${static})
    add_library(iclforge::${name}_shared ALIAS ${shared})
    if(BUILD_SHARED_LIBS)
        add_library(iclforge::${name} ALIAS ${shared})
    else()
        add_library(iclforge::${name} ALIAS ${static})
    endif()

    target_sources(${objects} PRIVATE ${ARG_SOURCES})
    target_include_directories(${objects}
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
            "$<INSTALL_INTERFACE:include>")
    foreach(dir IN LISTS ARG_PUBLIC_INCLUDES)
        target_include_directories(${objects} PUBLIC "$<BUILD_INTERFACE:${dir}>")
    endforeach()
    if(ARG_PRIVATE_INCLUDES)
        target_include_directories(${objects} PRIVATE ${ARG_PRIVATE_INCLUDES})
    endif()
    target_compile_features(${objects} PUBLIC cxx_std_23)

    # Compile-time use of each dependency on the objects; the wrappers carry the link. Only the
    # compile side: an ordinary link here would put the dependency's shared variant (what the bare
    # alias is under BUILD_SHARED_LIBS) on the link line of the static wrapper too, and a library
    # that embeds the static one, as iclforge::c does, would need it at run time.
    foreach(dep IN LISTS ARG_DEPENDS)
        target_link_libraries(${objects} PRIVATE "$<COMPILE_ONLY:iclforge::${dep}>")
        target_link_libraries(${static} PUBLIC iclforge_${dep}_static)
        target_link_libraries(${shared} PUBLIC iclforge_${dep}_shared)
    endforeach()
    # $<BUILD_INTERFACE:...>: dev-only targets must not enter an installed export set.
    target_link_libraries(${objects} PRIVATE
        "$<BUILD_INTERFACE:iclforge::warnings>"
        "$<BUILD_INTERFACE:iclforge::coverage>"
        ${ARG_LINK_PRIVATE})

    set_target_properties(${objects} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # C4251 fires for private STL members of exported classes; it is a consequence of exporting,
    # and it fires in every translation unit that parses the decorated class.
    target_compile_options(${objects} PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

    set_target_properties(${shared} PROPERTIES DEFINE_SYMBOL "${ARG_EXPORT_BASE}_BUILDING_SHARED")
    set(custom "")
    if(ARG_EXPORT_CUSTOM_CONTENT_VAR)
        set(custom CUSTOM_CONTENT_FROM_VARIABLE ${ARG_EXPORT_CUSTOM_CONTENT_VAR})
    endif()
    generate_export_header(${shared}
        BASE_NAME ${ARG_EXPORT_BASE}
        ${custom}
        EXPORT_MACRO_NAME ${ARG_EXPORT_BASE}_EXPORT
        EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/${name}/export.hpp"
        DEFINE_NO_DEPRECATED
        STATIC_DEFINE ${ARG_EXPORT_BASE}_STATIC_DEFINE)
    target_compile_definitions(${objects} PRIVATE ${ARG_EXPORT_BASE}_BUILDING_SHARED)
    target_compile_definitions(${static} PUBLIC ${ARG_EXPORT_BASE}_STATIC_DEFINE)

    # Pre-1.0 there is no ABI promise between two releases, so the SONAME is the full version.
    set_target_properties(${shared} PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION}"
        OUTPUT_NAME "iclforge_${name}"
        EXPORT_NAME "${name}_shared")
    set_target_properties(${static} PROPERTIES
        OUTPUT_NAME "iclforge_${name}_static"
        EXPORT_NAME "${name}_static")
    set_target_properties(${objects} PROPERTIES
        EXPORT_NAME "${name}_objects"
        ICLFORGE_DEPENDS "${ARG_DEPENDS}")
endfunction()

# The install and export rules of a library iclforge_add_library() made. `ICLFORGE_INSTALL_BOTH_LINKAGES`
# and BUILD_SHARED_LIBS choose which variants are installed, as cmake/InstallLibrary.cmake always did.
# The export set is <name>Targets under the iclforge:: namespace, in the package directory
# find_package(iclforge) reads; the package config includes it when the file exists. The library also
# gets a pkg-config file, iclforge_<name>.pc, that requires the .pc files of the libraries it links: a
# static-only install has to name every archive on a link line, since nothing in an archive records
# what it needs (cmake/PkgConfig.cmake).
function(iclforge_install_library name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DESCRIPTION" "")
    if(ICLFORGE_INSTALL_BOTH_LINKAGES)
        set(targets iclforge_${name}_objects iclforge_${name}_static iclforge_${name}_shared)
    elseif(BUILD_SHARED_LIBS)
        set(targets iclforge_${name}_objects iclforge_${name}_shared)
    else()
        set(targets iclforge_${name}_objects iclforge_${name}_static)
    endif()
    install(TARGETS ${targets}
        EXPORT ${name}Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/${name}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)
    install(FILES "${CMAKE_BINARY_DIR}/src/${name}/generated/iclforge/${name}/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/${name}"
        COMPONENT library)
    install(EXPORT ${name}Targets
        FILE ${name}Targets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)

    iclforge_pkgconfig_libname(pc_libname iclforge_${name}_shared iclforge_${name}
        iclforge_${name}_static "${targets}")
    get_target_property(deps iclforge_${name}_objects ICLFORGE_DEPENDS)
    set(pc_requires "")
    if(deps)
        foreach(dep IN LISTS deps)
            list(APPEND pc_requires iclforge_${dep})
        endforeach()
    endif()
    iclforge_install_pkgconfig(
        NAME iclforge_${name}
        DESCRIPTION "${ARG_DESCRIPTION}"
        LIBNAME "${pc_libname}"
        REQUIRES ${pc_requires})
endfunction()
