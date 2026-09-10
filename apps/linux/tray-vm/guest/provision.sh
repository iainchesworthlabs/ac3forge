#!/bin/bash
# Turns the stock Debian 13 cloud image into the machine the Crucible tray
# crash needs: a desktop with a StatusNotifier host, Qt 6.8 and the window's
# build dependencies, and the two things a 2 GB Raspberry Pi could not give -
# Qt's own debug symbols and enough room to run the window under valgrind.
#
# Run once by cloud-init at first boot, and safe to re-run by hand afterwards
# (`sudo crucible-provision`) after editing it in the working tree and
# re-seeding. Everything it does is logged to /var/log/crucible-provision.log
# and the last line of /var/lib/crucible-tray-vm/provisioned says how it went.
set -u
exec > >(tee -a /var/log/crucible-provision.log) 2>&1
echo "=== crucible-provision $(date -Is) ==="

export DEBIAN_FRONTEND=noninteractive
marker=/var/lib/crucible-tray-vm
mkdir -p "$marker"

# apt-get install is all or nothing, so one name this release does not have
# takes a whole group down with it - and the lists below are CI's, from a
# distribution that splits Qt's QML modules differently. So install a group,
# and if that fails install it a package at a time and report what would not
# go. A name that is not here is worth logging, not worth failing over.
#
# The candidate test is apt-cache policy rather than apt-cache show: show
# succeeds for a name that exists only as some other package's virtual
# provide, which apt-get install then refuses.
install_group() {
    local opts=(--no-install-recommends)
    # A desktop metapackage's recommends are the desktop: xfce4 without them
    # is an xfce4-session that starts, finds no xfconfd, and sits there with
    # no panel. Pass --recommends for those groups and nothing else.
    if [[ "$1" = --recommends ]]; then opts=(); shift; fi
    local label=$1 p missing=() present=()
    shift
    for p in "$@"; do
        if apt-cache policy "$p" 2>/dev/null | grep -q '^  Candidate: [^(]'; then present+=("$p"); else missing+=("$p"); fi
    done
    [[ ${#missing[@]} -eq 0 ]] || echo "$label: not in this release: ${missing[*]}"
    [[ ${#present[@]} -gt 0 ]] || return 0
    if apt-get install -y -qq "${opts[@]}" "${present[@]}"; then return 0; fi
    echo "$label: the group would not install together; going one at a time"
    local failed=()
    for p in "${present[@]}"; do
        apt-get install -y -qq "${opts[@]}" "$p" || failed+=("$p")
    done
    [[ ${#failed[@]} -eq 0 ]] || { echo "$label: could not install: ${failed[*]}"; return 1; }
}

# --- archives ---------------------------------------------------------------
# The -debug archive is where Debian's detached debug symbols live, and it is
# the whole reason this guest exists. deb-src as well, so `apt source` can
# fetch the Qt source the backtrace lands in.
cat > /etc/apt/sources.list.d/debug.sources <<'EOF'
Types: deb
URIs: http://deb.debian.org/debian-debug
Suites: trixie-debug
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources 2>/dev/null || true
apt-get update -qq || { echo "apt-get update failed"; echo "FAILED: apt-get update" > "$marker/provisioned"; exit 1; }

# --- the toolchain and the window's dependencies ----------------------------
# The build half of docs/crucible/install.md's Linux requirements, plus what
# vcpkg needs to bootstrap (curl/zip/unzip/tar) for the two packages the
# manifest asks for.
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl zip unzip tar ca-certificates \
    rsync openssh-server open-vm-tools sudo file less \
    || { echo "FAILED: base toolchain" > "$marker/provisioned"; exit 1; }

install_group 'Qt and PipeWire development packages' \
    libpipewire-0.3-dev libxcb1-dev dpkg-dev \
    qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools \
    qt6-l10n-tools qt6-tools-dev qt6-quick3d-dev qt6-shadertools-dev qt6-svg-dev \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-templates \
    qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-effects \
    qml6-module-qtquick-dialogs qml6-module-qt-labs-folderlistmodel \
    qml6-module-qt-labs-platform qml6-module-qtqml-workerscript qml6-module-qtqml-models \
    qml6-module-qtquick3d qml6-module-qttest \
    || { echo "FAILED: Qt and PipeWire development packages" > "$marker/provisioned"; exit 1; }

# --- a session with a StatusNotifier host -----------------------------------
# The precondition for the crash is a desktop that owns
# org.kde.StatusNotifierWatcher, and the reference machine's is wf-panel-pi
# under labwc. wf-panel-pi is a Raspberry Pi package with no amd64 build, so
# this guest runs the same stack a step out: labwc, the same wlroots
# compositor, with waybar as the panel - and Debian's waybar is linked
# against libdbusmenu-gtk3, which is the library that makes the GetLayout
# call the crash arrives on. Xwayland and qt6-wayland are both here so the
# window can be put on either platform plugin from the same session
# (QT_QPA_PLATFORM=wayland or xcb) without rebuilding the guest.
#
# Xfce was tried first and is not here: on this image xfconfd cannot resolve
# its own configuration directory, exits, and takes the panel's plugins with
# it - a session with no host on it, which is no experiment at all.
install_group --recommends 'desktop' \
    labwc waybar foot xwayland qt6-wayland \
    libgl1-mesa-dri mesa-vulkan-drivers \
    fonts-dejavu-core adwaita-icon-theme hicolor-icon-theme \
    x11-utils x11-xserver-utils xdotool grim \
    || echo "some desktop packages missing; see above"

# PipeWire with a session manager, which is what Crucible's Linux platform
# half talks to, plus its PulseAudio shim so the desktop's own sounds do not
# fight it for the device.
install_group 'PipeWire' \
    pipewire pipewire-audio pipewire-pulse wireplumber libspa-0.2-modules \
    || echo "some PipeWire packages missing; see above"

# --- what the Pi could not run ----------------------------------------------
# gdb with the debug symbols, valgrind for the first invalid read rather than
# the fault it eventually causes, and systemd-coredump so a crash that
# happens inside a ten-launch loop is still there to look at afterwards.
# debuginfod is belt and braces: it serves the same symbols over the network
# for anything the -dbgsym list below misses.
install_group 'debug tools' \
    gdb valgrind systemd-coredump elfutils debian-goodies debuginfod \
    || echo "some debug tools missing; see above"

install_group 'Qt debug symbols' \
    libqt6core6t64-dbgsym libqt6gui6-dbgsym libqt6dbus6-dbgsym libqt6widgets6-dbgsym \
    libqt6qml6-dbgsym libqt6quick6-dbgsym libqt6quickcontrols2-dbgsym \
    libqt6network6-dbgsym qt6-qpa-plugins-dbgsym \
    qml6-module-qt-labs-platform-dbgsym qml6-module-qtquick-dbgsym \
    || echo "some -dbgsym packages missing; find-dbgsym-packages on a core will name the rest"

cat > /etc/profile.d/debuginfod.sh <<'EOF'
export DEBUGINFOD_URLS="https://debuginfod.debian.net/"
EOF

# Keep the whole core. The interesting object is a few hundred bytes reached
# through a pointer, so a truncated dump is worth nothing.
mkdir -p /etc/systemd/coredump.conf.d
cat > /etc/systemd/coredump.conf.d/crucible.conf <<'EOF'
[Coredump]
Storage=external
Compress=yes
ProcessSizeMax=8G
ExternalSizeMax=8G
EOF
mkdir -p /etc/security/limits.d
echo '* - core unlimited' > /etc/security/limits.d/crucible-core.conf
mkdir -p /etc/systemd/system/user@.service.d
printf '[Service]\nLimitCORE=infinity\n' > /etc/systemd/system/user@.service.d/core.conf

# --- log straight in --------------------------------------------------------
# There is no one at the console, and the crash needs a real session on a
# real compositor rather than an Xvfb. No display manager: agetty logs the
# user in on tty1 and the login shell execs labwc, which is one moving part
# instead of three and puts the session on a seat logind already owns.
mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin crucible --noclear %I $TERM
EOF
systemctl set-default multi-user.target
systemctl disable lightdm 2>/dev/null || true

cat > /home/crucible/.bash_profile <<'EOF'
# The console login starts the session; an ssh login (any other tty) does not.
[[ -f ~/.bashrc ]] && . ~/.bashrc
if [[ -z "${WAYLAND_DISPLAY:-}" ]] && [[ "$(tty)" = "/dev/tty1" ]]; then
    exec labwc > ~/.labwc.log 2>&1
fi
EOF

# The panel. Only a clock and the tray: everything else in waybar's shipped
# configuration is a sway module that errors out under labwc, and the tray is
# the only part of it this guest exists for.
install -d -o crucible -g crucible /home/crucible/.config/waybar /home/crucible/.config/labwc
cat > /home/crucible/.config/waybar/config <<'EOF'
{
  "layer": "top",
  "position": "top",
  "height": 28,
  "modules-left": ["clock"],
  "modules-right": ["tray"],
  "tray": { "spacing": 8, "icon-size": 16 }
}
EOF
cat > /home/crucible/.config/labwc/autostart <<'EOF'
/usr/local/bin/crucible-session-env
waybar > ~/.waybar.log 2>&1 &
EOF

# An ssh from the host is not in the compositor's session, so it has neither
# the display nor the session bus the tray needs. The session writes its own
# environment out at startup and run-trials.sh sources it.
cat > /usr/local/bin/crucible-session-env <<'EOF'
#!/bin/sh
# Run inside the session. Writes the session's own environment where an ssh
# can read it, and opens the X display to local users where there is one,
# because the cookie belongs to the session and this guest is NAT-only.
xhost +local: >/dev/null 2>&1
for v in DISPLAY WAYLAND_DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS \
         XDG_RUNTIME_DIR XDG_SESSION_TYPE XDG_CURRENT_DESKTOP XDG_DATA_DIRS; do
    eval "value=\${$v:-}"
    [[ -n "$value" ]] && echo "$v=$value"
done > "$HOME/.crucible-session-env"
EOF
chmod 0755 /usr/local/bin/crucible-session-env
chown -R crucible:crucible /home/crucible/.config /home/crucible/.bash_profile

# --- vcpkg ------------------------------------------------------------------
# The presets chainload it (CMakePresets.json, "core"), and the manifest asks
# for catch2 and fmt only, so this is a few minutes once and cached after.
# A full clone, not a shallow one: vcpkg has to reach the manifest's
# builtin-baseline commit.
if [[ ! -d /opt/vcpkg/.git ]]; then
    git clone --quiet https://github.com/microsoft/vcpkg /opt/vcpkg \
        && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics >/dev/null
fi
chown -R crucible:crucible /opt/vcpkg 2>/dev/null || true
cat > /etc/profile.d/vcpkg.sh <<'EOF'
export VCPKG_ROOT=/opt/vcpkg
EOF

# --- what is here -----------------------------------------------------------
{
    echo "provisioned $(date -Is)"
    # shellcheck disable=SC1091  # /etc/os-release is the guest's, not in this tree
    echo "debian: $(. /etc/os-release; echo "$PRETTY_NAME")"
    echo "kernel: $(uname -srm)"
    echo "qmake6: $(qmake6 -query QT_VERSION 2>/dev/null || echo none)"
    echo "gcc:    $(gcc -dumpfullversion 2>/dev/null || echo none)"
    echo "cmake:  $(cmake --version 2>/dev/null | head -1)"
    echo "gdb:    $(gdb --version 2>/dev/null | head -1)"
    echo "dbgsym: $(dpkg -l 'libqt6*-dbgsym' 2>/dev/null | grep -c '^ii') Qt packages"
} > "$marker/provisioned"
cat "$marker/provisioned"
echo "=== crucible-provision done $(date -Is) ==="
