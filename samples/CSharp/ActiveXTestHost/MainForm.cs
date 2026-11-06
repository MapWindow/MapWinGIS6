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
    private const double ZoomStep = 1.5;
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
    private readonly ListView _layerList;
    private readonly NumericUpDown _panStep;
    private readonly ToolStripStatusLabel _coreStatusLabel;
    private readonly ToolStripStatusLabel _layerCountLabel;
    private readonly ToolStripStatusLabel _featureCountLabel;
    private readonly ToolStripStatusLabel _backendLabel;
    private readonly ToolStripStatusLabel _renderingStatusLabel;
    private readonly List<CheckResult> _results = [];
    private readonly object _resultsLock = new();
    private readonly object _reportLock = new();
    private readonly CancellationTokenSource _watchdogCancellation = new();
    private readonly string _testDatasetDirectory =
        Path.Combine(Path.GetTempPath(), $"MapWinGIS.ActiveXTestHost-{Guid.NewGuid():N}");
    private string TestDatasetPath => Path.Combine(_testDatasetDirectory, "sample.geojson");
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
        MinimumSize = new Size(620, 360);
        StartPosition = FormStartPosition.CenterScreen;
        TopMost = options.Automated;

        var toolbar = new ToolStrip
        {
            Dock = DockStyle.Top,
            GripStyle = ToolStripGripStyle.Hidden,
            ImageScalingSize = new Size(16, 16)
        };
        AddToolButton(toolbar, "Open", OpenVectorDatasets,
            "Open vector datasets. Only the first layer of each dataset is loaded; formats depend on the GDAL build.");
        AddToolButton(toolbar, "Clear", ClearLayers, "Remove all loaded layers.");
        toolbar.Items.Add(new ToolStripSeparator());
        AddToolButton(toolbar, "Zoom +", () => RunNavigation(automation => automation.ZoomIn(ZoomStep)));
        AddToolButton(toolbar, "Zoom -", () => RunNavigation(automation => automation.ZoomOut(ZoomStep)));
        AddToolButton(
            toolbar,
            "Map",
            () => RunNavigation(automation => automation.FitToMap()),
            "Zoom to the full map.");
        AddToolButton(toolbar, "Layer", ZoomToSelectedLayer, "Zoom to the selected layer.");
        toolbar.Items.Add(new ToolStripSeparator());
        AddToolButton(toolbar, "←", () => PanMap(-1, 0), "Pan left by the selected map-unit step.");
        AddToolButton(toolbar, "→", () => PanMap(1, 0), "Pan right by the selected map-unit step.");
        AddToolButton(toolbar, "↑", () => PanMap(0, 1), "Pan up by the selected map-unit step.");
        AddToolButton(toolbar, "↓", () => PanMap(0, -1), "Pan down by the selected map-unit step.");
        toolbar.Items.Add(new ToolStripLabel("Pan units:"));
        _panStep = new NumericUpDown
        {
            DecimalPlaces = 2,
            Minimum = 0.01M,
            Maximum = 1_000_000_000M,
            Increment = 0.5M,
            Value = 1M,
            Width = 72,
            ThousandsSeparator = true
        };
        toolbar.Items.Add(new ToolStripControlHost(_panStep)
        {
            AutoSize = false,
            Width = 78,
            ToolTipText = "Pan step in the dataset's map-coordinate units."
        });
        toolbar.Items.Add(new ToolStripSeparator());
        AddToolButton(toolbar, "Recreate", RecreateManually, "Recreate the ActiveX control.");
        AddToolButton(toolbar, "Repaint", RequestManualRepaint, "Request a repaint of the control.");

        var layerPanel = new Panel { Dock = DockStyle.Fill };
        _layerList = new ListView
        {
            Dock = DockStyle.Fill,
            View = View.Details,
            FullRowSelect = true,
            HideSelection = false,
            MultiSelect = false,
            ShowItemToolTips = true
        };
        _layerList.Columns.Add("Layer", 105);
        _layerList.Columns.Add("Features", 65, HorizontalAlignment.Right);
        _layerList.Columns.Add("ID", 75, HorizontalAlignment.Right);
        layerPanel.Controls.Add(_layerList);

        var layout = new SplitContainer
        {
            Size = ClientSize,
            Dock = DockStyle.Fill,
            Orientation = Orientation.Vertical,
            FixedPanel = FixedPanel.Panel1,
            SplitterDistance = 270,
            Panel1MinSize = 250,
            Panel2MinSize = 250
        };
        layout.Panel1.Controls.Add(layerPanel);

        _surfacePanel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Magenta
        };
        layout.Panel2.Controls.Add(_surfacePanel);

        var statusStrip = new StatusStrip();
        _coreStatusLabel = new ToolStripStatusLabel("Core: starting");
        _layerCountLabel = new ToolStripStatusLabel("Layers: 0");
        _featureCountLabel = new ToolStripStatusLabel("Features: 0");
        _backendLabel = new ToolStripStatusLabel("Backend: unavailable");
        _renderingStatusLabel = new ToolStripStatusLabel("Rendering: starting")
        {
            Spring = true,
            TextAlign = ContentAlignment.MiddleLeft
        };
        statusStrip.Items.AddRange(
        [
            _coreStatusLabel,
            new ToolStripStatusLabel("|"),
            _layerCountLabel,
            new ToolStripStatusLabel("|"),
            _featureCountLabel,
            new ToolStripStatusLabel("|"),
            _backendLabel,
            new ToolStripStatusLabel("|"),
            _renderingStatusLabel
        ]);

        Controls.Add(layout);
        Controls.Add(toolbar);
        Controls.Add(statusStrip);
        EnableDatasetDrop(this);
        EnableDatasetDrop(layout);
        EnableDatasetDrop(layerPanel);
        EnableDatasetDrop(_layerList);
        EnableDatasetDrop(_surfacePanel);
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
            RefreshStatus();
        }
        catch (Exception exception)
        {
            string message = ExplainFailure(exception);
            _renderingStatusLabel.Text = $"Rendering: {message}";
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
            _renderingStatusLabel.Text = "Rendering: the control is not active.";
            return;
        }

        try
        {
            RepaintHost();
            RefreshStatus();
        }
        catch (Exception exception)
        {
            ShowOperationError(exception);
        }
    }

    private async Task RunAutomatedAsync()
    {
        try
        {
            RecordCheck("Prerequisites", VerifyPrerequisites);
            RecordCheck("Activation and creation", CreateHostedControl);
            RecordCheck("Automation contract", () =>
            {
                _ = ReadCoreInitialized();
                RefreshStatus();
            });
            RecordCheck("Load vector layer", LoadTestLayer);
            await RecordCheckAsync("GDAL COM event sinks", VerifyGdalEventSinksAsync);
            RecordCheck("CPU backend preference and status", VerifyCpuBackendSelection);

            RecordCheck("Resize", VerifyResize);
            await RecordCheckAsync("Repaint", VerifyRepaintAsync);
            RecordCheck("Dispose", VerifyDispose);
            RecordCheck("Recreation", CreateHostedControl);
            RecordCheck("Recreated automation contract", () =>
            {
                _ = ReadCoreInitialized();
                RefreshStatus();
            });
            RecordCheck("Reload vector layer", LoadTestLayer);
            await RecordCheckAsync("Recreated repaint", VerifyRepaintAsync);
            RecordCheck("Layer loading, navigation and clear", VerifyLayerOperations);
            RecordCheck("Interactive load rendering status", VerifyInteractiveLoadStatus);
            RecordCheck("Final dispose", VerifyDispose);

            if (_reportWriteError is not null)
            {
                throw new InvalidOperationException(
                    $"Could not write the automated result file '{_options.ResultsPath}'.",
                    _reportWriteError);
            }

            ExitCode = 0;
            _renderingStatusLabel.Text = "Automated hosting checks passed.";
        }
        catch (Exception exception)
        {
            ExitCode = 1;
            DisposeHostedControl();
            _renderingStatusLabel.Text = $"Rendering: {ExplainFailure(exception)}";
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
        _layerList.Items.Clear();

        var host = new ActiveXHostControl { Dock = DockStyle.Fill };
        EnableDatasetDrop(host);
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

        AddLayerListItem(new LoadedLayer(
            (long)automation.GetLayerId(0), "sample", 2, TestDatasetPath));
        VerifyCoreInitialized("vector dataset load");
        RefreshStatus();
    }

    private LoadedLayer AddVectorDataset(string path, List<string> warnings)
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        dynamic automation = automationObject;
        int previousLayerCount = (int)automation.LayerCount;
        long previousFeatureCount = (long)automation.FeatureCount;
        long layerId = (long)automation.LoadVectorDatasetEx(path);
        var layer = new LoadedLayer(
            layerId, Path.GetFileNameWithoutExtension(path), null, path);
        ListViewItem item = AddLayerListItem(layer);

        // The load is committed. Metadata failures must not hide the layer or report a failed load.
        try
        {
            int currentLayerCount = (int)automation.LayerCount;
            long currentFeatureCount = (long)automation.FeatureCount;
            if (layerId <= 0 ||
                currentLayerCount != previousLayerCount + 1 ||
                currentFeatureCount < previousFeatureCount)
            {
                throw new InvalidOperationException(
                    "The control returned inconsistent layer information.");
            }

            // This aggregate delta is valid only for one load at a time on the UI thread.
            long featureCount = currentFeatureCount - previousFeatureCount;
            layer = layer with { FeatureCount = featureCount };
            item.Tag = layer;
            item.SubItems[1].Text = featureCount.ToString();
            long indexedLayerId = (long)automation.GetLayerId(previousLayerCount);
            if (indexedLayerId != layerId)
            {
                throw new InvalidOperationException(
                    $"The control returned layer ID {layerId}, but index {previousLayerCount} reports ID {indexedLayerId}.");
            }
        }
        catch (Exception exception)
        {
            warnings.Add($"{path}: loaded as layer {layerId}, but metadata could not be verified. {ExplainOperationFailure(exception)}");
        }

        return layer;
    }

    private ListViewItem AddLayerListItem(LoadedLayer layer)
    {
        var item = new ListViewItem(layer.Name)
        {
            Tag = layer,
            ToolTipText = layer.Path
        };
        item.SubItems.Add(layer.FeatureCount?.ToString() ?? "unavailable");
        item.SubItems.Add(layer.Id.ToString());
        _layerList.Items.Add(item);
        return item;
    }

    private void OpenVectorDatasets()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Open vector datasets",
            Filter =
                "Vector datasets (*.geojson;*.shp;*.gpkg;*.kml;*.gml;*.tab;*.mif)|" +
                "*.geojson;*.shp;*.gpkg;*.kml;*.gml;*.tab;*.mif|" +
                "All files (*.*)|*.*",
            Multiselect = true,
            CheckFileExists = true
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            LoadDatasetsInteractively(dialog.FileNames);
        }
    }

    private void LoadDatasetsInteractively(IEnumerable<string> paths)
    {
        List<string> failures = [];
        List<string> warnings = [];
        foreach (string path in paths)
        {
            try
            {
                AddVectorDataset(path, warnings);
            }
            catch (Exception exception)
            {
                failures.Add($"{path}: {ExplainOperationFailure(exception)}");
            }
        }

        try
        {
            RepaintHost();
            RefreshStatus();
        }
        catch (Exception exception)
        {
            warnings.Add($"Could not refresh the control status: {ExplainOperationFailure(exception)}");
        }

        if (failures.Count > 0 || warnings.Count > 0)
        {
            string message = string.Join(Environment.NewLine, failures.Concat(warnings));
            _renderingStatusLabel.Text = $"Rendering: {message}";
            MessageBox.Show(
                this,
                message,
                "Vector dataset load results",
                MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
        }
    }

    private void ClearLayers()
    {
        try
        {
            ClearLoadedLayers();
        }
        catch (Exception exception)
        {
            ShowOperationError(exception);
        }
    }

    private void ClearLoadedLayers()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            throw new InvalidOperationException("The ActiveX automation object is unavailable.");
        }

        dynamic automation = automationObject;
        automation.ClearLayers();
        _layerList.Items.Clear();
        if ((int)automation.LayerCount != 0 || (long)automation.FeatureCount != 0 ||
            (bool)automation.LayerLoaded)
        {
            throw new InvalidOperationException(
                "The control still reports loaded layers after clearing them.");
        }

        RepaintHost();
        RefreshStatus();
    }

    private void VerifyLayerOperations()
    {
        ClearLoadedLayers();
        List<string> warnings = [];
        LoadedLayer first = AddVectorDataset(TestDatasetPath, warnings);
        LoadedLayer second = AddVectorDataset(TestDatasetPath, warnings);
        if (warnings.Count > 0)
        {
            throw new InvalidOperationException(string.Join(Environment.NewLine, warnings));
        }

        dynamic automation = _host!.AutomationObject!;
        if (first.Id <= 0 || second.Id <= 0 || first.Id == second.Id ||
            first.FeatureCount != 2 || second.FeatureCount != 2 ||
            (long)automation.GetLayerId(0) != first.Id ||
            (long)automation.GetLayerId(1) != second.Id ||
            _layerList.Items.Count != 2)
        {
            throw new InvalidOperationException("The two loaded layers have inconsistent IDs or feature counts.");
        }

        automation.ZoomToLayer(first.Id);
        automation.ZoomToLayer(second.Id);
        automation.Pan(1.0, -1.0);
        RepaintHost();
        if (!(bool)automation.LayerLoaded || (int)automation.LayerCount != 2 ||
            (long)automation.FeatureCount != 4)
        {
            throw new InvalidOperationException("Navigation changed the loaded layer or feature counts.");
        }

        ClearLoadedLayers();
        if (_layerList.Items.Count != 0)
        {
            throw new InvalidOperationException("The layer list was not cleared.");
        }
    }

    private void VerifyInteractiveLoadStatus()
    {
        dynamic automation = _host!.AutomationObject!;
        string previousPreference = automation.RenderingBackendPreference;
        try
        {
            automation.RenderingBackendPreference = "GDI";
            LoadDatasetsInteractively([TestDatasetPath]);
            if ((int)automation.LayerCount != 1 || _layerList.Items.Count != 1 ||
                _backendLabel.Text != "Backend: GDI" ||
                _renderingStatusLabel.Text != $"Rendering: {(string)automation.RenderingStatus}")
            {
                throw new InvalidOperationException(
                    "Interactive loading did not report the current frame's rendering status.");
            }
        }
        finally
        {
            automation.RenderingBackendPreference = previousPreference;
            ClearLoadedLayers();
        }
    }

    private void ZoomToSelectedLayer()
    {
        if (_layerList.SelectedItems.Count == 0 ||
            _layerList.SelectedItems[0].Tag is not LoadedLayer layer)
        {
            _renderingStatusLabel.Text = "Rendering: select a layer first.";
            return;
        }

        RunNavigation(automation => automation.ZoomToLayer(layer.Id));
    }

    private void PanMap(double directionX, double directionY)
    {
        double step = (double)_panStep.Value;
        RunNavigation(automation => automation.Pan(directionX * step, directionY * step));
    }

    private void RunNavigation(Action<dynamic> operation)
    {
        try
        {
            if (_host?.AutomationObject is not object automationObject)
            {
                throw new InvalidOperationException("The ActiveX automation object is unavailable.");
            }

            dynamic automation = automationObject;
            operation(automation);
            RepaintHost();
            RefreshStatus();
        }
        catch (Exception exception)
        {
            ShowOperationError(exception);
        }
    }

    private void RefreshStatus()
    {
        if (_host?.AutomationObject is not object automationObject)
        {
            _coreStatusLabel.Text = "Core: inactive";
            _layerCountLabel.Text = $"Layers: {_layerList.Items.Count}";
            _featureCountLabel.Text = "Features: unavailable";
            _backendLabel.Text = "Backend: unavailable";
            _renderingStatusLabel.Text = "Rendering: control is not active.";
            return;
        }

        dynamic automation = automationObject;
        _coreStatusLabel.Text = (bool)automation.CoreInitialized
            ? "Core: initialized"
            : "Core: not initialized";
        _layerCountLabel.Text = $"Layers: {(int)automation.LayerCount}";
        _featureCountLabel.Text = $"Features: {(long)automation.FeatureCount}";
        _backendLabel.Text = $"Backend: {(string)automation.RenderingBackend}";
        _renderingStatusLabel.Text = $"Rendering: {(string)automation.RenderingStatus}";
    }

    private void ShowOperationError(Exception exception)
    {
        string message = ExplainOperationFailure(exception);
        _renderingStatusLabel.Text = $"Rendering: {message}";
        MessageBox.Show(
            this,
            message,
            "MapWinGIS ActiveX Test Host",
            MessageBoxButtons.OK,
            MessageBoxIcon.Error);
    }

    private void AddToolButton(
        ToolStrip toolbar,
        string text,
        Action action,
        string? toolTip = null)
    {
        var button = new ToolStripButton(text)
        {
            ToolTipText = toolTip ?? text,
            DisplayStyle = ToolStripItemDisplayStyle.Text
        };
        button.Click += (_, _) => action();
        toolbar.Items.Add(button);
    }

    private void EnableDatasetDrop(Control control)
    {
        control.AllowDrop = true;
        control.DragEnter += (_, eventArgs) =>
        {
            eventArgs.Effect = eventArgs.Data?.GetDataPresent(DataFormats.FileDrop) == true
                ? DragDropEffects.Copy
                : DragDropEffects.None;
        };
        control.DragDrop += (_, eventArgs) =>
        {
            if (eventArgs.Data?.GetData(DataFormats.FileDrop) is string[] paths)
            {
                string[] droppedPaths = paths.ToArray();
                // Return from the OLE drop before loading or showing a modal dialog.
                BeginInvoke(new Action(() => LoadDatasetsInteractively(droppedPaths)));
            }
        };
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
        RefreshStatus();
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

    private static string ExplainOperationFailure(Exception exception)
    {
        Exception cause = exception.GetBaseException();
        return cause is COMException
            ? $"HRESULT 0x{cause.HResult:x8}: {cause.Message}"
            : cause.Message;
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

    private sealed record LoadedLayer(long Id, string Name, long? FeatureCount, string Path);

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

    private sealed record CheckResult(string Check, bool Passed, string Details);
    private sealed record TestReport(bool Automated, int ExitCode, IReadOnlyList<CheckResult> Checks);
}
