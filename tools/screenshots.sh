#!/usr/bin/env bash
# Grab watchface screenshots for the app store listing: one emulator at a time, seeded with a
# mock_sender preset, saved as <dir>/<platform>_<preset>.png.
#
#   tools/screenshots.sh                        # every target platform, showcase preset
#   tools/screenshots.sh -p flint -p chalk      # only these platforms
#   tools/screenshots.sh showcase gap           # several presets per emulator
set -euo pipefail

cd "$(dirname "$0")/.."

outdir=screenshots
platforms=()

while getopts "p:o:h" opt; do
	case "$opt" in
	p) platforms+=("$OPTARG") ;;
	o) outdir="$OPTARG" ;;
	h)
		sed -n '2,8p' "$0"
		exit 0
		;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

presets=("$@")
[ ${#presets[@]} -eq 0 ] && presets=(showcase)

# The store wants a shot per platform, so the default list is package.json's targets rather than
# the justfile's one-per-screen-size set.
if [ ${#platforms[@]} -eq 0 ]; then
	mapfile -t platforms < <(python3 -c '
import json
print("\n".join(json.load(open("package.json"))["pebble"]["targetPlatforms"]))')
fi

mkdir -p "$outdir"
pebble build

for platform in "${platforms[@]}"; do
	# Only one emulator at a time: pebble kill takes them all down, and the screenshot then goes to
	# the one that is up without any window/port guessing.
	pebble kill >/dev/null 2>&1 || true
	echo "== $platform"
	pebble install --emulator "$platform"
	for preset in "${presets[@]}"; do
		./tools/mock_sender.py "$preset" --emulator "$platform" \
			--screenshot "$outdir/${platform}_${preset}.png"
	done
done

pebble kill >/dev/null 2>&1 || true
echo "screenshots in $outdir/"
