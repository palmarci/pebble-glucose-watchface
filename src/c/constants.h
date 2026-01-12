#pragma once

/**
 * xDrip-Pebble Protocol constants.
 * Must match the Android side (PebbleConstants.java).
 *
 * Design: xDrip formats values (respecting user's unit preference),
 * watchface just displays strings. Graph data is always mg/dL integers.
 *
 * Protocol version is bumped only for breaking changes.
 * We define keys explicitly instead of using auto-generated MESSAGE_KEY_*
 * because the Pebble SDK adds a 10000 offset to auto-generated keys.
 */

#define PROTOCOL_VERSION 1

// Message keys for capability announcement (Pebble -> xDrip)
#define KEY_PROTOCOL_VERSION  0
#define KEY_CAPABILITIES      1
#define KEY_GRAPH_HOURS       2

// Message keys for watchface data (xDrip -> Pebble)
#define KEY_BG_TIMESTAMP      3  // UNIX epoch time [seconds]
#define KEY_BG_STRING         4  // Formatted BG value, e.g. "7.5" or "135"
#define KEY_ARROW_INDEX       5
#define KEY_DELTA_STRING      6  // Formatted delta, e.g. "+0.3" or "-5"
#define KEY_IOB_MILLIUNITS    7
#define KEY_PUMP_STATE        8
#define KEY_PHONE_BATTERY     9
#define KEY_PUMP_BATTERY      10
#define KEY_GRAPH_DATA        11  // Always mg/dL integers

// Capability bits (what data the watchface wants to receive)
#define CAP_BG            (1 << 0)
#define CAP_TREND_ARROW   (1 << 1)
#define CAP_DELTA         (1 << 2)
#define CAP_IOB           (1 << 3)
#define CAP_PUMP_STATE    (1 << 4)
#define CAP_PHONE_BATTERY (1 << 5)
#define CAP_PUMP_BATTERY  (1 << 6)
#define CAP_GRAPH         (1 << 7)

// Trend arrow values
#define ARROW_UNKNOWN     0
#define ARROW_DOUBLE_UP   1
#define ARROW_UP          2
#define ARROW_UP_RIGHT    3
#define ARROW_RIGHT       4
#define ARROW_DOWN_RIGHT  5
#define ARROW_DOWN        6
#define ARROW_DOUBLE_DOWN 7

// Default capabilities for PoC (BG, trend arrow, delta)
#define DEFAULT_CAPABILITIES (CAP_BG | CAP_TREND_ARROW | CAP_DELTA)
