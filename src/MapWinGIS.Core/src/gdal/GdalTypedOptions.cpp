// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalTypedOptions.cpp
 * @brief Converts typed GDAL algorithm options to validated arguments.
 *
 * Implements the high-priority raster and vector option structures and
 * applies their values through the generic algorithm runner.
 */
#include "mapwingis/gdal/GdalTypedOptions.h"

#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "internal/Utf8.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace mwg
{
    namespace
    {
        void AddPath(GdalAlgorithmArguments& arguments,
                     const std::string_view name,
                     const std::filesystem::path& value)
        {
            arguments.push_back({std::string(name), detail::ToUtf8(value)});
        }

        void AddPaths(GdalAlgorithmArguments& arguments,
                      const std::string_view name,
                      const std::vector<std::filesystem::path>& values)
        {
            if (values.empty())
                return;

            std::vector<std::string> paths;
            paths.reserve(values.size());
            for (const std::filesystem::path& value : values)
                paths.push_back(detail::ToUtf8(value));
            arguments.push_back({std::string(name), std::move(paths)});
        }

        template <typename T>
        void AddOptional(GdalAlgorithmArguments& arguments,
                         const std::string_view name,
                         const std::optional<T>& value)
        {
            if (value.has_value())
                arguments.push_back({std::string(name), *value});
        }

        template <typename T, std::size_t N>
        void AddArray(GdalAlgorithmArguments& arguments,
                      const std::string_view name,
                      const std::optional<std::array<T, N>>& value)
        {
            if (!value.has_value())
                return;

            std::vector<T> values(value->begin(), value->end());
            arguments.push_back({std::string(name), std::move(values)});
        }

        void AddOptions(GdalAlgorithmArguments& arguments,
                        const std::string_view name,
                        const std::vector<std::string>& values)
        {
            if (!values.empty())
                arguments.push_back({std::string(name), values});
        }

        std::string FormatDouble(const double value)
        {
            char buffer[64];
            const auto [end, error] =
                std::to_chars(buffer, buffer + sizeof(buffer), value,
                              std::chars_format::general,
                              std::numeric_limits<double>::max_digits10);
            if (error != std::errc{})
                throw InvalidArgument("Could not format a GDAL option value.");
            return {buffer, end};
        }

        void AddNumericStringList(
            GdalAlgorithmArguments& arguments,
            const std::string_view name,
            const std::optional<std::vector<double>>& values)
        {
            if (!values.has_value() || values->empty())
                return;

            std::vector<std::string> strings;
            strings.reserve(values->size());
            for (const double value : *values)
                strings.push_back(FormatDouble(value));
            arguments.push_back({std::string(name), std::move(strings)});
        }

        void AddMosaicResolution(
            GdalAlgorithmArguments& arguments,
            const std::optional<std::variant<std::string,
                                             std::array<double, 2>>>& resolution)
        {
            if (!resolution.has_value())
                return;

            std::visit(
                [&arguments](const auto& value)
                {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, std::string>)
                    {
                        arguments.push_back({"resolution", value});
                    }
                    else
                    {
                        if (!std::isfinite(value[0]) || !std::isfinite(value[1]))
                            throw InvalidArgument("GDAL mosaic resolution values must be finite.");
                        std::string text = FormatDouble(value[0]);
                        text += ',';
                        text += FormatDouble(value[1]);
                        arguments.push_back({"resolution", std::move(text)});
                    }
                },
                *resolution);
        }
    }

    bool ConfigureGdalAlgorithm(GdalAlgorithmRunner& runner,
                                const GdalAlgorithmArguments& arguments)
    {
        for (const GdalAlgorithmArgument& argument : arguments)
        {
            const bool set = std::visit(
                [&runner, &argument](const auto& value)
                {
                    return runner.SetArgument(argument.name, value);
                },
                argument.value);
            if (!set)
                return false;
        }
        return runner.ValidateArguments();
    }

    GdalAlgorithmArguments RasterConvertOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "input", input);
        AddPath(arguments, "output", output);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "append", append);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments RasterReprojectOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "input", input);
        AddPath(arguments, "output", output);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "src-crs", sourceCrs);
        AddOptional(arguments, "dst-crs", targetCrs);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "append", append);
        AddArray(arguments, "resolution", resolution);
        AddArray(arguments, "size", size);
        AddArray(arguments, "bbox", boundingBox);
        AddOptional(arguments, "bbox-crs", boundingBoxCrs);
        AddOptional(arguments, "resampling", resampling);
        AddNumericStringList(arguments, "src-nodata", inputNoData);
        AddNumericStringList(arguments, "dst-nodata", outputNoData);
        AddOptional(arguments, "target-aligned-pixels", targetAlignedPixels);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments VectorConvertOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "input", input);
        AddPath(arguments, "output", output);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptions(arguments, "input-layer", inputLayers);
        AddOptional(arguments, "output-layer", outputLayer);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "update", update);
        AddOptional(arguments, "overwrite-layer", overwriteLayer);
        AddOptional(arguments, "append", append);
        AddOptional(arguments, "skip-errors", skipErrors);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "layer-creation-option", layerCreationOptions);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments RasterOverviewAddOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "dataset", input);
        if (!levels.empty())
            arguments.push_back({"levels", levels});
        AddOptional(arguments, "resampling", resampling);
        AddOptional(arguments, "min-size", minimumSize);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments RasterInfoOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "dataset", input);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "min-max", minMax);
        AddOptional(arguments, "stats", computeStatistics);
        AddOptional(arguments, "approx-stats", approximateStatistics);
        AddOptional(arguments, "hist", computeHistogram);
        AddOptional(arguments, "checksum", checksum);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments VectorInfoOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "input", input);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "input-layer", layer);
        AddOptional(arguments, "summary", summary);
        AddOptional(arguments, "features", features);
        AddOptional(arguments, "limit", limit);
        AddOptional(arguments, "where", where);
        AddOptional(arguments, "sql", sql);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments RasterMosaicOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPaths(arguments, "input", inputs);
        AddPath(arguments, "output", output);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "append", append);
        AddOptional(arguments, "band", bands);
        AddMosaicResolution(arguments, resolution);
        AddArray(arguments, "bbox", boundingBox);
        AddOptional(arguments, "src-nodata", inputNoData);
        AddOptional(arguments, "dst-nodata", outputNoData);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments VectorClipOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "input", input);
        AddPath(arguments, "output", output);
        if (like.has_value())
            AddPath(arguments, "like", *like);
        AddOptional(arguments, "input-layer", inputLayer);
        AddOptional(arguments, "like-layer", likeLayer);
        AddOptional(arguments, "like-where", likeWhere);
        AddOptional(arguments, "output-layer", outputLayer);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "update", update);
        AddOptional(arguments, "overwrite-layer", overwriteLayer);
        AddOptional(arguments, "append", append);
        AddArray(arguments, "bbox", boundingBox);
        AddOptional(arguments, "bbox-crs", boundingBoxCrs);
        AddOptional(arguments, "geometry", geometry);
        AddOptional(arguments, "geometry-crs", geometryCrs);
        AddOptional(arguments, "skip-errors", skipErrors);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "layer-creation-option", layerCreationOptions);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments RasterClipOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddPath(arguments, "input", input);
        AddPath(arguments, "output", output);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "append", append);
        AddArray(arguments, "bbox", boundingBox);
        AddOptional(arguments, "bbox-crs", boundingBoxCrs);
        AddArray(arguments, "window", window);
        AddOptional(arguments, "geometry", geometry);
        AddOptional(arguments, "geometry-crs", geometryCrs);
        if (like.has_value())
            AddPath(arguments, "like", *like);
        AddOptional(arguments, "like-layer", likeLayer);
        AddOptional(arguments, "like-sql", likeSql);
        AddOptional(arguments, "like-where", likeWhere);
        AddOptional(arguments, "only-bbox", onlyBoundingBox);
        AddOptional(arguments, "allow-bbox-outside-source", allowBoundingBoxOutsideSource);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }

    GdalAlgorithmArguments RasterCalcOptions::ToArgs() const
    {
        GdalAlgorithmArguments arguments;
        AddOptions(arguments, "calc", expressions);
        AddPaths(arguments, "input", inputs);
        AddPath(arguments, "output", output);
        AddOptional(arguments, "output-format", outputFormat);
        AddOptional(arguments, "output-data-type", outputDataType);
        AddOptional(arguments, "overwrite", overwrite);
        AddOptional(arguments, "append", append);
        AddOptional(arguments, "nodata", outputNoData);
        AddOptional(arguments, "dialect", dialect);
        AddOptional(arguments, "no-check-srs", noCheckCrs);
        AddOptional(arguments, "no-check-extent", noCheckExtent);
        AddOptional(arguments, "propagate-nodata", propagateNoData);
        AddOptional(arguments, "flatten", flatten);
        AddOptions(arguments, "creation-option", creationOptions);
        AddOptions(arguments, "input-format", inputFormats);
        AddOptions(arguments, "open-option", openOptions);
        return arguments;
    }
}
