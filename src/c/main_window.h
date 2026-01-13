#pragma once

#include <pebble.h>

/**
 * Main window - displays BG data, trend arrow, delta, and time.
 */

/**
 * Create and push the main window onto the window stack.
 */
void main_window_create(void);

/**
 * Destroy the main window and clean up resources.
 */
void main_window_destroy(void);

/**
 * Update the BG data display (BG value, delta, time ago, arrow).
 * Called by main.c when new data arrives from xDrip.
 */
void main_window_update_bg_data(void);

/**
 * Update the time and date display.
 * Called by main.c on tick events.
 */
void main_window_update_time(void);

void main_window_battery_handler(BatteryChargeState charge_state);