# The Linux package's notices (../../notices.cmake, cmake/Notices.cmake):
# what the tarball and the .deb carry, as the sections of NOTICES.txt in the
# order they appear. No Qt section for a bundled Qt (the package includes
# none; the loader finds the system's) and no driver section (the silent
# device is a PipeWire node the application makes). The two sections that
# depend on a build option - qt-quick3d, tracy - are inserted by
# notices.cmake, so nothing here names an option.
set(AC3CRUCIBLE_NOTICES_PLATFORM "Linux")
set(AC3CRUCIBLE_NOTICES_LOCATION "/usr/share/doc/ac3forge-crucible/NOTICES.txt (share/doc/ac3forge-crucible/ in the tarball), beside LICENSE.txt")
set(AC3CRUCIBLE_NOTICE_FRAGMENTS header qt-system pipewire fmt fonts trademarks)
