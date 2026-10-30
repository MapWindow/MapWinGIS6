// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalOperationResult.cpp
 * @brief Implements GDAL operation-result diagnostics and dataset ownership.
 *
 * Formats diagnostics and releases returned dataset references under the
 * shared GDAL execution lock.
 */
#include "mapwingis/gdal/GdalOperationResult.h"

#include "internal/GdalExecution.h"

#include <gdal_priv.h>

namespace mwg
{
    int GdalOperationResult::LastErrorCode() const noexcept
    {
        for (auto error = errors.rbegin(); error != errors.rend(); ++error)
        {
            if (error->errorClass >= GdalErrorClass::Failure)
                return error->code;
        }
        return 0;
    }

    std::string GdalOperationResult::ErrorMsg() const
    {
        for (auto error = errors.rbegin(); error != errors.rend(); ++error)
        {
            if (error->errorClass >= GdalErrorClass::Failure)
                return error->message;
        }
        return {};
    }

    std::string GdalOperationResult::DetailedErrorMsg() const
    {
        std::string details;
        for (const auto& error : errors)
        {
            if (!details.empty())
                details += '\n';
            details += "GDAL error (class " +
                       std::to_string(static_cast<int>(error.errorClass)) +
                       ", code " + std::to_string(error.code) + "): " + error.message;
        }
        return details;
    }

    void GdalDatasetDeleter::operator()(GDALDataset* dataset) const noexcept
    {
        const std::lock_guard lock(detail::GdalExecutionMutex());
        if (dataset != nullptr)
            GDALReleaseDataset(dataset);
    }
}
