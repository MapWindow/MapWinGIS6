# MapWinGIS Development Guidelines

## Scope and authoritative guidance

Follow [AGENTS.md](../AGENTS.md) and the detailed standards linked there.
They take precedence over the general guidance below.

`MapWinGIS.Core` is a C++20 DLL. It must not use COM, ATL, or MFC.
The ActiveX wrapper has an initial implementation. COM/ActiveX guidance below
applies to the implemented subset and any future compatibility work.

## Project Context

This repository contains the modernized MapWinGIS6 codebase.

MapWinGIS 5 is the legacy COM/ActiveX library and the behavior reference.
Port its functionality incrementally as modern C++20. Do not transliterate
the legacy implementation.

## Build and test

Run these commands from the repository root:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
bin\Debug\x64\MapWinGIS.Core.Tests.exe
```

Run one GoogleTest case by adding its suite and test name:

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe --gtest_filter=VectorDatasetTest.ReportsTheDriverAndPath
```

Write a JUnit-style test report when needed:

```powershell
bin\Debug\x64\MapWinGIS.Core.Tests.exe --gtest_output=xml:testresults.xml
```

The test project uses GoogleTest. Shared generated data and temporary-directory
helpers are in `tests/MapWinGIS.Core.Tests/TestData.h`. Keep fixtures in a
temporary directory and do not commit generated shapefiles or databases.

## Architecture across files

`MapWinGIS.Core.dll` contains the public native API, application logic, and
GDAL execution support. Public headers live under
`src/MapWinGIS.Core/include/mapwingis/<area>/`.
Implementations mirror those areas under `src/MapWinGIS.Core/src/<area>/`.
Put private shared code under `src/MapWinGIS.Core/src/internal/`.

Call `GdalEnvironment::Initialize()` before normal GDAL use. The build deploys
GDAL and PROJ data beside the DLL, and `GdalEnvironment` configures their paths.
Do not depend on `GDAL_DATA` or `PROJ_LIB` environment variables. Core objects
are not thread-safe unless their API documents otherwise.

Use `CoreError` subclasses for operational failures. Use `TryX` companions
when failure is a normal probe result. Use `std::optional` for absent values,
not for operational failures.

Use MapWinGIS 5 to understand established behavior, but optimize the v6
structure and APIs first. Backward compatibility is desirable only where it
fits the new design without undue complexity or limiting meaningful improvements.
Document intentional behavior/API differences and their rationale. Keep
compatibility at the appropriate boundary rather than importing legacy
architecture into the core.

When working in this repository, prioritize:

* Clear, maintainable, performant modern C++ and a sound architecture.
* Correctness and tests for the intended v6 behavior.
* Selective, explicitly considered backward compatibility.
* Small, focused changes with evidence for the chosen design.
* Understanding related code before changing it.
* Avoiding unrelated changes outside the requested scope.

Do not redesign unrelated functionality, but do not preserve a poor design solely for compatibility.

---

## General Coding Guidelines

Before changing code:

1. Understand the responsibility of the relevant class, component, or subsystem.
2. Inspect related declarations, implementations, and call sites when necessary.
3. Consider whether the code is part of the public COM/ActiveX API.
4. Preserve existing behavior when it fits the v6 design and requirements; do not treat legacy behavior as immutable when modernization justifies a deliberate, documented change.
5. Identify compatibility implications before changing public APIs.

When making changes:

* Keep changes focused on the requested task.
* Prefer simple, readable solutions over unnecessary abstraction.
* Follow the existing architecture unless there is a clear reason to change it.
* Do not introduce dependencies without a clear need.
* Do not perform unrelated refactoring.
* Do not silently change public API behavior.
* Do not remove functionality merely because it appears old or unusual.
* Do not make assumptions about code that are not supported by the source or project context.

When a requirement is ambiguous, inspect the surrounding code and project conventions before making assumptions.

---

# Documentation Guidelines

## File-level documentation

Every new or substantially modified C++ source or header file should have a concise Doxygen-style file header.

Preferred format:

```cpp
/**
 * @file Map.cpp
 *
 * @brief Implements the Map class.
 *
 * Provides functionality for managing map layers and interacting
 * with the map rendering system.
 */
```

