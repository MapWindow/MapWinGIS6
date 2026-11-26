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
    long? CompletedOperationId,
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

    /// <summary>
    /// Loads a vector dataset without blocking the UI thread.
    /// </summary>
    /// <remarks>
    /// <paramref name="onCompleted"/> runs synchronously inside the control's
    /// completion event, after the layer was committed and before the UI
    /// thread can process the resulting paint. Cancellation is a request:
    /// the method still waits for <c>EndLoadVectorDataset</c> so that the
    /// control releases the operation.
    /// <see cref="VectorLoadResult.CompletedOperationId"/> is set only when
    /// the completion event was observed.
    /// </remarks>
    public static async Task<VectorLoadResult> LoadAsync(
        object automationObject,
        string path,
        CancellationToken cancellationToken,
        Action? onCompleted = null)
    {
        var synchronizationContext = SynchronizationContext.Current ??
            throw new InvalidOperationException(
                "Start the vector load from a UI thread with a message loop.");
        var control = (IMapWinGISControl5)automationObject;
        var completed = new TaskCompletionSource<long>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var completedIds = new HashSet<long>();
        long operationId = 0;

        GdalCompletedEvent onCompletedEvent = id =>
        {
            if (id == operationId && operationId != 0)
            {
                if (completed.TrySetResult(id))
                {
                    onCompleted?.Invoke();
                }
                return;
            }
            // A very small load can finish and post its notification while
            // BeginLoadVectorDataset is still returning on the STA thread.
            completedIds.Add(id);
        };

        ComEventsHelper.Combine(control, EventsInterfaceId, 2, onCompletedEvent);
        try
        {
            operationId = control.BeginLoadVectorDataset(path);
            if (completedIds.Remove(operationId) &&
                completed.TrySetResult(operationId))
            {
                onCompleted?.Invoke();
            }
            Exception? cancellationFailure = null;
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
                                cancellationFailure = exception;
                            }
                        },
                        null));

            bool succeeded;
            bool cancelled;
            long layerId;
            string summary;
            string diagnostics;
            while (true)
            {
                try
                {
                    control.EndLoadVectorDataset(
                        operationId,
                        out succeeded,
                        out cancelled,
                        out layerId,
                        out summary,
                        out diagnostics);
                    break;
                }
                catch (COMException exception)
                    when (exception.HResult == HResultPending)
                {
                }

                if (cancellationFailure is not null)
                {
                    throw new InvalidOperationException(
                        "The vector load could not be cancelled.",
                        cancellationFailure);
                }
                // Cancellation must not shorten this wait: only a successful
                // End call releases the native operation slot.
                if (completed.Task.IsCompleted)
                {
                    await Task.Delay(10).ConfigureAwait(true);
                }
                else
                {
                    await Task.WhenAny(completed.Task, Task.Delay(10))
                        .ConfigureAwait(true);
                }
            }

            return new VectorLoadResult(
                operationId,
                completed.Task.IsCompletedSuccessfully
                    ? completed.Task.Result
                    : null,
                succeeded,
                cancelled,
                layerId,
                summary,
                diagnostics);
        }
        finally
        {
            ComEventsHelper.Remove(control, EventsInterfaceId, 2, onCompletedEvent);
        }
    }
}
