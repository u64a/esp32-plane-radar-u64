---
name: "git-workflow"
description: "Local-only Git workflow for ESP32 Plane Radar"
domain: "version-control"
confidence: "high"
source: "team-decision"
---

## Context

This repository is intentionally private and local-only. The imported source baseline is
commit `efd92a6`. There is no remote and no GitHub issue, pull-request, workflow, or
release automation.

## Rules

- Never add a remote, push, fork, publish, or create GitHub artifacts.
- Keep implementation changes in small local commits aligned with plan phases.
- Use isolated local worktrees only for genuinely independent parallel assignments.
- Before integrating a worktree, review its diff and run the smallest relevant tests.
- Never rewrite or discard user changes.
- Do not amend commits unless the user explicitly requests it.
- Include the configured Copilot trailers in commits created by the coordinator.

## Parallel Work

Create worktrees from the current local integration branch only after specialists agree
on interfaces and file ownership. Merge or cherry-pick reviewed commits locally, then
remove only the specific completed worktree.

## Anti-Patterns

- Adding `origin` or another remote
- Running `git pull`, `git push`, or `gh`
- Creating PR/issue branches for a repository with no GitHub workflow
- Combining unrelated plan phases into one large commit
- Deleting or resetting unrecognized working-tree changes
