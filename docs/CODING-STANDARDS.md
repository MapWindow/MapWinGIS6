# Coding standards

The existing sources are the normative examples. This file names the rules so
new code is indistinguishable from existing code.

## File layout and headers

- Every source file starts with `// SPDX-License-Identifier: MPL-2.0`
  followed by a Doxygen file block:

  ```cpp
  /**
   * @file VectorDataset.h
   * @brief Declares the vector dataset and layer APIs.
   *
   * (Optional extra paragraphs with behavioral notes.)
   */
  ```

- Use `#pragma once`; no include guards.
- One class (or a small group of closely related types) per header, with a
  matching `.cpp`.
- Public headers include only standard-library and other public MapWinGIS
  headers. Third-party headers (GDAL/OGR, Windows internals) belong in the
  `.cpp` or in `src/internal/`.

## Language and toolchain

- C++20 (`stdcpp20`, C17), conformance mode, `/utf-8`, `/Zc:__cplusplus`,
  `/Zc:preprocessor`, `/Zc:throwingNew`.
- Warning level 4 with zero new warnings; SDL checks on. Do not silence
  warnings with pragmas in source — fix the code.

## Formatting

- 4-space indentation, no tabs; Allman braces; `case` labels indented under
  `switch`.
- `namespace mwg { ... }` with contents indented one level.
- Naming: `PascalCase` public methods and types, `_camelCase` private members
  (`_impl`, `_dataset`), `kPascalCase` compile-time constants
  (`SampleLayer::kFeatureCount`).

## API shape

- All public API is in `namespace mwg` and exported with `MWG_CORE_API`.
- PImpl: declare `class Impl;` privately, construct via
  `explicit X(std::unique_ptr<Impl>)`, store as `std::unique_ptr<Impl> _impl`.
  Delete copy constructor and assignment; add move semantics only when there
  is a real need.
- Mark every non-void query `[[nodiscard]]`.
- Return `std::optional` for possibly-absent values, `std::unique_ptr` for
  owning factory results; take paths as `std::filesystem::path` and read-only
  strings as `std::string_view`.
- No output parameters, no raw owning pointers, no `new`/`delete` outside of
  PImpl construction.
- Document on the declaration: behavior, thread-safety, performance
  characteristics, and exceptions (`\throws`) — in the style of the existing
  headers. State "not thread-safe" explicitly whenever it is true.

## Types

- `enum class` with an explicit underlying type
  (`enum class ShapeType : int`).
- When an enum mirrors a MapWinGIS 5 type-library enum, the numeric values
  are pinned and must never be renumbered — the ActiveX layer converts with a
  `static_cast`. Note the pinning in the doc comment.
- Small type-safe helpers belong next to the enum
  (`constexpr ShapeType BaseShapeType(ShapeType) noexcept`).

## Errors and validation

- Throw `CoreError` subclasses (`mapwingis/core/Error.h`) for operational
  failure. Messages are plain English and suitable for surfacing through the
  ActiveX layer.
- Validate indexes and arguments at the API boundary and throw on violation.
- Never `catch (...)` silently, never show UI, never log to `std::cout` from
  the core. Report through `GdalEnvironment` diagnostics or throw.

## Windows and dependencies

- The environment defines `NOMINMAX`, `WIN32_LEAN_AND_MEAN` and
  `_WIN32_WINNT=0x0A00` globally. Write code that works with them; do not
  `#undef` or redefine them.
- The `min`/`max` macros are off: use `std::min`/`std::max`.
- All third-party dependencies come from the vcpkg manifest (`vcpkg.json`).
  Never vendor libraries, download SDKs by hand, or add `support/` folders.
- The product is Unicode (`CharacterSet=Unicode`); use wide APIs at the
  boundary and convert explicitly.
