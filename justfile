set default-list := true

# Build and launch in emulator (in X11 so just resize works)
emu platform="flint":
	pebble build
	SDL_VIDEODRIVER=x11 pebble install --emulator {{platform}}

# Resize QEMU window to an exact pixel multiplier of the platform's native resolution
resize platform="flint" multiplier="2":
	#!/usr/bin/env bash
	set -euo pipefail
	# The platform has to be given: the QEMU window title is just "QEMU" on every platform, and
	# reading the window's own geometry instead would multiply an already-resized window.
	read -r w h < <(./tools/display_size.py {{platform}})
	window_id=$(xdotool search --name 'QEMU' | tail -1 || true)
	[[ -n $window_id ]] || { echo "no QEMU window found — is the emulator running?" >&2; exit 1; }
	xdotool windowsize --sync "$window_id" $((w*{{multiplier}})) $((h*{{multiplier}}))
	# Set it always on top
	wmctrl -i -r "$window_id" -b add,above

# Send mock data to the emulator
seed scenario="":

# Quickly view some mock data in the emulator
quickview platform:
	just emu {{platform}}
	./tools/mock_sender.py --emulator {{platform}}
	just resize {{platform}}
