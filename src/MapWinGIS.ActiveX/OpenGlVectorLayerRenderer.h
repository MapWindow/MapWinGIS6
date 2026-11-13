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

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "VectorRenderBackend.h"

namespace mwg::activex
{
    namespace testing
    {
        class OpenGlVectorLayerRendererTestAccess;
    }

    class OpenGlVectorLayerRenderer final : public IVectorRenderBackend
    {
    public:
        OpenGlVectorLayerRenderer() noexcept;
        ~OpenGlVectorLayerRenderer() override;

        OpenGlVectorLayerRenderer(const OpenGlVectorLayerRenderer&) = delete;
        OpenGlVectorLayerRenderer& operator=(const OpenGlVectorLayerRenderer&) = delete;

        /**
         * @brief Returns why a target exceeds detected size or staging limits.
         *
         * @p maximumWidth and @p maximumHeight are the effective per-axis
         * limits: the smaller of the renderbuffer size and the viewport
         * dimensions, because `glViewport` silently clamps larger targets.
         * Returns an empty optional when the target is acceptable.
         */
        [[nodiscard]] static std::optional<std::wstring_view> GetTargetRejectionReason(
            int width,
            int height,
            int maximumWidth,
            int maximumHeight) noexcept;

        /**
         * @brief Draws a scene of triangulated polygons.
         *
         * Fills every polygon from the Core-prepared triangle mesh and draws
         * its outlines from the original rings before advancing to the next
         * polygon, matching the GDI backend's painter order within a layer.
         * Draws all layers into one frame, bottom first. Returns S_FALSE
         * without drawing when any polygon lacks a usable fill mesh or when
         * no OpenGL context with framebuffer-object support is available, so
         * callers can fall back to another backend for the whole frame.
         */
        [[nodiscard]] HRESULT Draw(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport) noexcept override;

        [[nodiscard]] std::wstring_view GetLastFailureReason() const noexcept
        {
            return _lastFailureReason;
        }

    private:
        friend class testing::OpenGlVectorLayerRendererTestAccess;

        /// True when every polygon of the scene has a usable triangulated
        /// fill mesh and finite coordinates. Exposed for tests so geometry
        /// support decisions can be checked without an OpenGL context.
        [[nodiscard]] static bool IsSceneSupported(
            const rendering::MapRenderScene& scene) noexcept;

        void SimulateContextLoss() noexcept;

        struct FramebufferFunctions;
        struct Target final
        {
            int width = 0;
            int height = 0;
            std::size_t pixelWidth = 0;
            std::size_t pixelHeight = 0;
        };
        struct TargetValidation final
        {
            HRESULT result = E_INVALIDARG;
            std::optional<Target> target;
        };

        [[nodiscard]] bool Initialize() noexcept;
        [[nodiscard]] bool InitializeFramebuffer() noexcept;
        [[nodiscard]] bool ResizeFramebuffer(int width, int height) noexcept;
        void Release() noexcept;
        [[nodiscard]] TargetValidation ValidateTarget(
            HDC deviceContext,
            const RECT& bounds,
            const rendering::MapRenderScene& scene,
            const Viewport& viewport) noexcept;
        [[nodiscard]] bool EnsureStaging(std::size_t stagingBytes) noexcept;
        [[nodiscard]] HRESULT RenderScene(
            const rendering::MapRenderScene& scene,
            const Viewport& viewport,
            const Target& target) noexcept;
        [[nodiscard]] HRESULT CopyToDeviceContext(
            HDC deviceContext,
            const RECT& bounds,
            const Target& target) noexcept;

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
