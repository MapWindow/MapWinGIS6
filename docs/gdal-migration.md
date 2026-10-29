# GDAL migration

## From GdalUtils (v5) to the v6 GDAL APIs

MapWinGIS 5 exposed GDAL operations through the COM `GdalUtils` object.
MapWinGIS6 exposes asynchronous generic GDAL execution through the ActiveX
wrapper's `IMapWinGISControlGdal` interface; this is not a binary-compatible
`GdalUtils` or `GdalUtils2` interface. The native replacement is
`GdalAlgorithmRunner` combined with the typed option structures in
`mapwingis/gdal/GdalTypedOptions.h`.

### Raster conversion

In v5, callers passed GDAL command-line options through a COM `SAFEARRAY`:

```csharp
var succeeded = gdalUtils.GdalRasterTranslate(
    inputPath, outputPath, options);
```

For format conversion in v6, configure the typed API and run it explicitly:

```cpp
mwg::RasterConvertOptions options;
options.input = inputPath;
options.output = outputPath;
options.outputFormat = "GTiff";
options.creationOptions = {"COMPRESS=LZW"};

mwg::GdalAlgorithmRunner runner("raster convert");
if (!mwg::ConfigureGdalAlgorithm(runner, options.ToArgs()) || !runner.Run())
{
    // Inspect runner.Errors() for GDAL diagnostics.
}
```

The typed `raster convert` algorithm does not provide every option accepted by
the v5 `GdalRasterTranslate` method. For data type, resolution, resampling, or
projection-window options, the deprecated native compatibility method accepts
the same GDAL utility flags:

```cpp
mwg::GdalUtils legacy;
const bool succeeded = legacy.GdalRasterTranslate(
    inputPath, outputPath,
    {"-ot", "Float32", "-tr", "10", "10", "-r", "bilinear",
     "-projwin", "xmin", "ymax", "xmax", "ymin"});
```

Use this bridge only while migrating; the method is marked `[[deprecated]]`.

### Vector conversion and clipping

The v5 COM method took paths, option arrays, and an optional shared-connection
flag. In native v6, paths are `std::filesystem::path`, options are UTF-8
strings, and shared connections are not used. New code uses typed options:

```cpp
mwg::VectorClipOptions options;
options.input = subjectPath;
options.output = outputPath;
options.like = overlayPath;
options.overwrite = true;

mwg::GdalAlgorithmRunner runner("vector clip");
if (!mwg::ConfigureGdalAlgorithm(runner, options.ToArgs()) || !runner.Run())
{
    // Inspect runner.Errors() for GDAL diagnostics.
}
```

The compatibility `GdalUtils::GdalVectorTranslate`,
`GdalUtils::GdalVectorReproject`, `GdalUtils::GdalBuildOverviews`, and
`GdalUtils::ClipVectorWithVector` methods preserve the v5 operation names for
native callers. Their boolean result and `LastErrorCode()`, `ErrorMsg(code)`,
and `DetailedErrorMsg()` properties provide operation status. Error codes are
GDAL diagnostic codes, not MapWinGIS 5 type-library error numbers. The shared
connection flags are accepted but ignored: each operation owns its GDAL
dataset handles.

The compatibility methods are native C++ APIs, not binary-compatible COM
interfaces. The ActiveX wrapper translates its generic COM arguments and
results to the core API; it does not expose these operation-specific methods.
