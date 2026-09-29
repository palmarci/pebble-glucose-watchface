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
// Hours of graph data. Phone-configurable (Settings page, GraphHours) since MINIMED_GRAPH_MAX_HOURS
// firmware-side is 24 and the wire protocol already carries this both ways; s_graph_hours is the
// live value, GRAPH_HOURS_DEFAULT only the fallback before any config (or persisted value) exists.
#define GRAPH_HOURS_DEFAULT 2
static uint8_t s_graph_hours = GRAPH_HOURS_DEFAULT;
#define GRAPH_TICK_MAX 6 // most hourly ticks to ever draw; see draw_graph_axes
#define STROKE_WIDTH 3 // Graph stroke width in pixels
#define STROKE_OFFSET (STROKE_WIDTH / 2)
#define MAX_GRAPH_POINTS 300 // Enough for 24 h @ 5 min + headroom

// --- Messy stuff, to be cleaned up ---

// Show "---" instead of a stale value once the last reading is this old. CGM cadence is 5 min, so
// keep the last value on screen across a couple of missed readings before giving up on it.
// Defaults to match the bridge's STALE_SECONDS (minimed-pebble-bridge BridgeForegroundService) so
// the watch and the phone status-bar icon go stale together -- phone-configurable (Settings page,
// StaleMinutes) for anyone who'd rather trade that agreement for more tolerance of sensor gaps.
#define STALE_MINUTES_DEFAULT 15
static uint8_t s_stale_minutes = STALE_MINUTES_DEFAULT;

// The sensor's own display ceiling, mg/dL: at or beyond it the pump stops reporting a number and
// says "HI", and the sender graphs the reading at the edge it crossed (SG_CEILING_MGDL in PebbleOS
// minimed_sake_read.c -- a 780G's range is 2.8-22.2 mmol/L). The protocol has no field for the
// sensor range, so the watchface cannot learn it from the sender; a different CGM sets its own with
// a "GRAPH_CEILING_MGDL=<mg/dL>" line in local_defines.txt (see wscript).
#ifndef GRAPH_CEILING_MGDL
#define GRAPH_CEILING_MGDL 400
#endif

// Y-axis in "mg/dL / 2" wire units, symmetric top and bottom: each defaults to its own threshold
// line (10/4 mmol/L unless the phone sets different ones) and only follows the data past that line,
// in 2 mmol/L steps, down to 2.2 mmol/L at the bottom or up to the sensor ceiling at the top (an
// in-range day gets the most pixels per mmol/L; an out-of-range one still fits whole, and the empty
// space below/above the target range that a fixed 2.2-16 mmol/L band would otherwise always show
// goes to the trace instead). Only a reading the sensor itself calls off-scale lands right on an
// edge, so nothing that has a number is ever cut off. See update_axis_max().
#define GRAPH_VALUE_MIN 20
#define GRAPH_VALUE_MAX (GRAPH_CEILING_MGDL / 2) // ~22.2 mmol/L, the highest the axis goes
// The axis defaults to exactly s_graph_high_line (10.0 mmol/L unless the phone overrides it) --
// see update_axis_max(). This constant is only the static initializer's fallback, for the one
// frame before that function has ever run.
#define GRAPH_AXIS_DEFAULT_MAX 90 // 10.0 mmol/L, matching s_graph_high_line's own default
#define GRAPH_AXIS_STEP 18        // ~2 mmol/L
// Don't connect points more than this far apart (a sensor gap draws as a break, not a straight line).
#define GRAPH_GAP_THRESHOLD_MINUTES 15

// The trace covers the left 4/5 of the screen (s_graph_hours of history); the right 1/5 is the same time
// scale continued for the 30-minute forecast, so the whole width is 2 h 30 min.
#define GRAPH_WIDTH_NUM 4 // graph width = screen width * NUM/DEN; the rest is for the forecast
#define GRAPH_WIDTH_DEN 5

// The layer is taller than the value band so a projection leaving a reading near the top or bottom of
// the range has somewhere to go instead of being clipped away (its length is clamped to the layer, so
// it shortens rather than vanishing). Asymmetric: more spare screen below the band than above it.
#define GRAPH_PAD_TOP 6
#define GRAPH_PAD_BOTTOM 8
// A few extra pixels of pure breathing room, on top of the above, between the BG value and the graph
// and between the graph and the time -- eye candy, not needed for anything to render correctly.
#define GRAPH_BREATHING_GAP 3
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
static int s_graph_bottom_y; // fixed once at window_load: just above the time row
static int s_status_top_y;   // fixed once at window_load: top of the status strip's opaque band
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
static char s_iob_total_string[8] = ""; // the same plus the basal insulin still active; empty = unknown
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
// Meals (KEY_MEAL_LIST, falling back to KEY_MEAL_CARBS/KEY_MEAL_TIMESTAMP): a fork mark on the
// graph at the time each was recorded, with the carb amount beside it, for as long as that time is
// inside the graph window. The sender's own forecast of the glucose 30 minutes after the newest
// reading (KEY_PREDICTED_BG). When present it sets the projection's slope; without it the
// projection extrapolates the last two points.
#define PREDICTION_HORIZON_MIN 30
static bool s_pred_valid = false;
static uint16_t s_pred_mgdl = 0;

// The hypo (treat-or-wait) model's score for a falling low (KEY_HYPO_TREAT_PCT/KEY_HYPO_P_LOW_PCT).
// Valid only while the sender is in that regime; absence (s_hypo_valid false) means "not
// applicable", not "zero". treat_pct decides TREAT vs WATCH (it factors in overtreatment risk);
// p_low is what's shown as the confidence number next to that decision, since "how likely do I
// need to treat" is a much more direct question than treat_pct answers on its own.
// sugar_predictor/INTEGRATION.md's Youden's-J threshold; phone-configurable (Settings page,
// HypoTreatThreshold) as a sensitivity knob -- lower catches more real lows at the cost of more
// false alarms. Leave HYPO_TREAT_THRESHOLD_DEFAULT as the model's own validated value.
#define HYPO_TREAT_THRESHOLD_DEFAULT 32
static uint8_t s_hypo_treat_threshold = HYPO_TREAT_THRESHOLD_DEFAULT;
static bool s_hypo_valid = false;
static uint8_t s_hypo_pct = 0;
static uint8_t s_hypo_p_low = 0;
// No vibration here: the sender pops up its own "Low predicted" notification when the score
// reaches TREAT, and that notification's vibration is the one alert. This setting (Settings page,
// HypoAlert) switches that notification on and off, relayed as SETTINGS_ALERT_HYPO_MODEL.
static bool s_hypo_alert = true;

// The sender scores a falling reading from 6.0 mmol/L down, well before most of those turn into a
// real low. Only a score at the treat threshold is worth screen space; below it, stay quiet.
static bool prv_hypo_banner_shown(void) {
    return s_hypo_valid && s_hypo_pct >= s_hypo_treat_threshold;
}

// Alert popup preference: configured on the phone (Settings page, Clay -- see src/pkjs), relayed
// to the firmware's annunciation filter in send_capability_announcement(). Persisted, unlike
// everything else in this app (see init()'s comment on that), because it is user intent rather
// than data the sender re-supplies on every launch -- there is no "sender" to ask; the firmware
// falls back to its own last-known/default value until this watchapp announces again.
#define PERSIST_KEY_ALERTS_LOW 20   // outside the legacy 1-12 range init() wipes on every launch
#define PERSIST_KEY_ALERTS_OTHER 21
static bool s_alerts_low = true;   // matches the firmware's own default (issue #15)
static bool s_alerts_other = false;

// Same phone-configured, persisted pattern as the alert prefs above.
#define PERSIST_KEY_GRAPH_HOURS 22
#define PERSIST_KEY_STALE_MINUTES 23
#define PERSIST_KEY_HYPO_TREAT_THRESHOLD 24
#define PERSIST_KEY_HYPO_ALERT 25
#define PERSIST_KEY_SHOW_MEALS 26
#define PERSIST_KEY_SHOW_HYPO 27
#define PERSIST_KEY_SHOW_PREDICTION 28
#define PERSIST_KEY_SHOW_TREND 29

// Feature toggles: whether to ask the sender for this data at all (send_capability_announcement)
// and show it if it arrives anyway (an in-flight push from before the toggle changed). All default
// on -- these hide existing features, not opt into new ones.
static bool s_show_meals = true;
static bool s_show_hypo = true;
static bool s_show_prediction = true;
static bool s_show_trend = true;

#define MEAL_LIST_MAX 8
static uint8_t s_meal_count = 0;
static uint32_t s_meal_ts[MEAL_LIST_MAX];
static uint16_t s_meal_grams[MEAL_LIST_MAX];

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

// True once the current reading is too old to trust (no fresh push for s_stale_minutes minutes). During a pump
// outage no message arrives to clear the display, so this is re-evaluated from the minute tick.
static bool is_stale(void) { return has_reading() && minutes_ago() >= s_stale_minutes; }

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

