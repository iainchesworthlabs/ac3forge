# The Windows package's notices (../../notices.cmake, cmake/Notices.cmake):
# what the zip carries, as the sections of NOTICES.txt in the order they
# appear. The two sections that depend on a build option rather than on the
# platform - qt-quick3d when the kit has Quick 3D, tracy in a profiling
# build - are inserted by notices.cmake, so nothing here names an option.
set(AC3CRUCIBLE_NOTICES_PLATFORM "Windows")
set(AC3CRUCIBLE_NOTICES_LOCATION "NOTICES.txt in the folder ac3crucible.exe was unpacked to, beside LICENSE.txt")
set(AC3CRUCIBLE_NOTICE_FRAGMENTS header qt-bundled windows-runtime fmt fonts driver trademarks)

# Where this package's Qt sits, and how the loader finds it. Both sentences
# were literal text inside fragments/qt-bundled.txt until 2026-09-06, when
# macOS became a third platform that bundles Qt and needed the same fragment
# to describe an .app bundle instead of a folder of DLLs. The two values below
# are that text, unchanged and with its own line wrapping, so this platform's
# NOTICES.txt is byte-for-byte what it was.
set(AC3CRUCIBLE_QT_PAYLOAD
    "ac3crucible.exe loads (bin/Qt6*.dll, plugins/, qml/\nand translations/)")
set(AC3CRUCIBLE_QT_LOOKUP
    "they are looked up by name from the directories\nbin/qt.conf points at.")
