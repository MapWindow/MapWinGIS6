// SPDX-License-Identifier: MPL-2.0

/**
 * @file FrameBuffer.cpp
 * @brief Implements the offscreen frame buffer.
 */
#include "FrameBuffer.h"

namespace mwg::activex
{
    namespace
    {
        [[nodiscard]] HBITMAP CreateBufferBitmap(
            const HDC deviceContext,
            const int width,
            const int height) noexcept
        {
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = width;
            bitmapInfo.bmiHeader.biHeight = -height; // top-down rows
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            return CreateDIBSection(deviceContext, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        }
    }

    FrameBuffer::~FrameBuffer()
    {
        Release();
    }

    bool FrameBuffer::Resize(const int width, const int height) noexcept
    {
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        if (IsValid() && width == _width && height == _height)
        {
            return true;
        }

        // Allocate the replacement before releasing the current buffer so a
        // failed resize keeps a presentable buffer.
        HDC memoryDc = CreateCompatibleDC(nullptr);
        if (memoryDc == nullptr)
        {
            ::OutputDebugStringW(L"MapWinGIS: could not create the frame buffer device context.\n");
            return false;
        }

        const HBITMAP bitmap = CreateBufferBitmap(memoryDc, width, height);
        if (bitmap == nullptr)
        {
            ::OutputDebugStringW(L"MapWinGIS: could not allocate the frame buffer bitmap.\n");
            DeleteDC(memoryDc);
            return false;
        }

        const HBITMAP previousBitmap = static_cast<HBITMAP>(SelectObject(memoryDc, bitmap));
        if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR)
        {
            ::OutputDebugStringW(L"MapWinGIS: could not select the frame buffer bitmap.\n");
            DeleteObject(bitmap);
            DeleteDC(memoryDc);
            return false;
        }

        Release();
        _memoryDc = memoryDc;
        _bitmap = bitmap;
        _previousBitmap = previousBitmap;
        _width = width;
        _height = height;
        return true;
    }

    void FrameBuffer::Release() noexcept
    {
        if (_memoryDc != nullptr)
        {
            if (_previousBitmap != nullptr && _previousBitmap != HGDI_ERROR)
            {
                SelectObject(_memoryDc, _previousBitmap);
                _previousBitmap = nullptr;
            }
            DeleteDC(_memoryDc);
            _memoryDc = nullptr;
        }

        if (_bitmap != nullptr)
        {
            DeleteObject(_bitmap);
            _bitmap = nullptr;
        }

        _width = 0;
        _height = 0;
    }
}
