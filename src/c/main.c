// MiniMed -> Pebble watchface (proof of concept).
//
// Displays the current blood glucose value pushed from the Android bridge app
// over AppMessage, plus how long ago it arrived, a 2 h graph, and the current time/date.
// The trend projection is NOT taken from the pump; it's extrapolated on-watch from the recent BG data
// (issue #1) and drawn as a dotted line continuing the graph line from its latest point.

#include <pebble.h>

#include "protocol.h"

// --- Constants ---

// Dark theme by default: black window, white foreground, colored BG value. On B&W platforms
// (aplite/flint) colors render as black/white, so the value falls back to white there.
#define COLOR_WINDOW_BG GColorBlack
#define COLOR_FG        GColorWhite
// Folly (#FF0055), not pure red: the blue pulls it away from orange, so it stays distinct from the
// yellow at low backlight.
#define COLOR_BG_LOW    GColorFolly    // below the low line
#define COLOR_BG_OK     GColorGreen    // within range
#define COLOR_BG_HIGH   GColorYellow   // above the high line
#define COLOR_PUMP_OFFLINE GColorRed   // the pump-offline cross
#define COLOR_MEAL      GColorVividCerulean // the meal fork (white on B&W)

// Graph config
#define GRAPH_HOURS 2  // Hours of graph data
#define STROKE_WIDTH 3 // Graph stroke width in pixels
#define STROKE_OFFSET (STROKE_WIDTH / 2)
#define MAX_GRAPH_POINTS 300 // Enough for 24 h @ 5 min + headroom

// --- Messy stuff, to be cleaned up ---

// Show "---" instead of a stale value once the last reading is this old. CGM cadence is 5 min, so
// keep the last value on screen across a couple of missed readings before giving up on it.
// MUST match the bridge's STALE_SECONDS (minimed-pebble-bridge BridgeForegroundService) so the watch
// and the phone status-bar icon go stale at the same time.
#define STALE_MINUTES 15

// Y-axis in "mg/dL / 2" wire units. The bottom is fixed at 2.2 mmol/L. The top follows the data in
// 2 mmol/L steps between 12 and 16 mmol/L (a normal day gets the most pixels per mmol/L, a high
// one still fits); anything above 16 clamps to the top edge. See update_axis_max().
#define GRAPH_VALUE_MIN 20
#define GRAPH_VALUE_MAX 144        // 16 mmol/L, the highest the axis goes
#define GRAPH_AXIS_DEFAULT_MAX 108 // 12 mmol/L, the lowest the top goes
#define GRAPH_AXIS_STEP 18         // ~2 mmol/L
// Don't connect points more than this far apart (a sensor gap draws as a break, not a straight line).
#define GRAPH_GAP_THRESHOLD_MINUTES 15

// The graph occupies most of the screen width; the small right region shows the extrapolated trend projection
#define GRAPH_WIDTH_NUM 7 // graph width = screen width * NUM/DEN; the rest is for trend projection
#define GRAPH_WIDTH_DEN 8

// The layer is taller than the value band so a projection leaving a reading near the top or bottom of
// the range has somewhere to go instead of being clipped away (its length is clamped to the layer, so
// it shortens rather than vanishing). Asymmetric: more spare screen below the band than above it.
#define GRAPH_PAD_TOP 6
#define GRAPH_PAD_BOTTOM 8
// Axes, trace and projection all live in one layer, so there is a single coordinate space and the
// projection pivot cannot drift off the trace. It sits behind the time/BG text, which stay on top.

// Trend projection (issue #1). Extrapolated on-watch from recent BG, NOT read from the pump. Its angle is
// the graph's own visual slope (same px/min and px/value as the trace), so it lies tangent to how the
// line would continue from the latest point — not an arbitrary rate->angle mapping. Drawn as a dotted line
// so it reads clearly as a projection, distinct from the solid data trace. See draw_projection for the
// slope estimator and why it was chosen.
#define TREND_MAX_GAP_MINUTES 15 // ignore the last two points if a sensor gap wider than this separates them
#define TREND_PROJ_LEN 12        // projection length start-to-end in px (clamped to stay inside the band)
#define TREND_PROJ_GAP 6         // gap (px) between the trace's last point and the projection start
#define TREND_DOT_COUNT 3        // dots drawn along the projection, spread over its (clamped) length

// Fonts: BG value at 42px bold — the bold weight renders a clearly visible decimal point (the
// Roboto 49 subset's period is a near-invisible dot, and the medium-numbers font's is too thin).
// Time stays 42px, secondary text bumps to 28.
//
// No bigger system font is a safe swap-in for a flat/no-trend state: Roboto-49 is the next size up
// and has exactly the near-invisible-dot problem this comment already warns about (confirmed by
// trying it -- v6 briefly used it and reintroduced the bug it was chosen to avoid). The BG value
// stays this one size regardless of whether the trend row below it is shown.
#define FONT_BG_VALUE     FONT_KEY_BITHAM_42_BOLD
#define FONT_SECONDARY    FONT_KEY_GOTHIC_28_BOLD
#define FONT_TIME         FONT_KEY_BITHAM_42_BOLD
#define BG_ROW_H 42 // FONT_BG_VALUE's own line height

// Trend arrow row: a thin strip directly below the BG value, 1-2 small arrows side by side (never
// stacked). Shown only for a valid, non-flat trend; hidden otherwise (flat/invalid/no reading).
#define TREND_ROW_H   16
#define TREND_ROW_GAP 2

// Vertical padding at the top and bottom so the content does not hug the bezel edge.
#define LAYOUT_TOP_GAP    12
#define LAYOUT_BOTTOM_GAP 6

// Status strip: a full-width opaque white band hugging the status text, sitting low over the graph so
// its uppercase letters land ~2px above the time. Custom-drawn (not a TextLayer background) so the
// band can be full width yet vertically tight to the caps. Layer-local coords, like every other layer
// here; it paints only the band + text, leaving the rest transparent so the graph shows through.
#define STATUS_FONT FONT_KEY_GOTHIC_18_BOLD
#define STATUS_H 24     // one line of STATUS_FONT, with room for descenders
#define STATUS_CAP_H 11 // STATUS_FONT's cap height (measured)

static Window *s_window;
static TextLayer *s_bg_layer;
static TextLayer *s_ago_layer;
static TextLayer *s_iob_layer;
static Layer *s_status_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static Layer *s_graph_layer; // axes, trace and projection all draw here
static int s_graph_band_h;   // px the BG value range maps onto; sized to the screen in window_load
static Layer *s_pump_layer;  // pump connection indicator: cross while offline, blank while connected
static Layer *s_trend_row_layer; // pump-provided trend arrow row (KEY_TREND_ARROW); blank when absent
static int s_caps_top_y; // cap top of the BG value at its normal (small-font) size; set once in window_load
static Layer *s_debug_layer; // draws the debug outlines below, nothing else

