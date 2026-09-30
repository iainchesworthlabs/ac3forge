# A standalone project, not part of iclforge's own build: tools/checks/check_install_consumer.sh
# configures it against an INSTALLED package (-DCMAKE_PREFIX_PATH=<prefix>), the way a downstream
# project finds one. consumer.c's header comment says what it checks and why it needs to be
# built this way.
#
# One consumer per C API target the package exports: iclforge::c_shared and iclforge::c_static
# when it was installed with ICLFORGE_INSTALL_BOTH_LINKAGES=ON, one of them otherwise, plus the
# bare iclforge::c that iclforgeConfig.cmake defines only for a single-linkage package (the name
# docs/library/c-api.md tells a consumer to link). Every one is built, linked and run.
#
# CXX is enabled beside C for the static target's sake. A static C API library is made of C++
# objects, and its exported target says so (IMPORTED_LINK_INTERFACE_LANGUAGES CXX), but CMake acts
# on that only for a language the consuming project has enabled. With LANGUAGES C alone the link
# goes through the C driver and stops at the C++ runtime, hundreds of undefined references with
# `operator new` among them. With CXX enabled the static consumer is linked by the C++ driver,
# which brings the C++ runtime and libm; consumer.c names neither. A shared target's consumer is
# pinned to the C driver, which is what a C-only project gets: the shared library carries the C++
# runtime it needs, and the driver would otherwise be chosen by whatever else lands on the link line.
#
# One C++ consumer per AC-4 decoder target the package exports, in the same way: iclforge::ac4dec_shared
# and iclforge::ac4dec_static, or the bare iclforge::ac4dec of a single-linkage package. consumer_ac4.cpp
# decodes a committed stream through the installed inspector and decoder. And one per AC-4 encoder
# target: consumer_ac4enc.cpp encodes a tone through the installed encoder and reads the stream
# back with the inspector. A package built with ICLFORGE_BUILD_AC4=OFF has neither;
# check_install_consumer.sh passes ICLFORGE_EXPECT_AC4 from the tree's own cache, so that a package
# that should have them and does not fails here.
cmake_minimum_required(VERSION 3.28)
project(iclforge_install_consumer LANGUAGES C CXX)

enable_testing()

find_package(iclforge CONFIG REQUIRED)

set(consumers)
foreach(target IN ITEMS iclforge::c_shared iclforge::c_static iclforge::c)
    if(NOT TARGET ${target})
        continue()
    endif()
    string(REPLACE "::" "_" name "consumer_${target}")
    add_executable(${name} consumer.c)
    add_test(NAME ${name} COMMAND ${name})
    target_link_libraries(${name} PRIVATE ${target})
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
        set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
    endif()
    list(APPEND consumers ${target})
endforeach()

if(NOT consumers)
    message(FATAL_ERROR
        "the installed package exports none of iclforge::c_shared, iclforge::c_static or "
        "iclforge::c - was it configured with ICLFORGE_BUILD_CAPI=OFF?")
endif()
message(STATUS "C API consumers of the installed package, each linked and run: ${consumers}")

option(ICLFORGE_EXPECT_AC4 "The package was built with ICLFORGE_BUILD_AC4=ON" OFF)
cmake_path(ABSOLUTE_PATH CMAKE_CURRENT_SOURCE_DIR NORMALIZE OUTPUT_VARIABLE here)
cmake_path(APPEND here .. .. .. tests golden external-baseline ac4-51-film-96 dee.ac4
    OUTPUT_VARIABLE ac4_stream)
cmake_path(NORMAL_PATH ac4_stream)

set(ac4_consumers)
foreach(target IN ITEMS iclforge::ac4dec_shared iclforge::ac4dec_static iclforge::ac4dec)
    if(NOT TARGET ${target})
        continue()
    endif()
    string(REPLACE "::" "_" name "consumer_${target}")
    add_executable(${name} consumer_ac4.cpp)
    add_test(NAME ${name} COMMAND ${name} "${ac4_stream}")
    target_link_libraries(${name} PRIVATE ${target})
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    list(APPEND ac4_consumers ${target})
endforeach()

if(ICLFORGE_EXPECT_AC4 AND NOT ac4_consumers)
    message(FATAL_ERROR
        "the package was built with ICLFORGE_BUILD_AC4=ON and exports none of "
        "iclforge::ac4dec_shared, iclforge::ac4dec_static or iclforge::ac4dec - see cmake/InstallLibrary.cmake")
endif()
if(ac4_consumers)
    message(STATUS "AC-4 consumers of the installed package, each linked and run: ${ac4_consumers}")
endif()

set(ac4enc_consumers)
foreach(target IN ITEMS iclforge::ac4enc_shared iclforge::ac4enc_static iclforge::ac4enc)
    if(NOT TARGET ${target})
        continue()
    endif()
    string(REPLACE "::" "_" name "consumer_${target}")
    add_executable(${name} consumer_ac4enc.cpp)
    add_test(NAME ${name} COMMAND ${name})
    target_link_libraries(${name} PRIVATE ${target})
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    list(APPEND ac4enc_consumers ${target})
endforeach()

if(ICLFORGE_EXPECT_AC4 AND NOT ac4enc_consumers)
    message(FATAL_ERROR
        "the package was built with ICLFORGE_BUILD_AC4=ON and exports none of "
        "iclforge::ac4enc_shared, iclforge::ac4enc_static or iclforge::ac4enc - see cmake/InstallLibrary.cmake")
endif()
if(ac4enc_consumers)
    message(STATUS
        "AC-4 encoder consumers of the installed package, each linked and run: ${ac4enc_consumers}")
endif()
