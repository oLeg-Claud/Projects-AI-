using System.Runtime.InteropServices;
using System.Threading;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;

namespace ProvodkaNG.Core;

/// <summary>
/// High-level abstraction over the native C++17 DspCore engine.
/// Enables clean unit testing of UI ViewModels and optional offline mocks.
/// </summary>
public interface IDspEngineClient : IDisposable
{
    bool IsRunning { get; }
    string SimdIsa { get; }
    double CurrentStepH { get; }

    event EventHandler<ToolfaceReading>? ToolfaceReceived;
    event EventHandler<VectorReading>? VectorDecoded;
    event EventHandler<FrameSnapshot>? FrameCompleted;
    event EventHandler<SignalSnapshot>? SignalBlockReceived;

    DspStatusCode Configure(DspSettings settings);
    int LoadFirFile(string path);
    DspStatusCode Start();
    void Stop();
    DspStatusCode PushSamples(ReadOnlySpan<double> samples, bool overflowFlag = false);
    FrameSnapshot? GetLastFrame();

    DspStatusCode SetUsoV2Options(UsoV2Options options) => DspStatusCode.Ok;
    UsoRuntimeInfo? GetUsoInfo() => null;
    DspStatusCode RecalibrateUso() => DspStatusCode.NotFound;
    DspStatusCode ResetUsoAdc() => DspStatusCode.NotFound;
    DspStatusCode RefreshUsoDiagnostics() => DspStatusCode.NotFound;
    SyncCorrTraceSnapshot? GetSyncCorrTrace(int maxCount = 1024) => null;
}

/// <summary>
/// Thread-safe, GC-pinned managed facade around DspCore.dll / libDspCore.so.
/// Holds strong references to all native callback delegates for the entire lifetime
/// of the underlying C++ DspEngine* handle to prevent CallbackOnCollectedDelegate crashes.
/// </summary>
public sealed unsafe class DspEngineClient : IDspEngineClient
{
    private readonly ReaderWriterLockSlim _handleLock = new(LockRecursionPolicy.NoRecursion);
    private readonly object _lifecycleLock = new();
    private nint _handle;
    private bool _disposed;
    private volatile bool _isRunning;

    // Pinned delegate fields: MUST remain rooted while _handle != 0
    private readonly DspOnToolfaceNativeCallback _onToolfaceDelegate;
    private readonly DspOnVectorNativeCallback _onVectorDelegate;
    private readonly DspOnFrameNativeCallback _onFrameDelegate;
    private readonly DspOnSignalNativeCallback _onSignalDelegate;

    public bool IsRunning
    {
        get
        {
            if (!_isRunning || _disposed || _handle == nint.Zero)
                return false;
            return DspNativeMethods.IsRunning(_handle) != 0;
        }
    }
    public string SimdIsa => DspNativeMethods.GetSimdIsa();

    public double CurrentStepH
    {
        get
        {
            _handleLock.EnterReadLock();
            try
            {
                ThrowIfDisposed();
                return DspNativeMethods.GetStepH(_handle);
            }
            finally
            {
                _handleLock.ExitReadLock();
            }
        }
    }

    public event EventHandler<ToolfaceReading>? ToolfaceReceived;
    public event EventHandler<VectorReading>? VectorDecoded;
    public event EventHandler<FrameSnapshot>? FrameCompleted;
    public event EventHandler<SignalSnapshot>? SignalBlockReceived;

    public DspEngineClient(DspSettings? initialSettings = null)
    {
        DspNativeMethods.EnsureResolverRegistered();

        _onToolfaceDelegate = HandleToolfaceNative;
        _onVectorDelegate = HandleVectorNative;
        _onFrameDelegate = HandleFrameNative;
        _onSignalDelegate = HandleSignalNative;

        DspConfigNative nativeCfg;
        if (initialSettings is not null)
        {
            nativeCfg = initialSettings.ToNative();
        }
        else
        {
            DspNativeMethods.ConfigDefault(out nativeCfg);
        }

        var callbacks = new DspCallbacksNative
        {
            OnToolface = Marshal.GetFunctionPointerForDelegate(_onToolfaceDelegate),
            OnVector = Marshal.GetFunctionPointerForDelegate(_onVectorDelegate),
            OnFrame = Marshal.GetFunctionPointerForDelegate(_onFrameDelegate),
            OnSignal = Marshal.GetFunctionPointerForDelegate(_onSignalDelegate),
            UserData = nint.Zero
        };

        _handle = DspNativeMethods.Create(in nativeCfg, in callbacks);
        if (_handle == nint.Zero)
        {
            throw new InvalidOperationException("Failed to allocate native DspEngine instance.");
        }
    }

