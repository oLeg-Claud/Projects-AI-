using ProvodkaNG.Core;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;
using ProvodkaNG.Desktop.ViewModels;
using Xunit;

namespace ProvodkaNG.Tests;

/// <summary>
/// Headless unit tests for autonomous USO32 MainWindowViewModel, Dock layout,
/// oscilloscope/FFT updates, 16-code correlation, and Zero-Latency toolface updates.
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
        public void EmitSignal(SignalSnapshot signal) => SignalBlockReceived?.Invoke(this, signal);
    }

    [Fact]
    public async Task AutoSupervisor_DiscoversUso32AndStartsStreamingAutomatically()
    {
        var fake = new FakeDspEngine();
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

        // Push 128 samples and verify Oscilloscope + Spectrum update immediately
        double[] sig = new double[128];
        for (int i = 0; i < sig.Length; i++)
            sig[i] = 10_000_000.0 + 50_000.0 * Math.Sin(2.0 * Math.PI * 5.0 * i / 250.0);

        fake.EmitSignal(new SignalSnapshot(sig, sig, OverflowDetected: true));
        Assert.NotNull(vm.OscRawSamples);
        Assert.Equal(128, vm.TotalSamplesReceived);
        Assert.True(vm.AdcOverflowFlag);
        Assert.NotNull(vm.LatestSpectrum);

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
}
