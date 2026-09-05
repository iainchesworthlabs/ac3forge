# The Windows package's notices (../../notices.cmake, cmake/Notices.cmake):
# what the zip carries, as the sections of NOTICES.txt in the order they
# appear. The two sections that depend on a build option rather than on the
# platform - qt-quick3d when the kit has Quick 3D, tracy in a profiling
# build - are inserted by notices.cmake, so nothing here names an option.
set(AC3CRUCIBLE_NOTICES_PLATFORM "Windows")
set(AC3CRUCIBLE_NOTICES_LOCATION "NOTICES.txt in the folder ac3crucible.exe was unpacked to, beside LICENSE.txt")
set(AC3CRUCIBLE_NOTICE_FRAGMENTS header qt-bundled windows-runtime fmt fonts driver trademarks)
