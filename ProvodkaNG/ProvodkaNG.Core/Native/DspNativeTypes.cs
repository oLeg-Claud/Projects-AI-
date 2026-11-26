using System.Runtime.InteropServices;
using System.Text;

namespace ProvodkaNG.Core.Native;

/// <summary>
/// Constants mirroring dsp_core_api.h (TZ §3.2).
/// </summary>
public static class DspConstants
{
    public const int DlpolR = 13;
    public const int SyncChips = 128;
    public const int VecChips = 32;
    public const int SyncSamples = SyncChips * DlpolR; // 1664
    public const int VecSamples = VecChips * DlpolR;   // 416
    public const int MaxVectors = 128;
    public const int Rm14Codes = 16;
    public const int PathBufferSize = 260;
}

public enum DspSourceType : int
{
    LegacySerial = 0, // Legacy USO: 57600 8N1, 5-byte (24-bit) / 6-byte (32-bit uco32)
    NewAdc32 = 1,     // New 32-bit ADC USO: 8 samples/half-period (256 samples/vector)
    FilePlayer = 2    // Binary .smp (sun32) emulator with variable playback speed
}

public enum DspFrameFormat : int
{
    StdGk = 1,        // №1: ZTS + GK, no pauses (39 vectors)
    NdmGkRetr = 2,    // №2: GK + NDM + Repeater (104 vectors + 7 pauses)
    GkRetr = 11,      // №11: GK + Repeater (43 vectors + 4 pauses)
    Std = 12,         // №12: Short without GK (33 vectors)
    StdRetr = 13,     // №13: Without GK + Repeater (37 vectors + 4 pauses)
    Geomash = 30,     // №30: Geomash packet with CRC32 (34 vectors)
    GeomashRetr = 31  // №31: Geomash + Repeater with CRC32 (35 vectors + 1 pause)
}

public enum DspToolfaceChannel : int
{
    Gravity = 0,  // OTK1..7
    Magnetic = 1, // AZ_OTK1..7
    Static = 2    // OTKu
}

public enum DspStatusCode : int
{
    Ok = 0,
    InvalidArg = -1,
    PortOpenError = -2,
    FileOpenError = -3,
    AlreadyRunning = -4,
    NotFound = -5,
    Overflow = -6
}

/// <summary>
/// Blittable 1:1 mirror of C struct DspConfig (sizeof = 624 bytes, align = 8 on x64).
/// Uses fixed byte buffers instead of ByValTStr to avoid marshaling thunk overhead
/// and guarantee identical binary layout across Windows and Linux.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct DspConfigNative
{
    public DspSourceType SourceType;
    public fixed byte PortOrPath[DspConstants.PathBufferSize];
    public int UsoBitMode;
    public int GainIndex;
    public double CarrierFreqHz;
    public double SampleRateHz;
    public int CorrectorEnabled;
    public DspFrameFormat FormatId;
    public int Ncod;
    public int SearchWindowIg;
    public int DcpThreshold;
    public int EnableStepTrack;
    public int PlaybackSpeed;
    public fixed byte FirFilePath[DspConstants.PathBufferSize];

    // Reference geophysical parameters for surface inclinometry
    public double RefHopNt;
    public double RefDipDeg;
    public double RefDecDeg;
    public double RefAzOpDeg;
    public double KgkZts;
    public int UseNubt;

    public string GetPortOrPath()
    {
        fixed (byte* ptr = PortOrPath)
        {
            return ReadNullTerminatedUtf8(ptr, DspConstants.PathBufferSize);
        }
    }

    public void SetPortOrPath(string? value)
    {
        fixed (byte* ptr = PortOrPath)
        {
            WriteNullTerminatedUtf8(ptr, DspConstants.PathBufferSize, value);
        }
    }

    public string GetFirFilePath()
    {
        fixed (byte* ptr = FirFilePath)
        {
            return ReadNullTerminatedUtf8(ptr, DspConstants.PathBufferSize);
        }
    }

    public void SetFirFilePath(string? value)
    {
        fixed (byte* ptr = FirFilePath)
        {
            WriteNullTerminatedUtf8(ptr, DspConstants.PathBufferSize, value);
        }
    }

    internal static string ReadNullTerminatedUtf8(byte* buffer, int maxLen)
    {
        int len = 0;
        while (len < maxLen && buffer[len] != 0)
            len++;
        return len == 0 ? string.Empty : Encoding.UTF8.GetString(buffer, len);
    }

    internal static void WriteNullTerminatedUtf8(byte* buffer, int maxLen, string? value)
    {
        var span = new Span<byte>(buffer, maxLen);
        span.Clear();
        if (string.IsNullOrEmpty(value))
            return;

        byte[] bytes = Encoding.UTF8.GetBytes(value);
        int copyLen = Math.Min(bytes.Length, maxLen - 1);
        bytes.AsSpan(0, copyLen).CopyTo(span);
        span[copyLen] = 0;
    }
}

