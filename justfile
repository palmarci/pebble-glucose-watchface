set default-list := true

# One emulator per distinct screen size: flint 144x168, chalk 180x180, emery 200x228,
# gabbro 260x260. The platforms left out (aplite, basalt, diorite) are all 144x168 like flint,
# so they render identically.
platforms := "flint chalk emery gabbro"

# Build and launch in emulator (in X11 so just resize works)
emu platform="flint":
	pebble build
	SDL_VIDEODRIVER=x11 pebble install --emulator {{platform}}

# Build and launch every distinct screen size at once, for comparing layouts
emu-all:
	#!/usr/bin/env bash
	set -euo pipefail
	pebble build
	for platform in {{platforms}}; do
		SDL_VIDEODRIVER=x11 pebble install --emulator "$platform"
	done

# Resize QEMU window to an exact pixel multiplier of the platform's native resolution
resize platform="flint" multiplier="2":
	#!/usr/bin/env bash
	set -euo pipefail
	# The platform has to be given: reading the window's own geometry instead would multiply an
	# already-resized window.
	read -r w h < <(./tools/display_size.py {{platform}})
	window_id=$(./tools/emu_window.py {{platform}})
	xdotool windowsize --sync "$window_id" $((w*{{multiplier}})) $((h*{{multiplier}}))
	# Set it always on top
	wmctrl -i -r "$window_id" -b add,above

# Send mock data to every running emulator (default preset if none given)
seed preset="":
	#!/usr/bin/env bash
	set -euo pipefail
	for platform in $(./tools/emu_window.py --running); do
		./tools/mock_sender.py {{preset}} --emulator "$platform"
	done

# Quickly view some mock data in the emulator
quickview platform:
	just emu {{platform}}
	./tools/mock_sender.py --emulator {{platform}}
	just resize {{platform}}
