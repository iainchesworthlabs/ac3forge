#!/usr/bin/env python3
"""Holds the ESP32 player's plays of its stream set to the set's manifest.

The stream set is esp-idf/ac3forge/examples/stream_player/www/ and its
manifest streams.json (tools/generators/gen_device_streams.py writes both).
The manifest gives, for each stream, the level each slot of a 7.1.4 output
should get: the host decoder's, as coded. The player prints what it sent to
each slot at the end of every play (stream.rms[n], RMS x 1e6), so a console
capture is enough to check a play against the host.

    check_stream_set.py check <streams.json> <console> [--all]
        Checks every play the console shows of a stream in the manifest. With
        --all, also fails if a stream the manifest expects the shape to play
        (every one not marked psram) has no play in the console.

    check_stream_set.py play <streams.json> <console> --device <url> --base <url>
        Plays every stream not marked psram, one POST /play at a time, waiting
        for each play's verdict on the console, and checks each as above - and
        what GET /status says about it, where the firmware reports the fields.

A play passes when it printed result=pass, skipped no bytes looking for a
sync word, decoded the manifest's access units (played and held together),
reported twelve slots, and every slot's level is the manifest's to within 1%
+ 20, with a slot the manifest has at 0 at exactly 0, since nothing in the
stream is there and the player does not upmix. The other ESP32 steps allow
5% + 200 for the float32 decode against the host's float64; over the set's
34 plays on 2026-09-11 the largest difference was one unit, and 1% still
catches a channel in the wrong slot, a doubled programme or a fold. A stream
the manifest marks refused must fail, for the reason it names. A panic or
second boot anywhere fails the run; tools/checks/check_esp_console.py is
the fuller check of that and runs after this in CI.

Standard library only: the ESP32 job's container has python3 and no pip.
"""

import argparse
import json
import pathlib
import re
import sys
import time
import urllib.error
import urllib.request

PANIC = re.compile(r"Guru Meditation|abort\(\) was called|assert failed|Rebooting\.\.\.")


def plays(console):
    """The plays in a console capture, in order: each from its 'source:' line
    to its 'result=' line. A play the console shows starting and never
    ending (a panic, a capture cut short) has result None."""
    found = []
    current = None
    for raw in console.splitlines():
        line = raw.strip()
        m = re.match(r"source: \S+ (\S+?), \d+ bytes$", line)
        if m:
            current = {
                "location": m.group(1),
                "rms": {},
                "result": None,
                "units": None,
                "resync": None,
                "stream": None,
                "player": None,
                "panic": False,
                "error": None,
            }
            found.append(current)
            continue
        if current is None:
            continue
        if PANIC.search(line):
            current["panic"] = True
        elif line.startswith("player: layout "):
            current["player"] = line
        elif line.startswith("stream: ") and " onto " in line:
            current["stream"] = line
        elif m := re.match(r"stream\.rms\[(\d+)\]=(\d+)$", line):
            current["rms"][int(m.group(1))] = int(m.group(2))
        elif line.startswith("stream.units="):
            fields = dict(f.split("=", 1) for f in line.split() if "=" in f)
            current["units"] = int(fields.get("stream.units", -1))
            current["held"] = int(fields.get("stream.held", 0))
            current["resync"] = int(fields.get("stream.resync_bytes", -1))
        elif line.startswith("error: "):
            current["error"] = line[len("error: ") :]
        elif line.startswith("result="):
            current["result"] = line[len("result=") :]
            current = None
    return found


def name_of(location):
    return location.split("?", 1)[0].rsplit("/", 1)[-1]


def check_play(play, entry, slots):
    """Problems with one play of one manifest entry, as strings."""
    problems = []
    if play["panic"]:
        problems.append("the part panicked during the play")
    refused = entry.get("refused")
    if refused:
        # A stream the player must not play: it fails, and says why.
        if play["result"] != "fail":
            problems.append(
                f"result={play['result']}, and the player should have refused it ({refused})"
            )
        elif not (play["error"] or "").startswith(f"{refused} failed"):
            problems.append(f"refused with {play['error']!r}, expected {refused!r}")
        return problems
    if play["result"] != "pass":
        problems.append(f"result={play['result']}")
        return problems
    if play["resync"] != 0:
        problems.append(f"{play['resync']} bytes skipped looking for a sync word")
    # Played and held together: a stream using transient pre-noise processing
    # ends with its last unit held back, and the block form the player decodes
    # through has no way to release it (planning/esp32-player.md, the
    # hand-over to the decoder core).
    decoded = play["units"] + play.get("held", 0)
    if entry.get("units") is not None and decoded != entry["units"]:
        problems.append(f"{decoded} access units decoded, the stream has {entry['units']}")
    expected = entry.get("levels_714")
    if expected is None:
        return problems
    if sorted(play["rms"]) != list(range(len(slots))):
        problems.append(f"{len(play['rms'])} slot levels printed, the layout has {len(slots)}")
        return problems
    for index, (name, want) in enumerate(zip(slots, expected, strict=True)):
        got = play["rms"][index]
        slack = 0 if want == 0 else want // 100 + 20
        if abs(got - want) > slack:
            problems.append(
                f"slot {index} ({name}) at {got}, the host decodes {want}"
                + (" and it must be silent" if want == 0 else f" (+/-{slack})")
            )
    return problems


