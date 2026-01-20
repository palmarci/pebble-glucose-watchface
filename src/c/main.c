// xDrip Pebble reference watchface
//
// This is a simple watchface created to serve as a reference for the new xDrip-Pebble communication
// protocol. It displays:
//
//   - blood glucose
//   - trend arrow
//   - time ago (time since BG reading)
//   - BG delta
//   - time and date
//
// Until it gets data, it displays "---" for glucose and nothing for the rest.

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

// Layout elements
static Window *s_window = NULL;
static TextLayer *s_bg_layer = NULL;
static TextLayer *s_delta_layer = NULL;
static TextLayer *s_time_ago_layer = NULL;
static TextLayer *s_time_layer = NULL;
static TextLayer *s_date_layer = NULL;
static BitmapLayer *s_arrow_layer = NULL;
static GBitmap *s_arrow_bitmap = NULL;

// Watchface data
static uint32_t s_bg_timestamp = 0;    // Seconds since epoch
static char s_bg_string[5] = "---";    // Fits '10.0'
static char s_delta_string[6] = "";    // Fits '+0.06'
static uint8_t s_arrow_index = 0;      // See ARROWS below
static char s_time_ago_buffer[4] = ""; // Fits '99h'
static char s_time_buffer[6] = "";     // Fits '20:23'
static char s_date_buffer[11] = "";    // Fits 'Tue 13 Jan'

// Mapping: Arrow index -> Arrow image resource ID
static const uint32_t ARROWS[] = {0, // unknown, no arrow
                                  RESOURCE_ID_ARROW_UP_DOUBLE,
                                  RESOURCE_ID_ARROW_UP,
                                  RESOURCE_ID_ARROW_UP_SLANT,
                                  RESOURCE_ID_ARROW_FLAT,
                                  RESOURCE_ID_ARROW_DOWN_SLANT,
                                  RESOURCE_ID_ARROW_DOWN,
                                  RESOURCE_ID_ARROW_DOWN_DOUBLE};

static inline char *safe_strncpy(char *dest, const char *src, size_t count) {
    if (count > 0) {
        strncpy(dest, src, count);
        dest[count - 1] = '\0';
    }
    return dest;
}

static void update_displayed_time_ago(void) {
    // Don't populate until we have valid data.
    if (s_bg_timestamp == 0) {
        return;
    }

    const int minutes_ago = (time(NULL) - s_bg_timestamp) / 60;
    if (minutes_ago < 60) {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dm", minutes_ago);
    } else {
        snprintf(s_time_ago_buffer, sizeof(s_time_ago_buffer), "%dh", minutes_ago / 60);
    }
    text_layer_set_text(s_time_ago_layer, s_time_ago_buffer);
}

static void update_displayed_xdrip_data(void) {
    // Update displayed BG value
    text_layer_set_text(s_bg_layer, s_bg_string);

    // Update displayed delta value
    text_layer_set_text(s_delta_layer, s_delta_string);

    // Update displayed trend arrow
    if (s_arrow_bitmap) {
        gbitmap_destroy(s_arrow_bitmap);
        s_arrow_bitmap = NULL;
    }
    if (s_arrow_index > 0 && s_arrow_index < sizeof(ARROWS) / sizeof(ARROWS[0])) {
        s_arrow_bitmap = gbitmap_create_with_resource(ARROWS[s_arrow_index]);
        bitmap_layer_set_bitmap(s_arrow_layer, s_arrow_bitmap);
    } else {
        bitmap_layer_set_bitmap(s_arrow_layer, NULL);
    }
}

static void update_displayed_time_and_date(void) {
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
    update_displayed_xdrip_data();
    update_displayed_time_and_date();
    update_displayed_time_ago();
}

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

void minute_tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_displayed_time_and_date();
    update_displayed_time_ago();
}

static void new_xdrip_data_callback(DictionaryIterator *iter, void *context) {
    // Check for timestamp (always present in data messages)
    Tuple *timestamp_tuple = dict_find(iter, KEY_BG_TIMESTAMP);
    if (timestamp_tuple) {
        s_bg_timestamp = timestamp_tuple->value->uint32;

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

        update_displayed_xdrip_data();
        update_displayed_time_ago();

        APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s, arrow: %d, delta: %s", s_bg_string,
                s_arrow_index, s_delta_string);
    }
}

// This can also be used to trigger xDrip to send fresh data.
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
    // Re-send capabilities on reconnect. This triggers xDrip to send fresh data.
    if (connected) {
        send_capability_announcement();
    }
}

void init_test_mode_data(void) {
#ifdef TEST_MODE
    s_bg_timestamp = time(NULL) - TEST_MINUTES_AGO * 60;
    safe_strncpy(s_bg_string, TEST_BG_STRING, sizeof(s_bg_string));
    s_arrow_index = TEST_ARROW_INDEX;
    safe_strncpy(s_delta_string, TEST_DELTA_STRING, sizeof(s_delta_string));
#endif
}

void init(void) {
    app_message_register_inbox_received(new_xdrip_data_callback);
    app_message_open(/*in*/ 256, /*out*/ 64);

    tick_timer_service_subscribe(MINUTE_UNIT, minute_tick_callback);

    connection_service_subscribe(
        (ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    s_window = window_create();
    window_set_window_handlers(s_window,
                               (WindowHandlers){.load = window_load, .unload = window_unload});
    window_stack_push(s_window, /*animated*/ true);

    send_capability_announcement();
}

void deinit(void) {
    app_message_deregister_callbacks();
    tick_timer_service_unsubscribe();
    connection_service_unsubscribe();
    window_destroy(s_window);
}

int main(void) {
    init_test_mode_data();
    init();
    app_event_loop();
    deinit();
}
