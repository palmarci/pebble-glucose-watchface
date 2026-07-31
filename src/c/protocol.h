// Pebble Glucose Protocol — the keys this watchface uses.
//
// Raw integer AppMessage keys, shared verbatim with the sender (the Android bridge). This watchface
// implements a SUBSET of the protocol (BG + timestamp, IOB, status, graph); the full reference —
// including delta, trend arrow, and sender battery — is at
// https://github.com/mortenfyhn/pebble-glucose-protocol.

#pragma once

// Bump for breaking protocol changes.
#define PROTOCOL_VERSION 1

// Watch -> sender: capability announcement / "ready" ping.
#define KEY_PROTOCOL_VERSION 0
#define KEY_CAPABILITIES 1
#define KEY_GRAPH_HOURS 2 // hours of graph history wanted; 0 = no graph

// Sender -> watch: current reading.
#define KEY_BG_TIMESTAMP 10  // uint32, UNIX epoch seconds of the reading
#define KEY_BG_STRING 11     // string, pre-formatted BG, e.g. "7.5"
#define KEY_IOB_STRING 14    // string, pre-formatted insulin-on-board, e.g. "2.5"
#define KEY_STATUS_STRING 15 // string, status line e.g. "SUSPENDED"; "" = nothing to show

// Sender -> watch: BG graph.
#define KEY_GRAPH_DATA 17      // byte array: [ref_ts u32 LE][count u16 LE][offset_min u16 LE ×n][bg u8 ×n]
#define KEY_GRAPH_HIGH_LINE 18 // uint8: high target line, mg/dL / 2 (e.g. 90 = 180 mg/dL = 10.0 mmol/L)
#define KEY_GRAPH_LOW_LINE 19  // uint8: low target line, mg/dL / 2 (e.g. 36 = 72 mg/dL = 4.0 mmol/L)

// Capability bits (single-value fields this watchface wants). Graph is gated by KEY_GRAPH_HOURS.
#define CAP_BG (1 << 0)
#define CAP_IOB (1 << 3)
#define CAP_STATUS (1 << 4)
