// SPDX-License-Identifier: MPL-2.0

/**
 * @file translate.cpp
 * @brief Converts a raster dataset with the typed GDAL API.
 */
#include <exception>
#include <filesystem>
#include <iostream>

#include "mapwingis/gdal/GdalAlgorithmRunner.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/gdal/GdalTypedOptions.h"

int wmain(const int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        std::wcerr << L"Usage: translate <input-raster> <output-raster>\n";
        return 2;
    }

    try
    {
        mwg::GdalEnvironment::Initialize();

        mwg::RasterConvertOptions options;
        options.input = std::filesystem::path(argv[1]);
        options.output = std::filesystem::path(argv[2]);
        options.outputFormat = "GTiff";

        mwg::GdalAlgorithmRunner runner("raster convert");
        if (!mwg::ConfigureGdalAlgorithm(runner, options.ToArgs()) || !runner.Run())
        {
            std::cerr << runner.Result().DetailedErrorMsg() << '\n';
            return 1;
        }

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
