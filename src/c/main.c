#include "comm.h"
#include "main_window.h"
#include <pebble.h>

/**
 * xDrip Pebble Watchface
 *
 * Displays blood glucose data from xDrip+ Android app.
 * Watchface announces its capabilities to xDrip, xDrip then sends that data back.
 *
 * This file handles app-level services (communication, timers, bluetooth).
 * UI/layout is in main_window.c.
 */

// Tick handler - called every minute
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    main_window_update_time();
}

// Bluetooth connection handler
static void bluetooth_callback(bool connected) {
    if (connected) {
        // Re-send capabilities on reconnect
        comm_send_capabilities();
    }
}

int main(void) {
    // Initialize communication
    comm_init();
    comm_set_data_callback(main_window_update_bg_data);

    // Create and show main window
    main_window_create();

    // Register tick handler (updates time every minute)
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

    // Register bluetooth handler (re-send capabilities on reconnect)
    connection_service_subscribe(
        (ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    battery_state_service_subscribe(main_window_battery_handler);

    // Send initial capabilities to xDrip
    comm_send_capabilities();

    // Main event loop
    app_event_loop();

    // Cleanup
    tick_timer_service_unsubscribe();
    connection_service_unsubscribe();
    comm_deinit();
    main_window_destroy();
}
