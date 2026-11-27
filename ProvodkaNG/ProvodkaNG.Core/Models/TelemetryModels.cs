using ProvodkaNG.Core.Native;

namespace ProvodkaNG.Core.Models;

/// <summary>
/// Managed configuration object mapped cleanly to/from blittable DspConfigNative.
/// </summary>
public sealed class DspSettings
{
    public DspSourceType SourceType { get; set; } = DspSourceType.FilePlayer;
    public string PortOrPath { get; set; } = string.Empty;
    public int UsoBitMode { get; set; } = 24;
    public int GainIndex { get; set; } = 0;
    public double CarrierFreqHz { get; set; } = 5.0;
    public double SampleRateHz { get; set; } = 250.0;
    public bool CorrectorEnabled { get; set; } = true;
    public DspFrameFormat FormatId { get; set; } = DspFrameFormat.StdGk;
    public int Ncod { get; set; } = 0;
    public int SearchWindowIg { get; set; } = 13;
    public int DcpThreshold { get; set; } = 55;
    public bool EnableStepTrack { get; set; } = true;
    public int PlaybackSpeed { get; set; } = 1;
    public string FirFilePath { get; set; } = string.Empty;

    public double RefHopNt { get; set; } = 46500.0;
    public double RefDipDeg { get; set; } = 71.0;
    public double RefDecDeg { get; set; } = 0.0;
    public double RefAzOpDeg { get; set; } = 0.0;
    public double KgkZts { get; set; } = 1.0;
    public bool UseNubt { get; set; } = false;

    public DspConfigNative ToNative()
    {
        var native = new DspConfigNative
        {
            SourceType = SourceType,
            UsoBitMode = UsoBitMode,
            GainIndex = Math.Clamp(GainIndex, 0, 5),
            CarrierFreqHz = CarrierFreqHz,
            SampleRateHz = SampleRateHz,
            CorrectorEnabled = CorrectorEnabled ? 1 : 0,
            FormatId = FormatId,
            Ncod = Ncod == 0 ? 0 : 1,
            SearchWindowIg = Math.Clamp(SearchWindowIg, 1, 52),
            DcpThreshold = Math.Clamp(DcpThreshold, 0, 100),
            EnableStepTrack = EnableStepTrack ? 1 : 0,
            PlaybackSpeed = Math.Max(0, PlaybackSpeed),
            RefHopNt = RefHopNt,
            RefDipDeg = RefDipDeg,
            RefDecDeg = RefDecDeg,
            RefAzOpDeg = RefAzOpDeg,
            KgkZts = KgkZts,
            UseNubt = UseNubt ? 1 : 0
        };
        native.SetPortOrPath(PortOrPath);
        native.SetFirFilePath(FirFilePath);
        return native;
    }

    public static DspSettings FromNative(in DspConfigNative native)
    {
        var copy = native;
        return new DspSettings
        {
            SourceType = copy.SourceType,
            PortOrPath = copy.GetPortOrPath(),
            UsoBitMode = copy.UsoBitMode,
            GainIndex = copy.GainIndex,
            CarrierFreqHz = copy.CarrierFreqHz,
            SampleRateHz = copy.SampleRateHz,
            CorrectorEnabled = copy.CorrectorEnabled != 0,
            FormatId = copy.FormatId,
            Ncod = copy.Ncod,
            SearchWindowIg = copy.SearchWindowIg,
            DcpThreshold = copy.DcpThreshold,
            EnableStepTrack = copy.EnableStepTrack != 0,
            PlaybackSpeed = copy.PlaybackSpeed,
            FirFilePath = copy.GetFirFilePath(),
            RefHopNt = copy.RefHopNt,
            RefDipDeg = copy.RefDipDeg,
            RefDecDeg = copy.RefDecDeg,
            RefAzOpDeg = copy.RefAzOpDeg,
            KgkZts = copy.KgkZts,
            UseNubt = copy.UseNubt != 0
        };
    }
}

public sealed record ToolfaceReading(
    long TimestampUs,
    DspToolfaceChannel Channel,
    int VectorIndex,
    double AngleDeg,
    int ReliabilityPct,
    double LatencyMs)
{
    public static ToolfaceReading FromNative(in DspToolfaceEventNative evt) =>
        new(evt.TimestampUs, evt.Channel, evt.VectorIndex, evt.AngleDeg, evt.ReliabilityPct, evt.LatencyMs);
}

