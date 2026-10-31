#!/bin/sh
# Prepare a microSD card (FAT32) for JustRide. Usage: tools/sd_deploy.sh /Volumes/SDCARD
# Copies the statistics app (gzipped; the device serves *.gz with Content-Encoding)
# and the sample workouts and route. Never touches /rides or /config.
set -eu
SD=${1:?usage: $0 <sd card mount point>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$ROOT/www/stats/vendor" ] || { echo "run tools/vendor_stats.sh first"; exit 1; }
[ -d "$SD" ] || { echo "$SD not found"; exit 1; }

rm -rf "$SD/www/stats"
mkdir -p "$SD/www/stats" "$SD/workouts" "$SD/rides" "$SD/config"
cp -R "$ROOT/www/stats/." "$SD/www/stats/"
# The manual, served as /stats/manual/index.html (linked from the web app)
mkdir -p "$SD/www/stats/manual"
cp "$ROOT/docs/manual.html" "$SD/www/stats/manual/index.html"
cp -R "$ROOT/docs/img" "$SD/www/stats/manual/img"
# Shown on the Device page (releases write the tag here)
(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev) > "$SD/www/stats/version.txt"
find "$SD/www/stats" -type f \( -name '*.html' -o -name '*.js' -o -name '*.mjs' -o -name '*.css' -o -name '*.wasm' \) \
    -exec gzip -9 -f {} \;
for f in "$ROOT"/tools/workouts/*.zwo "$ROOT"/tools/workouts/*.route; do
    [ -e "$SD/workouts/$(basename "$f")" ] || cp "$f" "$SD/workouts/"
done
# macOS litter on FAT cards
find "$SD/www" -name '._*' -delete 2>/dev/null || true
echo "SD card ready: $SD"
