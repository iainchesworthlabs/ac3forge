#!/usr/bin/env bash
# Record WHAT a benchmark run was measured on, beside the measurements.
#
# ms/frame is hardware-relative in a way the quality series' dB is not - which
# is why append_performance_history.py's thresholds are percentages rather than
# fixed deltas - but nothing recorded WHICH hardware. A hosted-image bump or a
# differently-specced runner then lands as an unattributable step in the
# series, indistinguishable from a code change.
#
# Written into the leg's own results directory rather than passed on the
# appender's command line, because the trend now has more than one leg and they
# are measured on different machines: one x86 job and one arm64 job cannot
# share a single --cpu-model value. append_performance_history.py reads this
# file per leg and records empty strings for a leg that has none.
#
# $LEG_DIR is the results directory to write into.
set -euo pipefail

: "${LEG_DIR:?must name the results directory for this leg}"

# `model name` is an x86 field. arm64 kernels do not emit it - the closest
# equivalent is lscpu's `Model name` - so lscpu is tried second, and the field
# is allowed to end up "unknown" rather than failing a trend job over a
# cosmetic string.
cpu="$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1 || true)"
if [ -z "$cpu" ] && command -v lscpu >/dev/null 2>&1; then
    cpu="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -1 || true)"
fi

# Squeezing the whitespace is not cosmetic: /proc/cpuinfo pads the model name
# (e.g. "AMD Ryzen 9 5950X 16-Core Processor" with a dozen trailing spaces),
# and that padding would otherwise be baked into every future record.
cpu="$(printf '%s' "$cpu" | tr -s '[:space:]' ' ' | sed 's/^ //; s/ $//')"
[ -n "$cpu" ] || cpu="unknown"

# ImageOS/ImageVersion are set on the runner host. These jobs run inside a
# container, where they may legitimately not be present - hence the fallback
# rather than a failure. cpu_model is the load-bearing half and reads the host
# CPU through /proc/cpuinfo either way.
image="${ImageOS:-unknown}/${ImageVersion:-unknown}"
arch="$(uname -m)"

# Built with python3 rather than a printf template: this file is machine-read,
# and a model name containing a quote or a backslash would otherwise emit JSON
# the appender cannot parse. No CPU string in the wild does, but a trend job is
# a bad place to discover the first one. python3 is already a hard dependency
# of every job that calls this - it runs the appender two steps later.
mkdir -p "$LEG_DIR"
CPU="$cpu" IMAGE="$image" ARCH="$arch" OUT="$LEG_DIR/environment.json" \
    python3 -c 'import json, os
json.dump({"cpu_model": os.environ["CPU"],
           "runner_image": os.environ["IMAGE"],
           "arch": os.environ["ARCH"]},
          open(os.environ["OUT"], "w"), indent=2)'

echo "measured on: $cpu ($image, $arch)"