public sealed record VectorReading(
    int FrameNumber,
    int VectorIndex,
    int TotalVectors,
    byte Code5Bit,
    int ReliabilityPct,
    int PeakOffsetIg,
    int PhasePolarity,
    int[] CodeScoresPct)
{
    public static unsafe VectorReading FromNative(in DspVectorEventNative evt)
    {
        var copy = evt;
        int[] scores = new int[DspConstants.Rm14Codes];
        for (int i = 0; i < DspConstants.Rm14Codes; i++)
            scores[i] = copy.CodeScoresPct[i];

        return new VectorReading(
            copy.FrameNumber,
            copy.VectorIndex,
            copy.TotalVectors,
            copy.Code5Bit,
            copy.ReliabilityPct,
            copy.PeakOffsetIg,
            copy.PhasePolarity,
            scores);
    }
}

public sealed record SignalSnapshot(
    double[] RawSamples,
    double[] FilteredSamples,
    bool OverflowDetected);

public sealed record FrameSnapshot(
    int FrameNumber,
    DspFrameFormat FormatId,
    bool IsStaticMode,
    int ServiceMode,
    int PhasePolarity,
    int SyncCorrPct,
    double StepH,
    int LastPikDrift,
    int QualStat,
    int CrcValid,
    double OtkGravDeg,
    double OtkMagDeg,
    double OtkStatDeg,
    double ZuDeg,
    double AzRawDeg,
    double AzKorrDeg,
    double AzTrueDeg,
    double GkZts,
    double GTotal,
    double Gx,
    double Gy,
    double Gz,
    int Hx,
    int Hy,
    int Hz,
    double HTotal,
    double DipCalcDeg,
    int RpmZts,
    double TempZts,
    double VoltZts,
    int VibrZts,
    int BatteryPct,
    double KksSignalMv,
    double ZuNdmDeg,
    double GkUpNdm,
    double GkDnNdm,
    double TempNdm,
    int RpmNdm,
    double WobNdm,
    double TorqueNdm,
    double PressNdm,
    int TokNdm,
    double PeakNdm,
    double DuNdm,
    double MahNdm,
    byte[] Vectors5Bit,
    byte[] Reliabilities)
{
    public static unsafe FrameSnapshot FromNative(in DspFrameDataNative native)
    {
        var copy = native;
        int count = Math.Clamp(copy.VectorCount, 0, DspConstants.MaxVectors);
        byte[] vecs = new byte[count];
        byte[] rels = new byte[count];
        for (int i = 0; i < count; i++)
        {
            vecs[i] = copy.Vectors5Bit[i];
            rels[i] = copy.Reliabilities[i];
        }

        return new FrameSnapshot(
            copy.FrameNumber,
            copy.FormatId,
            copy.IsStaticMode != 0,
            copy.ServiceMode,
            copy.PhasePolarity,
            copy.SyncCorrPct,
            copy.StepH,
            copy.LastPikDrift,
            copy.QualStat,
            copy.CrcValid,
            copy.OtkGravDeg,
            copy.OtkMagDeg,
            copy.OtkStatDeg,
            copy.ZuDeg,
            copy.AzRawDeg,
            copy.AzKorrDeg,
            copy.AzTrueDeg,
            copy.GkZts,
            copy.GTotal,
            copy.Gx,
            copy.Gy,
            copy.Gz,
            copy.Hx,
            copy.Hy,
            copy.Hz,
            copy.HTotal,
            copy.DipCalcDeg,
            copy.RpmZts,
            copy.TempZts,
            copy.VoltZts,
            copy.VibrZts,
            copy.BatteryPct,
            copy.KksSignalMv,
            copy.ZuNdmDeg,
            copy.GkUpNdm,
            copy.GkDnNdm,
            copy.TempNdm,
            copy.RpmNdm,
            copy.WobNdm,
            copy.TorqueNdm,
            copy.PressNdm,
            copy.TokNdm,
            copy.PeakNdm,
            copy.DuNdm,
            copy.MahNdm,
            vecs,
            rels);
    }
}

public sealed record UsoProbeResult(
    string PortName,
    bool Detected,
    int UsoId,
    DspStatusCode Status,
    string Diagnostics = "");
