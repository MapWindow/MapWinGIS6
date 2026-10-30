# GDAL integration

MapWinGIS6 provides a native C++20 interface to GDAL through
`MapWinGIS.Core.dll`, which configures GDAL's runtime data paths, implements
GDAL algorithm execution, and exposes the public headers under
`mapwingis/gdal/`. The Core DLL does not use COM, ATL, or MFC.

## Requirements and runtime setup

MapWinGIS6 requires GDAL 3.12.4 or later. The vcpkg manifest pins the version
used by this repository. GDAL headers and libraries come from vcpkg; do not
install another GDAL SDK beside the application. See [GDAL versioning](gdal-version.md)
and [Building MapWinGIS6](BUILDING.md) for dependency and build instructions.

`GdalEnvironment::Initialize()` registers drivers once and points GDAL and PROJ
to the runtime data deployed beside `MapWinGIS.Core.dll`. Call it before direct
GDAL use; the MapWinGIS GDAL APIs initialize the environment as needed. The
driver set depends on the vcpkg build features. Check availability at runtime
with `GdalEnvironment::HasRasterDriver()` or `HasVectorDriver()`. An absent
driver or unsupported algorithm produces GDAL diagnostics; it is not silently
substituted.

The DLL API exchanges C++ standard-library types across the DLL boundary.
Consumers must use the same MSVC toolset and CRT as the Core DLL.

## Generic algorithms and typed options

Use `GdalAlgorithmRunner` when you need an algorithm supported by the
installed GDAL build. Its path is the GDAL algorithm name, and
`UsageAsJson()` exposes the installed algorithm's own argument metadata:

```cpp
mwg::GdalEnvironment::Initialize();
mwg::GdalAlgorithmRunner runner("raster convert");
const std::string metadata = runner.UsageAsJson();
```

Pass arguments with the typed `SetArgument()` overloads or
`ParseCommandLineArgs()`, then call `Run()`. The runner validates arguments
against GDAL's metadata and captures warnings and failures in `Errors()`.
`Result()` returns a snapshot without transferring output datasets;
`TakeOutputDataset()` transfers ownership of an output dataset.

For common operations, use a typed options structure. This example converts
an input raster to GeoTIFF and reports captured diagnostics on failure:

```cpp
mwg::RasterConvertOptions options;
options.input = inputPath;
options.output = outputPath;
options.outputFormat = "GTiff";
options.creationOptions = {"COMPRESS=LZW"};

mwg::GdalAlgorithmRunner runner("raster convert");
if (!mwg::ConfigureGdalAlgorithm(runner, options.ToArgs()) || !runner.Run())
{
    const auto result = runner.Result();
    std::cerr << result.DetailedErrorMsg() << '\n';
}
```

The typed structures cover raster conversion, clipping, reprojection,
mosaicking, calculation and overviews, plus vector conversion and clipping.
They translate values to GDAL arguments; GDAL remains responsible for
algorithm-specific validation, driver support and defaults. To see the current
option list for an installed algorithm, inspect `UsageAsJson()` instead of
copying an option list into application documentation.

## Progress and cancellation

Progress callbacks execute synchronously on the thread running GDAL. Return
`false` to cancel. A cancellation token can also be cancelled from another
thread and must remain alive until the operation returns:

```cpp
mwg::GdalCancellationToken cancellation;
const auto progress = [](double fraction, std::string_view message)
{
    std::cout << fraction << ": " << message << '\n';
    return true;
};

const bool succeeded = runner.Run(progress, &cancellation);
```

Use the token with `WriteCog()` or `RunPipeline()` in the same way. Not every
GDAL algorithm reports progress, so cancellation may not interrupt those
algorithms promptly. Cancellation can leave partial output files. Callbacks
must not start another GDAL operation or wait on a thread that needs the GDAL
execution lock; callback exceptions are returned as operation failures.

## ActiveX operations

The ActiveX control exposes asynchronous generic GDAL execution through its
separate `IMapWinGISControlGdal` interface. `BeginGdalOperation` accepts a GDAL
algorithm path and an array of command-line argument tokens; use the installed
algorithm's `UsageAsJson()` or GDAL documentation for valid arguments. Text is
converted between BSTR and UTF-8 at the COM boundary.

Call the interface on its owning COM apartment, which must continue pumping
messages while operations are active. A message-only dispatcher delivers
`GdalProgress` and `GdalOperationCompleted` events on that thread. Progress is
coalesced per operation;
the terminal event is retained. The wrapper permits sixteen outstanding
operations. Call `EndGdalOperation` after completion to retrieve success,
cancellation, GDAL error code, summary, full diagnostics, callback error code
and message, and warnings, and to release the operation slot. An incomplete
operation returns `E_PENDING`; an unknown or ended identifier returns
`E_INVALIDARG`.
`EndGdalOperation` also returns `E_PENDING` while a progress or completion
event is being delivered. Call it after the event handler returns.

