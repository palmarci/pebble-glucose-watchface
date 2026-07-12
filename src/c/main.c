// MiniMed -> Pebble watchface (proof of concept).
//
// Displays the current blood glucose value pushed from the Android bridge app
// over AppMessage, plus how long ago it arrived and the current time/date.
// Deliberately minimal: no graph, delta or trend yet (see protocol.h).

#include <pebble.h>
#include "protocol.h"
#include "strings.h"
#include "test_mode.h"

// Show "---" instead of a stale value once the last reading is this old. CGM cadence is 5 min, so
// keep the last value on screen across a couple of missed readings before giving up on it.
#define STALE_MINUTES 15
// Below this age a reading is "fresh" and the time-ago label is hidden (it's only useful as an
// ageing/staleness hint once a reading has been missed).
#define FRESH_MINUTES 6

// Graph config.
#define GRAPH_HOURS 3
#define MAX_GRAPH_POINTS 64 // 3 h @ 5 min = 36; headroom
// Fixed y-axis 2.2–16 mmol/L, in "mg/dL / 2" wire units (40..288 mg/dL). Out-of-range clamps to edge.
#define GRAPH_VALUE_MIN 20
#define GRAPH_VALUE_MAX 144
// Don't connect points more than this far apart (a sensor gap draws as a break, not a straight line).
#define GRAPH_GAP_THRESHOLD_MINUTES 15

// Persistent-storage keys (survive watchface unload and watch reboot). Separate namespace from the
// AppMessage keys in protocol.h. Leaving the watchface for the menu and returning relaunches the app,
// which would otherwise reset the in-RAM graph to empty; we save on unload and reload on launch.
#define PERSIST_BG_STRING 1
#define PERSIST_BG_TIMESTAMP 2
#define PERSIST_IOB_STRING 3
#define PERSIST_STATUS_STRING 4
#define PERSIST_GRAPH_REF 5
#define PERSIST_GRAPH_COUNT 6
#define PERSIST_GRAPH_OFFSETS 7
#define PERSIST_GRAPH_VALUES 8
#define PERSIST_GRAPH_HIGH 9
#define PERSIST_GRAPH_LOW 10

// Status strip: a full-width opaque white band hugging the status text, sitting low over the graph so
// its uppercase letters land ~2px above the time. Custom-drawn (not a TextLayer background) so the
// band can be full width yet vertically tight to the caps. All in screen coords; the layer is a plain
// overlay over the graph that paints only the band + text (rest transparent, so the graph shows).
#define STATUS_FONT FONT_KEY_GOTHIC_18_BOLD
#define STATUS_LAYER_TOP 78 // overlay-layer top (screen y); gives graphics_draw_text room to render
#define STATUS_LAYER_H 36
#define STATUS_BAND_TOP 96 // white band top (~2px above the caps)
#define STATUS_BAND_H 17   // band height (caps + a little room)
#define STATUS_TEXT_TOP 92 // text box top; the font's top padding drops the glyphs into the band

static Window *s_window;
static TextLayer *s_bg_layer;
static TextLayer *s_ago_layer;
static TextLayer *s_iob_layer;
static Layer *s_status_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static Layer *s_graph_layer;

// Latest reading from the phone.
static char s_bg_string[16] = STR_NO_DATA;
static uint32_t s_bg_timestamp = 0; // 0 => never received

static char s_iob_string[8] = "";     // raw IOB units from phone, e.g. "2.5"; empty = unknown
static char s_status_string[20] = ""; // pump status, e.g. "SUSPENDED"; empty = normal

