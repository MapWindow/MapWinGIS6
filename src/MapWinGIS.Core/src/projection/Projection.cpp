// SPDX-License-Identifier: MPL-2.0

/**
 * @file Projection.cpp
 * @brief Implements immutable coordinate reference systems using OGR.
 *
 * Handles CRS parsing and export, EPSG identification, equivalence checks,
 * and traditional-GIS-order coordinate transformation construction.
 */
#include "mapwingis/projection/Projection.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <utility>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <ogr_api.h>
#include <ogr_spatialref.h>

#include "mapwingis/core/Error.h"
#include "mapwingis/gdal/GdalEnvironment.h"

namespace mwg
{
    namespace
    {
        using SpatialReferencePtr = std::shared_ptr<const OGRSpatialReference>;

        std::string ExportedString(char* value)
        {
            std::string result = value != nullptr ? value : "";
            CPLFree(value);
            return result;
        }

        std::string FailureMessage(const std::string_view prefix)
        {
            std::string message(prefix);
            const char* detail = CPLGetLastErrorMsg();
            if (detail != nullptr && detail[0] != '\0')
            {
                message += ": ";
                message += detail;
            }
            return message;
        }

        std::optional<int> AuthorityCode(const OGRSpatialReference& reference)
        {
            const char* code = reference.GetAuthorityCode(nullptr);
            if (code == nullptr)
            {
                code = reference.GetAuthorityCode("PROJCS");
            }
            if (code == nullptr)
            {
                code = reference.GetAuthorityCode("GEOGCS");
            }
            if (code == nullptr)
            {
                return std::nullopt;
            }

            char* end = nullptr;
            const long parsed = std::strtol(code, &end, 10);
            if (end == code || *end != '\0' ||
                parsed < 1 || parsed > std::numeric_limits<int>::max())
            {
                return std::nullopt;
            }
            return static_cast<int>(parsed);
        }

        std::string NormalizedName(const char* name)
        {
            if (name == nullptr)
            {
                return {};
            }

            std::string normalized;
            normalized.reserve(std::char_traits<char>::length(name));
            for (const unsigned char character : std::string_view(name))
            {
                if (std::isalnum(character))
                {
                    normalized.push_back(static_cast<char>(std::tolower(character)));
                }
            }
            return normalized;
        }

        SpatialReferencePtr MakeSpatialReference(const OGRSpatialReference& source)
        {
            std::unique_ptr<OGRSpatialReference> copy(source.Clone());
            if (copy == nullptr)
            {
                throw CoreError("Cannot copy coordinate reference system");
            }
            copy->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            return SpatialReferencePtr(copy.release());
        }
    }

    class ProjectionTransform::Impl
    {
    public:
        explicit Impl(OGRCoordinateTransformation* transformation)
            : _transformation(transformation, OGRCoordinateTransformation::DestroyCT)
        {
        }

        std::shared_ptr<OGRCoordinateTransformation> _transformation;
    };

    class Projection::Impl
    {
    public:
        explicit Impl(SpatialReferencePtr reference)
            : _reference(std::move(reference))
        {
        }

        std::optional<int> ResolveEpsg() const
        {
            if (_epsgResolved)
            {
                return _epsg;
            }

            _epsgResolved = true;
            std::unique_ptr<OGRSpatialReference> candidate(_reference->Clone());
            if (candidate == nullptr)
            {
                throw CoreError("Cannot copy coordinate reference system");
            }

            (void)candidate->morphFromESRI();
            (void)candidate->AutoIdentifyEPSG();
            _epsg = AuthorityCode(*candidate);

            if (_epsg.has_value())
            {
                return _epsg;
            }

            int count = 0;
            int* confidence = nullptr;
            OGRSpatialReferenceH* matches = OSRFindMatches(
                OGRSpatialReference::ToHandle(candidate.get()),
                nullptr, &count, &confidence);
            if (matches != nullptr)
            {
                for (int index = 0; index < count && !_epsg.has_value(); ++index)
                {
                    if (confidence == nullptr || confidence[index] < 80)
                    {
                        continue;
                    }

                    const auto* matched =
                        OGRSpatialReference::FromHandle(matches[index]);
                    if (matched != nullptr)
                    {
                        _epsg = AuthorityCode(*matched);
                    }
                }
                OSRFreeSRSArray(matches);
            }
            CPLFree(confidence);
            return _epsg;
        }

