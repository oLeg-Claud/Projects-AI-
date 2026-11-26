using System.Collections.ObjectModel;
using System.Globalization;
using Avalonia;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Dock.Model.Controls;
using ProvodkaNG.Core;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;
using ProvodkaNG.Desktop.Docking;

namespace ProvodkaNG.Desktop.ViewModels;

public sealed record FormatOption(DspFrameFormat Format, string DisplayName);
public sealed record SpeedOption(int Multiplier, string DisplayName);

public sealed record VectorCellViewModel(
    int Index,
    byte Code5Bit,
    int ReliabilityPct,
    bool IsReliable)
{
    private static readonly IBrush ReliableBrush = new SolidColorBrush(Color.Parse("#10B981"));
    private static readonly IBrush UnreliableBrush = new SolidColorBrush(Color.Parse("#EF4444"));

    public string CodeHex => Code5Bit.ToString("X2", CultureInfo.InvariantCulture);
    public IBrush StatusBrush => IsReliable ? ReliableBrush : UnreliableBrush;
}

/// <summary>
/// Primary MVVM ViewModel for ПРОВОДКА NG Desktop Shell (Stage 2).
/// - Exclusively targets legacy USO32 (uco32: 57600 8N1, 6-byte packets, gain 0x90..0x95, freq 0..5, corr 0x71/0x72).
/// - Fully automatic lifecycle (NO manual Start button): auto-discovers USO32 on all COM/ttyUSB ports,
///   auto-starts acquisition, and automatically re-initializes on cable disconnect or stream stall.
/// - Supports opening a legacy .smp recording as a temporary player override; closing returns to live USO32.
/// - Drives Oscilloscope, FFT Spectrum, RM(1,4) Correlation, and Zero-Latency Toolface windows inside Avalonia Dock Manager.
/// </summary>
public sealed partial class MainWindowViewModel : ObservableObject, IDisposable
{
    private const int SignalWindowLength = 1024;
    private const int FftWindowLength = 512;
    private const int MaxToolfaceTrail = 6;
    private const int MaxSyncHistory = 40;
    private const int StreamWatchdogTimeoutMs = 3000;

    private readonly Action<Action> _dispatchToUi;
    private readonly Func<DspSettings, IDspEngineClient> _engineFactory;
    private readonly Func<UsoProbeResult> _usoScanner;
    private readonly object _signalLock = new();
    private readonly double[] _rawRing = new double[SignalWindowLength];
    private readonly double[] _firRing = new double[SignalWindowLength];
    private readonly CancellationTokenSource _supervisorCts = new();
    private readonly ProvodkaDockFactory _dockFactory;

    private int _ringHead;
    private int _ringFilled;
    private bool _signalDirty;
    private bool _latestOverflow;
    private long _totalSamples;
    private long _lastSampleTickMs;
    private volatile bool _forceReinitRequested;
    private volatile string? _pendingSmpPath;
    private volatile bool _isSmpMode;

    private IDspEngineClient? _engine;
    private Task? _supervisorTask;
    private DispatcherTimer? _uiPlotTimer;
    private bool _disposed;

    public Func<Task<string?>>? PickSmpFileAsync { get; set; }
    public Func<Task<string?>>? PickFirFileAsync { get; set; }
    public Action? RequestExit { get; set; }

    public IReadOnlyList<FormatOption> FormatOptions { get; } =
    [
        new(DspFrameFormat.StdGk, "№1 — ЗТС + ГК (39 вект.)"),
        new(DspFrameFormat.NdmGkRetr, "№2 — ГК + НДМ + Ретр. (104 вект.)"),
        new(DspFrameFormat.GkRetr, "№11 — ЗТС + ГК + Ретр. (43 вект.)"),
        new(DspFrameFormat.Std, "№12 — ЗТС короткий (33 вект.)"),
        new(DspFrameFormat.StdRetr, "№13 — ЗТС + Ретр. (37 вект.)"),
        new(DspFrameFormat.Geomash, "№30 — Геомаш CRC32 (34 вект.)"),
        new(DspFrameFormat.GeomashRetr, "№31 — Геомаш + Ретр. (35 вект.)")
    ];

    public IReadOnlyList<SpeedOption> SpeedOptions { get; } =
    [
        new(1, "x1 (Реал. время)"),
        new(2, "x2"),
        new(4, "x4"),
        new(8, "x8"),
        new(0, "MAX")
    ];

