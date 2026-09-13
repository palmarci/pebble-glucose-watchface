// MiniMed -> Pebble watchface (proof of concept).
//
// Displays the current blood glucose value pushed from the Android bridge app
// over AppMessage, plus how long ago it arrived, a 2 h graph, and the current time/date.
// The trend projection is NOT taken from the pump; it's extrapolated on-watch from the recent BG data
// (issue #1) and drawn as a dotted line continuing the graph line from its latest point.

#include <pebble.h>

#include "protocol.h"

// --- Constants ---

// Graph config
#define GRAPH_HOURS 2  // Hours of graph data
#define STROKE_WIDTH 3 // Graph stroke width in pixels
#define STROKE_OFFSET (STROKE_WIDTH / 2)

// --- Messy stuff, to be cleaned up ---

// Show "---" instead of a stale value once the last reading is this old. CGM cadence is 5 min, so
// keep the last value on screen across a couple of missed readings before giving up on it.
// MUST match the bridge's STALE_SECONDS (minimed-pebble-bridge BridgeForegroundService) so the watch
// and the phone status-bar icon go stale at the same time.
#define STALE_MINUTES 15

// Graph config. We ask the phone for (and buffer) up to GRAPH_MAX_HOURS of history, but the visible
// window is fixed to GRAPH_HOURS (below); the extra buffered history is kept for future use.
#define GRAPH_MAX_HOURS 24   // history requested from / buffered for the phone; announced as our capability
#define MAX_GRAPH_POINTS 300 // 24 h @ 5 min = 288, + headroom
// persist_write_data caps at 256 B/key (uint16 offsets -> 128 points); a larger graph isn't
// persisted — the phone's ready-ping resend refills it.
#define PERSIST_MAX_POINTS 128
// Fixed y-axis 2.2–16 mmol/L, in "mg/dL / 2" wire units (40..288 mg/dL). Out-of-range clamps to edge.
#define GRAPH_VALUE_MIN 20
#define GRAPH_VALUE_MAX 144
// Don't connect points more than this far apart (a sensor gap draws as a break, not a straight line).
#define GRAPH_GAP_THRESHOLD_MINUTES 15

// Issue #1: the graph area is fixed to the last 2 h (regardless of the phone's KEY_GRAPH_HOURS) and
// occupies the left 2/3 of the screen; the right 1/3 shows the extrapolated trend projection (see below).

#define GRAPH_WIDTH_NUM 2 // graph width = screen width * NUM/DEN; the rest is the projection region
#define GRAPH_WIDTH_DEN 3
// The value band: BG values map into these GRAPH_BAND_H pixels, starting at this screen y.
#define GRAPH_BAND_TOP_Y 38
#define GRAPH_BAND_H 64
// The layer is taller than the value band so a projection leaving a reading near the top or bottom of
// the range has somewhere to go instead of being clipped away (its length is clamped to the layer, so
// it shortens rather than vanishing). Asymmetric: more spare screen below the band than above it.
#define GRAPH_PAD_TOP 8
#define GRAPH_PAD_BOTTOM 14
// Derived. Axes, trace and projection all live in this one layer, so there is a single coordinate space
// and the projection pivot cannot drift off the trace. It sits behind the time/BG text, which stay on top.
#define GRAPH_LAYER_TOP_Y (GRAPH_BAND_TOP_Y - GRAPH_PAD_TOP)
#define GRAPH_LAYER_H (GRAPH_PAD_TOP + GRAPH_BAND_H + GRAPH_PAD_BOTTOM)

// Trend projection (issue #1). Extrapolated on-watch from recent BG, NOT read from the pump. Its angle is
// the graph's own visual slope (same px/min and px/value as the trace), so it lies tangent to how the
// line would continue from the latest point — not an arbitrary rate->angle mapping. Drawn as a dotted line
// so it reads clearly as a projection, distinct from the solid data trace. See draw_projection for the
// slope estimator and why it was chosen.
#define TREND_MAX_GAP_MINUTES 15 // ignore the last two points if a sensor gap wider than this separates them
#define TREND_PROJ_LEN 12        // projection length start-to-end in px (clamped to stay inside the band)
#define TREND_PROJ_GAP 6         // gap (px) between the trace's last point and the projection start
#define TREND_DOT_COUNT 3        // dots drawn along the projection, spread over its (clamped) length