// Debug outlines. Frames are registered rather than layers, so a TextLayer, a custom layer and a
// region that is no layer at all (the graph's value band) all work the same way. Boxes are switched
// on by commenting the add_debug_outline() calls in window_load in or out.
#define DEBUG_MAX_OUTLINES 8
static GRect s_debug_outlines[DEBUG_MAX_OUTLINES];
static unsigned s_num_debug_outlines;

// Latest reading from the phone.
static char s_bg_string[16] = "";   // whatever the phone last sent; "" until the first reading arrives
static uint32_t s_bg_timestamp = 0; // 0 => never received

static char s_iob_string[8] = "";     // raw IOB units from phone, e.g. "2.5"; empty = unknown
static char s_status_string[20] = ""; // pump status, e.g. "SUSPENDED"; empty = normal
static uint32_t s_status_start = 0;
static uint32_t s_status_end = 0;

// Pump link state (KEY_PUMP_CONNECTED). Offline is the correct default until the sender says
// otherwise -- not "unknown" -- so a relaunch shows the cross rather than carrying a stale
// "connected" from before.
static bool s_pump_connected = false;

// Pump-provided trend arrow (KEY_TREND_ARROW). Distinct from the on-watch trend PROJECTION drawn on
// the graph above (issue #1, extrapolated locally) -- this one comes straight from the pump's own
// rate-of-change reading. Invalid (no arrow shown) until the sender says otherwise, and whenever a
// reading arrives with no trend field: the sender omits the key rather than send TREND_UNKNOWN, so
// "key absent" is the only signal that the arrow should go away.
// Latest meal (KEY_MEAL_CARBS/KEY_MEAL_TIMESTAMP): a fork mark on the graph at the time it was
// recorded, with the carb amount beside it, for as long as that time is inside the graph window.
// The sender's own forecast of the glucose 30 minutes after the newest reading (KEY_PREDICTED_BG).
// When present it sets the projection's slope; without it the projection extrapolates the last two
// points.
#define PREDICTION_HORIZON_MIN 30
static bool s_pred_valid = false;
static uint16_t s_pred_mgdl = 0;

static bool s_meal_valid = false;
static uint16_t s_meal_grams = 0;
static uint32_t s_meal_timestamp = 0;

static bool s_trend_valid = false;
static uint8_t s_trend_arrow = TREND_UNKNOWN;

// Graph data (all BG values in "mg/dL / 2" wire units).
static uint32_t s_graph_ref_timestamp = 0;
static uint16_t s_graph_count = 0;
static uint16_t s_graph_offsets[MAX_GRAPH_POINTS];  // minutes since ref_timestamp
static uint8_t s_graph_bg_values[MAX_GRAPH_POINTS]; // mg/dL / 2
static uint8_t s_graph_high_line = 90;              // 10.0 mmol/L (default; phone may override)
static uint8_t s_graph_low_line = 36;               // 4.0 mmol/L

static char s_ago_display[16];
static char s_iob_display[12];
static char s_time_display[8];
static char s_date_display[16];

static void safe_strncpy(char *dst, const char *src, size_t dst_size) {
    if (dst_size > 0) {
        strncpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

// Like safe_strncpy but the destination size is inferred with sizeof. Compile-time error if dst is a
// pointer rather than an array, which is the case sizeof would silently get wrong: the negative array
// size below is only well-formed when the two types differ. __typeof__ rather than typeof because the
// SDK compiles with -std=c99, where the unprefixed spelling isn't a keyword.
#define STRCPY(dst, src)                                                                                               \
    ((dst)[0] = (dst)[0],                                                                                              \
     (void)sizeof(char[1 - 2 * __builtin_types_compatible_p(__typeof__(dst), __typeof__(&(dst)[0]))]),                 \
     safe_strncpy(dst, src, sizeof(dst)))

static bool has_reading(void) { return s_bg_timestamp != 0; }

static void draw_layer_outline(GContext *ctx, GRect bounds) {
    const int16_t left = bounds.origin.x, top = bounds.origin.y;
    const int16_t right = left + bounds.size.w - 1, bottom = top + bounds.size.h - 1;
    for (int16_t x = left; x <= right; x += 2) {
        graphics_draw_pixel(ctx, GPoint(x, top));
        graphics_draw_pixel(ctx, GPoint(x, bottom));
    }
    for (int16_t y = top; y <= bottom; y += 2) {
        graphics_draw_pixel(ctx, GPoint(left, y));
        graphics_draw_pixel(ctx, GPoint(right, y));
    }
}

// A TextLayer owns its update proc, so its box has to be drawn from somewhere else: this layer sits
// over the whole window, on top of everything, and outlines the frames switched on in
// s_debug_outlines. Frames are parent-relative and this layer spans the root, so they need no
// translation.
[[maybe_unused]] static void add_debug_outline(GRect frame) {
    if (s_num_debug_outlines < DEBUG_MAX_OUTLINES) {
        s_debug_outlines[s_num_debug_outlines++] = frame;
    }
}

static void debug_layer_update_proc(Layer *layer, GContext *ctx) {
    // Explicit fg color: the default stroke color is black, which is invisible on the dark background.
    graphics_context_set_stroke_color(ctx, COLOR_FG);
    for (unsigned i = 0; i < s_num_debug_outlines; i++) {
        draw_layer_outline(ctx, s_debug_outlines[i]);
    }
}

// Minutes since the current reading, or -1 if we've never received one. A reading dated in the future
// (clock skew) reads as 0.
static int minutes_ago(void) {
    if (!has_reading()) {
        return -1;
    }
    int secs = (int)(time(NULL) - (time_t)s_bg_timestamp);
    return secs < 0 ? 0 : secs / 60;
}

// True once the current reading is too old to trust (no fresh push for STALE_MINUTES). During a pump
// outage no message arrives to clear the display, so this is re-evaluated from the minute tick.
static bool is_stale(void) { return has_reading() && minutes_ago() >= STALE_MINUTES; }

// Map the latest BG (mg/dL/2) to a display color by the high/low threshold lines.
static GColor prv_bg_color(void) {
    if (s_graph_count == 0) {
        return COLOR_FG;
    }
    const uint8_t bg = s_graph_bg_values[s_graph_count - 1];
    if (bg < s_graph_low_line) {
        return COLOR_BG_LOW;
    }
    if (bg > s_graph_high_line) {
        return COLOR_BG_HIGH;
    }
    return COLOR_BG_OK;
}

// Pixels from layer top to font cap height (defined near window_load, which is its main user).
int cap_offset(const char *font_key);

// Switch the BG value between its normal and "big" font/frame, and show/hide the trend row below
// it, based on whether there's currently a trend worth a row for. No trend (flat, invalid, or no
// reading yet): BG grows into FONT_BG_VALUE_BIG to fill the space the row would have used, rather
// than leaving it blank. This never depends on the BG string's rendered width -- the row is full
// width and independently centered, so it cannot collide with the ago/IOB corners the way an
// icon placed beside the digits could.
static void update_bg_trend_layout(void) {
    if (!s_bg_layer) {
        return;
    }
    // BG value's own font/frame never change (see FONT_BG_VALUE's comment) -- only the trend row
    // below it shows or hides.
    const bool show_trend_row = s_trend_valid && s_trend_arrow != TREND_FLAT && s_trend_arrow != TREND_UNKNOWN;
    const int y = s_caps_top_y - cap_offset(FONT_BG_VALUE);

    if (s_trend_row_layer) {
        layer_set_hidden(s_trend_row_layer, !show_trend_row);
        if (show_trend_row) {
            layer_set_frame(s_trend_row_layer,
                            GRect(0, y + BG_ROW_H + TREND_ROW_GAP, PBL_DISPLAY_WIDTH, TREND_ROW_H));
        }
        layer_mark_dirty(s_trend_row_layer);
    }
}

static void update_bg_display(void) {
    // Stale -> blank the number rather than showing a value that hasn't updated in a while (a stale BG
    // sat on screen for ~8 h during an overnight outage). The "ago" label still conveys how old it is.
    // While fresh, show verbatim what the phone sent: "---" appears only when the phone sends it (the
    // pump has no sensor value), so the watch never invents it -- every "---" mirrors the pump.
    // Guard the layer: a data message can arrive before window_load creates it (the on-watch sender
    // injects with zero latency, unlike a phone's), and text_layer_set_text(NULL,..) hard-faults.
    if (s_bg_layer) {
        text_layer_set_text(s_bg_layer, is_stale() ? "" : s_bg_string);
        // Color the value by range: red below the low line, yellow above the high line, green in
        // between. Uses the latest graph point (wire units mg/dL/2, same scale as the threshold
        // lines); falls back to the default foreground if there is no graph yet. On B&W platforms
        // every color maps to white so the value always stays readable.
        text_layer_set_text_color(s_bg_layer, PBL_IF_COLOR_ELSE(prv_bg_color(), GColorWhite));
    }
}

static void update_ago_display(void) {
    int mins = minutes_ago();

    if (mins < 6) {
        // Hide when fresh
        s_ago_display[0] = '\0';
    } else if (mins < 60) {
        // Minutes ago
        snprintf(s_ago_display, sizeof(s_ago_display), "%dm", mins);
    } else {
        // Hours ago
        snprintf(s_ago_display, sizeof(s_ago_display), "%dh", mins / 60);
    }

    if (s_ago_layer)
        text_layer_set_text(s_ago_layer, s_ago_display);
}

static void update_iob_display(void) {
    if (s_iob_string[0] == '\0' || is_stale()) {
        s_iob_display[0] = '\0';
    } else {
        snprintf(s_iob_display, sizeof(s_iob_display), "%sU", s_iob_string);
    }

    if (s_iob_layer)
        text_layer_set_text(s_iob_layer, s_iob_display);
}

// An X while the pump link is down, nothing while it's up: connected is the normal state, and a
// "connected" dot was hard to tell from the X at a glance. The link can be up while no CGM readings
// arrive (sensor warm-up, sensor-pump dropout), so the X is what separates that from a pump outage.
static void pump_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_pump_connected) {
        return;
    }
    const GRect bounds = layer_get_bounds(layer);
    const GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
    const int16_t r = 6;

    // Red would render black on B&W platforms, invisible on the black background.
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(COLOR_PUMP_OFFLINE, COLOR_FG));
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, GPoint(center.x - r, center.y - r), GPoint(center.x + r, center.y + r));
    graphics_draw_line(ctx, GPoint(center.x - r, center.y + r), GPoint(center.x + r, center.y - r));
}

