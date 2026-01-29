// xDrip Pebble reference watchface
//
// This is a simple watchface created to serve as a reference for the new xDrip-Pebble communication
// protocol. It displays:
//
//   - blood glucose
//   - 2-hour graph with dynamic trend arrow
//   - time ago (time since BG reading)
//   - BG delta
//   - time and date
//
// Until it gets data, it displays "---" for glucose and nothing for the rest.

#include "protocol.h"
#include "test_mode.h"
#include <pebble.h>

// Layout elements
static Window *s_window = NULL;
static TextLayer *s_bg_layer = NULL;
static TextLayer *s_delta_layer = NULL;
static TextLayer *s_time_ago_layer = NULL;
static TextLayer *s_time_layer = NULL;
static TextLayer *s_date_layer = NULL;
static Layer *s_graph_layer = NULL;

// Watchface data
static uint32_t s_bg_timestamp = 0;    // Seconds since epoch
static char s_bg_string[5] = "---";    // Fits '10.0'
static char s_delta_string[6] = "";    // Fits '+0.06'
static char s_time_ago_buffer[4] = ""; // Fits '99h'
static char s_time_buffer[6] = "";     // Fits '20:23'
static char s_date_buffer[11] = "";    // Fits 'Tue 13 Jan'

// Graph config
// All graph BG values are stored in "mg/dL / 2" units (matching wire protocol).
// This gives 0-510 mg/dL range with 2 mg/dL (0.1 mmol/L) resolution in one byte.
#define GRAPH_HOURS 2
#define MAX_GRAPH_POINTS 300                        // 24 hours @ 5 min intervals = 288
static uint32_t s_graph_ref_timestamp = 0;          // Reference timestamp (seconds)
static uint16_t s_graph_count = 0;                  // Number of graph points
static uint16_t s_graph_offsets[MAX_GRAPH_POINTS];  // Minutes since ref_timestamp
static uint8_t s_graph_bg_values[MAX_GRAPH_POINTS]; // BG values (mg/dL / 2)
static uint8_t s_graph_high_line = 90;              // High threshold (mg/dL / 2) = 180 mg/dL
static uint8_t s_graph_low_line = 36;               // Low threshold (mg/dL / 2) = 72 mg/dL

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

    // Redraw graph (which includes the dynamic arrow)
    if (s_graph_layer) {
        layer_mark_dirty(s_graph_layer);
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

#if 0
static void draw_arrow_head(GContext *ctx, GPoint tip, int dx, int dy, int size) {
    // Draw a triangular arrowhead at the tip pointing in the direction of (dx, dy)
    // We calculate perpendicular vectors to create the arrowhead base

    // Normalize the direction vector (approximate, using integer math)
    int len_sq = dx * dx + dy * dy;
    if (len_sq == 0)
        return;

    // Create perpendicular vector (-dy, dx) for the arrowhead sides
    // Scale down for the arrowhead size
    int perp_x = (-dy * size) / 10;
    int perp_y = (dx * size) / 10;

    // Back vector: opposite of direction
    int back_x = (-dx * size) / 10;
    int back_y = (-dy * size) / 10;

    // Calculate the two base points of the triangle
    GPoint points[3];
    points[0] = tip;
    points[1].x = tip.x + back_x + perp_x;
    points[1].y = tip.y + back_y + perp_y;
    points[2].x = tip.x + back_x - perp_x;
    points[2].y = tip.y + back_y - perp_y;

    // Draw filled triangle
    graphics_context_set_fill_color(ctx, GColorBlack);
    GPathInfo arrow_path_info = {.num_points = 3, .points = points};
    GPath *arrow_path = gpath_create(&arrow_path_info);
    gpath_draw_filled(ctx, arrow_path);
    gpath_destroy(arrow_path);
}
#endif

// static void graphics_draw_dotted_line(GContext *ctx, GPoint p0, GPoint p1) {
// graphics_draw_line(ctx, p0, p1);
// just draw
// }

static void graph_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_graph_count == 0) {
        return; // No data to display
    }

    const GRect bounds = layer_get_bounds(layer);
    const int width = bounds.size.w;
    const int height = bounds.size.h;

    // Graph data width (2/3 of screen), not including arrow space
    const int graph_width = (PBL_DISPLAY_WIDTH * 2) / 3;

    // Graph parameters (all in "mg/dL / 2" units)
    const int graph_min = 0;   // 0 mg/dL
    const int graph_max = 144; // 288 mg/dL = 16 mmol/L

    graphics_context_set_fill_color(ctx, GColorBlack);

    // Draw high/low threshold lines as thin rectangles (across full width)
    const int high_y =
        height - ((s_graph_high_line - graph_min) * height) / (graph_max - graph_min);
    const int low_y = height - ((s_graph_low_line - graph_min) * height) / (graph_max - graph_min);
    graphics_fill_rect(ctx, GRect(0, high_y, width, 2), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(0, low_y, width, 2), 0, GCornerNone);

    const int graph_minutes = GRAPH_HOURS * 60;
    const uint32_t now = time(NULL);

    int prev_x = 0;
    int prev_y = 0;
    int last_x = 0;
    int last_y = 0;
    int second_last_x = 0;
    int second_last_y = 0;
    int visible_count = 0;

    graphics_context_set_stroke_width(ctx, 3);

    // Draw graph line
    for (int i = 0; i < s_graph_count; i++) {
        // Calculate absolute timestamp of this point
        uint32_t point_timestamp = s_graph_ref_timestamp + (s_graph_offsets[i] * 60);

        // Calculate how many minutes ago this point was from now
        int minutes_ago = (now - point_timestamp) / 60;

        // Skip points that are too old (off the left edge)
        if (minutes_ago > graph_minutes) {
            continue;
        }

        // X position: right edge = now (0 min ago), left edge = graph_minutes ago
        int x = graph_width - ((minutes_ago * graph_width) / graph_minutes);

        // Y position: inverted (high BG at top)
        const int bg = s_graph_bg_values[i];
        const int y = height - ((bg - graph_min) * height) / (graph_max - graph_min);

        // Draw line connecting to previous point
        if (visible_count > 0) {
            graphics_draw_line(ctx, GPoint(prev_x, prev_y), GPoint(x, y));
        }

        // Track last two points for arrow calculation
        if (visible_count > 0) {
            second_last_x = last_x;
            second_last_y = last_y;
        }
        last_x = x;
        last_y = y;

        prev_x = x;
        prev_y = y;
        visible_count++;
    }

    // Draw a bigger dot at the most recent point
    if (visible_count > 0) {
        const int dot_radius = 4;
        // graphics_fill_circle(ctx, GPoint(last_x, last_y), dot_radius);
    }

    // Draw dynamic arrow extending from the most recent point
    if (visible_count >= 2) {
        // Calculate slope from second-to-last to last point
        int dx = last_x - second_last_x;
        int dy = last_y - second_last_y;

        APP_LOG(APP_LOG_LEVEL_DEBUG, "Arrow: visible=%d, dx=%d, dy=%d, last=(%d,%d)", visible_count,
                dx, dy, last_x, last_y);

        // Only draw arrow if we have a valid slope
        if (dx != 0 || dy != 0) {
            // Arrow length: 25 pixels
            const int arrow_length = 25;

            // Calculate length of the direction vector (approximate with integer math)
            // We'll use a simple approximation: max(|dx|, |dy|) + min(|dx|, |dy|)/2
            int abs_dx = dx > 0 ? dx : -dx;
            int abs_dy = dy > 0 ? dy : -dy;
            int approx_len = (abs_dx > abs_dy) ? abs_dx + abs_dy / 2 : abs_dy + abs_dx / 2;

            if (approx_len == 0)
                approx_len = 1; // Avoid division by zero

            // Scale the direction vector to arrow_length
            int arrow_dx = (dx * arrow_length) / approx_len;
            int arrow_dy = (dy * arrow_length) / approx_len;

            // Calculate arrow end point
            GPoint arrow_end = {.x = last_x + arrow_dx, .y = last_y + arrow_dy};

            APP_LOG(APP_LOG_LEVEL_DEBUG, "Drawing arrow to (%d,%d), arrow_d=(%d,%d)", arrow_end.x,
                    arrow_end.y, arrow_dx, arrow_dy);

            // ok draw some dots towards arrow_end
            // we have 1/3 of the screen for it
            // maybe use 1/6th?
            // so that's uh display_width/6
            // use that like this " . . ." (space dot x3)
            // that means dot spacing is... (1/6)/3
            const int num_dots = 3;
            const int dot_dx = PBL_DISPLAY_WIDTH / (8 * num_dots);
            const int dot_dy = dot_dx * dy / dx;
            int dot_x = prev_x;
            int dot_y = prev_y;
            for (int i = 0; i < num_dots; ++i) {
                dot_x += dot_dx;
                dot_y += dot_dy;
                // graphics_fill_circle(ctx, GPoint(dot_x, dot_y), 2);
                // graphics_fill_circle(ctx, GPoint(dot_x, dot_y + 10), 1);

                graphics_draw_rect(ctx, GRect(dot_x, dot_y, 2, 2));
            }

            // Draw arrow line
            // graphics_draw_dotted_line(ctx, GPoint(last_x, last_y), arrow_end);

            // Draw arrowhead
            // draw_arrow_head(ctx, arrow_end, arrow_dx, arrow_dy, 4);
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Arrow: not enough points, visible=%d", visible_count);
    }
}

