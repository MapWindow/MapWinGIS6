---
name: implementing-plan-steps
description: Implements the next unchecked step of a Markdown implementation plan in an isolated git worktree, reads relevant repository memory, adds tests, verifies the build, reviews memory for useful updates, updates the plan checklist, commits, and opens a pull request against the development branch. Use when the user asks to implement, continue, or work on the next step of a plan file.
argument-hint: Path to the plan file (e.g. _plans/My-Feature.md)
disable-model-invocation: true
---

# Implementing plan steps

Implements exactly one step of a plan per run and delivers it as one PR.

## Input

Required: path to the plan file.

If the user did not provide a plan path, stop and ask for it. Do not guess or search for one.

## Rules

- `AGENTS.md` is authoritative. Where it conflicts with this skill, `AGENTS.md` wins.
- Do only the selected step. No later steps. No unrelated refactoring.
- Prefer LSP/code-intelligence tools (symbols, references, definitions) over text search for navigating code.
- Follow `AGENTS.md` on selective compatibility. Document every intentional public API difference.
- If the plan seems wrong or a step is ambiguous: state it in one sentence, continue with the most reasonable reading. Ask only when readings lead to materially different work.
- Keep memory maintenance within the scope of the implementation. Do not invent knowledge just to make a memory change.
- Work only inside the worktree, except that an out-of-repository plan is updated at its supplied original path and remains uncommitted.

## Workflow

Copy this checklist and track progress:

```
- [ ] 1. Read AGENTS.md
- [ ] 2. Select the step
- [ ] 3. Create worktree
- [ ] 4. Read relevant project memory
- [ ] 5. Implement with tests
- [ ] 6. Build without new warnings and run tests
- [ ] 7. Review and maintain project memory
- [ ] 8. Update the plan
- [ ] 9. Commit
- [ ] 10. Push and open PR
```

**1. Read `AGENTS.md`** at the repository root. Apply its guidelines to every step below.

**2. Select the step.** Read the plan. Pick the first unchecked item in its checklist. If all items are checked, report that and stop.

Repository-local plans must already be committed on `development`, so the plan is present in the new worktree. If the supplied plan is untracked, uncommitted, or on another branch, treat it as an out-of-repository plan: read and update it at its supplied original path, and leave it uncommitted.

**3. Create worktree.**

```bash
git fetch origin
git worktree add -b <branch> .worktrees/<branch> origin/development
```

`<branch>`: short kebab-case name derived from the step. Work only inside the worktree from here on.

**4. Read relevant project memory.** Follow the memory workflow in `AGENTS.md#ai-memory-management`, starting with `.ai/memory/MEMORY.md`. Load only relevant category files, and verify important claims against current source and authoritative documentation.

**5. Implement with tests.** Follow conventions already used in the codebase. Add tests covering the new behavior. Run the tests; fix and re-run until they pass.

**6. Build without new warnings and run tests.** Follow the solution, configuration, and platform guidance in `AGENTS.md`. In a fresh worktree, bootstrap dependencies with `pwsh -File build\scripts\bootstrap-vcpkg.ps1`; reuse the configured vcpkg binary cache rather than rebuilding dependencies unnecessarily. Run `msbuild` from a Visual Studio Developer prompt. If it is not on `PATH`, locate it with `vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`; do not use a machine-specific path.

Compare warnings with the latest known `Debug | x64` baseline for the same toolchain. If no baseline is available, build `origin/development` once and record its warning count in the task notes or PR body; reuse that count rather than rebuilding the baseline on every run. Fix new warnings and rebuild. Run the relevant tests and all validation required by `AGENTS.md`.

**7. Review and maintain project memory** before committing, following `AGENTS.md#ai-memory-management`. Add only meaningful, verified knowledge; avoid duplicating authoritative documentation or recording temporary status. For substantial PRs, use the memory review checklist in `.ai/memory/README.md`.

**8. Update the plan**:
- Check off the implemented step.
- Add remaining limitations or follow-up items under that step so they stay visible.
- If the plan is repository-local and committed on `development`, update its copy in the worktree. Otherwise update the supplied original path and leave it uncommitted; do not copy it into the repository.

**9. Commit.** Before committing, check that `README.md` and the Markdown files in `docs/` are up to date for the selected step; update directly related documentation as needed. Make one commit containing code, tests, relevant documentation and memory changes, and the plan update if the plan is repository-local. Follow [`.github/commit-conventions.instructions.md`](../../commit-conventions.instructions.md), including its gitmoji, conventional type and scope, and description-list format. Example subject: `✨ feat(core): add memory store serialization`.

**10. Push and open PR** against `development`:
- Title: the plan title.
- Body: motivation, context, key design decisions, dependencies. Short. No filler or empty sections.
- Mention memory changes briefly if any were made.
- Require the `gh` CLI to be installed and authenticated. If it is unavailable or unauthenticated, stop and report the blocker rather than claiming the PR was opened.
- Write the PR body to a temporary file (for example, in `$env:TEMP`) and pass it with `--body-file`; do not leave it in the worktree.

```bash
git push -u origin <branch>
gh pr create --base development --title "<plan title>" --body-file <temporary-body-file>
```

If a build or test still fails after three focused fix-and-rerun attempts, stop and report the failure rather than looping indefinitely.

## Communication

- Before the first tool call: one sentence stating what you are about to do.
- During work: update only on important findings or direction changes.
- On finish: lead with the outcome — step implemented and PR URL. Then list:
  - Implementation summary.
  - Build and test results.
  - Memory files added or updated, with a short explanation. Explicitly say if no memory changes were needed.
  - Open issues or limitations.