    public IReadOnlyList<double> CarrierFrequencies { get; } = [16.0, 10.0, 7.0, 5.0, 3.6, 2.5, 1.8];
    public IReadOnlyList<int> GainIndices { get; } = [0, 1, 2, 3, 4, 5];
    public IReadOnlyList<int> Codebooks { get; } = [0, 1];

    public ObservableCollection<VectorCellViewModel> DecodedVectors { get; } = [];

    // --- Avalonia Dock Manager Layout ---
    [ObservableProperty] private IRootDock? _layout;

    // --- Theme & Auto-USO32 Status ---
    [ObservableProperty] private bool _isDarkTheme = true;
    [ObservableProperty] private string _themeButtonText = "Ночная вахта";
    [ObservableProperty] private bool _isConnected;
    [ObservableProperty] private bool _isSmpPlayerActive;
    [ObservableProperty] private string _activeSmpFileName = string.Empty;
    [ObservableProperty] private string _detectedUsoLabel = "Поиск УСО32...";
    [ObservableProperty] private string _detectedPort = string.Empty;
    [ObservableProperty] private int _detectedUsoId;
    [ObservableProperty] private int _reinitCount;
    [ObservableProperty] private string _statusBanner = "Автопоиск УСО32 (57600 8N1, uco32)...";
    [ObservableProperty] private string _simdIsa = "—";

    // --- USO32 & DSP Configuration (always uco32 6-byte, 0x90..0x95) ---
    [ObservableProperty] private FormatOption _selectedFormat;
    [ObservableProperty] private SpeedOption _selectedSpeed;
    [ObservableProperty] private string _firFilePath = string.Empty;
    [ObservableProperty] private int _gainIndex = 0;
    [ObservableProperty] private double _carrierFreqHz = 5.0;
    [ObservableProperty] private bool _correctorEnabled = true;
    [ObservableProperty] private int _ncod = 0;
    [ObservableProperty] private int _dcpThreshold = 55;
    [ObservableProperty] private bool _enableStepTrack = true;

    // --- Zero-Latency Toolface (OTK) State ---
    [ObservableProperty] private double _toolfaceAngleDeg;
    [ObservableProperty] private int _toolfaceReliabilityPct;
    [ObservableProperty] private double _toolfaceLatencyMs;
    [ObservableProperty] private DspToolfaceChannel _toolfaceChannel = DspToolfaceChannel.Gravity;
    [ObservableProperty] private IReadOnlyList<double> _toolfaceHistory = Array.Empty<double>();
    [ObservableProperty] private int _toolfaceUpdatesCount;

    // --- Oscilloscope, Spectrum & Correlation State ---
    [ObservableProperty] private double[]? _oscRawSamples;
    [ObservableProperty] private double[]? _oscFilteredSamples;
    [ObservableProperty] private bool _adcOverflowFlag;
    [ObservableProperty] private long _totalSamplesReceived;
    [ObservableProperty] private SpectrumSnapshot? _latestSpectrum;
    [ObservableProperty] private int[]? _latestCodeScoresPct;
    [ObservableProperty] private int _latestWinningCode5Bit = -1;
    [ObservableProperty] private IReadOnlyList<int> _syncCorrHistory = Array.Empty<int>();

    // --- Latest Decoded Frame Telemetry ---
    [ObservableProperty] private int _frameNumber;
    [ObservableProperty] private string _modeBadge = "ПОИСК УСО32";
    [ObservableProperty] private int _syncCorrPct;
    [ObservableProperty] private double _stepH = 1.923077;
    [ObservableProperty] private int _lastPikDrift;
    [ObservableProperty] private string _crcBadge = "Н/Д";

    [ObservableProperty] private double _zuDeg;
    [ObservableProperty] private double _azimuthDeg;
    [ObservableProperty] private double _gkZts;
    [ObservableProperty] private int _rpmZts;
    [ObservableProperty] private int _vibrZts;
    [ObservableProperty] private double _tempZts;
    [ObservableProperty] private double _voltZts;
    [ObservableProperty] private int _batteryPct;
    [ObservableProperty] private double _gTotal;
    [ObservableProperty] private double _hTotal;
    [ObservableProperty] private double _dipCalcDeg;

