// SPDX-License-Identifier: MPL-2.0

using System.Drawing;
using System.Runtime.InteropServices;
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
        automation.RenderingBackendPreference = "GDI";
        _host.Invalidate(true);
        _host.Update();
        Application.DoEvents();

        string activeBackend = automation.RenderingBackend;
        string renderingStatus = automation.RenderingStatus;
        if (activeBackend != "GDI" ||
            renderingStatus != "GDI was selected by the rendering preference.")
        {
            throw new InvalidOperationException(
                $"The requested CPU backend was not reported. Backend='{activeBackend}', status='{renderingStatus}'.");
        }
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

        uint background = GetSysColor(ColorWindow);
        VerifyPaintPixel(
            _host.PointToScreen(new Point(_host.Width / 5, _host.Height / 2)),
            ColorToColorRef(MapFillColor),
            "first polygon");
        VerifyPaintPixel(
            _host.PointToScreen(new Point(_host.Width / 2, _host.Height / 2)),
            background,
            "empty space between polygons");
        VerifyPaintPixel(
            _host.PointToScreen(new Point(_host.Width * 4 / 5, _host.Height / 2)),
            ColorToColorRef(MapFillColor),
            "second polygon");
    }

    private static void VerifyPaintPixel(Point point, uint expected, string location)
    {
        IntPtr screen = GetDC(IntPtr.Zero);
        if (screen == IntPtr.Zero)
        {
            throw new InvalidOperationException("Could not capture the visible control surface.");
        }

        try
        {
            uint actual = GetPixel(screen, point.X, point.Y);
            if (actual == 0xffffffff)
            {
                throw new InvalidOperationException(
                    $"Could not sample the {location} pixel of the control surface.");
            }

            if (actual != expected)
            {
                throw new InvalidOperationException(
                    $"Expected color 0x{expected:x6} at the {location} of the control, got 0x{actual:x6}.");
            }
        }
        finally
        {
            ReleaseDC(IntPtr.Zero, screen);
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

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr window, IntPtr dc);

    [DllImport("gdi32.dll")]
    private static extern uint GetPixel(IntPtr dc, int x, int y);

    [DllImport("user32.dll")]
    private static extern uint GetSysColor(int index);

    private sealed record CheckResult(string Check, bool Passed, string Details);
    private sealed record TestReport(bool Automated, int ExitCode, IReadOnlyList<CheckResult> Checks);
}