static void update_pump_indicator(void) {
    if (s_pump_layer)
        layer_mark_dirty(s_pump_layer);
}

// One "^" (up) or "v" (down) chevron, apex at (cx, apex_y), base at base_y.
static void draw_chevron(GContext *ctx, int16_t cx, int16_t apex_y, int16_t base_y, int16_t half_w) {
    graphics_draw_line(ctx, GPoint(cx - half_w, base_y), GPoint(cx, apex_y));
    graphics_draw_line(ctx, GPoint(cx, apex_y), GPoint(cx + half_w, base_y));
}

// The pump's own rate-of-change reading, drawn as 1-2 arrows SIDE BY SIDE in a row below the BG
// value (never stacked, and never beside the digits -- both were tried and both could collide with
// something depending on string width). Never derived from the graph on-watch -- see
// s_trend_arrow's comment. update_bg_trend_layout hides this layer entirely for flat/invalid/no
// reading, so by the time this proc runs it only ever needs to draw an up or down shape.
static void trend_row_update_proc(Layer *layer, GContext *ctx) {
    const bool up = (s_trend_arrow == TREND_SLANT_UP || s_trend_arrow == TREND_UP ||
                     s_trend_arrow == TREND_DOUBLE_UP || s_trend_arrow == TREND_TRIPLE_UP);
    const bool down = (s_trend_arrow == TREND_SLANT_DOWN || s_trend_arrow == TREND_DOWN ||
                       s_trend_arrow == TREND_DOUBLE_DOWN || s_trend_arrow == TREND_TRIPLE_DOWN);
    if (!up && !down) {
        return; // shouldn't happen while visible, but never draw garbage for a future arrow value
    }

    const GRect bounds = layer_get_bounds(layer);
    const int16_t cy = bounds.size.h / 2;
    const int16_t half_w = 5;
    // Slant is still a proper V, just a shallower one (shorter chevron_h relative to half_w) --
    // that's what distinguishes "gently" rising/falling from a full arrow, not a different shape.
    const bool slant = (s_trend_arrow == TREND_SLANT_UP || s_trend_arrow == TREND_SLANT_DOWN);
    const int16_t chevron_h = slant ? 4 : 8;

    graphics_context_set_stroke_color(ctx, COLOR_FG);
    graphics_context_set_stroke_width(ctx, 2);

    const int n = (s_trend_arrow == TREND_TRIPLE_UP || s_trend_arrow == TREND_TRIPLE_DOWN) ? 3
                 : (s_trend_arrow == TREND_DOUBLE_UP || s_trend_arrow == TREND_DOUBLE_DOWN) ? 2
                                                                                            : 1;
    const int16_t spacing = 2 * half_w + 4; // gap between adjacent arrows' centers
    const int16_t total_w = spacing * (n - 1);
    const int16_t first_cx = bounds.size.w / 2 - total_w / 2;
    for (int i = 0; i < n; i++) {
        const int16_t cx = first_cx + i * spacing;
        const int16_t apex_y = up ? cy - chevron_h / 2 : cy + chevron_h / 2;
        const int16_t base_y = up ? cy + chevron_h / 2 : cy - chevron_h / 2;
        draw_chevron(ctx, cx, apex_y, base_y, half_w);
    }
}

static void update_trend_indicator(void) {
    update_bg_trend_layout();
}