// The graph's frame and value-band height depend on whether the trend row and the status strip are
// currently showing:
//
// - No trend to show: the trend row's space (TREND_ROW_GAP + TREND_ROW_H) is given to the graph's
//   top instead of sitting blank, same as before.
// - No status/hypo text to show: the status strip paints an OPAQUE band (status_layer_update_proc)
//   that would otherwise cover real trace data near the bottom whenever it's active. The value
//   band now stops clear of that band's footprint (s_graph_bottom_y - s_status_top_y, both fixed
//   once at window_load) while something is shown there, and falls back to the smaller
//   GRAPH_PAD_BOTTOM (needed only for the forecast line's own headroom) the rest of the time,
//   reclaiming the difference for the trace.
//
// Reads s_trend_valid/s_trend_arrow and s_hypo_valid/s_status_string directly rather than taking
// them as parameters, so every call site (window_load, update_bg_trend_layout, and wherever status
// or hypo state changes) can just call this with no arguments and get a frame consistent with
// whichever of the two changed.
//
// On a short screen (168px, the aplite/basalt/diorite/flint/chalk class -- emery/gabbro get 228),
// the trend row (TREND_ROW_GAP + TREND_ROW_H, 18px) and the hypo band (STATUS_H, 24px) together eat
// a much bigger share of the vertical budget than on a tall one, and the two can be needed at the
// same time (a fast-moving reading with a live trend arrow is exactly when the hypo model is most
// likely to be evaluating). Confirmed on real hardware: with both showing, the graph itself
// shrinks to an illegible sliver. The pump-provided arrow is supplementary -- the on-graph forecast
// is the primary trend indicator -- so it's the one that goes on short screens, freeing that space
// for the graph outright rather than trying to shrink everything a little and fixing nothing.
static bool prv_should_show_trend_row(void) {
    if (PBL_DISPLAY_HEIGHT < 200) {
        return false;
    }
    return s_trend_valid && s_trend_arrow != TREND_FLAT && s_trend_arrow != TREND_UNKNOWN;
}

// The BG value's own box starts at s_caps_top_y - cap_offset(FONT_BG_VALUE), not at s_caps_top_y
// itself (that's the font's CAP line, used for aligning glyphs, not the box's top edge) -- so its
// bottom edge is (s_caps_top_y - cap_offset(FONT_BG_VALUE)) + BG_ROW_H. Anchoring the graph's top
// on s_caps_top_y + BG_ROW_H directly (as an earlier version of this did) silently overshot by
// cap_offset(FONT_BG_VALUE) (13 px) in every state, trend row or not. GRAPH_BREATHING_GAP is added
// at both ends on top of all of the above -- pure eye candy, not needed for anything to render.
static void prv_layout_graph(void) {
    if (!s_graph_layer) {
        return;
    }
    const bool show_trend_row = prv_should_show_trend_row();
    const bool show_status = prv_hypo_banner_shown() || s_status_string[0] != '\0';

    const int bg_bottom = (s_caps_top_y - cap_offset(FONT_BG_VALUE)) + BG_ROW_H;
    const int y = bg_bottom + (show_trend_row ? TREND_ROW_GAP + TREND_ROW_H : 0) + GRAPH_BREATHING_GAP;
    const int h = (s_graph_bottom_y - GRAPH_BREATHING_GAP) - y;
    const int pad_bottom = show_status ? (s_graph_bottom_y - s_status_top_y) : GRAPH_PAD_BOTTOM;
    s_graph_band_h = h - GRAPH_PAD_TOP - pad_bottom;
    layer_set_frame(s_graph_layer, GRect(0, y, PBL_DISPLAY_WIDTH, h));
    layer_mark_dirty(s_graph_layer);
}