The exact wording must reflect the actual contents of the file.

Do not blindly copy a generic description or infer functionality solely from the filename.

### Header files

For `.h` and `.hpp` files, describe:

* The main classes, interfaces, structs, enums, or other public types.
* The responsibility of the API declared in the file.
* Important relationships or constraints when relevant.

### Implementation files

For `.cpp`, `.cc`, and `.cxx` files, describe:

* The functionality implemented by the file.
* The main class or component being implemented.
* Important implementation responsibilities when they provide useful context.

### Length

Keep file-level documentation concise.

As a general guideline, 3–10 lines is sufficient for most files.

A more complex file may need additional context, but do not turn every file into a documentation page.

Do not document every function in the file from the file header.

---

## Doxygen

Use Doxygen-compatible syntax where appropriate.

For file-level documentation, prefer:

```cpp
/**
 * @file FileName.cpp
 *
 * @brief Short description of the file.
 */
```

Use additional Doxygen tags when they provide meaningful documentation.

Do not add tags merely for completeness.

In particular, do not automatically add:

* `@author`
* `@date`
* `@version`
* `@history`

unless explicitly requested or required by an established project convention.

---

## Public API documentation

Public APIs should be documented when their purpose or behavior is not obvious.

For public classes, methods, properties, interfaces, and other API elements, documentation should explain meaningful behavior and contracts rather than simply repeating the name.

Useful information can include:

* What the API does.
* Important parameters and return values.
* Preconditions or constraints.
* Ownership and lifetime requirements.
* Exceptions or error behavior.
* Threading requirements.
* COM/ActiveX compatibility considerations.
* Non-obvious behavior that callers need to understand.

Avoid documentation such as:

```cpp
// Gets the name.
std::string GetName();
```

when the declaration is already self-explanatory.

Prefer documentation that adds information:

```cpp
/**
 * @brief Gets the name of the layer.
 *
 * Returns the name currently exposed through the MapWinGIS API.
 * The returned value may be empty if no name has been assigned.
 */
```

Only document behavior that is supported by the code or established project requirements.

---

# Comments in Implementations

Comments should explain information that cannot be understood easily from the code itself.

Prefer comments that explain:

* Why code is implemented in a particular way.
* Compatibility requirements.
* Non-obvious algorithms.
* Workarounds.
* Architectural constraints.
* Important assumptions.
* Reasons for seemingly unusual behavior.

Avoid comments that merely restate the code.

Prefer:

```cpp
// Preserve the legacy behavior because existing COM clients
// may depend on this coordinate transformation.
```

over:

```cpp
// Transform the coordinates.
```

Do not add comments simply to increase the amount of documentation.

Do not invent explanations for code whose intent is unclear.

If the reason for unusual code cannot be established reliably, do not fabricate one.

---

# Change History

Do not add manual change logs to source files.

Do not add:

* `Revision History`
* `Change History`
* `Created by`
* `Author`
* `Last modified`
* Creation dates
* Modification dates
* Date-based change entries

For example, do not add:

```text
Revision History:
2020-05-12 John - Added layer support
2024-03-18 Jane - Fixed rendering
2026-10-01 Paul - Modernized implementation
```

Git provides this information more reliably through commit history and blame.

The purpose of source documentation is to explain the code's purpose, behavior, and design — not to maintain a manually updated history.

---

# Historical and Migrated Code

The new repository is being populated by moving functionality from the legacy MapWinGIS codebase.

When migrating code:

* Preserve useful technical comments from the original implementation when they remain relevant.
* Preserve comments explaining compatibility requirements or non-obvious behavior.
* Do not blindly copy obsolete or misleading comments.
* Do not introduce historical changelists into the new code.
* Do not assume that old comments are correct simply because they exist.
* Update documentation when the migrated implementation has materially changed.

The new repository should document the current implementation rather than reproduce the documentation style of the legacy repository.

---

# COM / ActiveX Compatibility

MapWinGIS exposes a long-lived COM/ActiveX API.

When modifying code that is part of the public API:

* Consider compatibility, but do not let it dictate the internal architecture.
* Avoid accidental public-interface changes; deliberate design improvements may justify a documented API difference.
* Be careful with COM-visible types, interfaces, methods, properties, enums, DISPIDs, and related contracts.
* Consider existing external applications that may depend on legacy behavior.
* Document compatibility constraints when they are important to understanding the implementation.

