#!/usr/bin/env python3
"""Install the watchface on the real watch from the PC, no tapping in the Pebble app.

Goes through the phone's Developer Connection over the adb USB tunnel (the Pebble app must be
running with Developer Connection enabled and the watch connected). The .pbw does not need to be
copied to the phone.

Usage (from the repository root):
    tools/install_watchface.py              # installs build/pebble-glucose-watchface.pbw
    tools/install_watchface.py --build      # runs `pebble build` first
    tools/install_watchface.py path/to.pbw  # installs another bundle

The watchface replaces the installed copy and is started on the watch.
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PBW = os.path.join(ROOT, "build", "pebble-glucose-watchface.pbw")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pbw", nargs="?", default=DEFAULT_PBW, help="bundle to install")
    ap.add_argument("--build", action="store_true", help="run `pebble build` first")
    ap.add_argument("--phone", default="127.0.0.1",
                    help="phone address (default 127.0.0.1, the adb USB tunnel)")
    ap.add_argument("--no-forward", action="store_true", help="do not run adb forward")
    args = ap.parse_args()

    if args.build:
        if subprocess.run(["pebble", "build"], cwd=ROOT).returncode != 0:
            return 1
    if not os.path.exists(args.pbw):
        print("No such file: {} (run with --build)".format(args.pbw))
        return 1

    if not args.no_forward and args.phone == "127.0.0.1":
        subprocess.run(["adb", "forward", "tcp:9000", "tcp:9000"], check=True,
                       stdout=subprocess.DEVNULL)

    print("Installing {} ...".format(args.pbw))
    return subprocess.run(["pebble", "install", "--phone", args.phone, args.pbw]).returncode


if __name__ == "__main__":
    sys.exit(main())
