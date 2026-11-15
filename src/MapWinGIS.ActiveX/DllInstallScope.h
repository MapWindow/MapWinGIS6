// SPDX-License-Identifier: MPL-2.0

/**
 * @file DllInstallScope.h
 * @brief Parses the registration scope passed to DllInstall.
 */
#pragma once

#include <Windows.h>

enum class DllInstallRegistrationScope
{
    Machine,
    User,
    Invalid
};

[[nodiscard]] inline DllInstallRegistrationScope ParseDllInstallRegistrationScope(
    const wchar_t* commandLine) noexcept
{
    if (commandLine == nullptr || commandLine[0] == L'\0')
    {
        return DllInstallRegistrationScope::Machine;
    }
    return ::CompareStringOrdinal(commandLine, -1, L"user", -1, TRUE) == CSTR_EQUAL
        ? DllInstallRegistrationScope::User
        : DllInstallRegistrationScope::Invalid;
}
