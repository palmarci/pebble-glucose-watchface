#!/usr/bin/env python3
"""Screenshot the real watch through the phone (adb USB tunnel + Developer Connection).

    tools/watch_screenshot.py                 # /tmp/wf_screens/watch_<timestamp>.png (+ _3x)
    tools/watch_screenshot.py -o shot.png --scale 4
    tools/watch_screenshot.py --count 5 --every 60    # a series, one a minute

The watch has to be in NORMAL mode (a phone session), like `pebble logs`. Whatever is on screen is
captured, so this shows the watchface exactly as the pump firmware is driving it.
"""
import argparse
import os
import subprocess
import sys
import time


def shoot(path, scale):
    subprocess.run(["adb", "forward", "tcp:9000", "tcp:9000"], capture_output=True)
    r = subprocess.run(
        ["pebble", "screenshot", "--phone", "127.0.0.1", "--no-open", path],
        capture_output=True, text=True, timeout=120,
    )
    if r.returncode != 0 or not os.path.exists(path):
        print("screenshot failed (NORMAL mode? Developer Connection on?):\n" + r.stderr[-300:], file=sys.stderr)
        return False
    print(path)
    if scale > 1:
        try:
            from PIL import Image
            im = Image.open(path).convert("RGB")
            big = os.path.splitext(path)[0] + "_%dx.png" % scale
            im.resize((im.width * scale, im.height * scale), Image.NEAREST).save(big)
            print(big)
        except ImportError:
            pass
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", help="output file (a series appends _<n>)")
    ap.add_argument("--scale", type=int, default=3, help="also write an upscaled copy (1 = don't)")
    ap.add_argument("--count", type=int, default=1, help="number of screenshots")
    ap.add_argument("--every", type=float, default=60, help="seconds between screenshots of a series")
    args = ap.parse_args()

    os.makedirs("/tmp/wf_screens", exist_ok=True)
    base = args.out or time.strftime("/tmp/wf_screens/watch_%Y%m%d_%H%M%S.png")
    ok = True
    for n in range(args.count):
        path = base if args.count == 1 else "%s_%d.png" % (os.path.splitext(base)[0], n + 1)
        ok &= shoot(path, args.scale)
        if n + 1 < args.count:
            time.sleep(args.every)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
