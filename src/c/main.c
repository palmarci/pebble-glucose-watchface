#include "comm.h"
#include "constants.h"
#include <pebble.h>

// Layout elements
static Window *s_main_window;
static TextLayer *s_bottom_bg_layer;
static TextLayer *s_bg_layer;
static TextLayer *s_delta_layer;
static TextLayer *s_time_ago_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static BitmapLayer *s_arrow_layer;
static GBitmap *s_arrow_bitmap;

// Text buffers
static char s_time_ago_buffer[8];
static char s_time_buffer[8];
static char s_date_buffer[16];

// Arrow resources
static const uint32_t ARROW_RESOURCES[] = {
    0,                            // ARROW_UNKNOWN - no image
    RESOURCE_ID_ARROW_UP_UP,      // ARROW_DOUBLE_UP
    RESOURCE_ID_ARROW_UP,         // ARROW_UP
    RESOURCE_ID_ARROW_UP_RIGHT,   // ARROW_UP_RIGHT
    RESOURCE_ID_ARROW_RIGHT,      // ARROW_RIGHT
    RESOURCE_ID_ARROW_DOWN_RIGHT, // ARROW_DOWN_RIGHT
    RESOURCE_ID_ARROW_DOWN,       // ARROW_DOWN
    RESOURCE_ID_ARROW_DOWN_DOWN   // ARROW_DOUBLE_DOWN
};

// Update the display with current data
static void update_display(void) {
    if (!comm_has_data()) {
        text_layer_set_text(s_bg_layer, "---");
        text_layer_set_text(s_delta_layer, "");
        text_layer_set_text(s_time_ago_layer, "");
        return;
    }

    // BG value - just display the string from xDrip
    text_layer_set_text(s_bg_layer, comm_get_bg_string());

    // Delta - just display the string from xDrip
    text_layer_set_text(s_delta_layer, comm_get_delta_string());

    // Time ago - we calculate this locally
    uint32_t timestamp = comm_get_timestamp();
    time_t now = time(NULL);
    int minutes_ago = (now - timestamp) / 60;
    if (minutes_ago < 60) {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dm", minutes_ago);
    } else {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dh", minutes_ago / 60);
    }
    text_layer_set_text(s_time_ago_layer, s_time_ago_buffer);

    // Arrow
    uint8_t arrow = comm_get_trend_arrow();
    if (s_arrow_bitmap) {
        gbitmap_destroy(s_arrow_bitmap);
        s_arrow_bitmap = NULL;
    }
    if (arrow > 0 && arrow < sizeof(ARROW_RESOURCES) / sizeof(ARROW_RESOURCES[0])) {
        s_arrow_bitmap = gbitmap_create_with_resource(ARROW_RESOURCES[arrow]);
        bitmap_layer_set_bitmap(s_arrow_layer, s_arrow_bitmap);
    } else {
        bitmap_layer_set_bitmap(s_arrow_layer, NULL);
    }
}

// Callback when new data arrives
static void data_received_callback(void) { update_display(); }

// Update current time display
static void update_time(void) {
    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);
    strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ? "%H:%M" : "%I:%M",
             tick_time);
    text_layer_set_text(s_time_layer, s_time_buffer);
    strftime(s_date_buffer, sizeof(s_date_buffer), "%a %d %b", tick_time);
    text_layer_set_text(s_date_layer, s_date_buffer);
}

// Tick handler - called every minute
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    // Only update display if we have data (don't overwrite with "---")
    if (comm_has_data()) {
        update_display();
    }
}

// Bluetooth connection handler
static void bluetooth_callback(bool connected) {
    if (connected) {
        // Re-send capabilities on reconnect
        comm_send_capabilities();
    }
}

// Window load - create UI
static void main_window_load(Window *window) {
    Layer *root_layer = window_get_root_layer(window);
    GRect bounds = layer_get_unobstructed_bounds(root_layer);

    // Background
    window_set_background_color(window, GColorWhite);

    // Calculate positions - simple centered layout
    int bg_y = bounds.size.h / 4;
    int time_y = bounds.size.h * 3 / 4;

    // Black background for bottom half
    // This layer is not for text, but TextLayer allows setting background color
    s_bottom_bg_layer = text_layer_create(GRect(0, bounds.size.h / 2, bounds.size.w, bounds.size.h / 2));
    text_layer_set_background_color(s_bottom_bg_layer, GColorBlack);
    layer_add_child(root_layer, text_layer_get_layer(s_bottom_bg_layer));

    // BG value - large, centered
    s_bg_layer = text_layer_create(GRect(0, -5, 95, 47));
    text_layer_set_background_color(s_bg_layer, GColorClear);
    text_layer_set_text_color(s_bg_layer, GColorBlack);
    text_layer_set_font(s_bg_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_bg_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_bg_layer));

    // Arrow - to the right of BG
    s_arrow_layer = bitmap_layer_create(GRect(85, -7, 78, 51));
    bitmap_layer_set_compositing_mode(s_arrow_layer, GCompOpSet);
    layer_add_child(root_layer, bitmap_layer_get_layer(s_arrow_layer));

    // Delta - below BG
    s_delta_layer = text_layer_create(GRect(0, 36, 143, 50));
    text_layer_set_background_color(s_delta_layer, GColorClear);
    text_layer_set_text_color(s_delta_layer, GColorBlack);
    text_layer_set_font(s_delta_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28));
    text_layer_set_text_alignment(s_delta_layer, GTextAlignmentRight);
    layer_add_child(root_layer, text_layer_get_layer(s_delta_layer));

    // Time ago - below BG, right side
    s_time_ago_layer = text_layer_create(GRect(104, 58, 40, 24));
    text_layer_set_background_color(s_time_ago_layer, GColorClear);
    text_layer_set_text_color(s_time_ago_layer, GColorBlack);
    text_layer_set_font(s_time_ago_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_time_ago_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_time_ago_layer));

    // Current time - bottom
    s_time_layer = text_layer_create(GRect(0, 82, 143, 44));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorWhite);
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_time_layer));

    // Date - below time
    s_date_layer = text_layer_create(GRect(0, 120, 143, 29));
    text_layer_set_background_color(s_date_layer, GColorClear);
    text_layer_set_text_color(s_date_layer, GColorWhite);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_date_layer));

    // Initial update
    update_time();
    // Restore data if available (window may be reloaded while app stays alive)
    update_display();
}

// Window unload - cleanup UI
static void main_window_unload(Window *window) {
    text_layer_destroy(s_bottom_bg_layer);
    text_layer_destroy(s_bg_layer);
    text_layer_destroy(s_delta_layer);
    text_layer_destroy(s_time_ago_layer);
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
    bitmap_layer_destroy(s_arrow_layer);
    if (s_arrow_bitmap) {
        gbitmap_destroy(s_arrow_bitmap);
    }
}

// App init
static void init(void) {
    // Initialize communication
    comm_init();
    comm_set_data_callback(data_received_callback);

    // Create main window
    s_main_window = window_create();
    window_set_window_handlers(
        s_main_window, (WindowHandlers){.load = main_window_load, .unload = main_window_unload});
    window_stack_push(s_main_window, true);

    // Register tick handler
    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

    // Register bluetooth handler
    connection_service_subscribe(
        (ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    // Send initial capabilities
    comm_send_capabilities();
}

// App deinit
static void deinit(void) {
    tick_timer_service_unsubscribe();
    connection_service_unsubscribe();
    comm_deinit();
    window_destroy(s_main_window);
}

// Main entry point
int main(void) {
    init();
    app_event_loop();
    deinit();
}