static void update_time_and_date(void) {
    time_t now = time(NULL);
    struct tm *time = localtime(&now);

    strftime(s_time_display, sizeof(s_time_display), clock_is_24h_style() ? "%H:%M" : "%I:%M", time);

#ifdef FULL_WEEKDAY_DATE
    // "Wednesday, 16" (set in local_defines.txt, see wscript)
    strftime(s_date_display, sizeof(s_date_display), "%A, %d", time);
#else
    if (time->tm_mday < 10) {
        // %e = " 9" with a space or "10"
        strftime(s_date_display, sizeof(s_date_display), "%a%e   W%V", time);
    } else {
        // %d = "09" with a zero, or "10"
        strftime(s_date_display, sizeof(s_date_display), "%a %d   W%V", time);
    }
#endif

    // Guarded for the same reason as the BG/ago/IOB layers: the tick is subscribed before
    // window_load creates the layers, so a tick landing in the launch gap would hit
    // text_layer_set_text(NULL,..) and hard-fault. window_load re-renders, so nothing is lost.
    if (s_time_layer)
        text_layer_set_text(s_time_layer, s_time_display);
    if (s_date_layer)
        text_layer_set_text(s_date_layer, s_date_display);
}

// The status label overlays the bottom of the graph as an opaque strip, but only when a status is
// active; otherwise it's hidden so the full graph shows.
static void update_status_display(void) {
    if (s_status_layer)
        layer_mark_dirty(s_status_layer);
}

