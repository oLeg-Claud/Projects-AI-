using ProvodkaNG.Core;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;
using ProvodkaNG.Desktop.Services;
using ProvodkaNG.Desktop.ViewModels;
using Xunit;

namespace ProvodkaNG.Tests;

/// <summary>
/// Headless unit tests for autonomous USO32 MainWindowViewModel, Dock layout,
/// oscilloscope/FFT updates, SP + 16-code correlation, workspace persistence, and Zero-Latency toolface updates.
/// </summary>
public sealed class MainWindowViewModelTests
{
    private sealed class FakeDspEngine : IDspEngineClient
    {
        public bool IsRunning { get; private set; }
        public string SimdIsa => "AVX2";
        public double CurrentStepH => 1.923077;
        public DspSettings? LastConfigured { get; private set; }
        public SyncCorrTraceSnapshot? NextSyncTrace { get; set; }

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
        public SyncCorrTraceSnapshot? GetSyncCorrTrace(int maxCount = 1024) => NextSyncTrace;
        public void Dispose() { IsRunning = false; }

        public void EmitToolface(ToolfaceReading reading) => ToolfaceReceived?.Invoke(this, reading);
        public void EmitVector(VectorReading reading) => VectorDecoded?.Invoke(this, reading);
        public void EmitFrame(FrameSnapshot frame) => FrameCompleted?.Invoke(this, frame);
        public void EmitSignal(SignalSnapshot signal) => SignalBlockReceived?.Invoke(this, signal);
    }

    [Fact]
    public async Task AutoSupervisor_DiscoversUso32AndStartsStreamingAutomatically()
    {
        var fake = new FakeDspEngine
        {
            NextSyncTrace = new SyncCorrTraceSnapshot([5, -12, 88, 10], 10, 88, 1)
        };
        DspSettings? capturedSettings = null;

        using var vm = new MainWindowViewModel(
            engineFactory: cfg =>
            {
                capturedSettings = cfg;
                return fake;
            },
            uiDispatcher: a => a(),
            usoScanner: () => new UsoProbeResult("COM4", true, 206, DspStatusCode.Ok),
            autoStart: true);

        // Wait briefly for background auto-discovery to connect
        for (int i = 0; i < 40 && !vm.IsConnected; i++)
            await Task.Delay(25);

        Assert.True(vm.IsConnected);
        Assert.Equal("COM4", vm.DetectedPort);
        Assert.Equal(206, vm.DetectedUsoId);
        Assert.NotNull(capturedSettings);
        Assert.Equal(DspSourceType.LegacySerial, capturedSettings!.SourceType);
        Assert.Equal(32, capturedSettings.UsoBitMode);
        Assert.NotNull(vm.Layout);

        // Push 128 samples and verify Oscilloscope, Spectrum, and live SP correlation trace update immediately
        double[] sig = new double[128];
        for (int i = 0; i < sig.Length; i++)
            sig[i] = 10_000_000.0 + 50_000.0 * Math.Sin(2.0 * Math.PI * 5.0 * i / 250.0);

        fake.EmitSignal(new SignalSnapshot(sig, sig, OverflowDetected: true));
        Assert.NotNull(vm.OscRawSamples);
        Assert.Equal(128, vm.TotalSamplesReceived);
        Assert.True(vm.AdcOverflowFlag);
        Assert.NotNull(vm.LatestSpectrum);
        Assert.NotNull(vm.LiveSyncCorrTrace);
        Assert.Equal(88, vm.LiveSyncPeakPct);
        Assert.Equal(88, vm.SyncCorrPct);
        Assert.Contains(88, vm.SyncCorrHistory);

        // Emit RM(1,4) vector with 16 correlation scores and verify Correlation panel data
        int[] scores = new int[16];
        scores[7] = -94;
        fake.EmitVector(new VectorReading(1, 0, 39, 0x17, 94, 0, 1, scores));
        Assert.Equal(0x17, vm.LatestWinningCode5Bit);
        Assert.NotNull(vm.LatestCodeScoresPct);
        Assert.Equal(-94, vm.LatestCodeScoresPct![7]);

        // Emit Zero-Latency Toolface
        fake.EmitToolface(new ToolfaceReading(1000, DspToolfaceChannel.Gravity, 0, 180.0, 92, 0.25));
        Assert.Equal(180.0, vm.ToolfaceAngleDeg);
        Assert.Equal(1, vm.ToolfaceUpdatesCount);
    }

