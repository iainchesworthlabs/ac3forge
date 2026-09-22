#!/usr/bin/env bash
# Gives hearth_sink a network over Improv Wi-Fi under QEMU, and plays to the
# Sendspin player it starts once it has one (planning/hearth-reference-player.md,
# B2 and B4).
#
# A board with no network listens for Improv on its console. QEMU has no radio,
# so two images stand in for one board, each booted from a copy (QEMU writes
# to its flash image, and NVS is in it):
#
# - The Sendspin player on QEMU's Ethernet (sdkconfig.ci-sendspin), the image
#   run_sendspin_qemu.sh plays to, booted with its link down. Its boot gives up
#   on the network after 30 s. improv_qemu.py's late-network scenario then
#   sends it a network over Improv and brings the link up while it waits. The
#   board must answer with its page's URL, and start mDNS, the player and the
#   player's console commands. The test server then pairs with that player by
#   its token and plays to it, as run_sendspin_qemu.sh does to one started at
#   boot. check_sendspin_levels.py holds its levels to the test sink's, and
#   check_esp_console.py holds its console to one clean boot and the heap floor.
# - The same player on WiFi, with nothing stored and nothing built in
#   (sdkconfig.ci-wifi). improv_qemu.py's unprovisioned scenario holds it to its
#   control surface and to Improv's current_state and device_info answers, and
#   check_esp_console.py holds its console to one clean boot. It is never given
#   a network: QEMU has no radio for it to join one with.
#
# Before #741, the first board aborted in the network_up() Improv's task called,
# and the second restarted in a loop on an lwIP assertion.
#
#   tools/checks/run_improv_qemu.sh --qemu QEMU --image DIR --wifi-image DIR \
#       --server SERVER [--out DIR]
#
#   --qemu        qemu-system-xtensa
#   --image       the directory holding the Sendspin image's qemu_flash.bin
#                 and qemu_efuse.bin
#   --wifi-image  the same for the WiFi image
#   --server      ac3hearth-testserver
#   --out         where the consoles, the report and the test sink's WAV go
#                 (default ./improv-qemu-run)
#
# Exit status 0 when all of it passes, 1 otherwise, with ::error::
# annotations saying what failed.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
qemu=""
image=""
wifi_image=""
server=""
out="improv-qemu-run"
while [ $# -gt 0 ]; do
    case "$1" in
        --qemu) qemu="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --wifi-image) wifi_image="$2"; shift 2 ;;
        --server) server="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
if [ -z "$qemu" ] || [ -z "$image" ] || [ -z "$wifi_image" ] || [ -z "$server" ]; then
    echo "usage: $0 --qemu QEMU --image DIR --wifi-image DIR --server SERVER [--out DIR]" >&2
    exit 2
fi

rm -rf "$out"
mkdir -p "$out"
failed=0
qemu_pid=""
helper_pid=""
helper_status=0
# Whatever is still running at exit. The pids are left unquoted so that an
# empty one drops out.
trap 'kill $qemu_pid $helper_pid 2>/dev/null || true' EXIT

# One ::error:: annotation, and the part's console lines that say what the
# board did, with the console check's own verdict first: a step that failed
# because the part panicked says so.
fail() {
    local title="$1" console="$2" message="$3"
    failed=1
    python3 "$here/check_esp_console.py" --title "$title" "$console" || true
    echo "::error title=$title::$message" >&2
    grep -aE 'openeth|network|improv|mdns|sendspin|console:|control:|burst player|heap:|error|ESP_ERROR_CHECK|file:|func:|expression:|abort|assert|Backtrace|Rebooting' \
        "$console" >&2 || true
}

# Boots a copy of the image in $1, in directory $2, with its console on TCP
# port $3 and its monitor on port $4, held at reset until the monitor says
# `cont`. Anything after that is QEMU's network. Sets qemu_pid.
boot() {
    local from="$1" dir="$2" serial="$3" monitor="$4"
    shift 4
    mkdir -p "$dir"
    cp "$from/qemu_flash.bin" "$from/qemu_efuse.bin" "$dir/"
    "$qemu" -M esp32s3 -m 32M -S \
        -drive "file=$dir/qemu_flash.bin,if=mtd,format=raw" \
        -drive "file=$dir/qemu_efuse.bin,if=none,format=raw,id=efuse" \
        -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        "$@" \
        -nographic \
        -monitor "tcp:127.0.0.1:$monitor,server=on,wait=off" \
        -serial "tcp:127.0.0.1:$serial,server=on,wait=off" \
        < /dev/null &
    qemu_pid=$!
}

