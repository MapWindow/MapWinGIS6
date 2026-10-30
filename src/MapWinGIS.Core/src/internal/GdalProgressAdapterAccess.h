// SPDX-License-Identifier: MPL-2.0

/**
 * @file GdalProgressAdapterAccess.h
 * @brief Declares internal access to GDAL's native progress callback ABI.
 *
 * Keeps GDAL callback plumbing private while allowing native Core operations
 * to bind the public C++ progress adapter to GDAL.
 */
#pragma once

#include "mapwingis/gdal/GdalProgress.h"

namespace mwg::detail
{
    class GdalProgressAdapterAccess final
    {
    public:
        using NativeCallback = int(__stdcall*)(double, const char*, void*);

        [[nodiscard]] static NativeCallback Function(const GdalProgressAdapter& adapter) noexcept;
        [[nodiscard]] static void* UserData(GdalProgressAdapter& adapter) noexcept;

    private:
        static int __stdcall HandleProgress(double progress, const char* message, void* userData) noexcept;
    };
}