// Paints only the band + text (when a status is active); everything else stays transparent so the
// graph below shows through. All coords are layer-relative.
static void status_layer_update_proc(Layer *layer, GContext *ctx) {
    if (s_status_string[0] == '\0') {
        return;
    }

    const int16_t w = layer_get_bounds(layer).size.w;

    char status_with_timer[sizeof(s_status_string) + 6]; // " hh:mm" is 6 chars

    bool show_timer = false;
    uint32_t hours = 0;
    uint32_t minutes = 0;

    if (s_status_start != 0 && s_status_end == 0) {
        // Display a count-up timer from the status start time
        show_timer = true;
        const uint32_t now = time(NULL);
        if (now > s_status_start) {
            const uint32_t seconds = now - s_status_start;
            hours = seconds / 3600;
            minutes = (seconds / 60) % 60;
        }
    } else if (s_status_end != 0 && s_status_start == 0) {
        // Display a count-down timer to the status end time
        show_timer = true;
        const uint32_t now = time(NULL);
        if (s_status_end > now) {
            const uint32_t seconds = s_status_end - now;
            hours = seconds / 3600;
            minutes = (seconds / 60) % 60;
        }
    }

    // Exception: I don't want to show the timer for "SUSPENDED"
    if (strcmp(s_status_string, "SUSPENDED") == 0) {
        show_timer = false;
    }

    if (show_timer) {
        if (hours > 99)
            hours = 99;  // Guarantee max 2 digits
        snprintf(status_with_timer, sizeof(status_with_timer), "%s %lu:%02lu", s_status_string, hours, minutes);
    } else {
        snprintf(status_with_timer, sizeof(status_with_timer), "%s", s_status_string);
    }

    graphics_context_set_text_color(ctx, COLOR_FG);
    graphics_draw_text(ctx, status_with_timer, fonts_get_system_font(STATUS_FONT), GRect(0, 0, w, STATUS_H),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Map a BG value (mg/dL / 2) to a y inside the graph layer, clamping to the fixed range. The only place
// that knows where the value band sits within the layer, so axes, trace and projection cannot disagree.
static int s_axis_max = GRAPH_AXIS_DEFAULT_MAX; // current top of the y-axis, wire units

static int graph_y(int bg) {
    if (bg < GRAPH_VALUE_MIN)
        bg = GRAPH_VALUE_MIN;
    if (bg > s_axis_max)
        bg = s_axis_max;
    return GRAPH_PAD_TOP + s_graph_band_h - ((bg - GRAPH_VALUE_MIN) * s_graph_band_h) / (s_axis_max - GRAPH_VALUE_MIN);
}

// Index range of the points inside the visible window (older ones have scrolled off the left edge).
static int first_visible_point(void) {
    const uint32_t now = time(NULL);
    for (int i = 0; i < s_graph_count; i++) {
        const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[i] * 60;
        if ((int64_t)now - (int64_t)pt_ts <= (int64_t)GRAPH_HOURS * 3600)
            return i;
    }
    return s_graph_count;
}

// Fit the top of the axis to the visible readings: the peak rounded up to the next step, never below
// GRAPH_AXIS_DEFAULT_MAX or above GRAPH_VALUE_MAX. Must run before anything calls graph_y().
static void update_axis_max(void) {
    int peak = 0;
    for (int i = first_visible_point(); i < s_graph_count; i++) {
        if (s_graph_bg_values[i] > peak)
            peak = s_graph_bg_values[i];
    }
    int top = GRAPH_AXIS_DEFAULT_MAX;
    while (top < peak && top < GRAPH_VALUE_MAX)
        top += GRAPH_AXIS_STEP;
    s_axis_max = top;
}

// A wire value as mmol/L text: whole numbers bare ("10"), otherwise one decimal ("7.4"). Same
// conversion constant as the firmware, so it matches the pump's own display.
static int wire_to_tenths(int wire) {
    return (wire * 2 * 100000 + 90091) / 180182;
}

static void format_mmol(char *out, size_t size, int wire) {
    const int tenths = wire_to_tenths(wire);
    if (tenths % 10 == 0)
        snprintf(out, size, "%d", tenths / 10);
    else
        snprintf(out, size, "%d.%d", tenths / 10, tenths % 10);
}

// The window's highest reading as a tiny number, no plate so it never hides the trace or the meal
// marker. The only text on the graph. Ties (by displayed value) go to the newest point.
#define PEAK_LABEL_W 26
#define PEAK_LABEL_H 14
static void draw_peak_label(GContext *ctx, GRect bounds) {
    const int first = first_visible_point();
    if (first >= s_graph_count)
        return;
    int hi = first;
    for (int i = first; i < s_graph_count; i++) {
        if (wire_to_tenths(s_graph_bg_values[i]) >= wire_to_tenths(s_graph_bg_values[hi]))
            hi = i;
    }

    const int w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const uint32_t now = time(NULL);
    const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[hi] * 60;
    const int mins_ago = (int)(((int64_t)now - (int64_t)pt_ts) / 60);
    int x = w - (mins_ago * w) / (GRAPH_HOURS * 60);
    if (x < PEAK_LABEL_W / 2)
        x = PEAK_LABEL_W / 2;
    if (x > w - PEAK_LABEL_W / 2)
        x = w - PEAK_LABEL_W / 2;

    // Above its point, or below it when the point is at the top of the band.
    const int y = graph_y(s_graph_bg_values[hi]);
    int top = y - STROKE_WIDTH - PEAK_LABEL_H;
    if (top < 0)
        top = y + STROKE_WIDTH;

    char text[8];
    format_mmol(text, sizeof(text), s_graph_bg_values[hi]);
    const GRect box = GRect(x - PEAK_LABEL_W / 2, top, PEAK_LABEL_W, PEAK_LABEL_H);
    graphics_context_set_text_color(ctx, COLOR_FG);
    graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(box.origin.x, box.origin.y - 3, box.size.w, box.size.h + 3),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_graph_axes(GContext *ctx, GRect bounds) {
    // Explicit fg color: the default stroke color is black, which is invisible on the dark
    // background (draw_bg_graph sets COLOR_FG later, but the axes draw first).
    graphics_context_set_stroke_color(ctx, COLOR_FG);
    const int width = bounds.size.w;
    const int hi_y = graph_y(s_graph_high_line);
    const int lo_y = graph_y(s_graph_low_line);

    // Draw high and low lines
    graphics_draw_line(ctx, GPoint(0, hi_y), GPoint(width, hi_y));
    graphics_draw_line(ctx, GPoint(0, lo_y), GPoint(width, lo_y));

    // Draw hourly tick marks
    const int tick_length = 5; // Pixel length
    const int half_tick = tick_length / 2;
    for (int n = 1; n <= GRAPH_HOURS; n++) {
        const int x = width * n / 3;
        graphics_draw_line(ctx, GPoint(x, hi_y - half_tick), GPoint(x, hi_y + half_tick));
        graphics_draw_line(ctx, GPoint(x, lo_y - half_tick), GPoint(x, lo_y + half_tick));
    }
}

static void draw_bg_graph(GContext *ctx, GRect bounds) {
    if (s_graph_count == 0) {
        return;
    }

    // Time-axis width; the layer spans the full screen so edge points aren't clipped.
    const int16_t w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN; // todo something better here
    const uint32_t now = time(NULL);
    const int graph_minutes = GRAPH_HOURS * 60;

    graphics_context_set_stroke_color(ctx, COLOR_FG);
    graphics_context_set_stroke_width(ctx, STROKE_WIDTH);

    bool have_prev = false;
    int prev_x = 0, prev_y = 0, prev_off = 0;

    for (int i = 0; i < s_graph_count; i++) {
        const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[i] * 60;
        const int mins_ago = (int)(((int64_t)now - (int64_t)pt_ts) / 60);
        const int x = w - (mins_ago * w) / graph_minutes;
        const int y = graph_y(s_graph_bg_values[i]);
        // Offsets ascend, so both gaps are simple subtractions.
        const bool join_prev = have_prev && (int)s_graph_offsets[i] - prev_off <= GRAPH_GAP_THRESHOLD_MINUTES;
        const bool join_next = i + 1 < s_graph_count &&
                               (int)s_graph_offsets[i + 1] - (int)s_graph_offsets[i] <= GRAPH_GAP_THRESHOLD_MINUTES;

        // Data gaps wider than GRAPH_GAP_THRESHOLD_MINUTES render as gaps.
        if (join_prev) {
            graphics_draw_line(ctx, GPoint(prev_x, prev_y), GPoint(x, y));
        } else if (!join_next) {
            // Gap on both sides: Draw an isolated dot
            graphics_context_set_fill_color(ctx, COLOR_FG);
            graphics_fill_rect(ctx, GRect(x - STROKE_OFFSET, y - STROKE_OFFSET, STROKE_WIDTH, STROKE_WIDTH), 0,
                               GCornerNone);
        }

        have_prev = true;
        prev_x = x;
        prev_y = y;
        prev_off = (int)s_graph_offsets[i];
    }
}

// --- Trend projection (issue #1) --------------------------------------------------------------------

// The slope, in wire units (mg/dL / 2) per minute, from the last two points; false if there aren't two
// or a sensor gap separates them. Points are oldest->newest by index, so the newest reading is last.
static bool trend_slope(float *slope) {
    const int n = s_graph_count;
    if (n < 2)
        return false;
    const int dt = (int)s_graph_offsets[n - 1] - (int)s_graph_offsets[n - 2];
    if (dt <= 0 || dt > TREND_MAX_GAP_MINUTES)
        return false;
    *slope = (float)((int)s_graph_bg_values[n - 1] - (int)s_graph_bg_values[n - 2]) / (float)dt;
    return true;
}

// newlib's sqrtf is unusable here: its literal pool holds absolute pointers into .text, and the
// Pebble app loader only relocates .rel.data and .got entries, so those pointers keep their
// link-time values. Reading them hard-faults on aplite/basalt/chalk/diorite (it happens to land on
// mapped memory on flint/emery/gabbro). Newton on a float needs no constant table.
static float sqrtf_local(float x) {
    if (x <= 0.0f)
        return 0.0f;
    float r = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 20; i++)
        r = 0.5f * (r + x / r);
    return r;
}

// The projection: a dotted line running from the latest point at the graph's own visual slope. Direction:
// over one minute the trace moves px_per_min right and slope*px_per_wire in y (screen y grows downward, so
// a rising slope points up). This makes the line tangent to how the trace would continue from the latest
// point, at the same scale as the graph. It starts TREND_PROJ_GAP past the pivot so there's a clear break
// between the data (solid trace) and the extrapolation (dotted line).
static void trend_draw_projection(GContext *ctx, GRect bounds, GPoint pivot, float slope, float px_per_min,
                                  float px_per_wire) {
    const float vx = px_per_min;
    const float vy = -slope * px_per_wire;
    const float mag = sqrtf_local(vx * vx + vy * vy);
    if (mag < 1e-6f)
        return;
    const float ux = vx / mag, uy = vy / mag; // unit vector along the projection

    // Clamp the length so the whole dotted line stays inside the layer. A steep projection from a reading
    // near the top/bottom of the range would otherwise run off the drawable area and vanish. Only the
    // length shrinks; the angle, which is the whole point, is preserved.
    float end = TREND_PROJ_GAP + TREND_PROJ_LEN;
    if (ux > 1e-6f) {
        const float d = (bounds.size.w - 1 - pivot.x) / ux;
        if (d < end)
            end = d;
    } else if (ux < -1e-6f) {
        const float d = (1 - pivot.x) / ux;
        if (d < end)
            end = d;
    }
    if (uy > 1e-6f) {
        const float d = (bounds.size.h - 1 - pivot.y) / uy;
        if (d < end)
            end = d;
    } else if (uy < -1e-6f) {
        const float d = (1 - pivot.y) / uy;
        if (d < end)
            end = d;
    }
    if (end <= TREND_PROJ_GAP)
        return; // too cramped for even a dot

    // Dots along the line (Pebble has no native dashed line): TREND_DOT_COUNT small filled squares,
    // each matching the trace's thickness, spread evenly from the start gap to the clamped end.
    graphics_context_set_fill_color(ctx, COLOR_FG);
    const float span = end - TREND_PROJ_GAP;
    for (int k = 0; k < TREND_DOT_COUNT; k++) {
        const float t = TREND_PROJ_GAP + span * k / (TREND_DOT_COUNT - 1);
        const int x = pivot.x + (int)(ux * t);
        const int y = pivot.y + (int)(uy * t);
        graphics_fill_rect(ctx, GRect(x - STROKE_OFFSET, y - STROKE_OFFSET, STROKE_WIDTH, STROKE_WIDTH), 0,
                           GCornerNone);
    }
}

static void draw_projection(GContext *ctx, GRect bounds) {
    // Estimator chosen after a July 2026 soak: the plain last-two-points slope. It's the most responsive
    // and, extended tangent to the trace, matched the eye best. The smoothed alternatives soaked
    // alongside it — an exp-weighted regression and a quadratic slope-at-latest — lagged real turns and
    // weren't worth their extra machinery at the 5-min sensor cadence. If a fancier estimator tempts you,
    // that's the history: it was tried and this won.
    // A forecast from the sender replaces that: the slope is the chord to its 30-minute value.
    if (s_graph_count < 1)
        return;
    float slope;
    if (s_pred_valid) {
        const float newest_wire = (float)s_graph_bg_values[s_graph_count - 1];
        slope = ((float)s_pred_mgdl / 2.0f - newest_wire) / (float)PREDICTION_HORIZON_MIN;
    } else if (!trend_slope(&slope)) {
        return;
    }

    // Don't extrapolate from stale data — no projection rather than a misleading one. (The BG number keeps
    // showing the last value once stale; the projection doesn't, since extrapolating from old points misleads.)
    const uint32_t now = time(NULL);
    const uint32_t newest_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[s_graph_count - 1] * 60;
    const int age_min = (int)(((int64_t)now - (int64_t)newest_ts) / 60);
    if (age_min >= STALE_MINUTES)
        return;

    // Anchor on the newest reading's ACTUAL position on the trace, so the projection is a true continuation
    // (collinear with the last segment) rather than a parallel-shifted copy — the point drifts left as it
    // ages, and the projection follows. newest_x and graph_y() are exactly what draw_bg_graph uses to plot
    // that point, so the pivot lands on it by construction. The graph's own px/min and px/value set the
    // angle; the time axis is the layer's left NUM/DEN and the projection runs into the rest.
    const int graph_w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const int graph_minutes = GRAPH_HOURS * 60;
    const int newest_x = graph_w - (age_min * graph_w) / graph_minutes;
    const float px_per_min = (float)graph_w / graph_minutes;
    const float px_per_wire = (float)s_graph_band_h / (s_axis_max - GRAPH_VALUE_MIN);
    const GPoint pivot = GPoint(newest_x, graph_y(s_graph_bg_values[s_graph_count - 1]));
    trend_draw_projection(ctx, bounds, pivot, slope, px_per_min, px_per_wire);
}

// A fork mark at the meal's time along the top of the value band (above almost every reading),
// with the carbs in grams beside it. Placed on the same time axis as the trace.
#define MEAL_ICON_H 11
#define MEAL_TEXT_W 34
static void draw_meal(GContext *ctx, GRect bounds) {
    if (!s_meal_valid) {
        return;
    }
    const uint32_t now = time(NULL);
    const int age_min = now > s_meal_timestamp ? (int)((now - s_meal_timestamp) / 60) : 0;
    const int graph_minutes = GRAPH_HOURS * 60;
    if (age_min > graph_minutes) {
        return;
    }
    const int graph_w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const int x = graph_w - (age_min * graph_w) / graph_minutes;
    const int y = GRAPH_PAD_TOP + 1; // top of the band: the bottom is under the status strip

    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(COLOR_MEAL, COLOR_FG));
    graphics_fill_rect(ctx, GRect(x - 4, y, 2, 5), 0, GCornerNone);     // tines
    graphics_fill_rect(ctx, GRect(x - 1, y, 2, 5), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(x + 2, y, 2, 5), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(x - 4, y + 5, 8, 2), 0, GCornerNone); // base
    graphics_fill_rect(ctx, GRect(x - 1, y + 7, 2, 4), 0, GCornerNone); // handle

    char label[8];
    snprintf(label, sizeof(label), "%u", (unsigned)s_meal_grams);
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(COLOR_MEAL, COLOR_FG));
    const bool right = x + 6 + MEAL_TEXT_W <= bounds.size.w;
    graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(right ? x + 6 : x - 6 - MEAL_TEXT_W, y - 3, MEAL_TEXT_W, 16),
                       GTextOverflowModeTrailingEllipsis, right ? GTextAlignmentLeft : GTextAlignmentRight,
                       NULL);
}

