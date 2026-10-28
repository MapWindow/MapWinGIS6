// SPDX-License-Identifier: MPL-2.0

/**
 * @file OpenGlVectorLayerRenderer.h
 * @brief Declares the WGL-backed polygon scene renderer.
 *
 * The backend owns a reusable OpenGL context, created on a hidden window that
 * is never presented, and renders into a framebuffer object before copying
 * the pixels into the host device context.
 */
#pragma once

#include <Windows.h>

#include <memory>
#include <string_view>
#include <vector>

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    class OpenGlVectorLayerRenderer final : public IVectorRenderBackend
    {
    public:
        OpenGlVectorLayerRenderer() noexcept;
        ~OpenGlVectorLayerRenderer() override;

        OpenGlVectorLayerRenderer(const OpenGlVectorLayerRenderer&) = delete;
        OpenGlVectorLayerRenderer& operator=(const OpenGlVectorLayerRenderer&) = delete;

        /**
         * @brief Checks a target against detected size limits and the staging budget.
         *
         * @p maximumWidth and @p maximumHeight are the effective per-axis
         * limits: the smaller of the renderbuffer size and the viewport
         * dimensions, because `glViewport` silently clamps larger targets.
         */
        [[nodiscard]] static bool IsTargetWithinLimits(
            int width,
            int height,
            int maximumWidth,
            int maximumHeight,
            std::wstring_view& rejectionReason) noexcept;

        /**
         * @brief Draws a scene of simple convex single-ring polygons.
         *
         * Returns S_FALSE without drawing when the scene contains unsupported
         * geometry or when no OpenGL context with framebuffer-object support
         * is available, so callers can fall back to another backend.
         */
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::VectorRenderScene& scene) noexcept override;

        [[nodiscard]] std::wstring_view GetLastFailureReason() const noexcept
        {
            return _lastFailureReason;
        }

        /**
         * @brief Test hook: destroys the hidden render-target window while the
         * backend still holds its handles.
         *
         * The next draw then fails to activate the context, as after a lost
         * device context, which lets tests verify recreation on later draws.
         * Not for production use.
         */
        void SimulateContextLossForTesting() noexcept;

    private:
        struct FramebufferFunctions;

        [[nodiscard]] bool Initialize() noexcept;
        [[nodiscard]] bool InitializeFramebuffer() noexcept;
        [[nodiscard]] bool ResizeFramebuffer(int width, int height) noexcept;
        void Release() noexcept;

        HWND _window{};
        HDC _deviceContext{};
        HGLRC _renderingContext{};
        std::unique_ptr<FramebufferFunctions> _framebufferFunctions;
        unsigned int _framebuffer{};
        unsigned int _renderbuffer{};
        int _framebufferWidth{};
        int _framebufferHeight{};
        int _maximumTargetWidth{};
        int _maximumTargetHeight{};
        bool _initializationFailed = false;
        bool _contextInvalid = false;
        std::wstring_view _lastFailureReason =
            L"OpenGL was not selected for this frame.";
        std::wstring_view _initializationFailureReason;
        std::vector<unsigned char> _rgbaPixels;
    };
}
