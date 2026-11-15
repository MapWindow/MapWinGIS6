// SPDX-License-Identifier: MPL-2.0

/**
 * @file MapWinGISControlTestAccess.h
 * @brief Inspects control buffers and stages drag and pending-fit states.
 *
 * Tests use the native implementation behind the OCX's class factory.
 * These helpers do not add COM interfaces or change the control's layout.
 */
#pragma once

#include "MapWinGISControl.h"

namespace mwg::activex::testing
{
    class MapWinGISControlTestAccess final
    {
    public:
        [[nodiscard]] static CMapWinGISControl& Control(IMapWinGISControl3* control) noexcept
        {
            return *static_cast<CMapWinGISControl*>(control);
        }

        [[nodiscard]] static bool CacheValid(const CMapWinGISControl& control) noexcept
        {
            return control._layerCacheValid;
        }

        [[nodiscard]] static MapPoint CachedCenter(const CMapWinGISControl& control)
        {
            return control._layerCacheViewport.Center().value();
        }

        [[nodiscard]] static const FrameBuffer& Frame(const CMapWinGISControl& control) noexcept
        {
            return control._frameBuffer;
        }

        [[nodiscard]] static const FrameBuffer& LayerCache(const CMapWinGISControl& control) noexcept
        {
            return control._layerCache;
        }

        [[nodiscard]] static std::size_t StagingCapacity(const CMapWinGISControl& control) noexcept
        {
            return control._renderer._openGlRenderer._pixelBuffer.capacity();
        }

        [[nodiscard]] static double Resolution(const CMapWinGISControl& control)
        {
            return control._view.GetViewport().Resolution().value();
        }

        static bool PanDuringDrag(CMapWinGISControl& control, const double pixels)
        {
            return control._view.Pan(pixels * Resolution(control), 0.0);
        }

        static void StagePendingFit(CMapWinGISControl& control, std::shared_ptr<VectorDataset> dataset)
        {
            (void)control._view.Resize(0, 0);
            (void)control._view.FitToMap();
            (void)control._view.AddLayer(std::move(dataset));
        }
    };
}
