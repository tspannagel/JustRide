---
name: keep-it-simple
description: User wants simple, non-overengineered code; only scaffold what is used now
metadata:
  type: feedback
---

User asked (2026-10-26): "keep a proper scaffolding. dont overengineer. keep it simple."

**Why:** Hobby project, user new to the board; less code is easier to follow and debug on hardware.
**How to apply:** Create components from the CLAUDE.md layout only when a milestone needs them (no empty placeholder dirs). Prefer one plain file over abstraction layers; tiny `test/host/test.h` macros instead of a test framework. See [[framework-choice]].
