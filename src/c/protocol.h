// MiniMed -> Pebble communication protocol constants.
//
// Raw integer AppMessage keys, shared verbatim with the Android bridge app
// (PebbleKit Android 2 sends these same integer keys). Kept compatible with the
// xDrip reference protocol so richer fields (delta/arrow/graph) can be added
// later without renumbering.

#pragma once

// Bump for breaking protocol changes.
#define PROTOCOL_VERSION 1

// Watch -> phone: capability announcement / "ready" ping.
#define KEY_PROTOCOL_VERSION 0
#define KEY_CAPABILITIES 1

// Phone -> watch: glucose data.
#define KEY_BG_TIMESTAMP 10 // uint32, UNIX epoch seconds of the reading
#define KEY_BG_STRING 11    // string, pre-formatted BG, e.g. "7.5"
#define KEY_IOB_STRING 18    // string, pre-formatted insulin-on-board, e.g. "2.5"
#define KEY_STATUS_STRING 19 // string, pump status e.g. "SUSPENDED"; "" = normal (SmartGuard on)

// Phone -> watch: 3-hour BG graph.
#define KEY_GRAPH_DATA 14       // byte array: [ref_ts u32 LE][count u16 LE][offset_min u16 LE ×n][bg u8 ×n]
#define KEY_GRAPH_HIGH_LINE 15  // uint8: high target line, mg/dL / 2 (e.g. 90 = 180 mg/dL = 10.0 mmol/L)
#define KEY_GRAPH_LOW_LINE 16   // uint8: low target line, mg/dL / 2 (e.g. 36 = 72 mg/dL = 4.0 mmol/L)

// Capability bits (what data the watchface wants).
#define CAP_BG (1 << 0)
#define CAP_IOB (1 << 4)
#define CAP_STATUS (1 << 5)
#define CAP_GRAPH (1 << 6)
