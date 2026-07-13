# Pebble Glucose Protocol

**Protocol version 1** (in progress — not yet finalized).

A small, source-agnostic protocol for pushing glucose data to a Pebble watchface over
[AppMessage](https://developer.rebble.io/). It decouples the watchface from the data source: the
**watchface** announces which fields it can display, and any **sender** pushes only those.

- **Sender** — anything with glucose data and a Pebble link: a CGM/pump bridge (e.g. the MiniMed→Pebble
  bridge), an xDrip integration, a Nightscout bridge, a Dexcom/Libre app, … On Android, a PebbleKit app
  that sends to the watchface's UUID.
- **Watchface** — any Pebble watchface implementing this protocol.

Each watchface has its own UUID and the sender targets it; there is no shared hard-coded UUID, so
watchfaces can be published and installed independently.

This document defines the full protocol. An implementation only needs the subset it cares about — a
watchface requests the fields it can show, and a sender sends the fields it has that were requested.

## Communication flow

1. On launch, and again on every Bluetooth reconnect, the **watchface** sends a **capability
   announcement** (`PROTOCOL_VERSION`, `CAPABILITIES`, and optionally `GRAPH_HOURS`).
2. The **sender** records the request and immediately pushes the latest values for the requested fields.
3. The sender pushes an update whenever new data arrives.
4. The watchface may re-send its announcement at any time to force a fresh push.

## Message keys: watchface → sender (capability announcement)

| Key | Name | Type | Description |
|----|------|------|-------------|
| 0 | PROTOCOL_VERSION | uint8 | Protocol version (currently 1). |
| 1 | CAPABILITIES | uint32 | Bitfield of the single-value fields the watchface wants (see below). |
| 2 | GRAPH_HOURS | uint8 | Hours of graph history wanted. **0 = no graph.** Also gates whether the sender sends `GRAPH_*` at all. |

## Message keys: sender → watchface (data)

| Key | Name | Type | Description |
|----|------|------|-------------|
| 10 | BG_TIMESTAMP | uint32 | Reading time, Unix epoch **seconds** — the *measurement* time, not the send time (see best practices). |
| 11 | BG_STRING | string | Pre-formatted BG in the sender's units, e.g. `"7.5"` or `"135"`. Shown verbatim. |
| 12 | DELTA_STRING | string | Pre-formatted change vs the previous reading, e.g. `"+0.3"`. |
| 13 | TREND_ARROW | uint8 | Trend arrow (see indices below). |
| 14 | IOB_STRING | string | Pre-formatted insulin-on-board, e.g. `"2.5"`. |
| 15 | STATUS_STRING | string | Short status line, sender-defined wording, e.g. `"SUSPENDED"`, `"TEMP TARGET 0:09"`, `"NO SIGNAL"`. Empty/absent = nothing to show. |
| 16 | PHONE_BATTERY | uint8 | Sender/phone battery level, 0–100. |
| 17 | GRAPH_DATA | bytes | Recent BG history for the graph (see format below). |
| 18 | GRAPH_HIGH_LINE | uint8 | High target line, **mg/dL ÷ 2** (e.g. 90 = 180 mg/dL = 10.0 mmol/L). |
| 19 | GRAPH_LOW_LINE | uint8 | Low target line, **mg/dL ÷ 2** (e.g. 36 = 72 mg/dL = 4.0 mmol/L). |

Values are pre-formatted **strings** where a unit or wording choice exists (BG, delta, IOB, status): the
*sender* owns units, rounding, wording and localization; the watchface just renders. Numeric variants
can be added later as new keys + capability bits if a watchface needs to format or draw them itself.

## Capability bits (CAPABILITIES, uint32)

The watchface sets a bit per single-value field it wants. (The graph is gated by `GRAPH_HOURS`, not a
bit.)

| Bit | Mask | Field |
|----|------|-------|
| 0 | `0x01` | BG value + timestamp |
| 1 | `0x02` | Trend arrow |
| 2 | `0x04` | Delta |
| 3 | `0x08` | IOB |
| 4 | `0x10` | Status line |
| 5 | `0x20` | Phone battery |

## Trend arrow indices

A `uint8`, so there's ample room; senders map their device's arrows to the nearest value and watchfaces
render whatever subset they support. Not all devices use slanted arrows (e.g. some pumps only have
single/double/triple up/down).

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
| 4 | ref_timestamp | uint32 | Unix time of the reference (oldest) point | seconds |
| 2 | count | uint16 | Number of points, N | |
| 2N | offsets | uint16[N] | Time of each point since `ref_timestamp` | minutes |
| N | bg_values | uint8[N] | BG of each point | mg/dL ÷ 2 |

**Total size:** `6 + 3N` bytes (3 h at 5-min spacing → N=36 → 114 bytes).

How the graph is drawn — window, axes, dots vs lines, gap handling, tick marks — is up to the watchface.

## Best practices for implementations

**Sender**

- **Push on new data**, event-driven, rather than polling on a timer; an occasional keep-alive/fallback
  push is fine as a safety net.
- **`BG_TIMESTAMP` must be the measurement time**, and must advance on each genuinely new reading *even
  when the value is unchanged*. A CGM "current value" read carries no time, so if you only advance on a
  value change, a flat stretch of identical readings looks stale within minutes on the watch. Key the
  timestamp off a real "new reading" signal, not off the value changing.
- **One message per update, carrying all requested fields** — don't send a message per field. Dedupe by
  the full payload so you never resend a byte-identical message; status and IOB simply ride along with
  each update. (Because the timestamp/age and IOB usually change every reading, a changed status is
  effectively sent promptly without a dedicated status-change trigger.)
- **A capability announcement always triggers a fresh push**, even if the data is unchanged, so a
  just-relaunched watchface fills in immediately instead of waiting for the next reading.
- **Honor `CAPABILITIES` and `GRAPH_HOURS`** — send only what was requested; send no `GRAPH_*` when
  `GRAPH_HOURS` is 0 or absent.
- **Target the watchface's UUID** (configurable), since UUIDs are per-watchface.

**Watchface**

- Send the announcement on launch **and on every Bluetooth reconnect**.
- Own **staleness**: pick a threshold and show a clear "no data" state when the last `BG_TIMESTAMP` is
  older than it (don't let a frozen value look live).
- Treat every message as "latest wins"; persist the last values so a relaunch (e.g. returning from the
  menu) renders immediately rather than blank.

## Reference implementations

- **Watchface:** `minimed-pebble-watchface` (this repo; keys in `src/c/protocol.h`). Implements a subset:
  BG + timestamp, IOB, status, graph. Not delta, trend arrow, or phone battery.
- **Sender:** the MiniMed→Pebble bridge `minimed-pebble-bridge` (keys in `Protocol.kt`). Sends BG,
  timestamp, IOB, status, graph.