// Graph data (all BG values in "mg/dL / 2" wire units).
static uint32_t s_graph_ref_timestamp = 0;
static uint16_t s_graph_count = 0;
static uint16_t s_graph_offsets[MAX_GRAPH_POINTS];  // minutes since ref_timestamp
static uint8_t s_graph_bg_values[MAX_GRAPH_POINTS]; // mg/dL / 2
static uint8_t s_graph_high_line = 90;              // 10.0 mmol/L (default; phone may override)
static uint8_t s_graph_low_line = 36;               // 4.0 mmol/L

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
    // How old the current BG value is (in minutes). Hidden while fresh; shown only once a reading has
    // been missed, so it reads as a staleness hint rather than constant clutter.
    int mins = minutes_ago();
    if (mins < FRESH_MINUTES) {
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

// The status label overlays the bottom of the graph as an opaque strip, but only when a status is
// active; otherwise it's hidden so the full graph shows.
static void update_status_display(void) {
    if (s_status_layer) layer_mark_dirty(s_status_layer);
}

// Paints only the band + text (when a status is active); everything else stays transparent so the
// graph below shows through. Coords are layer-relative (layer top = STATUS_LAYER_TOP screen y).
static void status_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_status_string[0] == '\0') {
        return;
    }
    const int16_t w = layer_get_bounds(layer).size.w;
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(0, STATUS_BAND_TOP - STATUS_LAYER_TOP, w, STATUS_BAND_H), 0,
                       GCornerNone);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, s_status_string, fonts_get_system_font(STATUS_FONT),
                       GRect(0, STATUS_TEXT_TOP - STATUS_LAYER_TOP, w, 24),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Map a BG value (mg/dL / 2) to a y within the graph, clamping to the fixed range.
static int graph_value_to_y(int16_t height, int bg) {
    if (bg < GRAPH_VALUE_MIN) bg = GRAPH_VALUE_MIN;
    if (bg > GRAPH_VALUE_MAX) bg = GRAPH_VALUE_MAX;
    return height - ((bg - GRAPH_VALUE_MIN) * height) / (GRAPH_VALUE_MAX - GRAPH_VALUE_MIN);
}

static void graph_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_graph_count == 0) {
        return;
    }
    const GRect b = layer_get_bounds(layer);
    const int16_t w = b.size.w;
    const int16_t h = b.size.h;

    // Target range lines: thin solid (1px), distinct from the thicker 2px BG trace.
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(0, graph_value_to_y(h, s_graph_high_line), w, 1), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(0, graph_value_to_y(h, s_graph_low_line), w, 1), 0, GCornerNone);

    // BG trace: newest on the right, oldest (GRAPH_HOURS ago) on the left.
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 2);
    const uint32_t now = time(NULL);
    const int graph_minutes = GRAPH_HOURS * 60;
    int prev_x = 0, prev_y = 0;
    bool has_prev = false;
    for (int i = 0; i < s_graph_count; i++) {
        const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[i] * 60;
        const int mins_ago = (int)(((int64_t)now - (int64_t)pt_ts) / 60);
        if (mins_ago < 0 || mins_ago > graph_minutes) {
            has_prev = false;
            continue;
        }
        const int x = w - (mins_ago * w) / graph_minutes;
        const int y = graph_value_to_y(h, s_graph_bg_values[i]);

        bool has_next = false;
        if (i + 1 < s_graph_count) {
            const uint32_t next_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[i + 1] * 60;
            const int gap = next_ts > pt_ts ? (int)((next_ts - pt_ts) / 60) : (int)((pt_ts - next_ts) / 60);
            has_next = gap <= GRAPH_GAP_THRESHOLD_MINUTES;
        }
        if (has_prev) {
            graphics_draw_line(ctx, GPoint(prev_x, prev_y), GPoint(x, y));
        } else if (!has_next) {
            graphics_fill_circle(ctx, GPoint(x, y), 1); // isolated point
        }
        prev_x = x;
        prev_y = y;
        has_prev = has_next;
    }
}

static void tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_time_and_date();
    update_ago_display();
    update_bg_display(); // may flip to "---" once the reading goes stale
}

