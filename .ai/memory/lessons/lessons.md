# Verified development lessons

## ATL registry scripts are not C++ source

The ATL registrar rejected `//` comments in `MapWinGISControl.rgs` with
`DISP_E_EXCEPTION` during per-user registration. Keep explanatory comments
outside the `.rgs` file and verify registration, not just resource compilation.
This was reproduced using process-local registry overrides in PR #77.

## Dataset and runner release must stay lock-free

**Problem:** Taking the shared GDAL execution mutex in `GdalDatasetDeleter` or
the `GdalAlgorithmRunner` destructor makes release wait for any long operation
on another thread and can deadlock when release happens from a progress
callback on a GDAL worker thread.

**Cause:** The execution mutex serializes GDAL *operations*. Reference
release (`GDALReleaseDataset`) is already thread-safe, so the lock added
nothing except blocking; no GDAL thread-safety requirement justifies it.

**Resolution:** Release dataset references and destroy runners without the
execution mutex, and document that the last reference must not be released
while another thread actively uses the dataset. Do not reintroduce the mutex
into release paths. Regression tests release a dataset and destroy a runner
while another thread's operation holds the lock inside a blocking progress
callback.

**Limitations:** The contract protects against blocking/deadlock, not against
misuse (releasing a dataset another thread is using).

See [GdalOperationResult.h](../../../src/MapWinGIS.Core/include/mapwingis/gdal/GdalOperationResult.h),
[GdalAlgorithmRunner.cpp](../../../src/MapWinGIS.Core/src/gdal/GdalAlgorithmRunner.cpp),
and [GdalAlgorithmRunnerTests.cpp](../../../tests/MapWinGIS.Core.Tests/GdalAlgorithmRunnerTests.cpp).

## Lazy work must not move the dominant cost to first paint

Removing triangulation from scene creation alone was not enough. The first
Automatic Fill paint became the first consumer and synchronously generated the
same meshes before presenting any pixels. The rendering path must keep the
paint operation bounded: use a complete GDI frame while immutable scenes are
prepared in the background, then publish the result and invalidate on the
owner thread. When backend target allocation is also expensive, warm that
target before the backend-switch repaint rather than moving the stall into the
next frame. Direct explicit-OpenGL draws retain the documented lazy fallback.

See [MapWinGISControl.cpp](../../../src/MapWinGIS.ActiveX/MapWinGISControl.cpp),
[VectorLayerRenderer.cpp](../../../src/MapWinGIS.ActiveX/VectorLayerRenderer.cpp),
and [OpenGlVectorLayerRenderer.cpp](../../../src/MapWinGIS.ActiveX/OpenGlVectorLayerRenderer.cpp).

## Measure Debug and Release rendering costs separately

Debug GDAL/GEOS builds can change the ranking and magnitude of costs. During the
first-frame work, triangulation was roughly six times slower in Debug on one
workstation, while Release users experienced a different bottleneck profile.
Collect structural assertions and threshold-free diagnostics in every build,
but make Release measurements before accepting or rejecting a performance
optimization. Keep optional real datasets and workstation-specific numbers
explicitly labeled rather than turning them into CI timing requirements.

