#!/usr/bin/env bash
# Usage: tests/check.sh
#
# Builds rtx50-keepalive with warnings as errors (with every compiler found
# among cc, gcc and clang) and tests how it runs a command. Needs no GPU: the
# Vulkan loader is pointed at a driver file that does not exist, so the
# keepalive fails to start, which is the path that must still run the command.
# Exits 1 if any check fails.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
# The loader reads VK_DRIVER_FILES (1.3.207 and later) or VK_ICD_FILENAMES (older).
export VK_DRIVER_FILES="$work_dir/no-such-driver.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"

failures=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1: $2"; failures=$((failures + 1)); }

build() {
    local compiler="$1" output="$2"
    "$compiler" -std=c17 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror \
        -o "$output" "$repo_dir/rtx50-keepalive.c" -lvulkan
}

compilers_built=0
for compiler in cc gcc clang; do
    if command -v "$compiler" >/dev/null; then
        if build "$compiler" "$work_dir/rtx50-keepalive-$compiler"; then
            pass "builds with $compiler and -Werror"
            compilers_built=$((compilers_built + 1))
        else
            fail "builds with $compiler and -Werror" "see the compiler output above"
        fi
    fi
done
if [[ "$compilers_built" -eq 0 ]]; then
    echo "FAIL  no compiler built it; nothing else can be tested"
    exit 1
fi
keepalive="$(find "$work_dir" -maxdepth 1 -name 'rtx50-keepalive-*' -type f | head -n 1)"

# Runs the keepalive with the given arguments; sets status, stdout and stderr.
run() {
    set +e
    "$keepalive" "$@" >"$work_dir/stdout" 2>"$work_dir/stderr" </dev/null
    status=$?
    set -e
    stdout="$(cat "$work_dir/stdout")"
    stderr="$(cat "$work_dir/stderr")"
}

expect() {
    local name="$1" field="$2" wanted="$3" actual="$4"
    if [[ "$actual" == "$wanted" ]]; then
        pass "$name"
    else
        fail "$name" "$field was [$actual], expected [$wanted]"
    fi
}

expect_contains() {
    local name="$1" wanted="$2" actual="$3"
    if [[ "$actual" == *"$wanted"* ]]; then
        pass "$name"
    else
        fail "$name" "output [$actual] lacks [$wanted]"
    fi
}

run --help
expect "--help exits 0" status 0 "$status"
expect_contains "--help prints the usage" "Usage: rtx50-keepalive COMMAND" "$stdout"

run --version
expect_contains "--version prints the version" "rtx50-keepalive " "$stdout"

run --bogus
expect "an unknown option exits 2" status 2 "$status"

run sh -c 'exit 0'
expect "a command's exit status 0 passes through" status 0 "$status"

run sh -c 'exit 42'
expect "a command's exit status 42 passes through" status 42 "$status"

run sh -c 'kill -TERM $$'
expect "a command ended by SIGTERM gives 128+15" status 143 "$status"

run printf '[%s]' 'a b' '' '*' '"q"'
expect "arguments reach the command unchanged" stdout '[a b][][*]["q"]' "$stdout"

run -- sh -c 'exit 3'
expect "-- before the command is accepted" status 3 "$status"

run sh -c 'echo "${VK_LOADER_LAYERS_DISABLE-unset}"'
expect "the command's environment is left as it was" stdout unset "$stdout"

# The keepalive raises its own soft limit; under a low one, the command must keep it.
low_limit=1024
if [[ "$(ulimit -Hn)" != unlimited && "$(ulimit -Hn)" -le "$low_limit" ]]; then
    low_limit="$(( $(ulimit -Hn) - 1 ))"
fi
set +e
output="$(ulimit -Sn "$low_limit" && "$keepalive" sh -c 'ulimit -Sn' 2>/dev/null)"
set -e
expect "the command's open-file limit is left as it was" stdout "$low_limit" "$output"

run sh -c 'echo ran'
expect "the command runs when the keepalive cannot start" stdout ran "$stdout"
expect_contains "the failed start is reported on stderr" "the keepalive is off" "$stderr"

set +e
output="$(echo hello | "$keepalive" cat 2>/dev/null)"
set -e
expect "stdin reaches the command" stdout hello "$output"

run "$work_dir/no-such-command"
expect "a missing command exits 127" status 127 "$status"
expect_contains "a missing command is named" "cannot run" "$stderr"

touch "$work_dir/not-executable"
run "$work_dir/not-executable"
expect "a command that is not executable exits 126" status 126 "$status"

# A SIGTERM sent to the keepalive's process is passed on to the command.
"$keepalive" sh -c 'trap "exit 7" TERM; while :; do sleep 0.05; done' >/dev/null 2>&1 &
wrapper_pid=$!
sleep 0.5
kill -TERM "$wrapper_pid"
for _ in $(seq 50); do
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$wrapper_pid" 2>/dev/null; then
    pkill -KILL -P "$wrapper_pid" || true
    kill -KILL "$wrapper_pid"
    wait "$wrapper_pid" || true
    fail "SIGTERM is passed on to the command" "the command was still running 5 s later"
else
    set +e
    wait "$wrapper_pid"
    status=$?
    set -e
    expect "SIGTERM is passed on to the command" status 7 "$status"
fi

run
expect "without a command and without a GPU it exits 1" status 1 "$status"

if command -v shellcheck >/dev/null; then
    if shellcheck "$repo_dir"/tests/*.sh; then
        pass "shellcheck finds nothing in tests/"
    else
        fail "shellcheck finds nothing in tests/" "see its output above"
    fi
else
    if [[ -n "${CI:-}" ]]; then
        fail "shellcheck runs in CI" "it is not installed"
    else
        echo "skip  shellcheck is not installed"
    fi
fi

if [[ "$failures" -gt 0 ]]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
