---
name: operational-safety
description: User requires every change to be reviewed for operational safety; hardware must not be damaged by the code
metadata:
  type: feedback
---

User (2026-10-26): "review for operational safety. hardware must not suffer defects from this code" and "never forget: keep it simple".

**Why:** Real trainer, real rider, a board known to be fragile (fast chargers burn it, SW6106 power chip quirks).
**How to apply:** Check each change against the "Operational safety" section in CLAUDE.md (unconfirmed chip registers, shared GPIOs, flash wear, trainer limits, OTA). Put new limits in pure code with host tests. Say explicitly in the hand-off what was reviewed. See [[keep-it-simple]].
