// MiniMed -> Pebble watchface (proof of concept).
//
// Displays the current blood glucose value pushed from the Android bridge app
// over AppMessage, plus how long ago it arrived and the current time/date.
// Deliberately minimal: no graph, delta or trend yet (see protocol.h).

#include <pebble.h>
#include "protocol.h"
#include "strings.h"
#include "test_mode.h"

// Show "---" instead of a stale value once the last reading is this old.
#define STALE_MINUTES 6

static Window *s_window;
static TextLayer *s_bg_layer;
static TextLayer *s_ago_layer;
static TextLayer *s_iob_layer;
static TextLayer *s_status_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;

// Latest reading from the phone.
static char s_bg_string[16] = STR_NO_DATA;
static uint32_t s_bg_timestamp = 0; // 0 => never received

static char s_iob_string[8] = "";     // raw IOB units from phone, e.g. "2.5"; empty = unknown
static char s_status_string[20] = ""; // pump status, e.g. "SUSPENDED"; empty = normal

static char s_bg_display[16];
static char s_ago_display[16];
static char s_iob_display[12];
static char s_time_display[8];
static char s_date_display[16];

static void safe_strncpy(char *dst, const char *src, size_t dst_size) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool has_reading(void) {
    return s_bg_timestamp != 0;
}

static int seconds_ago(void) {
    if (!has_reading()) {
        return -1;
    }
    int secs = (int)(time(NULL) - (time_t)s_bg_timestamp);
    return secs < 0 ? 0 : secs;
}

static int minutes_ago(void) {
    int secs = seconds_ago();
    return secs < 0 ? -1 : secs / 60;
}

static void update_bg_display(void) {
    int mins = minutes_ago();
    if (!has_reading() || mins >= STALE_MINUTES) {
        safe_strncpy(s_bg_display, STR_NO_DATA, sizeof(s_bg_display));
    } else {
        safe_strncpy(s_bg_display, s_bg_string, sizeof(s_bg_display));
    }
    text_layer_set_text(s_bg_layer, s_bg_display);
}

static void update_ago_display(void) {
    // How old the current BG value is (in minutes), regardless of whether it arrived via push or poll.
    // Future option: hide when fresh (mins < 5).
    int mins = minutes_ago();
    if (mins < 0) {
        s_ago_display[0] = '\0';
    } else if (mins < 60) {
        snprintf(s_ago_display, sizeof(s_ago_display), STR_AGO_MIN_FMT, mins);
    } else {
        snprintf(s_ago_display, sizeof(s_ago_display), STR_AGO_HOURS_FMT, mins / 60);
    }
    text_layer_set_text(s_ago_layer, s_ago_display);
}

static void update_iob_display(void) {
    if (s_iob_string[0] == '\0') {
        s_iob_display[0] = '\0';
    } else {
        snprintf(s_iob_display, sizeof(s_iob_display), STR_IOB_FMT, s_iob_string);
    }
    text_layer_set_text(s_iob_layer, s_iob_display);
}

static void update_time_and_date(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(s_time_display, sizeof(s_time_display),
             clock_is_24h_style() ? STR_TIME_24H_FMT : STR_TIME_12H_FMT, t);
    strftime(s_date_display, sizeof(s_date_display), STR_DATE_FMT, t);
    text_layer_set_text(s_time_layer, s_time_display);
    text_layer_set_text(s_date_layer, s_date_display);
}

static void tick_callback(struct tm *tick_time, TimeUnits units_changed) {
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

    Tuple *iob_tuple = dict_find(iter, KEY_IOB_STRING);
    if (iob_tuple) {
        safe_strncpy(s_iob_string, iob_tuple->value->cstring, sizeof(s_iob_string));
        update_iob_display();
    }

    Tuple *status_tuple = dict_find(iter, KEY_STATUS_STRING);
    if (status_tuple) {
        safe_strncpy(s_status_string, status_tuple->value->cstring, sizeof(s_status_string));
        text_layer_set_text(s_status_layer, s_status_string);
    }

    APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s (ts=%lu) IOB: %s", s_bg_string, s_bg_timestamp,
            s_iob_string);
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
    dict_write_uint32(iter, KEY_CAPABILITIES, CAP_BG | CAP_IOB | CAP_STATUS);
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
    text_layer_set_text_color(layer, GColorBlack);
    text_layer_set_font(layer, fonts_get_system_font(font_key));
    text_layer_set_text_alignment(layer, align);
    layer_add_child(root, text_layer_get_layer(layer));
    return layer;
}

static void window_load(Window *window) {
    window_set_background_color(window, GColorWhite);
    Layer *root = window_get_root_layer(window);
    GRect b = layer_get_bounds(root); // flint: 144 x 168

    // Layout mirrors the old xDrip watchface: BG (top) and time (bottom) share the same large
    // font; time-ago tucks into the top-left; the middle band is reserved for a future BG graph.

    // BG value — top, centered, large.
    s_bg_layer = make_label(root, GRect(0, -6, b.size.w, 42),
                            FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);
    // Time since last reading — top-left corner (m:ss while debugging).
    s_ago_layer = make_label(root, GRect(4, 4, 64, 26),
                             FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentLeft);
    // Insulin on board — top-right corner (e.g. "2.5U").
    s_iob_layer = make_label(root, GRect(b.size.w - 68, 4, 64, 26),
                             FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentRight);
    // Pump status — centered in the middle band (empty when normal/SmartGuard-on).
    // Shares space with the future graph; revisit placement when the graph lands.
    s_status_layer = make_label(root, GRect(0, 60, b.size.w, 26),
                                FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);
    text_layer_set_text(s_status_layer, s_status_string);
    // Current time — bottom, same large font as BG.
    s_time_layer = make_label(root, GRect(0, 105, b.size.w, 42),
                              FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);
    // Date — below the time.
    s_date_layer = make_label(root, GRect(0, 140, b.size.w, 26),
                              FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);

    update_bg_display();
    update_ago_display();
    update_iob_display();
    update_time_and_date();
}

static void window_unload(Window *window) {
    text_layer_destroy(s_bg_layer);
    text_layer_destroy(s_ago_layer);
    text_layer_destroy(s_iob_layer);
    text_layer_destroy(s_status_layer);
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
}

static void init_test_mode_data(void) {
#ifdef TEST_MODE
    safe_strncpy(s_bg_string, TEST_BG_STRING, sizeof(s_bg_string));
    s_bg_timestamp = time(NULL) - TEST_MINUTES_AGO * 60;
    safe_strncpy(s_iob_string, TEST_IOB_STRING, sizeof(s_iob_string));
    safe_strncpy(s_status_string, TEST_STATUS_STRING, sizeof(s_status_string));
#endif
}

static void init(void) {
    app_message_register_inbox_received(new_data_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_open(256, 64);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_callback);
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
