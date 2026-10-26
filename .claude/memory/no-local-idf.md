---
name: no-local-idf
description: Firmware builds only in GitHub Actions CI; no ESP-IDF on the dev Mac or in Claude cloud sessions
metadata:
  type: project
---

As of 2026-10-26 the dev Mac has no ESP-IDF. In Claude cloud sessions, installing ESP-IDF (`install.sh`) is refused by the permission policy and components.espressif.com / dl.espressif.com are blocked. The firmware is built by `.github/workflows/ci.yml` (espressif/esp-idf-ci-action, IDF v5.3.2) on every PR; read the job log via the GitHub tools to fix compile errors.

Off-device checks that work locally: host tests (`test/host`, gcc + ASan/UBSan/float-cast-overflow); FIT validation with fitdecode in a scratch venv; the stats app in headless Chrome.

**Why:** First successful firmware build was in CI (PR #1); IDF component versions float because there is no `dependencies.lock` yet.
**How to apply:** Push and read CI logs instead of trying to install IDF; ask the user to commit `dependencies.lock` from a local build.
