#pragma once

#include <pebble.h>

/**
 * Communication module for xDrip Pebble Protocol.
 * Handles AppMessage setup, capability announcements, and data reception.
 */

// Initialize AppMessage and register callbacks
void comm_init(void);

// Deinitialize and cleanup
void comm_deinit(void);

// Send capability announcement to xDrip
void comm_send_capabilities(void);

// Data accessors (returns received values or defaults)
uint32_t comm_get_timestamp(void);
uint16_t comm_get_bg_mmol_x10(void);  // mmol/L * 10
uint8_t comm_get_trend_arrow(void);
int16_t comm_get_delta_mmol_x10(void);  // mmol/L * 10

// Check if we have received any data
bool comm_has_data(void);

// Set callback for when new data is received
typedef void (*CommDataCallback)(void);
void comm_set_data_callback(CommDataCallback callback);
