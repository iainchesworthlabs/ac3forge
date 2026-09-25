# A standalone project, not part of ac3forge's own build: tools/checks/check_install_consumer.sh
# configures it against an INSTALLED package (-DCMAKE_PREFIX_PATH=<prefix>), the way a downstream
# project finds one. consumer.c's header comment says what it checks and why it needs to be
# built this way.
#
# One consumer per C API target the package exports: ac3::forge_c_shared and ac3::forge_c_static
# when it was installed with AC3FORGE_INSTALL_BOTH_LINKAGES=ON, one of them otherwise, plus the
# bare ac3::forge_c that ac3forgeConfig.cmake defines only for a single-linkage package (the name
# docs/library/c-api.md tells a consumer to link).
#
# A shared target is linked and run. A static one is compiled only: the installed static archives
# are not self-contained (they leave the C++ runtime, libm and {fmt} to the consumer, and the
# package config names none of them), so linking one from a C project fails for reasons that
# have nothing to do with what this project checks. Compiling still covers the installed
# headers, the include path and AC3FORGEC_STATIC_DEFINE that target carries.
cmake_minimum_required(VERSION 3.28)
project(ac3forge_install_consumer LANGUAGES C)

enable_testing()

find_package(ac3forge CONFIG REQUIRED)

set(consumers)
foreach(target IN ITEMS ac3::forge_c_shared ac3::forge_c_static ac3::forge_c)
    if(NOT TARGET ${target})
        continue()
    endif()
    string(REPLACE "::" "_" name "consumer_${target}")
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "STATIC_LIBRARY")
        add_library(${name} OBJECT consumer.c)
        list(APPEND consumers "${target} (compiled)")
    else()
        add_executable(${name} consumer.c)
        add_test(NAME ${name} COMMAND ${name})
        list(APPEND consumers "${target} (linked and run)")
    endif()
    target_link_libraries(${name} PRIVATE ${target})
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
endforeach()

if(NOT consumers)
    message(FATAL_ERROR
        "the installed package exports none of ac3::forge_c_shared, ac3::forge_c_static or "
        "ac3::forge_c - was it configured with AC3FORGE_BUILD_CAPI=OFF?")
endif()
message(STATUS "C API consumers of the installed package: ${consumers}")
