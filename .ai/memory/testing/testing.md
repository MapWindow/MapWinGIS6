# Test setup and constraints

The solution builds two native GoogleTest executables:

- `MapWinGIS.Core.Tests` tests the Core API and behavior.
- `MapWinGIS.ActiveX.Tests` tests COM-facing behavior and native ActiveX
  components. ActiveX implementation units are also built as
  `MapWinGIS.ActiveX.Impl.lib`, so renderer and conversion code can be tested
  without activating the OCX.

Shared generated fixtures and temporary-directory support are in
`tests/MapWinGIS.Core.Tests/TestData.h` and `.cpp`. Extend these helpers instead
of committing generated shapefiles or databases. Initialize GDAL before
direct GDAL use; Core APIs initialize it as needed. Wrap expected GDAL output
in `GdalEnvironment::QuietScope`.

The GitHub workflow runs both native test executables. The separate .NET
ActiveX host checks require Windows and an interactive desktop; they are not
currently part of CI. The rendering performance baseline is opt-in and should
be run in Release when collecting meaningful timings.

The C# host calls ActiveX methods late-bound. An invalid layer ID surfaces as
`.NET ArgumentException` with the COM error text and `E_INVALIDARG` HRESULT.
The host assertion accepts this mapping as well as `COMException`; see
`samples/CSharp/ActiveXTestHost/MainForm.cs`.

GeoPackage fixture layers need an explicit geometry type: a layer created with
`wkbUnknown` maps to `ShapeType::NullShape` and is rejected as non-polygonal by
scene creation, while `wkbMultiPolygon` layers map to `ShapeType::Polygon` and
accept multipart WKT. The GPKG driver also writes self-intersecting polygon WKT
without validation, so invalid-geometry behavior is testable through a generated
GeoPackage.

Renderer pixel tests sample device pixels, not map coordinates:
`Viewport::MapToScreen` flips the y axis (`height/2 - (y - center.y) /
resolution`), and both backends draw through the same transform. Sampling raw
map coordinates checks the wrong location; convert through `MapPixel`/`ToPixel`
or compute device coordinates explicitly (cost a test iteration in PR #68).

The Shapefile driver defaults to `AUTO_REPACK=YES`: a `DeleteFeature()` fixture
is physically repacked on flush or close, so a test for deleted DBF records must
open the update handle with the `AUTO_REPACK=NO` open option. Assert that the raw
`GetFeatureCount(TRUE)` still includes the record, so the fixture provably
keeps it (PR #75). Direct `OGRLayer` use in tests needs `<ogrsf_frmts.h>`;
`<gdal_priv.h>` only forward-declares it.

## References

- [Testing guide](../../../docs/TESTING.md)
- [Build guide test commands](../../../docs/BUILDING.md#running-the-tests)
- [Shared fixtures](../../../tests/MapWinGIS.Core.Tests/TestData.h)
- [Core test project](../../../tests/MapWinGIS.Core.Tests/MapWinGIS.Core.Tests.vcxproj)
- [ActiveX test project](../../../tests/MapWinGIS.ActiveX.Tests/MapWinGIS.ActiveX.Tests.vcxproj)
- [CI test workflow](../../../.github/workflows/build.yml)
