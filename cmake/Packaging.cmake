# ---------------------------------------------------------------------------
# CPack packaging. Included once, from the top-level CMakeLists.txt, after
# every target's install() rules have been declared.
#
# A plain ZIP archive is always offered (needs no external tool). Platform-
# native formats are layered on top when the packaging tool for that format is
# actually available, so `cpack` degrades gracefully instead of failing
# outright. Which targets end up in a package is decided entirely by which
# install() rules ran - ac3cli's runs unconditionally (AC3FORGE_BUILD_CLI
# defaults ON), ac3gui's only when AC3FORGE_BUILD_GUI is ON - so no extra
# gating is needed here for that.
#
# CMakePresets.json's packagePresets deliberately carry no "generators"
# field: `cpack --preset` passes that field to cpack as -G on the command
# line, which OVERRIDES the CPACK_GENERATOR list computed below - confirmed
# empirically, a preset naming NSIS made cpack hard-fail with "Cannot find
# NSIS compiler makensis" even with the find_program() gate below correctly
# leaving NSIS out of CPACK_GENERATOR because makensis was not on PATH.
# Omitting it lets CPack fall back to CPACK_GENERATOR from here instead, so
# the graceful degradation this file computes actually takes effect through
# `cpack --preset` and not only through a bare `cpack` invocation.
# ---------------------------------------------------------------------------

set(CPACK_PACKAGE_NAME "ac3forge")
set(CPACK_PACKAGE_VENDOR "Iain Chesworth")
set(CPACK_PACKAGE_CONTACT "Iain Chesworth")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ac3forge")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_VERBATIM_VARIABLES ON)

# A <package>.sha512 file beside every package, so a release can publish a
# checksum without a separate sha512sum pass over the packages/ directory.
set(CPACK_PACKAGE_CHECKSUM "SHA512")

set(CPACK_GENERATOR "ZIP")

# Per-generator override, sourced by cpack itself once per generator in a
# multi-generator run - see cmake/CPackProjectConfig.cmake for why DragNDrop
# needs one (CPACK_COMPONENTS_GROUPING below is global CPack state, and
# DragNDrop reads the same value the archive generators use to split).
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CPackProjectConfig.cmake")

