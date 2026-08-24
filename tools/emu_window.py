#!/usr/bin/env python3
"""Print the X window id of the emulator display for <platform>.

Every QEMU window is titled just "QEMU", so with several emulators up there is nothing in the
window itself to tell them apart. The platform is in the qemu-pebble command line instead
(.../sdk-core/pebble/<platform>/qemu...), and the window carries its process in _NET_WM_PID, so
platform -> pid -> window is exact and survives resizing.
"""

import re
import subprocess
import sys

from mock_sender import sdk_platforms


def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout.strip()


def emulator_pids():
    """{platform: pid} for every running qemu-pebble."""
    out = {}
    for line in run("pgrep", "-af", "qemu-pebble").splitlines():
        pid, _, cmdline = line.partition(" ")
        match = re.search(r"/pebble/([a-z]+)/qemu", cmdline)
        if match:
            out[match.group(1)] = pid
    return out


def geometry(window_id):
    shell = dict(
        line.split("=") for line in run("xdotool", "getwindowgeometry", "--shell", window_id).splitlines()
    )
    return int(shell["WIDTH"]), int(shell["HEIGHT"])


def display_window(pid, native):
    """QEMU owns more than one window per emulator; the display is the one scaled uniformly from the
    platform's native size, whatever multiplier it currently sits at."""
    ids = run("xdotool", "search", "--pid", pid).split()
    if len(ids) == 1:
        return ids[0]
    for window_id in ids:
        width, height = geometry(window_id)
        if abs(width / native[0] - height / native[1]) < 0.01:
            return window_id
    sys.exit("no display window among %s for pid %s" % (", ".join(ids) or "none", pid))


if len(sys.argv) != 2:
    sys.exit("usage: emu_window.py <platform> | --running")

if sys.argv[1] == "--running":
    print(" ".join(sorted(emulator_pids())))
    raise SystemExit

platform = sys.argv[1]
sizes = sdk_platforms()
if platform not in sizes:
    sys.exit("unknown platform %r; known: %s" % (platform, ", ".join(sorted(sizes))))

running = emulator_pids()
if platform not in running:
    sys.exit("no %s emulator running%s" % (
        platform, " (running: %s)" % ", ".join(sorted(running)) if running else ""))
print(display_window(running[platform], sizes[platform]))
