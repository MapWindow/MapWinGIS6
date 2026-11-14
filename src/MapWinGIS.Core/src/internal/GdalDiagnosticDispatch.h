// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalDiagnosticDispatch.h
 * @brief Routes GDAL diagnostics to the MapWinGIS logger and the host handler.
 */
#pragma once

#include <exception>

#include <cpl_error.h>

namespace mwg::detail
{
    /// Reports a GDAL diagnostic to the MapWinGIS logger, then forwards it with
    /// its original severity and error number to the host-installed process-wide
    /// GDAL handler. Both run inside GdalCallbackScope. The first exception
    /// raised by either is stored in firstException; none escapes.
    void DispatchGdalDiagnostic(CPLErr severity, CPLErrorNum errorNumber, const char* message,
                                std::exception_ptr& firstException) noexcept;
}
