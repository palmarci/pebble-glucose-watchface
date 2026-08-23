set default-list := true

# Seed emulator with mock data
seed scenario="":
	./tools/mock_sender.py {{scenario}}

# Build and launch in emulator (in X11 so just resize works)
emu platform="flint":
	pebble build
	SDL_VIDEODRIVER=x11 pebble install --emulator {{platform}}

# Resize QEMU window to an exact pixel multiplier
resize multiplier="2":
	#!/usr/bin/env bash
	# Find the window
	window_id=$(xdotool search --name -- 'Pebble|QEMU' | tail -1)
	# Resize it
	xdotool windowsize --sync $window_id $((144*{{multiplier}})) $((168*{{multiplier}}))
	# Set it always on top
	wmctrl -i -r $window_id -b add,above
