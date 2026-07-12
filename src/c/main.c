// MiniMed -> Pebble watchface (proof of concept).
//
// Displays the current blood glucose value pushed from the Android bridge app
// over AppMessage, plus how long ago it arrived and the current time/date.
// Deliberately minimal: no graph, delta or trend yet (see protocol.h).

#include <pebble.h>
#include "protocol.h"
#include "test_mode.h"

// Show "---" instead of a stale value once the last reading is this old.
#define STALE_MINUTES 6

#define NO_DATA "---"

static Window *s_window;
static TextLayer *s_bg_layer;
static TextLayer *s_ago_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;

// Latest reading from the phone.
static char s_bg_string[16] = NO_DATA;
static uint32_t s_bg_timestamp = 0; // 0 => never received

static char s_bg_display[16];
static char s_ago_display[16];
static char s_time_display[8];
static char s_date_display[16];

static void safe_strncpy(char *dst, const char *src, size_t dst_size) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool has_reading(void) {
    return s_bg_timestamp != 0;
}

static int minutes_ago(void) {
    if (!has_reading()) {
        return -1;
    }
    int secs = (int)(time(NULL) - (time_t)s_bg_timestamp);
    if (secs < 0) {
        secs = 0;
    }
    return secs / 60;
}

static void update_bg_display(void) {
    int mins = minutes_ago();
    if (!has_reading() || mins >= STALE_MINUTES) {
        safe_strncpy(s_bg_display, NO_DATA, sizeof(s_bg_display));
    } else {
        safe_strncpy(s_bg_display, s_bg_string, sizeof(s_bg_display));
    }
    text_layer_set_text(s_bg_layer, s_bg_display);
}

static void update_ago_display(void) {
    int mins = minutes_ago();
    if (mins < 0) {
        s_ago_display[0] = '\0';
    } else if (mins < 60) {
        snprintf(s_ago_display, sizeof(s_ago_display), "%dm ago", mins);
    } else {
        snprintf(s_ago_display, sizeof(s_ago_display), "%dh ago", mins / 60);
    }
    text_layer_set_text(s_ago_layer, s_ago_display);
}

static void update_time_and_date(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(s_time_display, sizeof(s_time_display),
             clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
    strftime(s_date_display, sizeof(s_date_display), "%a %d %b", t);
    text_layer_set_text(s_time_layer, s_time_display);
    text_layer_set_text(s_date_layer, s_date_display);
}

static void minute_tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_time_and_date();
    update_ago_display();
    update_bg_display(); // may flip to "---" once the reading goes stale
}

static void new_data_callback(DictionaryIterator *iter, void *context) {
    Tuple *bg_tuple = dict_find(iter, KEY_BG_STRING);
    Tuple *ts_tuple = dict_find(iter, KEY_BG_TIMESTAMP);
    if (bg_tuple) {
        safe_strncpy(s_bg_string, bg_tuple->value->cstring, sizeof(s_bg_string));
    }
    if (ts_tuple) {
        s_bg_timestamp = ts_tuple->value->uint32;
    } else if (bg_tuple) {
        s_bg_timestamp = time(NULL); // fall back to arrival time
    }
    APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s (ts=%lu)", s_bg_string, s_bg_timestamp);
    update_bg_display();
    update_ago_display();
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
}

// Announce which data we want. Also nudges the phone to push the latest reading,
// so a freshly launched watchface fills in without waiting for the next poll.
static void send_ready(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin failed");
        return;
    }
    dict_write_uint8(iter, KEY_PROTOCOL_VERSION, PROTOCOL_VERSION);
    dict_write_uint32(iter, KEY_CAPABILITIES, CAP_BG);
    if (app_message_outbox_send() != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send failed");
    }
}

static void bluetooth_callback(bool connected) {
    if (connected) {
        send_ready();
    }
}

static TextLayer *make_label(Layer *root, GRect frame, const char *font_key,
                             GTextAlignment align) {
    TextLayer *layer = text_layer_create(frame);
    text_layer_set_background_color(layer, GColorClear);
    text_layer_set_text_color(layer, GColorWhite);
    text_layer_set_font(layer, fonts_get_system_font(font_key));
    text_layer_set_text_alignment(layer, align);
    layer_add_child(root, text_layer_get_layer(layer));
    return layer;
}

static void window_load(Window *window) {
    window_set_background_color(window, GColorBlack);
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root); // flint: 144 x 168

    s_bg_layer = make_label(root, GRect(0, 18, b.size.w, 48),
                            FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);
    s_ago_layer = make_label(root, GRect(0, 68, b.size.w, 22),
                             FONT_KEY_GOTHIC_18, GTextAlignmentCenter);
    s_time_layer = make_label(root, GRect(0, 100, b.size.w, 40),
                              FONT_KEY_BITHAM_34_MEDIUM_NUMBERS, GTextAlignmentCenter);
    s_date_layer = make_label(root, GRect(0, 142, b.size.w, 22),
                              FONT_KEY_GOTHIC_18, GTextAlignmentCenter);

    update_bg_display();
    update_ago_display();
    update_time_and_date();
}

static void window_unload(Window *window) {
    text_layer_destroy(s_bg_layer);
    text_layer_destroy(s_ago_layer);
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
}

static void init_test_mode_data(void) {
#ifdef TEST_MODE
    safe_strncpy(s_bg_string, TEST_BG_STRING, sizeof(s_bg_string));
    s_bg_timestamp = time(NULL) - TEST_MINUTES_AGO * 60;
#endif
}

static void init(void) {
    app_message_register_inbox_received(new_data_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_open(256, 64);

    tick_timer_service_subscribe(MINUTE_UNIT, minute_tick_callback);
    connection_service_subscribe(
        (ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    s_window = window_create();
    window_set_window_handlers(s_window,
                               (WindowHandlers){.load = window_load, .unload = window_unload});
    window_stack_push(s_window, true);

    send_ready();
}

static void deinit(void) {
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
