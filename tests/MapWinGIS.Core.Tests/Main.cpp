// SPDX-License-Identifier: MPL-2.0

/**
 * @file Main.cpp
 * @brief Provides the GoogleTest entry point for the core test executable.
 *
 * Defines main locally so the test target does not link GoogleTest's main
 * libraries, which are auto-linked by the configured vcpkg setup.
 */
#include <gtest/gtest.h>

// Defined here rather than linking gtest_main, so that both gtest_main.lib and
// gmock_main.lib stay out of the link (vcpkg auto-links every library).
int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
