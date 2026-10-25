// SPDX-License-Identifier: MPL-2.0

using System.Windows.Forms;

namespace MapWinGIS.ActiveXTestHost;

internal sealed class ActiveXHostControl : AxHost
{
    private const string ControlClassId = "{2F0AB6FC-F7C0-4E42-A002-A61F9C6F294F}";

    public ActiveXHostControl()
        : base(ControlClassId)
    {
    }

    public object? AutomationObject => GetOcx();
}
