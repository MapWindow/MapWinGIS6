// SPDX-License-Identifier: MPL-2.0

/**
 * @file Error.h
 * @brief Declares exceptions used to report core operation failures.
 *
 * CoreError is the base exception. Its subclasses identify common operation
 * failures for callers and the ActiveX layer.
 */
#pragma once

#include <stdexcept>
#include <string>

#include "mapwingis/core/Api.h"

namespace mwg
{
    /// Thrown when a core operation fails.
    ///
    /// Exceptions cross the MapWinGIS.Core.dll boundary; see Api.h for why that
    /// is safe here. The ActiveX layer converts them into MapWinGIS error codes.
    /// Export constructors individually because MSVC's std::runtime_error is
    /// not a DLL-interface base class.
    class CoreError : public std::runtime_error
    {
    public:
        MWG_CORE_API explicit CoreError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    /// A file could not be opened, or no driver could read it.
    class OpenFailed : public CoreError
    {
    public:
        MWG_CORE_API explicit OpenFailed(const std::string& message)
            : CoreError(message)
        {
        }
    };

    /// An argument is invalid for the requested operation.
    class InvalidArgument : public CoreError
    {
    public:
        MWG_CORE_API explicit InvalidArgument(const std::string& message)
            : CoreError(message)
        {
        }
    };
}
