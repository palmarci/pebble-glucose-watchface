// xDrip-Pebble communication protocol constants

#pragma once

// Bump for breaking protocol changes
#define PROTOCOL_VERSION 1

// Message keys: Pebble -> xDrip capability announcement
#define KEY_PROTOCOL_VERSION 0
#define KEY_CAPABILITIES 1
#define KEY_GRAPH_HOURS 2

// Message keys: xDrip -> Pebble watchface data
#define KEY_BG_TIMESTAMP 10 // UNIX epoch time [seconds]
#define KEY_BG_STRING 11    // Formatted BG value, e.g. "7.5" or "135"
#define KEY_DELTA_STRING 12 // Formatted delta, e.g. "+0.3" or "-5"
#define KEY_ARROW_INDEX 13
#define KEY_GRAPH_DATA 14      // Byte array: raw graph data
#define KEY_GRAPH_HIGH_LINE 15 // uint8: high BG threshold in mg/dL / 2
#define KEY_GRAPH_LOW_LINE 16  // uint8: low BG threshold in mg/dL / 2
#define KEY_PHONE_BATTERY 17   // uint8: phone battery level (0-100)

// Capability bits (what data the watchface wants to receive)
#define CAP_BG (1 << 0)
#define CAP_TREND_ARROW (1 << 1)
#define CAP_DELTA (1 << 2)
#define CAP_PHONE_BATTERY (1 << 3)
