// SPDX-License-Identifier: MPL-2.0

/**
 * @file ComConversion.h
 * @brief Declares conversions between COM strings and native UTF-8 data.
 *
 * Provides explicit BSTR, SAFEARRAY, and UTF-8 conversion helpers for the
 * ActiveX automation boundary.
 */
#pragma once

#include <Windows.h>
#include <OleAuto.h>

#include <string>
#include <string_view>
#include <vector>

namespace mwg::activex::ComConversion
{
    [[nodiscard]] HRESULT CopyToBstr(
        std::wstring_view value,
        BSTR* result) noexcept;
    [[nodiscard]] HRESULT BstrToUtf8(BSTR value, std::string& result);
    [[nodiscard]] HRESULT Utf8ToBstr(std::string_view value, BSTR* result);
    [[nodiscard]] HRESULT CopyStringArray(
        SAFEARRAY* source,
        std::vector<std::string>& values);
    [[nodiscard]] HRESULT CreateStringArray(
        const std::vector<std::string>& values,
        SAFEARRAY** result);
}
