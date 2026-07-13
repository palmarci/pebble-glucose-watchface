# Pebble Glucose Protocol v1 (draft)

A small protocol for pushing glucose data to a Pebble watchface over [AppMessage](https://developer.rebble.io/). It decouples the watchface from the data source. The watchface announces which data it can display, and the sender pushes only those.

- Sender: Anything with glucose data and a Pebble link, could be xDrip, a custom app, or something else.
- Watchface: Any Pebble watchface implementing this protocol.

Each watchface has a UUID, which the sender must target. To support arbitrary watchfaces the sender must let the user set the watchface UUID.

## Communication flow

1. On launch, and again on every Bluetooth reconnect, the watchface sends a capability
   announcement (`PROTOCOL_VERSION`, `CAPABILITIES`, and optionally `GRAPH_HOURS`).
2. The sender records the request and immediately pushes the latest values for the requested fields.
3. The sender pushes an update whenever it has new data.
4. The watchface can re-send its announcement any time to request a full refresh.

## Message keys: watchface → sender (capability announcement)

| Key | Name | Type | Description |
|----|------|------|-------------|
| 0 | PROTOCOL_VERSION | uint8 | Protocol version. 1 = v1. |
| 1 | CAPABILITIES | uint32 | Capability bitfield, see below. |
| 2 | GRAPH_HOURS | uint8 | Hours of graph history wanted. 0 = no graph at all. |

## Message keys: sender → watchface (data)

| Key | Name | Type | Description |
|----|------|------|-------------|
| 10 | BG_TIMESTAMP | uint32 | BG reading timestamp, as Unix epoch seconds. |
| 11 | BG_STRING | string | Formatted BG data in the sender's units, e.g. `"7.5"` or `"135"` |
| 12 | DELTA_STRING | string | Formatted BG change from previous reading, e.g. `"+0.3"`. |
| 13 | TREND_ARROW | uint8 | Trend arrow index, see below. |
| 14 | IOB_STRING | string | Formatted insulin-on-board, e.g. `"2.5"`. |
| 15 | STATUS_STRING | string | Any sensor/pump status text, e.g. `"SUSPENDED"`, `"NO SIGNAL"`, etc. |
| 16 | SENDER_BATTERY | uint8 | Sender battery level, 0–100. |
| 17 | GRAPH_DATA | bytes | Recent BG history for the graph (see format below). |
| 18 | GRAPH_HIGH_LINE | uint8 | High target line, **mg/dL ÷ 2** (e.g. 90 = 180 mg/dL = 10.0 mmol/L). |
| 19 | GRAPH_LOW_LINE | uint8 | Low target line, **mg/dL ÷ 2** (e.g. 36 = 72 mg/dL = 4.0 mmol/L). |

## Capability bits (CAPABILITIES, uint32)

| Bit | Mask | Field |
|----|------|-------|
| 0 | `0x01` | BG value + timestamp |
| 1 | `0x02` | Trend arrow |
| 2 | `0x04` | Delta |
| 3 | `0x08` | IOB |
| 4 | `0x10` | Status line |
| 5 | `0x20` | Sender battery |

## Trend arrow indices

| Index | Meaning |
|-------|---------|
| 0 | Unknown |
| 1 | Flat |
| 2 | Slant up |
| 3 | Slant down |
| 4 | Up |
| 5 | Down |
| 6 | Double up |
| 7 | Double down |
| 8 | Triple up |
| 9 | Triple down |

## Graph data format

Little-endian. `bg_values` are **mg/dL ÷ 2**, which fits 0–510 mg/dL (0–28 mmol/L) at 2 mg/dL
(≈0.1 mmol/L) resolution in one byte.

| Bytes | Field | Type | Description | Unit |
|-------|-------|------|-------------|------|
| 4 | ref_timestamp | uint32 | Unix epoch time of the reference (oldest) point | seconds |
| 2 | count | uint16 | Number of points, N | |
| 2N | offsets | uint16[N] | Time of each point since `ref_timestamp` | minutes |
| N | bg_values | uint8[N] | BG of each point | mg/dL ÷ 2 |

Total size: `6 + 3N` bytes (3 hours at 5 min intervals → 114 bytes).

## Notes for implementations

Sender:

- `BG_TIMESTAMP` should be the reading's measurement time, and should advance on each new reading even when the value is unchanged. A CGM "current value" read carries no time, so key it off a real new-reading signal, not off the value changing — otherwise a flat run of identical readings looks stale on the watch.
- Push updates as new data arrives if you can. Polling is fine if that's all the source allows.
- You can send a full snapshot or just the fields that changed; the watchface keeps the last value for any field not in a message. Send large fields like `GRAPH_DATA` only when they actually change.
- Respond to a capability announcement with an immediate push even if nothing changed, so a just-launched watchface fills in without waiting for the next reading.

Watchface:

- Decide staleness on the watchface side, not the sender: pick a threshold and show a clear "no data" state once the last `BG_TIMESTAMP` is older than it, so a frozen value can't look live.
- Treat each field as latest-wins and keep the last value for fields not in a given message. Persist across relaunch so returning from the menu isn't blank.

## Reference implementations

- **Watchface:** `minimed-pebble-watchface` (this repo; keys in `src/c/protocol.h`). Implements a subset:
  BG + timestamp, IOB, status, graph. Not delta, trend arrow, or phone battery.
- **Sender:** the MiniMed→Pebble bridge `minimed-pebble-bridge` (keys in `Protocol.kt`). Sends BG,
  timestamp, IOB, status, graph.