Do not claim that a behavior is required for backwards compatibility unless that requirement is known or can reasonably be established from the code or project context.

---

# Modern C++ Guidelines

Modernize code deliberately rather than mechanically.

Use C++20 and follow the architecture and conventions in
[docs/CODING-STANDARDS.md](../docs/CODING-STANDARDS.md).

Examples include:

* RAII.
* Appropriate smart pointers.
* Scoped enums.
* `constexpr` where appropriate.
* `const` correctness.
* Range-based loops where they improve clarity.
* Standard library facilities instead of unnecessary custom implementations.
* Clear ownership and lifetime semantics.
* Avoiding raw owning pointers.

However:

* Do not modernize code merely for stylistic reasons during an unrelated task.
* Do not introduce abstractions without a clear benefit.
* Do not change behavior unintentionally.
* Consider COM/ActiveX ownership and lifetime semantics carefully before replacing existing pointer-based patterns.
* Consider ABI/API compatibility, while prioritizing a sound v6 design over legacy parity when the two conflict.

When migrating a legacy component, preserve externally observable behavior when it fits the v6 design. If compatibility would impose undue complexity or block a worthwhile improvement, prefer the better design and document the difference.

---

# Error Handling

Follow the error-handling conventions established by the surrounding code and API.

For COM-facing code, pay particular attention to existing error reporting and HRESULT-related behavior.

Do not replace an established error-handling mechanism with exceptions or another mechanism without considering its effect on callers and the public API.

Do not silently swallow errors.

When changing error handling, make the behavioral implications explicit.

---

# Dependencies

Prefer existing project dependencies and the C++ standard library.

Do not add a third-party dependency for functionality that can reasonably be implemented using existing project facilities or the standard library.

If a new dependency is genuinely necessary:

* Keep the change focused.
* Explain why it is needed.
* Consider build, deployment, licensing, and compatibility implications.

---

# Generated and Third-party Code

Do not modify generated code, build output, or third-party/vendor code unless explicitly requested.

Before editing an unfamiliar file, determine whether it is:

* Project source code.
* Generated code.
* Third-party code.
* Build output.
* Configuration or tooling.

When uncertain, inspect the project structure and related configuration rather than guessing.

---

# Scope and Unrelated Changes

Keep changes focused.

Unless explicitly requested, do not:

* Refactor unrelated code.
* Rename unrelated symbols.
* Reformat unrelated files.
* Change line endings unnecessarily.
* Reorder includes without a reason.
* Change build configuration.
* Update dependencies.
* Fix unrelated bugs.
* Modify generated files.
* Rewrite existing documentation unrelated to the task.

A clean, focused diff is preferred.

---

# Before Making Changes

For non-trivial changes:

1. Identify the relevant files.
2. Understand the existing implementation.
3. Identify related declarations and callers where necessary.
4. Determine whether the code is part of the public COM/ActiveX API.
5. Consider compatibility implications.
6. Make the smallest appropriate change.
7. Review the resulting diff for unintended modifications.

Do not make large speculative changes when a smaller change satisfies the requirement.

---

# After Making Changes

Review the resulting changes and verify:

* The requested functionality is implemented correctly.
* Existing behavior is preserved where required.
* Public API compatibility has been considered.
* Documentation accurately describes the current code.
* No invented behavior or assumptions were introduced.
* No unrelated files were modified.
* No unnecessary formatting changes were introduced.
* New comments provide useful information rather than restating code.
* File-level documentation follows the project's Doxygen conventions.

When possible, build and test the affected code.

If tests cannot be run, state that clearly rather than implying that the changes were verified.

---

# Communication

When explaining changes:

* Be concise and technical.
* Clearly distinguish facts from assumptions.
* Mention important compatibility considerations.
* Identify uncertainty rather than guessing.
* Report relevant files and changes.
* Mention tests/builds that were actually performed.
* Do not claim success for operations that were not verified.

For larger tasks, summarize:

1. What was changed.
2. Why it was changed.
3. Which files were affected.
4. What was tested.
5. Any remaining concerns or follow-up work.