if(WIN32)
    find_program(AC3FORGE_MAKENSIS_EXECUTABLE makensis)
    if(AC3FORGE_MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_PACKAGE_NAME "${CPACK_PACKAGE_NAME}")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        # Same source ac3gui's own .rc uses (apps/gui/CMakeLists.txt) - the
        # installer/uninstaller windows and shortcut both otherwise default
        # to NSIS's own generic icon. NSIS wants a Windows .ico specifically
        # for both variables, which generate_icons.py already produces.
        set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/apps/gui/icons/ac3forge.ico")
        set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/apps/gui/icons/ac3forge.ico")

        # Start Menu entries. Until this, the installer laid ac3cli.exe and
        # ac3gui.exe down under $INSTDIR\bin and created nothing anywhere a
        # user looks - the Start Menu folder CPack always makes held nothing
        # but the Uninstall shortcut, so an installed copy was reachable only
        # by browsing to the folder it went into. The Linux .deb has had a
        # menu entry since roadmap UX2 (apps/gui/packaging/linux/ac3gui.desktop);
        # Windows had never been given the same thing.
        #
        # CPACK_PACKAGE_EXECUTABLES is the shape CPack's NSIS generator wants:
        # a flat list of <executable-name-without-.exe>;<menu label> pairs,
        # from which it writes both the CreateShortCut lines in the installer
        # and the matching Delete lines in the uninstaller - so a shortcut
        # cannot be created here and then left behind on uninstall, which is
        # the failure mode of writing the CreateShortCut by hand. It resolves
        # each name as $INSTDIR\<CPACK_NSIS_EXECUTABLES_DIRECTORY>\<name>.exe,
        # and that variable's default is "bin" - the same "bin" GNUInstallDirs
        # gives CMAKE_INSTALL_BINDIR on Windows and the same one both
        # applications' install(TARGETS ... RUNTIME DESTINATION) use, so the
        # default is correct here rather than merely untouched.
        #
        # The label is "ac3gui" and not a product name because that is what
        # the Linux launcher's Name= already says: the two menus name the same
        # application and should not disagree, and choosing a new published
        # name for it is not this file's decision to take
        # (docs/family/recasting.md).
        #
        # ac3gui alone in that list, because it is the only windowed
        # application the installer carries (the Crucible is kept out of this
        # installer entirely - cmake/CPackProjectConfig.cmake says why). A
        # .lnk straight to ac3cli.exe would open a console, print the usage
        # text and close it again before anyone could read a line of it, so
        # the console tool gets the entry it can actually use instead: a
        # command prompt that already has $INSTDIR\bin on PATH. That has no
        # CPack variable of its own, hence raw NSIS through
        # CPACK_NSIS_CREATE_ICONS_EXTRA - which is injected inside the block
        # where $STARTMENU_FOLDER is in scope, unlike
        # CPACK_NSIS_EXTRA_INSTALL_COMMANDS below - and its uninstall half in
        # CPACK_NSIS_DELETE_ICONS_EXTRA, which runs in the uninstaller where
        # the same folder is $MUI_TEMP instead. The two name the same .lnk and
        # are one change; editing either alone leaves a shortcut behind.
        #
        # SetOutPath is there because NSIS gives a .lnk the CURRENT output
        # path as its working directory: without the first line the prompt
        # would open in $INSTDIR rather than beside the binaries. The second
        # puts it back for the shortcut CPack writes immediately after this
        # hook - CMake's own NSIS.template.in orders the core section
        # @CPACK_NSIS_CREATE_ICONS@, this, then `CreateShortCut ...
        # Uninstall.lnk` - so the uninstaller entry keeps the $INSTDIR working
        # directory every other CPack installer gives it. Nothing else in that
        # section is affected either way: @CPACK_NSIS_FULL_INSTALL@ lays the
        # files down at the top of it, long before this runs. Bracket
        # arguments for the same reason the file-association block below uses
        # them - the NSIS command syntax needs both quote kinds nested, and
        # escaping that through CMake's quoting rules is where this sort of
        # thing goes wrong.
        #
        # Deliberately not CPACK_CREATE_DESKTOP_LINKS: the defect is that the
        # applications are not findable, and a desktop icon nobody asked for
        # is a different decision from a Start Menu entry.
        if(TARGET ac3gui)
            set(CPACK_PACKAGE_EXECUTABLES "ac3gui" "ac3gui")
        endif()
        if(TARGET ac3cli)
            set(CPACK_NSIS_CREATE_ICONS_EXTRA [[
            SetOutPath "$INSTDIR\bin"
            CreateShortCut "$SMPROGRAMS\$STARTMENU_FOLDER\ac3cli command prompt.lnk" "$SYSDIR\cmd.exe" '/K "set PATH=$INSTDIR\bin;%PATH%"' "$INSTDIR\bin\ac3cli.exe" 0
            SetOutPath "$INSTDIR"
            ]])
            set(CPACK_NSIS_DELETE_ICONS_EXTRA [[
            Delete "$SMPROGRAMS\$MUI_TEMP\ac3cli command prompt.lnk"
            ]])
        endif()

        # Roadmap UX2: .ac3/.ec3 open in ac3gui - the same "double-click a
        # stream you already have" gesture the app's own DropArea and
        # `ac3gui <file>` launch handling (roadmap UX2's other two legs)
        # already understand once the file reaches the app; this is what
        # gets it there from Explorer. One ProgID for both extensions - they
        # are the same stream format (bsid decides AC-3 vs E-AC-3, the same
        # way every ac3gui/ac3cli command that takes either already does),
        # so a single "open in ac3gui" entry is the honest description
        # rather than two identical ones. $INSTDIR\bin matches
        # CMAKE_INSTALL_BINDIR, where apps/gui/CMakeLists.txt's own
        # install(TARGETS ac3gui RUNTIME DESTINATION ...) puts it.
        # SHChangeNotify is what makes Explorer pick the new association up
        # without a logoff/logon - without it the icon/"Open with" entry
        # only appears after one. Bracket arguments (CMake's raw-string
        # syntax) rather than a quoted string: NSIS's own command syntax
        # already needs both single and double quotes (nested, so an
        # "open" command's value can itself be double-quoted), and escaping
        # all of that through CMake's quoted-argument rules would be far
        # more error-prone than writing the NSIS script exactly as NSIS
        # wants it.
        set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [[
            WriteRegStr HKCR ".ac3" "" "AC3Forge.Stream"
            WriteRegStr HKCR ".ec3" "" "AC3Forge.Stream"
            WriteRegStr HKCR "AC3Forge.Stream" "" "AC-3 / E-AC-3 Stream"
            WriteRegStr HKCR "AC3Forge.Stream\DefaultIcon" "" "$INSTDIR\bin\ac3gui.exe,0"
            WriteRegStr HKCR "AC3Forge.Stream\shell\open\command" "" '"$INSTDIR\bin\ac3gui.exe" "%1"'
            System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
        ]])
        set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS [[
            DeleteRegKey HKCR ".ac3"
            DeleteRegKey HKCR ".ec3"
            DeleteRegKey HKCR "AC3Forge.Stream"
            System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
        ]])
    else()
        # DR7: this used to be silent - a missing makensis just meant the ZIP
        # packaged alone with no diagnostic anywhere, which is how the
        # Windows release shipped installer-less for several releases running
        # before anyone noticed (see docs/releasing.md#winget-manifest and
        # ROADMAP.md's DR7). CI now installs makensis explicitly
        # (.github/workflows/_build.yml's "Install NSIS (Windows)" step) and
        # asserts packages/*.exe exists after Package, so this warning firing
        # THERE means that install broke and the leg fails outright; degrading
        # to a ZIP-only package on purpose - with a visible reason why - is
        # still the right call for a local dev build without NSIS installed.
        message(WARNING "makensis not found on PATH - packaging a ZIP only, "
            "no NSIS installer. Install NSIS (https://nsis.sourceforge.io/) "
            "or `choco install nsis` to get one locally.")
    endif()
