---
name: stacked-prs
description: Stacked PRs: GitHub retargets the next PR to main only if the merged branch is deleted; check the base before merging
metadata:
  type: project
---

On 2026-11-25 PR #13 (based on #12's branch) was merged after #12 had gone into main, but the branch wasn't deleted, so #13 landed in `fix/plan-sync-ownership`, not main. v1.0.0-beta.3 was tagged on that merge, so main lacked the release's changes until PR #14.

**Why:** GitHub only moves a PR onto main when its base branch is deleted on merge (repo setting "Automatically delete head branches").
**How to apply:** Prefer basing new PRs on main. When stacking is unavoidable, tell the user to retarget the next PR to main (`gh pr edit N --base main`) before merging it, and check `gh pr view N --json baseRefName` first. Before tagging, check the commit is on main (`git merge-base --is-ancestor <sha> origin/main`).