    public static DspSettings CreateDefaultSettings()
    {
        DspNativeMethods.EnsureResolverRegistered();
        DspNativeMethods.ConfigDefault(out DspConfigNative cfg);
        return DspSettings.FromNative(in cfg);
    }

    public static UsoProbeResult ProbeLegacyUso(string portName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(portName);
        DspNativeMethods.EnsureResolverRegistered();
        DspStatusCode status = DspNativeMethods.ProbeLegacyUso(portName, out int usoId);
        return new UsoProbeResult(portName, status == DspStatusCode.Ok, usoId, status);
    }

    /// <summary>
    /// Automatically enumerates all OS serial ports (Windows SERIALCOMM / Linux ttyUSB+ttyACM)
    /// and returns the first responding legacy USO32 port and ID.
    /// </summary>
    public static UsoProbeResult ScanLegacyUso()
    {
        DspNativeMethods.EnsureResolverRegistered();
        byte* portBuf = stackalloc byte[DspConstants.PathBufferSize];
        byte* diagBuf = stackalloc byte[512];
        DspStatusCode status = DspNativeMethods.ScanLegacyUsoDiag(
            portBuf, DspConstants.PathBufferSize, out int usoId, diagBuf, 512);
        string port = status == DspStatusCode.Ok
            ? DspConfigNative.ReadNullTerminatedUtf8(portBuf, DspConstants.PathBufferSize)
            : string.Empty;
        string diag = DspConfigNative.ReadNullTerminatedUtf8(diagBuf, 512);
        return new UsoProbeResult(port, status == DspStatusCode.Ok, usoId, status, diag);
    }

    public static (DspStatusCode Status, int Sample) ParseLegacyPacket(ReadOnlySpan<byte> packet, int bitMode)
    {
        DspNativeMethods.EnsureResolverRegistered();
        fixed (byte* ptr = packet)
        {
            DspStatusCode status = DspNativeMethods.ParseLegacyPacket(ptr, packet.Length, bitMode, out int sample);
            return (status, sample);
        }
    }

