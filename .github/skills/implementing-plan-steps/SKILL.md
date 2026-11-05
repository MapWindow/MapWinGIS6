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
- Optimize structure first, backward compatibility second. Do not let compatibility constrain the new design.
- If the plan seems wrong or a step is ambiguous: state it in one sentence, continue with the most reasonable reading. Ask only when readings lead to materially different work.
- Keep memory maintenance within the scope of the implementation. Do not invent knowledge just to make a memory change.

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

**3. Create worktree.**

```bash
git fetch origin
git worktree add -b <branch> ../<repo>-<branch> origin/development
```

`<branch>`: short kebab-case name derived from the step. Work only inside the worktree from here on.

**4. Read relevant project memory** before making architectural or implementation decisions:
- Read `.ai/memory/MEMORY.md` first.
- Identify and read only the memory files relevant to the selected step. Use category READMEs to find relevant knowledge where necessary; do not read the entire memory directory by default.
- Treat memory as reusable project knowledge, not as a substitute for examining current source code and authoritative documentation.
- Verify important claims against the current implementation, build configuration, tests, or authoritative documentation. Investigate and resolve outdated or contradictory memory before relying on it.

**5. Implement with tests.** Follow conventions already used in the codebase. Add tests covering the new behavior. Run the tests; fix and re-run until they pass.

**6. Build without new warnings and run tests.** Follow the solution, configuration, and platform guidance in `AGENTS.md`. Use `D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` on this machine. Inspect this installation before reporting a missing toolset; do not silently substitute another installation or build system.

Compare warnings against a build of `origin/development`. Fix any new warnings, then rebuild. Run the relevant tests and confirm they pass, along with any additional validation required by `AGENTS.md`.

**7. Review and maintain project memory** before committing:
- Identify reusable knowledge, technical limitations, architectural decisions, non-obvious requirements, or lessons revealed by the implementation.
- Check whether that knowledge is already documented. Add or update memory only when there is meaningful, verified knowledge worth retaining, in the appropriate category under `.ai/memory/`.
- Update `.ai/memory/MEMORY.md` and the relevant category README if files or topics were added, renamed, or removed.
- Correct or remove existing entries that became demonstrably outdated because of this implementation.
- Verify every new statement against source code, documentation, tests, or successful experiments. Distinguish established facts from assumptions or proposals, and include useful references to source files, documentation, or PRs.
- Do not duplicate authoritative documentation, record temporary implementation status, add speculative conclusions, or store credentials, secrets, or personal information.
- For substantial PRs, use the memory review checklist in `.ai/memory/README.md`.
- If there is no meaningful new knowledge and existing memory remains correct, leave memory files unchanged.

**8. Update the plan**:
- Check off the implemented step.
- Add remaining limitations or follow-up items under that step so they stay visible.
- For repository-local plans, update the worktree copy. If the supplied plan is outside the repository, update it at its supplied path; do not copy it into the repository just to commit it.

**9. Commit.** Before committing, check that `README.md` and the Markdown files in `docs/` are up to date for the selected step; update directly related documentation as needed. Make one commit containing code, tests, relevant documentation and memory changes, and the plan update if the plan is repository-local. Use an imperative message describing the step (e.g. `Add memory store serialization`).

**10. Push and open PR** against `development`:
- Title: the plan title.
- Body: motivation, context, key design decisions, dependencies. Short. No filler or empty sections.
- Mention memory changes briefly if any were made.

```bash
git push -u origin <branch>
gh pr create --base development --title "<plan title>" --body-file <body.md>
```

## Communication

- Before the first tool call: one sentence stating what you are about to do.
- During work: update only on important findings or direction changes.
- On finish: lead with the outcome — step implemented and PR URL. Then list:
  - Implementation summary.
  - Build and test results.
  - Memory files added or updated, with a short explanation. Explicitly say if no memory changes were needed.
  - Open issues or limitations.
