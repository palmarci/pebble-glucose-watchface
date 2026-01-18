#pragma once

/**
 * xDrip-Pebble Protocol constants.
 * Keys must match the Android/xDrip side.
 */

// Bump this for breaking changes
#define PROTOCOL_VERSION 1

// Message keys for capability announcement (Pebble -> xDrip)
#define KEY_PROTOCOL_VERSION 0
#define KEY_CAPABILITIES 1
#define KEY_GRAPH_HOURS 2

// Message keys for watchface data (xDrip -> Pebble)
#define KEY_BG_TIMESTAMP 10 // UNIX epoch time [seconds]
#define KEY_BG_STRING 11    // Formatted BG value, e.g. "7.5" or "135"
#define KEY_ARROW_INDEX 12
#define KEY_DELTA_STRING 13 // Formatted delta, e.g. "+0.3" or "-5"
#define KEY_GRAPH_DATA 15 // Always mg/dL integers

// Capability bits (what data the watchface wants to receive)
#define CAP_BG (1 << 0)
#define CAP_TREND_ARROW (1 << 1)
#define CAP_DELTA (1 << 2)
#define CAP_GRAPH (1 << 5)

// Trend arrow values
#define ARROW_UNKNOWN 0
#define ARROW_DOUBLE_UP 1
#define ARROW_UP 2
#define ARROW_UP_RIGHT 3
#define ARROW_RIGHT 4
#define ARROW_DOWN_RIGHT 5
#define ARROW_DOWN 6
#define ARROW_DOUBLE_DOWN 7
