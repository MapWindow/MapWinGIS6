// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;

namespace MapWinGIS.ActiveXTestHost;

[ComImport]
[Guid("4F8CE31A-60D1-4B9D-A4AF-2C80C5F63A91")]
[InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
internal interface IMapWinGISControl5
{
    [DispId(18)]
    long BeginLoadVectorDataset(
        [MarshalAs(UnmanagedType.BStr)] string path);

    [DispId(19)]
    void CancelLoadVectorDataset(long operationId);

    [DispId(20)]
    void EndLoadVectorDataset(
        long operationId,
        [MarshalAs(UnmanagedType.VariantBool)] out bool succeeded,
        [MarshalAs(UnmanagedType.VariantBool)] out bool cancelled,
        out long layerId,
        [MarshalAs(UnmanagedType.BStr)] out string summary,
        [MarshalAs(UnmanagedType.BStr)] out string diagnostics);
}

internal sealed record VectorLoadResult(
    long OperationId,
    long CompletedOperationId,
    bool Succeeded,
    bool Cancelled,
    long LayerId,
    string Summary,
    string Diagnostics);

internal static class VectorLoadClient
{
    private const int HResultPending = unchecked((int)0x8000000A);
    private static readonly Guid EventsInterfaceId =
        new("3EC7E255-3045-4A32-A9DA-12FA310FD3F0");

    private delegate void GdalCompletedEvent(long operationId);

    public static async Task<VectorLoadResult> LoadAsync(
        object automationObject,
        string path,
        CancellationToken cancellationToken)
    {
        var synchronizationContext = SynchronizationContext.Current ??
            throw new InvalidOperationException(
                "Start the vector load from a UI thread with a message loop.");
        var control = (IMapWinGISControl5)automationObject;
        var completed = new TaskCompletionSource<long>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var completedIds = new HashSet<long>();
        long operationId = 0;

        GdalCompletedEvent onCompleted = id =>
        {
            if (id == operationId && operationId != 0)
            {
                completed.TrySetResult(id);
                return;
            }
            // A very small load can finish and post its notification while
            // BeginLoadVectorDataset is still returning on the STA thread.
            completedIds.Add(id);
        };

        ComEventsHelper.Combine(control, EventsInterfaceId, 2, onCompleted);
        try
        {
            operationId = control.BeginLoadVectorDataset(path);
            if (completedIds.Remove(operationId))
            {
                completed.TrySetResult(operationId);
            }
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
                                control.CancelLoadVectorDataset(operationId);
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
                control.CancelLoadVectorDataset(operationId);
            }

            long completedOperationId = 0;
            bool succeeded = false;
            bool cancelled = false;
            long layerId = 0;
            string summary = string.Empty;
            string diagnostics = string.Empty;
            while (completedOperationId == 0)
            {
                await Task.WhenAny(
                        completed.Task,
                        Task.Delay(10, cancellationToken))
                    .ConfigureAwait(true);
                try
                {
                    control.EndLoadVectorDataset(
                        operationId,
                        out succeeded,
                        out cancelled,
                        out layerId,
                        out summary,
                        out diagnostics);
                    completedOperationId = completed.Task.IsCompletedSuccessfully
                        ? await completed.Task.ConfigureAwait(true)
                        : operationId;
                }
                catch (COMException exception)
                    when (exception.HResult == HResultPending &&
                          !cancellationToken.IsCancellationRequested)
                {
                }
            }

            return new VectorLoadResult(
                operationId,
                completedOperationId,
                succeeded,
                cancelled,
                layerId,
                summary,
                diagnostics);
        }
        finally
        {
            ComEventsHelper.Remove(control, EventsInterfaceId, 2, onCompleted);
        }
    }
}
