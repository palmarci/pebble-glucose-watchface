#!/usr/bin/env python3
"""Send mock Pebble Glucose Protocol messages to the watchface.

Drives the real receive path (AppMessage dict -> new_data_callback -> parse_graph_blob), so
rendering can be iterated on without a phone, a pump, or a rebuild. Each invocation sends ONE
message carrying a complete snapshot: the watchface replaces its whole graph on every message,
it never appends.

    tools/mock_sender.py --list
    tools/mock_sender.py gap
    tools/mock_sender.py rise --phone
    tools/mock_sender.py flat --bg 12.3 --status SUSPENDED
    tools/mock_sender.py clip-high --screenshot /tmp/graph.png
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGE_JSON = os.path.join(HERE, os.pardir, "package.json")

# Protocol keys (keep in sync with src/c/protocol.h).
KEY_BG_TIMESTAMP = 10
KEY_BG_STRING = 11
KEY_IOB_STRING = 14
KEY_STATUS_STRING = 15
KEY_GRAPH_DATA = 17
KEY_GRAPH_HIGH_LINE = 18
KEY_GRAPH_LOW_LINE = 19

# Wire values are mg/dL / 2, so one wire unit is 2 mg/dL. Presets are authored in mmol/L because
# that's what the watch displays and what a reading looks like to a human.
MGDL_PER_MMOL = 18.018

# Must match GRAPH_GAP_THRESHOLD_MINUTES in main.c: points further apart than this draw as a break
# rather than a connected segment. One minute past it is the narrowest gap that still breaks.
GAP_THRESHOLD_MINUTES = 15
MIN_GAP = GAP_THRESHOLD_MINUTES + 1


def mmol_to_wire(mmol):
    """mmol/L -> the protocol's mg/dL / 2 byte. Out-of-byte values clamp; the watch clamps again
    to its own 2.2-16 mmol/L band (graph_y), which is what clip-high / clip-low exercise."""
    return max(0, min(255, round(mmol * MGDL_PER_MMOL / 2)))


def wire_to_mmol(wire):
    return wire * 2 / MGDL_PER_MMOL


def pack_graph(ref_ts, points):
    """[ref_ts u32 LE][count u16 LE][offset_min u16 LE x n][bg u8 x n]

    `points` is [(offset_minutes, wire_value)], oldest first — draw_trace and the slope estimator
    both assume ascending offsets. Written independently of the C decoder on purpose: if the two
    disagree the trace draws visibly wrong, which is a cheap check on the format itself.
    """
    blob = struct.pack("<IH", ref_ts, len(points))
    blob += b"".join(struct.pack("<H", off) for off, _ in points)
    blob += bytes(wire for _, wire in points)
    return blob


def ramp(start_mmol, end_mmol, minutes=120, step=5):
    """Points every `step` minutes over the last `minutes`, oldest first, linear start -> end.
    The newest point sits at 0 minutes ago so the reading reads as fresh."""
    ages = list(range(minutes, -1, -step))
    if not ages:
        return []
    span = max(1, len(ages) - 1)
    return [
        (age, start_mmol + (end_mmol - start_mmol) * i / span)
        for i, age in enumerate(ages)
    ]


def curve(keyframes, ts, window=120):
    """Sample a smooth curve through (t, mmol) keyframes at the times `ts`, where t counts minutes
    forward from the oldest point. Returns [(minutes_ago, mmol)].

    Smoothstep between keyframes rather than straight lines: a BG trace has no corners, and the
    eased ends make a keyframe-to-keyframe run read as a plateau instead of a ramp.
    """
    out = []
    for t in ts:
        for (t0, v0), (t1, v1) in zip(keyframes, keyframes[1:]):
            if t0 <= t <= t1:
                u = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
                out.append((window - t, v0 + (v1 - v0) * u * u * (3 - 2 * u)))
                break
    return out


def drop_between(points, oldest_age, newest_age):
    """Remove points whose age is inside [newest_age, oldest_age] — punches a sensor gap."""
    return [(age, v) for age, v in points if not (newest_age <= age <= oldest_age)]


# Each preset returns (points, overrides). Points are [(minutes_ago, mmol)] in any order; they get
# sorted oldest-first before packing.
DEFAULT_PRESET = "everything"

PRESETS = {
    "everything": (
        "the default: flat around 5, a drop to 2.8, then a rise to 16.0 at the top of the band — "
        "plus a status line and the narrowest possible gaps around a lone point in the flat "
        "stretch, so one send covers the trace, both target lines, the gap break, isolated-point "
        "rendering, the status overlay and the projection",
        # The lone point sits in the flat stretch, bracketed by the narrowest gaps that still
        # break, so the break is as easy to get wrong as it can be. That puts its two neighbours
        # off the 5-min grid, at exactly MIN_GAP either side. The drop and the rise keep their full
        # sampling, so they still draw as complete curves.
        lambda: (
            curve(
                [(0, 5.0), (50, 5.2), (75, 2.8), (120, 16.0)],
                [0, 5, 25 - MIN_GAP, 25, 25 + MIN_GAP] + list(range(45, 121, 5)),
            ),
            {"status": "SUSPENDED"},
        ),
    ),
    "flat": (
        "steady 7.0 for 2 h — baseline trace and a flat projection",
        lambda: (ramp(7.0, 7.0), {}),
    ),
    "rise": (
        "5.0 -> 11.0 over 2 h — projection angled up",
        lambda: (ramp(5.0, 11.0), {}),
    ),
    "fall": (
        "11.0 -> 4.0 over 2 h — projection angled down",
        lambda: (ramp(11.0, 4.0), {}),
    ),
    "clip-high": (
        "9.0 -> 19.0, above the 16 mmol/L top — graph_y clamp and a projection with no room",
        lambda: (ramp(9.0, 19.0), {}),
    ),
    "clip-low": (
        "6.0 -> 1.5, below the 2.2 mmol/L floor — graph_y clamp at the bottom edge",
        lambda: (ramp(6.0, 1.5), {}),
    ),
    "hypo": (
        "8.0 -> 3.1 with a low reading — status line and number at hypo range",
        lambda: (ramp(8.0, 3.1), {"iob": "0.0"}),
    ),
    "gap": (
        "40-min hole mid-window — should draw as a break, not a straight line "
        "(GRAPH_GAP_THRESHOLD_MINUTES)",
        lambda: (drop_between(ramp(6.0, 9.0), 70, 30), {}),
    ),
    "gap-recent": (
        "25 min between the last two points — projection suppressed (TREND_MAX_GAP_MINUTES)",
        lambda: (drop_between(ramp(6.0, 9.0), 20, 5), {}),
    ),
    "sparse": (
        "15-min spacing, exactly on the gap threshold — every segment should still connect",
        lambda: (ramp(5.5, 8.5, step=15), {}),
    ),
    "two-points": (
        "only two points — the minimum the slope estimator needs",
        lambda: ([(5, 7.0), (0, 7.6)], {}),
    ),
    "one-point": (
        "a single point — no slope, so no projection",
        lambda: ([(0, 7.2)], {}),
    ),
    "empty": (
        "count=0 blob — the no-graph path, with a BG number still shown",
        lambda: ([], {"bg": "7.4"}),
    ),
    "full-24h": (
        "288 points over 24 h — 2048-byte inbox, PERSIST_MAX_POINTS overflow; only the last 2 h "
        "is visible",
        lambda: (ramp(4.5, 12.0, minutes=1435, step=5), {}),
    ),
    "stale": (
        "readings stop 25 min ago — number blanks out and the age counter shows",
        lambda: (
            [(age + 25, v) for age, v in ramp(7.0, 6.4)],
            {},
        ),
    ),
    "suspended": (
        "status overlay instead of the graph",
        lambda: (ramp(7.0, 6.5), {"status": "SUSPENDED"}),
    ),
    "warmup": (
        "sensor warm-up status overlay",
        lambda: (ramp(7.0, 6.5), {"status": "WARMUP 42m"}),
    ),
}


def app_uuid():
    with open(PACKAGE_JSON) as f:
        return json.load(f)["pebble"]["uuid"]


def build_message(points, bg=None, iob="2.5", status="", high=90, low=36, now=None):
    """Turn preset output into the AppMessage fields. Returns (fields, blob).

    Timestamps are relative to now, so nothing is stale unless a preset means it to be.
    """
    now = int(time.time()) if now is None else now
    points = sorted(points, key=lambda p: -p[0])  # oldest first

    if points:
        oldest_age = points[0][0]
        ref_ts = now - oldest_age * 60
        wire_points = [(oldest_age - age, mmol_to_wire(mmol)) for age, mmol in points]
        newest_age, newest_wire = points[-1][0], wire_points[-1][1]
        # Report the newest graph point as the reading, so the big number and the trace agree.
        default_bg = "%.1f" % wire_to_mmol(newest_wire)
        bg_ts = now - newest_age * 60
    else:
        ref_ts = now
        wire_points = []
        default_bg = "0.0"
        bg_ts = now

    fields = {
        KEY_BG_TIMESTAMP: ("uint", bg_ts),
        KEY_BG_STRING: ("string", bg if bg is not None else default_bg),
        KEY_IOB_STRING: ("string", iob),
        KEY_STATUS_STRING: ("string", status),
        KEY_GRAPH_HIGH_LINE: ("uint", high),
        KEY_GRAPH_LOW_LINE: ("uint", low),
    }
    return fields, pack_graph(ref_ts, wire_points)


PLATFORMS = ("flint", "emery", "diorite", "chalk", "basalt", "aplite")
DEFAULT_PLATFORM = "flint"


def pebble_target(use_phone, platform):
    return ["--phone", "127.0.0.1"] if use_phone else ["--emulator", platform]


def send(fields, blob, use_phone, platform, verbose):
    """Shell out to `pebble send-app-message`. The blob goes via --bytes-file: a 24 h graph is
    ~900 bytes, well past a comfortable argv hex string."""
    cmd = ["pebble", "send-app-message"] + pebble_target(use_phone, platform)
    cmd += ["--app-uuid", app_uuid()]

    for kind in ("uint", "string"):
        pairs = ["%d=%s" % (k, v) for k, (t, v) in sorted(fields.items()) if t == kind]
        if pairs:
            cmd += ["--" + kind] + pairs

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blob)
        blob_path = f.name
    try:
        cmd += ["--bytes-file", "%d=%s" % (KEY_GRAPH_DATA, blob_path)]
        if verbose:
            print(" ".join(cmd))
        return subprocess.call(cmd)
    finally:
        os.unlink(blob_path)


def screenshot(path, use_phone, platform):
    cmd = ["pebble", "screenshot"] + pebble_target(use_phone, platform) + ["--no-open", path]
    return subprocess.call(cmd)


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("preset", nargs="?", default=DEFAULT_PRESET,
                   help="scenario to send (default: %s); see --list" % DEFAULT_PRESET)
    p.add_argument("-l", "--list", action="store_true", help="list presets and exit")
    p.add_argument("--phone", action="store_true",
                   help="send to the real watch via the adb tunnel (127.0.0.1) instead of the emulator")
    p.add_argument("--emulator", metavar="PLATFORM", default=DEFAULT_PLATFORM, choices=PLATFORMS,
                   help="emulator platform to target (default: %s); ignored with --phone"
                        % DEFAULT_PLATFORM)
    p.add_argument("--bg", help="override the BG string (default: newest graph point)")
    p.add_argument("--iob", help="override the IOB string")
    p.add_argument("--status", help="override the status string ('' shows the graph)")
    p.add_argument("--high", type=int, help="high target line, mg/dL / 2 (default 90 = 10.0 mmol/L)")
    p.add_argument("--low", type=int, help="low target line, mg/dL / 2 (default 36 = 4.0 mmol/L)")
    p.add_argument("--screenshot", metavar="PATH", help="grab a screenshot after sending")
    p.add_argument("-v", "--verbose", action="store_true", help="print the pebble command")
    args = p.parse_args()

    if args.list:
        width = max(len(n) for n in PRESETS)
        for name in PRESETS:
            print("  %-*s  %s" % (width, name, PRESETS[name][0]))
        return 0

    if args.preset not in PRESETS:
        print("unknown preset %r; --list to see them all" % args.preset, file=sys.stderr)
        return 2

    points, overrides = PRESETS[args.preset][1]()
    kwargs = dict(overrides)
    for name in ("bg", "iob", "status", "high", "low"):
        value = getattr(args, name)
        if value is not None:
            kwargs[name] = value

    fields, blob = build_message(points, **kwargs)
    print("%s: %d points, %d-byte blob, BG %s" % (
        args.preset, len(points), len(blob), fields[KEY_BG_STRING][1]))

    rc = send(fields, blob, args.phone, args.emulator, args.verbose)
    if rc != 0:
        return rc
    if args.screenshot:
        return screenshot(args.screenshot, args.phone, args.emulator)
    return 0


if __name__ == "__main__":
    sys.exit(main())