// Show/hide the trend row below the BG value, based on whether there's currently a trend worth a
// row for, and give the graph below whichever space that leaves. This never depends on the BG
// string's rendered width -- the row is full width and independently centered, so it cannot
// collide with the ago/IOB corners the way an icon placed beside the digits could.
static void update_bg_trend_layout(void) {
    if (!s_bg_layer) {
        return;
    }
    // BG value's own font/frame never change (see FONT_BG_VALUE's comment) -- only the trend row
    // below it, and the graph's top edge, move.
    const bool show_trend_row = prv_should_show_trend_row();
    const int y = s_caps_top_y - cap_offset(FONT_BG_VALUE);

    if (s_trend_row_layer) {
        layer_set_hidden(s_trend_row_layer, !show_trend_row);
        if (show_trend_row) {
            layer_set_frame(s_trend_row_layer,
                            GRect(0, y + BG_ROW_H + TREND_ROW_GAP, PBL_DISPLAY_WIDTH, TREND_ROW_H));
        }
        layer_mark_dirty(s_trend_row_layer);
    }
    prv_layout_graph();
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
    // The total (with basal) replaces the pump's bolus-only figure when the sender provides it.
    const char *iob = s_iob_total_string[0] != '\0' ? s_iob_total_string : s_iob_string;
    if (iob[0] == '\0' || is_stale()) {
        s_iob_display[0] = '\0';
    } else {
        snprintf(s_iob_display, sizeof(s_iob_display), "%sU", iob);
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
// active; otherwise the graph's own value band grows to reclaim that space -- see prv_layout_graph.
static void update_status_display(void) {
    if (s_status_layer)
        layer_mark_dirty(s_status_layer);
    prv_layout_graph();
}

// Paints only the band + text (when a status is active); everything else stays transparent so the
// graph below shows through. All coords are layer-relative.
static void status_layer_update_proc(Layer *layer, GContext *ctx) {
    const int16_t w = layer_get_bounds(layer).size.w;

    // The hypo (treat-or-wait) banner takes priority over the pump status line: it's the more
    // urgent, time-sensitive thing to show, and pump status resumes on its own once this clears.
    // The decision word plus p_low as its confidence number, rather than one bare percentage:
    // treat_pct alone also folds in overtreatment risk, which reads as ambiguous ("69%... of
    // what?") on its own. p_low directly answers "how likely do I need to treat". Only shown at
    // the treat threshold (prv_hypo_banner_shown); a sub-threshold score shows nothing at all.
    if (prv_hypo_banner_shown()) {
        char hypo_text[16];
        snprintf(hypo_text, sizeof(hypo_text), "TREAT %u%%", (unsigned)s_hypo_p_low);
        // The layer's own bottom edge sits flush against the time's cap-top with no gap at all --
        // fine for the plain pump-status text below (tuned tight on request, see its own comment),
        // but a solid filled block touching the time digits with zero clearance reads as visually
        // overlapping them even though it technically isn't. A few px shaved off just the filled
        // band's own height (not the layer, not the plain-status case) fixes that without giving up
        // any of the graph space the tight layer height otherwise reclaims.
        #define TREAT_BAND_BOTTOM_GAP 3
        const GRect band = GRect(0, 0, w, STATUS_H - TREAT_BAND_BOTTOM_GAP);
        graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(COLOR_BG_LOW, COLOR_FG));
        graphics_fill_rect(ctx, band, 0, GCornerNone);
        graphics_context_set_text_color(ctx, GColorBlack);
        // graphics_draw_text's vertical position within an oversized box isn't centered -- measured
        // 2px low here (7px clear above the glyphs, 3px below, in a 21px band), same top-padding-
        // before-the-glyphs behaviour the peak label already corrects for with its own "-3". Shifts
        // only the text, not the fill, so the visible band's own edges (and the gap fixed above) are
        // unaffected -- just where the glyphs sit inside it.
        const GRect text_box = GRect(band.origin.x, band.origin.y - 2, band.size.w, band.size.h);
        graphics_draw_text(ctx, hypo_text, fonts_get_system_font(STATUS_FONT), text_box,
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
        return;
    }

    if (s_status_string[0] == '\0') {
        return;
    }

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
static int s_axis_min = GRAPH_VALUE_MIN;        // current bottom of the y-axis, wire units

static int graph_y(int bg) {
    if (bg < s_axis_min)
        bg = s_axis_min;
    if (bg > s_axis_max)
        bg = s_axis_max;
    return GRAPH_PAD_TOP + s_graph_band_h - ((bg - s_axis_min) * s_graph_band_h) / (s_axis_max - s_axis_min);
}

// Index range of the points inside the visible window (older ones have scrolled off the left edge).
static int first_visible_point(void) {
    const uint32_t now = time(NULL);
    for (int i = 0; i < s_graph_count; i++) {
        const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[i] * 60;
        if ((int64_t)now - (int64_t)pt_ts <= (int64_t)s_graph_hours * 3600)
            return i;
    }
    return s_graph_count;
}

// Fit both ends of the axis to the visible readings, symmetrically: the top defaults to exactly the
// high line, the bottom to exactly the low line (instead of always leaving a fixed gap of empty
// space down to GRAPH_VALUE_MIN, most of which never has anything plotted in it on an ordinary day),
// so the whole box height is used for anything inside the target range. Each end only moves, in
// GRAPH_AXIS_STEP increments, once a reading (or the forecast) actually exceeds it on that side, so
// the peak/trough (or the forecast) always lands on screen. The last step at either end is short
// when the limit isn't a whole number of steps past the line; that only shows up at an off-scale
// reading anyway. Must run before anything calls graph_y().
static void update_axis_max(void) {
    int peak = 0, trough = GRAPH_VALUE_MAX;
    for (int i = first_visible_point(); i < s_graph_count; i++) {
        if (s_graph_bg_values[i] > peak)
            peak = s_graph_bg_values[i];
        if (s_graph_bg_values[i] < trough)
            trough = s_graph_bg_values[i];
    }
    if (s_pred_valid) {
        if (s_pred_mgdl / 2 > peak)
            peak = s_pred_mgdl / 2; // keep the forecast on the graph
        if (s_pred_mgdl / 2 < trough)
            trough = s_pred_mgdl / 2;
    }
    int top = s_graph_high_line;
    while (top < peak && top < GRAPH_VALUE_MAX)
        top += GRAPH_AXIS_STEP;
    if (top > GRAPH_VALUE_MAX)
        top = GRAPH_VALUE_MAX;
    s_axis_max = top;

    int bottom = s_graph_low_line;
    while (bottom > trough && bottom > GRAPH_VALUE_MIN)
        bottom -= GRAPH_AXIS_STEP;
    if (bottom < GRAPH_VALUE_MIN)
        bottom = GRAPH_VALUE_MIN;
    s_axis_min = bottom;
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

#define PEAK_LABEL_W 26
#define PEAK_LABEL_H 14
#define PEAK_LABEL_GAP (STROKE_WIDTH + 4) // clearance from the peak's own point; see draw_peak_label

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

    // Tick marks, counted back from "now" (the right end of the trace); the tick at "now" also marks
    // where the forecast starts. One per hour up to GRAPH_TICK_MAX; beyond that (a long graph window)
    // one per hour would be an unreadable comb, so the ticks space out to a whole-hour interval
    // instead of staying one-per-hour.
    const int tick_length = 5; // Pixel length
    const int half_tick = tick_length / 2;
    const int trace_w = width * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const int tick_hours = s_graph_hours <= GRAPH_TICK_MAX ? 1 : (s_graph_hours + GRAPH_TICK_MAX - 1) / GRAPH_TICK_MAX;
    for (int n = 0; n * tick_hours <= s_graph_hours; n++) {
        const int x = trace_w - trace_w * (n * tick_hours) / s_graph_hours;
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
    const int graph_minutes = s_graph_hours * 60;

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

// The forecast/trend line's geometry as a straight chord, for glyph collision purposes only -- good
// enough even though draw_forecast_curve renders the sender's forecast as a gentle curve, since the
// curve never strays far from this chord. The no-forecast trend projection's length here is its
// unclamped TREND_PROJ_GAP + TREND_PROJ_LEN (trend_draw_projection may clamp it shorter to fit the
// layer), which only makes this a more cautious estimate, never a less cautious one. False when
// there is currently no line to collide with (stale data, or not enough history yet).
static bool prv_projection_line(GRect bounds, GPoint *from, GPoint *to) {
    if (s_graph_count < 1) {
        return false;
    }
    float slope;
    if (s_pred_valid) {
        const float newest_wire = (float)s_graph_bg_values[s_graph_count - 1];
        slope = ((float)s_pred_mgdl / 2.0f - newest_wire) / (float)PREDICTION_HORIZON_MIN;
    } else if (!trend_slope(&slope)) {
        return false;
    }
    const uint32_t now = time(NULL);
    const uint32_t newest_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[s_graph_count - 1] * 60;
    const int age_min = (int)(((int64_t)now - (int64_t)newest_ts) / 60);
    if (age_min >= s_stale_minutes) {
        return false;
    }

    const int graph_w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const int graph_minutes = s_graph_hours * 60;
    const int newest_x = graph_w - (age_min * graph_w) / graph_minutes;
    const float px_per_min = (float)graph_w / graph_minutes;
    const float px_per_wire = (float)s_graph_band_h / (s_axis_max - s_axis_min);
    *from = GPoint(newest_x, graph_y(s_graph_bg_values[s_graph_count - 1]));
    if (s_pred_valid) {
        *to = GPoint(newest_x + (int)(PREDICTION_HORIZON_MIN * px_per_min + 0.5f), graph_y(s_pred_mgdl / 2));
        return true;
    }
    const float vx = px_per_min, vy = -slope * px_per_wire;
    const float mag = sqrtf_local(vx * vx + vy * vy);
    if (mag < 1e-6f) {
        *to = *from;
        return true;
    }
    const float len = TREND_PROJ_GAP + TREND_PROJ_LEN;
    *to = GPoint(from->x + (int)(vx / mag * len), from->y + (int)(vy / mag * len));
    return true;
}

// -- Generic glyph placement ----------------------------------------------------------------------
//
// Every marker drawn on the graph (the meal label, the peak label, and any future symbol) is a small
// rectangle that must not have the BG trace, the high/low lines, or the forecast/trend line passing
// through it, and must not run off its allotted area. Callers describe a short list of candidate
// positions, most-preferred first (e.g. "to the right", "to the left"), and prv_place_glyph returns
// the first one that collides with nothing, clamped inside `clamp_area`. If every candidate
// collides, the first (still clamped) one is used anyway -- something has to be drawn, and the
// candidate list should already be ordered by preference, so this is the least-bad fallback, not an
// arbitrary one.
#define GLYPH_SAMPLE_STEP 3 // px; granularity of the trace/line collision scan

// The trace's y at a given x, by linear interpolation between the two plotted points either side of
// it -- the same time-axis mapping draw_bg_graph plots with. False where x falls in a gap wider than
// GRAPH_GAP_THRESHOLD_MINUTES or outside the plotted range: nothing is actually drawn there to
// collide with (draw_bg_graph itself only connects points within that gap threshold).
static bool prv_trace_y_at_x(GRect bounds, int x, int *y_out) {
    if (s_graph_count == 0) {
        return false;
    }
    const int w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const uint32_t now = time(NULL);
    const int graph_minutes = s_graph_hours * 60;
    bool have_prev = false;
    int prev_x = 0, prev_y = 0, prev_off = 0;
    for (int i = 0; i < s_graph_count; i++) {
        const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[i] * 60;
        const int mins_ago = (int)(((int64_t)now - (int64_t)pt_ts) / 60);
        const int px = w - (mins_ago * w) / graph_minutes;
        const int py = graph_y(s_graph_bg_values[i]);
        const bool join_prev = have_prev && (int)s_graph_offsets[i] - prev_off <= GRAPH_GAP_THRESHOLD_MINUTES;
        if (join_prev) {
            const int lo = prev_x < px ? prev_x : px, hi = prev_x < px ? px : prev_x;
            if (x >= lo && x <= hi) {
                *y_out = (px == prev_x) ? py : prev_y + (int)((int64_t)(py - prev_y) * (x - prev_x) / (px - prev_x));
                return true;
            }
        }
        have_prev = true;
        prev_x = px;
        prev_y = py;
        prev_off = (int)s_graph_offsets[i];
    }
    return false;
}

static bool prv_box_crosses_hline(GRect box, int line_y) {
    return line_y >= box.origin.y && line_y <= box.origin.y + box.size.h;
}

// The trace itself is STROKE_WIDTH px wide, not the single mathematical pixel prv_trace_y_at_x
// returns, so a candidate whose nearest sampled y is a pixel or two outside the box can still have
// the stroke's ink land inside it (seen on a narrow screen with a sharp peak, where the "below"
// candidate's centerline missed by 1px but the 3px-wide stroke still touched the label). Padding the
// comparison by half the stroke width catches that.
#define TRACE_STROKE_MARGIN ((STROKE_WIDTH + 1) / 2)

static bool prv_box_crosses_trace(GRect bounds, GRect box) {
    for (int x = box.origin.x; x <= box.origin.x + box.size.w; x += GLYPH_SAMPLE_STEP) {
        int y;
        if (prv_trace_y_at_x(bounds, x, &y) && y + TRACE_STROKE_MARGIN >= box.origin.y &&
            y - TRACE_STROKE_MARGIN <= box.origin.y + box.size.h) {
            return true;
        }
    }
    return false;
}

static bool prv_box_crosses_segment(GRect box, GPoint a, GPoint b) {
    const int steps = 12;
    for (int i = 0; i <= steps; i++) {
        const int x = a.x + (b.x - a.x) * i / steps;
        const int y = a.y + (b.y - a.y) * i / steps;
        if (x >= box.origin.x && x <= box.origin.x + box.size.w && y >= box.origin.y &&
            y <= box.origin.y + box.size.h) {
            return true;
        }
    }
    return false;
}

// Every glyph box already placed on the graph THIS FRAME, so a later one can avoid overlapping an
// earlier one, not just a line -- e.g. the meal fork/label and the peak label can otherwise land on
// top of each other, since neither is a "line" the trace/hline/projection checks above catch.
// Reset once per redraw (prv_reset_glyph_registry, called from graph_layer_update_proc) and
// appended to by prv_register_glyph after each marker's final position is chosen.
#define GLYPH_REGISTRY_MAX 20 // MEAL_LIST_MAX (8) meals x 2 boxes (fork + label) + the peak label, with room
static GRect s_glyph_registry[GLYPH_REGISTRY_MAX];
static int s_glyph_registry_count;

static void prv_reset_glyph_registry(void) {
    s_glyph_registry_count = 0;
}

// Register a box that has already been placed (or is fixed, like the meal fork icon) so later
// glyphs this frame avoid it too. Silently drops the box past GLYPH_REGISTRY_MAX rather than
// faulting -- a frame that busy already has bigger legibility problems than one missed check.
static void prv_register_glyph(GRect box) {
    if (s_glyph_registry_count < GLYPH_REGISTRY_MAX) {
        s_glyph_registry[s_glyph_registry_count++] = box;
    }
}

static bool prv_boxes_overlap(GRect a, GRect b) {
    return a.origin.x < b.origin.x + b.size.w && a.origin.x + a.size.w > b.origin.x &&
           a.origin.y < b.origin.y + b.size.h && a.origin.y + a.size.h > b.origin.y;
}

static bool prv_box_crosses_registry(GRect box) {
    for (int i = 0; i < s_glyph_registry_count; i++) {
        if (prv_boxes_overlap(box, s_glyph_registry[i])) {
            return true;
        }
    }
    return false;
}

static bool prv_box_crosses_hlines(GRect box) {
    return prv_box_crosses_hline(box, graph_y(s_graph_high_line)) ||
           prv_box_crosses_hline(box, graph_y(s_graph_low_line));
}

static bool prv_box_crosses_projection(GRect bounds, GRect box) {
    GPoint from, to;
    return prv_projection_line(bounds, &from, &to) && prv_box_crosses_segment(box, from, to);
}

static bool prv_glyph_collides(GRect bounds, GRect box) {
    return prv_box_crosses_hlines(box) || prv_box_crosses_trace(bounds, box) ||
           prv_box_crosses_registry(box) || prv_box_crosses_projection(bounds, box);
}

static GRect prv_clamp_to_area(GRect area, GRect box) {
    if (box.origin.x < area.origin.x) {
        box.origin.x = area.origin.x;
    }
    if (box.origin.x + box.size.w > area.origin.x + area.size.w) {
        box.origin.x = area.origin.x + area.size.w - box.size.w;
    }
    if (box.origin.y < area.origin.y) {
        box.origin.y = area.origin.y;
    }
    if (box.origin.y + box.size.h > area.origin.y + area.size.h) {
        box.origin.y = area.origin.y + area.size.h - box.size.h;
    }
    return box;
}

static bool prv_box_within(GRect area, GRect box) {
    return box.origin.x >= area.origin.x && box.origin.x + box.size.w <= area.origin.x + area.size.w &&
           box.origin.y >= area.origin.y && box.origin.y + box.size.h <= area.origin.y + area.size.h;
}

// `chosen_out` (may be NULL) receives the index of the candidate actually used, since edge-clamping
// can move a box far enough that its final position no longer tells the caller which candidate it
// started as -- e.g. a "to the right" candidate clamped left on a narrow screen can end up left of
// its anchor, so comparing the returned box's position back against the anchor is not reliable.
//
// Three tiers, not one: a plain "first collision-free one, else give up" version falls straight
// through to the least-preferred candidate regardless of what it collides with whenever every
// candidate collides with *something* -- which happens more than it sounds, e.g. a peak sitting
// exactly at the high line (axis_max == high_line, so the peak's own y has nowhere to go but onto
// the line) or a steep rise right next to a meal. Ranked by how bad the collision actually reads:
//   1. nothing at all
//   2. no other glyph AND no threshold line -- may still cross the wandering trace/projection
//      (a number touching a moving line is easy to misread as "attached to the wrong point"; a
//      static reference line a user reads directly is worse to sit on than that)
//   3. no other glyph -- the least readable failure (two overlapping numbers) is still avoided
//   4. give up, use the first candidate
// An empty registry (no meals on screen) makes tier 3 trivially satisfied by candidate 0, which is
// exactly the peak-at-the-high-line case above -- tier 2 exists so that case still finds "below".
static GRect prv_place_glyph(GRect bounds, GRect clamp_area, const GRect *candidates, int count, int *chosen_out) {
    const GRect fallback = prv_clamp_to_area(clamp_area, candidates[0]);
    for (int i = 0; i < count; i++) {
        const GRect c = prv_clamp_to_area(clamp_area, candidates[i]);
        if (!prv_glyph_collides(bounds, c)) {
            if (chosen_out) {
                *chosen_out = i;
            }
            return c;
        }
    }
    for (int i = 0; i < count; i++) {
        const GRect c = prv_clamp_to_area(clamp_area, candidates[i]);
        if (!prv_box_crosses_registry(c) && !prv_box_crosses_hlines(c)) {
            if (chosen_out) {
                *chosen_out = i;
            }
            return c;
        }
    }
    for (int i = 0; i < count; i++) {
        const GRect c = prv_clamp_to_area(clamp_area, candidates[i]);
        if (!prv_box_crosses_registry(c)) {
            if (chosen_out) {
                *chosen_out = i;
            }
            return c;
        }
    }
    if (chosen_out) {
        *chosen_out = 0;
    }
    return fallback;
}

// The trace's vertical extent per pixel column, ink included, for the peak label's search below: a
// lookup per column instead of a walk over every graph point for each of the thousands of boxes it
// tries. Each column also spans its neighbours' y, so a steep segment counts as the solid stroke it
// is on screen rather than as one sample every GLYPH_SAMPLE_STEP px.
static int16_t s_trace_col_min[PBL_DISPLAY_WIDTH + 1];
static int16_t s_trace_col_max[PBL_DISPLAY_WIDTH + 1];

static void prv_build_trace_columns(GRect bounds, int w) {
    // Static, not on the stack: ~1 KB is a real share of an app's stack on the older platforms.
    static int16_t ys[PBL_DISPLAY_WIDTH + 1];
    static bool has[PBL_DISPLAY_WIDTH + 1];
    for (int x = 0; x <= w; x++) {
        int y = 0;
        has[x] = prv_trace_y_at_x(bounds, x, &y);
        ys[x] = (int16_t)y;
    }
    for (int x = 0; x <= w; x++) {
        s_trace_col_min[x] = INT16_MAX;
        s_trace_col_max[x] = INT16_MIN;
        for (int n = x - 1; n <= x + 1; n++) {
            if (n >= 0 && n <= w && has[n]) {
                if (ys[n] < s_trace_col_min[x]) s_trace_col_min[x] = ys[n];
                if (ys[n] > s_trace_col_max[x]) s_trace_col_max[x] = ys[n];
            }
        }
    }
}

// TRACE_STROKE_MARGIN covers the stroke's ink; the extra px keep the digits from sitting flush on it.
#define PEAK_TRACE_CLEARANCE (TRACE_STROKE_MARGIN + 2)

static bool prv_box_crosses_trace_columns(GRect box, int w) {
    const int x0 = box.origin.x < 0 ? 0 : box.origin.x;
    const int x1 = box.origin.x + box.size.w > w ? w : box.origin.x + box.size.w;
    for (int x = x0; x <= x1; x++) {
        if (s_trace_col_min[x] <= s_trace_col_max[x] &&
            s_trace_col_max[x] + PEAK_TRACE_CLEARANCE >= box.origin.y &&
            s_trace_col_min[x] - PEAK_TRACE_CLEARANCE <= box.origin.y + box.size.h) {
            return true;
        }
    }
    return false;
}

// The collision-free box nearest to (px, py) inside `area`, or false if every position collides.
// Scans the whole area on a 2px grid rather than trying a few fixed spots around the peak: on a
// steep trace (a peak at the graph's left edge with the BG falling away from it, the common case
// hours after a meal) every fixed spot sat on the line, and the tiered fallback then drew the label
// straight across it. Ties go to "above", then to horizontally centered over the peak.
#define PEAK_SEARCH_STEP 2
static bool prv_find_free_box_near(GRect bounds, GRect area, int w, int px, int py, int bw, int bh,
                                   GRect *out) {
    GPoint pfrom, pto;
    const bool have_proj = prv_projection_line(bounds, &pfrom, &pto);
    const int hi_y = graph_y(s_graph_high_line), lo_y = graph_y(s_graph_low_line);
    int32_t best = INT32_MAX;
    for (int by = area.origin.y; by + bh <= area.origin.y + area.size.h; by += PEAK_SEARCH_STEP) {
        const int dy = by > py ? by - py : (py > by + bh ? py - (by + bh) : 0);
        const bool below = by + bh / 2 > py;
        for (int bx = area.origin.x; bx + bw <= area.origin.x + area.size.w; bx += PEAK_SEARCH_STEP) {
            const int dx = bx > px ? bx - px : (px > bx + bw ? px - (bx + bw) : 0);
            const int off_center = bx + bw / 2 - px;
            const int32_t cost = (int32_t)(dx * dx + dy * dy) * 16 + (below ? 8 : 0) +
                                 (off_center < 0 ? -off_center : off_center);
            if (cost >= best) {
                continue;
            }
            const GRect c = GRect(bx, by, bw, bh);
            if (prv_box_crosses_hline(c, hi_y) || prv_box_crosses_hline(c, lo_y) ||
                prv_box_crosses_trace_columns(c, w) || prv_box_crosses_registry(c) ||
                (have_proj && prv_box_crosses_segment(c, pfrom, pto))) {
                continue;
            }
            best = cost;
            *out = c;
        }
    }
    return best != INT32_MAX;
}

// The window's highest reading as a tiny number, no plate so it never hides the trace or the meal
// marker. The only text on the graph. Ties (by displayed value) go to the newest point. Placed at
// the nearest spot nothing crosses (prv_find_free_box_near), kept to the trace's own width -- the
// label never belongs in the forecast column, unlike the meal label, which is allowed to reach
// into it.
static void draw_peak_label(GContext *ctx, GRect bounds) {
    const int first = first_visible_point();
    if (first >= s_graph_count) {
        return;
    }
    int hi = first;
    for (int i = first; i < s_graph_count; i++) {
        if (wire_to_tenths(s_graph_bg_values[i]) >= wire_to_tenths(s_graph_bg_values[hi]))
            hi = i;
    }

    const int w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const uint32_t now = time(NULL);
    const uint32_t pt_ts = s_graph_ref_timestamp + (uint32_t)s_graph_offsets[hi] * 60;
    const int mins_ago = (int)(((int64_t)now - (int64_t)pt_ts) / 60);
    const int x = w - (mins_ago * w) / (s_graph_hours * 60);
    const int y = graph_y(s_graph_bg_values[hi]);

    // graphics_draw_text needs 3px more headroom than the label's nominal height to center the
    // glyphs without clipping, drawn above the box's own top edge -- so every box below already
    // includes that 3px on top. Otherwise the collision check clears a box the real draw then
    // overshoots, and the digits end up sitting on whatever was just above it.
    const int render_h = PEAK_LABEL_H + 3;
    const GRect clamp_area = GRect(0, 0, w, bounds.size.h);
    prv_build_trace_columns(bounds, w);
    GRect box;
    if (!prv_find_free_box_near(bounds, clamp_area, w, x, y, PEAK_LABEL_W, render_h, &box)) {
        // Nowhere is clear (a very busy frame): the old fixed above/below spots, via the tiered
        // fallback that at least keeps it off the other glyphs.
        const int top = y - PEAK_LABEL_GAP - render_h, bottom = y + PEAK_LABEL_GAP - 3;
        const GRect candidates[] = {
            GRect(x - PEAK_LABEL_W / 2, top, PEAK_LABEL_W, render_h),
            GRect(x - PEAK_LABEL_W / 2, bottom, PEAK_LABEL_W, render_h),
        };
        box = prv_place_glyph(bounds, clamp_area, candidates, 2, NULL);
        APP_LOG(APP_LOG_LEVEL_INFO, "peak label: no free spot, peak=(%d,%d) box=(%d,%d)", x, y,
                box.origin.x, box.origin.y);
    }
    prv_register_glyph(box); // nothing draws after this yet, but keep the registry complete

    char text[8];
    format_mmol(text, sizeof(text), s_graph_bg_values[hi]);
    graphics_context_set_text_color(ctx, COLOR_FG);
    graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), box,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
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

// The forecast: dots along a smooth curve from the newest reading to the predicted value, ending in
// a larger dot. Same dot size as the trace so it reads as its continuation. A quadratic Bezier
// through a control point on the trace's own recent tangent (evaluated at the curve's midpoint x),
// so the curve leaves the trace smoothly instead of kinking into a straight chord to the forecast.
#define FORECAST_DOT_SPACING 6
#define FORECAST_STEPS 16
// How much the control point is allowed to pull the curve away from the straight from-to chord:
// 1.0 is the raw tangent projection (visibly bowed on a steep recent slope), 0.0 is a dead-straight
// line. 0.4 keeps just enough bend to leave the trace smoothly without reading as an exaggerated S.
#define FORECAST_CURVE_STRENGTH 0.4f
static void draw_forecast_curve(GContext *ctx, GPoint from, GPoint to, float tangent_dydx) {
    const float cx = (from.x + to.x) / 2.0f;
    const float straight_mid_y = (from.y + to.y) / 2.0f;
    const float tangent_cy = from.y + tangent_dydx * (cx - from.x);
    float cy = straight_mid_y + FORECAST_CURVE_STRENGTH * (tangent_cy - straight_mid_y);
    // Clamp the control point: a noisy recent slope must bend the curve, not fling it far outside
    // the from/to span. (No fabsf: see sqrtf_local's comment on why libm calls are avoided here.)
    const float span = (float)(to.y > from.y ? to.y - from.y : from.y - to.y) + 20.0f;
    const float lo = (float)(from.y < to.y ? from.y : to.y) - span;
    const float hi = (float)(from.y < to.y ? to.y : from.y) + span;
    if (cy < lo) cy = lo;
    if (cy > hi) cy = hi;

    graphics_context_set_fill_color(ctx, COLOR_FG);
    GPoint prev = from;
    float dist_since_dot = 0.0f;
    for (int i = 1; i <= FORECAST_STEPS; i++) {
        const float t = (float)i / FORECAST_STEPS;
        const float mt = 1.0f - t;
        const float x = mt * mt * from.x + 2.0f * mt * t * cx + t * t * to.x;
        const float y = mt * mt * from.y + 2.0f * mt * t * cy + t * t * to.y;
        const GPoint pt = GPoint((int16_t)(x + 0.5f), (int16_t)(y + 0.5f));
        const int dx = pt.x - prev.x, dy = pt.y - prev.y;
        dist_since_dot += sqrtf_local((float)(dx * dx + dy * dy));
        prev = pt;
        if (i == FORECAST_STEPS)
            break;  // the end gets its own bigger dot below
        if (dist_since_dot >= FORECAST_DOT_SPACING) {
            dist_since_dot = 0.0f;
            graphics_fill_rect(ctx, GRect(pt.x - STROKE_OFFSET, pt.y - STROKE_OFFSET, STROKE_WIDTH, STROKE_WIDTH), 0,
                               GCornerNone);
        }
    }
    graphics_fill_circle(ctx, to, STROKE_WIDTH);
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
    if (age_min >= s_stale_minutes)
        return;

    // Anchor on the newest reading's ACTUAL position on the trace, so the projection is a true continuation
    // (collinear with the last segment) rather than a parallel-shifted copy — the point drifts left as it
    // ages, and the projection follows. newest_x and graph_y() are exactly what draw_bg_graph uses to plot
    // that point, so the pivot lands on it by construction. The graph's own px/min and px/value set the
    // angle; the time axis is the layer's left NUM/DEN and the projection runs into the rest.
    const int graph_w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;
    const int graph_minutes = s_graph_hours * 60;
    const int newest_x = graph_w - (age_min * graph_w) / graph_minutes;
    const float px_per_min = (float)graph_w / graph_minutes;
    const float px_per_wire = (float)s_graph_band_h / (s_axis_max - s_axis_min);
    const GPoint pivot = GPoint(newest_x, graph_y(s_graph_bg_values[s_graph_count - 1]));
    if (s_pred_valid) {
        // The forecast is a value at a time: PREDICTION_HORIZON_MIN after the reading, on the graph's own
        // scale, so it ends exactly at the right edge for a fresh reading.
        const GPoint end = GPoint(newest_x + (int)(PREDICTION_HORIZON_MIN * px_per_min + 0.5f),
                                  graph_y(s_pred_mgdl / 2));
        // The curve's starting tangent: the same recent last-two-point slope the no-forecast branch
        // extrapolates from, so the curve leaves the trace smoothly. Falls back to the straight chord
        // to `end` (a flat curve, i.e. today's straight line) when there aren't two clean recent points.
        float recent_slope;
        float tangent_dydx;
        if (trend_slope(&recent_slope)) {
            tangent_dydx = -recent_slope * px_per_wire / px_per_min;
        } else {
            const int dx = end.x - pivot.x;
            tangent_dydx = dx != 0 ? (float)(end.y - pivot.y) / (float)dx : 0.0f;
        }
        draw_forecast_curve(ctx, pivot, end, tangent_dydx);
        return;
    }
    trend_draw_projection(ctx, bounds, pivot, slope, px_per_min, px_per_wire);
}

// A fork mark at a meal's time along the top of the value band (above almost every reading), with
// the carbs in grams beside it. Placed on the same time axis as the trace. The fork icon itself is
// deliberately NOT placed generically -- it is the "this happened at this time" anchor, and moving
// it to dodge a line would make it lie about when the meal was logged. Its label only picks a side.
#define MEAL_ICON_H 11
#define MEAL_TEXT_W 34

static void draw_meal_fork(GContext *ctx, int x) {
    const int y = GRAPH_PAD_TOP + 1; // top of the band: the bottom is under the status strip
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(COLOR_MEAL, COLOR_FG));
    graphics_fill_rect(ctx, GRect(x - 4, y, 2, 5), 0, GCornerNone);     // tines
    graphics_fill_rect(ctx, GRect(x - 1, y, 2, 5), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(x + 2, y, 2, 5), 0, GCornerNone);
    graphics_fill_rect(ctx, GRect(x - 4, y + 5, 8, 2), 0, GCornerNone); // base
    graphics_fill_rect(ctx, GRect(x - 1, y + 7, 2, 4), 0, GCornerNone); // handle
    // The icon itself is fixed, but it still needs to be in the registry so a later glyph -- the
    // peak label, most often -- knows to steer around it.
    prv_register_glyph(GRect(x - 4, y, 8, MEAL_ICON_H));
}

// The grams label beside the fork at `x`, on the preferred side unless that side does not fit on
// screen or (with `may_swap`) something crosses it and the other side is clear.
static void draw_meal_label(GContext *ctx, GRect bounds, int x, const char *label, bool prefer_left,
                            bool may_swap) {
    const int label_y = GRAPH_PAD_TOP + 1 - 3;
    const GRect right_box = GRect(x + 6, label_y, MEAL_TEXT_W, 16);
    const GRect left_box = GRect(x - 6 - MEAL_TEXT_W, label_y, MEAL_TEXT_W, 16);
    const GRect candidates[] = {prefer_left ? left_box : right_box, prefer_left ? right_box : left_box};
    const GRect clamp_area = GRect(0, 0, bounds.size.w, bounds.size.h);
    // The carb number must stay visually attached to its own fork, full stop -- prv_place_glyph's
    // usual clamp-then-pick can slide an edge-of-screen candidate away from its anchor and still
    // call it "clean". So: prefer whichever side needs no clamping at all, even if it collides with
    // something else; only fall back to the generic (possibly detached) placement when neither raw
    // side fits on screen, which real screen widths make unreachable.
    int chosen = -1;
    GRect box = candidates[0];
    for (int i = 0; i < 2 && chosen < 0 && may_swap; i++) {
        if (prv_box_within(clamp_area, candidates[i]) && !prv_glyph_collides(bounds, candidates[i])) {
            box = candidates[i];
            chosen = i;
        }
    }
    for (int i = 0; i < 2 && chosen < 0; i++) {
        if (prv_box_within(clamp_area, candidates[i])) {
            box = candidates[i];
            chosen = i;
        }
    }
    if (chosen < 0) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "meal label: fell back to clamped placement, x=%d", x);
        box = prv_place_glyph(bounds, clamp_area, candidates, 2, &chosen);
    }
    const bool left = (chosen == 0) == prefer_left;
    prv_register_glyph(box);
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(COLOR_MEAL, COLOR_FG));
    graphics_draw_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), box,
                       GTextOverflowModeTrailingEllipsis, left ? GTextAlignmentRight : GTextAlignmentLeft,
                       NULL);
}

