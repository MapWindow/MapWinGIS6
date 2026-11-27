// SPDX-License-Identifier: MPL-2.0

namespace MapWinGIS.ActiveXTestHost;

internal enum ProjectionMismatchChoice
{
    Reproject,
    LoadAsIs,
    Cancel
}

internal sealed class ProjectionMismatchDialog : Form
{
    public ProjectionMismatchDialog(
        string layerName,
        string layerProjection,
        string mapProjection)
    {
        Text = "Projection mismatch";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.CenterParent;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(12);

        var message = new Label
        {
            AutoSize = true,
            MaximumSize = new Size(560, 0),
            Text = $"""
                The projection of "{layerName}" differs from the map projection.

                Layer: {layerProjection}
                Map: {mapProjection}

                Choose how to display this layer.
                """
        };

        var reproject = new Button
        {
            AutoSize = true,
            Text = "Reproject to map projection"
        };
        reproject.Click += (_, _) =>
        {
            Choice = ProjectionMismatchChoice.Reproject;
            DialogResult = DialogResult.OK;
        };

        var loadAsIs = new Button
        {
            AutoSize = true,
            Text = "Load as is"
        };
        loadAsIs.Click += (_, _) =>
        {
            Choice = ProjectionMismatchChoice.LoadAsIs;
            DialogResult = DialogResult.OK;
        };

        var cancel = new Button
        {
            AutoSize = true,
            DialogResult = DialogResult.Cancel,
            Text = "Cancel"
        };
        CancelButton = cancel;

        var buttons = new FlowLayoutPanel
        {
            AutoSize = true,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false
        };
        buttons.Controls.Add(reproject);
        buttons.Controls.Add(loadAsIs);
        buttons.Controls.Add(cancel);

        var layout = new TableLayoutPanel
        {
            AutoSize = true,
            ColumnCount = 1
        };
        layout.Controls.Add(message);
        layout.Controls.Add(buttons);
        Controls.Add(layout);
    }

    public ProjectionMismatchChoice Choice { get; private set; } =
        ProjectionMismatchChoice.Cancel;
}
