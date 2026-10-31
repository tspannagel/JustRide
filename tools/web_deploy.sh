#!/bin/sh
# Update the web app on the head unit over Wi-Fi, without taking the SD card out.
# Usage: tools/web_deploy.sh <device address> [--no-vendor]
# First tap "Wi-Fi -> Allow firmware update" on the device (opens a 5-minute window).
# --no-vendor skips DuckDB/uPlot (unchanged between app updates, ~9 MB).
set -eu
HOST=${1:?usage: $0 <device address, e.g. trainer.local or 192.168.178.80> [--no-vendor]}
SKIP_VENDOR=${2:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$ROOT/www/stats/vendor" ] || [ "$SKIP_VENDOR" = --no-vendor ] || { echo "run tools/vendor_stats.sh first"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Same layout as tools/sd_deploy.sh: text and wasm gzipped, served with Content-Encoding
cp -R "$ROOT/www/stats/." "$TMP/"
[ "$SKIP_VENDOR" = --no-vendor ] && rm -rf "$TMP/vendor"
mkdir -p "$TMP/manual"
cp "$ROOT/docs/manual.html" "$TMP/manual/index.html"
cp -R "$ROOT/docs/img" "$TMP/manual/img"
(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev) > "$TMP/version.txt"
find "$TMP" -type f \( -name '*.html' -o -name '*.js' -o -name '*.mjs' -o -name '*.css' -o -name '*.wasm' \) -exec gzip -9 -f {} \;
find "$TMP" -name '._*' -delete

cd "$TMP"
find . -type f | sed 's|^\./||' | sort | while read -r f; do
    printf '%-60s ' "$f"
    code=$(curl -s -o "$TMP/.resp" -w '%{http_code}' -X POST -H 'X-JustRide: 1' -H 'Content-Type: application/octet-stream' \
        --data-binary "@$f" "http://$HOST/www/stats/$f")
    echo "$code $(cat "$TMP/.resp")"
    [ "$code" = 200 ] || { echo "upload failed"; exit 1; }
done
echo "done: open http://$HOST/"
