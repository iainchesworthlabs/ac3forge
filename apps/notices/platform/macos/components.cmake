# The macOS disk image's notices (../../notices.cmake, cmake/Notices.cmake).
# The .dmg is monolithic (cmake/CPackProjectConfig.cmake forces DragNDrop
# that way), so NOTICES.txt sits at its root, beside LICENSE.txt and
# whatever the build produced - bin/ for ac3cli, ac3gui.app for the window.
# No windows-runtime section: the files that fragment covers are a Windows
# Qt kit's, and macdeployqt places none of them.
#
# Nothing here has been read off a built package. This project has no macOS
# host (docs/building.md, "Verified configuration") and no .dmg has been
# made since this file was written, so what the sections say the image
# carries is what apps/gui/CMakeLists.txt's install rules and Qt's own
# deployment script put there, rather than a listing anyone has taken. The
# location line names only LICENSE.txt because ac3gui.app may not be in the
# image at all: the macos-llvm preset has the GUI off by default
# (CMakePresets.json), so a CLI-only .dmg is the ordinary local case.
set(AC3FORGE_NOTICES_PLATFORM "macOS")
set(AC3FORGE_NOTICES_LOCATION "NOTICES.txt at the root of the disk image, beside LICENSE.txt")
set(AC3FORGE_NOTICE_FRAGMENTS header qt-macos fmt fonts trademarks)
