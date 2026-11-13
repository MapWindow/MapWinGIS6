# AGENTS.md

Instructions for AI coding agents (GitHub Copilot, Claude Code, etc.) working
in this repository.

## Contents

- [Mission](#mission)
- [Repository layout](#repository-layout)
- [Build, test, validate](#build-test-validate)
- [Ground rules](#ground-rules)
- [Detailed guidance](#detailed-guidance)
- [Agent skills](#agent-skills)
- [AI Memory Management](#ai-memory-management)

## Mission

MapWinGIS6 re-implements MapWinGIS — a 20-year-old open-source MFC/ATL
ActiveX GIS control — as modern C++:

- **`MapWinGIS.Core.dll`** — the mapping engine as a plain C++ DLL. No COM,
  ATL or MFC. Usable and testable on its own.
- **`MapWinGIS.ocx`** — a thin ActiveX/COM wrapper over the core. The initial
  vertical slice supports COM activation, control lifecycle, vector loading, and
  basic polygon rendering; broader MapWinGIS 5 API compatibility remains future
  work.

[MapWinGIS 5](https://github.com/MapWindow/MapWinGIS) is a reference for
functionality and established behavior, not a constraint on the new architecture.
Optimize the structure and API for a maintainable modern version first; preserve
backward compatibility second, where it fits that design without imposing undue
complexity or limiting improvements. Do not transliterate the legacy code.

## Repository layout

| Path | Contents |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | Primary native CMake build and reusable modules |
| `src/MapWinGIS.Core/` | Core engine. Public headers in `include/mapwingis/`, implementation in `src/`, grouped by area (`core`, `gdal`, `vector`, ...) |
| `tests/MapWinGIS.Core.Tests/` | Core GoogleTest unit tests and shared fixtures in `TestData.h/.cpp` |
| `tests/MapWinGIS.ActiveX.Tests/` | ActiveX implementation and COM-hosting GoogleTest unit tests |
| `build/` | Shared MSBuild props, vcpkg overlay triplets, bootstrap scripts |
| `docs/` | Detailed documentation (see table below) |

## Build, test, validate

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Until CMake registers the ActiveX tests, also build the legacy solution and run
its ActiveX suite:

```powershell
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug\x64\MapWinGIS.ActiveX.Tests.exe
```

Every task must leave the tree in this state: CMake `Debug | x64` builds with
no new Level 4 warnings and all non-interactive tests pass. The legacy
`.slnx`/MSBuild build remains available during the transition; use it when
changing or validating that build integration. Full build details, CLion and
Visual Studio setup, vcpkg features, and switches:
[docs/BUILDING.md](docs/BUILDING.md).

## Ground rules

1. **Modernize, don't transliterate.** MapWinGIS 5 code is a behavior
   reference. Rewrite it as clean C++20 in the style already in this tree.
2. **Core stays pure.** No COM, ATL or MFC in `MapWinGIS.Core`; COM concerns
   belong to the ActiveX layer.
3. **Modernize the structure first.** Prefer a clean, maintainable C++20 design
   over matching the legacy architecture or signatures. Compatibility is a
   secondary goal and must not burden the new design.
4. **Preserve compatibility selectively and deliberately.** Keep established
   behavior when it is valuable and fits the design; document intentional
   differences. Treat 0-based indexes, legacy enum values, and error semantics
   as compatibility requirements only where the relevant API contract needs them.
   See [docs/PORTING.md](docs/PORTING.md).
5. **Tests with every feature.** GoogleTest with generated fixtures via
   `TestData.h`. Conventions: [docs/TESTING.md](docs/TESTING.md).

## Detailed guidance

| Topic | File |
|---|---|
| Build details, vcpkg, switches, troubleshooting | [docs/BUILDING.md](docs/BUILDING.md) |
| Architecture, DLL design, GDAL lifecycle, threading | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| Coding style and API conventions | [docs/CODING-STANDARDS.md](docs/CODING-STANDARDS.md) |
| Porting behavior from MapWinGIS 5 | [docs/PORTING.md](docs/PORTING.md) |
| Writing and running tests | [docs/TESTING.md](docs/TESTING.md) |

## Agent skills

The task-specific files under `.github/skills/` are GitHub Copilot skills, such
as `implementing-plan-steps` and `resolving-pr-comments`. They supplement this
file for Copilot clients that support repository skills. `AGENTS.md` remains
the shared guidance; do not assume other agents discover `.github/skills/`
without explicit configuration.

## AI Memory Management

The shared, provider-independent project memory is in `.ai/memory/`. It
complements this file and the authoritative project documentation; it does not
replace them.

### At the beginning of a session

1. Read `AGENTS.md`.
2. Read `.ai/memory/MEMORY.md`.
3. Select only the memory files relevant to the task.
4. Read those files before making architectural or implementation decisions.
5. Verify important assumptions against the current repository.

### During development

Identify knowledge that could help future work, such as a verified technical
limitation, a solution to a difficult build problem, an architectural decision,
a tooling requirement, or a lesson from a failed implementation. Do not record
every small implementation detail.

### At the end of a task

1. Review whether reusable knowledge was discovered and check whether it already
   exists.
2. Verify it against source code, documentation, successful experiments, or
   established architectural decisions.
3. Update the appropriate memory file and update `MEMORY.md` when files are
   added, renamed, or removed.
4. Include relevant memory changes in the implementation PR.

For substantial PRs, use the
[memory review checklist](.ai/memory/README.md#review-checklist).

### Memory quality

- Never store guesses as established facts. Distinguish verified facts from
  assumptions and proposals.
- Include references to relevant source files, documentation, or PRs where
  useful.
- Prefer concise explanations over long transcripts, and avoid duplicating
  authoritative documentation.
- Remove obsolete information when it is no longer relevant.
- Never store credentials, secrets, access tokens, or personal information.
- Do not record temporary implementation status as permanent knowledge.
- Do not turn an unsuccessful experiment into an architectural rule.

Memory is maintained through agent instructions and normal pull requests. Do
not add agent-specific memory systems, hooks, automatic memory injection, or
background processes.
