// SPDX-License-Identifier: MPL-2.0

/**
 * @file Enums.h
 * @brief Defines geometry and attribute types used by the core API.
 *
 * ShapeType and FieldType retain the numeric values of the corresponding
 * MapWinGIS 5 enums for conversion by the ActiveX layer.
 */
#pragma once

namespace mwg
{
    /// Geometry type of a layer or shape.
    ///
    /// The values are identical to the ShpfileType enum of the MapWinGIS 5 type
    /// library, so the ActiveX layer can convert with a static_cast.
    enum class ShapeType : int
    {
        NullShape = 0,
        Point = 1,
        PolyLine = 3,
        Polygon = 5,
        MultiPoint = 8,
        PointZ = 11,
        PolyLineZ = 13,
        PolygonZ = 15,
        MultiPointZ = 18,
        PointM = 21,
        PolyLineM = 23,
        PolygonM = 25,
        MultiPointM = 28,
        MultiPatch = 31,
    };

    /// Attribute field type.
    ///
    /// The values are identical to the FieldType enum of the MapWinGIS 5 type
    /// library.
    enum class FieldType : int
    {
        String = 0,
        Integer = 1,
        Double = 2,
        Boolean = 3,
        Date = 4,
    };

    /// Drops the Z and M variants, e.g. PolygonZ -> Polygon.
    constexpr ShapeType BaseShapeType(const ShapeType type) noexcept
    {
        switch (type)
        {
        case ShapeType::PointZ:
        case ShapeType::PointM:
            return ShapeType::Point;
        case ShapeType::PolyLineZ:
        case ShapeType::PolyLineM:
            return ShapeType::PolyLine;
        case ShapeType::PolygonZ:
        case ShapeType::PolygonM:
            return ShapeType::Polygon;
        case ShapeType::MultiPointZ:
        case ShapeType::MultiPointM:
            return ShapeType::MultiPoint;
        default:
            return type;
        }
    }
}
