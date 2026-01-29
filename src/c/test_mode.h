// For testing with dummy data in emulator.
// Separate file to avoid diff in main file when testing.

#pragma once

// Uncomment to enable test mode
#define TEST_MODE

#define TEST_BG_STRING "10.2"
#define TEST_DELTA_STRING "+0.3"
#define TEST_MINUTES_AGO 3
#define TEST_ARROW_INDEX 4 // 4 = flat arrow

// Test graph data: simulates 3 hours of data with some variation
// This will create a nice curve to visualize
#define TEST_GRAPH_COUNT 36
// Offsets: 0, 5, 10, 15, ... 175 minutes (every 5 minutes for 3 hours)
// BG values: oscillating between ~100-200 mg/dL to show variation
