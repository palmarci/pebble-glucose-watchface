// Watchface display strings, gathered here for easy tweaking.
//
// NOTE: the pump-STATUS text (SUSPENDED / SmartGuard off / Temp target / BG Required / Calibrate /
// Safe basal) is NOT here — it is composed on the phone and sent over AppMessage, so the watchface
// just displays whatever string it receives. Tweak that wording in the Android app:
//   minimed-pebble-bridge/app/src/main/java/com/mortenfyhn/minimedpebble/StatusLabels.kt

#pragma once

#define STR_NO_DATA "---"       // BG shown when there's no reading yet, or it's stale
#define STR_IOB_FMT "%sU"       // insulin on board, e.g. "2.5U"
#define STR_AGO_MIN_FMT "%dm"   // age of the current BG value, in minutes, e.g. "5m"
#define STR_AGO_HOURS_FMT "%dh" // age of the current BG value, >= 1 hour
#define STR_TIME_24H_FMT "%H:%M"
#define STR_TIME_12H_FMT "%I:%M"
#define STR_DATE_FMT "%a %d %b"      // e.g. "Sun 12 Jul"
