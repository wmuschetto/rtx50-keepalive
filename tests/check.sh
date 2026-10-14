#!/usr/bin/env bash
# Usage: tests/check.sh
#
# Builds rtx50-keepalive with warnings as errors (with each of cc, gcc and
# clang that is installed; in CI, a missing one fails) and tests how it runs a
# command and handles signals, what make, make install and make uninstall do
# (in a folder under its work directory), and the CI workflow's token, pins
# and credentials.
# Needs no GPU: the Vulkan loader is pointed at a driver file that does not
# exist and kept from the drivers a loader settings file adds, so the
# keepalive fails to start, which is the path that must still run the command.
# One check names a FIFO as the driver file instead, to hold the keepalive in
# vkCreateInstance, and others preload a stand-in for the loader: one with one
# NVIDIA GPU, to fail each create in turn and then the first submit, count
# what is destroyed and check that the keepalive turns off implicit layers,
# and one with the devices a check lists, to see which GPU it opens. One
# check feeds the hold report fixed job times, another checks a
# hold's time against the clock, and one runs the job loop through a hold
# against stand-ins for the Vulkan calls and the clock. The helpers in tests/
# (device-list, empty-argv, failed-create, failed-fork, hangup, hold-report,
# hold-start, keepalive-loop, kill-log, slow-fork, unreaped-child) are built
# with the first compiler that built it, and the checks run that compiler's
# build.
# Exits 1 if any check fails.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d)"
# The keepalive a test runs in the background, while it runs. One with a
# command runs under setsid, so it leads a process group that holds its command
# too, even after a keepalive that died first. Background jobs ignore Ctrl-C,
# so an interrupted run kills that group on the way out.
wrapper_pid=""
clean_up() {
    if [[ -n "$wrapper_pid" ]]; then
        kill -KILL -- "-$wrapper_pid" 2>/dev/null || kill -KILL "$wrapper_pid" 2>/dev/null || true
    fi
    rm -rf "$work_dir"
}
trap clean_up EXIT
# LD_PRELOAD splits its value at spaces and colons, and a helper it cannot load
# is skipped, so its check would pass without testing anything.
if [[ "$work_dir" == *[[:space:]:]* ]]; then
    echo "FAIL  the work directory $work_dir has a space or colon, which LD_PRELOAD cannot carry; set TMPDIR to a folder without one"
    exit 1
fi
# The loader reads VK_DRIVER_FILES (1.3.207 and later) or VK_ICD_FILENAMES (older).
export VK_DRIVER_FILES="$work_dir/no-such-driver.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
# The loader also loads a loader settings file's additional_drivers (Vulkan
# Configurator writes them) despite VK_DRIVER_FILES; this filter (loader 1.3.234
# and later) keeps them out.
export VK_LOADER_DRIVERS_SELECT='*no-such-driver.json,*stalled-driver.json'
# The keepalive sets this for itself; a check expects the command not to see it.
unset VK_LOADER_LAYERS_DISABLE

failures=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1: $2"; failures=$((failures + 1)); }

# Compiles with warnings as errors: the compiler, then the rest of its arguments.
build() {
    local compiler="$1"
    shift
    "$compiler" -std=c17 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror "$@"
}

# The first compiler that built it also builds the test helpers, and its build is
# the one the checks run.
helper_compiler=""
for compiler in cc gcc clang; do
    if ! command -v "$compiler" >/dev/null; then
        if [[ -n "${CI:-}" ]]; then
            fail "builds with $compiler and -Werror" "$compiler is not installed"
        else
            echo "skip  $compiler is not installed"
        fi
    elif build "$compiler" -o "$work_dir/rtx50-keepalive-$compiler" "$repo_dir/rtx50-keepalive.c" -lvulkan; then
        pass "builds with $compiler and -Werror"
        helper_compiler="${helper_compiler:-$compiler}"
    else
        fail "builds with $compiler and -Werror" "see the compiler output above"
    fi
done
if [[ -z "$helper_compiler" ]]; then
    echo "FAIL  no compiler built it; nothing else can be tested"
    exit 1
