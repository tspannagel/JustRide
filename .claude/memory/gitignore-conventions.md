---
name: gitignore-conventions
description: Non-obvious choices in .gitignore: sdkconfig ignored, vendored stats assets, BLE fixtures and sample FIT files kept
metadata:
  type: project
---

- `sdkconfig` is ignored; persistent config goes in `sdkconfig.defaults` (and `sdkconfig.defaults.esp32s3`). `dependencies.lock` stays committed, `managed_components/` is ignored.
- `www/stats` vendored DuckDB-WASM/uPlot must stay tracked (app works offline); only `node_modules/` and `www/stats/dist/` are ignored.
- `*.fit` is ignored except under `tools/` and `test/host/fixtures/` — real rides are personal data.
- `.claude/settings.local.json` is ignored; `.claude/memory/` is tracked ([[memory-in-repo]]).

**Why:** Decided 2026-10-26 when the .gitignore was created.
**How to apply:** If someone wants a committed full `sdkconfig` instead, remove that line and update this entry.
