---
name: gitignore-conventions
description: Non-obvious choices in .gitignore: sdkconfig ignored, stats vendor/ generated not committed, sample FIT files kept
metadata:
  type: project
---

- `sdkconfig` is ignored; persistent config goes in `sdkconfig.defaults` (and `sdkconfig.defaults.esp32s3`). `dependencies.lock` stays committed, `managed_components/` is ignored.
- `www/stats/vendor/` is ignored: `tools/vendor_stats.sh` rebuilds it from pinned versions (DuckDB-WASM 1.32.0 bundled with esbuild, uPlot 1.6.32). Still "vendored" in the sense that matters: served from the SD card, never a CDN.
- `*.fit` is ignored except under `tools/` and `test/host/fixtures/` — real rides are personal data.
- `.claude/settings.local.json` is ignored; `.claude/memory/` is tracked ([[memory-in-repo]]).

**Why:** Decided 2026-10-26 when the .gitignore was created.
**How to apply:** If someone wants a committed full `sdkconfig` instead, remove that line and update this entry.
