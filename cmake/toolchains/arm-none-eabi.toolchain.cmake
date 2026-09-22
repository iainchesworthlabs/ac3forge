# ---------------------------------------------------------------------------
# arm-none-eabi (bare metal, no OS) - the cross-compilation target roadmap PF7
# names for the minimum-footprint decoder profile, and the one the CI leg runs
# under QEMU.
#
# Cortex-M3 on QEMU's mps2-an385 board: an ARM reference platform QEMU models
# faithfully, with semihosting for stdout and an exit code, and 4 MB of flash
# and RAM. Cortex-M3 specifically, not something larger - it has no FPU at
# all, so every double in this codec is software-emulated and the profile is
# measured on the least forgiving target it plausibly ships to rather than on
# one that flatters it.
#
# Unlike the other toolchain files here this one is NOT chainloaded through
# vcpkg: there is no vcpkg triplet for bare-metal arm, nothing this profile
# builds has a third-party dependency, and the minimal profile turns off every
# component that would want one (see the guard in the root CMakeLists.txt).
# The wasm toolchain sits outside vcpkg for the same reason.
# ---------------------------------------------------------------------------

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Generic/bare-metal: CMake's compiler check links a full executable by
# default, which needs the specs and linker script below to already be in
# effect - a chicken-and-egg it resolves by compiling a static library
# instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(AC3FORGE_ARM_GCC arm-none-eabi-gcc REQUIRED)
find_program(AC3FORGE_ARM_GXX arm-none-eabi-g++ REQUIRED)
set(CMAKE_C_COMPILER "${AC3FORGE_ARM_GCC}")
set(CMAKE_CXX_COMPILER "${AC3FORGE_ARM_GXX}")

# Debian/Ubuntu package newlib's specs and libraries under a prefix GCC does
# not search by default (/usr/lib/arm-none-eabi/newlib), where the ARM-official
# tarball puts them where GCC already looks. Find them rather than assume
# either layout, and only add -B when there is something to add - passing a
# non-existent -B directory is silently ignored by GCC, which would turn a
# packaging difference into a confusing "cannot read spec file" later.
find_file(AC3FORGE_ARM_NANO_SPECS nano.specs
    PATHS /usr/lib/arm-none-eabi/newlib
    NO_DEFAULT_PATH)
if(AC3FORGE_ARM_NANO_SPECS)
    get_filename_component(AC3FORGE_ARM_NEWLIB_DIR "${AC3FORGE_ARM_NANO_SPECS}" DIRECTORY)
    set(_ac3_arm_prefix "-B ${AC3FORGE_ARM_NEWLIB_DIR}")
else()
    set(_ac3_arm_prefix "")
endif()

# -mcpu/-mthumb belong in the FLAGS_INIT rather than in add_compile_options()
# so CMake's own compiler identification and try_compile runs see them too; a
# probe built for the wrong architecture is worse than no probe.
#
# nano.specs is newlib-nano: the small-footprint C library variant, which is
# the only sensible choice for a profile whose entire subject is footprint.
# rdimon.specs is the semihosting host interface - it is what makes printf and
# exit() reach the QEMU console and the process exit code.
set(_ac3_arm_arch "-mcpu=cortex-m3 -mthumb")
set(_ac3_arm_specs "--specs=nano.specs --specs=rdimon.specs")

# Both specs go in the COMPILE flags and nowhere else. They are needed at
# compile time (nano.specs is what puts newlib's headers on the include path)
# and at link time, and CMake's link rule already passes CMAKE_<LANG>_FLAGS to
# the compiler driver it links with - so the link gets them from here. Adding
# them to CMAKE_EXE_LINKER_FLAGS_INIT as well makes GCC read nano.specs twice
# and fail outright: "attempt to rename spec 'link' to already defined spec
# 'nano_link'".
set(CMAKE_C_FLAGS_INIT "${_ac3_arm_prefix} ${_ac3_arm_arch} ${_ac3_arm_specs}")
set(CMAKE_CXX_FLAGS_INIT "${_ac3_arm_prefix} ${_ac3_arm_arch} ${_ac3_arm_specs}")

# Nothing on the host is findable from a bare-metal target, and CMake's
# default ROOT_PATH search would otherwise happily hand this build an x86-64
# library.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Read by apps/baremetal/CMakeLists.txt to decide it has a target to build
# for, and by the root CMakeLists.txt's own reporting. A plain variable rather
# than a compile definition: no source file branches on it - the platform
# split is a directory (apps/baremetal/platform/) as everywhere else here.
set(AC3FORGE_BAREMETAL_TARGET "mps2-an385" CACHE STRING
    "QEMU machine this bare-metal build targets" FORCE)
