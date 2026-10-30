// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalTypedOptions.h
 * @brief Declares typed options for common GDAL algorithms.
 *
 * The option structures convert native values to GDAL algorithm arguments.
 * GDAL remains responsible for validating supported arguments and constraints.
 */
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "mapwingis/core/Api.h"
#include "mapwingis/core/Error.h"

namespace mwg
{
    class GdalAlgorithmRunner;

    /**
     * @defgroup GdalTypedOptions Typed GDAL algorithm options
     * @brief Value types that copy native values into GDAL algorithm arguments.
     *
     * Each ToArgs() returns an independent, owning argument list. Paths become
     * UTF-8 strings. Conversion does not initialize GDAL, open datasets, or
     * validate required values, supported formats, or argument combinations.
     * Required paths are emitted even when empty.
     *
     * Unset optional fields and empty non-optional vectors are omitted.
     * On a new runner, omitted arguments retain the instantiated GDAL
     * algorithm's defaults. An optional bool set to false is emitted explicitly.
     * Other present optional values, including empty strings, are emitted.
     * RasterReprojectOptions also omits empty inputNoData and outputNoData lists.
     * GDAL defines format inference and all other algorithm-specific defaults.
     * Numeric arrays retain their values for GDAL validation. Mosaic resolution
     * becomes a string and requires finite values. NoData values can be NaN.
     *
     * Pass the result to ConfigureGdalAlgorithm() with the matching algorithm.
     * Configuration can open input datasets. Run() performs execution separately.
     * Configuration and execution failures return false with diagnostics in
     * GdalAlgorithmRunner::Errors(). ToArgs() does not validate GDAL algorithm
     * constraints.
     *
     * @par Thread safety and cost
     * Concurrent ToArgs() calls are safe if no thread modifies the source
     * struct or its members. Mutation of a shared struct is not thread-safe
     * without external synchronization. Conversion copies the supplied values,
     * with time and memory proportional to their total size.
     *
     * @par Exceptions from ToArgs()
     * Exceptions from allocation and filesystem path conversion propagate to
     * the caller. No GDAL diagnostic is recorded for these exceptions.
     * @throws std::bad_alloc If memory allocation fails.
     * @throws InvalidArgument If a RasterMosaicOptions resolution value is not finite.
     * @throws CoreError If numeric formatting fails.
     */

    using GdalAlgorithmArgumentValue =
        std::variant<bool, int, double, std::string, std::vector<int>,
                     std::vector<double>, std::vector<std::string>>;

    /// One named, strongly typed GDAL algorithm argument.
    struct MWG_CORE_API GdalAlgorithmArgument
    {
        std::string name;
        GdalAlgorithmArgumentValue value;
    };

    using GdalAlgorithmArguments = std::vector<GdalAlgorithmArgument>;

    /**
     * @brief Sets typed arguments and validates them with the instantiated GDAL algorithm.
     *
     * Use a new runner for the matching algorithm. Arguments are applied in
     * order, without rollback on failure. Validation can open input datasets.
     * Returns false for rejected arguments or validation failure, with
     * diagnostics in runner.Errors(). Does not execute the algorithm.
     *
     * Not thread-safe for a shared runner. The argument list is read-only.
     * Allocation and diagnostic-capture exceptions propagate to the caller.
     */
    [[nodiscard]] MWG_CORE_API bool ConfigureGdalAlgorithm(
        GdalAlgorithmRunner& runner, const GdalAlgorithmArguments& arguments);

    /// Options for GDAL raster format conversion.
    ///
    /// Requires input and output dataset paths. Unset fields follow the
    /// @ref GdalTypedOptions "shared conversion contract".
    /// GDAL 3.12's raster convert algorithm
    /// does not expose raster translate's window, band, or data-type options.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterConvertOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::optional<std::string> outputFormat;
        std::optional<bool> overwrite;
        std::optional<bool> append;
        std::vector<std::string> inputFormats;
        std::vector<std::string> creationOptions;
        std::vector<std::string> openOptions;

