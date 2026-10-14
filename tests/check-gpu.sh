#!/usr/bin/env bash
# Usage: tests/check-gpu.sh
#
# Runs rtx50-keepalive on this machine's NVIDIA GPU for a few seconds at a
# time: it starts and stops cleanly under a 1024 open-file soft limit, stops
# when its command exits, reports a hold, and loads no Vulkan layer even when
# the environment asks for Steam's overlay or MangoHud. Needs an NVIDIA GPU
# with its Vulkan driver and Vulkan loader 1.3.234 or later (an older loader
# keeps the layers on, so the last check fails); run it with no game running.
# Exits 1 if any check fails.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

failures=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1: $2"; failures=$((failures + 1)); }

build() {
    cc -std=c17 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror "$@" \
        "$repo_dir/rtx50-keepalive.c" -lvulkan
}
build -o "$work_dir/rtx50-keepalive"
# Every fence counts as late, so the first jobs make a hold.
build -DLATE_FENCE_MS=0 -o "$work_dir/rtx50-keepalive-every-fence-late"
keepalive="$work_dir/rtx50-keepalive"

# Standalone, under the soft limit systemd gives its units. --preserve-status
# gives the keepalive's own exit status: 1 if it failed after starting.
standalone_status=0
(ulimit -Sn 1024 && timeout --preserve-status -s INT 2 "$keepalive" 2>"$work_dir/standalone") ||
    standalone_status=$?
standalone="$(cat "$work_dir/standalone")"
# Jobs above 0: a stop signal during a slow start ends the keepalive before its first job.
jobs_pattern='; ([0-9]+) jobs, 0 late in 0 holds'
if [[ "$standalone_status" -eq 0 && "$standalone" != *"the keepalive is off"* &&
    "$standalone" == *"a 4-byte fill every 5 ms"* && "$standalone" =~ $jobs_pattern &&
    "${BASH_REMATCH[1]}" -gt 0 ]]; then
    pass "runs and stops cleanly under a 1024 open-file soft limit"
else
    fail "runs and stops cleanly under a 1024 open-file soft limit" \
        "exit status $standalone_status; stderr was [$standalone]"
fi

started_ns="$(date +%s%N)"
"$keepalive" sleep 2 2>"$work_dir/with-command"
elapsed_ms=$(( ($(date +%s%N) - started_ns) / 1000000 ))
with_command="$(cat "$work_dir/with-command")"
# Jobs above 0: a start that outlasts the command ends the keepalive before its first job.
jobs_pattern='stopped after [0-9]+ s; ([0-9]+) jobs'
if [[ "$with_command" != *"the keepalive is off"* && "$with_command" =~ $jobs_pattern &&
    "${BASH_REMATCH[1]}" -gt 0 && "$elapsed_ms" -lt 3000 ]]; then
    pass "stops within 1 s of its command's exit (${elapsed_ms} ms for a 2 s command)"
else
    fail "stops within 1 s of its command's exit" "took ${elapsed_ms} ms; stderr was [$with_command]"
fi

timeout -s INT 2 "$work_dir/rtx50-keepalive-every-fence-late" 2>"$work_dir/hold" || true
if grep -q ' hold: [0-9]* jobs late, longest fence ' "$work_dir/hold"; then
    pass "reports a hold"
else
    fail "reports a hold" "stderr was [$(cat "$work_dir/hold")]"
fi

# Implicit layers a launcher enables through the environment stay out of the
# keepalive (loader 1.3.234 or later).
ENABLE_VK_LAYER_VALVE_steam_overlay_1=1 MANGOHUD=1 "$keepalive" sleep 3 2>"$work_dir/layers" &
wrapper_pid=$!
sleep 2
layers="$(grep -i -o '[^/ ]*\(overlay\|mangohud\|fossilize\|VkLayer\)[^/ ]*\.so' "/proc/$wrapper_pid/maps" | sort -u || true)"
wait "$wrapper_pid"
layers_stderr="$(cat "$work_dir/layers")"
# A keepalive that failed has unloaded its layers, so an empty list proves nothing.
if [[ "$layers_stderr" != *"a 4-byte fill every 5 ms"* || "$layers_stderr" == *"the keepalive is off"* ]]; then
    fail "loads no Vulkan layer" "the keepalive did not run; stderr was [$layers_stderr]"
elif [[ -z "$layers" ]]; then
    pass "loads no Vulkan layer when the environment enables Steam's overlay and MangoHud"
else
    fail "loads no Vulkan layer" "mapped: $layers"
fi

if [[ "$failures" -gt 0 ]]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all GPU checks passed"
