// SPDX-License-Identifier: MPL-2.0

/**
 * @file ComConversionTests.cpp
 * @brief Tests UTF-8 and COM string conversion helpers.
 *
 * Covers BSTR conversion and validation of SAFEARRAY string inputs.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <OleAuto.h>

#include "ComConversion.h"

namespace
{
    struct SafeArrayDeleter final
    {
        void operator()(SAFEARRAY* array) const noexcept
        {
            if (array != nullptr)
            {
                ::SafeArrayDestroy(array);
            }
        }
    };

    using SafeArrayUniquePtr = std::unique_ptr<SAFEARRAY, SafeArrayDeleter>;
}

TEST(ComConversion, ConvertsUtf8AndBstr)
{
    constexpr std::string_view text = "MapWinGIS \xE2\x9C\x93";
    BSTR raw = nullptr;

    ASSERT_EQ(mwg::activex::ComConversion::Utf8ToBstr(text, &raw), S_OK);
    std::unique_ptr<wchar_t, decltype(&::SysFreeString)> value{
        raw, &::SysFreeString};
    ASSERT_NE(value, nullptr);

    std::string converted;
    ASSERT_EQ(
        mwg::activex::ComConversion::BstrToUtf8(value.get(), converted),
        S_OK);
    EXPECT_EQ(converted, text);
}

TEST(ComConversion, HandlesEmptyAndNullBstrValues)
{
    BSTR empty = ::SysAllocStringLen(nullptr, 0);
    ASSERT_NE(empty, nullptr);
    std::unique_ptr<wchar_t, decltype(&::SysFreeString)> value{
        empty, &::SysFreeString};

    std::string converted = "previous value";
    EXPECT_EQ(
        mwg::activex::ComConversion::BstrToUtf8(value.get(), converted),
        S_OK);
    EXPECT_TRUE(converted.empty());
    EXPECT_EQ(
        mwg::activex::ComConversion::BstrToUtf8(nullptr, converted),
        E_INVALIDARG);
}

TEST(ComConversion, RejectsEmbeddedNullsAndInvalidUtf8)
{
    const wchar_t embeddedNull[] = {L'a', L'\0', L'b'};
    BSTR raw = ::SysAllocStringLen(embeddedNull, 3);
    ASSERT_NE(raw, nullptr);
    std::unique_ptr<wchar_t, decltype(&::SysFreeString)> value{
        raw, &::SysFreeString};
    std::string converted;
    EXPECT_EQ(
        mwg::activex::ComConversion::BstrToUtf8(value.get(), converted),
        E_INVALIDARG);

    wchar_t marker = L'x';
    BSTR invalid = &marker;
    EXPECT_EQ(
        mwg::activex::ComConversion::Utf8ToBstr(
            std::string_view("\xC3\x28", 2),
            &invalid),
        E_INVALIDARG);
    EXPECT_EQ(invalid, nullptr);

    EXPECT_EQ(
        mwg::activex::ComConversion::Utf8ToBstr(
            std::string_view("a\0b", 3),
            &invalid),
        E_INVALIDARG);
    EXPECT_EQ(invalid, nullptr);
}

TEST(ComConversion, CopiesOneDimensionalStringArrayWithNonzeroLowerBound)
{
    SAFEARRAYBOUND bound{2, 5};
    SafeArrayUniquePtr array{::SafeArrayCreate(VT_BSTR, 1, &bound)};
    ASSERT_NE(array, nullptr);

    BSTR firstRaw = ::SysAllocString(L"first");
    BSTR secondRaw = ::SysAllocString(L"second");
    std::unique_ptr<wchar_t, decltype(&::SysFreeString)> first{
        firstRaw, &::SysFreeString};
    std::unique_ptr<wchar_t, decltype(&::SysFreeString)> second{
        secondRaw, &::SysFreeString};
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(
        ::SafeArrayPutElement(array.get(), &bound.lLbound, first.get()),
        S_OK);
    LONG secondIndex = bound.lLbound + 1;
    EXPECT_EQ(
        ::SafeArrayPutElement(array.get(), &secondIndex, second.get()),
        S_OK);

    std::vector<std::string> values;
    EXPECT_EQ(
        mwg::activex::ComConversion::CopyStringArray(array.get(), values),
        S_OK);
    EXPECT_EQ(values, (std::vector<std::string>{"first", "second"}));
}

TEST(ComConversion, RejectsWrongArrayDimensionAndElementType)
{
    SAFEARRAYBOUND bounds[2]{{1, 0}, {1, 0}};
    SafeArrayUniquePtr twoDimensional{
        ::SafeArrayCreate(VT_BSTR, 2, bounds)};
    ASSERT_NE(twoDimensional, nullptr);
    std::vector<std::string> values;
    EXPECT_EQ(
        mwg::activex::ComConversion::CopyStringArray(
            twoDimensional.get(),
            values),
        E_INVALIDARG);

    SafeArrayUniquePtr integerArray{
        ::SafeArrayCreateVector(VT_I4, 0, 1)};
    ASSERT_NE(integerArray, nullptr);
    EXPECT_EQ(
        mwg::activex::ComConversion::CopyStringArray(
            integerArray.get(),
            values),
        E_INVALIDARG);
}

TEST(ComConversion, AcceptsNullAndEmptyStringArrays)
{
    std::vector<std::string> values;
    EXPECT_EQ(
        mwg::activex::ComConversion::CopyStringArray(nullptr, values),
        S_OK);

    SafeArrayUniquePtr emptyArray{
        ::SafeArrayCreateVector(VT_BSTR, 0, 0)};
    ASSERT_NE(emptyArray, nullptr);
    EXPECT_EQ(
        mwg::activex::ComConversion::CopyStringArray(
            emptyArray.get(),
            values),
        S_OK);
    EXPECT_TRUE(values.empty());
}
