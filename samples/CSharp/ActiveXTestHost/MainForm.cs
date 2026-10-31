// SPDX-License-Identifier: MPL-2.0

using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Windows.Forms;

namespace MapWinGIS.ActiveXTestHost;

internal sealed class MainForm : Form
{
    private const string ProgId = "MapWinGIS6.Control.6";
    private const int ColorWindow = 5;
    private static readonly Color MapFillColor = Color.FromArgb(61, 122, 169);

#if MWG_X64
    private const ushort ExpectedMachine = 0x8664;
    private const string ExpectedArchitecture = "x64";
#elif MWG_X86
    private const ushort ExpectedMachine = 0x014c;
    private const string ExpectedArchitecture = "x86";
#else
#error The ActiveX test host must target x86 or x64.
#endif

    private readonly HostOptions _options;
    private readonly Panel _surfacePanel;
    private readonly ToolStripStatusLabel _statusLabel;
    private readonly List<CheckResult> _results = [];
    private readonly object _resultsLock = new();
    private readonly object _reportLock = new();
    private readonly CancellationTokenSource _watchdogCancellation = new();
    private readonly string _testDatasetDirectory =
        Path.Combine(Path.GetTempPath(), $"MapWinGIS.ActiveXTestHost-{Guid.NewGuid():N}");
    private string TestDatasetPath => Path.Combine(_testDatasetDirectory, "sample.geojson");
    private string TestSecondDatasetPath => Path.Combine(_testDatasetDirectory, "second.geojson");
    private string TestRasterPath => Path.Combine(_testDatasetDirectory, "sample.asc");
    private string TestRasterOutputPath => Path.Combine(_testDatasetDirectory, "sample.tif");
    private ActiveXHostControl? _host;
    private Exception? _reportWriteError;

    public MainForm(HostOptions options)
    {
        _options = options;
        ExitCode = options.Automated ? 1 : 0;
        Text = "MapWinGIS ActiveX Test Host";
        ClientSize = new Size(720, 480);
        MinimumSize = new Size(360, 260);
        StartPosition = FormStartPosition.CenterScreen;
        TopMost = options.Automated;

        var toolbar = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            Height = 42,
            Padding = new Padding(6),
            WrapContents = false
        };
        var recreateButton = new Button { Text = "Recreate control", AutoSize = true };
        var repaintButton = new Button { Text = "Request repaint", AutoSize = true };
        recreateButton.Click += (_, _) => RecreateManually();
        repaintButton.Click += (_, _) => RequestManualRepaint();
        toolbar.Controls.Add(recreateButton);
        toolbar.Controls.Add(repaintButton);

