#!/bin/sh
# Renders the device screens (main/ui.c) into docs/img/*.png for the manual.
# Needs cmake, a C compiler and one `idf.py build` (for managed_components/lvgl__lvgl).
# PNG conversion uses `sips` (macOS) or ImageMagick. Extra arguments go to cmake
# (e.g. -DLVGL=/path/to/lvgl-9.3).
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build="$repo/build/ui_shots"
out="$repo/docs/img"

cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
cmake --build "$build" -j >/dev/null
mkdir -p "$out"
"$build/ui_shots" "$build" >/dev/null
for bmp in "$build"/*.bmp; do
    png="$out/$(basename "$bmp" .bmp).png"
    if command -v sips >/dev/null; then
        sips -s format png "$bmp" --out "$png" >/dev/null
    else
        convert "$bmp" "$png"
    fi
done
ls "$out"