    // --- NDM (Format 2) Telemetry ---
    [ObservableProperty] private bool _isNdmVisible;
    [ObservableProperty] private double _zuNdmDeg;
    [ObservableProperty] private double _gkUpNdm;
    [ObservableProperty] private double _gkDnNdm;
    [ObservableProperty] private double _wobNdm;
    [ObservableProperty] private double _torqueNdm;
    [ObservableProperty] private double _pressNdm;

    public MainWindowViewModel(
        Func<DspSettings, IDspEngineClient>? engineFactory = null,
        Action<Action>? uiDispatcher = null,
        Func<UsoProbeResult>? usoScanner = null,
        bool autoStart = true)
    {
        _engineFactory = engineFactory ?? (cfg => new DspEngineClient(cfg));
        _dispatchToUi = uiDispatcher ?? (action => Dispatcher.UIThread.Post(action, DispatcherPriority.Render));
        _usoScanner = usoScanner ?? DspEngineClient.ScanLegacyUso;

        _selectedFormat = FormatOptions[0];
        _selectedSpeed = SpeedOptions[0];

        _dockFactory = new ProvodkaDockFactory(this);
        ResetDockLayout();
        TryQueryNativeIsa();

        if (autoStart)
        {
            StartPlotRefreshTimer();
            _supervisorTask = Task.Run(() => RunAutoSupervisorAsync(_supervisorCts.Token));
        }
    }

    partial void OnSelectedFormatChanged(FormatOption value)
    {
        IsNdmVisible = value.Format == DspFrameFormat.NdmGkRetr;
        ApplyLiveConfigIfRunning();
    }

    partial void OnGainIndexChanged(int value) => ApplyLiveConfigIfRunning();
    partial void OnCorrectorEnabledChanged(bool value) => ApplyLiveConfigIfRunning();
    partial void OnNcodChanged(int value) => ApplyLiveConfigIfRunning();
    partial void OnDcpThresholdChanged(int value) => ApplyLiveConfigIfRunning();
    partial void OnEnableStepTrackChanged(bool value) => ApplyLiveConfigIfRunning();

    partial void OnCarrierFreqHzChanged(double value)
    {
        // Changing carrier frequency on USO32 requires resending hardware freq byte (0..5) + 500ms settle
        if (!_isSmpMode)
            _forceReinitRequested = true;
        else
            ApplyLiveConfigIfRunning();
    }

    partial void OnSelectedSpeedChanged(SpeedOption value)
    {
        if (_isSmpMode && !string.IsNullOrEmpty(_pendingSmpPath))
        {
            _forceReinitRequested = true;
        }
    }

    public DspSettings BuildSettings(DspSourceType sourceType, string portOrPath)
    {
        return new DspSettings
        {
            SourceType = sourceType,
            PortOrPath = portOrPath.Trim(),
            UsoBitMode = 32, // Exclusively uco32: 6-byte packets, gain 0x90..0x95
            GainIndex = GainIndex,
            CarrierFreqHz = CarrierFreqHz,
            SampleRateHz = 250.0,
            CorrectorEnabled = CorrectorEnabled,
            FormatId = SelectedFormat.Format,
            Ncod = Ncod,
            SearchWindowIg = 13,
            DcpThreshold = DcpThreshold,
            EnableStepTrack = EnableStepTrack,
            PlaybackSpeed = SelectedSpeed.Multiplier,
            FirFilePath = FirFilePath.Trim()
        };
    }

    [RelayCommand]
    private void ToggleShiftTheme()
    {
        IsDarkTheme = !IsDarkTheme;
        ThemeButtonText = IsDarkTheme ? "Ночная вахта" : "Дневная вахта";

        if (Application.Current is not null)
        {
            Application.Current.RequestedThemeVariant = IsDarkTheme ? ThemeVariant.Dark : ThemeVariant.Light;
        }
    }

    [RelayCommand]
    private void ResetDockLayout()
    {
        IRootDock layout = _dockFactory.CreateLayout();
        _dockFactory.InitLayout(layout);
        Layout = layout;
    }

    [RelayCommand]
    private void ActivatePanel(string? panelId)
    {
        if (!string.IsNullOrWhiteSpace(panelId))
        {
            _dockFactory.TryActivatePanel(panelId);
        }
    }

    [RelayCommand]
    private void ForceReinitUso()
    {
        _isSmpMode = false;
        _pendingSmpPath = null;
        IsSmpPlayerActive = false;
        ActiveSmpFileName = string.Empty;
        _forceReinitRequested = true;
        StatusBanner = "Принудительный поиск и переинициализация УСО32...";
    }