// Every meal still inside the graph window, oldest first (s_meal_ts is kept sorted by the parser).
// Meals whose forks land closer than one label's width form a cluster, so no label ever sits
// between two forks where it reads as belonging to either: a pair gets its labels on the outside
// ("60 fork fork 24"), and three or more (or a pair too close to an edge for that) share one label
// with their total beside the last fork.
static void draw_meal(GContext *ctx, GRect bounds) {
    const uint32_t now = time(NULL);
    const int graph_minutes = s_graph_hours * 60;
    const int graph_w = bounds.size.w * GRAPH_WIDTH_NUM / GRAPH_WIDTH_DEN;

    int xs[MEAL_LIST_MAX];
    uint16_t grams[MEAL_LIST_MAX];
    int n = 0;
    for (uint8_t i = 0; i < s_meal_count && n < MEAL_LIST_MAX; i++) {
        const int age_min = now > s_meal_ts[i] ? (int)((now - s_meal_ts[i]) / 60) : 0;
        if (age_min > graph_minutes) {
            continue;
        }
        xs[n] = graph_w - (age_min * graph_w) / graph_minutes;
        grams[n] = s_meal_grams[i];
        n++;
    }

    char label[8];
    for (int first = 0; first < n;) {
        int last = first;
        while (last + 1 < n && xs[last + 1] - xs[last] < MEAL_TEXT_W) {
            last++;
        }
        for (int i = first; i <= last; i++) {
            draw_meal_fork(ctx, xs[i]);
        }
        // A pair keeps its labels on the outside even when a line crosses one: a label between the
        // two forks is worse. Too close to an edge for that, it gets the total instead.
        const bool pair_fits = xs[first] - 6 - MEAL_TEXT_W >= 0 &&
                               xs[last] + 6 + MEAL_TEXT_W <= bounds.size.w;
        if (first == last) {
            snprintf(label, sizeof(label), "%u", (unsigned)grams[first]);
            draw_meal_label(ctx, bounds, xs[first], label, false, true);
        } else if (last == first + 1 && pair_fits) {
            snprintf(label, sizeof(label), "%u", (unsigned)grams[first]);
            draw_meal_label(ctx, bounds, xs[first], label, true, false);
            snprintf(label, sizeof(label), "%u", (unsigned)grams[last]);
            draw_meal_label(ctx, bounds, xs[last], label, false, false);
        } else {
            unsigned total = 0;
            for (int i = first; i <= last; i++) {
                total += grams[i];
            }
            snprintf(label, sizeof(label), "%u", total);
            draw_meal_label(ctx, bounds, xs[last], label, false, false);
        }
        first = last + 1;
    }
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
    prv_reset_glyph_registry(); // a fresh frame: forget last redraw's glyph boxes
    if (!s_pump_connected && s_pump_layer) {
        // The pump-offline cross lives in its own layer, outside the graph, but still shares the
        // same screen region near the graph's top-left corner and can visually collide with a
        // glyph placed there (confirmed: the peak label, when the peak is near the left edge of the
        // window, on a narrow display). Register its footprint, translated from root-relative
        // (layer_get_frame) into this layer's own local coordinates, same as everything else here.
        const GRect graph_frame = layer_get_frame(layer);
        const GRect pump_frame = layer_get_frame(s_pump_layer);
        prv_register_glyph(GRect(pump_frame.origin.x - graph_frame.origin.x,
                                 pump_frame.origin.y - graph_frame.origin.y, pump_frame.size.w,
                                 pump_frame.size.h));
    }
    draw_graph_axes(ctx, bounds);
    if (!has_reading()) {
        draw_no_data(ctx, bounds);
        return;
    }
    draw_bg_graph(ctx, bounds);
    if (s_show_meals)
        draw_meal(ctx, bounds); // placed before the peak label so it registers its boxes first
    // draw_projection falls back to extrapolating the trace's own recent trend when there's no
    // KEY_PREDICTED_BG -- gate the call itself, not just s_pred_valid, so the toggle hides both.
    if (s_show_prediction)
        draw_projection(ctx, bounds);
    draw_peak_label(ctx, bounds);
}