static void window_load(Window *window) {
    Layer *root_layer = window_get_root_layer(window);

    // BG value - top, centered
    s_bg_layer = text_layer_create(GRect(0, -6, PBL_DISPLAY_WIDTH, 42));
    text_layer_set_background_color(s_bg_layer, GColorClear);
    text_layer_set_text_color(s_bg_layer, GColorBlack);
    text_layer_set_font(s_bg_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_bg_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_bg_layer));

    // Time ago - below BG, left
    s_time_ago_layer = text_layer_create(GRect(10, 30, 50, 42));
    text_layer_set_background_color(s_time_ago_layer, GColorClear);
    text_layer_set_text_color(s_time_ago_layer, GColorBlack);
    text_layer_set_font(s_time_ago_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_time_ago_layer, GTextAlignmentLeft);
    layer_add_child(root_layer, text_layer_get_layer(s_time_ago_layer));

    // Delta - below BG, right
    s_delta_layer = text_layer_create(GRect(PBL_DISPLAY_WIDTH - 50 - 10, 30, 50, 42));
    text_layer_set_background_color(s_delta_layer, GColorClear);
    text_layer_set_text_color(s_delta_layer, GColorBlack);
    text_layer_set_font(s_delta_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_delta_layer, GTextAlignmentRight);
    // layer_add_child(root_layer, text_layer_get_layer(s_delta_layer));

    // Graph - positioned from left edge to 2/3 of screen, plus arrow space
    const int graph_width = (PBL_DISPLAY_WIDTH * 2) / 3;
    const int arrow_space = PBL_DISPLAY_WIDTH - graph_width; // Remaining 1/3 for arrow
    s_graph_layer = layer_create(GRect(0, 35, graph_width + arrow_space, 100));
    layer_set_update_proc(s_graph_layer, graph_layer_update_proc);
    layer_add_child(root_layer, s_graph_layer);

    // Current time - bottom, centered
    s_time_layer = text_layer_create(GRect(0, 105, PBL_DISPLAY_WIDTH, 42));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorBlack);
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(root_layer, text_layer_get_layer(s_time_layer));

    // Date - below time
    s_date_layer = text_layer_create(GRect(0, 140, PBL_DISPLAY_WIDTH, 24));
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
    layer_destroy(s_graph_layer);
}

