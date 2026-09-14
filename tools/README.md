# mock_sender.py

Sends mock Pebble Glucose Protocol messages to the watchface, so rendering can be iterated on
without a phone, a pump, or a rebuild.

It drives the real receive path — a genuine AppMessage dict through `handle_dictionary` and
`parse_graph_blob` — via `pebble send-app-message`. Each invocation sends **one** message carrying
a complete snapshot: the watchface replaces its whole graph every time, it never appends.

## Emulator

    pebble build && pebble install --emulator flint     # once, and after every C change
    tools/mock_sender.py --list
    tools/mock_sender.py rise
    tools/mock_sender.py gap --screenshot /tmp/gap.png

The watchface has to be the app running on the emulator for the message to reach it.

## Real watch

Add `--phone`, which targets `127.0.0.1` over the USB tunnel (see the repo CLAUDE.md — don't chase
the phone's IP):

    adb forward tcp:9000 tcp:9000
    tools/mock_sender.py rise --phone

The watch has to be connected to the phone, with the Pebble app in Developer Connection.

## Overrides

Any preset takes `--bg`, `--iob`, `--status`, `--high`, `--low`:

    tools/mock_sender.py flat --bg 12.3 --status SUSPENDED --iob 0.0

`--high`/`--low` are in wire units (mg/dL / 2): 90 = 10.0 mmol/L, 36 = 4.0 mmol/L.

## Two things move on their own after a send

Point offsets are minutes relative to a fixed `ref_ts` and the age counter reads off
`s_bg_timestamp`, so the trace scrolls left and the reading goes stale by itself as real time
passes. Send once and leave it if that's what you want to look at.

## What it doesn't cover

- The outbound capability announce (`send_capability_announcement`). Nothing here replies to it; a `src/pkjs`
  mock sender would be needed, and only works on the emulator.
- Incremental graph updates. There is no such path in the watchface — `parse_graph_blob`
  overwrites `s_graph_count` and both arrays on every message.

## Presets

Authored in mmol/L and minutes-ago, converted to wire units at send time. `pack_graph` is written
independently of the C decoder on purpose: if the two disagree, the trace draws visibly wrong.
`--list` prints the current set with what each one exercises.