/// <summary>
/// Blittable mirror of DspToolfaceEvent (sizeof = 40 bytes, align = 8).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct DspToolfaceEventNative
{
    public long TimestampUs;
    public DspToolfaceChannel Channel;
    public int VectorIndex;
    public double AngleDeg;
    public int ReliabilityPct;
    public double LatencyMs;
}

/// <summary>
/// Blittable mirror of DspVectorEvent (sizeof = 92 bytes, align = 4).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct DspVectorEventNative
{
    public int FrameNumber;
    public int VectorIndex;
    public int TotalVectors;
    public byte Code5Bit;
    public int ReliabilityPct;
    public int PeakOffsetIg;
    public int PhasePolarity;
    public fixed int CodeScoresPct[DspConstants.Rm14Codes];
}

/// <summary>
/// Blittable mirror of DspFrameData (sizeof = 576 bytes, align = 8).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct DspFrameDataNative
{
    public int FrameNumber;
    public DspFrameFormat FormatId;
    public int IsStaticMode;
    public int ServiceMode;
    public int PhasePolarity;
    public int SyncCorrPct;
    public double StepH;
    public int LastPikDrift;
    public int QualStat;
    public int CrcValid;

    // ZTS Inclinometry & Toolface
    public double OtkGravDeg;
    public double OtkMagDeg;
    public double OtkStatDeg;
    public double ZuDeg;
    public double AzRawDeg;
    public double AzKorrDeg;
    public double AzTrueDeg;
    public double GkZts;
    public double GTotal;
    public double Gx;
    public double Gy;
    public double Gz;
    public int Hx;
    public int Hy;
    public int Hz;
    public double HTotal;
    public double DipCalcDeg;

    // ZTS Service
    public int RpmZts;
    public double TempZts;
    public double VoltZts;
    public int VibrZts;
    public int BatteryPct;
    public double KksSignalMv;

    // NDM (Near-Bit Module, Format 2)
    public double ZuNdmDeg;
    public double GkUpNdm;
    public double GkDnNdm;
    public double TempNdm;
    public int RpmNdm;
    public double WobNdm;
    public double TorqueNdm;
    public double PressNdm;
    public int TokNdm;
    public double PeakNdm;
    public double DuNdm;
    public double MahNdm;

    // Raw decoded vectors & reliabilities
    public int VectorCount;
    public fixed byte Vectors5Bit[DspConstants.MaxVectors];
    public fixed byte Reliabilities[DspConstants.MaxVectors];
}

/// <summary>
/// Blittable mirror of DspSignalBlock (sizeof = 24 bytes, align = 8 on x64).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public unsafe struct DspSignalBlockNative
{
    public double* RawSamples;
    public double* FilteredSamples;
    public int SampleCount;
    public int OverflowFlag;
}

/// <summary>
/// Native callback delegates matching DSPCORE_CALL (__stdcall on Win32, default ABI on x64).
/// </summary>
[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public unsafe delegate void DspOnToolfaceNativeCallback(DspToolfaceEventNative* evt, nint userData);

[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public unsafe delegate void DspOnVectorNativeCallback(DspVectorEventNative* evt, nint userData);

[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public unsafe delegate void DspOnFrameNativeCallback(DspFrameDataNative* frame, nint userData);

[UnmanagedFunctionPointer(CallingConvention.StdCall)]
public unsafe delegate void DspOnSignalNativeCallback(DspSignalBlockNative* block, nint userData);

/// <summary>
/// Mirror of C struct DspCallbacks (sizeof = 40 bytes on x64).
/// Uses raw function pointers from Marshal.GetFunctionPointerForDelegate with pinned delegates.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct DspCallbacksNative
{
    public nint OnToolface;
    public nint OnVector;
    public nint OnFrame;
    public nint OnSignal;
    public nint UserData;
}
