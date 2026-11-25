# Project memory (Claude-managed)

Claude keeps durable, non-obvious facts about this project here, one fact per file. This index is imported by `CLAUDE.md`.
Don't duplicate what `CLAUDE.md`, `HANDOVER.md`, the code or git history already record. Update or delete stale entries.

- [Memory lives in the repo](memory-in-repo.md) — store project memory here, not in ~/.claude
- [Framework: ESP-IDF](framework-choice.md) — confirmed ESP-IDF >= 5.3, no Arduino
- [Keep it simple](keep-it-simple.md) — no overengineering, only scaffold what's used now
- [Operational safety](operational-safety.md) — review every change; hardware must not be damaged
- [ESP-IDF on the Mac](no-local-idf.md) — local `idf.py build` works since 2026-10-27; cloud sessions use CI
- [gitignore conventions](gitignore-conventions.md) — sdkconfig ignored, stats vendor/ generated (not committed)
- [USB console resets the board](usb-console-resets.md) — opening the serial port reboots it; keep one session open, never mid-ride
- [Finish the job](finish-the-job.md) — flash, verify, commit, push, PR and docs without asking
- [Bluetooth works (baseline)](bluetooth-working-config.md) — 3 sensors + watch connect since MAX_ACT=10; timing changes need a log
