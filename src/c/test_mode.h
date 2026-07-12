// Compile-time dummy data so the watchface can be verified in the emulator
// without a phone. Enable by building with TEST_MODE defined, e.g.:
//   pebble build -- -DTEST_MODE
// Left disabled by default.

#pragma once

// #define TEST_MODE

#ifdef TEST_MODE
#define TEST_BG_STRING "7.5"
#define TEST_MINUTES_AGO 2
#define TEST_IOB_STRING "2.5"
#define TEST_STATUS_STRING "SmartGuard off" // worst-case longest label
#endif
