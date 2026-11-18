---
name: finish-the-job
description: User expects fixes flashed, verified on the device, committed, pushed and PR'd without asking
metadata:
  type: feedback
---

When a fix is done, flash it (board on USB), verify on the device, then commit on a new branch from main, push and open a PR, and update docs (CLAUDE.md, HANDOVER.md, docs/manual.html, nas/README.md) and memory in the same go. Don't stop to ask "should I flash / commit?".

**Why:** 2026-11-18 the user was angry that a Wi-Fi fix was left unflashed and uncommitted ("it is your job to keep it running"; "why not committed?!").
**How to apply:** Still respect safety: never flash or open the console while a ride may be running; web-app deploys still need the user's tap. [[usb-console-resets]] [[operational-safety]]
