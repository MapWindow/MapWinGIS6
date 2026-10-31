// SPDX-License-Identifier: MPL-2.0

/**
 * @file CoordinateTransformation.h
 * @brief Provides private OGR coordinate transformations for map data.
 */
#pragma once

#include <memory>
#include <string>

#include <ogr_spatialref.h>

#include "mapwingis/core/Error.h"

namespace mwg::detail
{
    class CoordinateTransformation final
    {
    public:
        CoordinateTransformation(const std::string& sourceWkt, const std::string& targetWkt)
        {
            if (sourceWkt.empty() || targetWkt.empty())
            {
                if (sourceWkt != targetWkt)
                {
                    throw InvalidArgument(
                        "Layers with and without a coordinate reference system cannot be combined");
                }
                return;
            }

            _source = std::make_unique<OGRSpatialReference>();
            _target = std::make_unique<OGRSpatialReference>();
            _source->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            _target->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            if (_source->SetFromUserInput(sourceWkt.c_str()) != OGRERR_NONE ||
                _target->SetFromUserInput(targetWkt.c_str()) != OGRERR_NONE)
            {
                throw InvalidArgument("Layer coordinate reference system is invalid");
            }
            if (_source->IsSame(_target.get()))
            {
                return;
            }

            _transform.reset(OGRCreateCoordinateTransformation(_source.get(), _target.get()));
            if (_transform == nullptr)
            {
                throw CoreError("Cannot transform between the layer coordinate reference systems");
            }
        }

        [[nodiscard]] bool IsNeeded() const noexcept
        {
            return _transform != nullptr;
        }

        [[nodiscard]] bool Apply(double& x, double& y) const noexcept
        {
            if (_transform == nullptr)
            {
                return true;
            }
            return _transform->Transform(1, &x, &y) != FALSE;
        }

        [[nodiscard]] OGRCoordinateTransformation* Get() const noexcept
        {
            return _transform.get();
        }

    private:
        struct DestroyTransform
        {
            void operator()(OGRCoordinateTransformation* value) const noexcept
            {
                if (value != nullptr)
                {
                    OCTDestroyCoordinateTransformation(value);
                }
            }
        };

        std::unique_ptr<OGRSpatialReference> _source;
        std::unique_ptr<OGRSpatialReference> _target;
        std::unique_ptr<OGRCoordinateTransformation, DestroyTransform> _transform;
    };
}
