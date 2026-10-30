---
name: no-local-idf
description: ESP-IDF v5.3.2 is installed on the dev Mac (~/esp/esp-idf) since 2026-10-27; cloud sessions still can't install it
metadata:
  type: project
---

Since 2026-10-27 the dev Mac has ESP-IDF v5.3.2 in `~/esp/esp-idf`. Build locally with `. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build` (run in the repo; `| grep -E "error:|warning:|Project build complete"` keeps the output short). CI (`.github/workflows/ci.yml`) builds every PR too. In Claude cloud sessions installing ESP-IDF is refused and Espressif's servers are blocked: use the CI log there.

Off-device checks: host tests (`test/host`, ASan/UBSan); FIT validation with fitdecode in a scratch venv; the stats app in headless Chrome (puppeteer-core + local Chrome).

**Why:** The first build happened only in CI; the user then installed IDF locally. ESP-IDF warnings are errors here (-Werror=format-truncation bit twice: bound `%s` with `%.Ns`).
**How to apply:** Always run a local `idf.py build` before committing firmware changes; flash with `idf.py -p /dev/cu.usbmodem2101 flash` when the user asks (board on USB since 2026-10-28).
