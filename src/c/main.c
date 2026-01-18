// xDrip Pebble reference watchface
//
// This is a simple watchface created to serve as a reference for the new xDrip-Pebble communication
// protocol. It displays blood glucose, BG delta, time ago, trend arrow, and BG graph, in addition
// to the time and date.
//
// TODO see x for info about the protocol

#include "test_mode.h"
#include <pebble.h>

#define PROTOCOL_VERSION 1 // Bump for breaking protocol changes

// Message keys: Pebble -> xDrip capability announcement
#define KEY_PROTOCOL_VERSION 0
#define KEY_CAPABILITIES 1

// Message keys: xDrip -> Pebble watchface data
#define KEY_BG_TIMESTAMP 10 // UNIX epoch time [seconds]
#define KEY_BG_STRING 11    // Formatted BG value, e.g. "7.5" or "135"
#define KEY_DELTA_STRING 12 // Formatted delta, e.g. "+0.3" or "-5"
#define KEY_ARROW_INDEX 13

// Capability bits (what data the watchface wants to receive)
#define CAP_BG (1 << 0)
#define CAP_TREND_ARROW (1 << 1)
#define CAP_DELTA (1 << 2)
#define CAP_GRAPH (1 << 5)

// Trend arrow indices
#define ARROW_INDEX_UNKNOWN 0 // no image
#define ARROW_INDEX_UP_DOUBLE 1
#define ARROW_INDEX_UP 2
#define ARROW_INDEX_UP_SLANT 3
#define ARROW_INDEX_FLAT 4
#define ARROW_INDEX_DOWN_SLANT 5
#define ARROW_INDEX_DOWN 6
#define ARROW_INDEX_DOWN_DOUBLE 7

// Layout elements
static Window *s_window = NULL;
static TextLayer *s_bg_layer = NULL;
static TextLayer *s_delta_layer = NULL;
static TextLayer *s_time_ago_layer = NULL;
static TextLayer *s_time_layer = NULL;
static TextLayer *s_date_layer = NULL;
static BitmapLayer *s_arrow_layer = NULL;
static GBitmap *s_arrow_bitmap = NULL;

// TODO make a struct?
// Watchface data
static bool s_has_data = false; // TODO clarify that this is just "we have ever received data"
static uint32_t s_bg_timestamp = 0;
static char s_bg_string[5];    // Fits '10.0'
static char s_delta_string[6]; // Fits '+10.0'
static uint8_t s_arrow_index = ARROW_INDEX_UNKNOWN;
static char s_time_ago_buffer[4]; // Fits '59m'
static char s_time_buffer[6];     // Fits '20:23'
static char s_date_buffer[11];    // Fits 'Tue 13 Jan'

// Arrow resources
// TODO is there not a better way to map indices to resources?
static const uint32_t ARROW_RESOURCES[] = {0, // ARROW_INDEX_UNKNOWN - no image
                                           RESOURCE_ID_ARROW_UP_DOUBLE,
                                           RESOURCE_ID_ARROW_UP,
                                           RESOURCE_ID_ARROW_UP_SLANT,
                                           RESOURCE_ID_ARROW_FLAT,
                                           RESOURCE_ID_ARROW_DOWN_SLANT,
                                           RESOURCE_ID_ARROW_DOWN,
                                           RESOURCE_ID_ARROW_DOWN_DOUBLE};

// Helper functions
// TODO should this be static?
static char *safe_strncpy(char *dest, const char *src, size_t count) {
    strncpy(dest, src, count);
    dest[count - 1] = '\0';
    return dest;
}

// Update the display with current BG data
// todo rename to update xdrip data?
static void update_bg_data(void) {
    if (!s_has_data) {
        // TODO should these just set the values behind instead of write directly to the layers?
        text_layer_set_text(s_bg_layer, "---");
        text_layer_set_text(s_delta_layer, "---");
        text_layer_set_text(s_time_ago_layer, "---");
        return;
    }

    // BG value - just display the string from xDrip
    text_layer_set_text(s_bg_layer, s_bg_string);

    // Delta - just display the string from xDrip
    text_layer_set_text(s_delta_layer, s_delta_string);

    // Time ago - we calculate this locally
    uint32_t timestamp = s_bg_timestamp;
    time_t now = time(NULL);
    int minutes_ago = (now - timestamp) / 60;
    if (minutes_ago < 60) {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dm", minutes_ago);
    } else {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dh", minutes_ago / 60);
    }
    text_layer_set_text(s_time_ago_layer, s_time_ago_buffer);

    // Arrow
    uint8_t arrow_index = s_arrow_index;
    if (s_arrow_bitmap) {
        gbitmap_destroy(s_arrow_bitmap);
        s_arrow_bitmap = NULL;
    }
    if (arrow_index > 0 && arrow_index < sizeof(ARROW_RESOURCES) / sizeof(ARROW_RESOURCES[0])) {
        s_arrow_bitmap = gbitmap_create_with_resource(ARROW_RESOURCES[arrow_index]);
        bitmap_layer_set_bitmap(s_arrow_layer, s_arrow_bitmap);
    } else {
        bitmap_layer_set_bitmap(s_arrow_layer, NULL);
    }
}

// Update current time display
static void update_time_and_date(void) {
    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);
    strftime(s_time_buffer, sizeof(s_time_buffer), clock_is_24h_style() ? "%H:%M" : "%I:%M",
             tick_time);
    text_layer_set_text(s_time_layer, s_time_buffer);
    strftime(s_date_buffer, sizeof(s_date_buffer), "%a %d %b", tick_time);
    text_layer_set_text(s_date_layer, s_date_buffer);
}