// Nothing to plot until the sender has delivered a reading (and, with it, whatever history it has).
static void draw_no_data(GContext *ctx, GRect bounds) {
    graphics_context_set_text_color(ctx, COLOR_FG);
    const GRect box = GRect(0, (bounds.size.h - 28) / 2, bounds.size.w, 28);
    graphics_draw_text(ctx, "No data", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), box,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Axes behind the trace, projection on top of both.
static void graph_layer_update_proc(Layer *layer, GContext *ctx) {
    const GRect bounds = layer_get_bounds(layer);
    update_axis_max();
    draw_graph_axes(ctx, bounds);
    if (!has_reading()) {
        draw_no_data(ctx, bounds);
        return;
    }
    draw_bg_graph(ctx, bounds);
    draw_meal(ctx, bounds);
    draw_projection(ctx, bounds);
    draw_peak_label(ctx, bounds);
}

static void send_capability_announcement(void);

static void tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_time_and_date();
    update_ago_display(); // advances the staleness hint each minute
    // Re-run on the tick, not just on receipt: during an outage no message arrives, so this is what
    // blanks the BG/IOB once they cross STALE_MINUTES.
    update_bg_display();
    update_iob_display();

    // Self-heal a missed push: past the "fresh" window but not yet stale, re-announce every couple
    // of minutes. The sender answers an announcement with the latest reading, the same nudge that
    // leaving and re-entering the watchface gives.
    const int mins = minutes_ago();
    if (has_reading() && mins >= 6 && mins < STALE_MINUTES && (tick_time->tm_min % 2) == 0)
        send_capability_announcement();

    if (s_status_layer && (s_status_start != 0 || s_status_end != 0))
        layer_mark_dirty(s_status_layer);

    // Redraw the graph too: point x-positions are computed from the current time, so without this the
    // trace freezes between the 5-min pushes (doesn't creep left, old points don't fall off the edge).
    // TODO: Consider if this is worth it, probably eats some battery
    if (s_graph_layer)
        layer_mark_dirty(s_graph_layer); // trace scrolls and the projection goes stale together
}

// Graph wire format: [ref_ts u32 LE][count u16 LE][offset_min u16 LE ×n][bg u8 ×n]. Returns false and
// leaves the graph untouched if the blob is short, so a truncated message can't half-update the trace:
// everything is validated before anything is written. (Writing ref_ts before the length check plotted the
// previous message's points against a new reference, shifting the whole trace in time.)
static bool parse_graph_blob(const uint8_t *d, uint16_t len) {
    if (len < 6) {
        return false;
    }
    uint16_t count = d[4] | (d[5] << 8);
    if (count > MAX_GRAPH_POINTS)
        count = MAX_GRAPH_POINTS;
    if (len < (uint16_t)(6 + count * 3)) {
        return false;
    }
    s_graph_ref_timestamp = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
    for (int i = 0; i < count; i++) {
        const int o = 6 + i * 2;
        s_graph_offsets[i] = d[o] | (d[o + 1] << 8);
    }
    for (int i = 0; i < count; i++) {
        s_graph_bg_values[i] = d[6 + count * 2 + i];
    }
    s_graph_count = count;
    return true;
}