    [RelayCommand]
    private async Task OpenSmpFileAsync()
    {
        if (PickSmpFileAsync is null)
            return;

        string? path = await PickSmpFileAsync();
        if (!string.IsNullOrWhiteSpace(path))
        {
            PlaySmpFile(path);
        }
    }

    [RelayCommand]
    private async Task LoadFirFileAsync()
    {
        if (PickFirFileAsync is null)
            return;

        string? path = await PickFirFileAsync();
        if (!string.IsNullOrWhiteSpace(path))
        {
            SetFirFilePath(path);
        }
    }

    [RelayCommand]
    private void CloseSmpAndReturnToUso()
    {
        _isSmpMode = false;
        _pendingSmpPath = null;
        IsSmpPlayerActive = false;
        ActiveSmpFileName = string.Empty;
        _forceReinitRequested = true;
        StatusBanner = "Плеер .smp закрыт — возврат к автопоиску УСО32...";
    }

    [RelayCommand]
    private void SetGain(string? gainStr)
    {
        if (int.TryParse(gainStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out int gain))
            GainIndex = Math.Clamp(gain, 0, 5);
    }

    [RelayCommand]
    private void SetCarrierFreq(string? freqStr)
    {
        if (double.TryParse(freqStr, NumberStyles.Float, CultureInfo.InvariantCulture, out double freq))
            CarrierFreqHz = freq;
    }

    [RelayCommand]
    private void SetCodebook(string? ncodStr)
    {
        if (int.TryParse(ncodStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out int ncod))
            Ncod = ncod == 1 ? 1 : 0;
    }

    [RelayCommand]
    private void ExitApplication() => RequestExit?.Invoke();

    public void PlaySmpFile(string path)
    {
        _pendingSmpPath = path;
        _isSmpMode = true;
        IsSmpPlayerActive = true;
        ActiveSmpFileName = Path.GetFileName(path);
        _forceReinitRequested = true;
        StatusBanner = $"Запуск записи .smp: {ActiveSmpFileName}";
    }

    public void SetFirFilePath(string path)
    {
        FirFilePath = path;
        if (_engine is not null)
        {
            int taps = _engine.LoadFirFile(path);
            StatusBanner = taps > 0
                ? $"Загружен КИХ-фильтр ({taps} коэфф.): {Path.GetFileName(path)}"
                : $"Не удалось загрузить КИХ-фильтр: {Path.GetFileName(path)}";
        }
    }

    /// <summary>
    /// Manually pumps one synchronous signal/plot flush (used both by 30 FPS UI timer and headless unit tests).
    /// </summary>
    public void FlushPlotsToUi()
    {
        double[] rawSnap;
        double[] firSnap;
        bool overflow;
        long totalSamples;
        int validCount;

        lock (_signalLock)
        {
            if (!_signalDirty)
                return;
            _signalDirty = false;

            validCount = _ringFilled;
            overflow = _latestOverflow;
            totalSamples = _totalSamples;

            rawSnap = new double[SignalWindowLength];
            firSnap = new double[SignalWindowLength];
            int tailLen = SignalWindowLength - _ringHead;
            Array.Copy(_rawRing, _ringHead, rawSnap, 0, tailLen);
            Array.Copy(_rawRing, 0, rawSnap, tailLen, _ringHead);
            Array.Copy(_firRing, _ringHead, firSnap, 0, tailLen);
            Array.Copy(_firRing, 0, firSnap, tailLen, _ringHead);
        }

        SpectrumSnapshot? spec = validCount >= 64
            ? SignalAnalyzer.ComputeSpectrum(firSnap.AsSpan(Math.Max(0, SignalWindowLength - Math.Min(validCount, FftWindowLength))), 250.0, FftWindowLength)
            : null;

        _dispatchToUi(() =>
        {
            OscRawSamples = rawSnap;
            OscFilteredSamples = firSnap;
            AdcOverflowFlag = overflow;
            TotalSamplesReceived = totalSamples;
            if (spec is not null)
                LatestSpectrum = spec;
        });
    }

