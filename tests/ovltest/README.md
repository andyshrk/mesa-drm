# ovltest

`ovltest` is a DRM atomic KMS test program for connector, CRTC, and plane
testing. It is intended for display driver bring-up and debugging. Unlike a
general-purpose display utility, it deliberately exposes plane formats,
modifiers, source and destination rectangles, zpos, rotation, and raw buffer
files so that an overlay pipeline can be exercised in a controlled way.

The tool changes the state of the selected DRM device. Each test disables
planes, CRTCs, and connectors not selected by its command line, including
outputs enabled by other programs. Run it on a test system or through a remote
session where temporarily changing the display mode and output is acceptable.

## Features

* List DRM connectors, encoders, framebuffers, CRTCs, and planes.
* Set a connector and CRTC mode with an atomic request.
* Enable one or more overlay planes on one or more CRTCs.
* Specify source and destination plane rectangles.
* Load raw RGB and YUV image files into dumb buffers.
* Test format modifiers such as AFBC, RFBC, Rockchip tiled, and AFRC.
* Set plane properties such as zpos and rotation.
* Run all extracted commands from a trusted script in source order with `-S`.
* Select a random extracted command for every iteration with `-R`.
* Configure the delay between script tests with `-i`.
* Stop cleanly on SIGINT or SIGTERM and disable the active display state.

## Build

Build the tool with the repository's Meson build directory:

```sh
ninja -C Sbuild64/ tests/ovltest/ovltest
```

Build and run the host parser test with:

```sh
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror tests/ovltest/ovltest_script.c tests/ovltest/ovltest_script_test.c -o /tmp/ovltest_script_test
/tmp/ovltest_script_test
```

For a native Meson build, run the parser and internal state tests as:

```sh
meson test -C build/ ovltest-script ovltest-state --print-errorlogs
```

If the build directory is a cross build, such as `Sbuild64/` in this
repository, use the direct host compiler command above or run the generated
test binary on the target system.

The state test uses synthetic DRM resources and does not access display
hardware. It checks optional properties, failed mode preparation, writeback
mode ownership, missing object IDs, and interruptible delays.

## Query DRM Objects

Use `-p` to list CRTCs and planes:

```sh
ovltest -M rockchip -p
```

Other resource options are:

* `-c`: list connectors
* `-e`: list encoders
* `-f`: list framebuffers
* `-p`: list CRTCs and planes

Running `ovltest` without test options dumps all supported resource types.
Use `-D /dev/dri/cardN` to select a device explicitly or `-M <driver>` to
select a DRM driver.

The legacy `-w` option is parsed but does not apply arbitrary property
updates; do not rely on it to change DRM object properties. Script mode
rejects it.

## Run One Overlay Test

A single test normally needs:

* One `-s` option for a connector and CRTC mode.
* One or more `-P` options for planes.
* One or more comma-separated picture files with `-F`.

Example:

```sh
ovltest -M rockchip -s 209@95:800x1280 -P 124@95:800x1280@AB24 -F /data/800x1280_AB24.bin
```

The connector syntax is:

```text
<connector-id>@<crtc-id>:<mode>[@<format>]
```

The plane syntax is:

```text
<plane-id>@<crtc-id>:<width>x<height>[:<crtc-w>x<crtc-h>][+<x>+<y>][@<format>][@modifier...]
```

For example:

```sh
ovltest -M rockchip -s 209@95:800x1280 -P 124@95:1920x1080:800x600@NV12 -F /data/1920x1080_NV12.bin
```

This creates a 1920x1080 NV12 source buffer and displays it as an 800x600
destination rectangle on the selected CRTC.

`-F` accepts a comma-separated list. Files are assigned to `-P` planes in
command-line order:

```sh
ovltest -M rockchip -s 209@95:800x1280 -P 59@95:960x540:800x500@XB24 -P 124@95:1920x1080:800x600@AB24 -F /data/xb24.bin,/data/ab24.bin
```

Picture files contain raw image data. Their layout and size must match the
requested width, height, format, stride, and modifier.

## Script Loop Modes

The script modes parse a trusted shell script, extract its numeric-branch
`ovltest` commands, validate them, and run them inside one `ovltest` process.
They do not spawn a new `ovltest` process for every script command.

### Sequential Mode

Run commands in script source order:

```sh
ovltest -S /path/to/script.sh
```

After the last command, return to the first command and continue. Configure
the delay between tests with `-i`:

```sh
ovltest -S /path/to/script.sh -i 0.5
```

The default interval is two seconds. Intervals may be fractional and must
be finite values between zero and 2147483647 seconds, inclusive.

### Random Mode

Select one extracted command at random for every iteration:

```sh
ovltest -R /path/to/script.sh
```

The interval option is shared with sequential mode:

```sh
ovltest -R /path/to/script.sh -i 0.5
```

Random selection is independent for each iteration. A command may run twice
in a row, and another command may not run for a long time.

### Script Mode Behavior

* `-S` and `-R` are mutually exclusive.
* Script mode accepts only the script option and `-i` as outer command-line
  options. DRM test options come from commands inside the script.
* Every extracted command is parsed and validated before the DRM device is
  opened.
* All commands must resolve to the same `-D` device and `-M` module as the
  first command. Raw property updates with `-w` are not supported in script
  mode and are rejected during the startup checks.
* A command with planes must provide a readable picture file for every plane.
* All tests share one DRM device and one set of DRM resources.
* Each transition is one atomic request. Planes, CRTCs, and connectors not
  selected by the current command are disabled in that request.