static void send_capability_announcement(void);

static void tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_time_and_date();
    update_ago_display(); // advances the staleness hint each minute
    // Re-run on the tick, not just on receipt: during an outage no message arrives, so this is what
    // blanks the BG/IOB once they cross s_stale_minutes.
    update_bg_display();
    update_iob_display();

    // Self-heal a missed push: past the "fresh" window, re-announce every couple of minutes. The
    // sender answers an announcement with the latest reading, the same nudge that leaving and
    // re-entering the watchface gives. Deliberately has no upper bound on mins -- stopping once
    // the display actually goes stale (mins >= s_stale_minutes) is exactly backwards: that is the
    // moment recovery matters most, and until this had no cap a single missed push left the watch
    // frozen until manually relaunched, even though the pump/phone link came back on its own.
    //
    // Before the first reading ever arrives (has_reading() false, e.g. the pump connects while
    // this watchface is already foreground and its own connect-event push races the firmware's
    // "who is foreground" check and gets dropped), retry every minute instead of every two -- there
    // is no display to disturb yet, so there's no reason to wait, and this is what used to leave
    // the pump-connected icon showing X and the graph empty even after the pump had come online
    // and the phone could see IOB values (a push the firmware DID send, just not this one).
    const int mins = minutes_ago();
    const bool never_had_reading = !has_reading();
    if ((never_had_reading || mins >= 6) &&
        (never_had_reading || (tick_time->tm_min % 2) == 0))
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

