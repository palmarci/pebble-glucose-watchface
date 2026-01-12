#pragma once

#include <pebble.h>

/**
 * Communication module for xDrip Pebble Protocol.
 * Handles AppMessage setup, capability announcements, and data reception.
 *
 * BG and delta values are received as pre-formatted strings from xDrip.
 */

// Initialize AppMessage and register callbacks
void comm_init(void);

// Deinitialize and cleanup
void comm_deinit(void);

// Send capability announcement to xDrip
void comm_send_capabilities(void);

// Data accessors (returns received values or defaults)
uint32_t comm_get_timestamp(void);
const char* comm_get_bg_string(void);     // e.g., "7.5" or "135"
uint8_t comm_get_trend_arrow(void);
const char* comm_get_delta_string(void);  // e.g., "+0.3" or "-5"

// Check if we have received any data
bool comm_has_data(void);

// Set callback for when new data is received
typedef void (*CommDataCallback)(void);
void comm_set_data_callback(CommDataCallback callback);
