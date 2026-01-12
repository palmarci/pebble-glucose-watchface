#include "comm.h"
#include "constants.h"

// Buffer sizes for received strings
#define BG_STRING_LEN 8
#define DELTA_STRING_LEN 8

// Stored data from last received message
static uint32_t s_timestamp = 0;
static char s_bg_string[BG_STRING_LEN] = "";
static uint8_t s_trend_arrow = ARROW_UNKNOWN;
static char s_delta_string[DELTA_STRING_LEN] = "";
static bool s_has_data = false;

// Callback for new data
static CommDataCallback s_data_callback = NULL;

// AppMessage callbacks
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
    // Check for timestamp (always present in data messages)
    Tuple *timestamp_tuple = dict_find(iter, KEY_BG_TIMESTAMP);
    if (timestamp_tuple) {
        s_timestamp = timestamp_tuple->value->uint32;
        s_has_data = true;

        // BG as string
        Tuple *bg_tuple = dict_find(iter, KEY_BG_STRING);
        if (bg_tuple) {
            strncpy(s_bg_string, bg_tuple->value->cstring, BG_STRING_LEN - 1);
            s_bg_string[BG_STRING_LEN - 1] = '\0';
        }

        // Trend arrow
        Tuple *arrow_tuple = dict_find(iter, KEY_ARROW_INDEX);
        if (arrow_tuple) {
            s_trend_arrow = arrow_tuple->value->uint8;
        }

        // Delta as string
        Tuple *delta_tuple = dict_find(iter, KEY_DELTA_STRING);
        if (delta_tuple) {
            strncpy(s_delta_string, delta_tuple->value->cstring, DELTA_STRING_LEN - 1);
            s_delta_string[DELTA_STRING_LEN - 1] = '\0';
        }

        // Notify callback
        if (s_data_callback) {
            s_data_callback();
        }

        APP_LOG(APP_LOG_LEVEL_INFO, "Received BG: %s, arrow: %d, delta: %s",
                s_bg_string, s_trend_arrow, s_delta_string);
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
    // Inbox needs to be large enough for string data
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

    // Capabilities we want (BG, trend arrow, delta)
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

const char* comm_get_bg_string(void) {
    return s_bg_string;
}

uint8_t comm_get_trend_arrow(void) {
    return s_trend_arrow;
}

const char* comm_get_delta_string(void) {
    return s_delta_string;
}

bool comm_has_data(void) {
    return s_has_data;
}

void comm_set_data_callback(CommDataCallback callback) {
    s_data_callback = callback;
}