// Meal list wire format: [count u8][(timestamp u32 LE)(grams u16 LE) x count]. Same
// validate-before-write discipline as parse_graph_blob. The sender keeps this sorted oldest-first;
// nothing here depends on that, but draw_meal's stagger logic reads better if it stays that way.
static bool parse_meal_list_blob(const uint8_t *d, uint16_t len) {
    if (len < 1) {
        return false;
    }
    uint8_t count = d[0];
    if (count > MEAL_LIST_MAX)
        count = MEAL_LIST_MAX;
    if (len < (uint16_t)(1 + count * 6)) {
        return false;
    }
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = d + 1 + i * 6;
        s_meal_ts[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        s_meal_grams[i] = (uint16_t)(p[4] | (p[5] << 8));
    }
    s_meal_count = count;
    return true;
}

// Clay's toggle wire type isn't pinned down by messageKeys (unlike the shared protocol.h keys,
// which spell out an exact width) -- read whatever width/signedness it used rather than assuming.
static bool prv_tuple_bool(const Tuple *t, bool fallback) {
    if (!t) {
        return fallback;
    }
    switch (t->type) {
        case TUPLE_UINT:
            return t->length == 1 ? t->value->uint8 != 0 : t->length == 2 ? t->value->uint16 != 0 : t->value->uint32 != 0;
        case TUPLE_INT:
            return t->length == 1 ? t->value->int8 != 0 : t->length == 2 ? t->value->int16 != 0 : t->value->int32 != 0;
        default:
            return fallback;
    }
}