// Persist the current reading + graph so relaunching the watchface (e.g. after the menu) shows it
// immediately instead of an empty graph. Called on unload; the phone also re-sends on the ready ping.
static void save_state(void) {
    persist_write_string(PERSIST_BG_STRING, s_bg_string);
    persist_write_int(PERSIST_BG_TIMESTAMP, (int32_t)s_bg_timestamp);
    persist_write_string(PERSIST_IOB_STRING, s_iob_string);
    persist_write_string(PERSIST_STATUS_STRING, s_status_string);
    persist_write_int(PERSIST_GRAPH_REF, (int32_t)s_graph_ref_timestamp);
    persist_write_int(PERSIST_GRAPH_COUNT, s_graph_count);
    persist_write_int(PERSIST_GRAPH_HIGH, s_graph_high_line);
    persist_write_int(PERSIST_GRAPH_LOW, s_graph_low_line);
    if (s_graph_count > 0) {
        persist_write_data(PERSIST_GRAPH_OFFSETS, s_graph_offsets, s_graph_count * sizeof(uint16_t));
        persist_write_data(PERSIST_GRAPH_VALUES, s_graph_bg_values, s_graph_count * sizeof(uint8_t));
    }
}

static void load_state(void) {
    if (persist_exists(PERSIST_BG_STRING)) persist_read_string(PERSIST_BG_STRING, s_bg_string, sizeof(s_bg_string));
    if (persist_exists(PERSIST_BG_TIMESTAMP)) s_bg_timestamp = (uint32_t)persist_read_int(PERSIST_BG_TIMESTAMP);
    if (persist_exists(PERSIST_IOB_STRING)) persist_read_string(PERSIST_IOB_STRING, s_iob_string, sizeof(s_iob_string));
    if (persist_exists(PERSIST_STATUS_STRING)) persist_read_string(PERSIST_STATUS_STRING, s_status_string, sizeof(s_status_string));
    if (persist_exists(PERSIST_GRAPH_HIGH)) s_graph_high_line = (uint8_t)persist_read_int(PERSIST_GRAPH_HIGH);
    if (persist_exists(PERSIST_GRAPH_LOW)) s_graph_low_line = (uint8_t)persist_read_int(PERSIST_GRAPH_LOW);
    if (persist_exists(PERSIST_GRAPH_COUNT) && persist_exists(PERSIST_GRAPH_OFFSETS) &&
        persist_exists(PERSIST_GRAPH_VALUES)) {
        uint16_t count = (uint16_t)persist_read_int(PERSIST_GRAPH_COUNT);
        if (count > MAX_GRAPH_POINTS) count = MAX_GRAPH_POINTS;
        if (count > 0) {
            persist_read_data(PERSIST_GRAPH_OFFSETS, s_graph_offsets, count * sizeof(uint16_t));
            persist_read_data(PERSIST_GRAPH_VALUES, s_graph_bg_values, count * sizeof(uint8_t));
            s_graph_ref_timestamp = (uint32_t)persist_read_int(PERSIST_GRAPH_REF);
            s_graph_count = count;
        }
    }
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
        update_status_display();
    }

    // Graph: [ref_ts u32 LE][count u16 LE][offset_min u16 LE ×n][bg u8 ×n].
    Tuple *graph_tuple = dict_find(iter, KEY_GRAPH_DATA);
    if (graph_tuple && graph_tuple->length >= 6) {
        const uint8_t *d = graph_tuple->value->data;
        s_graph_ref_timestamp = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
        uint16_t count = d[4] | (d[5] << 8);
        if (count > MAX_GRAPH_POINTS) count = MAX_GRAPH_POINTS;
        if (graph_tuple->length >= (uint16_t)(6 + count * 3)) {
            for (int i = 0; i < count; i++) {
                int o = 6 + i * 2;
                s_graph_offsets[i] = d[o] | (d[o + 1] << 8);
            }
            for (int i = 0; i < count; i++) {
                s_graph_bg_values[i] = d[6 + count * 2 + i];
            }
            s_graph_count = count;
            if (s_graph_layer) layer_mark_dirty(s_graph_layer);
        }
    }
    Tuple *high_tuple = dict_find(iter, KEY_GRAPH_HIGH_LINE);
    if (high_tuple) s_graph_high_line = high_tuple->value->uint8;
    Tuple *low_tuple = dict_find(iter, KEY_GRAPH_LOW_LINE);
    if (low_tuple) s_graph_low_line = low_tuple->value->uint8;

    APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s (ts=%lu) IOB: %s graph=%d", s_bg_string,
            s_bg_timestamp, s_iob_string, s_graph_count);
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
    dict_write_uint32(iter, KEY_CAPABILITIES, CAP_BG | CAP_IOB | CAP_STATUS | CAP_GRAPH);
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

    // Layout: BG (top) and time (bottom) share the same large font; time-ago top-left, IOB top-right;
    // the middle band is the 3-hour graph, with the pump-status label overlaid on its bottom strip.

    // BG value — top, centered, large.
    s_bg_layer = make_label(root, GRect(0, -6, b.size.w, 42),
                            FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);
    // Time since last reading — top-left corner.
    s_ago_layer = make_label(root, GRect(4, 4, 64, 26),
                             FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentLeft);
    // Insulin on board — top-right corner (e.g. "2.5U").
    s_iob_layer = make_label(root, GRect(b.size.w - 68, 4, 64, 26),
                             FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentRight);

    // BG graph — middle band.
    const GRect graph_frame = GRect(0, 38, b.size.w, 64); // y 38..102
    s_graph_layer = layer_create(graph_frame);
    layer_set_update_proc(s_graph_layer, graph_layer_update_proc);
    layer_add_child(root, s_graph_layer);

    // Pump status — a full-width band + text painted low over the graph (see status_layer_update_proc).
    // Added after the graph so it draws on top; the time (added next) still draws over its bottom edge.
    s_status_layer = layer_create(GRect(0, STATUS_LAYER_TOP, b.size.w, STATUS_LAYER_H));
    layer_set_update_proc(s_status_layer, status_layer_update_proc);
    layer_add_child(root, s_status_layer);

    // Current time — bottom, same large font as BG.
    s_time_layer = make_label(root, GRect(0, 105, b.size.w, 42),
                              FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);
    // Date — below the time.
    s_date_layer = make_label(root, GRect(0, 140, b.size.w, 26),
                              FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);

    update_bg_display();
    update_ago_display();
    update_iob_display();
    update_status_display();
    update_time_and_date();
}