        SpatialReferencePtr _reference;
        mutable bool _epsgResolved = false;
        mutable std::optional<int> _epsg;
    };

    ProjectionTransform::ProjectionTransform(std::shared_ptr<Impl> impl) noexcept
        : _impl(std::move(impl))
    {
    }

    ProjectionTransform::ProjectionTransform(const ProjectionTransform&) = default;
    ProjectionTransform& ProjectionTransform::operator=(const ProjectionTransform&) = default;
    ProjectionTransform::ProjectionTransform(ProjectionTransform&&) noexcept = default;
    ProjectionTransform& ProjectionTransform::operator=(ProjectionTransform&&) noexcept = default;
    ProjectionTransform::~ProjectionTransform() = default;

    bool ProjectionTransform::operator()(double& x, double& y) const
    {
        return Transform(1, &x, &y);
    }

    bool ProjectionTransform::operator()(double& x, double& y, double& z) const
    {
        return Transform(1, &x, &y, &z);
    }

    bool ProjectionTransform::Transform(const std::size_t count,
                                       double* x,
                                       double* y,
                                       double* z,
                                       int* perPointSuccess) const
    {
        if (x == nullptr || y == nullptr || _impl == nullptr)
        {
            return false;
        }
        return _impl->_transformation->Transform(
                   count, x, y, z, perPointSuccess) == TRUE;
    }

    Projection::Projection() = default;
    Projection::Projection(std::shared_ptr<Impl> impl) noexcept
        : _impl(std::move(impl))
    {
    }

    Projection::Projection(const Projection&) = default;
    Projection& Projection::operator=(const Projection&) = default;
    Projection::Projection(Projection&&) noexcept = default;
    Projection& Projection::operator=(Projection&&) noexcept = default;
    Projection::~Projection() = default;

    Projection Projection::FromEpsg(const int code)
    {
        if (code <= 0)
        {
            throw InvalidArgument("EPSG code must be positive");
        }

        GdalEnvironment::Initialize();
        const GdalEnvironment::QuietScope quiet;
        OGRSpatialReference reference;
        CPLErrorReset();
        if (reference.importFromEPSG(code) != OGRERR_NONE)
        {
            throw InvalidArgument(FailureMessage("Cannot import EPSG coordinate reference system"));
        }
        reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return Projection(std::make_shared<Impl>(MakeSpatialReference(reference)));
    }

    Projection Projection::FromWkt(const std::string_view wkt)
    {
        GdalEnvironment::Initialize();
        const auto parsed = TryFromWkt(wkt);
        if (!parsed.has_value())
        {
            throw InvalidArgument(FailureMessage("Cannot import WKT coordinate reference system"));
        }
        return *parsed;
    }

    Projection Projection::FromProj4(const std::string_view proj4)
    {
        GdalEnvironment::Initialize();
        const auto parsed = TryFromProj4(proj4);
        if (!parsed.has_value())
        {
            throw InvalidArgument(FailureMessage("Cannot import PROJ4 coordinate reference system"));
        }
        return *parsed;
    }

    std::optional<Projection> Projection::TryFromWkt(const std::string_view wkt)
    {
        GdalEnvironment::Initialize();
        if (wkt.empty())
        {
            return std::nullopt;
        }

        const std::string input(wkt);
        const char* text = input.c_str();
        const GdalEnvironment::QuietScope quiet;
        OGRSpatialReference reference;
        CPLErrorReset();
        if (reference.importFromWkt(&text) != OGRERR_NONE ||
            reference.Validate() != OGRERR_NONE)
        {
            return std::nullopt;
        }
        reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return Projection(std::make_shared<Impl>(MakeSpatialReference(reference)));
    }