fi
keepalive="$work_dir/rtx50-keepalive-$helper_compiler"

# Runs the keepalive with the given arguments; sets status, stdout and stderr.
run() {
    status=0
    "$keepalive" "$@" >"$work_dir/stdout" 2>"$work_dir/stderr" </dev/null || status=$?
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

for option in --help -h; do
    run "$option"
    expect "$option exits 0" status 0 "$status"
    expect_contains "$option prints the usage" "Usage: rtx50-keepalive COMMAND" "$stdout"
done

run --version
expect "--version exits 0" status 0 "$status"
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

run --
expect "-- without a command exits 2" status 2 "$status"
expect_contains "-- without a command prints the usage" "Usage: rtx50-keepalive COMMAND" "$stderr"

run sh -c 'echo "${VK_LOADER_LAYERS_DISABLE-unset}"'
expect "the command's environment is left as it was" stdout unset "$stdout"

# The keepalive raises its own soft limit; under a low one, the command must keep it.
low_limit=1024
if [[ "$(ulimit -Hn)" != unlimited && "$(ulimit -Hn)" -le "$low_limit" ]]; then
    low_limit="$(( $(ulimit -Hn) - 1 ))"
fi
output="$(ulimit -Sn "$low_limit" && "$keepalive" sh -c 'ulimit -Sn' 2>/dev/null)" || true
expect "the command's open-file limit is left as it was" stdout "$low_limit" "$output"

run sh -c 'echo ran'
expect "the command runs when the keepalive cannot start" stdout ran "$stdout"
expect_contains "the failed start is reported on stderr" "the keepalive is off" "$stderr"

# Under a hard open-file limit too low for its fences, the keepalive names the
# limit and stops before its first Vulkan call (the loader would fail
# vkCreateInstance here), and the command runs.
status=0
(ulimit -n "$low_limit" && exec "$keepalive" sh -c 'exit 5') >/dev/null 2>"$work_dir/stderr" || status=$?
limit_stderr="$(cat "$work_dir/stderr")"
if [[ "$limit_stderr" != *"the hard limit is $low_limit"* || "$limit_stderr" == *VkResult* ]]; then
    fail "a hard open-file limit too low stops the keepalive before Vulkan" "stderr was [$limit_stderr]"
else
    expect "a hard open-file limit too low stops the keepalive before Vulkan" status 5 "$status"
fi

# A failed create may leave a handle it never created in its output, and
# destroying that would crash the keepalive and orphan the command; what was
# created must still be destroyed, each object before its parent.
# failed-create.so stands in for the loader with one NVIDIA GPU, makes the
# create FAILED_CREATE names do that (for vkCreateFence, the second fence's),
# exits 66 when a call gets the handle, and exits 68 when an object is
# destroyed after its parent or left at exit.
# It exits 69 at vkCreateInstance when the keepalive has not set
# VK_LOADER_LAYERS_DISABLE to ~all~.
# The keepalive's report shows it reached that create: under a hard open-file
# limit too low for its fences it stops before any. VkResult -3 is the
# stand-in's; the loader itself fails vkCreateInstance with -9 here.
if build "$helper_compiler" -shared -fPIC -o "$work_dir/failed-create.so" "$repo_dir/tests/failed-create.c"; then
    for create in vkCreateInstance vkCreateDevice vkCreateBuffer vkAllocateMemory vkCreateCommandPool \
        vkCreateFence; do
        status=0
        env FAILED_CREATE="$create" LD_PRELOAD="$work_dir/failed-create.so" "$keepalive" sh -c 'exit 5' \
            >/dev/null 2>"$work_dir/stderr" || status=$?
        create_stderr="$(cat "$work_dir/stderr")"
        if [[ "$create_stderr" != *"$create failed (VkResult -3)"* ]]; then
            fail "after a failed $create, what was created is destroyed and the failed output is not used" \
                "the keepalive did not reach $create; stderr was [$create_stderr]"
        elif [[ "$status" != 5 ]]; then
            fail "after a failed $create, what was created is destroyed and the failed output is not used" \
                "status was [$status], expected [5]; stderr was [$create_stderr]"
        else
            pass "after a failed $create, what was created is destroyed and the failed output is not used"
        fi
    done
    # A keepalive that fails mid-run, as on a lost GPU, frees its device and
    # waits for its command. With every create done, the stand-in fails the
    # first submit with VK_ERROR_DEVICE_LOST; the command runs until the
    # keepalive reports that, or 5 s.
    status=0
    # shellcheck disable=SC2016,SC2094 # the inner sh expands $1, and polls the stderr file the keepalive writes
    env FAILED_CREATE=vkQueueSubmit LD_PRELOAD="$work_dir/failed-create.so" "$keepalive" \
        sh -c 'for _ in $(seq 500); do grep -q "vkQueueSubmit failed" "$1" && exit 5; sleep 0.01; done' \
        sh "$work_dir/stderr" >/dev/null 2>"$work_dir/stderr" || status=$?
    submit_stderr="$(cat "$work_dir/stderr")"
    if [[ "$submit_stderr" != *"vkQueueSubmit failed (VkResult -4)"* ]]; then
        fail "a keepalive that fails mid-run destroys what it created, and the command runs on" \
            "the keepalive did not reach vkQueueSubmit; stderr was [$submit_stderr]"
    elif [[ "$status" != 5 ]]; then
        fail "a keepalive that fails mid-run destroys what it created, and the command runs on" \
            "status was [$status], expected [5]; stderr was [$submit_stderr]"
    else
        pass "a keepalive that fails mid-run destroys what it created, and the command runs on"
    fi
else
    fail "after a failed create, what was created is destroyed and the failed output is not used" \
        "tests/failed-create.c did not build"
fi

# The keepalive opens the first discrete NVIDIA GPU, past an integrated GPU and
# an integrated NVIDIA GPU listed before it. device-list.so stands in for the
# loader with the devices DEVICES lists; its vkCreateDevice names the device it
# got and fails.
if build "$helper_compiler" -shared -fPIC -o "$work_dir/device-list.so" "$repo_dir/tests/device-list.c"; then
    env DEVICES=8086:integrated,10de:integrated,10de:discrete LD_PRELOAD="$work_dir/device-list.so" \
        "$keepalive" true >/dev/null 2>"$work_dir/stderr" || true
    expect_contains "the first discrete NVIDIA GPU is chosen" "vkCreateDevice got device 2" \
        "$(cat "$work_dir/stderr")"
    # On a machine whose one GPU is another vendor's, the keepalive says so and
    # makes no device on it (that would add a VkResult), and the command runs.
    status=0
    env DEVICES=8086:discrete LD_PRELOAD="$work_dir/device-list.so" \
        "$keepalive" sh -c 'exit 5' >/dev/null 2>"$work_dir/stderr" || status=$?
    no_gpu_stderr="$(cat "$work_dir/stderr")"
    if [[ "$no_gpu_stderr" != *"no discrete NVIDIA GPU among 1 Vulkan device ("* ||
        "$no_gpu_stderr" == *VkResult* ]]; then
        fail "another vendor's GPU is not used" "stderr was [$no_gpu_stderr]"
    else
        expect "another vendor's GPU is not used" status 5 "$status"
    fi
else
    fail "the first discrete NVIDIA GPU is chosen" "tests/device-list.c did not build"
fi

output="$(echo hello | "$keepalive" cat 2>/dev/null)" || true
expect "stdin reaches the command" stdout hello "$output"

run "$work_dir/no-such-command"
expect "a missing command exits 127" status 127 "$status"
expect_contains "a missing command is named" "cannot run $work_dir/no-such-command" "$stderr"

touch "$work_dir/not-executable"
run "$work_dir/not-executable"
expect "a command that is not executable exits 126" status 126 "$status"

# A launcher can leave SIGCHLD ignored, and an ignored SIGCHLD lets the kernel
# reap a child at once. slow-fork.so holds the keepalive for 300 ms after its
# fork, so the command exits before the signal handlers are in place.
status=0
build "$helper_compiler" -shared -fPIC -o "$work_dir/slow-fork.so" "$repo_dir/tests/slow-fork.c" -ldl &&
    env --ignore-signal=CHLD LD_PRELOAD="$work_dir/slow-fork.so" \
        "$keepalive" "$work_dir/no-such-command" >/dev/null 2>&1 || status=$?
expect "a quick command's exit status survives an inherited ignored SIGCHLD" status 127 "$status"

# A failed fork runs the command in the keepalive's place. The check needs the
# "fork failed" line, since a preload that cannot load is skipped.
if build "$helper_compiler" -shared -fPIC -o "$work_dir/failed-fork.so" "$repo_dir/tests/failed-fork.c"; then
    status=0
    env LD_PRELOAD="$work_dir/failed-fork.so" "$keepalive" sh -c 'exit 3' >/dev/null 2>"$work_dir/stderr" ||
        status=$?
    fork_stderr="$(cat "$work_dir/stderr")"
    if [[ "$fork_stderr" != *"fork failed"* ]]; then
        fail "a failed fork still runs the command" \
            "the keepalive did not report a failed fork; stderr was [$fork_stderr]"
    else
        expect "a failed fork still runs the command" status 3 "$status"
    fi
else
    fail "a failed fork still runs the command" "tests/failed-fork.c did not build"
fi

# Whether a signal mask from /proc/PID/status (hexadecimal) holds the named signal.
mask_has() {
    (( 16#$1 >> ($(kill -l "$2") - 1) & 1 ))
}

# Waits up to 5 s for the named file to hold a pid, which a check's command
# writes once its trap is set.
wait_for_pid_file() {
    for _ in $(seq 500); do
        if [[ -s "$1" ]]; then
            return 0
        fi
        sleep 0.01
    done
}

# Sends the named signal (TERM, INT, ...) to the keepalive running in the
# background as wrapper_pid and expects it to exit within 5 s with the given
# status; sets status. Then it kills the keepalive's process group, which ends
# a command left running.
expect_signal_passed_on() {
    local signal_name="$1" name="$2" wanted="$3" timed_out=0
    # A keepalive that exited before its command fails the check below
    # instead of stopping the script.
    kill -s "$signal_name" "$wrapper_pid" 2>/dev/null || true
    for _ in $(seq 50); do
        kill -0 "$wrapper_pid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$wrapper_pid" 2>/dev/null; then
        timed_out=1
    fi
    kill -KILL -- "-$wrapper_pid" 2>/dev/null || true
    status=0
    wait "$wrapper_pid" || status=$?
    wrapper_pid=""
    if [[ "$timed_out" -eq 1 ]]; then
        fail "$name" "the keepalive was still running 5 s later"
    else
        expect "$name" status "$wanted" "$status"
    fi
}

# Each stop signal sent to the keepalive's process is passed on to the command.
# A background job starts with SIGINT and SIGQUIT ignored, and a shell cannot
# trap a signal ignored on entry, so env gives them their default action back.
# The command writes its pid to descriptor 3 once its trap is set.
for signal_name in TERM INT HUP QUIT; do
    ready_file="$work_dir/ready-$signal_name.pid"
    setsid env --default-signal=INT,QUIT "$keepalive" \
        sh -c "trap 'exit 7' $signal_name; echo \$\$ >&3; while :; do sleep 0.05; done" \
        >/dev/null 2>&1 3>"$ready_file" &
    wrapper_pid=$!
    wait_for_pid_file "$ready_file"
    expect_signal_passed_on "$signal_name" "SIG$signal_name is passed on to the command" 7
done

# A stop signal sent before the handlers are in place is held until they are,
# then passed on: slow-fork.so holds the keepalive for 300 ms after its fork.
# The command writes its pid to descriptor 3 once it runs.
held_pid_file="$work_dir/held-signal-command.pid"
setsid env LD_PRELOAD="$work_dir/slow-fork.so" "$keepalive" \
    sh -c 'trap "exit 7" TERM; echo $$ >&3; while :; do sleep 0.05; done' \
    >/dev/null 2>&1 3>"$held_pid_file" &
wrapper_pid=$!
# The pid means the fork is done and the keepalive is in its 300 ms hold,
# unless the command took longer than that to start, which SigCgt shows.
wait_for_pid_file "$held_pid_file"
held_name="a SIGTERM sent before the handlers are in place is passed on"
caught_mask="$(awk '/^SigCgt:/ { print $2 }' "/proc/$wrapper_pid/status" 2>/dev/null)" || true
if [[ -n "$caught_mask" ]] && mask_has "$caught_mask" TERM; then
    kill -KILL -- "-$wrapper_pid" 2>/dev/null || true
    wait "$wrapper_pid" 2>/dev/null || true
    wrapper_pid=""
    fail "$held_name" "the handlers were in place first: the command took over slow-fork.so's 300 ms to start"
else
    expect_signal_passed_on TERM "$held_name" 7
fi

# The command gets the launcher's signal handling: an ignored SIGCHLD is
# restored after the fork, and the stop signals and the mask are left alone.
launcher_signals=(--ignore-signal=CHLD --ignore-signal=HUP --block-signal=USR1)
expected="$(env "${launcher_signals[@]}" cat /proc/self/status | grep -E '^Sig(Ign|Blk):')" || true
output="$(env "${launcher_signals[@]}" "$keepalive" cat /proc/self/status 2>/dev/null |
    grep -E '^Sig(Ign|Blk):')" || true
if [[ -z "$expected" ]]; then
    # --block-signal needs GNU coreutils 8.31 or later.
    fail "the command's ignored and blocked signals are the launcher's" "env cannot set them"
else
    expect "the command's ignored and blocked signals are the launcher's" output "$expected" "$output"
fi

# With a command, a stop signal ignored on entry is still caught and passed on,
# since a game sets its own handlers. A shell cannot trap a signal ignored on
# entry, so env gives the command's shell the default back.
ready_file="$work_dir/ready-ignored-TERM.pid"
setsid env --ignore-signal=TERM "$keepalive" \
    env --default-signal=TERM sh -c 'trap "exit 7" TERM; echo $$ >&3; while :; do sleep 0.05; done' \
    >/dev/null 2>&1 3>"$ready_file" &
wrapper_pid=$!
wait_for_pid_file "$ready_file"
expect_signal_passed_on TERM "with a command, a SIGTERM ignored on entry is passed on" 7

# Expects the hangup helper's run to have exited with the given status, and
# shows the run's stderr, from the given file, when it did not. Given the log
# kill-log.so wrote, it also expects the keepalive to have passed on no signal.
expect_hangup_run() {
    local name="$1" wanted="$2" stderr_file="$3" kill_log="${4:-}"
    if [[ "$status" != "$wanted" ]]; then
        fail "$name" "status was [$status], expected [$wanted]; stderr was [$(cat "$stderr_file" 2>/dev/null)]"
    elif [[ -z "$kill_log" ]]; then
        pass "$name"
    elif [[ ! -f "$kill_log" ]]; then
        fail "$name" "kill-log.so wrote no log"
    else
        expect "$name" "the kill() calls" "" "$(cat "$kill_log")"
    fi
}

# A terminal emulator runs the keepalive as the leader of a new session; when
# the window closes, the kernel sends SIGHUP to the leader alone.
# Each command writes a line to descriptor 3 once its trap is in place, and
# hangup waits for that line before the hangup or Ctrl-C. The keepalive forks
# only after blocking its stop signals, so the line also means it is ready.
status=0
build "$helper_compiler" -o "$work_dir/hangup" "$repo_dir/tests/hangup.c" &&
    "$work_dir/hangup" "$keepalive" sh -c 'trap "exit 9" HUP; echo >&3; while :; do sleep 0.05; done' \
        >/dev/null 2>"$work_dir/hangup.stderr" || status=$?
expect_hangup_run "a terminal's hangup is passed on to the command" 9 "$work_dir/hangup.stderr"

# The terminal's Ctrl-C goes to its whole foreground process group, the command
# included, so passing it on would deliver it twice. kill-log.so logs each
# kill() the keepalive makes.
status=0
build "$helper_compiler" -shared -fPIC -o "$work_dir/kill-log.so" "$repo_dir/tests/kill-log.c" -ldl &&
    "$work_dir/hangup" --interrupt \
        env LD_PRELOAD="$work_dir/kill-log.so" KILL_LOG="$work_dir/interrupt-kills.log" \
        "$keepalive" sh -c 'trap "exit 9" INT; echo >&3; while :; do sleep 0.05; done' \
        >/dev/null 2>"$work_dir/interrupt.stderr" || status=$?
expect_hangup_run "Ctrl-C is not passed on to the command" 9 "$work_dir/interrupt.stderr" \
    "$work_dir/interrupt-kills.log"

# When a shell leads the session, the hangup's SIGHUP ends the shell, and the
# kernel then sends SIGHUP to the shell's whole process group, the keepalive
# and its command included. hangup exits with the shell's 128+1 once they have
# all exited.
status=0
"$work_dir/hangup" sh -c '"$@"; exit' sh \
    env LD_PRELOAD="$work_dir/kill-log.so" KILL_LOG="$work_dir/shell-hangup-kills.log" \
    "$keepalive" sh -c 'trap "exit 9" HUP; echo >&3; while :; do sleep 0.05; done' \
    >/dev/null 2>"$work_dir/shell-hangup.stderr" || status=$?
expect_hangup_run "a hangup's SIGHUP is not passed on when a shell leads the session" 129 \
    "$work_dir/shell-hangup.stderr" "$work_dir/shell-hangup-kills.log"

# kill-log.so creates KILL_LOG when it loads, so as the library of a driver a
# loader settings file adds, it shows whether the loader loaded that driver.
# It checks the tests' own VK_LOADER_DRIVERS_SELECT filter above, which keeps
# them off a real GPU, and nothing in the keepalive.
mkdir -p "$work_dir/config/vulkan/loader_settings.d"
printf '{"file_format_version": "1.0.0", "ICD": {"library_path": "%s", "api_version": "1.0.0"}}\n' \
    "$work_dir/kill-log.so" >"$work_dir/settings-driver.json"
printf '{"file_format_version": "1.0.0", "settings_array": [{"additional_drivers": [{"path": "%s"}]}]}\n' \
    "$work_dir/settings-driver.json" >"$work_dir/config/vulkan/loader_settings.d/vk_loader_settings.json"
XDG_CONFIG_HOME="$work_dir/config" KILL_LOG="$work_dir/settings-driver.log" \
    "$keepalive" sh -c 'exit 0' >/dev/null 2>&1 || true
if [[ ! -f "$work_dir/kill-log.so" ]]; then
    fail "the tests keep a loader settings file's driver out" "kill-log.so did not build"
elif [[ -e "$work_dir/settings-driver.log" ]]; then
    fail "the tests keep a loader settings file's driver out" "the loader loaded $work_dir/settings-driver.json"
else
    pass "the tests keep a loader settings file's driver out"
fi

# The command's pid stays taken until the keepalive exits, so a stop signal
# passed on after the command exited cannot reach a process that took the pid
# over. unreaped-child.so exits 67 if the command was reaped before then.
status=0
build "$helper_compiler" -shared -fPIC -o "$work_dir/unreaped-child.so" "$repo_dir/tests/unreaped-child.c" &&
    env LD_PRELOAD="$work_dir/unreaped-child.so" "$keepalive" sh -c 'exit 5' >/dev/null 2>&1 || status=$?
expect "the command is not reaped before the keepalive exits" status 5 "$status"

run
expect "without a command and without a GPU it exits 1" status 1 "$status"

# A kernel without the exec change of Linux 5.18 (some older stable kernels
# have it) gives a program run with an empty argv argc 0 and its environment
# right after argv[0]; that environment is not a command.
if build "$helper_compiler" -o "$work_dir/empty-argv" "$repo_dir/tests/empty-argv.c" -lvulkan; then
    status=0
    output="$("$work_dir/empty-argv" 2>/dev/null)" || status=$?
    # Status 1 is the keepalive run alone, without a GPU; a crash prints nothing too.
    expect "an empty argv runs no command" "stdout and status" "[] 1" "[$output] $status"
else
    fail "an empty argv runs no command" "tests/empty-argv.c did not build"
fi

# The hold report comes from the keepalive's bookkeeping alone, which needs no
# GPU: hold-report feeds it job times against the 20 ms limit, and the times
# in tests/hold-report.c make three holds. A hold's time is masked, since it
# is the wall-clock time the hold began; hold-start checks it.
# -Wno-unused-function: the helper calls only part of the keepalive's source.
hold_report_wanted="rtx50-keepalive: TIME hold: 2 jobs late, longest fence 45.0 ms
rtx50-keepalive: TIME hold: 1 job late, longest fence 21.0 ms
rtx50-keepalive: TIME hold: 2 jobs late, longest fence 25.0 ms
rtx50-keepalive: stopped after 0 s; 8 jobs, 5 late in 3 holds, longest fence 45.0 ms"
if build "$helper_compiler" -Wno-unused-function \
    -o "$work_dir/hold-report" "$repo_dir/tests/hold-report.c" -lvulkan; then
    output="$("$work_dir/hold-report" 2>&1 | sed -E 's/[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/TIME/')" || true
    expect "consecutive late jobs make one hold, and the report counts each" stdout \
        "$hold_report_wanted" "$output"
else
    fail "consecutive late jobs make one hold, and the report counts each" \
        "tests/hold-report.c did not build"
fi

# A hold's time is when its first late job was submitted: hold-start checks
# that time against the clock.
status=0
if build "$helper_compiler" -Wno-unused-function \
    -o "$work_dir/hold-start" "$repo_dir/tests/hold-start.c" -lvulkan; then
    "$work_dir/hold-start" || status=$?
    expect "a hold's time is when its first late job was submitted" status 0 "$status"
else
    fail "a hold's time is when its first late job was submitted" "tests/hold-start.c did not build"
fi

# The job loop needs no GPU either: keepalive-loop runs it against stand-ins
# for the Vulkan calls and the clock, through a hold that fills all 2048 slots
# and on until the ring of slots has wrapped three times. It exits with a
# message on a fence reset, submitted or waited on out of turn.
loop_wanted="rtx50-keepalive: TIME hold: 2048 jobs late, longest fence 10350.0 ms
rtx50-keepalive: stopped after 31 s; 6144 jobs, 2048 late in 1 hold, longest fence 10350.0 ms"
if build "$helper_compiler" -Wno-unused-function \
    -o "$work_dir/keepalive-loop" "$repo_dir/tests/keepalive-loop.c" -lvulkan; then
    output="$("$work_dir/keepalive-loop" 2>&1 | sed -E 's/[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/TIME/')" || true
    expect "the job loop keeps submitting through a hold that fills every slot" stdout \
        "$loop_wanted" "$output"
else
    fail "the job loop keeps submitting through a hold that fills every slot" \
        "tests/keepalive-loop.c did not build"
fi

# Without a command, a stop signal ignored on entry stays ignored, as nohup and
# a shell's background jobs rely on. A FIFO as the driver file holds the
# keepalive in vkCreateInstance, after its signal handlers are set up: the
# loader waits to open it until a writer comes (Vulkan loader 1.4.341,
# measured 2026-10-14). env sets SIGHUP's ignored bit at exec, so the check
# also needs the handlers in place: SIGTERM is the one stop signal caught here,
# since a background job starts with SIGINT and SIGQUIT ignored.
mkfifo "$work_dir/stalled-driver.json"
VK_DRIVER_FILES="$work_dir/stalled-driver.json" VK_ICD_FILENAMES="$work_dir/stalled-driver.json" \
    env --ignore-signal=HUP "$keepalive" >/dev/null 2>"$work_dir/stderr" &
wrapper_pid=$!
# Up to 5 s, until SIGTERM is caught or the keepalive has exited.
for _ in $(seq 500); do
    ignored_mask="" caught_mask=""
    read -r ignored_mask caught_mask < <(awk '/^SigIgn:/ { ignored = $2 } /^SigCgt:/ { caught = $2 }
        END { print ignored, caught }' "/proc/$wrapper_pid/status" 2>/dev/null) || true
    if [[ -z "$caught_mask" ]] || mask_has "$caught_mask" TERM; then
        break
    fi
    sleep 0.01
done
kill -KILL "$wrapper_pid" 2>/dev/null || true
wait "$wrapper_pid" 2>/dev/null || true
wrapper_pid=""
if [[ -z "$caught_mask" ]]; then
    fail "without a command, a SIGHUP ignored on entry stays ignored" \
        "it exited before the check; its stderr was [$(cat "$work_dir/stderr")]"
elif ! mask_has "$caught_mask" TERM; then
    fail "without a command, a SIGHUP ignored on entry stays ignored" \
        "SigCgt was $caught_mask, without SIGTERM's bit: its signal handlers were not in place yet"
elif mask_has "$ignored_mask" HUP; then
    pass "without a command, a SIGHUP ignored on entry stays ignored"
else
    fail "without a command, a SIGHUP ignored on entry stays ignored" \
        "SigIgn was $ignored_mask, without SIGHUP's bit"
fi

# make install and uninstall refuse a PREFIX or DESTDIR with a space, which
# would split the path and copy or delete the wrong files.
for target in install uninstall; do
    status=0
    env -u MAKEFLAGS make -n --no-print-directory -C "$repo_dir" DESTDIR= "$target" PREFIX="$work_dir/a b" \
        >/dev/null 2>&1 || status=$?
    expect "make $target refuses a PREFIX with a space" status 2 "$status"
done

# make builds it, on a copy of the Makefile and the source. make install gives
# the binary mode 755, so under sudo only root can change it, and make
# uninstall removes it. PREFIX is given since one in the environment would
# move the binary.
stage="$work_dir/stage"
mkdir "$stage"
cp "$repo_dir/Makefile" "$repo_dir/rtx50-keepalive.c" "$stage/"
installed_binary="$stage/root/usr/local/bin/rtx50-keepalive"
make_in_stage() {
    env -u MAKEFLAGS make --no-print-directory -C "$stage" DESTDIR="$stage/root" PREFIX=/usr/local "$@" 2>&1
}
if output="$(make_in_stage CC="$helper_compiler" CFLAGS="-O2 -Werror")"; then
    pass "make builds it with -Werror"
else
    fail "make builds it with -Werror" "make printed: $output"
fi
# A umask that drops the group and others shows a mode the Makefile does not set.
output="$(umask 077 && make_in_stage install)" || true
expect "make install gives the binary mode 755" mode 755 \
    "$(stat -c %a "$installed_binary" 2>/dev/null || echo "none, make printed: $output")"
output="$(make_in_stage uninstall)" || true
if [[ -e "$installed_binary" ]]; then
    fail "make uninstall removes the binary" "it is still there, make printed: $output"
else
    pass "make uninstall removes the binary"
fi

# The workflow runs the code it tests with a token that can only read, actions
# pinned to a commit, and no credentials left in the checkout.
workflow="$repo_dir/.github/workflows/check.yml"
expect "the workflow's token can only read" permissions "  contents: read" \
    "$(awk '/^permissions:/ { block = 1; next } /^[^ #]/ { block = 0 } block && NF && !/^[[:space:]]*#/' "$workflow")"
expect "no job widens the workflow's token" "job permissions" "" \
    "$(grep -E '^[[:space:]]+permissions:' "$workflow" || true)"
expect "every action is pinned to a commit" "unpinned actions" "" \
    "$(grep -E '^[[:space:]-]*uses:' "$workflow" | grep -vE 'uses: [^@[:space:]]+@[0-9a-f]{40}([[:space:]]|$)' || true)"
expect "the checkout keeps no credentials" "persist-credentials: false lines" \
    "$(grep -c 'uses: actions/checkout@' "$workflow" || true)" \
    "$(grep -cE '^[[:space:]]+persist-credentials: false([[:space:]]|$)' "$workflow" || true)"

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
