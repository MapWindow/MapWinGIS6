// SPDX-License-Identifier: MPL-2.0

/**
 * @file ComConversion.cpp
 * @brief Implements conversions between COM strings and native UTF-8 data.
 */
#include "ComConversion.h"

#include <limits>
#include <utility>

namespace mwg::activex::ComConversion
{
    HRESULT CopyToBstr(
        const std::wstring_view value,
        BSTR* result) noexcept
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }

        *result = ::SysAllocStringLen(
            value.data(),
            static_cast<UINT>(value.size()));
        return *result != nullptr ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT BstrToUtf8(const BSTR value, std::string& result)
    {
        if (value == nullptr)
        {
            return E_INVALIDARG;
        }

        const UINT length = ::SysStringLen(value);
        if (length > static_cast<UINT>(std::numeric_limits<int>::max()) ||
            std::wstring_view(value, length).find(L'\0') != std::wstring_view::npos)
        {
            return E_INVALIDARG;
        }
        if (length == 0)
        {
            result.clear();
            return S_OK;
        }

        const int required = ::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value,
            static_cast<int>(length),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required == 0)
        {
            return E_INVALIDARG;
        }

        result.resize(static_cast<std::size_t>(required));
        if (::WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value,
                static_cast<int>(length),
                result.data(),
                required,
                nullptr,
                nullptr) == 0)
        {
            return E_INVALIDARG;
        }
        return S_OK;
    }

    HRESULT Utf8ToBstr(const std::string_view value, BSTR* result)
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return E_INVALIDARG;
        }
        if (value.empty())
        {
            *result = ::SysAllocStringLen(nullptr, 0);
            return *result != nullptr ? S_OK : E_OUTOFMEMORY;
        }

        const int required = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (required == 0)
        {
            return E_INVALIDARG;
        }

        BSTR converted = ::SysAllocStringLen(nullptr, static_cast<UINT>(required));
        if (converted == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        if (::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                converted,
                required) == 0)
        {
            ::SysFreeString(converted);
            return E_INVALIDARG;
        }

        *result = converted;
        return S_OK;
    }

    HRESULT CopyStringArray(
        SAFEARRAY* source,
        std::vector<std::string>& values)
    {
        if (source == nullptr)
        {
            return S_OK;
        }
        if (::SafeArrayGetDim(source) != 1)
        {
            return E_INVALIDARG;
        }

        VARTYPE elementType = VT_EMPTY;
        if (FAILED(::SafeArrayGetVartype(source, &elementType)) ||
            elementType != VT_BSTR)
        {
            return E_INVALIDARG;
        }

        LONG lowerBound = 0;
        LONG upperBound = -1;
        HRESULT result = ::SafeArrayGetLBound(source, 1, &lowerBound);
        if (FAILED(result))
        {
            return result;
        }
        result = ::SafeArrayGetUBound(source, 1, &upperBound);
        if (FAILED(result))
        {
            return result;
        }
        if (upperBound < lowerBound)
        {
            return S_OK;
        }

        const auto count =
            static_cast<unsigned long long>(
                static_cast<long long>(upperBound) -
                static_cast<long long>(lowerBound) + 1);
        if (count > static_cast<unsigned long long>(values.max_size()))
        {
            return E_INVALIDARG;
        }
        values.reserve(static_cast<std::size_t>(count));

        for (LONG index = lowerBound;; ++index)
        {
            BSTR argument = nullptr;
            result = ::SafeArrayGetElement(source, &index, &argument);
            if (FAILED(result))
            {
                return result;
            }

            std::string converted;
            result = BstrToUtf8(argument, converted);
            ::SysFreeString(argument);
            if (FAILED(result))
            {
                return result;
            }
            values.push_back(std::move(converted));
            if (index == upperBound)
            {
                break;
            }
        }
        return S_OK;
    }

    HRESULT CreateStringArray(
        const std::vector<std::string>& values,
        SAFEARRAY** result)
    {
        if (result == nullptr)
        {
            return E_POINTER;
        }
        *result = nullptr;
        if (values.empty())
        {
            return S_OK;
        }
        if (values.size() >
            static_cast<std::size_t>(std::numeric_limits<LONG>::max()))
        {
            return E_INVALIDARG;
        }

        SAFEARRAY* array = ::SafeArrayCreateVector(
            VT_BSTR,
            0,
            static_cast<ULONG>(values.size()));
        if (array == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        for (std::size_t offset = 0; offset < values.size(); ++offset)
        {
            BSTR value = nullptr;
            HRESULT status = Utf8ToBstr(values[offset], &value);
            if (FAILED(status))
            {
                ::SafeArrayDestroy(array);
                return status;
            }

            LONG index = static_cast<LONG>(offset);
            status = ::SafeArrayPutElement(array, &index, value);
            ::SysFreeString(value);
            if (FAILED(status))
            {
                ::SafeArrayDestroy(array);
                return status;
            }
        }

        *result = array;
        return S_OK;
    }
}