// Persistent-storage keys (survive watchface unload and watch reboot). Separate namespace from the
// AppMessage keys in protocol.h. Leaving the watchface for the menu and returning relaunches the app,
// which would otherwise reset the in-RAM graph to empty; we save on unload and reload on launch.
#define PERSIST_BG_STRING 1
#define PERSIST_BG_TIMESTAMP 2
#define PERSIST_IOB_STRING 3
#define PERSIST_STATUS_STRING 4
#define PERSIST_STATUS_START 5
#define PERSIST_STATUS_END 6
#define PERSIST_GRAPH_REF 7
#define PERSIST_GRAPH_COUNT 8
#define PERSIST_GRAPH_OFFSETS 9
#define PERSIST_GRAPH_VALUES 10
#define PERSIST_GRAPH_HIGH 11
#define PERSIST_GRAPH_LOW 12

// Status strip: a full-width opaque white band hugging the status text, sitting low over the graph so
// its uppercase letters land ~2px above the time. Custom-drawn (not a TextLayer background) so the
// band can be full width yet vertically tight to the caps. Layer-local coords, like every other layer
// here; it paints only the band + text, leaving the rest transparent so the graph shows through.
#define STATUS_FONT FONT_KEY_GOTHIC_18_BOLD
#define STATUS_TOP_Y 92        // layer top (screen y), which is also the text box's top
#define STATUS_H 24            // one line of STATUS_FONT, with room for descenders
#define STATUS_BAND_OFFSET_Y 4 // band top within the layer; the font's top padding drops the caps into it
#define STATUS_BAND_H 17       // band height (caps + a little room)

static Window *s_window;
static TextLayer *s_bg_layer;
static TextLayer *s_ago_layer;
static TextLayer *s_iob_layer;
static Layer *s_status_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static Layer *s_graph_layer; // axes, trace and projection all draw here
static Layer *s_debug_layer; // layer outlines for layout debugging

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

static void add_debug_outline(GRect frame) {
    if (s_num_debug_outlines < DEBUG_MAX_OUTLINES) {
        s_debug_outlines[s_num_debug_outlines++] = frame;
    }
}

static void debug_layer_update_proc(Layer *layer, GContext *ctx) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
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

