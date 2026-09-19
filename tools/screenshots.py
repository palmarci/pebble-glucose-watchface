#!/usr/bin/env python3
"""Generate the review screenshots: every interesting scenario on the chosen emulator platforms.

    tools/screenshots.py                       # emery (the PT2), into /tmp/wf_screens/<timestamp>
    tools/screenshots.py --platforms all       # every platform the watchface targets
    tools/screenshots.py --platforms emery,aplite --out shots --scale 3
    tools/screenshots.py --only spike,no-data  # a subset of scenarios
    tools/screenshots.py --list

Each scenario is one mock_sender.py message (see its presets) on a freshly installed watchface, and
one `pebble screenshot`. With Pillow installed, a contact sheet per scenario (all platforms side by
side, upscaled) and one per platform (all scenarios) are written next to the images.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MOCK = os.path.join(HERE, "mock_sender.py")

ALL_PLATFORMS = ["emery", "basalt", "chalk", "diorite", "flint", "aplite", "gabbro"]
DEFAULT_PLATFORMS = ["emery"]  # the PT2; the rest are benchmarks only

# name -> (description, mock_sender arguments; None = send nothing)
SCENARIOS = {
    "no-data": ("fresh launch, nothing received yet", None),
    "showcase": ("typical in-range day", ["showcase", "--pump-connected", "1"]),
    "showcase-full": ("age counter and status strip", ["showcase-full", "--pump-connected", "1"]),
    "crowded": ("worst case for horizontal space", ["crowded", "--pump-connected", "1"]),
    "meal-rise": ("post-meal climb, carbs marker", ["realistic-meal", "--pump-connected", "1", "--meal", "45", "--meal-ago", "50"]),
    "falling-low": ("falling toward a low", ["realistic-low", "--pump-connected", "1"]),
    "overnight": ("steady overnight", ["realistic-overnight", "--pump-connected", "1"]),
    "high-double-up": ("high, rising fast, status", ["realistic-high", "--pump-connected", "1"]),
    "spike": ("7 -> 19 -> 8 over two hours", ["spike", "--pump-connected", "1"]),
    "pump-offline": ("pump link down", ["showcase", "--pump-connected", "0"]),
    "trend-triple-up": ("triple arrow up", ["realistic-high", "--pump-connected", "1", "--trend", "triple-up"]),
    "trend-triple-down": ("triple arrow down", ["realistic-low", "--pump-connected", "1", "--trend", "triple-down"]),
    "trend-slant-up": ("slanted arrow up", ["showcase", "--pump-connected", "1", "--trend", "slant-up"]),
    "trend-slant-down": ("slanted arrow down", ["showcase", "--pump-connected", "1", "--trend", "slant-down"]),
    "trend-flat": ("flat trend, no arrow row", ["showcase", "--pump-connected", "1", "--trend", "flat"]),
    "no-trend": ("reading without a trend", ["showcase", "--pump-connected", "1"]),
    "status-suspended": ("suspended status strip", ["showcase", "--pump-connected", "1", "--status", "SUSPENDED"]),
    "lo": ("LO string", ["realistic-low", "--pump-connected", "1", "--bg", "LO"]),
    "hi": ("HI string", ["realistic-high", "--pump-connected", "1", "--bg", "HI"]),
}


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def shoot(platform, out_dir, names, settle):
    done = []
    for name in names:
        args = SCENARIOS[name][1]
        # A fresh install per scenario, so nothing leaks from one to the next (the watchface keeps
        # no state across launches, and "no-data" must really be empty).
        run(["pebble", "kill"])
        r = run(["pebble", "install", "--emulator", platform])
        if r.returncode != 0:
            print("  install failed: %s %s\n%s" % (platform, name, r.stderr[-400:]), file=sys.stderr)
            continue
        time.sleep(settle)
        if args is not None:
            r = run([sys.executable, MOCK] + args + ["--emulator", platform])
            if r.returncode != 0:
                print("  send failed: %s %s\n%s" % (platform, name, r.stderr[-300:]), file=sys.stderr)
                continue
            time.sleep(1.5)
        path = os.path.join(out_dir, "%s_%s.png" % (platform, name))
        r = run(["pebble", "screenshot", "--emulator", platform, "--no-open", path])
        if r.returncode != 0:
            print("  screenshot failed: %s %s" % (platform, name), file=sys.stderr)
            continue
        print("  %s %s" % (platform, name))
        done.append(name)
    run(["pebble", "kill"])
    return done


def contact_sheets(out_dir, shots, scale):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow not installed; skipping contact sheets")
        return

    def sheet(items, path):
        ims = []
        for label, file in items:
            im = Image.open(file).convert("RGB")
            ims.append((label, im.resize((im.width * scale, im.height * scale), Image.NEAREST)))
        if not ims:
            return
        pad, head = 10, 18
        w = sum(im.width + pad for _, im in ims) + pad
        h = max(im.height for _, im in ims) + head + 2 * pad
        sheet_im = Image.new("RGB", (w, h), (60, 60, 60))
        draw = ImageDraw.Draw(sheet_im)
        x = pad
        for label, im in ims:
            draw.text((x, pad), label, fill=(255, 255, 255))
            sheet_im.paste(im, (x, pad + head))
            x += im.width + pad
        sheet_im.save(path)

    platforms = sorted({p for p, _ in shots})
    scenarios = sorted({s for _, s in shots})
    for s in scenarios:
        sheet([(p, os.path.join(out_dir, "%s_%s.png" % (p, s))) for p in platforms if (p, s) in shots],
              os.path.join(out_dir, "sheet_scenario_%s.png" % s))
    for p in platforms:
        sheet([(s, os.path.join(out_dir, "%s_%s.png" % (p, s))) for s in scenarios if (p, s) in shots],
              os.path.join(out_dir, "sheet_platform_%s.png" % p))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platforms", default=",".join(DEFAULT_PLATFORMS), help="comma list, or 'all'")
    ap.add_argument("--only", help="comma list of scenario names (default: all)")
    ap.add_argument("--out", help="output directory (default /tmp/wf_screens/<timestamp>)")
    ap.add_argument("--scale", type=int, default=2, help="contact sheet upscale factor")
    ap.add_argument("--settle", type=float, default=4.0, help="seconds to wait after each install")
    ap.add_argument("--no-build", action="store_true", help="skip `pebble build`")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    args = ap.parse_args()

    if args.list:
        for n, (d, _) in SCENARIOS.items():
            print("  %-18s %s" % (n, d))
        return 0

    platforms = ALL_PLATFORMS if args.platforms == "all" else args.platforms.split(",")
    names = args.only.split(",") if args.only else list(SCENARIOS)
    unknown = [n for n in names if n not in SCENARIOS] + [p for p in platforms if p not in ALL_PLATFORMS]
    if unknown:
        print("unknown: %s" % ", ".join(unknown), file=sys.stderr)
        return 2

    out_dir = args.out or time.strftime("/tmp/wf_screens/%Y%m%d_%H%M%S")
    os.makedirs(out_dir, exist_ok=True)
    if not args.no_build:
        r = run(["pebble", "build"])
        if r.returncode != 0:
            print(r.stdout[-800:] + r.stderr[-800:], file=sys.stderr)
            return 1

    shots = set()
    for p in platforms:
        print("== %s" % p)
        for n in shoot(p, out_dir, names, args.settle):
            shots.add((p, n))
    contact_sheets(out_dir, shots, args.scale)
    print("%d screenshots in %s" % (len(shots), out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
