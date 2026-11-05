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
  relationships, lifecycle details, and architectural constraints:
  [overview](architecture/overview.md),
  [ActiveX](architecture/activex.md),
  [Core library](architecture/core-library.md).
- [Build](build/README.md) — non-obvious build requirements, dependency
  configuration, and verified build problem solutions:
  [Windows](build/windows.md), [CMake status](build/cmake.md),
  [dependencies](build/dependencies.md).
- [Testing](testing/README.md) — test setup, fixtures, test-specific
  requirements, and verified testing pitfalls:
  [testing](testing/testing.md).
- [Decisions](decisions/README.md) — established design choices, their
  rationale, alternatives, and consequences:
  [modern architecture](decisions/2026-11-05-modernize-before-porting.md),
  [selective compatibility](decisions/2026-11-05-selective-compatibility.md),
  [Core and ActiveX boundary](decisions/2026-11-05-core-activex-boundary.md).
- [Lessons](lessons/README.md) — reusable outcomes and limitations verified
  during development: [lessons](lessons/lessons.md).
- [Tools](tools/README.md) — non-obvious tool setup, usage, and limitations.
  There is no additional tool-specific knowledge to record yet.

## Selecting relevant knowledge

Use the task's affected area and activity to choose files. Consult architecture
and decisions for design work; build for configuration or dependency changes;
testing for test setup and fixtures; and lessons when working on related
failure modes. Read tool knowledge only when such a file exists and applies.
Do not load every file by default.

## Keeping this index current

List each category's purpose here. When knowledge files are added, renamed, or
removed, update the corresponding category README and these links or
descriptions so they stay accurate. Keep this index concise and link to details
instead of copying them here.
