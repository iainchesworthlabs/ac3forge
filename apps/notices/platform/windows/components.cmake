# The Windows package's notices (../../notices.cmake, cmake/Notices.cmake):
# what the zip and the NSIS installer carry, as the sections of NOTICES.txt
# in the order they appear. The Qt sections are dropped by notices.cmake when
# this is a CLI-only build, so nothing here names an option.
set(AC3FORGE_NOTICES_PLATFORM "Windows")
set(AC3FORGE_NOTICES_LOCATION "NOTICES.txt in the folder this was installed or unpacked to, beside LICENSE.txt and the bin/ directory")
set(AC3FORGE_NOTICE_FRAGMENTS header qt-windows windows-runtime fmt fonts trademarks)
