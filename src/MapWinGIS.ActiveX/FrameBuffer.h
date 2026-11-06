// SPDX-License-Identifier: MPL-2.0

/**
 * @file FrameBuffer.h
 * @brief Owns a resizable offscreen device context for flicker-free painting.
 *
 * Backs a memory device context with a 32-bit DIB section so a complete
 * frame can be composed offscreen and presented to the visible surface with
 * a single blit. This is host-rendering infrastructure owned by the OCX
 * layer; it holds no scene, dataset, or view state.
 */
#pragma once

#include <Windows.h>

namespace mwg::activex
{
    class FrameBuffer final
    {
    public:
        FrameBuffer() noexcept = default;
        ~FrameBuffer();

        FrameBuffer(const FrameBuffer&) = delete;
        FrameBuffer& operator=(const FrameBuffer&) = delete;

        [[nodiscard]] HDC Get() const noexcept { return _memoryDc; }
        [[nodiscard]] int Width() const noexcept { return _width; }
        [[nodiscard]] int Height() const noexcept { return _height; }

        /// True when Get returns a device context with the bitmap selected.
        [[nodiscard]] bool IsValid() const noexcept
        {
            return _memoryDc != nullptr && _bitmap != nullptr;
        }

        /// Recreates the buffer when the size changed and keeps the current
        /// bitmap for an equal size. Returns false for non-positive sizes
        /// and allocation failures; a failed resize leaves the previous
        /// buffer intact so it can still be presented.
        [[nodiscard]] bool Resize(int width, int height) noexcept;

        /// Releases the device context and bitmap; the next Resize
        /// recreates them.
        void Release() noexcept;

    private:
        HDC _memoryDc = nullptr;
        HBITMAP _bitmap = nullptr;
        HBITMAP _previousBitmap = nullptr;
        int _width = 0;
        int _height = 0;
    };
}
