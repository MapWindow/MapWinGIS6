// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;

namespace MapWinGIS.ActiveXTestHost;

[ComImport]
[Guid("65E33073-669C-43E3-9A96-EF8D38F5EDAC")]
[InterfaceType(ComInterfaceType.InterfaceIsDual)]
internal interface IMapWinGISControlGdal
{
    [DispId(1)]
    long BeginGdalOperation(
        [MarshalAs(UnmanagedType.BStr)] string algorithmPath,
        [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_BSTR)]
        string[] arguments);

    [DispId(2)]
    void CancelGdalOperation(long operationId);

    [DispId(3)]
    void EndGdalOperation(
        long operationId,
        [MarshalAs(UnmanagedType.VariantBool)] out bool succeeded,
        [MarshalAs(UnmanagedType.VariantBool)] out bool cancelled,
        out int errorCode,
        out int callbackErrorCode,
        [MarshalAs(UnmanagedType.BStr)] out string callbackError,
        [MarshalAs(UnmanagedType.BStr)] out string summary,
        [MarshalAs(UnmanagedType.BStr)] out string diagnostics,
        [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_BSTR)]
        out string[] warnings);
}

internal sealed record GdalOperationResult(
    bool Succeeded,
    bool Cancelled,
    int ErrorCode,
    int CallbackErrorCode,
    string CallbackError,
    string Summary,
    string Diagnostics,
    string[] Warnings);

internal static class GdalOperationClient
{
    private static readonly Guid EventsInterfaceId =
        new("3EC7E255-3045-4A32-A9DA-12FA310FD3F0");

    private delegate void GdalProgressEvent(
        long operationId,
        double progress,
        string message);

    private delegate void GdalCompletedEvent(long operationId);

    public static async Task<GdalOperationResult> ConvertRasterAsync(
        object automationObject,
        string inputPath,
        string outputPath,
        IProgress<(double Progress, string Message)>? progress,
        CancellationToken cancellationToken)
    {
        var synchronizationContext = SynchronizationContext.Current ??
            throw new InvalidOperationException(
                "Start the GDAL operation from a UI thread with a message loop.");
        var control = (IMapWinGISControlGdal)automationObject;
        var completed = new TaskCompletionSource<long>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        long operationId = 0;

        GdalProgressEvent onProgress = (id, fraction, message) =>
        {
            if (id == operationId)
            {
                progress?.Report((fraction, message));
            }
        };
        GdalCompletedEvent onCompleted = id =>
        {
            if (id == operationId)
            {
                completed.TrySetResult(id);
            }
        };

        ComEventsHelper.Combine(control, EventsInterfaceId, 1, onProgress);
        ComEventsHelper.Combine(control, EventsInterfaceId, 2, onCompleted);
        try
        {
            operationId = control.BeginGdalOperation(
                "raster convert",
                [
                    "--input", inputPath,
                    "--output", outputPath,
                    "--output-format", "GTiff",
                ]);

            using CancellationTokenRegistration cancellation =
                cancellationToken.Register(() =>
                    synchronizationContext.Post(
                        _ =>
                        {
                            if (completed.Task.IsCompleted)
                            {
                                return;
                            }
                            try
                            {
                                control.CancelGdalOperation(operationId);
                            }
                            catch (COMException exception)
                            {
                                if (!completed.Task.IsCompleted)
                                {
                                    completed.TrySetException(exception);
                                }
                            }
                        },
                        null));

            if (cancellationToken.IsCancellationRequested)
            {
                control.CancelGdalOperation(operationId);
            }

            await completed.Task.ConfigureAwait(true);
            control.EndGdalOperation(
                operationId,
                out bool succeeded,
                out bool cancelled,
                out int errorCode,
                out int callbackErrorCode,
                out string callbackError,
                out string summary,
                out string diagnostics,
                out string[] warnings);

            return new GdalOperationResult(
                succeeded,
                cancelled,
                errorCode,
                callbackErrorCode,
                callbackError,
                summary,
                diagnostics,
                warnings);
        }
        finally
        {
            ComEventsHelper.Remove(control, EventsInterfaceId, 1, onProgress);
            ComEventsHelper.Remove(control, EventsInterfaceId, 2, onCompleted);
        }
    }
}
