# Pebble Glucose Protocol

**Protocol version 1** · status: *as-built* (documents the current implementation; see [Notes & possible tweaks](#notes--possible-tweaks)).

A small, source-agnostic protocol for pushing glucose data to a Pebble watchface over
[AppMessage](https://developer.rebble.io/developer.pebble.com/docs/c/Foundation/AppMessage/index.html).
It decouples the watchface from the data source: the **watchface** announces which fields it can
display, and any **sender** pushes only those. Neither side needs to know about the other's internals.

- **Sender** = anything that has glucose data and a Pebble link: a CGM/pump bridge (e.g. the MiniMed→Pebble
  bridge), an xDrip integration, a Nightscout bridge, a Dexcom/Libre app, … On Android this is a
  PebbleKit app that sends to the watchface's UUID.
- **Watchface** = any Pebble watchface implementing this protocol. This repo is the reference watchface.

There is no shared, hard-coded app UUID (unlike the legacy xDrip↔Pebble protocol): each watchface has
its own UUID, and the sender targets it. That's what lets watchfaces be distributed independently.

## Communication flow

1. Watchface launches — and again on every Bluetooth reconnect — and sends a **capability announcement**.
2. Sender stores the announced capabilities and immediately pushes the latest data for those fields.
3. Sender pushes an update whenever new data arrives.
4. The watchface can re-send the announcement at any time to force a fresh push.

All values are pre-formatted **strings** where practical (BG, IOB, status): the *sender* owns units,
rounding, wording and localization; the watchface is a dumb renderer. See the tradeoff in the notes.

## Message keys: watchface → sender (capability announcement)

| Key | Name | Type | Description |
|----|------|------|-------------|
| 0 | PROTOCOL_VERSION | uint8 | Protocol version (currently 1) |
| 1 | CAPABILITIES | uint32 | Bitfield of the fields the watchface wants (see below) |

## Message keys: sender → watchface (data)

| Key | Name | Type | Description |
|----|------|------|-------------|
| 10 | BG_TIMESTAMP | uint32 | Reading time, Unix epoch **seconds**. The watchface derives "time ago" and staleness from this, so it should be the *measurement* time, not the send time. |
| 11 | BG_STRING | string | Pre-formatted BG in the sender's chosen units, e.g. `"7.5"` or `"135"`. Displayed verbatim. |
| 18 | IOB_STRING | string | Pre-formatted insulin-on-board, e.g. `"2.5"`. The reference watchface appends `U`. |
| 19 | STATUS_STRING | string | Short pump/sensor status line, sender-defined wording, e.g. `"SUSPENDED"`, `"TEMP TARGET 0:09"`, `"NO SIGNAL"`. Empty or absent = nothing to show. |
| 14 | GRAPH_DATA | bytes | Recent BG history for the graph (see [format](#graph-data-format)). |
| 15 | GRAPH_HIGH_LINE | uint8 | High target line, **mg/dL ÷ 2** (e.g. 90 = 180 mg/dL = 10.0 mmol/L). |
| 16 | GRAPH_LOW_LINE | uint8 | Low target line, **mg/dL ÷ 2** (e.g. 36 = 72 mg/dL = 4.0 mmol/L). |

### Reserved / legacy keys (not used by the current reference implementation)

Carried over from the xDrip reference protocol; senders/watchfaces MAY implement them, but the current
reference watchface ignores them and the MiniMed bridge does not send them.

| Key | Name | Type | Description |
|----|------|------|-------------|
| 2 | GRAPH_HOURS | uint8 | Requested hours of graph history. *Currently unused* — graph is gated by the `CAP_GRAPH` bit and the reference watchface fixes the window at 3 h. |
| 12 | DELTA_STRING | string | Formatted change vs previous reading, e.g. `"+0.3"`. |
| 13 | ARROW_INDEX | uint8 | Trend arrow (see [indices](#trend-arrow-indices)). |
| 17 | PHONE_BATTERY | uint8 | Sender/phone battery level, 0–100. |

## Capability bits (CAPABILITIES, uint32)

The watchface sets a bit for each field it wants; the sender pushes only those.

| Bit | Mask | Capability | Status |
|----|------|------------|--------|
| 0 | `0x01` | BG value + timestamp | active |
| 1 | `0x02` | Trend arrow | reserved (legacy) |
| 2 | `0x04` | Delta | reserved (legacy) |
| 3 | `0x08` | Phone battery | reserved (legacy) |
| 4 | `0x10` | IOB | active |
| 5 | `0x20` | Status line | active |
| 6 | `0x40` | Graph | active |

## Graph data format

Little-endian. `bg_values` are sent as **mg/dL ÷ 2**, which fits 0–510 mg/dL (0–28 mmol/L) at 2 mg/dL
(≈0.1 mmol/L) resolution in one byte.

| Bytes | Field | Type | Description | Unit |
|-------|-------|------|-------------|------|
| 4 | ref_timestamp | uint32 | Unix time of the reference (oldest) point | seconds |
| 2 | count | uint16 | Number of points, N | |
| 2N | offsets | uint16[N] | Time of each point since `ref_timestamp` | minutes |
| N | bg_values | uint8[N] | BG of each point | mg/dL ÷ 2 |

**Total size:** `6 + 3N` bytes (3 h at 5-min spacing → N=36 → 114 bytes).

Rendering (window length, y-axis range, connecting points vs dots, breaking the line across sensor
gaps, hour tick marks, …) is entirely the watchface's business — the protocol only carries the points
and the two target lines. The reference watchface uses a fixed 2.2–16 mmol/L y-axis, breaks the trace
across gaps > 15 min, and draws hour ticks at each past hour.

### Trend arrow indices

For the reserved `ARROW_INDEX` field.

| Index | Meaning |
|-------|---------|
| 0 | Unknown |
| 1 | Double up |
| 2 | Up |
| 3 | Diagonal up |
| 4 | Flat |
| 5 | Diagonal down |
| 6 | Down |
| 7 | Double down |

## Notes & possible tweaks

Flagged for review — this section describes *questions*, not decided changes.

- **String vs numeric values.** BG/IOB/status are pre-formatted strings, so the sender owns units,
  rounding and wording and the watchface stays trivial. The alternative — numeric BG + coded status
  enums — would let watchfaces format/localize and draw status icons, at the cost of a heavier
  contract. Current choice favours simple watchfaces; worth deciding deliberately for a published spec.
- **Capability-bit numbering.** Active bits are 0 and 4–6, with 1–3 reserved for the legacy
  arrow/delta/battery fields. Fine for back-compat, but a clean v2 might renumber.
- **Graph gating.** The graph is gated by `CAP_GRAPH` (bit 6); the legacy `GRAPH_HOURS` key is unused.
  Decide whether to keep `GRAPH_HOURS` (so the watchface can request a window length) or drop it.
- **Timestamp semantics.** A CGM "current value" read often carries no measurement time. Senders should
  still make `BG_TIMESTAMP` reflect the real measurement time (e.g. advance it on a genuine new-reading
  event, not merely on value change) so "time ago"/staleness are correct on the watch.

## Reference implementations

- **Watchface:** this repository (`minimed-pebble-watchface`) — keys in `src/c/protocol.h`.
- **Sender:** the MiniMed→Pebble bridge (`minimed-pebble-bridge`) — keys in `Protocol.kt`.
