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

// Capability bits (what data the watchface wants).
#define CAP_BG (1 << 0)
