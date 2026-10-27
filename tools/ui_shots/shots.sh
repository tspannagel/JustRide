#!/bin/sh
# Renders the device screens (main/ui.c) into docs/img/*.png for the manual.
# Needs cmake, a C compiler and one `idf.py build` (for managed_components/lvgl__lvgl).
# PNG conversion uses `sips` (macOS).
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build="$repo/build/ui_shots"
out="$repo/docs/img"

cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j >/dev/null
mkdir -p "$out"
"$build/ui_shots" "$build" >/dev/null
for bmp in "$build"/*.bmp; do
    sips -s format png "$bmp" --out "$out/$(basename "$bmp" .bmp).png" >/dev/null
done
ls "$out"
