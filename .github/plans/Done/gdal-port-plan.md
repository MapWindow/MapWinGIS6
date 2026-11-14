# Plan: GDAL Integration for MapWinGIS6

**Purpose:** Give Copilot a concrete, phase-by-phase plan to implement GDAL functionality in
[MapWinGIS6](https://github.com/MapWindow/MapWinGIS6) as a *modern rewrite* — not a
line-by-line port of [MapWinGIS5](https://github.com/MapWindow/MapWinGIS).

**Scope of this document:** How GDAL is initialized, wrapped, exposed, tested and documented in
MapWinGIS6. UI work (menus/dialogs) is out of scope except where noted.

---

## 1. Background & Goals

### 1.1 Where MapWinGIS5 stands

MapWinGIS5's `GdalUtils` class (see `src/COM classes/GdalUtils.cs` + native core) wraps the GDAL
**v2 "librified" C API** (`GDALTranslate()`, `GDALWarp()`, `GDALVectorTranslate()`,
`GDALBuildOverviews()`). It exposes them as string-array based methods:

- `GdalRasterTranslate(src, dst, string[] options)` → wraps `gdal_translate.exe`
- `GdalRasterWarp(src, dst, string[] options)` → wraps `gdalwarp.exe`
- `GdalVectorTranslate(src, dst, string[] options, useSharedConnection)` → wraps `ogr2ogr.exe`
- `GdalBuildOverviews(...)`, `ClipVectorWithVector(...)`, `GdalVectorReproject(...)`

Problems with the MapWinGIS5 approach, to be fixed in v6:

1. **Stringly-typed options** — no compile-time safety, hard to discover, hard to validate.
2. **Legacy tool mapping only** — only 4 of the ~40+ GDAL utilities are wrapped, and only via the
   old per-tool "librified" API.
3. **No access to modern GDAL capabilities**: the unified `gdal` CLI (RFC 104), algorithm
   registry, pipelines, cloud/virtual filesystems (`/vsicurl/`, `/vsiaz/`, `/vsis3/`), COG/Zarr
   drivers, multidimensional rasters.
4. **Weak progress/error reporting** — relies on `LastErrorCode`/`ErrorMsg` pattern; no structured
   error capture or cancellation.

### 1.2 Goals for MapWinGIS6

| # | Goal |
|---|------|
| G1 | Target **GDAL ≥ 3.11** (pin a specific version, e.g. 3.12.x — see §2.2). |
| G2 | Build GDAL access on the **`GDALAlgorithm` API** (RFC 104) — one generic runner for all current and future GDAL algorithms instead of N per-tool wrappers. |
| G3 | Provide a **type-safe option layer** (strongly-typed structs per algorithm) *on top of* the generic runner, so callers never hand-assemble strings. |
| G4 | Keep **backward compatibility**: keep a `GdalUtils`-style façade so existing MapWinGIS5 client code keeps working (deprecated but functional). |
| G5 | First-class **progress callback + cancellation + structured error** support on every call. |
| G6 | Support **in-memory (MEM/VRT) outputs** so results can feed directly into MapWinGIS6 layers without temp files. |
| G7 | Support **pipelines** (chained algorithms) where GDAL ≥ 3.11 provides them. |

---

## 2. Technical Decisions (decided up front — do not re-litigate in code)

### 2.1 Architecture: three layers

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 3: Public API (COM interface `IGdalUtils` / .NET façade)│
│   - Strongly-typed methods + enum-driven options              │
│   - Backward-compat MapWinGIS5 signatures (obsolete attr)     │
├─────────────────────────────────────────────────────────────┤
│ Layer 2: Option/algorithm layer (C++)                       │
│   - `GdalAlgorithmRunner`: generic GDALAlgorithm executor    │
│   - One typed options struct per algorithm (see §5)          │
├─────────────────────────────────────────────────────────────┤
│ Layer 1: GDAL core (external)                               │
│   - gdalalgorithm.h (GDALAlgorithm, GDALGlobalAlgorithmRegistry)│
│   - /vsi virtual filesystems, drivers, OGR                   │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 GDAL version & build

- **Pin GDAL 3.12+** for the C++ API. The C++ `GDALGlobalAlgorithmRegistry::GetSingleton()`
  entry point was added in **GDAL 3.12**; the C API (`GDALGetGlobalAlgorithmRegistry`) exists
  since 3.11. If v6 must ship on GDAL 3.11, use the C API and wrap handles in RAII C++ classes —
  decide once, document in `docs/gdal-version.md`.
- Build requirement: **C++17 compiler** (GDAL requirement since 3.9). MapWinGIS6 must compile
  with `/std:c++17` (MSVC) at minimum.
- Use **vcpkg** (`vcpkg install gdal`) or a pre-built GDAL binary distribution; do not vendor
  GDAL source into the repo. Record the exact version in `vcpkg.json` / CMake `find_package(GDAL)`.

### 2.3 API binding strategy

- New GDAL work lives in a **native C++ static lib** `MwGdalCore` (no COM inside).
- COM interop: expose `IGdalUtils` on the MapWinGIS6 OCX that forwards to `MwGdalCore`.
- All async/progress callbacks cross the boundary via function pointers (C-style), never
  C++ lambdas crossing COM boundaries.

---

## 3. Implementation Phases

> Each phase ends with **green tests + a PR**. Copilot should implement phases in order and stop
> after each PR for human review.

### Phase 0 — Foundation (GDAL lifecycle)

**Tasks:**
1. Add GDAL dependency to the build (CMake `find_package(GDAL REQUIRED)`, vcpkg manifest).
2. Create `MwGdalCore` project skeleton with:
   - `GdalInitializer` — RAII class: `GDALAllRegister()` on construct, `GDALDestroy()` on
     destruct. Never call `GDALAllRegister` more than once per process.
   - Global config-option helper: `SetConfigOption(name, value)` / scoped guard (save/restore).
3. **Error handling subsystem** (`GdalErrorScope`):
   - `CPLPushErrorHandler`/`CPLPopErrorHandler` based RAII catcher that collects
     `CPLErrorHandler`-delivered messages (class, code, message) into a `std::vector<GdalError>`.
   - Expose `LastErrorCode`, `LastErrorMsg`, `DetailedErrorMsg` + full error list to Layer 3.
4. **Progress subsystem**: `GdalProgressAdapter` that maps `GDALProgressFunc` to a
   `std::function<bool(double, const char*)>`; return `FALSE` from the C callback when the
   caller cancels (wire to a cancellation token).

**Acceptance criteria:**
- Unit test: init/shutdown twice safely; error handler captures a forced `CE_Failure`.
- Unit test: progress callback invoked 0→100 % during a slow translate; cancel aborts cleanly.

### Phase 1 — Generic Algorithm Runner (the heart of the rewrite)

**Task:** Implement `GdalAlgorithmRunner` (Layer 2) on top of `GDALAlgorithm`.

```cpp
// include/MwGdal/GdalAlgorithmRunner.h
class GdalAlgorithmRunner {
public:
    // Instantiate from a path like {"raster","convert"} or {"vector","reproject"}
    static GdalAlgorithmRunner Create(std::initializer_list<const char*> path);

    bool SetArg(const char* name, const char* value);
    bool SetArg(const char* name, bool value);
    bool SetArg(const char* name, int value);
    bool SetArg(const char* name, double value);
    bool SetArg(const char* name, const std::vector<double>& values);
    bool SetArg(const char* name, const std::vector<std::string>& values);

    // Legacy escape hatch: parse classic gdal_translate-style flags, e.g. "-tr","0.2","0.2"
    bool ParseCommandLineArgs(const std::vector<std::string>& args);

    bool Run(GdalProgress progress = {}, GdalCancelToken cancel = {});
    bool Finalize();                       // always call after successful Run()
    GDALDataset* TakeOutputDataset();      // for MEM/VRT outputs

    const std::vector<GdalError>& Errors() const;
    std::string UsageAsJson() const;       // forward GDALAlgorithm::GetUsageAsJSON()
};
```

Implementation notes for Copilot:
- Use `GDALGlobalAlgorithmRegistry::GetSingleton().Instantiate(path...)` (C++ API, GDAL 3.12+).
- For every `SetArg` call, check the boolean return and record `CPLGetLastErrorMsg()` on failure.
- `Run()` must be invoked **at most once per instance** (GDAL requirement) — enforce in code.
- Always call `Finalize()` after `Run()` and propagate its error status.

**Task:** COM façade `IGdalUtils2`:
```csharp
// Conceptual IDL/C# shape (adapt to actual MapWinGIS6 interop tech)
bool RunAlgorithm(string algorithmPath, string[] argNames, object[] argValues,
                  string[] openOptions, out string errorDetails);
```
This single method future-proofs MapWinGIS6: any algorithm GDAL adds in 3.13+ is usable
immediately without new wrapper code.

**Acceptance criteria:**
- Test: `Create({"raster","convert"})` translates a GeoTIFF to PNG (compare checksums).
- Test: `ParseCommandLineArgs` with MapWinGIS5-style options (`-ot Float32 -tr 0.2 0.2 -r average`)
  produces identical output to the typed-arg path.
- Test: unknown algorithm path and invalid arg names produce structured errors, not crashes.

### Phase 2 — Typed option structs (Layer 3 ergonomics)

**Task:** For the highest-value algorithms, define **strongly-typed option structs** with
defaults, validation, and `ToArgs()` conversion:

```cpp
struct RasterConvertOptions {           // gdal raster convert (was gdal_translate)
    std::optional<std::string> outputFormat;   // "GTiff", "COG", ...
    std::optional<std::string> dataType;       // "Byte", "Float32", ...
    std::vector<int> bands;
    std::optional<double> noDataValue;
    std::optional<RectD> projWin;              // georeferenced clip window
    std::optional<PixelWindow> srcWin;
    std::optional<double> outSizeX, outSizeY;  // or percent
    std::optional<double> resX, resY;          // -tr equivalent
    std::optional<std::string> resampling;
    std::vector<std::string> creationOptions;
    std::vector<std::string> openOptions;
    std::optional<int> overviewLevel;          // GDAL >= 3.6
    bool stats = false;
    // -> std::vector<std::string> ToArgs() const;  emits new-style "raster convert" flags
};
```

Priority algorithms to wrap typed-first (map new CLI name → old tool):

| New (GDAL ≥ 3.11 CLI) | Old tool | MapWinGIS5 equivalent |
|---|---|---|
| `raster convert` | `gdal_translate` | `GdalRasterTranslate` |
| `raster reproject` | `gdalwarp` | `GdalRasterWarp` |
| `vector convert` | `ogr2ogr` | `GdalVectorTranslate` |
| `raster overview add` | `gdaladdo` | `GdalBuildOverviews` |
| `raster info` | `gdalinfo` | — (new) |
| `vector info` | `ogrinfo` | — (new) |
| `raster mosaic` | `gdalbuildvrt` | — (new) |
| `vector clip` | `ogr2ogr -clipsrc/-clipdst` | `ClipVectorWithVector` |
| `raster clip` | `gdalwarp -cutline` | — (new) |
| `raster calc` (GDALCalc) | `gdal_calc.py` | — (new) |

**Acceptance criteria:** each typed struct round-trips through `ToArgs()` into the runner and is
validated against `UsageAsJson()` (fail fast on unknown/mutually exclusive args).

### Phase 3 — Backward compatibility façade

**Task:** Implement MapWinGIS5-compatible signatures on top of Phase 1–2, marked `[Obsolete]`:

```cpp
bool GdalRasterTranslate(const std::string& src, const std::string& dst,
                         const std::vector<std::string>& options);   // old flag strings
bool GdalRasterWarp(...); bool GdalVectorTranslate(...);
bool GdalBuildOverviews(...); bool ClipVectorWithVector(...);
bool GdalVectorReproject(src, dst, srcEpsg, dstEpsg);
```

- Internally route to the new algorithm runner (translate old flag names → new-style args, or
  just use `ParseCommandLineArgs` which accepts legacy flags).
- Map old error semantics (`LastErrorCode`, `ErrorMsg(code)`) onto the new error subsystem.
- Add a porting guide section: "From GdalUtils (v5) to GdalUtils2 (v6)" with before/after code.

**Acceptance criteria:** compile a small test that calls all five legacy methods using the
MapWinGIS5 doc examples (translate a TIF with `-ot Float32 -tr 0.2 0.2 -r average -projwin ...`;
reproject a shapefile EPSG:4326→28992; clip vector with vector).

### Phase 4 — Modern GDAL capabilities (the "new stuff")

4.1 **Cloud & virtual filesystems**
- Expose VSI paths transparently: pass `/vsicurl/https://...`, `/vsis3/bucket/key`,
  `/vsizip/...` directly as dataset names. Add helper `VsiPath` builder (bucket/prefix/credentials
  via config options, never hard-coded keys).
- Ensure `GdalInitializer` supports `CPLSetConfigOption("AWS_...", ...)` flows.

4.2 **Cloud-Optimized GeoTIFF (COG)**
- First-class save: default creation options `TILED=YES COMPRESS=DEFLATE` and expose
  `WriteCog()` convenience over `raster convert --of COG`.

4.3 **VRT / in-memory pipelines**
- `TakeOutputDataset()` + MEM output: `raster convert --output-format MEM` returns a
  `GDALDataset*` that MapWinGIS6 raster layers can adopt directly (zero temp files).
- `raster mosaic` to VRT for on-the-fly mosaicking of many tiles.

4.4 **Pipelines** (if targeting GDAL 3.11+ with `vector pipeline`/`raster pipeline` subcommands)
- Expose `RunPipeline(std::vector<PipelineStep>)` where each step is `{algorithmPath, args}`;
  bind step N output dataset object to step N+1 input via `GDALArgDatasetValue`.

4.5 **Multidimensional / Zarr** (stretch goal — flag-gated `#ifdef GDAL_VERSION_NUM >= ...`)
- Read-only support via `gdal mdim` algorithms; no UI work.

**Acceptance criteria:** tests read a COG from `/vsicurl/` (use a public test URL), run a
two-step pipeline in memory, and save a COG with overviews.

### Phase 5 — Error handling, logging, telemetry polish

- Every public method returns a rich result object: `{ bool Success; int LastErrorCode;
  std::string ErrorMsg; std::string DetailedErrorMsg; std::vector<GdalError> AllErrors; }`.
- Route GDAL warnings ( `CE_Warning` ) to MapWinGIS6's logging sink.
- Add cancellation-token plumbing through COM (async pattern: `BeginGdalOperation` /
  `EndGdalOperation`).

### Phase 6 — Tests, samples, docs

- **Test data**: commit 3 tiny rasters + 1 shapefile (< 1 MB total) to `testdata/`.
- **Unit tests** (Google Test or Catch2): every phase above.
- **Integration tests**: golden-file comparisons (checksums) against GDAL CLI output for the
  same operation — this catches argument-mapping drift.
- **Samples**: `samples/Cpp/translate.cpp`, `samples/CSharp/...` mirroring the MapWinGIS5
  documentation examples so users can migrate mechanically.
- **Docs**: `docs/gdal-integration.md` — version pin, architecture diagram, per-algorithm
  reference generated from `UsageAsJson()` where feasible, migration table v5→v6.

---

## 4. Key Implementation Details for Copilot

1. **Always pair `Run()` with `Finalize()`** — errors during dataset flush appear in
   `Finalize()`, not `Run()`. Both must succeed for the operation to be "successful".
2. **One `GDALAlgorithm` instance per execution.** Instantiate fresh for retries.
3. **Mutual exclusions** (e.g. `-outsize` vs `-tr`) are validated by `ValidateArguments()` —
   surface those messages verbatim to the caller; do not pre-validate by hand except where the
   typed structs make it free.
4. **SRS handling**: accept `EPSG:xxxx`, WKT, or PROJ strings. Where GDAL 3 semantics apply,
   axis order matters — document that MapWinGIS6 follows GDAL 3 axis-order rules
   (`OAMS_TRADITIONAL_GIS_ORDER` may be set globally if legacy behavior is required; decide in
   Phase 0 and document).
5. **Encoding**: paths may be UTF-8; use `CPLString`/`CPLUTF8` and avoid ANSI conversions.
6. **Threading**: GDAL operations run on a worker thread in the OCX; progress marshalled to the
   UI thread. Never call GDAL from multiple threads without holding the existing MapWinGIS
   global lock (match MapWinGIS5 behavior).

## 5. Suggested public surface (v6, native + COM)

```cpp
namespace mwgdal {
  // Phase 1
  class GdalAlgorithmRunner;              // generic, future-proof
  // Phase 2 (typed)
  bool RasterConvert(src, dst, const RasterConvertOptions&, Result&);
  bool RasterReproject(src, dst, const RasterReprojectOptions&, Result&);
  bool VectorConvert(src, dst, const VectorConvertOptions&, Result&);
  bool RasterOverviewAdd(src, const OverviewOptions&, Result&);
  bool RasterClip(src, dst, cutline, const RasterClipOptions&, Result&);
  bool VectorClip(subject, overlay, dst, const VectorClipOptions&, Result&);
  // Phase 3 (compat)
  [[deprecated]] bool GdalRasterTranslate(...);  // v5 signature
}
```

---

## 6. Risks & Open Questions

| Risk | Mitigation |
|---|---|
| GDALAlgorithm CLI named "experimental" in 3.11/3.12 | Keep the legacy per-tool C API (`gdal_utils.h`) behind an abstraction so we can swap implementations without touching Layer 3. |
| COM threading + GDAL not thread-safe in all drivers | Serialize all GDAL calls through a global mutex (as v5 does). |
| Axis-order behavior changes (GDAL 3) vs MapWinGIS5 expectations | Decide `OAMS_TRADITIONAL_GIS_ORDER` in Phase 0; add regression tests with known coordinates. |
| Driver availability in vcpkg GDAL build (e.g. ECW, MrSID) | Verify `gdalinfo --formats` in CI; document which drivers the shipped build includes. |

## 7. Milestones

| Milestone | Phases | Deliverable |
|---|---|---|
| M1 | 0–1 | Core builds, generic runner works, tests green |
| M2 | 2–3 | Typed APIs + v5 compat façade, migration doc |
| M3 | 4 | COG/VSI/VRT/pipeline support, samples |
| M4 | 5–6 | Polish, full test coverage, release docs |