// Handle a new dictionary of AppMessage keys and values.
static void handle_dictionary(DictionaryIterator *iter, void *context) {

    // BG and timestamp
    Tuple *bg_tuple = dict_find(iter, KEY_BG_STRING);
    Tuple *ts_tuple = dict_find(iter, KEY_BG_TIMESTAMP);
    if (bg_tuple) {
        STRCPY(s_bg_string, bg_tuple->value->cstring);
    }
    if (ts_tuple) {
        s_bg_timestamp = ts_tuple->value->uint32;
    } else if (bg_tuple) {
        s_bg_timestamp = time(NULL); // fall back to arrival time
    }

    // IoB
    Tuple *iob_tuple = dict_find(iter, KEY_IOB_STRING);
    if (iob_tuple) {
        STRCPY(s_iob_string, iob_tuple->value->cstring);
        update_iob_display();
    }

    // Pump status
    Tuple *status_tuple = dict_find(iter, KEY_STATUS_STRING);
    if (status_tuple) {
        STRCPY(s_status_string, status_tuple->value->cstring);

        Tuple *status_start_tuple = dict_find(iter, KEY_STATUS_START);
        s_status_start = 0;
        if (status_start_tuple) {
            s_status_start = status_start_tuple->value->uint32;
        }
        Tuple *status_end_tuple = dict_find(iter, KEY_STATUS_END);
        s_status_end = 0;
        if (status_end_tuple) {
            s_status_end = status_end_tuple->value->uint32;
        }

        update_status_display();
    }

    // Pump connection
    Tuple *pump_tuple = dict_find(iter, KEY_PUMP_CONNECTED);
    if (pump_tuple) {
        s_pump_connected = pump_tuple->value->uint8 != 0;
        update_pump_indicator();
    }

    // Trend arrow: the key is only ever present alongside a BG push, and the sender omits it
    // entirely (rather than sending TREND_UNKNOWN) when the current reading has no trend field --
    // so its absence here must clear any previously shown arrow, not leave the old one stale.
    if (bg_tuple) {
        Tuple *trend_tuple = dict_find(iter, KEY_TREND_ARROW);
        s_trend_valid = (trend_tuple != NULL);
        s_trend_arrow = trend_tuple ? trend_tuple->value->uint8 : TREND_UNKNOWN;
        update_trend_indicator();
    }

    // Prediction: like the trend arrow, sent with a BG push, and its absence clears the last one.
    if (bg_tuple) {
        Tuple *pred_tuple = dict_find(iter, KEY_PREDICTED_BG);
        s_pred_valid = (pred_tuple != NULL);
        s_pred_mgdl = pred_tuple ? pred_tuple->value->uint16 : 0;
    }

    // Meal: the sender keeps the latest until a newer one replaces it, so absence means no change.
    Tuple *meal_tuple = dict_find(iter, KEY_MEAL_CARBS);
    Tuple *meal_ts_tuple = dict_find(iter, KEY_MEAL_TIMESTAMP);
    if (meal_tuple && meal_ts_tuple) {
        s_meal_valid = true;
        s_meal_grams = meal_tuple->value->uint16;
        s_meal_timestamp = meal_ts_tuple->value->uint32;
        if (s_graph_layer)
            layer_mark_dirty(s_graph_layer);
    }

    // Graph data
    Tuple *graph_tuple = dict_find(iter, KEY_GRAPH_DATA);
    if (graph_tuple && parse_graph_blob(graph_tuple->value->data, graph_tuple->length)) {
        if (s_graph_layer)
            layer_mark_dirty(s_graph_layer); // new points -> retrace and recompute the trend
    }
    Tuple *high_tuple = dict_find(iter, KEY_GRAPH_HIGH_LINE);
    if (high_tuple)
        s_graph_high_line = high_tuple->value->uint8;
    Tuple *low_tuple = dict_find(iter, KEY_GRAPH_LOW_LINE);
    if (low_tuple)
        s_graph_low_line = low_tuple->value->uint8;
    if ((high_tuple || low_tuple) && s_graph_layer)
        layer_mark_dirty(s_graph_layer);
    // KEY_GRAPH_HOURS from the phone is ignored: the visible window is fixed to GRAPH_HOURS.

    APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s (ts=%lu) IOB: %s graph=%d", s_bg_string, s_bg_timestamp, s_iob_string,
            s_graph_count);
    update_bg_display();
    update_ago_display();
    if (bg_tuple && s_graph_layer)
        layer_mark_dirty(s_graph_layer); // the first reading replaces the "No data" screen
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
}

// Announce which data we want. Also nudges the phone to push the latest reading,
// so a freshly launched watchface fills in without waiting for the next poll.
static void send_capability_announcement(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_begin failed");
        return;
    }
    dict_write_uint8(iter, KEY_PROTOCOL_VERSION, PROTOCOL_VERSION);
    dict_write_uint32(iter, KEY_CAPABILITIES,
                      CAP_BG | CAP_IOB | CAP_STATUS | CAP_PUMP_CONNECTED | CAP_TREND_ARROW | CAP_MEAL |
                          CAP_PREDICTION);
    dict_write_uint8(iter, KEY_GRAPH_HOURS, GRAPH_HOURS);
    if (app_message_outbox_send() != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send failed");
    }
}

static void bluetooth_callback(bool connected) {
    if (connected) {
        send_capability_announcement();
    }
}

static TextLayer *make_text_layer(Layer *root, GRect frame, const char *font_key, GTextAlignment align) {
    TextLayer *layer = text_layer_create(frame);
    text_layer_set_background_color(layer, GColorClear);
    text_layer_set_text_color(layer, COLOR_FG);
    text_layer_set_font(layer, fonts_get_system_font(font_key));
    text_layer_set_text_alignment(layer, align);
    layer_add_child(root, text_layer_get_layer(layer));
    return layer;
}

static Layer *make_layer(Layer *root, GRect frame, LayerUpdateProc update_proc) {
    Layer *layer = layer_create(frame);
    layer_set_update_proc(layer, update_proc);
    layer_add_child(root, layer);
    return layer;
}

// Pixels from layer top to font cap height.
int cap_offset(const char *font_key) {
    static const struct {
        const char *key;
        int offset;
    } table[] = {
        {FONT_KEY_BITHAM_42_BOLD, 13},
        {FONT_KEY_ROBOTO_BOLD_SUBSET_49, 15},
        {FONT_KEY_GOTHIC_28_BOLD, 11},
        {FONT_KEY_GOTHIC_24_BOLD, 10},
        {FONT_KEY_GOTHIC_18_BOLD, 7},
    };

    for (unsigned i = 0; i < ARRAY_LENGTH(table); i++) {
        if (strcmp(font_key, table[i].key) == 0)
            return table[i].offset;
    }

    APP_LOG(APP_LOG_LEVEL_ERROR, "Unknown font key: %s", font_key);
    return 0;
}

