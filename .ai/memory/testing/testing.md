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

## References

- [Testing guide](../../../docs/TESTING.md)
- [Build guide test commands](../../../docs/BUILDING.md#running-the-tests)
- [Shared fixtures](../../../tests/MapWinGIS.Core.Tests/TestData.h)
- [Core test project](../../../tests/MapWinGIS.Core.Tests/MapWinGIS.Core.Tests.vcxproj)
- [ActiveX test project](../../../tests/MapWinGIS.ActiveX.Tests/MapWinGIS.ActiveX.Tests.vcxproj)
- [CI test workflow](../../../.github/workflows/build.yml)