See [the rendering performance baseline](../../../docs/TESTING.md#rendering-performance-baseline).

## A feature count may require a checked scan

**Problem:** Some OGR drivers cannot report a feature count. An indexed
feature API still needs a stable mapping from the public 0-based index to the
driver's feature ID.

**Cause:** Driver feature IDs can differ from public indexes, and a forced
count may itself scan the layer. A failed scan must not publish a partial
index or count as complete.

**Resolution:** The indexed-lookup ID scan uses `GetFeatureCount(FALSE)`, so it
does not force a preliminary count. `FeatureCount()` first requests a forced
count and uses the ID scan only as a fallback. The scan reads sequentially with
failure detection, resets reading through RAII, and marks the cache valid only
after it finishes. Tests cover unknown counts and retry after a mid-scan
failure.

**Limitations:** The scan is still O(n), and Core dataset access is not
thread-safe. See [VectorDataset.cpp](../../../src/MapWinGIS.Core/src/vector/VectorDataset.cpp),
[VectorLayerTests.cpp](../../../tests/MapWinGIS.Core.Tests/VectorLayerTests.cpp),
and [PR #47](https://github.com/MapWindow/MapWinGIS6/pull/47).

## vcpkg feature switches need explicit install tracking

**Problem:** Changing an MSBuild-selected vcpkg feature can otherwise leave
the existing manifest install in place.

**Cause:** vcpkg's manifest stamp does not include the repository's
`MwgVcpkgFeatures` property.

**Resolution:** `Directory.Build.targets` records the feature set with the
install stamp and invalidates the stamp when a later build requests another
set. This keeps feature changes effective across configurations sharing the
same platform install tree.

**References:** [Directory.Build.targets](../../../Directory.Build.targets),
[build guide](../../../docs/BUILDING.md#dependencies), and
[PR #26](https://github.com/MapWindow/MapWinGIS6/pull/26).

## Pixel checks must sample the host window

**Problem:** A screen-based pixel assertion can inspect a different window
when another window covers the test host.

**Resolution:** The automated C# host captures its own composed window with
`PrintWindow(PW_RENDERFULLCONTENT)` before checking rendered pixels. The
integration test still requires an interactive Windows desktop.

**References:** [Testing guide](../../../docs/TESTING.md#activex-host-integration-checks)
and [PR #26](https://github.com/MapWindow/MapWinGIS6/pull/26).

## ActiveX headers included by tests must be self-contained

**Problem:** `GdalOperationDispatcherTests.cpp` failed with `C1189: atlwin.h
requires atlbase.h to be included first` after it included
`GdalDispatchWindow.h` directly.

**Cause:** The header relied on an earlier `<atlbase.h>` include from its
ActiveX callers. The ActiveX tests include implementation headers directly,
so implicit include order does not hold there.

**Resolution:** ActiveX headers include every ATL header they need, with
`<atlbase.h>` before `<atlwin.h>`. Verified on
[PR #78](https://github.com/MapWindow/MapWinGIS6/pull/78).

See [GdalDispatchWindow.h](../../../src/MapWinGIS.ActiveX/GdalDispatchWindow.h).

## Shapefile geometry edits do not shrink stored bounds

OGR's Shapefile driver expands bounds during `SetFeature` but does not shrink
them. Core runs `RECOMPUTE EXTENT ON` during an explicit `VectorDataset::Flush()`
for dirty Shapefile and GeoPackage layers. Call it before closing after geometry
edits to preserve exact stored bounds. This costs one O(n) scan per dirty layer,
not per edit. Attribute edits do not trigger it. A recomputation failure throws
after geometry writes and leaves the layer dirty for retry. It does not roll
back the writes. Other formats retain their driver's stored-extent behavior.

See [VectorDataset.cpp](../../../src/MapWinGIS.Core/src/vector/VectorDataset.cpp)
and the shrinking and reopening cases in
[VectorLayerTests.cpp](../../../tests/MapWinGIS.Core.Tests/VectorLayerTests.cpp).

## Squash-merging development into main breaks later merges

**Problem:** After `main` received `development` as one squash commit, a trial
merge of `development` into `main` conflicted in 29 files, although the squash
tree was identical to a commit already on `development`.

**Cause:** The squash commit is not an ancestor of `development`, so Git sees
both branches adding the same files independently.

**Resolution:** Verify that `git diff <development-commit> origin/main` is
empty, then record the merge on `development` with
`git merge -s ours origin/main`. Use merge commits, not squash commits, for
`development` to `main` releases.

## Driver coercion makes cached written values stale

OGR drivers can coerce a written value; the Shapefile driver truncates a string
to the DBF field width, for example. Core therefore discards its cached feature
after every `SetFeature` write, successful or not, so a later read returns the
stored value instead of the value the caller wrote. See
`VectorLayerEditTest.ReadsBackTheStoredValueAfterTheDriverCoercesAWrite`.

## Posted completions can run before the first WM_PAINT

Windows dispatches posted messages before `WM_PAINT`. A background job that
finishes quickly can therefore post its owner-thread completion before the
invalidated control paints its first frame. Work done in that handler delays
that first frame. The control defers fill-mesh publication and OpenGL warm-up
until the first frame of a new layer set has been presented. See
`CMapWinGISControl::DispatchFillMeshPreparation` in
[MapWinGISControl.cpp](../../../src/MapWinGIS.ActiveX/MapWinGISControl.cpp)
and `ActiveXMapTest.FillMeshCompletionBeforeTheFirstPaintWaitsForThatFrame`.
