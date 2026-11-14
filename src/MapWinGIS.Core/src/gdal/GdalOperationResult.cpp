// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalOperationResult.cpp
 * @brief Implements GDAL operation-result diagnostics and dataset ownership.
 *
 * Formats diagnostics and releases returned dataset references without
 * waiting on the shared GDAL execution lock.
 */
#include "mapwingis/gdal/GdalOperationResult.h"

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
        // GDALReleaseDataset only decrements the reference count, which GDAL
        // implements atomically; the actual close runs on the releasing
        // thread when the last reference goes away. Releasing must never
        // wait for unrelated operations that hold the execution lock.
        if (dataset != nullptr)
            GDALReleaseDataset(dataset);
    }
}