`CancelGdalOperation` is non-blocking. Repeated cancellation returns `S_FALSE`;
an unknown identifier returns `E_INVALIDARG`. Cancellation remains distinct
from GDAL failures and event-handler failures. Closing the control requests
cancellation and discards pending events. Drivers without progress callbacks
may not stop promptly. The COM API does not expose native dataset handles or
in-memory outputs; use a GDAL output path when a client needs a persistent
result. For a managed client, see
[`GdalOperationClient.cs`](../samples/CSharp/ActiveXTestHost/GdalOperationClient.cs).

## Errors and logging

Runner and convenience-operation results own their diagnostic lists.
`Succeeded()` is true only if GDAL execution and finalization succeed.
`LastErrorCode()` and `ErrorMsg()` select the last failure or fatal diagnostic;
warnings alone do not make a successful operation fail. Use
`DetailedErrorMsg()` or `AllErrors()` when the full diagnostic context matters.

Install an optional process-wide handler with
`GdalEnvironment::SetLogHandler()`. Diagnostics are delivered on the thread
that raises them. Handlers must not re-enter GDAL or wait for GDAL work on
another thread. The callback boundary does not permit exceptions to cross into
GDAL; captured logger exceptions are rethrown by the operation after GDAL
returns.

## In-memory datasets and pipelines

`RunPipeline()` passes each step's output dataset directly to the next step.
Later steps omit the `input` argument. Keep the returned result alive while
using its output dataset; it retains intermediate datasets required by VRT
outputs. Choose output formats such as `MEM` for in-memory raster processing
or `VRT` for virtual datasets when the algorithm supports them:

```cpp
mwg::RasterConvertOptions options;
options.input = inputPath;
options.outputFormat = "MEM";

mwg::GdalPipelineStep step{
    {"raster", "convert"},
    options.ToArgs()
};
const auto result = mwg::RunPipeline({step});
```

`WriteCog()` is the convenience operation for Cloud-Optimized GeoTIFF output.
The COG driver and the GDAL build determine which creation options are
available. VRT files can refer to source datasets; keep their sources
available for as long as the VRT needs them.

## Cloud and virtual filesystem paths

GDAL virtual filesystem paths such as `/vsicurl/`, `/vsis3/`, `/vsizip/`,
`/vsiaz/`, and `/vsimem/` are accepted as UTF-8 paths when the selected driver
supports them. Cloud access depends on the GDAL build, network access and
provider configuration. Use GDAL's supported credential mechanisms or
configuration options. `GdalConfig::SetOption()` changes process-wide
configuration; `ScopedGdalConfigOption` restores the previous value at scope
exit but does not isolate concurrent changes to the same option. Never put
credentials in paths, source code, tests or logs.

## GDAL 3 axis order

GDAL 3 honors the axis order defined by a coordinate reference system's
authority when using OGR spatial-reference and coordinate-transformation APIs.
For example, EPSG:4326 is latitude then longitude according to its authority
definition. MapWinGIS6 does not install a global axis-order override; GDAL
algorithms apply their own documented behavior.

For direct OGR transformations that must use traditional GIS X/Y order, set
the strategy on the spatial reference objects before creating the
transformation:

```cpp
OGRSpatialReference srs;
srs.importFromEPSG(4326);
srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
```

Apply the same intended convention to both source and target spatial
references. This is an explicit per-object choice, not a process-wide
compatibility setting. Check the selected algorithm's usage metadata for
algorithm-specific CRS options.

## Migration and examples

MapWinGIS5's COM `IGdalUtils` is not binary-compatible with the new generic v6
interface. The old IID and operation-specific signatures are not reused.
Use the native runner and typed options for new C++ code. A deprecated native
`GdalUtils` bridge remains available for gradual native migration. See
[GDAL migration](gdal-migration.md) for operation examples and API differences.

The solution builds the runnable C++ example
[`samples/Cpp/translate.cpp`](../samples/Cpp/translate.cpp). Run it with
`bin\Debug\x64\translate.exe <input-raster> <output-raster>`.

## Limitations and troubleshooting

- Driver and algorithm availability varies with the vcpkg feature set. Check
  `HasRasterDriver()`, `HasVectorDriver()`, and `UsageAsJson()` at runtime.
- The runner serializes its GDAL algorithm calls, but returned datasets and
  direct GDAL calls are not covered by that lock. Synchronize access to shared
  datasets and process-wide configuration yourself.
- Native Core objects are not thread-safe unless their API says otherwise.
  Progress and logging callbacks run synchronously on the operation thread.
- If initialization reports missing GDAL or PROJ data, rebuild with runtime
  data deployment enabled. The data directories are placed beside the DLL;
  do not set `GDAL_DATA` or `PROJ_LIB` as a workaround.
- If a cloud path fails, confirm the driver, protocol support, credentials,
  network access and GDAL configuration for the installed build.
- If a conversion fails, inspect all captured diagnostics; an installed GDAL
  build may not include the requested output driver or algorithm.

The automated tests create small raster and vector fixtures under temporary
directories and remove them after each test. They do not depend on network
data unless `MWG_RUN_NETWORK_GDAL_TESTS=1` is set for the opt-in public COG
test.