# Stops the part's QEMU, which ends a helper's capture, and waits for both.
# A helper left running in the background sets helper_status as it exits.
stop() {
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
    qemu_pid=""
    if [ -n "$helper_pid" ]; then
        helper_status=0
        wait "$helper_pid" || helper_status=$?
        helper_pid=""
    fi
}

# --- The Sendspin player, given its network over Improv -------------------

title="ESP32-S3 Improv then Sendspin"
dir="$out/late-network"
console="$dir/qemu-improv.txt"
boot "$image" "$dir" 15571 15572 \
    -nic user,id=n0,model=open_eth,hostfwd=tcp:127.0.0.1:28928-:8928,hostfwd=tcp:127.0.0.1:28080-:80
python3 "$here/improv_qemu.py" late-network \
    --serial 127.0.0.1:15571 --monitor 127.0.0.1:15572 \
    --control http://127.0.0.1:28080 \
    --capture "$console" --ready "$dir/improv.ready" --title "$title" &
helper_pid=$!

# The helper says when the board has its network and its player, or exits.
# Its own waits bound this one.
for _ in $(seq 1 900); do
    if [ -e "$dir/improv.ready" ] || ! kill -0 "$helper_pid" 2>/dev/null; then
        break
    fi
    sleep 1
done
if [ ! -e "$dir/improv.ready" ]; then
    stop
    fail "$title" "$console" "the board was not provisioned over Improv (the helper's annotation above says why)"
else
    # The token, which the helper has seen. The pattern is anchored at neither
    # end: the token has a word in front of it, and how a console ends its
    # lines is its own business.
    token="$(grep -aoE 'pairing token SP:0[A-Za-z0-9_-]+' "$console" | head -1 | sed 's/^pairing token //')" || true
    server_status=0
    "$server" \
        --state "$dir/server-state" \
        --play "$root/tests/golden/object-fixture/dee_joc_514.ec3" --seconds 10 --timeout 180 \
        --player ws://127.0.0.1:28928/sendspin --label board --token "$token" --layout 2.0 \
        --status http://127.0.0.1:28080/status \
        --sink "$dir/reference" --sink-layout 2.0 \
        --report "$dir/report.json" 2> "$dir/server.txt" || server_status=$?
    cat "$dir/server.txt"
    if [ "$server_status" -ne 0 ]; then
        stop
        fail "$title" "$console" "the test server failed ($(tail -1 "$dir/server.txt"))"
    else
        # The board's closing lines for the stream, which follow its end.
        for _ in $(seq 1 30); do
            if grep -aq 'sendspin\.rms\[1\]=' "$console"; then
                break
            fi
            sleep 1
        done
        stop
        wav="$(find "$dir/reference/out" -name 'bursts-*.wav' -print -quit 2>/dev/null)" || true
        if [ "$helper_status" -ne 0 ]; then
            fail "$title" "$console" "the helper's capture ended badly (status $helper_status)"
        elif [ -z "$wav" ]; then
            fail "$title" "$console" "the test sink wrote no WAV file"
        elif ! python3 "$here/check_sendspin_levels.py" --title "$title" --console "$console" --wav "$wav"; then
            fail "$title" "$console" "the board's levels are not the test sink's (the lines above say which)"
        # The heap floor run_sendspin_qemu.sh holds the same player to.
        elif ! python3 "$here/check_esp_console.py" --title "$title" --min-heap-free 20480 "$console"; then
            failed=1
        fi
    fi
fi
rm -f "$dir/qemu_flash.bin" "$dir/qemu_efuse.bin"

# --- The WiFi player with nothing to join ---------------------------------

title="ESP32-S3 Improv with nothing stored"
dir="$out/unprovisioned"
console="$dir/qemu-improv.txt"
boot "$wifi_image" "$dir" 15581 15582 -nic none
# In the foreground this time, with a few seconds' more capture once the
# answers are in, for a reset that follows them.
answers_status=0
python3 "$here/improv_qemu.py" unprovisioned \
    --serial 127.0.0.1:15581 --monitor 127.0.0.1:15582 \
    --capture "$console" --hold 5 --title "$title" || answers_status=$?
stop
if [ "$answers_status" -ne 0 ]; then
    fail "$title" "$console" "the board did not answer Improv as a board with nothing stored should (the helper's annotation above says why)"
elif ! python3 "$here/check_esp_console.py" --title "$title" "$console"; then
    failed=1
fi
rm -f "$dir/qemu_flash.bin" "$dir/qemu_efuse.bin"

exit "$failed"
