# Project memory (Claude-managed)

Claude keeps durable, non-obvious facts about this project here, one fact per file. This index is imported by `CLAUDE.md`.
Don't duplicate what `CLAUDE.md`, `HANDOVER.md`, the code or git history already record. Update or delete stale entries.

- [Memory lives in the repo](memory-in-repo.md) — store project memory here, not in ~/.claude
- [Framework: ESP-IDF](framework-choice.md) — confirmed ESP-IDF >= 5.3, no Arduino
- [Keep it simple](keep-it-simple.md) — no overengineering, only scaffold what's used now
- [Operational safety](operational-safety.md) — review every change; hardware must not be damaged
- [Firmware builds in CI only](no-local-idf.md) — no local or cloud ESP-IDF; use the CI firmware job
- [gitignore conventions](gitignore-conventions.md) — sdkconfig ignored, stats vendor/ generated (not committed)
