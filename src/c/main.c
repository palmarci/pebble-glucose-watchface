#include "main_window.h"
#include <pebble.h>

/**
 * xDrip Pebble Watchface
 *
 * Displays blood glucose data from xDrip+ Android app.
 * Uses capability-based protocol - watchface announces what data it wants,
 * xDrip sends pre-formatted strings (unit-agnostic).
 */

static void init(void) {
    main_window_push();
}

static void deinit(void) {
    main_window_destroy();
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
