#!/bin/sh
# Update the web app on the head unit over Wi-Fi, without taking the SD card out.
# Usage: tools/web_deploy.sh [address] [--check] [--force]
#   address   default trainer.local (or an IP address)
#   --check   only say whether the device is behind (exit 1 if so); changes nothing, needs no tap
#   --force   upload every file, whatever the device says it has
# Only files that changed are sent: the device keeps a list of what it has (deploy-manifest.txt).
# When the upload window is closed, the script waits for "Wi-Fi -> Allow firmware update" on the
# device. A new version alone (no file changed) is not worth a tap: nothing is sent then.
set -eu
HOST=trainer.local CHECK=0 FORCE=0
for a in "$@"; do
    case $a in
        --check) CHECK=1 ;;
        --force) FORCE=1 ;;
        -*) echo "usage: $0 [address] [--check] [--force]"; exit 2 ;;
        *) HOST=$a ;;
    esac
done
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$ROOT/www/stats/vendor" ] || { echo "run tools/vendor_stats.sh first"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MANIFEST=deploy-manifest.txt

# -4: a .local name takes 5 s per request otherwise (the device doesn't answer the IPv6 lookup)
upload_left() { curl -4 -s -m 5 "http://$HOST/api/info" | sed -n 's/.*"upload_s": *\([0-9]*\).*/\1/p'; }
fetch() { curl -4 -sf -m 15 "http://$HOST/stats/$1"; }

# Waits until the device accepts uploads; one tap is enough, this also covers a window that ran out
wait_window() {
    [ "$(upload_left)" -gt 0 ] 2>/dev/null && return 0
    echo "On the device, tap Wi-Fi -> Allow firmware update (waiting up to 5 minutes)"
    i=0
    while [ "$i" -lt 150 ]; do
        sleep 2
        [ "$(upload_left)" -gt 0 ] 2>/dev/null && return 0
        i=$((i + 1))
    done
    echo "no tap, nothing more uploaded; run this again"
    exit 1
}

# The same files as tools/sd_deploy.sh
APP=$TMP/app
mkdir -p "$APP/manual/img"
cp -R "$ROOT/www/stats/." "$APP/"
cp "$ROOT/docs/manual.html" "$APP/manual/index.html"
cp "$ROOT"/docs/img/*.png "$APP/manual/img/"
VERSION=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)
echo "$VERSION" > "$APP/version.txt"
find "$APP" \( -name '._*' -o -name '.DS_Store' \) -delete

cd "$APP"
find . -type f -exec shasum -a 256 {} + | sed 's|  \./|  |' | LC_ALL=C sort > "$TMP/want"
curl -4 -s -m 5 -o /dev/null "http://$HOST/api/info" || {
    echo "$HOST doesn't answer: is it on, on this network, and not in a ride (Wi-Fi is off then)?"
    exit 1
}
fetch "$MANIFEST" 2>/dev/null | LC_ALL=C sort > "$TMP/have" || true
[ "$FORCE" = 0 ] || : > "$TMP/have"
LC_ALL=C comm -12 "$TMP/want" "$TMP/have" > "$TMP/same"
# Vendor first (versioned paths, the app needs them there), then the app, its version, the manual
LC_ALL=C comm -23 "$TMP/want" "$TMP/have" | sed 's/^[^ ]*  //' | awk '
    /^vendor\//      { print 0, $0; next }
    $0 == "version.txt" { print 2, $0; next }
    /^manual\//      { print 3, $0; next }
    { print 1, $0 }' | LC_ALL=C sort | sed 's/^[0-9] //' > "$TMP/todo"
grep -vx 'version.txt' "$TMP/todo" > "$TMP/changed" || true

if [ ! -s "$TMP/changed" ]; then
    echo "The device is up to date ($(fetch version.txt || echo unknown))"
    exit 0
fi
if [ "$CHECK" = 1 ]; then
    echo "The device is behind ($(fetch version.txt || echo unknown), this copy: $VERSION), $(wc -l < "$TMP/changed" | tr -d ' ') files differ:"
    sed 's/^/  /' "$TMP/changed"
    exit 1
fi

# Retries a failed file: a window that ran out, or Wi-Fi hiccups
upload() {
    tries=0
    while :; do
        wait_window
        code=$(curl -4 -s -m 300 -o "$TMP/resp" -w '%{http_code}' -X POST -H 'X-JustRide: 1' \
            -H 'Content-Type: application/octet-stream' --data-binary "@$1" "http://$HOST/www/stats/$2") || code=000
        [ "$code" = 200 ] && return 0
        tries=$((tries + 1))
        if [ "$tries" -ge 3 ]; then
            echo "upload of $2 failed: $code $(cat "$TMP/resp" 2>/dev/null)"
            exit 1
        fi
        sleep 2
    done
}

# Listed as present only once uploaded: a deploy cut short is simply picked up by the next one.
# The first line keeps the file from being empty (the device refuses empty uploads).
{ echo '# deploy in progress'; cat "$TMP/same"; } > "$MANIFEST"
upload "$MANIFEST" "$MANIFEST"
while read -r f; do
    name=$f
    case $f in
        *.html | *.js | *.mjs | *.css | *.wasm) gzip -9 -n "$f"; f=$f.gz; name=$f ;;
    esac
    printf '%-60s %10s\n' "$name" "$(wc -c < "$f" | tr -d ' ')"
    upload "$f" "$name"
done < "$TMP/todo"
cp "$TMP/want" "$MANIFEST"
upload "$MANIFEST" "$MANIFEST"

[ "$(fetch version.txt)" = "$VERSION" ] && fetch "$MANIFEST" | cmp -s - "$TMP/want" || {
    echo "the device doesn't show the new version: run this again"
    exit 1
}
echo "done: the device has $VERSION ($(wc -l < "$TMP/todo" | tr -d ' ') files sent). Reload http://$HOST/"