    [Fact]
    public void LiveSettings_KeepNegotiatedSampleRate()
    {
        var fake = new FakeDspEngine();
        using var vm = new MainWindowViewModel(
            engineFactory: _ => fake, uiDispatcher: a => a(),
            usoScanner: () => new UsoProbeResult(string.Empty, false, 0, DspStatusCode.NotFound),
            autoStart: false);
        // File settings must remain independent of the UI's last live Fs.
        vm.ActiveSampleRateHz = 400;
        Assert.Equal(400, vm.BuildSettings(DspSourceType.LegacySerial, "COM10").SampleRateHz);
        Assert.Equal(250, vm.BuildSettings(DspSourceType.FilePlayer, "test.smp").SampleRateHz);
    }

    [Fact]
    public void ToggleShiftTheme_SwitchesDayAndNightLabels()
    {
        using var vm = new MainWindowViewModel(
            engineFactory: _ => new FakeDspEngine(),
            uiDispatcher: a => a(),
            usoScanner: () => new UsoProbeResult(string.Empty, false, 0, DspStatusCode.NotFound),
            autoStart: false);

        Assert.True(vm.IsDarkTheme);
        Assert.Equal("Ночная вахта", vm.ThemeButtonText);
        Assert.NotNull(vm.Layout);

        vm.ToggleShiftThemeCommand.Execute(null);

        Assert.False(vm.IsDarkTheme);
        Assert.Equal("Дневная вахта", vm.ThemeButtonText);
    }

    [Fact]
    public void WorkspaceState_SavesAndRestoresSettingsWindowAndLayout()
    {
        string tempPath = Path.Combine(Path.GetTempPath(), $"provodka_state_{Guid.NewGuid():N}.json");
        try
        {
            using (var vm1 = new MainWindowViewModel(
                engineFactory: _ => new FakeDspEngine(),
                uiDispatcher: a => a(),
                usoScanner: () => new UsoProbeResult(string.Empty, false, 0, DspStatusCode.NotFound),
                autoStart: false,
                stateFilePath: tempPath))
            {
                vm1.ToggleShiftThemeCommand.Execute(null);
                vm1.CarrierFreqHz = 16.0;
                vm1.GainIndex = 4;
                vm1.Ncod = 1;
                vm1.DcpThreshold = 68;
                vm1.CorrectorEnabled = false;
                vm1.SelectedFormat = vm1.FormatOptions[5]; // Geomash #30
                vm1.SaveWorkspaceState(new WindowPlacementDto
                {
                    Left = 120,
                    Top = 80,
                    Width = 1600,
                    Height = 960,
                    IsMaximized = true
                });
            }

            using var vm2 = new MainWindowViewModel(
                engineFactory: _ => new FakeDspEngine(),
                uiDispatcher: a => a(),
                usoScanner: () => new UsoProbeResult(string.Empty, false, 0, DspStatusCode.NotFound),
                autoStart: false,
                stateFilePath: tempPath);

            Assert.False(vm2.IsDarkTheme);
            Assert.Equal(16.0, vm2.CarrierFreqHz);
            Assert.Equal(4, vm2.GainIndex);
            Assert.Equal(1, vm2.Ncod);
            Assert.Equal(68, vm2.DcpThreshold);
            Assert.False(vm2.CorrectorEnabled);
            Assert.Equal(DspFrameFormat.Geomash, vm2.SelectedFormat.Format);
            Assert.NotNull(vm2.SavedWindowPlacement);
            Assert.Equal(120, vm2.SavedWindowPlacement!.Left);
            Assert.Equal(1600, vm2.SavedWindowPlacement.Width);
            Assert.True(vm2.SavedWindowPlacement.IsMaximized);
            Assert.NotNull(vm2.Layout);
        }
        finally
        {
            if (File.Exists(tempPath)) File.Delete(tempPath);
        }
    }
}
