#include "main_window.h"
#include "comm.h"
#include "constants.h"
#include <pebble.h>

// UI elements
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
    uint8_t arrow = comm_get_arrow_index();
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

// Window load - create UI
static void window_load(Window *window) {
    Layer *root_layer = window_get_root_layer(window);
    GRect bounds = layer_get_unobstructed_bounds(root_layer);

    // Background
    window_set_background_color(window, GColorWhite);

    // Black background for bottom half (TextLayer used purely for its background color)
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
static void window_unload(Window *window) {
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

void main_window_create(void) {
    s_main_window = window_create();
    window_set_window_handlers(
        s_main_window, (WindowHandlers){.load = window_load, .unload = window_unload});
    window_stack_push(s_main_window, true);
}

void main_window_destroy(void) { window_destroy(s_main_window); }

void main_window_update_bg_data(void) { update_display(); }

void main_window_update_time(void) {
    update_time();
    // Also update BG data time-ago if we have data
    if (comm_has_data()) {
        update_display();
    }
}
