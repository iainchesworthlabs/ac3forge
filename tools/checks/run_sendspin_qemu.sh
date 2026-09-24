#!/usr/bin/env bash
# Plays to hearth_sink's Sendspin player under QEMU from the host, and holds
# what it played to a test sink's (planning/hearth-reference-player.md, B4).
#
# The board boots sdkconfig.ci-sendspin's image with nothing to play, prints
# its pairing token on the console, and waits for a server. The test server
# (apps/hearth/testserver) dials it through QEMU's port forward, pairs by the
# token, sends it a 2.0 layout, and plays the E-AC-3 JOC fixture to it and to
# a test sink of its own in one group, reading the board's GET /status as it
# plays. It fails on a connection, a pairing, a setting or a burst that goes
# wrong. The board's GET /pairing must then list the test server by name, and
# forget it by its server_id. Then check_sendspin_levels.py holds each of the
# board's outputs to the test sink's WAV, and check_esp_console.py holds the
# console to one clean boot and the heap floor.
#
#   tools/checks/run_sendspin_qemu.sh --qemu QEMU --image DIR --server SERVER \
#       [--out DIR] [--board-trim-db LIST]
#
#   --qemu           qemu-system-xtensa
#   --image          the directory holding the build's qemu_flash.bin and
#                    qemu_efuse.bin
#   --server         ac3hearth-testserver
#   --out            where the console, the report and the test sink's WAV go
#                    (default ./sendspin-qemu-run)
#   --board-trim-db  trims for the board that the test sink does not have, as
#                    the test server's --trim-db takes them: a level mismatch
#                    made on purpose, which this must fail on
#
# Exit status 0 when all of it passes, 1 otherwise, with ::error::
# annotations saying what failed.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
qemu=""
image=""
server=""
out="sendspin-qemu-run"
trims=""
while [ $# -gt 0 ]; do
    case "$1" in
        --qemu) qemu="$2"; shift 2 ;;
        --image) image="$2"; shift 2 ;;
        --server) server="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --board-trim-db) trims="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
if [ -z "$qemu" ] || [ -z "$image" ] || [ -z "$server" ]; then
    echo "usage: $0 --qemu QEMU --image DIR --server SERVER [--out DIR] [--board-trim-db LIST]" >&2
    exit 2
fi

title="ESP32-S3 Sendspin player"
rm -rf "$out"
mkdir -p "$out"
console="$out/qemu-sendspin.txt"
: > "$console"

# QEMU writes to its flash image, NVS included, so the board boots a copy and
# leaves the image as it was for whatever boots it next.
cp "$image/qemu_flash.bin" "$image/qemu_efuse.bin" "$out/"
"$qemu" -M esp32s3 -m 32M \
    -drive "file=$out/qemu_flash.bin,if=mtd,format=raw" \
    -drive "file=$out/qemu_efuse.bin,if=none,format=raw,id=efuse" \
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:18928-:8928,hostfwd=tcp:127.0.0.1:18080-:80 \
    -nographic -monitor none -serial "file:$console" &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true' EXIT

# A failure is reported with the console's own verdict first, so a check that
# trips because the part panicked says so.
fail() {
    python3 "$here/check_esp_console.py" --title "$title" "$console" || true
    echo "::error title=$title::$1" >&2
    grep -aE 'sendspin|burst player|heap:|error' "$console" >&2 || true
    exit 1
}

# The token, once the player is up. The pattern is anchored at neither end:
# the token has a word in front of it, and how a console ends its lines is its
# own business.
token=""
for _ in $(seq 1 300); do
    token="$(grep -aoE 'pairing token SP:0[A-Za-z0-9_-]+' "$console" | head -1 | sed 's/^pairing token //')" || true
    if [ -n "$token" ]; then
        break
    fi
    kill -0 "$qemu_pid" 2>/dev/null || fail "QEMU exited before the player printed its pairing token"
    sleep 1
done
[ -n "$token" ] || fail "the player printed no pairing token in 300 s"

board_trims=()
if [ -n "$trims" ]; then
    board_trims=(--trim-db "$trims")
fi
server_status=0
"$server" \
    --state "$out/server-state" \
    --play "$root/tests/golden/object-fixture/dee_joc_514.ec3" --seconds 10 --timeout 180 \
    --player ws://127.0.0.1:18928/sendspin --label board --token "$token" --layout 2.0 \
    "${board_trims[@]}" --status http://127.0.0.1:18080/status \
    --sink "$out/reference" --sink-layout 2.0 \
    --report "$out/report.json" 2> "$out/server.txt" || server_status=$?
cat "$out/server.txt"
if [ "$server_status" -ne 0 ]; then
    fail "the test server failed ($(tail -1 "$out/server.txt"))"
fi

# The board's closing lines for the stream, which follow its end.
for _ in $(seq 1 30); do
    if grep -aq 'sendspin\.rms\[1\]=' "$console"; then
        break
    fi
    sleep 1
done

# The board's list of the servers it is paired with (GET /pairing): the test
# server alone, by the name its hello gave and the server_id its report gives.
# Then that one server forgotten by its server_id, which takes it off the list
# and out of /status's count (planning/esp32-device-ui.md, Several servers).
server_id="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["server"]["id"])' "$out/report.json")" \
    || fail "the test server's report gives no server id"
curl -fsS http://127.0.0.1:18080/pairing > "$out/pairing.json" || fail "GET /pairing failed"
python3 - "$out/pairing.json" "$server_id" <<'EOF' || fail "GET /pairing did not list the test server as it paired (the line above says how)"
import json, sys
listed = json.load(open(sys.argv[1]))
servers = listed.get("servers", [])
expected = {"server_id": sys.argv[2], "name": "Hearth test server", "seen": True}
if listed.get("capacity") != 8 or len(servers) != 1 or any(servers[0].get(k) != v for k, v in expected.items()):
    print(f"GET /pairing: {json.dumps(listed)}; expected one server like {json.dumps(expected)}", file=sys.stderr)
    sys.exit(1)
EOF
curl -fsS -X POST --data "forget $server_id" http://127.0.0.1:18080/pairing >/dev/null \
    || fail "POST /pairing did not forget the test server by its server_id"
curl -fsS http://127.0.0.1:18080/pairing > "$out/pairing-forgotten.json" || fail "GET /pairing failed after the forget"
curl -fsS http://127.0.0.1:18080/status > "$out/status-forgotten.json" || fail "GET /status failed after the forget"
python3 - "$out/pairing-forgotten.json" "$out/status-forgotten.json" <<'EOF' || fail "the forgotten server is still paired (the line above says how)"
import json, sys
servers = json.load(open(sys.argv[1])).get("servers", [])
paired = json.load(open(sys.argv[2]))["sendspin"]["paired"]
if servers or paired != 0:
    print(f"after the forget, GET /pairing lists {json.dumps(servers)} and /status counts {paired}", file=sys.stderr)
    sys.exit(1)
EOF
kill "$qemu_pid" 2>/dev/null || true

wav="$(find "$out/reference/out" -name 'bursts-*.wav' -print -quit 2>/dev/null)" || true
[ -n "$wav" ] || fail "the test sink wrote no WAV file"
python3 "$here/check_sendspin_levels.py" --title "$title" --console "$console" --wav "$wav" \
    || fail "the board's levels are not the test sink's (the lines above say which)"
# The floor is the stream set's (.github/workflows/_build.yml): the widest
# shape the part runs holds more than this one, which runs a 16 KB ring, two
# HTTP servers and a Sendspin session beside the decoder.
python3 "$here/check_esp_console.py" --title "$title" --min-heap-free 20480 "$console"