    private void StartPlotRefreshTimer()
    {
        if (Application.Current is null)
            return;

        _uiPlotTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(33), DispatcherPriority.Render, (_, _) => FlushPlotsToUi());
        _uiPlotTimer.Start();
    }

    /// <summary>
    /// Autonomous USO32 & SMP supervisor loop:
    /// 1. In USO32 mode: scans all COM/ttyUSB ports -> opens uco32 -> monitors live packet heartbeat -> auto-reinits on fault.
    /// 2. In SMP mode: opens .smp file -> streams samples -> loops or waits until closed -> resumes USO32 automatically.
    /// </summary>
    private async Task RunAutoSupervisorAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                if (_isSmpMode && !string.IsNullOrWhiteSpace(_pendingSmpPath))
                {
                    await RunSmpSessionAsync(_pendingSmpPath!, ct);
                }
                else
                {
                    await RunUso32SessionAsync(ct);
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (DllNotFoundException)
            {
                _dispatchToUi(() =>
                {
                    IsConnected = false;
                    ModeBadge = "НЕТ DLL";
                    DetectedUsoLabel = "DspCore.dll не найдена";
                    StatusBanner = "Не найдена нативная библиотека DspCore.dll / libDspCore.so";
                });
                await Task.Delay(2500, ct);
            }
            catch (Exception ex)
            {
                _dispatchToUi(() =>
                {
                    IsConnected = false;
                    StatusBanner = $"Сбой диспетчера УСО32: {ex.Message} — повтор через 1 с...";
                });
                await Task.Delay(1000, ct);
            }
        }
    }

    private async Task RunUso32SessionAsync(CancellationToken ct)
    {
        _forceReinitRequested = false;
        DisposeEngine();
        ResetSignalBuffers();

        _dispatchToUi(() =>
        {
            IsConnected = false;
            ModeBadge = "ПОИСК УСО32";
            DetectedUsoLabel = "Сканирование портов...";
            StatusBanner = "Автопоиск УСО32 по COM-портам (0xAA → 0x66)...";
        });

        UsoProbeResult probe = _usoScanner();
        if (!probe.Detected || string.IsNullOrEmpty(probe.PortName))
        {
            _dispatchToUi(() =>
            {
                DetectedUsoLabel = "УСО32 не обнаружено";
                StatusBanner = "УСО32 не найдено — повторный поиск через 1.5 с...";
            });
            await DelayWithWakeupAsync(1500, ct);
            return;
        }

        DspSettings cfg = BuildSettings(DspSourceType.LegacySerial, probe.PortName);
        if (!TryStartEngine(cfg, out DspStatusCode startRc))
        {
            _dispatchToUi(() =>
            {
                ReinitCount++;
                DetectedUsoLabel = $"{probe.PortName} (ошибка {startRc})";
                StatusBanner = $"Ошибка инициализации УСО32 #{probe.UsoId} на {probe.PortName} ({startRc}) — повтор...";
            });
            await DelayWithWakeupAsync(1000, ct);
            return;
        }

        _lastSampleTickMs = Environment.TickCount64;
        _dispatchToUi(() =>
        {
            IsConnected = true;
            DetectedPort = probe.PortName;
            DetectedUsoId = probe.UsoId;
            DetectedUsoLabel = $"{probe.PortName} (УСО32 #{probe.UsoId})";
            ModeBadge = "УСО32 ОНЛАЙН";
            StatusBanner = $"УСО32 #{probe.UsoId} активно на {probe.PortName} • 57600 8N1 (uco32) • SIMD: {SimdIsa}";
        });

        // Watchdog loop: monitor hardware stream health & auto-reinit on stall or disconnect
        while (!ct.IsCancellationRequested && !_forceReinitRequested && !_isSmpMode)
        {
            await Task.Delay(350, ct);

            bool engineAlive = _engine is not null && _engine.IsRunning;
            long silenceMs = Environment.TickCount64 - Interlocked.Read(ref _lastSampleTickMs);

            if (!engineAlive || silenceMs > StreamWatchdogTimeoutMs)
            {
                _dispatchToUi(() =>
                {
                    IsConnected = false;
                    ReinitCount++;
                    ModeBadge = "ПЕРЕИНИЦИАЛИЗАЦИЯ";
                    StatusBanner = $"Потеря потока данных с {probe.PortName} ({silenceMs} мс) — переинициализация УСО32...";
                });
                DisposeEngine();
                await DelayWithWakeupAsync(800, ct);
                return;
            }
        }
    }

    private async Task RunSmpSessionAsync(string smpPath, CancellationToken ct)
    {
        _forceReinitRequested = false;
        DisposeEngine();
        ResetSignalBuffers();

        DspSettings cfg = BuildSettings(DspSourceType.FilePlayer, smpPath);
        if (!TryStartEngine(cfg, out DspStatusCode startRc))
        {
            _dispatchToUi(() =>
            {
                IsConnected = false;
                ModeBadge = "ОШИБКА .SMP";
                StatusBanner = $"Не удалось открыть .smp ({startRc}): {Path.GetFileName(smpPath)}";
            });
            await DelayWithWakeupAsync(1500, ct);
            return;
        }

        _dispatchToUi(() =>
        {
            IsConnected = true;
            ModeBadge = "ПЛЕЕР .SMP";
            DetectedUsoLabel = $"Файл: {Path.GetFileName(smpPath)}";
            StatusBanner = $"Воспроизведение .smp: {Path.GetFileName(smpPath)} ({SelectedSpeed.DisplayName}) • SIMD: {SimdIsa}";
        });

        while (!ct.IsCancellationRequested && !_forceReinitRequested && _isSmpMode)
        {
            await Task.Delay(150, ct);
            if (_engine is null || !_engine.IsRunning)
            {
                // Ensure final samples of .smp file are always rendered onto oscilloscope & FFT
                FlushPlotsToUi();
                _dispatchToUi(() =>
                {
                    ModeBadge = "КОНЕЦ .SMP";
                    StatusBanner = $"Файл {Path.GetFileName(smpPath)} воспроизведён ({_totalSamples} отсч.). Выберите другой файл или вернитесь к УСО32.";
                });

                // Wait until user opens another file, changes speed, or returns to live USO32
                while (!ct.IsCancellationRequested && !_forceReinitRequested && _isSmpMode)
                {
                    await Task.Delay(200, ct);
                }
                break;
            }
        }
    }

    private async Task DelayWithWakeupAsync(int totalMs, CancellationToken ct)
    {
        int elapsed = 0;
        while (elapsed < totalMs && !ct.IsCancellationRequested && !_forceReinitRequested)
        {
            int step = Math.Min(100, totalMs - elapsed);
            await Task.Delay(step, ct);
            elapsed += step;
        }
    }

    private bool TryStartEngine(DspSettings settings, out DspStatusCode status)
    {
        DisposeEngine();
        _engine = _engineFactory(settings);
        string isa = _engine.SimdIsa;
        _dispatchToUi(() => SimdIsa = isa);

        if (!string.IsNullOrWhiteSpace(settings.FirFilePath))
        {
            _engine.LoadFirFile(settings.FirFilePath);
        }

        _engine.ToolfaceReceived += OnToolfaceReceived;
        _engine.VectorDecoded += OnVectorDecoded;
        _engine.FrameCompleted += OnFrameCompleted;
        _engine.SignalBlockReceived += OnSignalBlockReceived;

        status = _engine.Start();
        if (status != DspStatusCode.Ok)
        {
            DisposeEngine();
            return false;
        }
        return true;
    }

    private void ResetSignalBuffers()
    {
        lock (_signalLock)
        {
            Array.Clear(_rawRing);
            Array.Clear(_firRing);
            _ringHead = 0;
            _ringFilled = 0;
            _totalSamples = 0;
            _latestOverflow = false;
            _signalDirty = false;
        }
    }

    private void ApplyLiveConfigIfRunning()
    {
        if (_engine is null)
            return;

        DspSourceType src = _isSmpMode ? DspSourceType.FilePlayer : DspSourceType.LegacySerial;
        string path = _isSmpMode ? (_pendingSmpPath ?? string.Empty) : DetectedPort;
        _engine.Configure(BuildSettings(src, path));
    }

    private void TryQueryNativeIsa()
    {
        try
        {
            SimdIsa = DspNativeMethods.GetSimdIsa();
        }
        catch
        {
            SimdIsa = "N/A";
        }
    }

    private void OnToolfaceReceived(object? sender, ToolfaceReading reading)
    {
        _dispatchToUi(() =>
        {
            ToolfaceAngleDeg = reading.AngleDeg;
            ToolfaceReliabilityPct = reading.ReliabilityPct;
            ToolfaceLatencyMs = reading.LatencyMs;
            ToolfaceChannel = reading.Channel;
            ToolfaceUpdatesCount++;

            var trail = new List<double>(ToolfaceHistory) { reading.AngleDeg };
            while (trail.Count > MaxToolfaceTrail)
                trail.RemoveAt(0);
            ToolfaceHistory = trail;
        });
    }

    private void OnVectorDecoded(object? sender, VectorReading reading)
    {
        _dispatchToUi(() =>
        {
            if (reading.VectorIndex == 0)
                DecodedVectors.Clear();

            DecodedVectors.Add(new VectorCellViewModel(
                reading.VectorIndex,
                reading.Code5Bit,
                reading.ReliabilityPct,
                reading.ReliabilityPct >= DcpThreshold));

            LatestCodeScoresPct = reading.CodeScoresPct;
            LatestWinningCode5Bit = reading.Code5Bit;
        });
    }

    private void OnFrameCompleted(object? sender, FrameSnapshot frame)
    {
        _dispatchToUi(() =>
        {
            FrameNumber = frame.FrameNumber;
            ModeBadge = frame.IsStaticMode ? "СТАТИКА" : "БУРЕНИЕ";
            SyncCorrPct = frame.SyncCorrPct;
            StepH = frame.StepH;
            LastPikDrift = frame.LastPikDrift;
            CrcBadge = frame.CrcValid switch
            {
                1 => "CRC32 OK",
                0 => "CRC32 ОШИБКА",
                _ => "Н/Д"
            };

            var syncHist = new List<int>(SyncCorrHistory) { frame.SyncCorrPct };
            while (syncHist.Count > MaxSyncHistory)
                syncHist.RemoveAt(0);
            SyncCorrHistory = syncHist;

            ZuDeg = frame.ZuDeg;
            AzimuthDeg = frame.AzTrueDeg > -9000.0
                ? frame.AzTrueDeg
                : frame.AzKorrDeg > -9000.0
                    ? frame.AzKorrDeg
                    : frame.AzRawDeg;
            GkZts = frame.GkZts;
            RpmZts = frame.RpmZts;
            VibrZts = frame.VibrZts;
            TempZts = frame.TempZts;
            VoltZts = frame.VoltZts;
            BatteryPct = frame.BatteryPct;
            GTotal = frame.GTotal;
            HTotal = frame.HTotal;
            DipCalcDeg = frame.DipCalcDeg;

            IsNdmVisible = frame.FormatId == DspFrameFormat.NdmGkRetr;
            ZuNdmDeg = frame.ZuNdmDeg;
            GkUpNdm = frame.GkUpNdm;
            GkDnNdm = frame.GkDnNdm;
            WobNdm = frame.WobNdm;
            TorqueNdm = frame.TorqueNdm;
            PressNdm = frame.PressNdm;
        });
    }

    private void OnSignalBlockReceived(object? sender, SignalSnapshot block)
    {
        Interlocked.Exchange(ref _lastSampleTickMs, Environment.TickCount64);

        bool shouldFlushImmediately = false;
        lock (_signalLock)
        {
            int n = Math.Min(block.RawSamples.Length, block.FilteredSamples.Length);
            bool wasEmpty = _totalSamples == 0;
            for (int i = 0; i < n; i++)
            {
                _rawRing[_ringHead] = block.RawSamples[i];
                _firRing[_ringHead] = block.FilteredSamples[i];
                _ringHead = (_ringHead + 1) % SignalWindowLength;
            }
            _ringFilled = Math.Min(SignalWindowLength, _ringFilled + n);
            _totalSamples += n;
            _latestOverflow = block.OverflowDetected;
            _signalDirty = true;

            // If running without UI timer (headless tests) or first chunk arrives, flush right away
            shouldFlushImmediately = wasEmpty || _uiPlotTimer is null;
        }

        if (shouldFlushImmediately)
        {
            FlushPlotsToUi();
        }
    }

    private void DisposeEngine()
    {
        if (_engine is null)
            return;

        _engine.ToolfaceReceived -= OnToolfaceReceived;
        _engine.VectorDecoded -= OnVectorDecoded;
        _engine.FrameCompleted -= OnFrameCompleted;
        _engine.SignalBlockReceived -= OnSignalBlockReceived;
        _engine.Stop();
        _engine.Dispose();
        _engine = null;
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;

        _uiPlotTimer?.Stop();
        _supervisorCts.Cancel();
        try
        {
            _supervisorTask?.Wait(TimeSpan.FromSeconds(1));
        }
        catch
        {
            // Ignore cancellation exceptions on shutdown
        }
        _supervisorCts.Dispose();
        DisposeEngine();
    }
}