// Same idea as prv_tuple_bool, for a Clay slider's numeric value.
static uint32_t prv_tuple_uint32(const Tuple *t, uint32_t fallback) {
    if (!t) {
        return fallback;
    }
    switch (t->type) {
        case TUPLE_UINT:
            return t->length == 1 ? t->value->uint8 : t->length == 2 ? t->value->uint16 : t->value->uint32;
        case TUPLE_INT:
            return t->length == 1 ? (uint32_t)t->value->int8 : t->length == 2 ? (uint32_t)t->value->int16 : (uint32_t)t->value->int32;
        default:
            return fallback;
    }
}

// Handle a new dictionary of AppMessage keys and values.
static void handle_dictionary(DictionaryIterator *iter, void *context) {

    // Settings from the phone's Settings page (Clay -- see src/pkjs). Sent as their own AppMessage
    // on save, independent of any BG push from the firmware, so this doesn't belong gated behind
    // bg_tuple below. One dict_find per field so a partial/older config page still applies whatever
    // it does send.
    Tuple *alerts_low_tuple = dict_find(iter, MESSAGE_KEY_AlertsLow);
    Tuple *alerts_other_tuple = dict_find(iter, MESSAGE_KEY_AlertsOther);
    Tuple *graph_hours_tuple = dict_find(iter, MESSAGE_KEY_GraphHours);
    Tuple *stale_minutes_tuple = dict_find(iter, MESSAGE_KEY_StaleMinutes);
    Tuple *hypo_threshold_tuple = dict_find(iter, MESSAGE_KEY_HypoTreatThreshold);
    Tuple *hypo_alert_tuple = dict_find(iter, MESSAGE_KEY_HypoAlert);
    Tuple *show_meals_tuple = dict_find(iter, MESSAGE_KEY_ShowMeals);
    Tuple *show_hypo_tuple = dict_find(iter, MESSAGE_KEY_ShowHypo);
    Tuple *show_prediction_tuple = dict_find(iter, MESSAGE_KEY_ShowPrediction);
    Tuple *show_trend_tuple = dict_find(iter, MESSAGE_KEY_ShowTrend);
    if (alerts_low_tuple || alerts_other_tuple || graph_hours_tuple || stale_minutes_tuple ||
        hypo_threshold_tuple || hypo_alert_tuple || show_meals_tuple || show_hypo_tuple ||
        show_prediction_tuple || show_trend_tuple) {
        s_alerts_low = prv_tuple_bool(alerts_low_tuple, s_alerts_low);
        s_alerts_other = prv_tuple_bool(alerts_other_tuple, s_alerts_other);
        s_graph_hours = (uint8_t)prv_tuple_uint32(graph_hours_tuple, s_graph_hours);
        s_stale_minutes = (uint8_t)prv_tuple_uint32(stale_minutes_tuple, s_stale_minutes);
        s_hypo_treat_threshold = (uint8_t)prv_tuple_uint32(hypo_threshold_tuple, s_hypo_treat_threshold);
        s_hypo_alert = prv_tuple_bool(hypo_alert_tuple, s_hypo_alert);
        s_show_meals = prv_tuple_bool(show_meals_tuple, s_show_meals);
        s_show_hypo = prv_tuple_bool(show_hypo_tuple, s_show_hypo);
        s_show_prediction = prv_tuple_bool(show_prediction_tuple, s_show_prediction);
        s_show_trend = prv_tuple_bool(show_trend_tuple, s_show_trend);

        persist_write_bool(PERSIST_KEY_ALERTS_LOW, s_alerts_low);
        persist_write_bool(PERSIST_KEY_ALERTS_OTHER, s_alerts_other);
        persist_write_int(PERSIST_KEY_GRAPH_HOURS, s_graph_hours);
        persist_write_int(PERSIST_KEY_STALE_MINUTES, s_stale_minutes);
        persist_write_int(PERSIST_KEY_HYPO_TREAT_THRESHOLD, s_hypo_treat_threshold);
        persist_write_bool(PERSIST_KEY_HYPO_ALERT, s_hypo_alert);
        persist_write_bool(PERSIST_KEY_SHOW_MEALS, s_show_meals);
        persist_write_bool(PERSIST_KEY_SHOW_HYPO, s_show_hypo);
        persist_write_bool(PERSIST_KEY_SHOW_PREDICTION, s_show_prediction);
        persist_write_bool(PERSIST_KEY_SHOW_TREND, s_show_trend);

        // Apply disabled features immediately, rather than waiting for the sender to notice its
        // announced capabilities shrank and stop pushing them -- a toggle should look instant.
        if (!s_show_meals) {
            s_meal_count = 0;
        }
        if (!s_show_hypo) {
            s_hypo_valid = false;
        }
        if (!s_show_prediction) {
            s_pred_valid = false;
        }
        if (!s_show_trend) {
            s_trend_valid = false;
            update_trend_indicator();
        }
        if (s_graph_layer) {
            layer_mark_dirty(s_graph_layer);
        }
        if (s_status_layer) {
            layer_mark_dirty(s_status_layer);
        }
        prv_layout_graph();
        send_capability_announcement(); // relay to the firmware right away, not on the next poll
        APP_LOG(APP_LOG_LEVEL_INFO,
                "settings applied: alerts(low=%d other=%d) graph=%uh stale=%um hypo(thr=%u%% alert=%d) "
                "show(meal=%d hypo=%d pred=%d trend=%d)",
                s_alerts_low, s_alerts_other, s_graph_hours, s_stale_minutes, s_hypo_treat_threshold,
                s_hypo_alert, s_show_meals, s_show_hypo, s_show_prediction, s_show_trend);
    }

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
    Tuple *iob_total_tuple = dict_find(iter, KEY_IOB_TOTAL_STRING);
    if (iob_total_tuple) {
        STRCPY(s_iob_total_string, iob_total_tuple->value->cstring);
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
        s_trend_valid = s_show_trend && (trend_tuple != NULL);
        s_trend_arrow = trend_tuple ? trend_tuple->value->uint8 : TREND_UNKNOWN;
        update_trend_indicator();
    }

    // Prediction: like the trend arrow, sent with a BG push, and its absence clears the last one.
    if (bg_tuple) {
        Tuple *pred_tuple = dict_find(iter, KEY_PREDICTED_BG);
        s_pred_valid = s_show_prediction && (pred_tuple != NULL);
        s_pred_mgdl = pred_tuple ? pred_tuple->value->uint16 : 0;
    }

    // Meals: KEY_MEAL_LIST (every meal still in the window) takes priority; KEY_MEAL_CARBS/
    // KEY_MEAL_TIMESTAMP (the newest one only) is the fallback for a sender that predates it.
    // Either way the sender keeps sending until a change, so absence here means no change.
    if (s_show_meals) {
        Tuple *meal_list_tuple = dict_find(iter, KEY_MEAL_LIST);
        if (meal_list_tuple && parse_meal_list_blob(meal_list_tuple->value->data, meal_list_tuple->length)) {
            if (s_graph_layer)
                layer_mark_dirty(s_graph_layer);
        } else {
            Tuple *meal_tuple = dict_find(iter, KEY_MEAL_CARBS);
            Tuple *meal_ts_tuple = dict_find(iter, KEY_MEAL_TIMESTAMP);
            if (meal_tuple && meal_ts_tuple) {
                s_meal_count = 1;
                s_meal_grams[0] = meal_tuple->value->uint16;
                s_meal_ts[0] = meal_ts_tuple->value->uint32;
                if (s_graph_layer)
                    layer_mark_dirty(s_graph_layer);
            }
        }
    }

    // Hypo (treat-or-wait): like the trend arrow and prediction, sent with a BG push, and its
    // absence clears the last one -- the reading is no longer in the falling-low regime.
    if (bg_tuple) {
        Tuple *hypo_tuple = dict_find(iter, KEY_HYPO_TREAT_PCT);
        Tuple *hypo_plow_tuple = dict_find(iter, KEY_HYPO_P_LOW_PCT);
        const bool new_valid = s_show_hypo && (hypo_tuple != NULL);
        const uint8_t new_pct = hypo_tuple ? hypo_tuple->value->uint8 : 0;
        s_hypo_valid = new_valid;
        s_hypo_pct = new_pct;
        s_hypo_p_low = hypo_plow_tuple ? hypo_plow_tuple->value->uint8 : 0;
        if (s_status_layer)
            layer_mark_dirty(s_status_layer);
        prv_layout_graph(); // s_hypo_valid changed -> the graph's reclaimed bottom space may have too
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
    // Disabled features (Settings page) drop their capability bits too, not just the local draw --
    // no point asking the sender for data nobody will show; it also saves it the work.
    uint32_t caps = CAP_BG | CAP_IOB | CAP_STATUS | CAP_PUMP_CONNECTED | CAP_IOB_TOTAL;
    if (s_show_trend) caps |= CAP_TREND_ARROW;
    if (s_show_meals) caps |= CAP_MEAL | CAP_MEAL_LIST;
    if (s_show_prediction) caps |= CAP_PREDICTION;
    if (s_show_hypo) caps |= CAP_HYPO;
    dict_write_uint8(iter, KEY_PROTOCOL_VERSION, PROTOCOL_VERSION);
    dict_write_uint32(iter, KEY_CAPABILITIES, caps);
    dict_write_uint8(iter, KEY_GRAPH_HOURS, s_graph_hours);
    dict_write_uint8(iter, KEY_SETTINGS_ALERTS, (s_alerts_low ? SETTINGS_ALERT_LOW : 0) |
                                                   (s_alerts_other ? SETTINGS_ALERT_OTHER : 0) |
                                                   (s_hypo_alert ? SETTINGS_ALERT_HYPO_MODEL : 0));
    // Not just CAP_HYPO above: this stops the sender computing the model at all, not just sending
    // it -- ShowHypo off should be a real "completely disabled", not just a hidden result.
    dict_write_uint8(iter, KEY_SETTINGS_FEATURES, s_show_hypo ? SETTINGS_FEATURE_HYPO : 0);
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
    // The default is word-wrap: content too wide for one line silently wraps to a second one, which
    // a box sized for one line then clips vertically -- for the ago/iob corners specifically, that
    // showed up as the reading's leading digit going missing (".1U" for "2.1U") rather than a visibly
    // truncated line. Ellipsis keeps it to one line and truncates visibly instead.
    text_layer_set_overflow_mode(layer, GTextOverflowModeTrailingEllipsis);
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
    // Right/left-aligned text sits at the box's right/left edge regardless of the box's own width
    // (as long as it's wide enough not to truncate), so shrinking top_row_w alone cannot buy any
    // clearance from the centered BG value -- confirmed: it only risks ellipsizing longer content
    // for no benefit. What actually overlapped the BG value's own digits on a 144px screen
    // (confirmed: 6.9 vs 1.4U on flint/aplite/basalt/diorite) is FONT_SECONDARY's rendered width at
    // 28pt; a smaller 24pt clears it by rendering "1.4U"/"45m"-length content narrower, so at the
    // same right/left-aligned edge its far end sits closer to that edge, away from center.
    const char *top_row_font = PBL_DISPLAY_WIDTH < 180 ? FONT_KEY_GOTHIC_24_BOLD : FONT_SECONDARY;
    // Ago/IOB cap top matches the BG value's own cap top (caps_top_y), not their box origin --
    // using the box origin as their y left their caps visibly lower than the BG digits' caps.
    const int top_row_y = caps_top_y - cap_offset(top_row_font);
    const int top_row_h = 28;
    // Widening this only buys clipping headroom on the box's far side from the screen edge -- the
    // right/left-aligned edge itself is anchored to PBL_DISPLAY_WIDTH regardless (see top_row_font's
    // comment above). 52px clipped "2.1U" to ".1U" in the worst case (crowded preset, 4-char BG and
    // a 4-char IOB competing for the same corner on a 144px screen); 60 clears it.
    const int top_row_w = 60;
    // prv_layout_graph needs this before the graph layer below is created; the status layer itself
    // is still created in its own block further down, reusing this same value.
    s_status_top_y = time_caps_y - internal_margin - STATUS_CAP_H - cap_offset(STATUS_FONT) - 3;

    // --- Graph ---------------------------------------------------------------
    // Created first so all text draws over it. The value band runs from under the BG value (and its
    // trend row, only when one is shown -- see prv_layout_graph) to just above the time, so no
    // reading can be drawn over the text, and it grows with the screen instead of running into the
    // time.
    {
        s_graph_bottom_y = time_caps_y - internal_margin;
        // Placeholder frame; prv_layout_graph below sizes it for real, matching s_trend_valid's and
        // s_status_string's startup defaults of "nothing to show" -- update_bg_trend_layout and
        // update_status_display correct it the moment either state is actually known.
        s_graph_layer = make_layer(root, GRect(0, 0, PBL_DISPLAY_WIDTH, 0), graph_layer_update_proc);
        prv_layout_graph();

        // add_debug_outline(layer_get_frame(s_graph_layer)); // Debug (entire layer)
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
        const int x = PBL_IF_RECT_ELSE(edge_margin, PBL_DISPLAY_WIDTH / 10);
        s_ago_layer = make_text_layer(root, GRect(x, top_row_y, top_row_w, top_row_h), top_row_font,
                                      GTextAlignmentLeft);

        // add_debug_outline(GRect(x, top_row_y, top_row_w, top_row_h));
    }

    // --- Insulin on board ----------------------------------------------------
    {
        const int x = PBL_DISPLAY_WIDTH - top_row_w - PBL_IF_RECT_ELSE(edge_margin, PBL_DISPLAY_WIDTH / 10);
        s_iob_layer = make_text_layer(root, GRect(x, top_row_y, top_row_w, top_row_h), top_row_font,
                                      GTextAlignmentRight);

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
        s_status_layer =
            make_layer(root, GRect(0, s_status_top_y, PBL_DISPLAY_WIDTH, STATUS_H), status_layer_update_proc);

        // add_debug_outline(GRect(0, s_status_top_y, PBL_DISPLAY_WIDTH, STATUS_H));
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
    // The exceptions: the phone-configured settings above (persisted, unlike everything else).
    if (persist_exists(PERSIST_KEY_ALERTS_LOW))
        s_alerts_low = persist_read_bool(PERSIST_KEY_ALERTS_LOW);
    if (persist_exists(PERSIST_KEY_ALERTS_OTHER))
        s_alerts_other = persist_read_bool(PERSIST_KEY_ALERTS_OTHER);
    if (persist_exists(PERSIST_KEY_GRAPH_HOURS))
        s_graph_hours = (uint8_t)persist_read_int(PERSIST_KEY_GRAPH_HOURS);
    if (persist_exists(PERSIST_KEY_STALE_MINUTES))
        s_stale_minutes = (uint8_t)persist_read_int(PERSIST_KEY_STALE_MINUTES);
    if (persist_exists(PERSIST_KEY_HYPO_TREAT_THRESHOLD))
        s_hypo_treat_threshold = (uint8_t)persist_read_int(PERSIST_KEY_HYPO_TREAT_THRESHOLD);
    if (persist_exists(PERSIST_KEY_HYPO_ALERT))
        s_hypo_alert = persist_read_bool(PERSIST_KEY_HYPO_ALERT);
    if (persist_exists(PERSIST_KEY_SHOW_MEALS))
        s_show_meals = persist_read_bool(PERSIST_KEY_SHOW_MEALS);
    if (persist_exists(PERSIST_KEY_SHOW_HYPO))
        s_show_hypo = persist_read_bool(PERSIST_KEY_SHOW_HYPO);
    if (persist_exists(PERSIST_KEY_SHOW_PREDICTION))
        s_show_prediction = persist_read_bool(PERSIST_KEY_SHOW_PREDICTION);
    if (persist_exists(PERSIST_KEY_SHOW_TREND))
        s_show_trend = persist_read_bool(PERSIST_KEY_SHOW_TREND);

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