# Where the player places a channel that has no Table E2.5 location: dual
# mono's two programmes go to the left and right speakers.
PLACED = {"Ch1": "L", "Ch2": "R"}


def expected_silent(entry, slots):
    """What stream.silent should say: the layout's slots no channel of the
    stream goes to. A channel that is in the stream but quiet still reaches its
    slot, so this comes from the channels, not from the levels."""
    if entry.get("levels_714") is None:
        return None
    placed = {PLACED.get(name, name) for name in entry.get("channels", [])}
    return ",".join(name for name in slots if name not in placed)


def check_status(status, entry, slots, layout):
    """Problems with GET /status's report of a finished play of `entry`, for
    the fields the firmware reports: a firmware without them is not wrong."""
    problems = []
    if entry.get("refused"):
        if status.get("state") != "failed" or status.get("why") != entry["refused"]:
            problems.append(
                f"/status says {status.get('state')!r} ({status.get('why')!r}), "
                f"expected failed ({entry['refused']!r})"
            )
        return problems
    stream = status.get("stream") or {}
    if "layout" in stream and stream["layout"] != layout:
        problems.append(f"stream.layout {stream['layout']!r}, the play's layout is {layout!r}")
    if "render" in stream:
        want = "channels"
        if stream["render"] != want:
            problems.append(f"stream.render {stream['render']!r}, expected {want!r}")
    if "coded" in stream and entry.get("channels"):
        want = ",".join(entry["channels"])
        if stream["coded"] != want:
            problems.append(f"stream.coded {stream['coded']!r}, the stream carries {want!r}")
    if "silent" in stream:
        want = expected_silent(entry, slots)
        if want is not None and stream["silent"] != want:
            problems.append(f"stream.silent {stream['silent']!r}, the levels say {want!r}")
    return problems


def run_check(manifest, console, require_all):
    slots = manifest["slots"]
    by_name = {s["file"]: s for s in manifest["streams"]}
    failures = []
    seen = set()
    for play in plays(console):
        entry = by_name.get(name_of(play["location"]))
        if entry is None:
            continue
        seen.add(entry["file"])
        problems = check_play(play, entry, slots)
        verdict = "ok" if not problems else "; ".join(problems)
        print(f"{entry['file']}: {verdict}")
        if problems:
            failures.append(entry["file"])
    if require_all:
        for entry in manifest["streams"]:
            if not entry.get("psram") and entry["file"] not in seen:
                print(f"{entry['file']}: never played")
                failures.append(entry["file"])
    return failures


def http(method, url, body=None, timeout=10):
    request = urllib.request.Request(
        url, data=None if body is None else body.encode(), method=method
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, response.read().decode()


def run_play(manifest, console_path, device, base, timeout):
    slots = manifest["slots"]
    layout = manifest["layout"]
    failures = []
    for entry in manifest["streams"]:
        if entry.get("psram"):
            continue
        before = len(plays(console_path.read_text(errors="replace")))
        location = f"{base.rstrip('/')}/{entry['file']}"
        try:
            code, text = http("POST", f"{device.rstrip('/')}/play", location)
        except (urllib.error.URLError, OSError) as e:
            print(f"{entry['file']}: POST /play did not answer ({e})")
            failures.append(entry["file"])
            break
        if code != 202:
            print(f"{entry['file']}: POST /play answered {code}: {text.strip()}")
            failures.append(entry["file"])
            continue
        deadline = time.monotonic() + timeout
        play = None
        while time.monotonic() < deadline:
            found = plays(console_path.read_text(errors="replace"))
            mine = [p for p in found[before:] if name_of(p["location"]) == entry["file"]]
            if mine and (mine[-1]["result"] is not None or mine[-1]["panic"]):
                play = mine[-1]
                break
            time.sleep(1)
        if play is None:
            print(f"{entry['file']}: no verdict on the console in {timeout} s")
            failures.append(entry["file"])
            break
        problems = check_play(play, entry, slots)
        if not play["panic"]:
            try:
                status = json.loads(http("GET", f"{device.rstrip('/')}/status")[1])
                problems += check_status(status, entry, slots, layout)
            except (urllib.error.URLError, OSError, ValueError) as e:
                problems.append(f"GET /status: {e}")
        print(f"{entry['file']}: {'ok' if not problems else '; '.join(problems)}")
        if problems:
            failures.append(entry["file"])
        if play["panic"]:
            break
    return failures


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check")
    c.add_argument("manifest", type=pathlib.Path)
    c.add_argument("console", type=pathlib.Path)
    c.add_argument("--all", action="store_true")
    p = sub.add_parser("play")
    p.add_argument("manifest", type=pathlib.Path)
    p.add_argument("console", type=pathlib.Path)
    p.add_argument("--device", required=True)
    p.add_argument("--base", required=True)
    p.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args(argv)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if args.command == "check":
        failures = run_check(manifest, args.console.read_text(errors="replace"), args.all)
    else:
        failures = run_play(manifest, args.console, args.device, args.base, args.timeout)
    if failures:
        names = ", ".join(failures)
        print(
            f"::error title=ESP32-S3 stream set::{len(failures)} of the set's plays failed: {names}"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
