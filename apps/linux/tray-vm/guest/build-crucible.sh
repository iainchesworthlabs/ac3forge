#!/bin/bash
# Builds ac3crucible inside the guest from the tree Sync-Source.ps1 pushed
# to ~/src, optionally with one of the two edits this VM exists to measure.
#
#   crucible-build                 the window as it ships
#   crucible-build --tray          force the tray on where the seam says no
#   crucible-build --nest-submenu  put a submenu back in the tray's menu -
#                                  the reproducer for the Qt bug
#   crucible-build --nest-submenu --asan   the same, with AddressSanitizer
#
# Both edits are applied here rather than committed, and reverted from a
# pristine copy on every run, so the shipped source in ~/src never drifts:
# what the tree says is what the tree says, and this only ever changes what
# is on this VM.
#
# --nest-submenu is the one that matters now. QDBusPlatformMenu implements no
# createSubMenu(), so a Qt.labs Menu nested inside a tray icon's menu gets Qt
# Labs Platform's QWidget fallback and is then static_cast to the D-Bus one;
# the panel's first GetLayout reads it as the wrong type and the window dies,
# nine or ten launches in ten. The tray menu is flat because of it, and this
# flag is what shows that the constraint is still real.
#
# --asan instruments Crucible, not Qt. That is still worth having: ASan's
# allocator replaces glibc's process-wide, so a use-after-free or an overflow
# of memory *we* allocated is caught even when the bad access happens inside
# Qt, and every report carries the allocating and freeing stacks. For a fault
# inside Qt's own objects, though, valgrind (crucible-trials --valgrind) is
# the better tool and needs no rebuild - it is what found this one.
set -euo pipefail

SRC=${CRUCIBLE_SRC:-$HOME/src}
tray=0
nest=0
asan=0
builddir=
for arg in "$@"; do
    case "$arg" in
        --tray) tray=1 ;;
        --nest-submenu) nest=1 ;;
        --asan) asan=1 ;;
        --build-dir=*) builddir=${arg#*=} ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
if [ -z "$builddir" ]; then
    builddir=$SRC/build/crucible
    [ $tray = 0 ] || builddir=$builddir-tray
    [ $nest = 0 ] || builddir=$builddir-nested
    [ $asan = 0 ] || builddir=$builddir-asan
fi

export VCPKG_ROOT=${VCPKG_ROOT:-/opt/vcpkg}
qml=$SRC/apps/crucible/ui/qml/Main.qml
[ -f "$qml" ] || { echo "no $qml; run Sync-Source.ps1 from the host first" >&2; exit 1; }
[ -f "$qml.pristine" ] || cp "$qml" "$qml.pristine"
cp "$qml.pristine" "$qml"

if [ $tray = 1 ]; then
    # \r? because a tree synced from a Windows working copy can arrive with
    # CRLF and an anchored match would then silently find nothing.
    sed -i 's/^\(\s*\)visible: CrucibleController\.trayAvailable\r\?$/\1visible: true/' "$qml"
    grep -q '^[[:space:]]*visible: true\r\?$' "$qml" || { echo "the tray edit did not apply; has Main.qml moved?" >&2; exit 1; }
    echo "edit applied: SystemTrayIcon visible: true, whatever the seam says"
fi

if [ $nest = 1 ]; then
    # One submenu, of literals, in the tray's menu. Anchored on the tray
    # menu's first item so it lands inside that menu and nowhere else.
    python3 - "$qml" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding='utf-8').read()
anchor = 'Platform.MenuItem { text: qsTr("Open the room");'
i = text.index(anchor)
end = text.index('\n', i) + 1
nested = (' ' * 12 + 'Platform.Menu { title: "nested"; '
          'Platform.MenuItem { text: "one" } Platform.MenuItem { text: "two" } }\n')
open(path, 'w', encoding='utf-8', newline='').write(text[:end] + nested + text[end:])
PY
    grep -q 'Platform.Menu { title: "nested"' "$qml" || { echo "the submenu edit did not apply; has Main.qml moved?" >&2; exit 1; }
    echo "edit applied: one Platform.Menu nested in the tray's menu"
fi

flags=()
if [ $asan = 1 ]; then
    flags+=(-DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
            -DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g'
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address)
fi

# The Linux recipe from docs/crucible/install.md. Tests off: this build
# exists to be launched ten times, not to be tested, and the library's suite
# is a long build for nothing here. RelWithDebInfo rather than the preset's
# Release so our own frames in the backtrace carry line numbers.
cmake --preset config-linux-gcc -B "$builddir" -S "$SRC" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DAC3FORGE_BUILD_CRUCIBLE=ON \
    -DAC3FORGE_BUILD_TESTS=OFF \
    -DAC3FORGE_BUILD_CLI=OFF \
    -DAC3FORGE_WITH_ALSA=OFF \
    -DAC3FORGE_WITH_PIPEWIRE=ON \
    "${flags[@]}"
cmake --build "$builddir" --target ac3crucible -- -j"$(nproc)"

cp "$qml.pristine" "$qml"
binary=$(find "$builddir" -name ac3crucible -type f -perm -u+x | head -1)
echo "built: $binary"
echo "$binary" > "$builddir/.binary"