    std::optional<Projection> Projection::TryFromProj4(const std::string_view proj4)
    {
        GdalEnvironment::Initialize();
        if (proj4.empty())
        {
            return std::nullopt;
        }

        const std::string input(proj4);
        const GdalEnvironment::QuietScope quiet;
        OGRSpatialReference reference;
        CPLErrorReset();
        if (reference.importFromProj4(input.c_str()) != OGRERR_NONE ||
            reference.Validate() != OGRERR_NONE)
        {
            return std::nullopt;
        }
        reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        return Projection(std::make_shared<Impl>(MakeSpatialReference(reference)));
    }

    std::optional<int> Projection::Epsg() const
    {
        return _impl != nullptr ? _impl->ResolveEpsg() : std::nullopt;
    }

    std::string Projection::Name() const
    {
        if (_impl == nullptr)
        {
            return {};
        }
        const char* name = _impl->_reference->GetName();
        return name != nullptr ? name : "";
    }

    bool Projection::IsGeographic() const
    {
        return _impl != nullptr && _impl->_reference->IsGeographic() == TRUE;
    }

    bool Projection::IsProjected() const
    {
        return _impl != nullptr && _impl->_reference->IsProjected() == TRUE;
    }

    bool Projection::IsEmpty() const noexcept
    {
        return _impl == nullptr;
    }

    std::string Projection::ExportToWkt() const
    {
        if (_impl == nullptr)
        {
            return {};
        }

        char* text = nullptr;
        CPLErrorReset();
        if (_impl->_reference->exportToWkt(&text) != OGRERR_NONE)
        {
            CPLFree(text);
            throw CoreError(FailureMessage("Cannot export coordinate reference system to WKT"));
        }
        return ExportedString(text);
    }

    std::string Projection::ExportToProj4() const
    {
        if (_impl == nullptr)
        {
            return {};
        }

        char* text = nullptr;
        CPLErrorReset();
        if (_impl->_reference->exportToProj4(&text) != OGRERR_NONE)
        {
            CPLFree(text);
            throw CoreError(FailureMessage("Cannot export coordinate reference system to PROJ4"));
        }
        return ExportedString(text);
    }

    bool Projection::IsSame(const Projection& other) const
    {
        if (IsEmpty() || other.IsEmpty())
        {
            return IsEmpty() && other.IsEmpty();
        }

        const std::optional<int> code = Epsg();
        const std::optional<int> otherCode = other.Epsg();
        if (code.has_value() && otherCode.has_value())
        {
            return *code == *otherCode;
        }
        if (_impl->_reference->IsSame(other._impl->_reference.get()) == TRUE)
        {
            return true;
        }

        const std::string name = NormalizedName(_impl->_reference->GetName());
        const std::string otherName =
            NormalizedName(other._impl->_reference->GetName());
        if (!name.empty() && name == otherName)
        {
            return true;
        }

        const std::string projection =
            NormalizedName(_impl->_reference->GetAttrValue("PROJECTION"));
        const std::string otherProjection =
            NormalizedName(other._impl->_reference->GetAttrValue("PROJECTION"));
        return !projection.empty() && projection == otherProjection;
    }

    ProjectionTransform Projection::TransformTo(const Projection& target) const
    {
        if (IsEmpty() || target.IsEmpty())
        {
            throw InvalidArgument("Cannot transform to or from an empty projection");
        }

        auto sourceReference = _impl->_reference->Clone();
        auto targetReference = target._impl->_reference->Clone();
        std::unique_ptr<OGRSpatialReference> ownedSource(sourceReference);
        std::unique_ptr<OGRSpatialReference> ownedTarget(targetReference);
        if (ownedSource == nullptr || ownedTarget == nullptr)
        {
            throw CoreError("Cannot copy coordinate reference system for transformation");
        }
        ownedSource->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        ownedTarget->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        CPLErrorReset();
        OGRCoordinateTransformation* transformation =
            OGRCreateCoordinateTransformation(ownedSource.get(), ownedTarget.get());
        if (transformation == nullptr)
        {
            throw CoreError(FailureMessage(
                "Cannot create coordinate reference system transformation"));
        }
        return ProjectionTransform(
            std::make_shared<ProjectionTransform::Impl>(transformation));
    }
}
