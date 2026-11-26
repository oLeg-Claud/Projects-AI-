using System.Runtime.InteropServices;
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
}

/// <summary>
/// Thread-safe, GC-pinned managed facade around DspCore.dll / libDspCore.so.
/// Holds strong references to all native callback delegates for the entire lifetime
/// of the underlying C++ DspEngine* handle to prevent CallbackOnCollectedDelegate crashes.
/// </summary>
public sealed unsafe class DspEngineClient : IDspEngineClient
{
    private readonly object _sync = new();
    private nint _handle;
    private bool _disposed;
    private volatile bool _isRunning;

    // Pinned delegate fields: MUST remain rooted while _handle != 0
    private readonly DspOnToolfaceNativeCallback _onToolfaceDelegate;
    private readonly DspOnVectorNativeCallback _onVectorDelegate;
    private readonly DspOnFrameNativeCallback _onFrameDelegate;
    private readonly DspOnSignalNativeCallback _onSignalDelegate;

    public bool IsRunning => _isRunning;
    public string SimdIsa => DspNativeMethods.GetSimdIsa();

    public double CurrentStepH
    {
        get
        {
            lock (_sync)
            {
                ThrowIfDisposed();
                return DspNativeMethods.GetStepH(_handle);
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
        lock (_sync)
        {
            ThrowIfDisposed();
            DspConfigNative nativeCfg = settings.ToNative();
            return DspNativeMethods.Configure(_handle, in nativeCfg);
        }
    }

    public int LoadFirFile(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        lock (_sync)
        {
            ThrowIfDisposed();
            return DspNativeMethods.LoadFirFile(_handle, path);
        }
    }

    public DspStatusCode Start()
    {
        lock (_sync)
        {
            ThrowIfDisposed();
            DspStatusCode status = DspNativeMethods.Start(_handle);
            if (status == DspStatusCode.Ok)
                _isRunning = true;
            return status;
        }
    }

    public void Stop()
    {
        lock (_sync)
        {
            if (_disposed || _handle == nint.Zero)
                return;
            DspNativeMethods.Stop(_handle);
            _isRunning = false;
        }
    }

    public DspStatusCode PushSamples(ReadOnlySpan<double> samples, bool overflowFlag = false)
    {
        if (samples.IsEmpty)
            return DspStatusCode.Ok;

        lock (_sync)
        {
            ThrowIfDisposed();
            fixed (double* ptr = samples)
            {
                return DspNativeMethods.PushSamples(_handle, ptr, samples.Length, overflowFlag ? 1 : 0);
            }
        }
    }

    public FrameSnapshot? GetLastFrame()
    {
        lock (_sync)
        {
            ThrowIfDisposed();
            DspStatusCode status = DspNativeMethods.GetLastFrame(_handle, out DspFrameDataNative native);
            return status == DspStatusCode.Ok ? FrameSnapshot.FromNative(in native) : null;
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
        lock (_sync)
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
        GC.SuppressFinalize(this);
    }

    ~DspEngineClient()
    {
        Dispose();
    }
}
