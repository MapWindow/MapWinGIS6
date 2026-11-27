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
The layer tooltip shows its EPSG code when the dataset provides one.
The status bar shows the map EPSG code.

Available file formats depend on the GDAL drivers in the build. The host does not display raster datasets.

Before an interactive load, the host probes the dataset projection. If the map
has another projection, a dialog offers three choices:

1. Reproject the layer to the map projection for display.
2. Load the layer in its source coordinates.
3. Cancel the load.

The dialog belongs to the host. The ActiveX control only exposes projection
facts and display operations.

## Test navigation

Choose `Zoom +` or `Zoom -` to change the map scale. Use the arrow buttons to pan by the `Pan units` value. You can also drag with the left mouse button to pan, or use the mouse wheel to zoom at the cursor.

Choose `Repaint` to request a redraw. Choose `Recreate` to dispose of the control and create it again.

## Test rendering backends

The status bar shows the active rendering backend and rendering status. Automated mode selects GDI, then requests OpenGL. The OpenGL request can fall back to GDI when the scene or system does not support OpenGL.

With the default `Automatic` preference, Fill frames are painted with GDI
until OpenGL fill meshes exist. The control prepares those meshes with
bounded background workers from an immutable scene snapshot. The control
publishes them only from its owner-thread completion handler, and only after
the first frame of a newly loaded or changed layer set has been presented, so
that frame is never delayed by mesh publication or OpenGL warm-up. After
publication, the control warms the OpenGL target and invalidates the layer
frame; a later frame switches to OpenGL. `RenderingStatus` reports GDI while
the meshes are pending and OpenGL after the transition succeeds.

## Read the load and first-frame timings

Interactive Open and drag-and-drop loads use
`BeginLoadVectorDataset` and completion events, so the scene read does not
block the UI thread. The rightmost status-bar field shows the async load
duration up to the completion event and how long the first repaint took. The
host measures that repaint synchronously inside the completion event, before
any other queued paint can run. Use it for a debugger-free comparison with
MapWinGIS 5; meaningful timings require a Release build.

## Run automated checks

Run automated mode from an interactive Windows desktop session:

```powershell
pwsh -File samples\CSharp\ActiveXTestHost\Run-ActiveXTestHost.ps1 -Configuration Debug -Platform x64 -Automated
```

The host writes individual results to `bin\Debug\x64\ActiveXTestHost-results.json`. It checks dataset loading, layer IDs and counts, map navigation, clearing and reloading, invalid input, resizing, projection automation, and rendering. Automated mode does not open the projection mismatch dialog. A watchdog stops the run after 30 seconds. Use `-TimeoutSeconds` to set a value from 5 to 300 seconds.

## Limitations

The host loads only the first layer from each dataset. Reprojection changes
display coordinates only. It does not edit the source data. The host does not
implement raster display, vector editing, or tile services. Automated checks
require Windows and an interactive desktop session. The host does not provide
a manual rendering-backend selector.