* There is no separate clear-screen commit between script tests.
* The previous test's framebuffers and buffer objects remain alive until the
  replacing atomic commit succeeds.
* SIGINT and SIGTERM interrupt the inter-test delay immediately and prevent
  another test from starting. An in-progress operation finishes before exit.
  The final commit
  attempts to disable the active planes and modes before `ovltest` exits;
  failures to build or commit that cleanup request are reported.
* Preparation, buffer creation, property, or atomic commit failures terminate
  the loop with a nonzero status.

## Trusted Script Format

The script parser supports the fixed shell subset used by the existing overlay
test scripts:

* `NAME=value` assignments
* `$NAME` and `${NAME}` expansion
* Undefined variables expanding to an empty string
* Backtick command substitution for assignment values
* Top-level `echo`
* Numeric `if` and `elif` branches comparing `$1` with `=`
* Branch-local `echo`
* `dump_summary_later`
* Commands whose basename is `ovltest`
* Lines continued with a trailing backslash
* Top-level assignments, and only before the first conditional block

The parser ignores:

* `modetest` commands
* The no-argument branch
* `else` branches
* `dump_summary_later()` function definitions
* Other unsupported commands

Only backtick commands are passed to `/bin/sh`. The script as a whole is not
executed. Because backticks can run arbitrary shell commands, use only
trusted scripts.

The full [`minjiang_vop3_win.sh`][minjiang-script] script is a practical
reference:

[minjiang-script]: https://github.com/andyshrk/AndyHack/blob/master/tool/minjiang_vop3_win.sh

The following shape matches the supported format:

```sh
#!/bin/sh
CONN=`cat /sys/kernel/debug/dri/0/state | grep " DPI-1" | awk -F '[][]' '{print $2}'`
CRTC=`cat /sys/kernel/debug/dri/0/state | grep " video_port0" | awk -F '[][]' '{print $2}'`
C0=`cat /sys/kernel/debug/dri/0/state | grep " Cluster0-win0" | awk -F '[][]' '{print $2}'`
C1=`cat /sys/kernel/debug/dri/0/state | grep " Cluster1-win0" | awk -F '[][]' '{print $2}'`
MODE=800x1280

dump_summary_later()
{
    sleep "${1:-1}"
    cat /sys/kernel/debug/dri/0/summary
}

echo "Conn: $CONN"
echo "CRTC: $CRTC"

if [ $# -eq 0 ]; then
modetest -M rockchip -s $CONN@$CRTC:$MODE
elif [ "$1" = "1" ]; then
dump_summary_later
/data/ovltest -M rockchip -s $CONN@$CRTC:$MODE -P $C0@$CRTC:800x1280@AB24 -F /data/first.bin
elif [ "$1" = "2" ]; then
echo "second test"
/data/ovltest -M rockchip -s $CONN@$CRTC:$MODE -P $C1@$CRTC:1920x1080:800x600@NV12 -F /data/second.bin
else
/data/ovltest -M rockchip --ignored-by-parser
fi
```

For this example, sequential mode runs the branch-one command first, then the
branch-two command, then starts over at branch one. Random mode independently
chooses one of those two commands on every iteration.

Top-level `echo` output is printed once after the script is parsed. An echo
inside a numeric branch is printed whenever that branch's `ovltest` command
runs, whether it appears before or after the command. Echoes in ignored
branches are also ignored.

`dump_summary_later` is converted into a marker. After the command's atomic
commit succeeds, `ovltest` immediately reads:

```text
/sys/kernel/debug/dri/0/summary
```

The optional delay argument is accepted for compatibility with old scripts but
does not create a background timer. Failure to open or read the summary file
prints a warning and does not stop the loop.

## Parser Tests

Run the built-in parser tests without arguments:

```sh
/tmp/ovltest_script_test
```

Expected output:

```text
ovltest script tests: PASS
```

Pass a script path to validate and count its extracted commands:

```sh
/tmp/ovltest_script_test /path/to/script.sh
```

Example output:

```text
parsed 55 ovltest commands
```

The parser test does not open a DRM device. It does execute backtick command
substitutions while parsing a user-provided script.

For address and undefined-behavior checking:

```sh
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer tests/ovltest/ovltest_script.c tests/ovltest/ovltest_script_test.c -o /tmp/ovltest_script_test
ASAN_OPTIONS=detect_leaks=0 /tmp/ovltest_script_test
```

Leak detection can be disabled by the execution environment. Set
`detect_leaks=1` when the host supports it.

## Troubleshooting

### Permission denied

DRM mode setting normally requires appropriate permissions for the DRM device
and, for the script summary output, debugfs. Running as root on a test system
is the simplest option.

### Empty connector or plane IDs

Scripts commonly obtain connector, CRTC, and plane IDs by reading
`/sys/kernel/debug/dri/0/state`. If the `grep` pattern does not match the
target board, the variable expands to an empty string and the startup checks
report missing mode or object IDs. Inspect the state file and update the
script's ID queries for that board.

### Missing picture files

Script mode checks picture files before opening the DRM device. A missing or
unreadable file terminates startup with a nonzero status.

### Atomic commit failures

An invalid format, unsupported modifier, unavailable resource, or rejected
atomic request stops the loop. The failing test is not skipped because silent
skips would hide driver regressions.

### Script parse errors

The parser reports a source line for errors in supported constructs, such as
unterminated quotes or failed command substitution. Unsupported commands
are ignored; a script with no extracted `ovltest` commands is rejected.
