# MapWinGIS6 AI Memory

This index is the entry point to shared, version-controlled project knowledge.
Memory complements `AGENTS.md` and the authoritative documentation; it does not
replace them.

Read this index at the start of a task, then load only the category files that
can inform that task. Verify important assumptions against the current
repository before relying on them. Memory management rules are in
[`AGENTS.md`](../../AGENTS.md#ai-memory-management).

## Categories

- [Architecture](architecture/README.md) — verified component boundaries,
  relationships, lifecycle details, and architectural constraints.
- [Build](build/README.md) — non-obvious build requirements, dependency
  configuration, and verified build problem solutions.
- [Testing](testing/README.md) — test setup, fixtures, test-specific
  requirements, and verified testing pitfalls.
- [Decisions](decisions/README.md) — established design choices, their
  rationale, alternatives, and consequences.
- [Lessons](lessons/README.md) — reusable outcomes and limitations verified
  during development.
- [Tools](tools/README.md) — non-obvious tool setup, usage, and limitations.

## Selecting relevant knowledge

Use the task's affected area and activity to choose categories. For example,
consult build and testing knowledge for build or test work, architecture and
decisions for design work, and tools for tool-specific changes. Read an
individual knowledge file only when its subject is relevant; do not load every
file by default.

## Keeping this index current

List each category's purpose here. When knowledge files are added, renamed, or
removed, update the corresponding category README and these links or
descriptions so they stay accurate. Keep this index concise and link to details
instead of copying them here.
