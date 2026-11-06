// SPDX-License-Identifier: MPL-2.0

/**
 * @file FrameBufferTests.cpp
 * @brief Tests the offscreen frame buffer used for flicker-free painting.
 *
 * Covers allocation, resizing, reuse for equal sizes, and pixel round-trips
 * through the backed device context.
 */
#include <Windows.h>

#include <gtest/gtest.h>

#include "FrameBuffer.h"

TEST(FrameBuffer, IsInvalidBeforeResize)
{
    mwg::activex::FrameBuffer buffer;
    EXPECT_FALSE(buffer.IsValid());
    EXPECT_EQ(buffer.Get(), nullptr);
    EXPECT_EQ(buffer.Width(), 0);
    EXPECT_EQ(buffer.Height(), 0);
}

TEST(FrameBuffer, RejectsNonPositiveSizes)
{
    mwg::activex::FrameBuffer buffer;
    EXPECT_FALSE(buffer.Resize(0, 10));
    EXPECT_FALSE(buffer.Resize(10, 0));
    EXPECT_FALSE(buffer.Resize(-1, 10));
    EXPECT_FALSE(buffer.Resize(10, -1));
    EXPECT_FALSE(buffer.IsValid());
}

TEST(FrameBuffer, AllocatesRequestedSize)
{
    mwg::activex::FrameBuffer buffer;
    ASSERT_TRUE(buffer.Resize(120, 80));
    EXPECT_TRUE(buffer.IsValid());
    EXPECT_NE(buffer.Get(), nullptr);
    EXPECT_EQ(buffer.Width(), 120);
    EXPECT_EQ(buffer.Height(), 80);
}

TEST(FrameBuffer, KeepsBufferForSameSize)
{
    mwg::activex::FrameBuffer buffer;
    ASSERT_TRUE(buffer.Resize(64, 48));
    const HDC deviceContext = buffer.Get();

    ASSERT_TRUE(buffer.Resize(64, 48));
    EXPECT_EQ(buffer.Get(), deviceContext);
    EXPECT_EQ(buffer.Width(), 64);
    EXPECT_EQ(buffer.Height(), 48);
}

TEST(FrameBuffer, ResizesToNewDimensions)
{
    mwg::activex::FrameBuffer buffer;
    ASSERT_TRUE(buffer.Resize(64, 48));
    ASSERT_TRUE(buffer.Resize(100, 60));
    EXPECT_TRUE(buffer.IsValid());
    EXPECT_EQ(buffer.Width(), 100);
    EXPECT_EQ(buffer.Height(), 60);
}

TEST(FrameBuffer, FailedResizeKeepsPreviousBuffer)
{
    mwg::activex::FrameBuffer buffer;
    ASSERT_TRUE(buffer.Resize(64, 48));
    const HDC deviceContext = buffer.Get();

    EXPECT_FALSE(buffer.Resize(0, 100));
    EXPECT_TRUE(buffer.IsValid());
    EXPECT_EQ(buffer.Get(), deviceContext);
    EXPECT_EQ(buffer.Width(), 64);
    EXPECT_EQ(buffer.Height(), 48);
}

TEST(FrameBuffer, DrawnContentRoundTripsThroughTheBitmap)
{
    mwg::activex::FrameBuffer buffer;
    ASSERT_TRUE(buffer.Resize(32, 32));

    const RECT fill{0, 0, 32, 32};
    ASSERT_NE(
        ::FillRect(buffer.Get(), &fill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))),
        0);
    EXPECT_EQ(::GetPixel(buffer.Get(), 10, 10), RGB(0, 0, 0));

    // A resize replaces the bitmap, so the new buffer starts uninitialized.
    ASSERT_TRUE(buffer.Resize(40, 40));
    ASSERT_NE(
        ::FillRect(buffer.Get(), &fill, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH))),
        0);
    EXPECT_EQ(::GetPixel(buffer.Get(), 5, 5), RGB(255, 255, 255));
}

TEST(FrameBuffer, ReleaseResetsStateAndAllowsReuse)
{
    mwg::activex::FrameBuffer buffer;
    ASSERT_TRUE(buffer.Resize(10, 10));

    buffer.Release();
    EXPECT_FALSE(buffer.IsValid());
    EXPECT_EQ(buffer.Get(), nullptr);
    EXPECT_EQ(buffer.Width(), 0);
    EXPECT_EQ(buffer.Height(), 0);

    ASSERT_TRUE(buffer.Resize(10, 10));
    EXPECT_TRUE(buffer.IsValid());
}
