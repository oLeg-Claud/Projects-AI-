using System.Reflection;
using System.Runtime.InteropServices;

namespace ProvodkaNG.Core.Native;

/// <summary>
/// Cross-platform P/Invoke bindings to DspCore.dll (Windows) / libDspCore.so (Linux) / libDspCore.dylib (macOS).
/// Registers a custom DllImportResolver to locate the native binary across standard build & deployment paths.
/// </summary>
public static unsafe class DspNativeMethods
{
    public const string LibraryName = "DspCore";

    private static int _resolverInitialized;

    static DspNativeMethods()
    {
        EnsureResolverRegistered();
    }

    public static void EnsureResolverRegistered()
    {
        if (Interlocked.Exchange(ref _resolverInitialized, 1) == 0)
        {
            NativeLibrary.SetDllImportResolver(typeof(DspNativeMethods).Assembly, ResolveLibrary);
        }
    }

    private static nint ResolveLibrary(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, LibraryName, StringComparison.Ordinal))
            return nint.Zero;

        // First try default OS resolution (next to executable, PATH / LD_LIBRARY_PATH)
        if (NativeLibrary.TryLoad(libraryName, assembly, searchPath, out nint handle))
            return handle;

        string fileName = OperatingSystem.IsWindows()
            ? "DspCore.dll"
            : OperatingSystem.IsMacOS()
                ? "libDspCore.dylib"
                : "libDspCore.so";

        string baseDir = AppContext.BaseDirectory;
        string[] candidates =
        [
            Path.Combine(baseDir, fileName),
            Path.Combine(baseDir, "runtimes", RuntimeInformation.RuntimeIdentifier, "native", fileName),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "out", "build", "x64-Debug", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "out", "build", "x64-Release", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "out", "build", "Debug", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "out", "build", "Release", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "out", "build", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "build", "Debug", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "build", "Release", fileName)),
            Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "dsp_core", "build", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "out", "build", "x64-Debug", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "out", "build", "x64-Release", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "out", "build", "Debug", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "out", "build", "Release", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "out", "build", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "build", "Debug", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "build", "Release", fileName)),
            Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "dsp_core", "build", fileName))
        ];

        foreach (string path in candidates)
        {
            if (File.Exists(path) && NativeLibrary.TryLoad(path, out handle))
                return handle;
        }

        return nint.Zero;
    }

    [DllImport(LibraryName, EntryPoint = "dsp_config_default", CallingConvention = CallingConvention.StdCall)]
    public static extern void ConfigDefault(out DspConfigNative cfg);

    [DllImport(LibraryName, EntryPoint = "dsp_create", CallingConvention = CallingConvention.StdCall)]
    public static extern nint Create(in DspConfigNative cfg, in DspCallbacksNative callbacks);

    [DllImport(LibraryName, EntryPoint = "dsp_destroy", CallingConvention = CallingConvention.StdCall)]
    public static extern void Destroy(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_configure", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode Configure(nint engine, in DspConfigNative cfg);

    [DllImport(LibraryName, EntryPoint = "dsp_load_fir_file", CallingConvention = CallingConvention.StdCall, CharSet = CharSet.Ansi)]
    public static extern int LoadFirFile(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

    [DllImport(LibraryName, EntryPoint = "dsp_start", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode Start(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_stop", CallingConvention = CallingConvention.StdCall)]
    public static extern void Stop(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_push_samples", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode PushSamples(nint engine, double* samples, int count, int overflowFlag);

    [DllImport(LibraryName, EntryPoint = "dsp_get_last_frame", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode GetLastFrame(nint engine, out DspFrameDataNative outFrame);

    [DllImport(LibraryName, EntryPoint = "dsp_get_step_h", CallingConvention = CallingConvention.StdCall)]
    public static extern double GetStepH(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_get_simd_isa", CallingConvention = CallingConvention.StdCall)]
    public static extern nint GetSimdIsaPtr();

    public static string GetSimdIsa()
    {
        nint ptr = GetSimdIsaPtr();
        return ptr == nint.Zero ? "UNKNOWN" : Marshal.PtrToStringAnsi(ptr) ?? "UNKNOWN";
    }

    [DllImport(LibraryName, EntryPoint = "dsp_is_running", CallingConvention = CallingConvention.StdCall)]
    public static extern int IsRunning(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_probe_legacy_uso", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode ProbeLegacyUso(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string portName,
        out int outUsoId);

    [DllImport(LibraryName, EntryPoint = "dsp_scan_legacy_uso", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode ScanLegacyUso(
        byte* outPortBuf,
        int bufLen,
        out int outUsoId);

    [DllImport(LibraryName, EntryPoint = "dsp_scan_legacy_uso_diag", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode ScanLegacyUsoDiag(
        byte* outPortBuf,
        int bufLen,
        out int outUsoId,
        byte* outDiagBuf,
        int diagLen);

    [DllImport(LibraryName, EntryPoint = "dsp_parse_legacy_packet", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode ParseLegacyPacket(
        byte* packet,
        int packetLen,
        int bitMode,
        out int outSample);

    [DllImport(LibraryName, EntryPoint = "dsp_uso_v2_options_default", CallingConvention = CallingConvention.StdCall)]
    public static extern void UsoV2OptionsDefault(out DspUsoV2OptionsNative opts);

    [DllImport(LibraryName, EntryPoint = "dsp_set_uso_v2_options", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode SetUsoV2Options(nint engine, in DspUsoV2OptionsNative opts);

    [DllImport(LibraryName, EntryPoint = "dsp_get_uso_info", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode GetUsoInfo(nint engine, out DspUsoRuntimeInfoNative outInfo);

    [DllImport(LibraryName, EntryPoint = "dsp_uso_recalibrate", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode UsoRecalibrate(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_uso_reset_adc", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode UsoResetAdc(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_uso_refresh_diag", CallingConvention = CallingConvention.StdCall)]
    public static extern DspStatusCode UsoRefreshDiag(nint engine);

    [DllImport(LibraryName, EntryPoint = "dsp_get_sync_corr_trace", CallingConvention = CallingConvention.StdCall)]
    public static extern int GetSyncCorrTrace(
        nint engine,
        int* outTrace,
        int maxCount,
        out int outLivePct,
        out int outLockedPct,
        out int outLockCount);
}
