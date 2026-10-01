// SPDX-License-Identifier: MPL-2.0

/**
 * @file Error.h
 * @brief Declares exceptions used to report core operation failures.
 *
 * CoreError is the base exception and OpenFailed represents data source open
 * failures. The ActiveX layer translates these exceptions to MapWinGIS errors.
 */
#pragma once

#include <stdexcept>
#include <string>

namespace mwg
{
    /// Thrown when a core operation fails.
    ///
    /// Exceptions cross the MapWinGIS.Core.dll boundary; see Api.h for why that
    /// is safe here. The ActiveX layer converts them into MapWinGIS error codes.
    class CoreError : public std::runtime_error
    {
    public:
        explicit CoreError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    /// A file could not be opened, or no driver could read it.
    class OpenFailed : public CoreError
    {
    public:
        explicit OpenFailed(const std::string& message)
            : CoreError(message)
        {
        }
    };
}
