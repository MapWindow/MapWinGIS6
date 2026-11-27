// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;

namespace MapWinGIS.ActiveXTestHost;

[ComImport]
[Guid("26717861-E47D-4326-9414-FCF2E904B70D")]
[InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
internal interface IMapWinGISControlProjection
{
    [DispId(1)]
    string MapProjection
    {
        [return: MarshalAs(UnmanagedType.BStr)]
        get;
        [param: MarshalAs(UnmanagedType.BStr)]
        set;
    }

    [DispId(2)]
    int MapEpsg { get; }

    [DispId(3)]
    [return: MarshalAs(UnmanagedType.BStr)]
    string LayerProjection(long layerId);

    [DispId(4)]
    int LayerEpsg(long layerId);

    [DispId(5)]
    void ProbeVectorProjection(
        [MarshalAs(UnmanagedType.BStr)] string path,
        out int epsg,
        [MarshalAs(UnmanagedType.BStr)] out string wkt);

    [DispId(6)]
    void ReprojectLayer(long layerId);

    [DispId(7)]
    void ClearLayerReprojection(long layerId);
}
