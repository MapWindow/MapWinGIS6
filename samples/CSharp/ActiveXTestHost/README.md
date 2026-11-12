# ActiveX Test Host

ActiveX Test Host is a Windows Forms application for testing the MapWinGIS ActiveX control. It loads vector datasets and lets you test layers, map navigation, and rendering.

## Build

Run these commands from the repository root:

```powershell
pwsh -File build\scripts\bootstrap-vcpkg.ps1
msbuild MapWinGIS6.slnx -restore -p:Configuration=Debug -p:Platform=x64 -m
```

The solution builds the host beside the ActiveX control and its native dependencies. For x86, build the matching `Win32` platform.

## Run

Start the manual host from an interactive Windows desktop session:

```powershell
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64
```

The host activates the control through its application manifest. It does not require global COM registration.

## Load and manage layers

Select `Open` to choose one or more vector datasets. You can also drag files onto the host. Each dataset adds its first layer to the list.

The list shows each layer name, feature count, stable ID, and source path. Select a layer and choose `Layer` to fit that layer. Choose `Map` to fit all layers. Choose `Clear` to remove all layers.

Available file formats depend on the GDAL drivers in the build. The host does not display raster datasets.

## Test navigation

Choose `Zoom +` or `Zoom -` to change the map scale. Use the arrow buttons to pan by the `Pan units` value. You can also drag with the left mouse button to pan, or use the mouse wheel to zoom at the cursor.

Choose `Repaint` to request a redraw. Choose `Recreate` to dispose of the control and create it again.

## Test rendering backends

The status bar shows the active rendering backend and rendering status. Automated mode selects GDI, then requests OpenGL. The OpenGL request can fall back to GDI when the scene or system does not support OpenGL.

## Run automated checks

Run automated mode from an interactive Windows desktop session:

```powershell
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64 -Automated
```

The host writes individual results to `bin\Debug\x64\ActiveXTestHost-results.json`. It checks dataset loading, layer IDs and counts, map navigation, clearing and reloading, invalid input, resizing, and rendering. A watchdog stops the run after 30 seconds. Use `-TimeoutSeconds` to set a value from 5 to 300 seconds.

## Limitations

The host loads only the first layer from each dataset. It does not implement raster display, projections, vector editing, or tile services. Automated checks require Windows and an interactive desktop session. The host does not provide a manual rendering-backend selector.
