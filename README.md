# Glucose — a Pebble watchface

Shows your current blood glucose, a two-hour graph with an extrapolated trend projection, insulin on
board, and a status line — plus the time and date.

The watchface is **source-agnostic**: it displays whatever a sender pushes to it over the
[Pebble Glucose Protocol](docs/PEBBLE_GLUCOSE_PROTOCOL.md), and doesn't know or care whether the
numbers came from a pump, a CGM, Nightscout or a file. It's also the protocol's reference
implementation.

## What it displays

| Field | Source | If the sender omits it |
|-------|--------|------------------------|
| BG value | `BG_STRING`, pre-formatted by the sender in its own units | blank until the first reading |
| Age of the reading | computed on-watch from `BG_TIMESTAMP` | hidden while under 6 minutes old |
| Insulin on board | `IOB_STRING` | the top-right corner stays empty |
| Status line | `STATUS_STRING` | no band is drawn, so the graph shows in full |
| Graph | `GRAPH_DATA`, plus `GRAPH_HIGH_LINE` / `GRAPH_LOW_LINE` | axes only |
| Trend projection | extrapolated on-watch from the last two graph points | nothing drawn |

The sender formats the values, so mmol/L and mg/dL both work with no setting on the watch. After 15
minutes with no fresh reading the BG and IOB blank rather than showing a stale number, and the
projection stops rather than extrapolating from old points.

Every field is optional, so a sender that only has a glucose value produces a clean
glucose-and-time watchface with an empty graph. Nothing needs disabling.

## Using it with your own data source

Write a sender that speaks [the protocol](docs/PEBBLE_GLUCOSE_PROTOCOL.md) and targets this
watchface's UUID, `567a3f6e-97d0-4f3a-b63f-916a8213d284`. The watchface announces which fields it
wants on launch and on every Bluetooth reconnect; the sender replies with those fields, and pushes
again whenever it has new data.

**Native Android senders need one extra step.** The Pebble/Core mobile app will not route
AppMessages from an Android package that isn't listed under `companionApp` in this watchface's
`package.json`, so a new native sender has to be added there — open an issue or a PR. Senders that
aren't native Android apps aren't affected.

Already declared, so they work without a new release: `com.mortenfyhn.minimedpebble`
(minimed-pebble-bridge) and `com.eveningoutpost.dexdrip` (xDrip+ — pre-declared in case someone
implements the protocol there, which is where this watchface's protocol started).

The reference sender is
[minimed-pebble-bridge](https://github.com/mortenfyhn/minimed-pebble-bridge), which reads a
Medtronic MiniMed 780G directly over Bluetooth, fully offline.

## Building

```sh
pebble build
pebble install --emulator flint
pebble screenshot --no-open --emulator flint
```

Targets `flint` (Pebble 2 Duo) and `emery` (the emulator). To render dummy data in the emulator,
uncomment `#define TEST_MODE` in `src/c/test_mode.h` and rebuild — passing `-DTEST_MODE` on the
`pebble build` command line does **not** work with this wscript.

## History

This repo's history contains two earlier lines of development, merged rather than squashed so both
stay intact: a hand-built xDrip reference watchface from January–February 2026, and the
MiniMed-specific watchface from July 2026 that replaced it. `git log --graph` shows the two roots
meeting.

## Licence

GPL-3.0.
