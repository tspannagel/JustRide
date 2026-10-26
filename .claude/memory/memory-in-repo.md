---
name: memory-in-repo
description: Project memory for JustRide is kept in .claude/memory/ inside the repo, not in the user-level memory dir
metadata:
  type: feedback
---

Store memories for this project in `.claude/memory/` (one fact per file, frontmatter, pointer line in `MEMORY.md`), not in `~/.claude/projects/.../memory/`.

**Why:** User asked on 2026-10-26 to "create and manage your own memory in this repository", so it's versioned with the code.
**How to apply:** Write new project facts here; `CLAUDE.md` imports `MEMORY.md` so it loads each session. Keep entries short and commit them with related changes. See [[framework-choice]], [[gitignore-conventions]].
