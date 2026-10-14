# rtx50-keepalive

A workaround for multi-second game freezes on NVIDIA RTX 50 series GPUs under
Linux. It runs beside a game and gives the GPU a tiny job every 5 ms. It draws
nothing and doesn't touch the game. You turn it on for a game by adding one word
to that game's launch options.

## Why it exists

On RTX 50 cards, some games freeze for 1 to 10 seconds and then carry on from
where they stopped. Sound keeps playing and the kernel log shows nothing. It is
tracked upstream as
[NVIDIA/open-gpu-kernel-modules#880](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/880)
(NVIDIA internal bug 5376205). It has been reported on every driver from the
570 series to 615.71.09, and as of October 2026 no driver fixes it.

Traces posted in that issue show where the time goes: the GPU finishes the
game's work, but the driver holds back the "work done" signal for seconds, and
the game waits for it. Users there report that other GPU activity ends a freeze
early: a video playing on a second monitor, or opening the desktop's overview.
This program supplies that activity without putting anything on screen.

To check whether your freezes are this bug, watch GPU utilization during one,
for example with `nvidia-smi --query-gpu=utilization.gpu --format=csv -lms 200`.
With this bug it drops to 0% for the length of the freeze, then goes straight
back to where it was.

## Results so far

On one PC (RTX 5090, driver 595.99.02, GNOME on Wayland, ARC Raiders through
Proton 11), logging GPU use every 200 ms:

| | Session | Freezes |
|---|---|---|
| Without rtx50-keepalive | 3 h 38 min | 3 |
| With rtx50-keepalive | 3 h 50 min | 0 |

That is one session each. At the first session's rate, a freeze-free session of
that length would happen by chance about 4% of the time, so more sessions and
more machines are needed. Results are welcome in #880.

## Requirements

- Linux with an NVIDIA GPU and NVIDIA's driver (proprietary or open kernel modules)
- A C compiler, `make`, and the Vulkan loader and headers:
  - Debian, Ubuntu: `sudo apt install build-essential libvulkan-dev`
  - Fedora: `sudo dnf install gcc make vulkan-loader-devel vulkan-headers`
  - Arch: `sudo pacman -S base-devel vulkan-icd-loader vulkan-headers`

## Install

From a copy of this repository:

```sh
make
sudo make install
```

This installs `/usr/local/bin/rtx50-keepalive`. To install somewhere else, set
`PREFIX`, e.g. `make install PREFIX=~/.local`, and then use the program's full
path in launch options, since a launcher's `PATH` may not include that folder.

To remove it: `sudo make uninstall`.

## Use

Steam: right-click the game, then Properties > General > Launch Options, and
enter:

```
rtx50-keepalive %command%
```

If the game already has launch options, put `rtx50-keepalive` right before
`%command%`, e.g. `SOME_SETTING=1 rtx50-keepalive %command%`.

Lutris: in the game's configuration, set System options > Command prefix (an
advanced option) to `rtx50-keepalive`. Other launchers that accept a wrapper
command or command prefix work the same way.

From a terminal: `rtx50-keepalive ./game --its-options`.

To turn it off for a game, remove `rtx50-keepalive` from its launch options.

## Checking that it runs

While the game runs, `pgrep -a rtx50-keepalive` lists it. It writes to stderr,
which Steam passes on to its own output, so start Steam from a terminal to see
its lines:

```
rtx50-keepalive: NVIDIA GeForce RTX 5090, a 4-byte fill every 5 ms; reporting fences over 20 ms
rtx50-keepalive: stopped after 13801 s; 2760290 jobs, 0 late in 0 holds, longest fence 6.3 ms
```

A line with `hold:` means the driver held back this program's own jobs too, and
gives the time it started. That is worth adding to #880.

If it cannot start (no NVIDIA GPU, no NVIDIA Vulkan driver), it says so with
"the keepalive is off" and the game runs anyway.

## What it costs

Measured on an RTX 5090 at the desktop: under 1% of one CPU core, 1-4% GPU
use, and about 15 W more power at idle. In a game the GPU already runs at full
power. It starts in about a second, alongside the game, and exits within about
0.3 s of the game.

## How it works

- It starts the game command as a child process, with the environment it was
  given, and opens its own Vulkan device on the first discrete NVIDIA GPU.
- Every 5 ms it submits a 4-byte `vkCmdFillBuffer` and waits up to 5 ms on that
  job's fence. Up to 2048 jobs can be in flight, so it keeps submitting through
  a 10 s hold.
- NVIDIA's driver opens a file for each fence, about 2,100 in all. It raises its
  own open-file soft limit for them, and leaves the game's alone.
- It turns off Vulkan layers for its own device (Steam's overlay, MangoHud), so
  its jobs go straight to the driver. This needs Vulkan loader 1.3.234 or later
  (Ubuntu 22.04 ships 1.3.204, which keeps the layers on). A layer that
  `VK_INSTANCE_LAYERS` or `VK_LOADER_LAYERS_ENABLE` asks for stays on, and
  `VK_LOADER_LAYERS_ALLOW` (loader 1.3.262 or later) exempts the layers it
  names.
- It passes SIGTERM, SIGINT, SIGHUP and SIGQUIT sent to it on to the game, and
  exits with the game's exit status.

## Anti-cheat

It doesn't read, inject into or trace the game, and it adds no Vulkan layer to
it. It is a separate process that starts the game and waits for it. No
anti-cheat vendor has reviewed it, so use it with online games at your own
judgment.

## Limitations

- It is a workaround, not a fix, and has been measured on one machine so far.
- It uses the first discrete NVIDIA GPU. On a machine with two, that may not be
  the one the game runs on.
- If a game's launcher starts the game and then exits, the keepalive stops with
  the launcher.

## Development

- `make check` builds with warnings as errors and runs the tests that need no
  GPU. CI runs it on every push.
- `make check-gpu` runs the keepalive on this machine's NVIDIA GPU for a few
  seconds at a time. Run it with no game running.

## License

MIT. See [LICENSE](LICENSE).
