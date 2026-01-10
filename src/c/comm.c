#include "comm.h"
#include "protocol.h"

// Stored data from last received message
static uint32_t s_timestamp = 0;
static uint16_t s_bg_mmol_x10 = 0;  // mmol/L * 10 (e.g., 7.5 = 75)
static uint8_t s_trend_arrow = ARROW_UNKNOWN;
static int16_t s_delta_mmol_x10 = INT16_MAX;  // mmol/L * 10
static bool s_has_data = false;

// Callback for new data
static CommDataCallback s_data_callback = NULL;

// AppMessage callbacks
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
    // Check for timestamp (always present in data messages)
    Tuple *timestamp_tuple = dict_find(iter, KEY_TIMESTAMP);
    if (timestamp_tuple) {
        s_timestamp = timestamp_tuple->value->uint32;
        s_has_data = true;

        // BG in mmol/L * 10
        Tuple *bg_tuple = dict_find(iter, KEY_BG_MMOL_X10);
        if (bg_tuple) {
            s_bg_mmol_x10 = bg_tuple->value->uint16;
        }

        // Trend arrow
        Tuple *arrow_tuple = dict_find(iter, KEY_TREND_ARROW);
        if (arrow_tuple) {
            s_trend_arrow = arrow_tuple->value->uint8;
        }

        // Delta in mmol/L * 10
        Tuple *delta_tuple = dict_find(iter, KEY_DELTA_MMOL_X10);
        if (delta_tuple) {
            s_delta_mmol_x10 = delta_tuple->value->int16;
        }

        // Notify callback
        if (s_data_callback) {
            s_data_callback();
        }

        APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %d.%d mmol/L, arrow: %d, delta: %d",
                s_bg_mmol_x10 / 10, s_bg_mmol_x10 % 10, s_trend_arrow, s_delta_mmol_x10);
    }
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", reason);
}

static void outbox_sent_handler(DictionaryIterator *iter, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Message sent successfully");
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message send failed: %d", reason);
}

void comm_init(void) {
    // Register callbacks
    app_message_register_inbox_received(inbox_received_handler);
    app_message_register_inbox_dropped(inbox_dropped_handler);
    app_message_register_outbox_sent(outbox_sent_handler);
    app_message_register_outbox_failed(outbox_failed_handler);

    // Open AppMessage with reasonable buffer sizes
    // Inbox needs to be large enough for data message
    // Outbox only needs enough for capability announcement
    const uint32_t inbox_size = 256;
    const uint32_t outbox_size = 64;
    app_message_open(inbox_size, outbox_size);

    APP_LOG(APP_LOG_LEVEL_INFO, "Comm initialized");
}

void comm_deinit(void) {
    app_message_deregister_callbacks();
}

void comm_send_capabilities(void) {
    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);

    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to begin outbox: %d", result);
        return;
    }

    // Protocol version
    dict_write_uint8(iter, KEY_PROTOCOL_VERSION, PROTOCOL_VERSION);

    // Capabilities we want (PoC: BG in mmol/L, trend arrow, delta)
    dict_write_uint32(iter, KEY_CAPABILITIES, DEFAULT_CAPABILITIES);

    // No graph for PoC
    dict_write_uint8(iter, KEY_GRAPH_HOURS, 0);

    result = app_message_outbox_send();
    if (result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to send capabilities: %d", result);
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "Sent capability announcement");
    }
}

uint32_t comm_get_timestamp(void) {
    return s_timestamp;
}

uint16_t comm_get_bg_mmol_x10(void) {
    return s_bg_mmol_x10;
}

uint8_t comm_get_trend_arrow(void) {
    return s_trend_arrow;
}

int16_t comm_get_delta_mmol_x10(void) {
    return s_delta_mmol_x10;
}

bool comm_has_data(void) {
    return s_has_data;
}

void comm_set_data_callback(CommDataCallback callback) {
    s_data_callback = callback;
}