static void window_unload(Window *window) {
    text_layer_destroy(s_bg_layer);
    text_layer_destroy(s_ago_layer);
    text_layer_destroy(s_iob_layer);
    layer_destroy(s_status_layer);
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
    layer_destroy(s_graph_layer);
}

static void init_test_mode_data(void) {
#ifdef TEST_MODE
    safe_strncpy(s_bg_string, TEST_BG_STRING, sizeof(s_bg_string));
    s_bg_timestamp = time(NULL) - TEST_MINUTES_AGO * 60;
    safe_strncpy(s_iob_string, TEST_IOB_STRING, sizeof(s_iob_string));
    safe_strncpy(s_status_string, TEST_STATUS_STRING, sizeof(s_status_string));
    // Dummy 3-hour graph: a triangle wave ~70..190 mg/dL (crosses the 4.0 & 10.0 target lines).
    s_graph_ref_timestamp = time(NULL) - (uint32_t)GRAPH_HOURS * 3600;
    s_graph_count = 36; // 3 h @ 5 min
    for (int i = 0; i < 36; i++) {
        s_graph_offsets[i] = i * 5;
        int swing = (i % 12) < 6 ? (i % 12) * 10 : (12 - (i % 12)) * 10; // 0..60..0
        s_graph_bg_values[i] = (90 + swing) / 2; // ~90..150 mg/dL, in-range
    }
#endif
}

static void init(void) {
    load_state(); // restore last reading + graph so a relaunch renders immediately, not empty
    app_message_register_inbox_received(new_data_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_open(1024, 64); // inbox large enough for the graph byte array

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
    save_state(); // persist before unload so returning from the menu shows the graph, not "---"
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
