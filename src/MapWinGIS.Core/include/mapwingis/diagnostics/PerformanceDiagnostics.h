// SPDX-License-Identifier: MPL-2.0

/**
 * @file PerformanceDiagnostics.h
 * @brief Declares opt-in, process-wide performance measurements.
 *
 * Provides thread-safe aggregate timings and counters for the vector loading
 * and rendering pipeline. Measurements are disabled by default and are kept
 * in Core so both Core and ActiveX can contribute to the same diagnostic run.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "mapwingis/core/Api.h"

namespace mwg::diagnostics
{
    enum class PerformanceOperation : int
    {
        DatasetOpen,
        LayerMetadataRead,
        FeatureGeometryRead,
        GeometryConversion,
        SceneBuild,
        GeometryValidation,
        PolygonTriangulation,
        OpenGlGeometrySubmission,
        FrameRender,
        FramebufferReadback,
        OpenGlGpuBufferUpload,
        OpenGlContextInitialization,
        Count,
    };

    enum class PerformanceCounter : int
    {
        FeaturesProcessed,
        GeometriesLoaded,
        PolygonsTriangulated,
        TrianglesGenerated,
        InvalidGeometries,
        TriangulationFailures,
        FeaturesSkippedOutsideViewport,
        SceneCacheHits,
        SceneCacheMisses,
        Count,
    };

    constexpr std::size_t PerformanceOperationCount =
        static_cast<std::size_t>(PerformanceOperation::Count);
    constexpr std::size_t PerformanceCounterCount =
        static_cast<std::size_t>(PerformanceCounter::Count);

    struct MWG_CORE_API PerformanceTiming
    {
        std::int64_t callCount = 0;
        std::int64_t durationNanoseconds = 0;

        [[nodiscard]] double DurationMilliseconds() const noexcept;
    };

    struct MWG_CORE_API PerformanceSnapshot
    {
        std::array<PerformanceTiming, PerformanceOperationCount> timings{};
        std::array<std::int64_t, PerformanceCounterCount> counters{};

        [[nodiscard]] const PerformanceTiming& Timing(PerformanceOperation operation) const noexcept;
        [[nodiscard]] std::int64_t Counter(PerformanceCounter counter) const noexcept;
    };

    /// Process-wide aggregate collector for opt-in performance diagnostics.
    ///
    /// Timings and counters are safe to record from concurrent threads.
    /// Disabled recording has only an atomic enablement check at each
    /// instrumentation point. Reset before starting a diagnostic run and
    /// do not reset while measured work is active; otherwise a long-running
    /// scope may add its completion to the new run.
    class MWG_CORE_API PerformanceDiagnostics
    {
    public:
        [[nodiscard]] static bool IsEnabled() noexcept;
        static void SetEnabled(bool enabled) noexcept;
        static void Reset() noexcept;
        [[nodiscard]] static PerformanceSnapshot Snapshot() noexcept;

        static void IncrementCounter(
            PerformanceCounter counter,
            std::int64_t amount = 1) noexcept;
        static void RecordDuration(
            PerformanceOperation operation,
            std::chrono::nanoseconds duration) noexcept;
    };

    /// Measures one operation with std::chrono::steady_clock when diagnostics
    /// are enabled. A scope that started enabled records on destruction even
    /// if diagnostics were disabled meanwhile.
    class MWG_CORE_API PerformanceScope final
    {
    public:
        explicit PerformanceScope(PerformanceOperation operation) noexcept;
        ~PerformanceScope();

        PerformanceScope(const PerformanceScope&) = delete;
        PerformanceScope& operator=(const PerformanceScope&) = delete;

    private:
        PerformanceOperation _operation;
        std::chrono::steady_clock::time_point _start;
        bool _enabled = false;
    };
}