        /// Copies values for `raster convert`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for reprojecting a raster dataset.
    ///
    /// Requires input and output dataset paths. sourceCrs overrides the input
    /// coordinate reference system (CRS). Set targetCrs to request a different CRS.
    /// An unset sourceCrs uses the input dataset's CRS.
    /// GDAL determines omitted output bounds and dimensions from the input.
    /// resolution is {x, y} in target CRS units. size is {width, height} in pixels.
    /// boundingBox is {xmin, ymin, xmax, ymax}, in target CRS units unless
    /// boundingBoxCrs is set.
    /// Mutually exclusive choices, such as target resolution and target size,
    /// are validated by the instantiated GDAL algorithm.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterReprojectOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::optional<std::string> outputFormat;
        std::optional<std::string> sourceCrs;
        std::optional<std::string> targetCrs;
        std::optional<bool> overwrite;
        std::optional<bool> append;
        std::optional<std::array<double, 2>> resolution;
        std::optional<std::array<int, 2>> size;
        std::optional<std::array<double, 4>> boundingBox;
        std::optional<std::string> boundingBoxCrs;
        std::optional<std::string> resampling;
        std::optional<std::vector<double>> inputNoData;
        std::optional<std::vector<double>> outputNoData;
        std::optional<bool> targetAlignedPixels;
        std::vector<std::string> inputFormats;
        std::vector<std::string> creationOptions;
        std::vector<std::string> openOptions;

        /// Copies values for `raster reproject`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        /// @throws CoreError if numeric formatting fails.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for converting vector datasets and layers.
    ///
    /// Requires input and output dataset paths. Empty inputLayers leaves layer
    /// selection to GDAL. Set outputLayer to override the output layer name.
    /// Dataset and layer creation options depend on the selected output driver.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API VectorConvertOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::optional<std::string> outputFormat;
        std::vector<std::string> inputLayers;
        std::optional<std::string> outputLayer;
        std::optional<bool> overwrite;
        std::optional<bool> update;
        std::optional<bool> overwriteLayer;
        std::optional<bool> append;
        std::optional<bool> skipErrors;
        std::vector<std::string> inputFormats;
        std::vector<std::string> creationOptions;
        std::vector<std::string> layerCreationOptions;
        std::vector<std::string> openOptions;

