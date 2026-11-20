// SPDX-License-Identifier: MPL-2.0

/**
 * @file PerformanceDiagnosticsTests.cpp
 * @brief Tests opt-in timing and counter aggregation for vector pipelines.
 */
#include <gtest/gtest.h>

#include <array>
#include <thread>
#include <vector>

#include "TestData.h"
#include "mapwingis/core/Error.h"
#include "mapwingis/diagnostics/PerformanceDiagnostics.h"
#include "mapwingis/gdal/GdalEnvironment.h"
#include "mapwingis/map/MapView.h"
#include "mapwingis/rendering/PolygonTriangulation.h"
#include "mapwingis/rendering/VectorRenderScene.h"
#include "mapwingis/vector/VectorDataset.h"

using namespace mwg;
using namespace mwg::diagnostics;
using namespace mwg::test;

namespace
{
    class PerformanceDiagnosticsTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            PerformanceDiagnostics::SetEnabled(false);
            PerformanceDiagnostics::Reset();
            PerformanceDiagnostics::SetEnabled(true);
        }

        void TearDown() override
        {
            PerformanceDiagnostics::SetEnabled(false);
            PerformanceDiagnostics::Reset();
        }
    };
}

TEST_F(PerformanceDiagnosticsTest, AggregatesScopesAndCountersFromConcurrentThreads)
{
    constexpr int kThreadCount = 4;
    constexpr int kIncrementsPerThread = 100;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (int index = 0; index < kThreadCount; ++index)
    {
        threads.emplace_back([]()
        {
            const PerformanceScope scope(
                PerformanceOperation::OpenGlGeometrySubmission);
            for (int increment = 0; increment < kIncrementsPerThread; ++increment)
            {
                PerformanceDiagnostics::IncrementCounter(
                    PerformanceCounter::FeaturesProcessed);
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(
        snapshot.Timing(PerformanceOperation::OpenGlGeometrySubmission).callCount,
        kThreadCount);
    EXPECT_GE(
        snapshot.Timing(PerformanceOperation::OpenGlGeometrySubmission).durationNanoseconds,
        0);
    EXPECT_EQ(
        snapshot.Counter(PerformanceCounter::FeaturesProcessed),
        kThreadCount * kIncrementsPerThread);
}

TEST_F(PerformanceDiagnosticsTest, RecordsRequestedFillPreparationStages)
{
    TempDirectory directory;
    constexpr std::array<Extent, 1> rectangles{{{0, 0, 10, 10}}};
    const std::filesystem::path path =
        MakeRectanglesDataset(directory.Path(), "diagnostics.gpkg", rectangles);

    PerformanceDiagnostics::Reset();
    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(path);
    const rendering::VectorRenderScene scene =
        rendering::CreateVectorRenderScene(
            *dataset,
            rendering::VectorRenderSceneOptions{.prepareFillMeshes = true});

    ASSERT_EQ(scene.polygons.size(), 1U);
    ASSERT_EQ(scene.fillMeshes.size(), 1U);
    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::DatasetOpen).callCount, 1);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::LayerMetadataRead).callCount, 1);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::FeatureGeometryRead).callCount, 2);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::GeometryConversion).callCount, 1);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::SceneBuild).callCount, 1);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::GeometryValidation).callCount, 1);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::PolygonTriangulation).callCount, 1);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::FeaturesProcessed), 1);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::GeometriesLoaded), 1);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::PolygonsTriangulated), 1);
    EXPECT_GT(snapshot.Counter(PerformanceCounter::TrianglesGenerated), 0);
}

TEST_F(PerformanceDiagnosticsTest, DoesNotTriangulateScenesByDefault)
{
    TempDirectory directory;
    constexpr std::array<Extent, 1> rectangles{{{0, 0, 10, 10}}};
    const std::filesystem::path path =
        MakeRectanglesDataset(directory.Path(), "outlines.gpkg", rectangles);
    const std::unique_ptr<VectorDataset> dataset = VectorDataset::Open(path);

    PerformanceDiagnostics::Reset();
    const rendering::VectorRenderScene scene =
        rendering::CreateVectorRenderScene(*dataset);

    EXPECT_EQ(scene.polygons.size(), 1U);
    EXPECT_TRUE(scene.fillMeshes.empty());
    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::GeometryValidation).callCount, 0);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::PolygonTriangulation).callCount, 0);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::PolygonsTriangulated), 0);
}

TEST_F(PerformanceDiagnosticsTest, RecordsSceneCacheMissesAndHits)
{
    TempDirectory directory;
    constexpr std::array<Extent, 1> rectangles{{{0, 0, 10, 10}}};
    const std::filesystem::path path =
        MakeRectanglesDataset(directory.Path(), "cache.gpkg", rectangles);
    MapView view;
    (void)view.AddLayer(VectorDataset::Open(path));

    PerformanceDiagnostics::Reset();
    ASSERT_FALSE(view.Scene().layers.empty());
    ASSERT_FALSE(view.Scene().layers.empty());

    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::SceneCacheMisses), 1);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::SceneCacheHits), 1);
}

TEST_F(PerformanceDiagnosticsTest, RecordsInvalidAndFailedTriangulation)
{
    const rendering::Polygon bowTie{{
        {0, 0}, {2, 2}, {0, 2}, {2, 0}, {0, 0}}};

    {
        const GdalEnvironment::QuietScope quiet;
        EXPECT_THROW(
            (void)rendering::TriangulatePolygon(bowTie),
            CoreError);
    }

    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::InvalidGeometries), 1);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::TriangulationFailures), 1);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::PolygonsTriangulated), 0);
}

TEST(PerformanceDiagnosticsDisabled, DoesNotRecordNewMeasurements)
{
    PerformanceDiagnostics::SetEnabled(false);
    PerformanceDiagnostics::Reset();
    {
        const PerformanceScope scope(PerformanceOperation::SceneBuild);
        PerformanceDiagnostics::IncrementCounter(
            PerformanceCounter::FeaturesProcessed);
    }
    PerformanceDiagnostics::RecordDuration(
        PerformanceOperation::FrameRender,
        std::chrono::milliseconds(1));

    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::SceneBuild).callCount, 0);
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::FrameRender).callCount, 0);
    EXPECT_EQ(snapshot.Counter(PerformanceCounter::FeaturesProcessed), 0);
}

TEST_F(PerformanceDiagnosticsTest, RecordsScopeThatStartsWhileEnabled)
{
    {
        const PerformanceScope scope(PerformanceOperation::SceneBuild);
        PerformanceDiagnostics::SetEnabled(false);
    }

    const PerformanceSnapshot snapshot = PerformanceDiagnostics::Snapshot();
    EXPECT_EQ(snapshot.Timing(PerformanceOperation::SceneBuild).callCount, 1);
}
