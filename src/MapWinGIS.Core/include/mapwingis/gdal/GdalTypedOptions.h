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

namespace mwg
{
    class GdalAlgorithmRunner;

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

    /// Sets typed arguments and validates them with GDAL's algorithm metadata.
    [[nodiscard]] MWG_CORE_API bool ConfigureGdalAlgorithm(
        GdalAlgorithmRunner& runner, const GdalAlgorithmArguments& arguments);

    /// Options for GDAL raster format conversion.
    ///
    /// Unset values use GDAL defaults. GDAL 3.12's raster convert algorithm
    /// does not expose raster translate's window, band, or data-type options.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for reprojecting a raster dataset.
    ///
    /// Mutually exclusive choices, such as target resolution and target size,
    /// are validated by the instantiated GDAL algorithm.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for converting vector datasets and layers.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for building raster overviews.
    struct MWG_CORE_API RasterOverviewAddOptions
    {
        std::filesystem::path input;
        std::vector<int> levels;
        std::optional<std::string> resampling;
        std::optional<int> minimumSize;
        std::vector<std::string> creationOptions;
        std::vector<std::string> openOptions;

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for reporting raster metadata and optional statistics.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for reporting vector dataset and layer metadata.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for combining multiple raster datasets.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for clipping a vector dataset by bounds, geometry, or a template.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for clipping a raster dataset by bounds, geometry, or a template.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };

    /// Options for calculating a raster from an expression and input datasets.
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

        [[nodiscard]] GdalAlgorithmArguments ToArgs() const;
    };
}