static void update_bg_display(void) {
    // Stale -> blank the number rather than showing a value that hasn't updated in a while (a stale BG
    // sat on screen for ~8 h during an overnight outage). The "ago" label still conveys how old it is.
    // While fresh, show verbatim what the phone sent: "---" appears only when the phone sends it (the
    // pump has no sensor value), so the watch never invents it -- every "---" mirrors the pump.
    // Guard the layer: a data message can arrive before window_load creates it (the on-watch sender
    // injects with zero latency, unlike a phone's), and text_layer_set_text(NULL,..) hard-faults.
    if (s_bg_layer)
        text_layer_set_text(s_bg_layer, is_stale() ? "" : s_bg_string);
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

static void update_time_and_date(void) {
    time_t now = time(NULL);
    struct tm *time = localtime(&now);

    strftime(s_time_display, sizeof(s_time_display), clock_is_24h_style() ? "%H:%M" : "%I:%M", time);

    if (time->tm_mday < 10) {
        // %e = " 9" with a space or "10"
        strftime(s_date_display, sizeof(s_date_display), "%a%e   W%V", time);
    } else {
        // %d = "09" with a zero, or "10"
        strftime(s_date_display, sizeof(s_date_display), "%a %d   W%V", time);
    }

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

    if (show_timer) {
        snprintf(status_with_timer, sizeof(status_with_timer), "%s %lu:%02lu", s_status_string, hours, minutes);
    } else {
        snprintf(status_with_timer, sizeof(status_with_timer), "%s", s_status_string);
    }

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, status_with_timer, fonts_get_system_font(STATUS_FONT), GRect(0, 0, w, STATUS_H),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Map a BG value (mg/dL / 2) to a y inside the graph layer, clamping to the fixed range. The only place
// that knows where the value band sits within the layer, so axes, trace and projection cannot disagree.
static int graph_y(int bg) {
    if (bg < GRAPH_VALUE_MIN)
        bg = GRAPH_VALUE_MIN;
    if (bg > GRAPH_VALUE_MAX)
        bg = GRAPH_VALUE_MAX;
    return GRAPH_PAD_TOP + GRAPH_BAND_H - ((bg - GRAPH_VALUE_MIN) * GRAPH_BAND_H) / (GRAPH_VALUE_MAX - GRAPH_VALUE_MIN);
}

static void draw_graph_axes(GContext *ctx, GRect bounds) {
    const int width = bounds.size.w;
    const int hi_y = graph_y(s_graph_high_line);
    const int lo_y = graph_y(s_graph_low_line);
    graphics_context_set_stroke_color(ctx, GColorBlack);

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

    graphics_context_set_stroke_color(ctx, GColorBlack);
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
    graphics_context_set_fill_color(ctx, GColorBlack);
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
    float slope;
    if (!trend_slope(&slope))
        return;

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
    const float px_per_wire = (float)GRAPH_BAND_H / (GRAPH_VALUE_MAX - GRAPH_VALUE_MIN);
    const GPoint pivot = GPoint(newest_x, graph_y(s_graph_bg_values[s_graph_count - 1]));
    trend_draw_projection(ctx, bounds, pivot, slope, px_per_min, px_per_wire);
}

// Axes behind the trace, projection on top of both.
static void graph_layer_update_proc(Layer *layer, GContext *ctx) {
    const GRect bounds = layer_get_bounds(layer);
    draw_graph_axes(ctx, bounds);
    draw_bg_graph(ctx, bounds);
    draw_projection(ctx, bounds);
}

static void tick_callback(struct tm *tick_time, TimeUnits units_changed) {
    update_time_and_date();
    update_ago_display(); // advances the staleness hint each minute
    // Re-run on the tick, not just on receipt: during an outage no message arrives, so this is what
    // blanks the BG/IOB once they cross STALE_MINUTES.
    update_bg_display();
    update_iob_display();

    if (s_status_layer && (s_status_start != 0 || s_status_end != 0))
        layer_mark_dirty(s_status_layer);

    // Redraw the graph too: point x-positions are computed from the current time, so without this the
    // trace freezes between the 5-min pushes (doesn't creep left, old points don't fall off the edge).
    // TODO: Consider if this is worth it, probably eats some battery
    if (s_graph_layer)
        layer_mark_dirty(s_graph_layer); // trace scrolls and the projection goes stale together
}

// Persist the current reading + graph so relaunching the watchface (e.g. after the menu) shows it
// immediately instead of an empty graph. Called on unload; the phone also re-sends on the ready ping.
static void save_state(void) {
    persist_write_string(PERSIST_BG_STRING, s_bg_string);
    persist_write_int(PERSIST_BG_TIMESTAMP, (int32_t)s_bg_timestamp);
    persist_write_string(PERSIST_IOB_STRING, s_iob_string);
    persist_write_string(PERSIST_STATUS_STRING, s_status_string);
    persist_write_int(PERSIST_STATUS_START, (uint32_t)s_status_start);
    persist_write_int(PERSIST_STATUS_END, (uint32_t)s_status_end);
    persist_write_int(PERSIST_GRAPH_REF, (int32_t)s_graph_ref_timestamp);
    persist_write_int(PERSIST_GRAPH_HIGH, s_graph_high_line);
    persist_write_int(PERSIST_GRAPH_LOW, s_graph_low_line);
    // persist_write_data caps at 256 B/key, so only persist reasonably small graphs; a larger one is
    // left out (COUNT=0) and refilled by the phone's resend on the ready ping after relaunch.
    if (s_graph_count > 0 && s_graph_count <= PERSIST_MAX_POINTS) {
        persist_write_int(PERSIST_GRAPH_COUNT, s_graph_count);
        persist_write_data(PERSIST_GRAPH_OFFSETS, s_graph_offsets, s_graph_count * sizeof(uint16_t));
        persist_write_data(PERSIST_GRAPH_VALUES, s_graph_bg_values, s_graph_count * sizeof(uint8_t));
    } else {
        persist_write_int(PERSIST_GRAPH_COUNT, 0);
    }
}

static void load_state(void) {
    if (persist_exists(PERSIST_BG_STRING))
        persist_read_string(PERSIST_BG_STRING, s_bg_string, sizeof(s_bg_string));
    if (persist_exists(PERSIST_BG_TIMESTAMP))
        s_bg_timestamp = (uint32_t)persist_read_int(PERSIST_BG_TIMESTAMP);
    if (persist_exists(PERSIST_IOB_STRING))
        persist_read_string(PERSIST_IOB_STRING, s_iob_string, sizeof(s_iob_string));
    if (persist_exists(PERSIST_STATUS_STRING))
        persist_read_string(PERSIST_STATUS_STRING, s_status_string, sizeof(s_status_string));
    if (persist_exists(PERSIST_STATUS_START))
        s_status_start = (uint32_t)persist_read_int(PERSIST_STATUS_START);
    if (persist_exists(PERSIST_STATUS_END))
        s_status_end = (uint32_t)persist_read_int(PERSIST_STATUS_END);
    if (persist_exists(PERSIST_GRAPH_HIGH))
        s_graph_high_line = (uint8_t)persist_read_int(PERSIST_GRAPH_HIGH);
    if (persist_exists(PERSIST_GRAPH_LOW))
        s_graph_low_line = (uint8_t)persist_read_int(PERSIST_GRAPH_LOW);
    if (persist_exists(PERSIST_GRAPH_COUNT) && persist_exists(PERSIST_GRAPH_OFFSETS) &&
        persist_exists(PERSIST_GRAPH_VALUES)) {
        uint16_t count = (uint16_t)persist_read_int(PERSIST_GRAPH_COUNT);
        if (count > MAX_GRAPH_POINTS)
            count = MAX_GRAPH_POINTS;
        if (count > 0) {
            persist_read_data(PERSIST_GRAPH_OFFSETS, s_graph_offsets, count * sizeof(uint16_t));
            persist_read_data(PERSIST_GRAPH_VALUES, s_graph_bg_values, count * sizeof(uint8_t));
            s_graph_ref_timestamp = (uint32_t)persist_read_int(PERSIST_GRAPH_REF);
            s_graph_count = count;
        }
    }
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
    dict_write_uint8(iter, KEY_GRAPH_HOURS, GRAPH_MAX_HOURS); // the most we can display; sender may send less
    if (app_message_outbox_send() != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "outbox_send failed");
    }
}

static void bluetooth_callback(bool connected) {
    if (connected) {
        send_ready();
    }
}

static TextLayer *make_text_layer(Layer *root, GRect frame, const char *font_key, GTextAlignment align) {
    TextLayer *layer = text_layer_create(frame);
    text_layer_set_background_color(layer, GColorClear);
    text_layer_set_text_color(layer, GColorBlack);
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
    window_set_background_color(window, GColorWhite);
    Layer *root = window_get_root_layer(window);

    const int edge_margin = PBL_IF_RECT_ELSE(6, 12);
    const int internal_margin = 3;

    // --- BG value ------------------------------------------------------------
    {
#if PBL_RECT
        const int x = edge_margin;
        const int w = 92; // Exactly enough for "20.0" in Bitham 42 bold
        const GTextAlignment a = GTextAlignmentRight;
#else
        const int x = 0;
        const int w = PBL_DISPLAY_WIDTH;
        const GTextAlignment a = GTextAlignmentCenter;
#endif
        const int y = edge_margin - cap_offset(FONT_KEY_BITHAM_42_BOLD);
        const int h = 42;

        s_bg_layer = make_text_layer(root, GRect(x, y, w, h), FONT_KEY_BITHAM_42_BOLD, a);
        // add_debug_outline(GRect(x, y, w, h));
    }

    // --- Time ago ------------------------------------------------------------
    const int y_time_ago = PBL_IF_RECT_ELSE(
        4 - cap_offset(FONT_KEY_GOTHIC_24_BOLD), // Margin 4 makes the IOB layer below perfectly match the BG baseline
        PBL_DISPLAY_HEIGHT / 6);
    {
        const int w = 48;
        const int h = 24;
        const int y = y_time_ago;

#if PBL_RECT
        // Top right
        const int x = PBL_DISPLAY_WIDTH - w - edge_margin;
        const GTextAlignment a = GTextAlignmentRight;
#else
        // Top left
        const int x = PBL_DISPLAY_HEIGHT / 10;
        const GTextAlignment a = GTextAlignmentLeft;
#endif

        s_ago_layer = make_text_layer(root, GRect(x, y, w, h), FONT_KEY_GOTHIC_24_BOLD, a);
        // add_debug_outline(GRect(x, y, w, h));
    }

    // --- Insulin on board ----------------------------------------------------
    {
        const int w = 48;
        const int h = 24;
        const GTextAlignment a = GTextAlignmentRight;

#if PBL_RECT
        const int x = PBL_DISPLAY_WIDTH - w - edge_margin;
        const int y = y_time_ago + 24 - cap_offset(FONT_KEY_GOTHIC_24_BOLD) + internal_margin;
#else
        const int x = PBL_DISPLAY_WIDTH - w - PBL_DISPLAY_WIDTH / 10;
        const int y = PBL_DISPLAY_HEIGHT / 6;
#endif
        s_iob_layer = make_text_layer(root, GRect(x, y, w, h), FONT_KEY_GOTHIC_24_BOLD, a);
        // add_debug_outline(GRect(x, y, w, h));
    }

    // --- Graph ---------------------------------------------------------------
    const int y_graph = (PBL_DISPLAY_HEIGHT - GRAPH_LAYER_H) / 2 - 11;
    {
        const int y = y_graph;
        s_graph_layer = make_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, GRAPH_LAYER_H), graph_layer_update_proc);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, GRAPH_LAYER_H)); // Debug (entire layer)
        // add_debug_outline(GRect(0, y + GRAPH_PAD_TOP, PBL_DISPLAY_WIDTH, GRAPH_BAND_H)); // Debug (data band only)
    }

    // --- Status --------------------------------------------------------------
    {
        const int y = y_graph + GRAPH_LAYER_H - STATUS_H;
        const int h = STATUS_H; // Todo tighten and unify
        s_status_layer = make_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), status_layer_update_proc);

        // add_debug_outline(GRect(0, STATUS_TOP_Y, PBL_DISPLAY_WIDTH, STATUS_H));
    }

    // --- Date ----------------------------------------------------------------
    const int date_y = PBL_DISPLAY_HEIGHT - edge_margin - 24;
    {
        const int h = 24;
        const int y = date_y;
        s_date_layer =
            make_text_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), FONT_KEY_GOTHIC_24_BOLD, GTextAlignmentCenter);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, h));
    }

    // --- Time ----------------------------------------------------------------
    {
        const int h = 42;
        const int y = date_y + cap_offset(FONT_KEY_GOTHIC_24_BOLD) - internal_margin - h;
        s_time_layer =
            make_text_layer(root, GRect(0, y, PBL_DISPLAY_WIDTH, h), FONT_KEY_BITHAM_42_BOLD, GTextAlignmentCenter);

        // add_debug_outline(GRect(0, y, PBL_DISPLAY_WIDTH, h));
    }

    // Last, so the outlines draw over every other layer.
    s_debug_layer = make_layer(root, layer_get_bounds(root), debug_layer_update_proc);

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
    layer_destroy(s_debug_layer);
}

static void init(void) {
    load_state(); // restore last reading + graph so a relaunch renders immediately, not empty
    app_message_register_inbox_received(handle_dictionary);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_open(2048, 64); // inbox large enough for the graph byte array (up to 24 h of points)

    tick_timer_service_subscribe(MINUTE_UNIT, tick_callback);
    connection_service_subscribe((ConnectionHandlers){.pebble_app_connection_handler = bluetooth_callback});

    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){.load = window_load, .unload = window_unload});
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
    init();
    app_event_loop();
    deinit();
}
