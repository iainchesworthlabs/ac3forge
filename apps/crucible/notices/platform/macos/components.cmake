# The macOS package's notices (../../notices.cmake, cmake/Notices.cmake):
# what the bundle carries, as the sections of NOTICES.txt in the order they
# appear. The two sections that depend on a build option rather than on the
# platform - qt-quick3d when the kit has Quick 3D, tracy in a profiling build -
# are inserted by notices.cmake, so nothing here names an option.
#
# Written 2026-09-06 with the rest of the macOS platform half. Until this
# component's own CI packaging pass runs green, the section list below is
# still like the rest of that half - compiled, never run: no Mac has read
# the NOTICES.txt this produces, packaged or otherwise. What this file
# originally settled, before cmake/Packaging.cmake's Crucible component
# grew an APPLE arm, was the one thing that would otherwise stop a macOS
# configure dead - notices.cmake FATAL_ERRORs on a platform with no
# components.cmake - and the section list a configure would then assemble.
#
# Three differences from the other two platforms, each following from a fact
# about the build rather than a preference:
#
#   qt-bundled, not qt-system. apps/crucible/CMakeLists.txt runs
#   qt_generate_deploy_qml_app_script under `if(WIN32 OR APPLE)`, so a macOS
#   package carries its own Qt inside the .app the way the Windows zip carries
#   its own beside the .exe; only the Linux package leaves Qt to the system
#   loader. AC3CRUCIBLE_QT_PAYLOAD and AC3CRUCIBLE_QT_LOOKUP below are what
#   make that one shared fragment describe this layout instead of Windows'.
#
#   No pipewire section. That library is Linux's, and this platform links
#   CoreAudio and AppKit, which ship with the OS and ask for no notice.
#
#   No driver section. The MS-PL text belongs to the Windows null-sink driver;
#   macOS needs no silent device at all
#   (engine/platform/macos/virtual_device.cpp), so there is nothing to credit.
set(AC3CRUCIBLE_NOTICES_PLATFORM "macOS")
# Where the notices sit: apps/crucible/CMakeLists.txt's own APPLE install()
# branch puts this file and LICENSE.txt at the archive root, beside the
# bundle, the same DESTINATION "." apps/notices/notices.cmake already used
# for the runtime component's pair beside ac3gui.app. The bundle directory is
# named after the target, ac3crucible.app; MACOSX_BUNDLE_BUNDLE_NAME
# ("Crucible") is the display name and not the path.
set(AC3CRUCIBLE_NOTICES_LOCATION "NOTICES.txt beside ac3crucible.app, next to LICENSE.txt")
set(AC3CRUCIBLE_NOTICE_FRAGMENTS header qt-bundled fmt fonts trademarks)

# The two sentences in the shared qt-bundled fragment that describe where this
# package's Qt actually sits. Windows' own components.cmake supplies the
# wording it had before the fragment was tokenised on 2026-09-06, so that
# file's output is unchanged to the byte.
#
# The embedded newlines are the fragment's line wrapping: a token expands into
# the middle of a line, so each value carries the breaks that keep the
# generated paragraph inside the same margin the rest of NOTICES.txt uses.
set(AC3CRUCIBLE_QT_PAYLOAD
    "ac3crucible loads from inside its application bundle\n(Contents/Frameworks/, Contents/PlugIns/ and\nContents/Resources/)")
set(AC3CRUCIBLE_QT_LOOKUP
    "they are loaded from inside the\napplication bundle, by the install names each Mach-O file records.")
