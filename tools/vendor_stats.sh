#!/bin/sh
# Fetch the pinned DuckDB-WASM and uPlot into www/stats/vendor/ (run once, needs Node).
# duckdb-browser.mjs imports "apache-arrow", so it's bundled with esbuild into one file.
set -eu
DUCKDB=1.32.0
UPLOT=1.6.32
ESBUILD=0.25.0

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/www/stats/vendor"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
npm init -y >/dev/null
npm install --silent --no-audit --no-fund "@duckdb/duckdb-wasm@$DUCKDB" "uplot@$UPLOT" "esbuild@$ESBUILD"

D="$OUT/duckdb-$DUCKDB"
U="$OUT/uplot-$UPLOT"
rm -rf "$OUT"
mkdir -p "$D" "$U"
echo 'export * from "@duckdb/duckdb-wasm";' > entry.mjs
npx esbuild entry.mjs --bundle --format=esm --minify --log-level=warning --outfile="$D/duckdb.mjs"
cp node_modules/@duckdb/duckdb-wasm/dist/duckdb-eh.wasm "$D/"
cp node_modules/@duckdb/duckdb-wasm/dist/duckdb-browser-eh.worker.js "$D/"
cp node_modules/uplot/dist/uPlot.esm.js node_modules/uplot/dist/uPlot.min.css "$U/"
echo "vendored into $OUT:"
ls -la "$D" "$U"
