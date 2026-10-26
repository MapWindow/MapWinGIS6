# AGENTS.md

Instructions for AI coding agents (GitHub Copilot, Claude Code, etc.) working
in this repository.

## Contents

- [Mission](#mission)
- [Repository layout](#repository-layout)
- [Build, test, validate](#build-test-validate)
- [Ground rules](#ground-rules)
- [Detailed guidance](#detailed-guidance)

## Mission

MapWinGIS6 re-implements MapWinGIS — a 20-year-old open-source MFC/ATL
ActiveX GIS control — as modern C++:

- **`MapWinGIS.Core.dll`** — the mapping engine as a plain C++ DLL. No COM,
  ATL or MFC. Usable and testable on its own.
- **`MapWinGIS.ocx`** — a thin ActiveX/COM wrapper over the core. The initial
  vertical slice supports COM activation, control lifecycle, vector loading, and
  basic polygon rendering; broader MapWinGIS 5 API compatibility remains future
  work.

[MapWinGIS 5](https://github.com/MapWindow/MapWinGIS) is the reference
implementation and the behavioral ground truth. Port its functionality; do
not transliterate its code.

## Repository layout

| Path | Contents |
|---|---|
| `src/MapWinGIS.Core/` | Core engine. Public headers in `include/mapwingis/`, implementation in `src/`, grouped by area (`core`, `gdal`, `vector`, ...) |
| `tests/MapWinGIS.Core.Tests/` | GoogleTest unit tests; shared fixtures in `TestData.h/.cpp` |
| `build/` | Shared MSBuild props, vcpkg overlay triplets, bootstrap scripts |
| `docs/` | Detailed documentation (see table below) |

## Build, test, validate

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug\x64\MapWinGIS.Core.Tests.exe
```

Every task must leave the tree in this state: `Debug | x64` builds with no
new Level 4 warnings and all tests pass. Full build details, vcpkg features
and switches: [docs/BUILDING.md](docs/BUILDING.md).

## Ground rules

1. **Modernize, don't transliterate.** MapWinGIS 5 code is a behavior
   reference. Rewrite it as clean C++20 in the style already in this tree.
2. **Core stays pure.** No COM, ATL or MFC in `MapWinGIS.Core`; COM concerns
   belong to the future ActiveX layer.
3. **Match existing conventions.** SPDX header, Doxygen file block, `mwg`
   namespace, PImpl classes, `[[nodiscard]]`, exceptions with `TryX`
   companions. Full list: [docs/CODING-STANDARDS.md](docs/CODING-STANDARDS.md).
4. **Preserve the MapWinGIS 5 contract.** 0-based feature indexes, enum
   values pinned to the v5 type library, error semantics.
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
