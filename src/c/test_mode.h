// Compile-time dummy data so the watchface can be verified in the emulator without a phone.
// Enable by uncommenting TEST_MODE below and rebuilding. Note that `pebble build -- -DTEST_MODE`
// does NOT work: this wscript doesn't forward the flag, so editing this file is the only way.
// Left disabled by default.

#pragma once

// #define TEST_MODE

#ifdef TEST_MODE
#define TEST_BG_STRING "7.5"
#define TEST_MINUTES_AGO 2
#define TEST_IOB_STRING "2.5"
#define TEST_STATUS_STRING "" // "" = full graph shows; set e.g. "SUSPENDED" to test the overlay
#endif