    public DspStatusCode Configure(DspSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            DspConfigNative nativeCfg = settings.ToNative();
            return DspNativeMethods.Configure(_handle, in nativeCfg);
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public int LoadFirFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            return DspNativeMethods.LoadFirFile(_handle, path);
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public DspStatusCode Start()
    {
        _handleLock.EnterReadLock();
        try
        {
            lock (_lifecycleLock)
            {
                ThrowIfDisposed();
                DspStatusCode status = DspNativeMethods.Start(_handle);
                if (status == DspStatusCode.Ok)
                    _isRunning = true;
                return status;
            }
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public void Stop()
    {
        _handleLock.EnterReadLock();
        try
        {
            lock (_lifecycleLock)
            {
                if (_disposed || _handle == nint.Zero)
                    return;
                DspNativeMethods.Stop(_handle);
                _isRunning = false;
            }
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public DspStatusCode PushSamples(ReadOnlySpan<double> samples, bool overflowFlag = false)
    {
        if (samples.IsEmpty)
            return DspStatusCode.Ok;

        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            fixed (double* ptr = samples)
            {
                return DspNativeMethods.PushSamples(_handle, ptr, samples.Length, overflowFlag ? 1 : 0);
            }
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public FrameSnapshot? GetLastFrame()
    {
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            DspStatusCode status = DspNativeMethods.GetLastFrame(_handle, out DspFrameDataNative native);
            return status == DspStatusCode.Ok ? FrameSnapshot.FromNative(in native) : null;
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public DspStatusCode SetUsoV2Options(UsoV2Options options)
    {
        ArgumentNullException.ThrowIfNull(options);
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            DspUsoV2OptionsNative native = options.ToNative();
            return DspNativeMethods.SetUsoV2Options(_handle, in native);
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public UsoRuntimeInfo? GetUsoInfo()
    {
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            DspStatusCode status = DspNativeMethods.GetUsoInfo(_handle, out DspUsoRuntimeInfoNative native);
            return status == DspStatusCode.Ok ? UsoRuntimeInfo.FromNative(in native) : null;
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public DspStatusCode RecalibrateUso()
    {
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            return DspNativeMethods.UsoRecalibrate(_handle);
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public DspStatusCode ResetUsoAdc()
    {
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            return DspNativeMethods.UsoResetAdc(_handle);
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public DspStatusCode RefreshUsoDiagnostics()
    {
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            return DspNativeMethods.UsoRefreshDiag(_handle);
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    public SyncCorrTraceSnapshot? GetSyncCorrTrace(int maxCount = 1024)
    {
        int cap = Math.Clamp(maxCount, 16, 2048);
        _handleLock.EnterReadLock();
        try
        {
            ThrowIfDisposed();
            int[] buf = new int[cap];
            fixed (int* ptr = buf)
            {
                int n = DspNativeMethods.GetSyncCorrTrace(
                    _handle,
                    ptr,
                    cap,
                    out int livePct,
                    out int lockedPct,
                    out int lockCount);
                if (n < 0)
                    return null;

                int[] trace = n == cap ? buf : buf[..n];
                return new SyncCorrTraceSnapshot(trace, livePct, lockedPct, lockCount);
            }
        }
        catch (EntryPointNotFoundException)
        {
            return null;
        }
        finally
        {
            _handleLock.ExitReadLock();
        }
    }

    private void HandleToolfaceNative(DspToolfaceEventNative* evt, nint userData)
    {
        if (evt == null || _disposed)
            return;

        var handler = ToolfaceReceived;
        if (handler is null)
            return;

        try
        {
            handler.Invoke(this, ToolfaceReading.FromNative(in *evt));
        }
        catch
        {
            // Never allow managed exceptions to unwind across native C++ stack frames.
        }
    }

    private void HandleVectorNative(DspVectorEventNative* evt, nint userData)
    {
        if (evt == null || _disposed)
            return;

        var handler = VectorDecoded;
        if (handler is null)
            return;

        try
        {
            handler.Invoke(this, VectorReading.FromNative(in *evt));
        }
        catch
        {
            // Prevent managed exception propagation across native ABI boundary.
        }
    }

    private void HandleFrameNative(DspFrameDataNative* frame, nint userData)
    {
        if (frame == null || _disposed)
            return;

        var handler = FrameCompleted;
        if (handler is null)
            return;

        try
        {
            handler.Invoke(this, FrameSnapshot.FromNative(in *frame));
        }
        catch
        {
            // Prevent managed exception propagation across native ABI boundary.
        }
    }

    private void HandleSignalNative(DspSignalBlockNative* block, nint userData)
    {
        if (block == null || block->SampleCount <= 0 || _disposed)
            return;

        var handler = SignalBlockReceived;
        if (handler is null)
            return;

        try
        {
            int count = block->SampleCount;
            double[] raw = new double[count];
            double[] filtered = new double[count];

            if (block->RawSamples != null)
            {
                new ReadOnlySpan<double>(block->RawSamples, count).CopyTo(raw);
            }
            if (block->FilteredSamples != null)
            {
                new ReadOnlySpan<double>(block->FilteredSamples, count).CopyTo(filtered);
            }

            handler.Invoke(this, new SignalSnapshot(raw, filtered, block->OverflowFlag != 0));
        }
        catch
        {
            // Prevent managed exception propagation across native ABI boundary.
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed || _handle == nint.Zero, this);
    }

    public void Dispose()
    {
        // Stop worker first so any waiting sync op (Recalibrate/ResetAdc) wakes up before acquiring write lock.
        try
        {
            Stop();
        }
        catch (ObjectDisposedException)
        {
            // Already disposed
        }

        _handleLock.EnterWriteLock();
        try
        {
            if (_disposed)
                return;
            _disposed = true;
            _isRunning = false;

            if (_handle != nint.Zero)
            {
                DspNativeMethods.Destroy(_handle);
                _handle = nint.Zero;
            }
        }
        finally
        {
            _handleLock.ExitWriteLock();
        }
        GC.SuppressFinalize(this);
    }

    ~DspEngineClient()
    {
        Dispose();
    }
}
