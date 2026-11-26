using System.Runtime.InteropServices;
using ProvodkaNG.Core;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;
using Xunit;

namespace ProvodkaNG.Tests;

/// <summary>
/// Verifies exact binary C ABI parity between C# blittable structs and dsp_core_api.h,
/// plus live P/Invoke round-trip calls into DspCore.dll / libDspCore.so.
/// </summary>
public sealed class DspNativeAbiTests
{
    [Fact]
    public unsafe void NativeStructs_MatchExactCAbiSizesAndOffsets()
    {
        // Verified against g++ 12 / MSVC x64 natural alignment of dsp_core_api.h
        Assert.Equal(624, sizeof(DspConfigNative));
        Assert.Equal(40, sizeof(DspToolfaceEventNative));
        Assert.Equal(28, sizeof(DspVectorEventNative));
        Assert.Equal(576, sizeof(DspFrameDataNative));
        Assert.Equal(24, sizeof(DspSignalBlockNative));
        Assert.Equal(40, sizeof(DspCallbacksNative));

        Assert.Equal(4, (int)Marshal.OffsetOf<DspConfigNative>(nameof(DspConfigNative.PortOrPath)));
        Assert.Equal(264, (int)Marshal.OffsetOf<DspConfigNative>(nameof(DspConfigNative.UsoBitMode)));
        Assert.Equal(272, (int)Marshal.OffsetOf<DspConfigNative>(nameof(DspConfigNative.CarrierFreqHz)));
        Assert.Equal(316, (int)Marshal.OffsetOf<DspConfigNative>(nameof(DspConfigNative.FirFilePath)));
        Assert.Equal(576, (int)Marshal.OffsetOf<DspConfigNative>(nameof(DspConfigNative.RefHopNt)));
        Assert.Equal(616, (int)Marshal.OffsetOf<DspConfigNative>(nameof(DspConfigNative.UseNubt)));

        Assert.Equal(32, (int)Marshal.OffsetOf<DspToolfaceEventNative>(nameof(DspToolfaceEventNative.LatencyMs)));
        Assert.Equal(16, (int)Marshal.OffsetOf<DspVectorEventNative>(nameof(DspVectorEventNative.ReliabilityPct)));

        Assert.Equal(24, (int)Marshal.OffsetOf<DspFrameDataNative>(nameof(DspFrameDataNative.StepH)));
        Assert.Equal(48, (int)Marshal.OffsetOf<DspFrameDataNative>(nameof(DspFrameDataNative.OtkGravDeg)));
        Assert.Equal(312, (int)Marshal.OffsetOf<DspFrameDataNative>(nameof(DspFrameDataNative.VectorCount)));
        Assert.Equal(316, (int)Marshal.OffsetOf<DspFrameDataNative>(nameof(DspFrameDataNative.Vectors5Bit)));
        Assert.Equal(444, (int)Marshal.OffsetOf<DspFrameDataNative>(nameof(DspFrameDataNative.Reliabilities)));
    }

    [Fact]
    public void DspSettings_RoundTripsStringsIntoFixedByteBuffers()
    {
        var settings = new DspSettings
        {
            SourceType = DspSourceType.FilePlayer,
            PortOrPath = "/tek/smp/26-06-26 15-44-56.smp",
            FirFilePath = "/KIH5_4F.dat",
            FormatId = DspFrameFormat.Geomash,
            GainIndex = 4,
            CarrierFreqHz = 5.0,
            CorrectorEnabled = false,
            UseNubt = true
        };

        DspConfigNative native = settings.ToNative();
        DspSettings back = DspSettings.FromNative(in native);

        Assert.Equal(settings.PortOrPath, back.PortOrPath);
        Assert.Equal(settings.FirFilePath, back.FirFilePath);
        Assert.Equal(DspFrameFormat.Geomash, back.FormatId);
        Assert.Equal(4, back.GainIndex);
        Assert.False(back.CorrectorEnabled);
        Assert.True(back.UseNubt);
    }

    [Fact]
    public void NativeDspCore_ParseLegacyPackets_MatchesHardwareSpec()
    {
        // 24-bit packet: [0x00, b1, b2, b3, 0xFF]
        ReadOnlySpan<byte> pkt24 = [0x00, 0x12, 0x34, 0x56, 0xFF];
        var (status24, sample24) = DspEngineClient.ParseLegacyPacket(pkt24, 24);
        Assert.Equal(DspStatusCode.Ok, status24);
        Assert.Equal(0x12345600, sample24);

        // 32-bit uco32 packet with 0xFE overflow trailer
        ReadOnlySpan<byte> pkt32Overflow = [0x00, 0x01, 0x02, 0x03, 0x04, 0xFE];
        var (status32, sample32) = DspEngineClient.ParseLegacyPacket(pkt32Overflow, 32);
        Assert.Equal(DspStatusCode.Overflow, status32);
        Assert.Equal(0x01020304, sample32);
    }

    [Fact]
    public void NativeDspCore_CreatesEngineAndReportsSimdIsa()
    {
        DspSettings defaults = DspEngineClient.CreateDefaultSettings();
        using var client = new DspEngineClient(defaults);

        Assert.Contains(client.SimdIsa, new[] { "AVX2", "SSE2", "SCALAR" });
        Assert.True(client.CurrentStepH > 0.0);
    }
}