static void window_load(Window *window) {
    Layer *root_layer = window_get_root_layer(window);

    // BG value - top, left
    s_bg_layer = text_layer_create(GRect(0, 0, PBL_DISPLAY_WIDTH - 30 - 10, 42));
    text_layer_set_background_color(s_bg_layer, GColorClear);
    text_layer_set_text_color(s_bg_layer, GColorBlack);
    text_layer_set_font(s_bg_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_bg_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_bg_layer));

    // Arrow - to the right of BG
    s_arrow_layer = bitmap_layer_create(GRect(PBL_DISPLAY_WIDTH - 30 - 10, 12, 30, 30));
    bitmap_layer_set_compositing_mode(s_arrow_layer, GCompOpSet);
    layer_add_child(root_layer, bitmap_layer_get_layer(s_arrow_layer));

    // Time ago - below BG, left
    s_time_ago_layer = text_layer_create(GRect(10, 42, 50, 42));
    text_layer_set_background_color(s_time_ago_layer, GColorClear);
    text_layer_set_text_color(s_time_ago_layer, GColorBlack);
    text_layer_set_font(s_time_ago_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_time_ago_layer, GTextAlignmentLeft);
    layer_add_child(root_layer, text_layer_get_layer(s_time_ago_layer));

    // Delta - below BG, right
    s_delta_layer = text_layer_create(GRect(PBL_DISPLAY_WIDTH - 50 - 10, 42, 50, 42));
    text_layer_set_background_color(s_delta_layer, GColorClear);
    text_layer_set_text_color(s_delta_layer, GColorBlack);
    text_layer_set_font(s_delta_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_delta_layer, GTextAlignmentRight);
    layer_add_child(root_layer, text_layer_get_layer(s_delta_layer));

    // Current time - bottom, centered
    s_time_layer = text_layer_create(GRect(0, 82, PBL_DISPLAY_WIDTH, 42));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorBlack);
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_time_layer));

    // Date - below time
    s_date_layer = text_layer_create(GRect(0, 126, PBL_DISPLAY_WIDTH, 24));
    text_layer_set_background_color(s_date_layer, GColorClear);
    text_layer_set_text_color(s_date_layer, GColorBlack);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_date_layer));

    // Initial update
    update_time_and_date();
    update_bg_data();
}

// Window unload - cleanup UI
static void window_unload(Window *window) {
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

void main_window_update_time(struct tm *tick_time, TimeUnits units_changed) {
    update_time_and_date();

    // Also update BG data time-ago if we have data
    // TODO split out update time ago specifically?
    if (s_has_data) {
        update_bg_data();
    }
}

// AppMessage callbacks
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
    // Check for timestamp (always present in data messages)
    Tuple *timestamp_tuple = dict_find(iter, KEY_BG_TIMESTAMP);
    if (timestamp_tuple) {
        s_bg_timestamp = timestamp_tuple->value->uint32;
        s_has_data = true;

        // BG as string
        Tuple *bg_tuple = dict_find(iter, KEY_BG_STRING);
        if (bg_tuple) {
            safe_strncpy(s_bg_string, bg_tuple->value->cstring, sizeof(s_bg_string));
        }

        // Trend arrow
        Tuple *arrow_tuple = dict_find(iter, KEY_ARROW_INDEX);
        if (arrow_tuple) {
            s_arrow_index = arrow_tuple->value->uint8;
        }

        // Delta as string
        Tuple *delta_tuple = dict_find(iter, KEY_DELTA_STRING);
        if (delta_tuple) {
            safe_strncpy(s_delta_string, delta_tuple->value->cstring, sizeof(s_delta_string));
        }

        update_bg_data();

        APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s, arrow: %d, delta: %s", s_bg_string,
                s_arrow_index, s_delta_string);
    }
}

void send_capability_announcement(void) {
    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);

    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to begin outbox: %d", result);
        return;
    }

    dict_write_uint8(iter, KEY_PROTOCOL_VERSION, PROTOCOL_VERSION);
    const uint32_t capabilities = CAP_BG | CAP_TREND_ARROW | CAP_DELTA;
    dict_write_uint32(iter, KEY_CAPABILITIES, capabilities);

    result = app_message_outbox_send();
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send capabilities: %d", result);
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "Sent capability announcement");
    }
}

static void bluetooth_callback(bool connected) {
    if (connected) {
        // Re-send capabilities on reconnect
        send_capability_announcement();
    }
}

void init(void) {
    // Register callbacks
    app_message_register_inbox_received(inbox_received_handler);

    // Inbox needs to be large enough for string data
    // Outbox only needs enough for capability announcement
    // TODO check dictionary
    app_message_open(/*inbox_size*/ 256, /*outbox_size*/ 64);

#ifdef TEST_MODE
    // Populate test data for emulator testing
    s_bg_timestamp = time(NULL) - TEST_MINUTES_AGO * 60;
    safe_strncpy(s_bg_string, TEST_BG_STRING, sizeof(s_bg_string));
    s_arrow_index = TEST_ARROW_INDEX;
    safe_strncpy(s_delta_string, TEST_DELTA_STRING, sizeof(s_delta_string));
    s_has_data = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "Test mode: populated sample data");
#endif

    s_window = window_create();
    window_set_window_handlers(s_window,
                               (WindowHandlers){.load = window_load, .unload = window_unload});
    window_stack_push(s_window, true);

    // Register tick handler (updates time every minute)
    tick_timer_service_subscribe(MINUTE_UNIT, main_window_update_time);

    // Register bluetooth handler (re-send capabilities on reconnect)
    connection_service_subscribe(
        (ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    // Send initial capabilities to xDrip
    send_capability_announcement();
}

void deinit(void) {
    app_message_deregister_callbacks();
    tick_timer_service_unsubscribe();
    connection_service_unsubscribe();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