void minute_tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_displayed_time_and_date();
    update_displayed_time_ago();
}

static void new_xdrip_data_callback(DictionaryIterator *iter, void *context) {

    APP_LOG(APP_LOG_LEVEL_INFO, "incoming dict size: %lu", dict_size(iter));

    // Check for timestamp (always present in data messages)
    Tuple *timestamp_tuple = dict_find(iter, KEY_BG_TIMESTAMP);
    if (timestamp_tuple) {
        s_bg_timestamp = timestamp_tuple->value->uint32;

        // BG as string
        Tuple *bg_tuple = dict_find(iter, KEY_BG_STRING);
        if (bg_tuple) {
            safe_strncpy(s_bg_string, bg_tuple->value->cstring, sizeof(s_bg_string));
        }

        // Delta as string
        Tuple *delta_tuple = dict_find(iter, KEY_DELTA_STRING);
        if (delta_tuple) {
            safe_strncpy(s_delta_string, delta_tuple->value->cstring, sizeof(s_delta_string));
        }

        // Graph data
        Tuple *graph_tuple = dict_find(iter, KEY_GRAPH_DATA);
        if (graph_tuple && graph_tuple->length >= 6) {
            const uint8_t *data = graph_tuple->value->data;

            // Parse reference timestamp (4 bytes, little-endian)
            s_graph_ref_timestamp = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);

            // Parse count (uint16, little-endian)
            s_graph_count = data[4] | (data[5] << 8);
            APP_LOG(APP_LOG_LEVEL_INFO, "Raw count: %d", s_graph_count);
            if (s_graph_count > MAX_GRAPH_POINTS) {
                APP_LOG(APP_LOG_LEVEL_WARNING, "Count %d exceeds max %d, clamping", s_graph_count,
                        MAX_GRAPH_POINTS);
                s_graph_count = MAX_GRAPH_POINTS;
            }

            // Verify we have enough data (6 header + count*2 offsets + count*1 bg values)
            int expected_size = 6 + (s_graph_count * 3);
            APP_LOG(APP_LOG_LEVEL_INFO, "Graph: count=%d, expected=%d bytes, actual=%u bytes",
                    s_graph_count, expected_size, graph_tuple->length);

            if (graph_tuple->length >= expected_size) {
                // Parse time offsets (uint16, little-endian)
                for (int i = 0; i < s_graph_count; i++) {
                    int offset_idx = 6 + (i * 2);
                    s_graph_offsets[i] = data[offset_idx] | (data[offset_idx + 1] << 8);
                }

                // Parse BG values in "mg/dL / 2" units
                for (int i = 0; i < s_graph_count; i++) {
                    s_graph_bg_values[i] = data[6 + (s_graph_count * 2) + i];
                }

                APP_LOG(APP_LOG_LEVEL_INFO, "Received graph: ref_ts=%lu", s_graph_ref_timestamp);
                APP_LOG(APP_LOG_LEVEL_INFO, "First point: offset=%d min, bg=%d mg/dL",
                        s_graph_offsets[0], s_graph_bg_values[0] * 2);
                APP_LOG(APP_LOG_LEVEL_INFO, "Last point: offset=%d min, bg=%d mg/dL",
                        s_graph_offsets[s_graph_count - 1],
                        s_graph_bg_values[s_graph_count - 1] * 2);

                // Trigger graph redraw
                if (s_graph_layer) {
                    layer_mark_dirty(s_graph_layer);
                }
            } else {
                APP_LOG(APP_LOG_LEVEL_ERROR, "Graph data too short");
            }
        }

        // Graph high/low lines (stored as mg/dL / 2)
        Tuple *high_line_tuple = dict_find(iter, KEY_GRAPH_HIGH_LINE);
        if (high_line_tuple) {
            s_graph_high_line = high_line_tuple->value->uint8;
        }

        Tuple *low_line_tuple = dict_find(iter, KEY_GRAPH_LOW_LINE);
        if (low_line_tuple) {
            s_graph_low_line = low_line_tuple->value->uint8;
        }

        update_displayed_xdrip_data();
        update_displayed_time_ago();

        APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s, delta: %s", s_bg_string, s_delta_string);
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
    dict_write_uint32(iter, KEY_CAPABILITIES, CAP_BG | CAP_TREND_ARROW | CAP_DELTA);
    dict_write_uint8(iter, KEY_GRAPH_HOURS, GRAPH_HOURS);

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
    safe_strncpy(s_delta_string, TEST_DELTA_STRING, sizeof(s_delta_string));

    // Initialize test graph data
    s_graph_ref_timestamp = time(NULL) - (GRAPH_HOURS * 60 * 60);
    s_graph_count = TEST_GRAPH_COUNT;

    for (int i = 0; i < TEST_GRAPH_COUNT; i++) {
        // Offsets: 0, 5, 10, 15, ... 175 minutes
        s_graph_offsets[i] = i * 5;

        // BG values: create a wave pattern between 100-200 mg/dL (stored as mg/dL / 2)
        // Using simple sine-like pattern: 150 + 50*sin(i/6)
        int base_bg = 150;
        int variation = (i % 12) < 6 ? (i % 12) * 8 : (12 - (i % 12)) * 8;
        s_graph_bg_values[i] = (base_bg + variation - 24) / 2;
    }

    APP_LOG(APP_LOG_LEVEL_INFO, "Test mode: initialized graph with %d points", s_graph_count);
#endif
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    // A message was received, but had to be dropped
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped. Reason: %d", (int)reason);
}

void init(void) {
    app_message_register_inbox_received(new_xdrip_data_callback);

    // Register to be notified about inbox dropped events
    app_message_register_inbox_dropped(inbox_dropped_callback);

    app_message_open(/*in*/ 1024, /*out*/ 64);

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