static void window_load(Window *window) {
    window_set_background_color(window, COLOR_WINDOW_BG);
    Layer *root = window_get_root_layer(window);

    const int edge_margin = PBL_IF_RECT_ELSE(6, 12);
    const int internal_margin = 3;
    const int top_gap = PBL_IF_RECT_ELSE(LAYOUT_TOP_GAP, 0);
    const int bottom_gap = PBL_IF_RECT_ELSE(LAYOUT_BOTTOM_GAP, 0);

    const int caps_top_y = top_gap + edge_margin; // cap top of the BG value, the topmost text
    s_caps_top_y = caps_top_y; // update_bg_trend_layout needs this after window_load returns
    const int date_y = PBL_DISPLAY_HEIGHT - bottom_gap - edge_margin - 30;
    const int time_y = date_y + cap_offset(FONT_KEY_GOTHIC_24_BOLD) - internal_margin - 42;
    const int time_caps_y = time_y + cap_offset(FONT_TIME);
    // Ago/IOB cap top matches the BG value's own cap top (caps_top_y), not their box origin --
    // using the box origin as their y left their caps visibly lower than the BG digits' caps.
    const int top_row_y = caps_top_y - cap_offset(FONT_SECONDARY);
    const int top_row_h = 28;

    // --- Graph ---------------------------------------------------------------
    // Created first so all text draws over it. The value band runs from under the BG value and its
    // trend row to just above the time, so no reading can be drawn over the text, and it grows with
    // the screen instead of running into the time.
    {
        // Starts under the BG value and its trend row, so no reading can be drawn over the text.
        const int y = caps_top_y + BG_ROW_H + TREND_ROW_GAP + TREND_ROW_H;
        const int h = time_caps_y - internal_margin - y;
        s_graph_band_h = h - GRAPH_PAD_TOP - GRAPH_PAD_BOTTOM;
        s_graph_layer = make_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), graph_layer_update_proc);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, h)); // Debug (entire layer)
        // add_debug_outline(GRect(0, caps_top_y, PBL_DISPLAY_WIDTH, s_graph_band_h)); // Debug (data band only)
    }

    // --- BG value ------------------------------------------------------------
    {
        const int y = caps_top_y - cap_offset(FONT_BG_VALUE);
        s_bg_layer =
            make_text_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, BG_ROW_H), FONT_BG_VALUE, GTextAlignmentCenter);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, BG_ROW_H));
    }

    // --- Time ago ------------------------------------------------------------
    {
        const int w = 52;
        const int x = PBL_IF_RECT_ELSE(edge_margin, PBL_DISPLAY_WIDTH / 10);
        s_ago_layer = make_text_layer(root, GRect(x, top_row_y, w, top_row_h), FONT_SECONDARY, GTextAlignmentLeft);

        // add_debug_outline(GRect(x, top_row_y, w, top_row_h));
    }

    // --- Insulin on board ----------------------------------------------------
    {
        const int w = 52;
        const int x = PBL_DISPLAY_WIDTH - w - PBL_IF_RECT_ELSE(edge_margin, PBL_DISPLAY_WIDTH / 10);
        s_iob_layer = make_text_layer(root, GRect(x, top_row_y, w, top_row_h), FONT_SECONDARY, GTextAlignmentRight);

        // add_debug_outline(GRect(x, top_row_y, w, top_row_h));
    }

    // --- Pump connection indicator --------------------------------------------
    // Below the "ago" label (not beside it, not sharing its origin): the two used to collide
    // whenever both were showing. Below is free real estate regardless of what ago is currently
    // displaying.
    {
        const int w = 20;
        const int h = 18;
        const int x = PBL_IF_RECT_ELSE(edge_margin, PBL_DISPLAY_WIDTH / 10);
        const int y = top_row_y + top_row_h + internal_margin;
        s_pump_layer = make_layer(root, GRect(x, y, w, h), pump_layer_update_proc);

        // add_debug_outline(GRect(x, y, w, h));
    }

    // --- Trend arrow row -------------------------------------------------------
    // Frame is a placeholder; update_bg_trend_layout (called via update_trend_indicator below)
    // places and sizes it for real, and hides it unless the trend is valid and non-flat.
    {
        s_trend_row_layer =
            make_layer(root, GRect(0, 0, PBL_DISPLAY_WIDTH, TREND_ROW_H), trend_row_update_proc);
        layer_set_hidden(s_trend_row_layer, true);
    }

    // --- Status --------------------------------------------------------------
    // Caps end just above the time's, so the strip overlays the bottom of the graph but never the
    // time or the date. (Reverted an attempted rewrite that moved this ~19px further up than
    // intended and drove it into the graph's low line -- time_y is the time box's top, well above
    // its visible cap height, not a usable anchor on its own.) The extra 3px nudges it a little
    // further from the time than the bare formula, on request.
    {
        const int y = time_caps_y - internal_margin - STATUS_CAP_H - cap_offset(STATUS_FONT) - 3;
        const int h = STATUS_H;
        s_status_layer = make_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), status_layer_update_proc);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, h));
    }

    // --- Date ----------------------------------------------------------------
    {
        const int h = 30;  // taller than the font so descenders ("y" in "Saturday") are not clipped
        const int y = date_y;
        s_date_layer =
            make_text_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, h));
    }

    // --- Time ----------------------------------------------------------------
    {
        const int h = 42;
        const int y = time_y;
        s_time_layer =
            make_text_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), FONT_TIME, GTextAlignmentCenter);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, h));
    }

    // Last, so the outlines draw over every other layer.
    s_debug_layer = make_layer(root, layer_get_bounds(root), debug_layer_update_proc);

    update_bg_display();
    update_ago_display();
    update_iob_display();
    update_status_display();
    update_pump_indicator();
    update_trend_indicator();
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
    layer_destroy(s_pump_layer);
    layer_destroy(s_trend_row_layer);
    layer_destroy(s_debug_layer);
}

static void init(void) {
    // Nothing is stored across launches or reboots: the sender re-sends its whole state when the
    // watchface announces itself, and until then the screen says so rather than showing old data.
    // Drop what earlier versions persisted (keys 1-12).
    for (uint32_t key = 1; key <= 12; key++) {
        persist_delete(key);
    }
    app_message_register_inbox_received(handle_dictionary);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_open(2048, 64); // inbox large enough for the graph byte array (up to 24 h of points)

    tick_timer_service_subscribe(MINUTE_UNIT, tick_callback);
    connection_service_subscribe((ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){.load = window_load, .unload = window_unload});
    window_stack_push(s_window, true);

    send_capability_announcement();
}

static void deinit(void) {
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