elseif(APPLE)
    list(APPEND CPACK_GENERATOR "DragNDrop")
elseif(UNIX)
    list(APPEND CPACK_GENERATOR "TGZ")

    find_program(AC3FORGE_DPKG_DEB_EXECUTABLE dpkg-deb)
    if(AC3FORGE_DPKG_DEB_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_VENDOR}")
        set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

        # The QML modules ac3gui needs, declared by hand, because nothing
        # automatic can find them. Two separate gaps, and only the first is
        # the one usually talked about:
        #
        #   dpkg-shlibdeps reads a binary's DT_NEEDED entries and asks dpkg
        #   which *installed apt package* owns each .so. It resolves the Qt
        #   LIBRARIES that way when the kit is the distribution's own (the
        #   CI legs install qt6-base-dev/qt6-declarative-dev, so it does),
        #   and silently drops anything it cannot map - which is what
        #   happens with a private prebuilt kit from aqtinstall or a
        #   relocated archive. CountdownSolver hit that case in its own
        #   packaging (the comment in R:\CountdownSolver\cmake\Packaging.cmake)
        #   and works around it exactly this way.
        #
        #   A QML import is invisible to shlibdeps in EVERY case, distro kit
        #   or not. `import QtQuick.Controls` is resolved at run time by the
        #   QML engine walking its import paths for a qmldir and a plugin;
        #   none of that reaches the executable's ELF headers, so no
        #   library-level scan can ever see it. Debian and Ubuntu split
        #   those modules into one qml6-module-* package each, so a .deb
        #   without them installs cleanly and then dies at the first import
        #   - which is what the released .deb has been doing.
        #
        # The list is ac3gui's own imports, read off apps/gui/qml/*.qml, not
        # copied from the Crucible pass in .github/workflows/_build.yml: the
        # two windows import different things. ac3gui imports QtQuick,
        # QtQuick.Controls, QtQuick.Dialogs, QtQuick.Layouts, QtQuick.Window
        # and QtCore (Main.qml's Settings). Crucible additionally imports
        # QtQuick.Effects, Qt.labs.platform and QtQuick3D and none of those
        # belong here; ac3crucible is its own component with its own Depends
        # further down. Four entries are named that no .qml file imports:
        #   qml6-module-qtquick-templates       what QtQuick.Controls is
        #                                       implemented on top of
        #   qml6-module-qtqml-models            named in QtQuick's own qmldir;
        #   qml6-module-qtqml-workerscript      the first is where the delegate
        #                                       model behind this window's
        #                                       ListView and its Repeaters
        #                                       comes from
        #   qml6-module-qt-labs-folderlistmodel QtQuick.Dialogs' non-native
        #                                       FileDialog/FolderDialog
        #                                       fallback, which is what runs
        #                                       where no XDG portal answers
        # Each of those is already pulled in by the package above it on
        # Ubuntu 26.04, so naming them changes nothing there. They are named
        # because this package should state what it needs rather than
        # inherit it from another package's Depends field, which that
        # package is free to change.
        #
        # Only when the GUI is in the package. A CLI-only .deb
        # (AC3FORGE_BUILD_GUI=OFF, still the default on every Linux preset in
        # CMakePresets.json) links no Qt at all, and pulling the whole QML
        # runtime onto a machine that asked for ac3cli would be a regression.
        # The Qt libraries themselves stay with shlibdeps, which resolves
        # them from an apt kit; a private kit needs them added here too, and
        # the first gap above is the reason why.
        if(AC3FORGE_BUILD_GUI)
            set(AC3FORGE_GUI_QML_MODULES
                qml6-module-qtcore
                qml6-module-qtqml-models
                qml6-module-qtqml-workerscript
                qml6-module-qtquick
                qml6-module-qtquick-controls
                qml6-module-qtquick-dialogs
                qml6-module-qtquick-layouts
                qml6-module-qtquick-templates
                qml6-module-qtquick-window
                qml6-module-qt-labs-folderlistmodel)
            # Debian wants one comma-separated field and a CMake list is
            # semicolon-separated, so the join happens here rather than being
            # left to CPack - CPACK_VERBATIM_VARIABLES (top of this file)
            # passes the value through exactly as written. No version floors:
            # every one of these arrived with Qt 6 itself, and the Qt version
            # floor that does matter is already carried by the library
            # dependencies shlibdeps writes.
            list(JOIN AC3FORGE_GUI_QML_MODULES ", " CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS)
        endif()

        # Component-aware packaging, OFF by default for the DEB generator -
        # without this, CPack ignores CPACK_COMPONENTS_ALL/GROUP entirely and
        # bundles every install()'d file (ac3cli AND the full library SDK)
        # into one monolithic .deb, confirmed empirically against a real
        # `dpkg-deb -c` of this project's own pre-split output. Turning it on
        # is what makes runtime/library/libruntime become three independent
        # .deb files instead. CPACK_COMPONENTS_GROUPING's file-level default
        # (below) merges library+libruntime into one "dev" archive for ZIP/
        # TGZ - cmake/CPackProjectConfig.cmake overrides that back to IGNORE
        # for exactly the DEB/RPM passes, so those two stay three separate
        # packages instead of collapsing to the archives' two.
        set(CPACK_DEB_COMPONENT_INSTALL ON)

        # Package-name overrides: without these, CPack derives
        # <name>-<component> for every component once component install is
        # on (e.g. "ac3forge-runtime"), which both renames today's existing
        # ac3cli package and ignores Debian's own libFOO/libFOO-dev naming
        # convention for the library halves.
        set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "ac3forge")
        set(CPACK_DEBIAN_LIBRUNTIME_PACKAGE_NAME "libac3forge0")
        set(CPACK_DEBIAN_LIBRARY_PACKAGE_NAME "libac3forge-dev")
        # AC3Forge Crucible (roadmap UX12): its own package, since it is its
        # own download everywhere else. shlibdeps resolves libpipewire-0.3 and
        # the Qt runtime from the binary; what it cannot see is that the
        # application needs the PipeWire *daemon* and a session manager
        # running, which is a Depends on the service packages, not a library.
        # No ALSA dependency, on purpose: apps/crucible/CMakeLists.txt refuses
        # to build against the ALSA backend at all.
        set(CPACK_DEBIAN_CRUCIBLE_PACKAGE_NAME "ac3forge-crucible")
        # Named for what it is, the same reasoning as the archive override
        # further down: without this the file is
        # ac3forge-<version>-<system>-crucible.deb, the base name with the
        # component appended, and nothing in it says "Crucible" until dpkg
        # is asked. DEB-DEFAULT is dpkg's own <name>_<version>_<arch>.deb.
        set(CPACK_DEBIAN_CRUCIBLE_FILE_NAME DEB-DEFAULT)
        set(CPACK_DEBIAN_CRUCIBLE_PACKAGE_SECTION "sound")
        set(CPACK_DEBIAN_CRUCIBLE_PACKAGE_DEPENDS "pipewire, wireplumber | pipewire-media-session")
        # The extended Debian description for this component. The synopsis -
        # the first line, what `apt show` headlines - is the project-wide
        # CPACK_PACKAGE_DESCRIPTION_SUMMARY on every component's package, and
        # this CPack offers no per-component override of it that took effect
        # when tried (both CPACK_COMPONENT_<C>_DESCRIPTION and this variable
        # feed only the indented part). So the crucible .deb headlines as the
        # library and says what it is on the next line. Cosmetic, and noted in
        # docs/releasing.md rather than hidden.
        set(CPACK_DEBIAN_CRUCIBLE_DESCRIPTION
            "AC3Forge Crucible - your applications, placed in a live Dolby Atmos room
 Every application making sound on the desk becomes an object in a Dolby Atmos
 scene: drag each to a place in the room and the result streams to a receiver
 as E-AC-3 JOC, or as Dolby Digital, multichannel PCM or stereo, following the
 hardware. Needs a running PipeWire session; the silent device applications
 play into is a PipeWire node Crucible creates while it runs.")

        # The -dev package's headers/static-archives are useless without a
        # matching runtime .so to actually link and load - and since this
        # project makes no ABI-compatibility promise pre-1.0 (see
        # src/forge/CMakeLists.txt's SOVERSION comment), the pin has to be
        # exact, not a >= floor. libac3forge0 itself declares no such
        # dependency the other way: it is a plain .so with no headers or
        # symlink of its own, valid to have installed alone.
        # PROJECT_VERSION, not CPACK_PACKAGE_VERSION: the latter is only
        # computed by include(CPack) itself, further down this file - read
        # here, before that point, it is still unset and silently renders
        # this Depends line as "libac3forge0 (= )" with no version at all
        # (confirmed empirically against a real dpkg-deb -I). See
        # CPACK_SYSTEM_NAME's identical trap, documented below.
        set(CPACK_DEBIAN_LIBRARY_PACKAGE_DEPENDS "libac3forge0 (= ${PROJECT_VERSION})")
    endif()

    find_program(AC3FORGE_RPMBUILD_EXECUTABLE rpmbuild)
    if(AC3FORGE_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
        set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
        set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)

        # Same reasoning and the same three-way split as the DEB block above,
        # RPM's own equivalent switch and per-component variable names.
        # "-devel" rather than "-dev": Fedora/RHEL/openSUSE package-naming
        # convention for a development package, where Debian/Ubuntu use "-dev".
        set(CPACK_RPM_COMPONENT_INSTALL ON)
        set(CPACK_RPM_RUNTIME_PACKAGE_NAME "ac3forge")
        # No QML Requires here, unlike the DEB block above, and that is the
        # one asymmetry between the two worth knowing. The RPM distributions
        # this generator targets do not split the QML modules out: Fedora and
        # RHEL ship QtQuick, Quick Controls, Dialogs, Layouts and the QtCore
        # QML module inside qt6-qtdeclarative, the same package that owns the
        # libQt6Qml.so.6/libQt6Quick.so.6 that ac3gui links - so
        # CPACK_RPM_PACKAGE_AUTOREQPROV's soname scan already pulls every one
        # of them in, and Debian's per-module qml6-module-* split is what
        # makes the .deb need a list by hand. Reasoned from the two
        # distributions' package layouts rather than measured: this project
        # has no RPM host, and `rpm -qp --requires` on a built package is
        # what would confirm it.
        #
        # The Crucible RPM is a local-only product, and deliberately so - the
        # three settings below produce one for `cpack` on a developer's
        # machine and never in CI. Written down here because the reason is not
        # what it looks like from this file, and an audit reading only this
        # block reasonably concluded the RPM generator was missing a tool:
        #
        #   - rpmbuild is NOT absent from the Linux image. .github/workflows/
        #     _build.yml's "Bootstrap container" step installs the `rpm`
        #     package, which is where rpmbuild comes from on Debian/Ubuntu,
        #     and the Package leg's upload allowlist already collects
        #     packages/*.rpm - the library and runtime components' RPMs are
        #     built and attached to releases today.
        #   - What excludes the Crucible is the Crucible pass's own cpack
        #     call, which names its generators on the command line:
        #     `cpack -D CPACK_COMPONENTS_ALL=crucible -G "TGZ;DEB"`. A -G on
        #     the command line overrides CPACK_GENERATOR computed here (the
        #     same override this file's header documents for packagePresets),
        #     so the RPM generator never runs in that pass however available
        #     rpmbuild is.
        #   - And the legs that DO run a full `cpack --preset pack-linux-*`,
        #     where CPACK_GENERATOR from this file applies in full, configure
        #     their build tree without -DAC3FORGE_BUILD_CRUCIBLE=ON, so
        #     `crucible` is not in CPACK_COMPONENTS_ALL there at all (see the
        #     list(APPEND) further down).
        #
        # Nothing here is worth "fixing" by adding RPM to that pass: a .deb
        # exercises the same component install and the same install rules, the
        # runner is Debian-derived so only the .deb is installable on it, and
        # there is no RPM host in this project to test the result on - the
        # same gap CPACK_RPM_PACKAGE_AUTOREQPROV's reasoning above already
        # names. docs/releasing.md tells a release manager the same thing in
        # the reader's own words.
        set(CPACK_RPM_CRUCIBLE_PACKAGE_NAME "ac3forge-crucible")
        set(CPACK_RPM_CRUCIBLE_FILE_NAME RPM-DEFAULT)
        set(CPACK_RPM_CRUCIBLE_PACKAGE_REQUIRES "pipewire, wireplumber")
        set(CPACK_RPM_LIBRUNTIME_PACKAGE_NAME "libac3forge0")
        set(CPACK_RPM_LIBRARY_PACKAGE_NAME "ac3forge-devel")
        set(CPACK_RPM_LIBRARY_PACKAGE_REQUIRES "libac3forge0 = %{version}-%{release}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Library component(s): a second, separate download alongside the existing
# ac3cli/ac3gui package - headers + .lib/.dll/.a/.so + CMake package config
# for a third party consuming ac3::forge/matroska::matroska via
# find_package(ac3forge) (see cmake/InstallLibrary.cmake). Everything
# install()'d without an explicit COMPONENT falls into CPack's own
# "Unspecified" component, which is why ac3cli/ac3gui and every
# InstallLibrary.cmake rule now carry one explicitly.
#
# Three components, not two: "runtime" (ac3cli/ac3gui, unchanged), "library"
# (headers, static archives, CMake package config, and - on Unix - the
# unversioned .so namelink symlink you link against), and "libruntime" (just
# the versioned .so/.dylib a linked binary loads at runtime - see
# cmake/InstallLibrary.cmake's NAMELINK_COMPONENT comment for why that file
# alone is split out). library+libruntime are DELIBERATELY kept as one
# archive download below (a "-dev" ZIP/TGZ downloader wants both without
# knowing this split exists) but as three separate DEB/RPM packages
# (cmake/CPackProjectConfig.cmake overrides the grouping back to IGNORE for
# just those two generators) - that split is the entire point of shipping
# them as .deb/.rpm at all: apt/dnf can then pull in "the .so a linked binary
# needs" via libac3forge0 without the headers/static archives libac3forge-dev
# carries, the same libFOO/libFOO-dev shape every other Linux C library uses.
#
# CPACK_ARCHIVE_COMPONENT_INSTALL is specifically the Archive generator
# family's (ZIP/TGZ) own component-install switch - it does not affect
# NSIS/DragNDrop, each of which has its own separate
# CPACK_<GENERATOR>_COMPONENT_INSTALL flag, left off here deliberately:
#   - NSIS: a component installer can't also produce a second standalone
#     download the way a second archive naturally can - splitting it would
#     need an entirely different NSIS packaging shape, not a flag flip.
#   - DragNDrop: no macOS host to build or verify this against at all (see
#     the DragNDrop branch above); cmake/CPackProjectConfig.cmake already
#     forces it monolithic regardless of the component/grouping state here.
# DEB/RPM get their own *_COMPONENT_INSTALL switch, set inside their own
# find_program() blocks above, now that the split is real work rather than
# a placeholder.
# The `runtime` component is ac3cli/ac3gui plus, since roadmap IO8, the
# generated ac3cli.1 man page and the bash/zsh/fish/PowerShell completion
# scripts - all install()'d with COMPONENT runtime from
# apps/cli/CMakeLists.txt, so every generator below picks them up with the
# binary rather than needing a component of their own. They are absent from a
# CROSS build's packages by construction: they are produced by running the
# freshly built ac3cli, which a cross build cannot do (see that file's own
# CMAKE_CROSSCOMPILING branch for why that is the chosen trade).
set(CPACK_COMPONENTS_ALL runtime library libruntime)

# The Windows AC3Forge Crucible (roadmap UX11) as a fourth component, and so
# its own archive rather than part of the runtime one: it is Windows-only, it
# carries a second Qt deployment of its own, and its null-sink driver is still
# test-signed, so someone downloading ac3cli/ac3gui should not be handed it.
# Added only when it was actually built, since CPack would otherwise package
# an empty component; kept out of the NSIS installer for now by
# cmake/CPackProjectConfig.cmake, which is where that choice is explained.
if(AC3FORGE_BUILD_CRUCIBLE AND (WIN32 OR LINUX))
    list(APPEND CPACK_COMPONENTS_ALL crucible)
endif()

set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)

# Default grouping (CPack's own ONE_PER_GROUP): one archive/package per
# CPACK_COMPONENT_<C>_GROUP, one per otherwise-ungrouped component. "runtime"
# stays ungrouped (its own archive, as always); "library"+"libruntime" share
# GROUP "dev" so the archive generators still merge them into the single
# "-dev" download documented in docs/releasing.md - the DEB/RPM split above
# is a per-generator override of this default, not a replacement for it.
set(CPACK_COMPONENT_LIBRARY_GROUP "dev")
set(CPACK_COMPONENT_LIBRUNTIME_GROUP "dev")

# Per-component/per-group filename overrides so the existing ac3cli/ac3gui
# and library archives' names don't change now that they are formally
# "the runtime component"/"the dev group" rather than "everything". Without
# an override, an archive's default name appends the component or group's own
# name (e.g. -runtime/-dev) - the runtime override below exists purely to
# suppress that suffix and keep today's exact filename; the dev-group
# override chooses the name explicitly rather than accepting CPack's default
# "-dev" suffix, matching the ac3forge-dev-* convention docs/releasing.md
# documents. CPACK_ARCHIVE_<NAME>_FILE_NAME keys off the GROUP name once one
# is assigned (library+libruntime share GROUP "dev" above), not the
# individual component name - CPACK_ARCHIVE_LIBRARY_FILE_NAME /
# CPACK_ARCHIVE_LIBRUNTIME_FILE_NAME would silently do nothing now.
#
# CPACK_SYSTEM_NAME and CPACK_PACKAGE_FILE_NAME are NOT usable here despite
# looking already computed above - both are actually filled in by the
# include(CPack) module itself, further down, not by any of the set() calls
# in this file: confirmed by an empty CPACK_SYSTEM_NAME producing a real
# "ac3forge-dev-0.2.0-beta.1-.zip" (trailing hyphen, no platform) and the
# runtime override silently no-op'ing back to CPack's own "-runtime"
# suffixed default, from an actual cpack --preset pack-windows-msvc run, not
# assumed. Setting both explicitly here, before include(CPack), replicates
# CPack's own default computation (win32/win64 on Windows, the bare
# CMAKE_SYSTEM_NAME elsewhere; NAME-VERSION-SYSTEM for the base filename) so
# today's existing filename is unchanged, and include(CPack) leaves an
# already-set variable alone rather than recomputing it.
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
        # Arch-qualified, the same reason the LINUX branch below already is:
        # an arm64 build is also an 8-byte-pointer build, so
        # CMAKE_SIZEOF_VOID_P alone can't tell it apart from x64, and without
        # this an arm64 archive would silently collide with (overwrite/get
        # confused with) the existing x64 "win64" archive name.
        # CMAKE_SYSTEM_PROCESSOR is set to exactly "ARM64" by
        # cmake/toolchains/windows.msvc.toolchain.cmake for that target -
        # see its own comment.
        set(CPACK_SYSTEM_NAME "win-arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CPACK_SYSTEM_NAME "win64")
    else()
        set(CPACK_SYSTEM_NAME "win32")
    endif()
elseif(LINUX)
    # Arch-qualified so an x64 and an arm64 TGZ/ZIP built for the same release
    # don't produce an identical filename - CMAKE_SYSTEM_PROCESSOR (x86_64 /
    # aarch64) is already set correctly by cmake/toolchains/linux.*.toolchain.cmake.
    # DEB/RPM don't need this: their own filenames already carry the arch.
    # APPLE stays plain "Darwin" below - only one macOS arch (arm64) exists as
    # a triplet today, so there is no collision to avoid there yet.
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
else()
    set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}")
endif()
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}-${CPACK_SYSTEM_NAME}")

set(CPACK_ARCHIVE_RUNTIME_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
# Named for what it is rather than taking CPack's "-crucible" suffix on the
# base name, the same reasoning as the dev group's override below.
set(CPACK_ARCHIVE_CRUCIBLE_FILE_NAME
    "ac3forge-crucible-${PROJECT_VERSION_FULL}-${CPACK_SYSTEM_NAME}")
set(CPACK_ARCHIVE_DEV_FILE_NAME "ac3forge-dev-${PROJECT_VERSION_FULL}-${CPACK_SYSTEM_NAME}")

include(CPack)

# Lets `cpack` be triggered from inside an IDE's target list (e.g. Visual
# Studio), not just the command line.
add_custom_target(pack-${PROJECT_NAME}
    COMMAND "${CMAKE_CPACK_COMMAND}" -C $<CONFIGURATION> --config "${CPACK_OUTPUT_CONFIG_FILE}"
    COMMENT "Running CPack. Please wait..."
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
set_target_properties(pack-${PROJECT_NAME} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD 1)
