# ---------------------------------------------------------------------------
# ac3::admbridge - roadmap item B1 phase 2: maps the ADM object graph
# ac3adm::ac3adm (src/ac3adm) parses onto ac3::oba::AtmosEncoder's input shape.
#
# A NEW standalone module, not folded into either side it bridges, for two
# reasons that are each independently sufficient:
#
#   - src/ac3adm is documentedly codec-blind (see its own CMakeLists.txt/
#     header comments) - it does not, and should not, gain a dependency on
#     ac3::forge/ac3::oba just to know what an "object" or "bed" means in
#     JOC/Atmos terms.
#   - src/forge (-> ac3::forge, ac3::oba::AtmosEncoder) is always built,
#     unconditionally, by every configuration of this project. It cannot
#     gain a dependency on the opt-in, Boost-requiring ac3adm without
#     breaking every default build - the exact reason ac3adm itself defaults
#     AC3FORGE_BUILD_ADM off (see src/ac3adm/CMakeLists.txt's own header).
#
# Gated by the SAME AC3FORGE_BUILD_ADM option ac3adm itself uses, rather than
# a new AC3FORGE_BUILD_ADMBRIDGE of its own: this module is meaningless
# without ac3adm (there is nothing to bridge), and ac3::forge is
# unconditionally available regardless, so a separate option would only ever
# take the same value AC3FORGE_BUILD_ADM already does - one flag, not two
# that must be kept in sync by hand. ROADMAP.md's B2 entry (a future DAMF
# reader) names this module as the "mapping layer" it plans to share, which
# is the other half of why this lives on its own rather than inside either
# neighbour: a future second ADM-graph producer should be able to depend on
# this module without dragging in BW64/RF64-specific parsing it does not
# need.
#
# Installed/exported via find_package(ac3forge) (cmake/InstallLibrary.cmake's AC3FORGE_BUILD_ADM
# block) as ac3::admbridge_shared ONLY - same shared-only deviation as ac3adm::ac3adm itself (see
# src/ac3adm/CMakeLists.txt's header comment), and for a related reason: admbridge_objects PUBLIC-
# links ac3adm::ac3adm below, a genuine usage requirement (this module's own public headers name
# ac3adm types), so admTargets must resolve it - and since only ac3adm_shared is ever exported,
# admbridge has to resolve to that specific variant too, not the ambient ac3adm::ac3adm alias
# (which would pick ac3adm_static whenever BUILD_SHARED_LIBS=OFF, breaking the export the same
# way an unpinned dependency broke ac3adm's own export before the fix described there). See the
# target_link_libraries(admbridge_objects...) call below.
# ---------------------------------------------------------------------------
add_library(admbridge_objects OBJECT)
add_library(admbridge_static STATIC)
add_library(admbridge_shared SHARED)

target_link_libraries(admbridge_static PUBLIC admbridge_objects)
target_link_libraries(admbridge_shared PUBLIC admbridge_objects)

add_library(ac3::admbridge_static ALIAS admbridge_static)
add_library(ac3::admbridge_shared ALIAS admbridge_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3::admbridge ALIAS admbridge_shared)
else()
    add_library(ac3::admbridge ALIAS admbridge_static)
endif()

target_sources(admbridge_objects
    PRIVATE
        src/coordinates.cpp
        src/bridge.cpp)

target_include_directories(admbridge_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(admbridge_objects PUBLIC cxx_std_23)

# PUBLIC: this module's own public headers (bridge.hpp, coordinates.hpp) name types from both
# ac3::oba (ac3::forge) and ac3adm::ac3adm in their signatures (ac3::oba::ObjectPath,
# ac3adm::AdmDocument, ...), so anything linking ac3::admbridge needs both transitively -
# the same PUBLIC-link-its-dependency shape src/signing/CMakeLists.txt uses for ac3::forge.
#
# ac3adm::ac3adm_shared explicitly, not the ambient ac3adm::ac3adm alias: this file's header
# comment explains why - the alias tracks BUILD_SHARED_LIBS and would resolve to ac3adm_static
# whenever that's OFF (the common case, including the coverage preset - the only place
# AC3FORGE_BUILD_ADM=ON is exercised in CI today), which is never in any export set under
# ac3adm's shared-only design. Pinning here mirrors src/capi/CMakeLists.txt's forge_c precedent
# of embedding one fixed variant regardless of the ambient default, just for a PUBLIC dependency
# instead of a PRIVATE one.
target_link_libraries(admbridge_objects
    PUBLIC
        ac3::forge
        ac3adm::ac3adm_shared
    PRIVATE
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")

set_target_properties(admbridge_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(admbridge_shared PROPERTIES DEFINE_SYMBOL "ADMBRIDGE_BUILDING_SHARED")
generate_export_header(admbridge_shared
    BASE_NAME ADMBRIDGE
    EXPORT_MACRO_NAME AC3ADMBRIDGE_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/admbridge/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ADMBRIDGE_STATIC_DEFINE)
target_compile_definitions(admbridge_objects PRIVATE ADMBRIDGE_BUILDING_SHARED)
target_compile_definitions(admbridge_static PUBLIC ADMBRIDGE_STATIC_DEFINE)

set_target_properties(admbridge_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(admbridge_shared PROPERTIES OUTPUT_NAME "admbridge")
set_target_properties(admbridge_static PROPERTIES OUTPUT_NAME "admbridge_static")
