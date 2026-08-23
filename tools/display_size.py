#!/usr/bin/env python3
"""Print "<width> <height>" for a Pebble platform, from the SDK's own platform table.

Used by `just resize`; the lookup lives in mock_sender.py so there is one copy of it.
"""

import sys

from mock_sender import sdk_platforms

if len(sys.argv) != 2:
    sys.exit("usage: display_size.py <platform>")

sizes = sdk_platforms()
if sys.argv[1] not in sizes:
    sys.exit("unknown platform %r; known: %s" % (sys.argv[1], ", ".join(sorted(sizes))))
print("%d %d" % sizes[sys.argv[1]])
