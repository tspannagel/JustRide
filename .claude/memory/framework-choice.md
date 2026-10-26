---
name: framework-choice
description: ESP-IDF vs Arduino is still unconfirmed; user referred to the project as "ESP32/Arduino"
metadata:
  type: project
---

`CLAUDE.md` and the plan assume ESP-IDF 5.x (C11), but HANDOVER.md open question 2 is unresolved. On 2026-10-26 the user described the project as "ESP32/ARDUINO" when asking for a .gitignore, which may hint at Arduino (or Arduino-as-component / PlatformIO).

**Why:** Scaffolding, build system and component layout differ a lot between the two.
**How to apply:** Ask the user explicitly before scaffolding any firmware; once answered, update this entry, `CLAUDE.md` and HANDOVER.md. The .gitignore covers both ([[gitignore-conventions]]).
