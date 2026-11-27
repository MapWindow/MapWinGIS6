// SPDX-License-Identifier: MPL-2.0

/**
 * @file Projection.h
 * @brief Declares immutable coordinate reference system values and transforms.
 *
 * Provides EPSG-first CRS identity, GDAL-backed parsing and export, and
 * stateless coordinate transformations without exposing GDAL types.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "mapwingis/core/Api.h"

namespace mwg
{
    class Projection;

    /// A reusable, stateless coordinate transformation returned by Projection.
    ///
    /// Copies share the GDAL transformation. Coordinates are transformed in
    /// traditional GIS order (x, y, then optional z). The callable overloads
    /// return false when GDAL cannot transform the coordinate.
    class MWG_CORE_API ProjectionTransform
    {
    public:
        ProjectionTransform(const ProjectionTransform&);
        ProjectionTransform& operator=(const ProjectionTransform&);
        ProjectionTransform(ProjectionTransform&&) noexcept;
        ProjectionTransform& operator=(ProjectionTransform&&) noexcept;
        ~ProjectionTransform();

        [[nodiscard]] bool operator()(double& x, double& y) const;
        [[nodiscard]] bool operator()(double& x, double& y, double& z) const;

        /// Transforms arrays in place. z and perPointSuccess may be nullptr.
        /// Returns false when one or more points fail.
        [[nodiscard]] bool Transform(std::size_t count,
                                     double* x,
                                     double* y,
                                     double* z = nullptr,
                                     int* perPointSuccess = nullptr) const;

    private:
        friend class Projection;

        class Impl;
        explicit ProjectionTransform(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> _impl;
    };

    /// Immutable coordinate reference system value.
    ///
    /// A default-constructed Projection is empty and represents an unknown
    /// CRS, not an error. Copies are cheap and share cached GDAL state. Core
    /// projection objects are not thread-safe.
    class MWG_CORE_API Projection
    {
    public:
        Projection();
        Projection(const Projection&);
        Projection& operator=(const Projection&);
        Projection(Projection&&) noexcept;
        Projection& operator=(Projection&&) noexcept;
        ~Projection();

        [[nodiscard]] static Projection FromEpsg(int code);
        [[nodiscard]] static Projection FromWkt(std::string_view wkt);
        [[nodiscard]] static Projection FromProj4(std::string_view proj4);
        [[nodiscard]] static std::optional<Projection> TryFromWkt(std::string_view wkt);
        [[nodiscard]] static std::optional<Projection> TryFromProj4(std::string_view proj4);

        /// EPSG code, autodetected when authority metadata is absent.
        [[nodiscard]] std::optional<int> Epsg() const;
        [[nodiscard]] std::string Name() const;
        [[nodiscard]] bool IsGeographic() const;
        [[nodiscard]] bool IsProjected() const;
        [[nodiscard]] bool IsEmpty() const noexcept;
        [[nodiscard]] std::string ExportToWkt() const;
        [[nodiscard]] std::string ExportToProj4() const;

        /// Compares CRS identity by EPSG code when possible, otherwise by OGR
        /// equivalence and normalized CRS/projection names.
        [[nodiscard]] bool IsSame(const Projection& other) const;

        /// Creates a stateless transformation using traditional GIS axis order.
        /// \throws InvalidArgument when either CRS is empty.
        /// \throws CoreError when GDAL cannot construct the transformation.
        [[nodiscard]] ProjectionTransform TransformTo(const Projection& target) const;

    private:
        class Impl;
        explicit Projection(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> _impl;
    };
}
