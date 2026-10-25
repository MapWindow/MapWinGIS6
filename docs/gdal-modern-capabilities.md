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
dataset. Intermediate datasets are released when the pipeline ends or fails.

## Multidimensional data

GDAL's registered `mdim` algorithms and multidimensional drivers are available
through `GdalAlgorithmRunner` only when included in the GDAL build. Zarr driver
availability is build-dependent; this phase does not add a separate
multidimensional API.
