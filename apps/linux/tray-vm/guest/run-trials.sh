#!/bin/bash
# Launches a built ac3crucible N times in the guest's own desktop session and
# counts how many survived, which is the only measurement that means anything
# for a fault that appears on nine or ten launches out of ten.
#
#   crucible-trials --build ~/src/build/crucible -n 10
#   crucible-trials --build ... -n 10 --gdb        one launch per run under gdb,
#                                                  backtrace and registers on death
#   crucible-trials --build ... --valgrind         one launch under memcheck
#
# It refuses to report anything until it has seen a StatusNotifier host on
# the session bus, because a clean run on a session with no host is not
# evidence of anything - that mistake is recorded in promotion.md as the
# first wrong reading of this crash.
set -uo pipefail

# The session the desktop is actually running in. agetty logs `crucible`
# straight in on tty1, the login shell execs labwc, and labwc's autostart
# drops the session's own environment here - so an ssh from the host can
# reach the same display and the same bus.
env_file=$HOME/.crucible-session-env
if [[ -f "$env_file" ]]; then
    set -a
    # shellcheck source=/dev/null  # written by the session at login, not in this tree
    . "$env_file"
    set +a
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}

# Software GL, and this is the one place this guest is not the Pi. VMware's
# vmwgfx does not let wlroots import the dmabufs Qt hands it through the
# guest's accelerated driver - the window dies at once with "importing the
# supplied dmabufs failed", which is a graphics-stack argument and not this
# bug. llvmpipe's buffers import fine, and the window then runs the ordinary
# OpenGL scene graph rather than Qt's software fallback, so the render path
# and its threads are the real ones. Override by exporting it beforehand.
export LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-wayland}

builddir=; runs=10; mode=plain; secs=15; label=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) builddir=$2; shift 2 ;;
        -n) runs=$2; shift 2 ;;
        --seconds) secs=$2; shift 2 ;;
        --label) label=$2; shift 2 ;;
        --gdb) mode=gdb; shift ;;
        --valgrind) mode=valgrind; runs=1; secs=180; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$builddir" ]] || { echo "--build <dir> is required" >&2; exit 2; }
binary=$(cat "$builddir/.binary" 2>/dev/null || find "$builddir" -name ac3crucible -type f -perm -u+x | head -1)
[[ -x "$binary" ]] || { echo "no ac3crucible under $builddir" >&2; exit 1; }

# --- the precondition -------------------------------------------------------
host=$(busctl --user list --no-pager --no-legend 2>/dev/null | grep -i statusnotifier || true)
if [[ -z "$host" ]]; then
    echo "no StatusNotifier host on this session bus." >&2
    echo "busctl --user list shows no org.kde.StatusNotifierWatcher owner, so an item" >&2
    echo "would never be asked for its menu and a clean run would prove nothing." >&2
    exit 1
fi
echo "StatusNotifier host:"
while IFS= read -r line; do echo "  $line"; done <<< "$host"
echo "session: ${XDG_SESSION_TYPE:-unknown}, ${XDG_CURRENT_DESKTOP:-unknown}, display ${WAYLAND_DISPLAY:-${DISPLAY:-none}}"
echo "binary:  $binary"
# shellcheck disable=SC1091  # /etc/os-release is the guest's, not in this tree
echo "arch:    $(uname -m), $(. /etc/os-release; echo "$PRETTY_NAME"), Qt $(qmake6 -query QT_VERSION 2>/dev/null)"
echo "qpa:     $QT_QPA_PLATFORM, LIBGL_ALWAYS_SOFTWARE=$LIBGL_ALWAYS_SOFTWARE"

outdir=$builddir/trials${label:+-$label}
mkdir -p "$outdir"
survived=0; died=0
for i in $(seq 1 "$runs"); do
    log=$outdir/run-$i.log
    case $mode in
    gdb)
        # -ex commands after `run` only reach the prompt when the inferior
        # stops, which for this fault means the signal; a survivor is killed
        # by the timeout and gdb reports nothing further.
        timeout -s KILL "$secs" gdb -q -batch \
            -ex 'set pagination off' -ex 'handle SIGPIPE nostop noprint pass' \
            -ex run -ex 'echo \n=== backtrace ===\n' -ex 'thread apply all bt full' \
            -ex 'echo \n=== registers ===\n' -ex 'info registers' \
            -ex 'echo \n=== frame ===\n' -ex 'info frame' -ex 'info all-registers' \
            --args "$binary" > "$log" 2>&1
        rc=$?
        ;;
    valgrind)
        timeout -s KILL "$secs" valgrind --error-limit=no --num-callers=40 \
            --track-origins=yes --read-var-info=yes --smc-check=all-non-file \
            --log-file="$outdir/valgrind.log" "$binary" > "$log" 2>&1
        rc=$?
        ;;
    *)
        # Grouped with stderr closed so that bash's own job report for an
        # abnormal exit - "Segmentation fault  timeout -s KILL ..." - does
        # not land between the run lines. The verdict below says the same
        # thing, from the exit code, in one line.
        { timeout -s KILL "$secs" "$binary" > "$log" 2>&1; rc=$?; } 2>/dev/null
        ;;
    esac
    # 137 is the KILL the timeout sends: the window was still up, which is a
    # survival. Anything else this early is a death.
    if [[ "$rc" = 137 ]] || [[ "$rc" = 124 ]]; then
        survived=$((survived + 1)); verdict="survived $secs s"
    else
        died=$((died + 1)); verdict="died rc=$rc"
        if [[ "$rc" -gt 128 ]] 2>/dev/null; then verdict="$verdict (SIG$(kill -l $((rc - 128)) 2>/dev/null))"; fi
    fi
    echo "run $i: $verdict  ($log)"
    pkill -x ac3crucible 2>/dev/null
    sleep 1
done

echo
echo "=== $survived of $runs survived, $died died ==="
if [[ "$died" -gt 0 ]]; then
    echo "cores:"
    coredumpctl list --no-pager ac3crucible 2>/dev/null | tail -5 || echo "  (systemd-coredump has none)"
fi
