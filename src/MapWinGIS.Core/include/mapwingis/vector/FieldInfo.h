// SPDX-License-Identifier: MPL-2.0

/**
 * @file FieldInfo.h
 * @brief Defines metadata for a vector layer attribute field.
 *
 * FieldInfo stores the field name, MapWinGIS field type, width, and precision
 * exposed by the vector dataset API.
 */
#pragma once

#include <string>

#include "mapwingis/core/Enums.h"

namespace mwg
{
    /// Definition of one attribute field.
    struct FieldInfo
    {
        std::string name;
        FieldType type = FieldType::String;
        int width = 0;
        int precision = 0;
    };
}
