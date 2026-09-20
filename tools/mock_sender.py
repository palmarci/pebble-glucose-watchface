#!/usr/bin/env python3
"""Send mock Pebble Glucose Protocol messages to the watchface.

Drives the real receive path (AppMessage dict -> handle_dictionary -> parse_graph_blob), so
rendering can be iterated on without a phone, a pump, or a rebuild. Each invocation sends ONE
message carrying a complete snapshot: the watchface replaces its whole graph on every message,
it never appends.

    tools/mock_sender.py --list
    tools/mock_sender.py crowded
    tools/mock_sender.py showcase --phone
    tools/mock_sender.py showcase --bg 12.3 --status SUSPENDED
    tools/mock_sender.py crowded --screenshot /tmp/graph.png
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

# Display sizes come from the SDK's own platform table rather than a copy kept here: it lists every
# platform with its PBL_DISPLAY_WIDTH/HEIGHT, so a new one (gabbro was the last) needs no edit.
SDK_TOOLS = os.path.expanduser(
    "~/.pebble-sdk/SDKs/current/sdk-core/pebble/common/tools"
)


def sdk_platforms():
    """{platform: (width, height)} from pebble_sdk_platform.py."""
    sys.path.insert(0, SDK_TOOLS)
    try:
        import pebble_sdk_platform
    except ImportError:
        sys.exit(
            "can't read the SDK platform table at %s — is the Pebble SDK installed?"
            % SDK_TOOLS
        )
    finally:
        sys.path.pop(0)
    out = {}
    for name, platform in pebble_sdk_platform.pebble_platforms.items():
        sizes = dict(d.split("=") for d in platform["DEFINES"] if "=" in d)
        out[name] = (int(sizes["PBL_DISPLAY_WIDTH"]), int(sizes["PBL_DISPLAY_HEIGHT"]))
    return out


# Protocol keys (keep in sync with src/c/protocol.h).
KEY_BG_TIMESTAMP = 10
KEY_BG_STRING = 11
KEY_TREND_ARROW = 13
KEY_IOB_STRING = 14
KEY_STATUS_STRING = 15
KEY_STATUS_START = 17
KEY_STATUS_END = 18
KEY_PUMP_CONNECTED = 19
KEY_MEAL_CARBS = 20
KEY_MEAL_TIMESTAMP = 21
KEY_PREDICTED_BG = 1000
KEY_GRAPH_DATA = 30
KEY_GRAPH_HIGH_LINE = 31
KEY_GRAPH_LOW_LINE = 32

# Keep in sync with protocol.h's TREND_* constants.
TREND_ARROWS = {
    "unknown": 0,
    "flat": 1,
    "slant-up": 2,
    "slant-down": 3,
    "up": 4,
    "down": 5,
    "double-up": 6,
    "double-down": 7,
    "triple-up": 8,
    "triple-down": 9,
}

# Wire values are mg/dL / 2, so one wire unit is 2 mg/dL. Presets are authored in mmol/L because
# that's what the watch displays and what a reading looks like to a human.
MGDL_PER_MMOL = 18.018


def mmol_to_wire(mmol):
    """mmol/L -> the protocol's mg/dL / 2 byte. Out-of-byte values clamp; the watch clamps again
    to its own 2.2-16 mmol/L band (graph_y), which is what clip-high / clip-low exercise.
    """
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


def noisy(points, amplitude=0.12):
    """Add a fixed pseudo-random wobble (mmol/L) to `points`, so runs are repeatable."""
    out = []
    for i, (age, mmol) in enumerate(points):
        wobble = amplitude * ((i * 7919) % 13 - 6) / 6.0
        out.append((age, mmol + (0 if age == 0 else wobble)))  # the newest reading stays exact
    return out


# The day both showcase presets sample, as (minutes, mmol) keyframes: 5.4 up to 7.9, back down to a
# wandering 5.7-6.0 stretch, a rise to a 8.2 peak, then a slow fall. showcase shows the first two
# hours of it, showcase-full the two hours from 30 min in.
SHOWCASE_DAY = [
    (0, 5.4),
    (30, 7.9),
    (60, 6.0),
    (75, 5.7),
    (90, 6.0),
    (105, 5.8),
    (135, 8.0),
    (150, 8.2),
    (175, 6.9),
]

# Each preset returns (points, overrides). Points are [(minutes_ago, mmol)] in any order; they get
# sorted oldest-first before packing.
DEFAULT_PRESET = "showcase"

PRESETS = {
    "showcase": (
        "the typical view: an in-range day, fresh reading, no status",
        # For the README shot: nothing clipped, nothing stale, no status overlay, and enough shape
        # that the trace, both target lines and the projection are all visible at once.
        # The last keyframe sits past the end of the window (135 > 120) on purpose: smoothstep eases
        # into a keyframe, so ending on one would flatten the newest segment and leave the
        # projection almost horizontal. Cutting the sampling mid-rise keeps the slope steep.
        lambda: (
            curve(SHOWCASE_DAY, list(range(0, 121, 5))),
            {"iob": "1.4"},
        ),
    ),
    "showcase-full": (
        "every extra on screen: age counter and status strip",
        # The same day as showcase, 30 min further along: the window slides forward over the same
        # curve, so the two store shots read as one trace continuing rather than two unrelated days.
        # Sampling stops 7 min short of the window end, so the reading is stale and the age counter
        # shows.
        lambda: (
            curve(SHOWCASE_DAY, list(range(33, 144, 5)), window=150),
            {
                "iob": "1.4",
                "status": "SUSPENDED",
                "status_start": int(time.time() - 5 * 60),  # 5 min ago
            },
        ),
    ),
    # The realistic-* presets are typical days for design review across platforms. Keyframes get a
    # small deterministic wobble (sensor noise) so the trace does not look hand-drawn.
    "realistic-meal": (
        "after a meal: a rise from 5.8 to a 10.6 peak, still climbing, IOB on board",
        lambda: (
            noisy(curve([(0, 5.8), (25, 5.9), (55, 7.6), (85, 10.2), (120, 10.6)], list(range(0, 121, 5)))),
            {"iob": "3.8", "trend": "up"},
        ),
    ),
    "realistic-low": (
        "falling toward a low: 7.4 down to 3.7, IOB on board",
        lambda: (
            noisy(curve([(0, 7.4), (40, 6.6), (80, 4.9), (120, 3.7)], list(range(0, 121, 5)))),
            {"iob": "1.2", "trend": "down"},
        ),
    ),
    "realistic-overnight": (
        "overnight: steady 5-6 mmol/L, no IOB to speak of, flat",
        lambda: (
            noisy(curve([(0, 5.6), (50, 5.9), (100, 5.4), (120, 5.5)], list(range(0, 121, 5)))),
            {"iob": "0.4", "trend": "flat"},
        ),
    ),
    "realistic-high": (
        "high after a big meal: 13.9 and rising fast, double arrow, large IOB",
        lambda: (
            noisy(curve([(0, 9.1), (40, 11.0), (80, 13.0), (120, 13.9)], list(range(0, 121, 5)))),
            {"iob": "7.6", "trend": "double-up", "status": "SMARTGUARD"},
        ),
    ),
    "spike": (
        "7 two hours ago, a spike to 19 an hour ago, back to 8 now (unrealistic on purpose)",
        lambda: (
            noisy(curve([(0, 7.0), (60, 19.0), (120, 8.0)], list(range(0, 121, 5)))),
            {"iob": "2.4", "trend": "down"},
        ),
    ),
    "crowded": (
        "worst case for space: two-digit BG, age counter, IOB, status strip",
        # Deliberately the worst case for horizontal space: 10.0 is the widest the big number gets,
        # and it has the age counter on one side and IOB on the other. Sampling stops at t=110 of
        # the 120-minute window, so the reading is genuinely 10 min old and the age counter shows.
        lambda: (
            curve(
                [(0, 7.2), (60, 8.4), (150, 11.2)],
                list(range(0, 111, 5)),
            ),
            {
                "iob": "2.1",
                "status": "TEMP TARGET",
                "status_end": int(time.time()) + 60 * 60,  # 1 hour ahead
            },
        ),
    ),
}


def app_uuid():
    with open(PACKAGE_JSON) as f:
        return json.load(f)["pebble"]["uuid"]


def build_message(
    points,
    bg=None,
    iob="2.5",
    status="",
    status_start=0,
    status_end=0,
    high=90,
    low=36,
    now=None,
):
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
        KEY_STATUS_START: ("uint", status_start),
        KEY_STATUS_END: ("uint", status_end),
        KEY_GRAPH_HIGH_LINE: ("uint", high),
        KEY_GRAPH_LOW_LINE: ("uint", low),
    }

    return fields, pack_graph(ref_ts, wire_points)


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
    cmd = (
        ["pebble", "screenshot"]
        + pebble_target(use_phone, platform)
        + ["--no-open", path]
    )
    return subprocess.call(cmd)


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "preset",
        nargs="?",
        default=DEFAULT_PRESET,
        help="scenario to send (default: %s); see --list" % DEFAULT_PRESET,
    )
    p.add_argument("-l", "--list", action="store_true", help="list presets and exit")
    p.add_argument(
        "--phone",
        action="store_true",
        help="send to the real watch via the adb tunnel (127.0.0.1) instead of the emulator",
    )
    p.add_argument(
        "--emulator",
        metavar="PLATFORM",
        default=DEFAULT_PLATFORM,
        choices=sorted(sdk_platforms()),
        help="emulator platform to target (default: %s); ignored with --phone"
        % DEFAULT_PLATFORM,
    )
    p.add_argument("--bg", help="override the BG string (default: newest graph point)")
    p.add_argument("--iob", help="override the IOB string")
    p.add_argument("--status", help="override the status string ('' shows the graph)")
    p.add_argument(
        "--high",
        type=int,
        help="high target line, mg/dL / 2 (default 90 = 10.0 mmol/L)",
    )
    p.add_argument(
        "--low", type=int, help="low target line, mg/dL / 2 (default 36 = 4.0 mmol/L)"
    )
    p.add_argument(
        "--pump-connected",
        type=int,
        choices=[0, 1],
        help="send KEY_PUMP_CONNECTED (0=offline, 1=connected); omit to not send it at all",
    )
    p.add_argument(
        "--trend",
        choices=sorted(TREND_ARROWS),
        help="send KEY_TREND_ARROW; omit to not send it at all (the real sender omits it "
             "whenever the pump's reading has no trend field)",
    )
    p.add_argument(
        "--meal",
        type=int,
        metavar="GRAMS",
        help="send KEY_MEAL_CARBS/KEY_MEAL_TIMESTAMP with this many grams of carbs",
    )
    p.add_argument(
        "--meal-ago",
        type=int,
        default=45,
        metavar="MIN",
        help="how many minutes ago the meal was recorded (default 45; with --meal)",
    )
    p.add_argument(
        "--predicted",
        type=int,
        metavar="MGDL",
        help="send KEY_PREDICTED_BG: the sender's forecast 30 minutes ahead, mg/dL; omit to leave "
             "the projection to the watchface's own extrapolation",
    )
    p.add_argument(
        "--screenshot", metavar="PATH", help="grab a screenshot after sending"
    )
    p.add_argument(
        "-v", "--verbose", action="store_true", help="print the pebble command"
    )
    args = p.parse_args()

    if args.list:
        width = max(len(n) for n in PRESETS)
        for name in PRESETS:
            print("  %-*s  %s" % (width, name, PRESETS[name][0]))
        return 0

    if args.preset not in PRESETS:
        print(
            "unknown preset %r; --list to see them all" % args.preset, file=sys.stderr
        )
        return 2

    points, overrides = PRESETS[args.preset][1]()
    kwargs = dict(overrides)
    preset_trend = kwargs.pop("trend", None)  # a preset's own arrow, unless --trend overrides it
    for name in ("bg", "iob", "status", "high", "low"):
        value = getattr(args, name)
        if value is not None:
            kwargs[name] = value

    fields, blob = build_message(points, **kwargs)
    if args.pump_connected is not None:
        fields[KEY_PUMP_CONNECTED] = ("uint", args.pump_connected)
    if args.meal is not None:
        fields[KEY_MEAL_CARBS] = ("uint", args.meal)
        fields[KEY_MEAL_TIMESTAMP] = ("uint", int(time.time()) - args.meal_ago * 60)
    if args.predicted is not None:
        fields[KEY_PREDICTED_BG] = ("uint", args.predicted)
    trend = args.trend if args.trend is not None else preset_trend
    if trend is not None:
        fields[KEY_TREND_ARROW] = ("uint", TREND_ARROWS[trend])
    print(
        "%s: %d points, %d-byte blob, BG %s"
        % (args.preset, len(points), len(blob), fields[KEY_BG_STRING][1])
    )

    rc = send(fields, blob, args.phone, args.emulator, args.verbose)
    if rc != 0:
        return rc
    if args.screenshot:
        return screenshot(args.screenshot, args.phone, args.emulator)
    return 0


if __name__ == "__main__":
    sys.exit(main())