        _surfacePanel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Magenta
        };

        var statusStrip = new StatusStrip();
        _statusLabel = new ToolStripStatusLabel("Starting...");
        statusStrip.Items.Add(_statusLabel);

        Controls.Add(_surfacePanel);
        Controls.Add(toolbar);
        Controls.Add(statusStrip);
        Shown += OnShown;
        CreateTestDataset();
    }

    public int ExitCode { get; private set; }

    private void OnShown(object? sender, EventArgs eventArgs)
    {
        if (_options.Automated)
        {
            _ = EnforceTimeoutAsync();
            Activate();
            BeginInvoke(new Action(async () => await RunAutomatedAsync()));
            return;
        }

        RecreateManually();
    }

    private void RecreateManually()
    {
        try
        {
            VerifyPrerequisites();
            DisposeHostedControl();
            CreateHostedControl();
            LoadTestLayer();
            _host!.Update();
            Application.DoEvents();
            bool initialized = ReadCoreInitialized();
            dynamic automation = _host.AutomationObject!;
            _statusLabel.Text = initialized
                ? $"Active: {ProgId}; backend={automation.RenderingBackend}; {automation.RenderingStatus}"
                : $"Active: {ProgId}; CoreInitialized=false";
        }
        catch (Exception exception)
        {
            string message = ExplainFailure(exception);
            _statusLabel.Text = message;
            MessageBox.Show(
                this,
                message,
                "MapWinGIS ActiveX activation failed",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
    }

    private void RequestManualRepaint()
    {
        if (_host is null)
        {
            _statusLabel.Text = "The control is not active.";
            return;
        }

        _host.Invalidate(true);
        _host.Update();
        _surfacePanel.Update();
        dynamic automation = _host.AutomationObject!;
        _statusLabel.Text =
            $"backend={automation.RenderingBackend}; {automation.RenderingStatus}";
    }

    private async Task RunAutomatedAsync()
    {
        try
        {
            RecordCheck("Prerequisites", VerifyPrerequisites);
            RecordCheck("Activation and creation", CreateHostedControl);
            RecordCheck("Automation contract", () =>
            {
                bool initialized = ReadCoreInitialized();
                _statusLabel.Text = $"Active: {ProgId}; CoreInitialized={initialized}";
            });
            RecordCheck("Load vector layer", LoadTestLayer);
            RecordCheck("Programmatic map navigation", VerifyProgrammaticNavigation);
            await RecordCheckAsync("Mouse-wheel zoom and drag-to-pan", VerifyMouseNavigationAsync);
            await RecordCheckAsync("GDAL COM event sinks", VerifyGdalEventSinksAsync);
            RecordCheck("CPU backend preference and status", VerifyCpuBackendSelection);

            RecordCheck("Resize", VerifyResize);
            await RecordCheckAsync("Repaint", VerifyRepaintAsync);
            RecordCheck("Dispose", VerifyDispose);
            RecordCheck("Recreation", CreateHostedControl);
            RecordCheck("Recreated automation contract", () =>
            {
                bool initialized = ReadCoreInitialized();
                _statusLabel.Text =
                    $"Recreated: {ProgId}; CoreInitialized={initialized}";
            });
            RecordCheck("Reload vector layer", LoadTestLayer);
            await RecordCheckAsync("Recreated repaint", VerifyRepaintAsync);
            RecordCheck("Final dispose", VerifyDispose);

            if (_reportWriteError is not null)
            {
                throw new InvalidOperationException(
                    $"Could not write the automated result file '{_options.ResultsPath}'.",
                    _reportWriteError);
            }

            ExitCode = 0;
            _statusLabel.Text = "All automated hosting checks passed.";
        }
        catch (Exception exception)
        {
            ExitCode = 1;
            _statusLabel.Text = ExplainFailure(exception);
            DisposeHostedControl();
        }
        finally
        {
            _watchdogCancellation.Cancel();
            SaveResults();
            Close();
        }
    }

    private void VerifyPrerequisites()
    {
        if (IntPtr.Size != (ExpectedMachine == 0x8664 ? 8 : 4))
        {
            throw new InvalidOperationException(
                $"This host must run as {ExpectedArchitecture}; its process architecture does not match.");
        }

        string directory = AppContext.BaseDirectory;
        string controlPath = Path.Combine(directory, "MapWinGIS.ocx");
        string corePath = Path.Combine(directory, "MapWinGIS.Core.dll");

        VerifyFileArchitecture(controlPath);
        VerifyFileArchitecture(corePath);

        foreach (string runtimeDirectory in new[] { "gdal-data", "proj" })
        {
            if (!Directory.Exists(Path.Combine(directory, runtimeDirectory)))
            {
                throw new FileNotFoundException(
                    $"Required runtime data folder '{runtimeDirectory}' is missing beside the host.");
            }
        }

        if (!File.Exists(TestDatasetPath))
        {
            throw new FileNotFoundException(
                "The generated vector test dataset is missing.",
                TestDatasetPath);
        }
        if (!File.Exists(TestSecondDatasetPath))
        {
            throw new FileNotFoundException(
                "The generated second vector test dataset is missing.",
                TestSecondDatasetPath);
        }
    }

    private void CreateTestDataset()
    {
        Directory.CreateDirectory(_testDatasetDirectory);
        File.WriteAllText(
            TestDatasetPath,
            """
            {
              "type": "FeatureCollection",
              "name": "sample",
              "crs": { "type": "name", "properties": { "name": "urn:ogc:def:crs:OGC:1.3:CRS84" } },
              "features": [
                {
                  "type": "Feature",
                  "properties": { "id": 1, "name": "west" },
                  "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[0, 0], [1.5, 0], [1.5, 2], [0, 2], [0, 0]]]
                  }
                },
                {
                  "type": "Feature",
                  "properties": { "id": 2, "name": "east" },
                  "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[2.5, 0], [4, 0], [4, 2], [2.5, 2], [2.5, 0]]]
                  }
                }
              ]
            }
            """);
        File.WriteAllText(
            TestSecondDatasetPath,
            """
            {
              "type": "FeatureCollection",
              "name": "second",
              "crs": { "type": "name", "properties": { "name": "urn:ogc:def:crs:OGC:1.3:CRS84" } },
              "features": [
                {
                  "type": "Feature",
                  "properties": { "id": 3, "name": "west" },
                  "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[100, 0], [101.5, 0], [101.5, 2], [100, 2], [100, 0]]]
                  }
                },
                {
                  "type": "Feature",
                  "properties": { "id": 4, "name": "east" },
                  "geometry": {
                    "type": "Polygon",
                    "coordinates": [[[102.5, 0], [104, 0], [104, 2], [102.5, 2], [102.5, 0]]]
                  }
                }
              ]
            }
            """);

        var raster = new StringBuilder();
        raster.AppendLine("ncols 512");
        raster.AppendLine("nrows 512");
        raster.AppendLine("xllcorner 0");
        raster.AppendLine("yllcorner 0");
        raster.AppendLine("cellsize 1");
        raster.AppendLine("NODATA_value -9999");
        for (int row = 0; row < 512; row++)
        {
            for (int column = 0; column < 512; column++)
            {
                raster.Append("1 ");
            }
            raster.AppendLine();
        }
        File.WriteAllText(TestRasterPath, raster.ToString());
    }

    private static void VerifyFileArchitecture(string path)
    {
        if (!File.Exists(path))
        {
            throw new FileNotFoundException($"Required native file was not found: '{path}'.", path);
        }

        using var stream = File.OpenRead(path);
        using var reader = new BinaryReader(stream);
        if (reader.ReadUInt16() != 0x5a4d)
        {
            throw new InvalidDataException($"'{path}' is not a valid Windows executable.");
        }

        stream.Position = 0x3c;
        int headerOffset = reader.ReadInt32();
        if (headerOffset < 0 || headerOffset > stream.Length - 6)
        {
            throw new InvalidDataException($"'{path}' has an invalid PE header.");
        }

        stream.Position = headerOffset;
        if (reader.ReadUInt32() != 0x00004550)
        {
            throw new InvalidDataException($"'{path}' has an invalid PE signature.");
        }

        ushort machine = reader.ReadUInt16();
        if (machine != ExpectedMachine)
        {
            string actual = machine switch
            {
                0x014c => "x86",
                0x8664 => "x64",
                _ => $"unknown (0x{machine:x4})"
            };
            throw new BadImageFormatException(
                $"Architecture mismatch: '{Path.GetFileName(path)}' is {actual}, but this host is {ExpectedArchitecture}.");
        }
    }

    private void CreateHostedControl()
    {
        DisposeHostedControl();

        var host = new ActiveXHostControl { Dock = DockStyle.Fill };
        _surfacePanel.Controls.Add(host);
        _host = host;
        host.CreateControl();
        _ = host.Handle;

        if (!host.IsHandleCreated || host.AutomationObject is null)
        {
            throw new InvalidOperationException(
                "AxHost did not create a window and automation object for the ActiveX control.");
        }

        Application.DoEvents();
    }

    private async Task WaitForRepaintAsync()
    {
        RepaintHost();
        await Task.Delay(150);
        Application.DoEvents();
    }

    private void SendMouseWheel(Point clientPosition, short delta)
    {
        if (_host is null)
        {
            throw new InvalidOperationException("The ActiveX control has not been created.");
        }

        Point screenPosition = _host.PointToScreen(clientPosition);
        IntPtr wParam = new(unchecked((int)((ushort)delta << 16)));
        SendMessage(
            _host.Handle,
            MouseWheelMessage,
            wParam,
            PackPosition(screenPosition.X, screenPosition.Y));
    }

    private void SendMouseMessage(uint message, nint buttonState, Point clientPosition)
    {
        if (_host is null)
        {
            throw new InvalidOperationException("The ActiveX control has not been created.");
        }

        SendMessage(
            _host.Handle,
            message,
            buttonState,
            PackPosition(clientPosition.X, clientPosition.Y));
    }

    private static void VerifyImagesDiffer(Bitmap before, Bitmap after, string operation)
    {
        if (before.Width != after.Width || before.Height != after.Height)
        {
            throw new InvalidOperationException($"The host image size changed during {operation}.");
        }

        int changedPixels = 0;
        for (int y = 0; y < before.Height; y++)
        {
            for (int x = 0; x < before.Width; x++)
            {
                if (before.GetPixel(x, y).ToArgb() != after.GetPixel(x, y).ToArgb())
                {
                    changedPixels++;
                }
            }
        }
        if (changedPixels < 100)
        {
            throw new InvalidOperationException(
                $"The {operation} did not change the rendered map enough to verify navigation.");
        }
    }

    private bool ReadCoreInitialized()
    {
        if (_host is null)
        {
            throw new InvalidOperationException("The ActiveX control has not been created.");
        }

        dynamic automation = _host.AutomationObject ??
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        return (bool)automation.CoreInitialized;
    }

    private void LoadTestLayer()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        dynamic automation = automationObject;
        automation.LoadVectorDataset(TestDatasetPath);
        if (!(bool)automation.LayerLoaded || (long)automation.FeatureCount != 2)
        {
            throw new InvalidOperationException(
                "The control did not load the two-feature polygon test layer.");
        }

        VerifyCoreInitialized("vector dataset load");
    }

    private void VerifyProgrammaticNavigation()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        dynamic automation = automationObject;
        automation.LoadVectorDataset(TestSecondDatasetPath);
        if ((int)automation.LayerCount != 2 || (long)automation.FeatureCount != 4)
        {
            throw new InvalidOperationException(
                "Loading another dataset did not add its layer while preserving the map.");
        }

        long firstLayerId = (long)automation.GetLayerId(0);
        long secondLayerId = (long)automation.GetLayerId(1);
        if (firstLayerId <= 0 || secondLayerId <= 0 || firstLayerId == secondLayerId)
        {
            throw new InvalidOperationException("The map did not expose distinct stable layer IDs.");
        }

        if (!(bool)automation.ZoomToMap() ||
            !(bool)automation.ZoomToLayer(secondLayerId) ||
            (bool)automation.ZoomToLayer(secondLayerId + 1000) ||
            !(bool)automation.ZoomToLayer(firstLayerId) ||
            !(bool)automation.ZoomIn() ||
            !(bool)automation.ZoomOut())
        {
            throw new InvalidOperationException(
                "The control's zoom-to-map, zoom-to-layer, zoom-in, or zoom-out operation failed.");
        }

        if (!(bool)automation.ZoomToMap() ||
            !(bool)automation.SetLayerVisible(secondLayerId, false) ||
            !(bool)automation.ZoomToMap() ||
            !(bool)automation.SetLayerVisible(secondLayerId, true) ||
            !(bool)automation.ZoomToMap() ||
            !(bool)automation.ZoomToLayer(firstLayerId) ||
            !(bool)automation.RemoveLayer(secondLayerId) ||
            (int)automation.LayerCount != 1 ||
            (long)automation.FeatureCount != 2 ||
            (long)automation.GetLayerId(0) != firstLayerId)
        {
            throw new InvalidOperationException(
                "The control's layer visibility or removal operation failed.");
        }

        automation.ClearLayers();
        if ((int)automation.LayerCount != 0 ||
            (long)automation.FeatureCount != 0 ||
            (bool)automation.ZoomToMap())
        {
            throw new InvalidOperationException("The control did not clear its layer collection.");
        }

        automation.LoadVectorDataset(TestDatasetPath);
        automation.LoadVectorDataset(TestSecondDatasetPath);
        if ((int)automation.LayerCount != 2 || !(bool)automation.ZoomToMap())
        {
            throw new InvalidOperationException(
                "The control did not restore its layers after clearing the map.");
        }
    }

    private async Task VerifyMouseNavigationAsync()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        dynamic automation = automationObject;
        long firstLayerId = (long)automation.GetLayerId(0);

        automation.ZoomToMap();
        if (!(bool)automation.ZoomToLayer(firstLayerId))
        {
            throw new InvalidOperationException("Could not reset the view before testing mouse input.");
        }
        RepaintHost();
        using (Bitmap beforeWheel = CaptureWindowContent())
        {
            SendMouseWheel(new Point(_host.Width / 5, _host.Height / 2), 240);
            await WaitForRepaintAsync();
            using Bitmap afterWheel = CaptureWindowContent();
            VerifyImagesDiffer(beforeWheel, afterWheel, "centered mouse-wheel zoom");
        }

        automation.ZoomToMap();
        if (!(bool)automation.ZoomToLayer(firstLayerId))
        {
            throw new InvalidOperationException("Could not reset the view before testing edge zoom.");
        }
        RepaintHost();
        using (Bitmap beforeEdgeWheel = CaptureWindowContent())
        {
            SendMouseWheel(new Point(_host.Width * 4 / 5, _host.Height / 2), -120);
            await WaitForRepaintAsync();
            using Bitmap afterEdgeWheel = CaptureWindowContent();
            VerifyImagesDiffer(beforeEdgeWheel, afterEdgeWheel, "near-edge mouse-wheel zoom");
        }

        automation.ZoomToMap();
        if (!(bool)automation.ZoomToLayer(firstLayerId))
        {
            throw new InvalidOperationException("Could not reset the view before testing drag-to-pan.");
        }
        RepaintHost();
        int dragDistance = Math.Max(40, _host.Width / 8);
        Point start = new(_host.Width / 2, _host.Height / 2);
        Point end = new(start.X + dragDistance, start.Y);
        SendMouseMessage(LeftButtonDown, 1, start);
        SendMouseMessage(MouseMoveMessage, 1, end);
        SendMouseMessage(LeftButtonUp, 0, new Point(-8, -8));
        await WaitForRepaintAsync();

        using Bitmap afterPan = CaptureWindowContent();
        VerifyPaintPixel(
            afterPan,
            _host.PointToScreen(new Point(_host.Width / 5 + dragDistance, _host.Height / 2)),
            ColorToColorRef(MapFillColor),
            "panned polygon");
        if (!(bool)automation.ZoomToLayer(firstLayerId))
        {
            throw new InvalidOperationException("Could not restore the view after testing drag-to-pan.");
        }
        RepaintHost();
    }

    private void VerifyCoreInitialized(string activation)
    {
        if (!ReadCoreInitialized())
        {
            throw new InvalidOperationException(
                $"The ActiveX control reported CoreInitialized=false after {activation}.");
        }
    }

    private void VerifyCpuBackendSelection()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        dynamic automation = automationObject;
        string previousPreference = automation.RenderingBackendPreference;
        try
        {
            automation.RenderingBackendPreference = "GDI";
            RepaintHost();

            string activeBackend = automation.RenderingBackend;
            string renderingStatus = automation.RenderingStatus;
            if (activeBackend != "GDI" ||
                renderingStatus != "GDI was selected by the rendering preference.")
            {
                throw new InvalidOperationException(
                    $"The requested CPU backend was not reported. Backend='{activeBackend}', status='{renderingStatus}'.");
            }

            VerifyMapPixels();
        }
        finally
        {
            // Later resize/repaint checks must exercise the default backend path.
            automation.RenderingBackendPreference = previousPreference;
            RepaintHost();
        }
    }

    private async Task VerifyGdalEventSinksAsync()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        GdalOperationResult result = await GdalOperationClient.ConvertRasterAsync(
            automationObject,
            TestRasterPath,
            TestRasterOutputPath,
            progress: null,
            CancellationToken.None);

        if (!result.Succeeded || result.Cancelled || !File.Exists(TestRasterOutputPath))
        {
            throw new InvalidOperationException(
                $"The GDAL raster conversion did not complete successfully. {result.Diagnostics}");
        }
        if (result.OperationId <= 0 ||
            result.CompletedOperationId != result.OperationId)
        {
            throw new InvalidOperationException(
                "The GDAL completion event did not report the operation ID returned by BeginGdalOperation.");
        }
        if (!result.ProgressEvents.Any(notification =>
                notification.OperationId == result.OperationId &&
                double.IsFinite(notification.Progress) &&
                notification.Progress is >= 0.0 and <= 1.0 &&
                notification.Message is not null))
        {
            throw new InvalidOperationException(
                "The GDAL progress event did not report valid operation arguments.");
        }
    }

    private void RepaintHost()
    {
        _host?.Invalidate(true);
        _host?.Update();
        Application.DoEvents();
    }

    private void VerifyResize()
    {
        if (_host is null)
        {
            throw new InvalidOperationException("The ActiveX control has not been created.");
        }

        int initialWidth = _host.Width;
        int initialHeight = _host.Height;
        ClientSize = new Size(840, 560);
        PerformLayout();
        Application.DoEvents();

        if (_host.Bounds != _surfacePanel.ClientRectangle ||
            _host.Width <= initialWidth ||
            _host.Height <= initialHeight)
        {
            throw new InvalidOperationException(
                $"The hosted surface did not follow the resized panel. Control={_host.Bounds}, panel={_surfacePanel.ClientRectangle}.");
        }
    }

    private async Task VerifyRepaintAsync()
    {
        if (_host is null)
        {
            throw new InvalidOperationException("The ActiveX control has not been created.");
        }

        _host.Invalidate(true);
        _surfacePanel.Invalidate();
        _host.Update();
        _surfacePanel.Update();
        await Task.Delay(150);
        Application.DoEvents();
        VerifyMapPixels();
    }

    private void VerifyMapPixels()
    {
        if (_host is null)
        {
            throw new InvalidOperationException("The ActiveX control has not been created.");
        }

        uint background = GetSysColor(ColorWindow);
        using Bitmap windowImage = CaptureWindowContent();
        VerifyPaintPixel(
            windowImage,
            _host.PointToScreen(new Point(_host.Width / 5, _host.Height / 2)),
            ColorToColorRef(MapFillColor),
            "first polygon");
        VerifyPaintPixel(
            windowImage,
            _host.PointToScreen(new Point(_host.Width / 2, _host.Height / 2)),
            background,
            "empty space between polygons");
        VerifyPaintPixel(
            windowImage,
            _host.PointToScreen(new Point(_host.Width * 4 / 5, _host.Height / 2)),
            ColorToColorRef(MapFillColor),
            "second polygon");
    }

    // Sampling the screen DC reads whatever is on top at that position, so another
    // window or a lost foreground race made the check fail although the control had
    // painted correctly. PrintWindow with PW_RENDERFULLCONTENT reads this window's own
    // composed content, including child windows and OpenGL output, regardless of z-order.
    private Bitmap CaptureWindowContent()
    {
        if (!GetWindowRect(Handle, out NativeRect windowRect))
        {
            throw new InvalidOperationException("Could not read the host window bounds.");
        }

        var image = new Bitmap(
            windowRect.Right - windowRect.Left,
            windowRect.Bottom - windowRect.Top);
        try
        {
            using (Graphics graphics = Graphics.FromImage(image))
            {
                IntPtr dc = graphics.GetHdc();
                try
                {
                    if (!PrintWindow(Handle, dc, PrintWindowRenderFullContent))
                    {
                        throw new InvalidOperationException(
                            "Could not capture the rendered content of the host window.");
                    }
                }
                finally
                {
                    graphics.ReleaseHdc(dc);
                }
            }

            image.Tag = new Point(windowRect.Left, windowRect.Top);
            return image;
        }
        catch
        {
            image.Dispose();
            throw;
        }
    }

    private static void VerifyPaintPixel(
        Bitmap windowImage,
        Point screenPoint,
        uint expected,
        string location)
    {
        var origin = (Point)windowImage.Tag!;
        int x = screenPoint.X - origin.X;
        int y = screenPoint.Y - origin.Y;
        if (x < 0 || y < 0 || x >= windowImage.Width || y >= windowImage.Height)
        {
            throw new InvalidOperationException(
                $"The {location} sample point lies outside the captured host window.");
        }

        uint actual = ColorToColorRef(windowImage.GetPixel(x, y));
        if (actual != expected)
        {
            throw new InvalidOperationException(
                $"Expected color 0x{expected:x6} at the {location} of the control, got 0x{actual:x6}.");
        }
    }

    private void VerifyDispose()
    {
        DisposeHostedControl();
        if (_host is not null || _surfacePanel.Controls.Count != 0)
        {
            throw new InvalidOperationException("The hosted control was not disposed.");
        }
    }

    private void DisposeHostedControl()
    {
        if (_host is null)
        {
            return;
        }

        ActiveXHostControl host = _host;
        _host = null;
        _surfacePanel.Controls.Remove(host);
        host.Dispose();
        Application.DoEvents();
    }

    private void RecordCheck(string name, Action action)
    {
        try
        {
            action();
            AddResult(name, true, "Passed.");
        }
        catch (Exception exception)
        {
            AddResult(name, false, ExplainFailure(exception));
            throw;
        }
    }

    private async Task RecordCheckAsync(string name, Func<Task> action)
    {
        try
        {
            await action();
            AddResult(name, true, "Passed.");
        }
        catch (Exception exception)
        {
            AddResult(name, false, ExplainFailure(exception));
            throw;
        }
    }

    private void AddResult(string name, bool passed, string details)
    {
        lock (_resultsLock)
        {
            _results.Add(new CheckResult(name, passed, details));
        }
        SaveResults();
    }

    private void SaveResults()
    {
        lock (_reportLock)
        {
            List<CheckResult> snapshot;
            lock (_resultsLock)
            {
                snapshot = [.. _results];
            }

            try
            {
                string? directory = Path.GetDirectoryName(_options.ResultsPath);
                if (!string.IsNullOrEmpty(directory))
                {
                    Directory.CreateDirectory(directory);
                }

                File.WriteAllText(
                    _options.ResultsPath,
                    JsonSerializer.Serialize(
                        new TestReport(_options.Automated, ExitCode, snapshot),
                        new JsonSerializerOptions { WriteIndented = true }));
            }
            catch (Exception exception)
            {
                _reportWriteError ??= exception;
                System.Diagnostics.Debug.WriteLine(
                    $"Could not write the test report '{_options.ResultsPath}': {exception}");
            }
        }
    }

    private static string ExplainFailure(Exception exception)
    {
        Exception cause = exception;
        while (cause.InnerException is not null)
        {
            cause = cause.InnerException;
        }

        if (cause is FileNotFoundException)
        {
            return $"Missing file: {cause.Message}";
        }

        if (cause is BadImageFormatException ||
            cause.HResult == unchecked((int)0x8007000b))
        {
            return $"Architecture mismatch: {cause.Message}";
        }

        if (cause is COMException comException)
        {
            return comException.HResult switch
            {
                unchecked((int)0x80040154) =>
                    "The control is not available through the host manifest. Check the OCX CLSID and typelib entries in app.manifest.",
                unchecked((int)0x8007007e) =>
                    "A native control dependency is missing. Rebuild the matching native platform and check the runtime files beside the host.",
                _ => $"ActiveX activation failed (HRESULT 0x{comException.HResult:x8}): {comException.Message}"
            };
        }

        return exception.Message;
    }

    private async Task EnforceTimeoutAsync()
    {
        try
        {
            await Task.Delay(
                TimeSpan.FromSeconds(_options.TimeoutSeconds),
                _watchdogCancellation.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        AddResult(
            "Timeout",
            false,
            $"Automated hosting did not finish within {_options.TimeoutSeconds} seconds.");
        ExitCode = 2;
        SaveResults();
        Environment.Exit(2);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            DisposeHostedControl();
            _watchdogCancellation.Dispose();
            if (Directory.Exists(_testDatasetDirectory))
            {
                Directory.Delete(_testDatasetDirectory, recursive: true);
            }
        }
        base.Dispose(disposing);
    }

    private static uint ColorToColorRef(Color color) =>
        (uint)(color.R | (color.G << 8) | (color.B << 16));

    private static IntPtr PackPosition(int x, int y) =>
        new(unchecked((int)((ushort)(short)x | ((uint)(ushort)(short)y << 16))));

    private const uint MouseWheelMessage = 0x020A;
    private const uint LeftButtonDown = 0x0201;
    private const uint MouseMoveMessage = 0x0200;
    private const uint LeftButtonUp = 0x0202;
    private const uint PrintWindowRenderFullContent = 0x00000002;

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr window, out NativeRect rect);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetSysColor(int index);

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr window, uint message, nint wParam, nint lParam);

    private sealed record CheckResult(string Check, bool Passed, string Details);
    private sealed record TestReport(bool Automated, int ExitCode, IReadOnlyList<CheckResult> Checks);
}
