using ProvodkaNG.Core;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;
using ProvodkaNG.Desktop.ViewModels;
using Xunit;

namespace ProvodkaNG.Tests;

/// <summary>
/// Headless unit tests for MainWindowViewModel using a deterministic fake IDspEngineClient.
/// </summary>
public sealed class MainWindowViewModelTests
{
    private sealed class FakeDspEngine : IDspEngineClient
    {
        public bool IsRunning { get; private set; }
        public string SimdIsa => "AVX2";
        public double CurrentStepH => 1.923077;
        public DspSettings? LastConfigured { get; private set; }

        public event EventHandler<ToolfaceReading>? ToolfaceReceived;
        public event EventHandler<VectorReading>? VectorDecoded;
        public event EventHandler<FrameSnapshot>? FrameCompleted;
        public event EventHandler<SignalSnapshot>? SignalBlockReceived;

        public DspStatusCode Configure(DspSettings settings)
        {
            LastConfigured = settings;
            return DspStatusCode.Ok;
        }

        public int LoadFirFile(string path) => 3887;
        public DspStatusCode Start() { IsRunning = true; return DspStatusCode.Ok; }
        public void Stop() { IsRunning = false; }
        public DspStatusCode PushSamples(ReadOnlySpan<double> samples, bool overflowFlag = false) => DspStatusCode.Ok;
        public FrameSnapshot? GetLastFrame() => null;
        public void Dispose() { IsRunning = false; }

        public void EmitToolface(ToolfaceReading reading) => ToolfaceReceived?.Invoke(this, reading);
        public void EmitVector(VectorReading reading) => VectorDecoded?.Invoke(this, reading);
        public void EmitFrame(FrameSnapshot frame) => FrameCompleted?.Invoke(this, frame);
    }

    [Fact]
    public void ZeroLatencyToolface_UpdatesImmediatelyAndMaintainsTrailHistory()
    {
        var fake = new FakeDspEngine();
        using var vm = new MainWindowViewModel(_ => fake, uiDispatcher: a => a());

        vm.StartAcquisition();
        Assert.True(vm.IsRunning);
        Assert.Equal("AVX2", vm.SimdIsa);

        for (int i = 0; i < 8; i++)
        {
            fake.EmitToolface(new ToolfaceReading(
                TimestampUs: i * 1000,
                Channel: DspToolfaceChannel.Gravity,
                VectorIndex: i,
                AngleDeg: i * 45.0,
                ReliabilityPct: 92,
                LatencyMs: 0.35));
        }

        Assert.Equal(8, vm.ToolfaceUpdatesCount);
        Assert.Equal(315.0, vm.ToolfaceAngleDeg);
        Assert.Equal(6, vm.ToolfaceHistory.Count);
        Assert.Equal(90.0, vm.ToolfaceHistory[0]);
        Assert.Equal(315.0, vm.ToolfaceHistory[^1]);
    }

    [Fact]
    public void FrameCompletion_PopulatesInclinometryAndNdmVisibility()
    {
        var fake = new FakeDspEngine();
        using var vm = new MainWindowViewModel(_ => fake, uiDispatcher: a => a());

        vm.StartAcquisition();

        var frame = new FrameSnapshot(
            FrameNumber: 12,
            FormatId: DspFrameFormat.NdmGkRetr,
            IsStaticMode: true,
            ServiceMode: 1,
            PhasePolarity: 1,
            SyncCorrPct: 96,
            StepH: 1.9241,
            LastPikDrift: -2,
            QualStat: 1,
            CrcValid: -1,
            OtkGravDeg: 180.0,
            OtkMagDeg: 95.0,
            OtkStatDeg: 181.2,
            ZuDeg: 42.5,
            AzRawDeg: 123.4,
            AzKorrDeg: 124.0,
            AzTrueDeg: 126.8,
            GkZts: 58.2,
            GTotal: 1.001,
            Gx: 0.1, Gy: 0.2, Gz: 0.9,
            Hx: 12000, Hy: 8000, Hz: 44000,
            HTotal: 46500.0,
            DipCalcDeg: 71.1,
            RpmZts: 110,
            TempZts: 78.5,
            VoltZts: 28.4,
            VibrZts: 2,
            BatteryPct: 88,
            KksSignalMv: 45.0,
            ZuNdmDeg: 43.1,
            GkUpNdm: 61.0,
            GkDnNdm: 54.0,
            TempNdm: 80.0,
            RpmNdm: 110,
            WobNdm: 12.5,
            TorqueNdm: 4.8,
            PressNdm: 185.0,
            TokNdm: 140,
            PeakNdm: 3.1,
            DuNdm: 1.2,
            MahNdm: 2100.0,
            Vectors5Bit: [1, 2, 3],
            Reliabilities: [95, 90, 88]);

        fake.EmitFrame(frame);

        Assert.Equal(12, vm.FrameNumber);
        Assert.Equal("СТАТИКА", vm.ModeBadge);
        Assert.Equal(42.5, vm.ZuDeg);
        Assert.Equal(126.8, vm.AzimuthDeg);
        Assert.True(vm.IsNdmVisible);
        Assert.Equal(12.5, vm.WobNdm);
    }

    [Fact]
    public void ToggleShiftTheme_SwitchesDayAndNightLabels()
    {
        using var vm = new MainWindowViewModel(_ => new FakeDspEngine(), uiDispatcher: a => a());

        Assert.True(vm.IsDarkTheme);
        Assert.Equal("Ночная вахта", vm.ThemeButtonText);

        vm.ToggleShiftThemeCommand.Execute(null);

        Assert.False(vm.IsDarkTheme);
        Assert.Equal("Дневная вахта", vm.ThemeButtonText);
    }
}
