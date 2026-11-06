# Verified development lessons

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