        /// Copies values for `vector convert`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for building raster overviews.
    ///
    /// Requires an input dataset path. Execution updates that dataset in place.
    /// Explicit levels are integer reduction factors of at least two.
    /// If levels is empty, GDAL refreshes existing levels or selects levels
    /// using minimumSize. An unset minimumSize leaves GDAL's default unchanged.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterOverviewAddOptions
    {
        std::filesystem::path input;
        std::vector<int> levels;
        std::optional<std::string> resampling;
        std::optional<int> minimumSize;
        std::vector<std::string> creationOptions;
        std::vector<std::string> openOptions;

        /// Copies values for `raster overview add`. See @ref GdalTypedOptions
        /// for defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for reporting raster metadata and optional statistics.
    ///
    /// Requires an input dataset path. No output dataset path is needed.
    /// Unset report flags and outputFormat retain GDAL's report defaults.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterInfoOptions
    {
        std::filesystem::path input;
        std::optional<std::string> outputFormat;
        std::optional<bool> minMax;
        std::optional<bool> computeStatistics;
        std::optional<bool> approximateStatistics;
        std::optional<bool> computeHistogram;
        std::optional<bool> checksum;
        std::vector<std::string> inputFormats;
        std::vector<std::string> openOptions;

        /// Copies values for `raster info`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for reporting vector dataset and layer metadata.
    ///
    /// Requires an input dataset path. No output dataset path is needed.
    /// Set layer to select a layer. Unset report flags, filters, and outputFormat
    /// retain GDAL's report defaults.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API VectorInfoOptions
    {
        std::filesystem::path input;
        std::optional<std::string> outputFormat;
        std::optional<std::string> layer;
        std::optional<bool> summary;
        std::optional<bool> features;
        std::optional<int> limit;
        std::optional<std::string> where;
        std::optional<std::string> sql;
        std::vector<std::string> inputFormats;
        std::vector<std::string> openOptions;

        /// Copies values for `vector info`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for combining multiple raster datasets.
    ///
    /// Requires at least one input path and an output dataset path.
    /// bands contains one-based GDAL band numbers, not feature indexes.
    /// resolution accepts a GDAL strategy string or {x, y} pixel sizes.
    /// boundingBox is {xmin, ymin, xmax, ymax} in the input CRS.
    /// Unset resolution and bounds leave output grid selection to GDAL.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterMosaicOptions
    {
        std::vector<std::filesystem::path> inputs;
        std::filesystem::path output;
        std::optional<std::string> outputFormat;
        std::optional<bool> overwrite;
        std::optional<bool> append;
        std::optional<std::vector<int>> bands;
        std::optional<std::variant<std::string, std::array<double, 2>>> resolution;
        std::optional<std::array<double, 4>> boundingBox;
        std::optional<std::vector<double>> inputNoData;
        std::optional<std::vector<double>> outputNoData;
        std::vector<std::string> creationOptions;
        std::vector<std::string> inputFormats;
        std::vector<std::string> openOptions;

        /// Copies values for `raster mosaic`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        /// @throws InvalidArgument if a resolution value is not finite.
        /// @throws CoreError if numeric formatting fails.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for clipping a vector dataset by bounds, geometry, or a template.
    ///
    /// Requires input and output paths and exactly one of boundingBox, geometry,
    /// or like. boundingBox is {xmin, ymin, xmax, ymax} in the input CRS unless
    /// boundingBoxCrs is set. geometry accepts WKT or GeoJSON.
    /// like supplies the template dataset path. GDAL validates clip choices
    /// and resolves omitted geometry CRS and layer selection.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API VectorClipOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::optional<std::filesystem::path> like;
        std::optional<std::string> inputLayer;
        std::optional<std::string> likeLayer;
        std::optional<std::string> likeWhere;
        std::optional<std::string> outputLayer;
        std::optional<std::string> outputFormat;
        std::optional<bool> overwrite;
        std::optional<bool> update;
        std::optional<bool> overwriteLayer;
        std::optional<bool> append;
        std::optional<std::array<double, 4>> boundingBox;
        std::optional<std::string> boundingBoxCrs;
        std::optional<std::string> geometry;
        std::optional<std::string> geometryCrs;
        std::optional<bool> skipErrors;
        std::vector<std::string> creationOptions;
        std::vector<std::string> layerCreationOptions;
        std::vector<std::string> openOptions;

        /// Copies values for `vector clip`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for clipping a raster dataset by bounds, geometry, or a template.
    ///
    /// Requires input and output paths and exactly one clip selector: boundingBox,
    /// window, geometry, or like. window is {xOffset, yOffset, width, height}
    /// in pixels, with zero-based offsets from the top-left corner.
    /// boundingBox is {xmin, ymin, xmax, ymax} in the input CRS unless
    /// boundingBoxCrs is set. geometry accepts WKT or GeoJSON.
    /// GDAL validates selector combinations and resolves omitted CRS values.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterClipOptions
    {
        std::filesystem::path input;
        std::filesystem::path output;
        std::optional<std::string> outputFormat;
        std::optional<bool> overwrite;
        std::optional<bool> append;
        std::optional<std::array<double, 4>> boundingBox;
        std::optional<std::string> boundingBoxCrs;
        std::optional<std::array<int, 4>> window;
        std::optional<std::string> geometry;
        std::optional<std::string> geometryCrs;
        std::optional<std::filesystem::path> like;
        std::optional<std::string> likeLayer;
        std::optional<std::string> likeSql;
        std::optional<std::string> likeWhere;
        std::optional<bool> onlyBoundingBox;
        std::optional<bool> allowBoundingBoxOutsideSource;
        std::vector<std::string> creationOptions;
        std::vector<std::string> inputFormats;
        std::vector<std::string> openOptions;

        /// Copies values for `raster clip`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for calculating a raster from an expression and input datasets.
    ///
    /// Requires nonempty expressions and inputs and an output dataset path.
    /// GDAL evaluates expressions in order using its selected dialect.
    /// With one unnamed input, expressions refer to that dataset as X.
    /// Multiple inputs use GDAL's NAME=path notation in inputs.
    /// Unset dialect, data type, and nodata fields retain GDAL's defaults.
    /// @ingroup GdalTypedOptions
    struct MWG_CORE_API RasterCalcOptions
    {
        std::vector<std::string> expressions;
        std::vector<std::filesystem::path> inputs;
        std::filesystem::path output;
        std::optional<std::string> outputFormat;
        std::optional<std::string> outputDataType;
        std::optional<bool> overwrite;
        std::optional<bool> append;
        std::optional<double> outputNoData;
        std::optional<std::string> dialect;
        std::optional<bool> noCheckCrs;
        std::optional<bool> noCheckExtent;
        std::optional<bool> propagateNoData;
        std::optional<bool> flatten;
        std::vector<std::string> creationOptions;
        std::vector<std::string> inputFormats;
        std::vector<std::string> openOptions;

        /// Copies values for `raster calc`. See @ref GdalTypedOptions for
        /// defaults, validation, exceptions, and thread-safety requirements.
        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };
}
