// SPDX-License-Identifier: MPL-2.0

/**
 * @file PerformanceDiagnostics.cpp
 * @brief Implements aggregate vector-pipeline performance diagnostics.
 *
 * Stores timings and counters in atomics so Core and ActiveX workers can
 * record measurements without serializing the pipeline or allocating while
 * rendering.
 */
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"

#include <atomic>

namespace mwg::diagnostics
{
    namespace
    {
        struct AtomicPerformanceState
        {
            std::atomic_bool enabled = false;
            std::array<std::atomic_int64_t, PerformanceOperationCount> callCounts{};
            std::array<std::atomic_int64_t, PerformanceOperationCount> durations{};
            std::array<std::atomic_int64_t, PerformanceCounterCount> counters{};
        };

        AtomicPerformanceState& State()
        {
            static AtomicPerformanceState state;
            return state;
        }

        [[nodiscard]] std::size_t OperationIndex(
            const PerformanceOperation operation) noexcept
        {
            return static_cast<std::size_t>(operation);
        }

        [[nodiscard]] std::size_t CounterIndex(
            const PerformanceCounter counter) noexcept
        {
            return static_cast<std::size_t>(counter);
        }
    }

    double PerformanceTiming::DurationMilliseconds() const noexcept
    {
        return static_cast<double>(durationNanoseconds) / 1000000.0;
    }

    const PerformanceTiming& PerformanceSnapshot::Timing(
        const PerformanceOperation operation) const noexcept
    {
        return timings[OperationIndex(operation)];
    }

    std::int64_t PerformanceSnapshot::Counter(
        const PerformanceCounter counter) const noexcept
    {
        return counters[CounterIndex(counter)];
    }

    bool PerformanceDiagnostics::IsEnabled() noexcept
    {
        return State().enabled.load(std::memory_order_relaxed);
    }

    void PerformanceDiagnostics::SetEnabled(const bool enabled) noexcept
    {
        State().enabled.store(enabled, std::memory_order_relaxed);
    }

    void PerformanceDiagnostics::Reset() noexcept
    {
        AtomicPerformanceState& state = State();
        for (std::atomic_int64_t& value : state.callCounts)
        {
            value.store(0, std::memory_order_relaxed);
        }
        for (std::atomic_int64_t& value : state.durations)
        {
            value.store(0, std::memory_order_relaxed);
        }
        for (std::atomic_int64_t& value : state.counters)
        {
            value.store(0, std::memory_order_relaxed);
        }
    }

    PerformanceSnapshot PerformanceDiagnostics::Snapshot() noexcept
    {
        const AtomicPerformanceState& state = State();
        PerformanceSnapshot result;
        for (std::size_t index = 0; index < PerformanceOperationCount; ++index)
        {
            result.timings[index] = {
                state.callCounts[index].load(std::memory_order_relaxed),
                state.durations[index].load(std::memory_order_relaxed)};
        }
        for (std::size_t index = 0; index < PerformanceCounterCount; ++index)
        {
            result.counters[index] =
                state.counters[index].load(std::memory_order_relaxed);
        }
        return result;
    }

    void PerformanceDiagnostics::IncrementCounter(
        const PerformanceCounter counter,
        const std::int64_t amount) noexcept
    {
        if (IsEnabled())
        {
            State().counters[CounterIndex(counter)].fetch_add(
                amount, std::memory_order_relaxed);
        }
    }

    void PerformanceDiagnostics::RecordDuration(
        const PerformanceOperation operation,
        const std::chrono::nanoseconds duration) noexcept
    {
        AtomicPerformanceState& state = State();
        state.callCounts[OperationIndex(operation)].fetch_add(
            1, std::memory_order_relaxed);
        state.durations[OperationIndex(operation)].fetch_add(
            duration.count(), std::memory_order_relaxed);
    }

    PerformanceScope::PerformanceScope(
        const PerformanceOperation operation) noexcept
        : _operation(operation),
          _enabled(PerformanceDiagnostics::IsEnabled())
    {
        if (_enabled)
        {
            _start = std::chrono::steady_clock::now();
        }
    }

    PerformanceScope::~PerformanceScope()
    {
        if (_enabled)
        {
            PerformanceDiagnostics::RecordDuration(
                _operation,
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - _start));
        }
    }
}
