# Modern GDAL capabilities

GDAL paths are passed through the typed options and algorithm runner as UTF-8.
This includes GDAL virtual filesystem paths such as `/vsicurl/`, `/vsis3/`,
`/vsizip/`, `/vsiaz/`, and `/vsimem/`. Availability depends on the GDAL build,
drivers, and access configuration. GDAL reports an operational error when a
path or driver is unavailable.

## Cloud credentials and configuration

Use GDAL configuration options or the credential mechanisms supported by the
selected cloud provider. `GdalConfig::SetOption()` changes process-wide
configuration; `ScopedGdalConfigOption` restores an option when its scope ends.
These options are not isolated from concurrent changes to the same setting.
Do not put credentials in paths, source code, tests, or logs. Network tests use
public data and are skipped unless `MWG_RUN_NETWORK_GDAL_TESTS=1` is set.

## COG creation

`WriteCog()` uses GDAL's `raster convert` algorithm and the COG output driver.
It forwards creation options to GDAL and leaves unspecified defaults unchanged.
With GDAL 3.12.4, the COG driver's default block size is 512 pixels, compression
is LZW, and `OVERVIEWS=AUTO` uses existing source overviews or generates them
when needed. The driver and build determine support for individual creation
options.

## Datasets and pipelines

`GdalDatasetPtr` owns datasets returned by algorithms. A pipeline passes an
output dataset to the next algorithm as a referenced GDAL dataset object; it
does not create intermediate files when steps use in-memory output formats.
For example, set `output-format` to `MEM` to use GDAL's in-memory raster driver.
`VRT` output can be used with `raster mosaic` for a virtual mosaic. A VRT may
reference its input datasets, so those sources must remain available while the
VRT is used.

Each pipeline step after the first receives the previous step's dataset as
`input`. It must omit an explicit `input` argument and must produce an output
dataset. On success, the result retains intermediate datasets so outputs such
as VRTs that depend on earlier datasets remain valid. Keep the result alive
while using its output dataset. On failure, intermediate datasets are released.

## Multidimensional data

GDAL's registered `mdim` algorithms and multidimensional drivers are available
through `GdalAlgorithmRunner` only when included in the GDAL build. Zarr driver
availability is build-dependent; this phase does not add a separate
multidimensional API.

## Operation results, logging, and cancellation

`GdalOperationResult` contains the status, diagnostics, and returned datasets.
`WriteCog()` and `RunPipeline()` return this result directly.
`GdalAlgorithmRunner::Result()` and `GdalUtils::Result()` return snapshots without datasets.
`TakeOutputDataset()` remains the runner's ownership-transfer method.
Snapshots keep their diagnostics when a later compatibility operation changes the object's state.

`Succeeded()` is true only after execution and finalization succeed.
`LastErrorCode()` and `ErrorMsg()` select the last failure or fatal diagnostic.
They return zero and an empty string when the diagnostics contain only warnings.
`DetailedErrorMsg()` includes every diagnostic with its severity and GDAL code.
`AllErrors()` exposes the full diagnostic list.
An unsuccessful GDAL call that emits only warnings also receives a failure diagnostic.

`WasCancelled()` distinguishes cancellation from other failures.
A cancelled token or a progress callback that returns false cancels the operation.
A callback exception produces a failure diagnostic instead.
Cancellation does not guarantee removal of partial output files.
GDAL algorithms that do not report progress cannot respond promptly to cancellation during execution.

`GdalErrorScope` forwards captured warnings to the handler installed through `GdalEnvironment::SetLogHandler()`.
It also forwards other diagnostic levels and retains the diagnostics in the operation result.
Logger exceptions remain inside the error scope until GDAL returns.
They propagate to the caller through `RethrowCaptureException()`.
The runner still finalizes a successful GDAL execution before it reports a callback or logger failure.
For direct GDAL calls outside an error scope, the default handler retains logger exceptions on the calling thread.
Call `GdalEnvironment::RethrowLogException()` after those calls to report and clear the first exception.
Neither handler lets an exception cross the native GDAL callback boundary.

## Execution and thread ownership

The native runner, compatibility utilities, and convenience operations share one recursive mutex, a lock that permits internal nested calls.
A pipeline holds the lock across all steps.
Independent operations can run on different threads, but their GDAL calls run one at a time.
Their diagnostic lists remain separate.
This lock does not cover direct GDAL calls, returned datasets, or the vector dataset API.
Callers must still serialize access to shared datasets and process-wide configuration.

Progress and logging callbacks run synchronously on the thread that executes the operation.
Callbacks must not start another GDAL operation or wait for a thread that needs the execution lock.
The native operation APIs reject same-thread callback re-entry with `CoreError`.
A callback must remain valid until the operation returns.
Removing a logger does not stop a callback that already holds a copy of that logger.
Use owned state or shared ownership for captures that other threads can access.

The merged ActiveX work defines architecture, not a control implementation.
It introduces no COM interfaces, callback dispatcher, or asynchronous operation model.
The completed GDAL phases therefore retain their native API and dataset ownership contracts.
`BeginGdalOperation` and `EndGdalOperation` are not added.
A future ActiveX wrapper must dispatch worker progress to its owning COM/UI thread without blocking the worker on another GDAL operation.
