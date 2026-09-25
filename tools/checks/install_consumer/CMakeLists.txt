# A standalone project, not part of ac3forge's own build: tools/checks/check_install_consumer.sh
# configures it against an INSTALLED package (-DCMAKE_PREFIX_PATH=<prefix>), the way a downstream
# project finds one. consumer.c's header comment says what it checks and why it needs to be
# built this way.
#
# One consumer per C API target the package exports: ac3::forge_c_shared and ac3::forge_c_static
# when it was installed with AC3FORGE_INSTALL_BOTH_LINKAGES=ON, one of them otherwise, plus the
# bare ac3::forge_c that ac3forgeConfig.cmake defines only for a single-linkage package (the name
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
cmake_minimum_required(VERSION 3.28)
project(ac3forge_install_consumer LANGUAGES C CXX)

enable_testing()

find_package(ac3forge CONFIG REQUIRED)

set(consumers)
foreach(target IN ITEMS ac3::forge_c_shared ac3::forge_c_static ac3::forge_c)
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
        "the installed package exports none of ac3::forge_c_shared, ac3::forge_c_static or "
        "ac3::forge_c - was it configured with AC3FORGE_BUILD_CAPI=OFF?")
endif()
message(STATUS "C API consumers of the installed package, each linked and run: ${consumers}")
