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
public sealed record RateModeOption(bool Adaptive, int DrCode, int Decimation, string DisplayName);
public sealed record AdcFilterOption(int FilterCode, string DisplayName);
public sealed record McuLpfOption(int ModeCode, string DisplayName);
public sealed record InputMuxOption(int MuxCode, string DisplayName);

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
    private readonly SemaphoreSlim _wakeupSignal = new(0, 1);
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

    public IReadOnlyList<RateModeOption> RateModeOptions { get; } =
    [
        new(true,  0x08, 0, "Авто (под несущую)"),
        new(false, 0x08, 1, "400 Гц (×1)"),
        new(false, 0x08, 2, "200 Гц (400 / 2)"),
        new(false, 0x07, 1, "100 Гц (Sinc)"),
        new(false, 0x06, 1, "60 Гц (Sinc)"),
        new(false, 0x05, 1, "50 Гц (Sinc)"),
        new(false, 0x09, 1, "1200 Гц (×1)")
    ];

    public IReadOnlyList<AdcFilterOption> AdcFilterOptions { get; } =
    [
        new(3, "Sinc4 (рек.)"),
        new(2, "Sinc3"),
        new(1, "Sinc2"),
        new(0, "Sinc1")
    ];

    public IReadOnlyList<McuLpfOption> McuLpfOptions { get; } =
    [
        new(0, "Bypass (без ФНЧ МК)"),
        new(1, "Баттерворт 4-го пор. (авто)"),
        new(2, "Legacy 10-го пор.")
    ];

    public IReadOnlyList<InputMuxOption> InputMuxOptions { get; } =
    [
        new(0x89, "Сейсмодатчик (AIN8/9)"),
        new(0xFF, "Замыкание входа (шум)"),
        new(0xBB, "Датчик темп. АЦП"),
        new(0xCC, "Контроль AVDD")
    ];

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
    [ObservableProperty] private string _statusBanner = "Автопоиск УСО32 (V2 / Legacy uco32)...";
    [ObservableProperty] private string _simdIsa = "—";
    [ObservableProperty] private bool _isUsoV2Active;
    [ObservableProperty] private string _activeProtocolLabel = "Автоопределение (V2 / Legacy)";
    [ObservableProperty] private double _activeSampleRateHz = 250.0;
    [ObservableProperty] private string _usoDiagSummary = "Пак: 0 • CRC: 0 • Проп: 0";

    // --- USO32 & DSP Configuration (Dual-Mode V2 + Legacy uco32 fallback) ---
    [ObservableProperty] private FormatOption _selectedFormat;
    [ObservableProperty] private SpeedOption _selectedSpeed;
    [ObservableProperty] private RateModeOption _selectedRateMode;
    [ObservableProperty] private AdcFilterOption _selectedAdcFilter;
    [ObservableProperty] private McuLpfOption _selectedMcuLpfMode;
    [ObservableProperty] private InputMuxOption _selectedInputMux;
    [ObservableProperty] private bool _chopEnabled;
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
        _selectedRateMode = RateModeOptions[0];
        _selectedAdcFilter = AdcFilterOptions[0];
        _selectedMcuLpfMode = McuLpfOptions[1]; // Adaptive 4th-order Butterworth on MCU
        _selectedInputMux = InputMuxOptions[0];

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
        if (value is null) return;
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
        // Fast live reconfigure directly on the open USO32 handle (~25 ms V2 / ~130 ms Legacy, zero COM rescan!)
        ApplyLiveConfigIfRunning();
        if (IsConnected && !_isSmpMode)
        {
            StatusBanner = $"Переключение несущей на {value:0.#} Гц...";
        }
    }

    partial void OnSelectedSpeedChanged(SpeedOption value)
    {
        if (value is null) return;
        if (_isSmpMode && !string.IsNullOrEmpty(_pendingSmpPath))
        {
            _forceReinitRequested = true;
            WakeSupervisor();
        }
    }

    partial void OnSelectedRateModeChanged(RateModeOption value)
    {
        if (value is null) return;
        ApplyV2OptionsLiveIfRunning();
    }

    partial void OnSelectedAdcFilterChanged(AdcFilterOption value)
    {
        if (value is null) return;
        ApplyV2OptionsLiveIfRunning();
    }

    partial void OnSelectedMcuLpfModeChanged(McuLpfOption value)
    {
        if (value is null) return;
        ApplyV2OptionsLiveIfRunning();
    }

    partial void OnSelectedInputMuxChanged(InputMuxOption value)
    {
        if (value is null) return;
        ApplyV2OptionsLiveIfRunning();
    }

    partial void OnChopEnabledChanged(bool value) => ApplyV2OptionsLiveIfRunning();

    public UsoV2Options BuildUsoV2Options() => new()
    {
        AdaptiveRate = SelectedRateMode?.Adaptive ?? true,
        DrCode = SelectedRateMode?.DrCode ?? 0x08,
        Decimation = SelectedRateMode?.Decimation ?? 0,
        AdcFilter = SelectedAdcFilter?.FilterCode ?? 3,
        ChopEnabled = ChopEnabled,
        VbiasEnabled = true,
        Inpmux = SelectedInputMux?.MuxCode ?? 0x89,
        McuLpfMode = SelectedMcuLpfMode?.ModeCode ?? 1,
        McuLpfCutoffHz = 0.0,
        McuHpfCutoffHz = CorrectorEnabled ? 0.1 : 0.0,
        BatchSize = 0,
        AutoRecover = true
    };

    public DspSettings BuildSettings(DspSourceType sourceType, string portOrPath)
    {
        double fs = (sourceType == DspSourceType.FilePlayer)
            ? 250.0
            : (ActiveSampleRateHz > 1.0 ? ActiveSampleRateHz : 250.0);
        return new DspSettings
        {
            SourceType = sourceType,
            PortOrPath = portOrPath.Trim(),
            UsoBitMode = 32, // Exclusively uco32: 6-byte packets, gain 0x90..0x95
            GainIndex = GainIndex,
            CarrierFreqHz = CarrierFreqHz,
            SampleRateHz = fs,
            CorrectorEnabled = CorrectorEnabled,
            FormatId = SelectedFormat?.Format ?? DspFrameFormat.StdGk,
            Ncod = Ncod,
            SearchWindowIg = 13,
            DcpThreshold = DcpThreshold,
            EnableStepTrack = EnableStepTrack,
            PlaybackSpeed = SelectedSpeed?.Multiplier ?? 1,
            FirFilePath = string.Empty
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
        WakeSupervisor();
        StatusBanner = "Принудительный поиск и переинициализация УСО32...";
    }

    [RelayCommand]
    private async Task RecalibrateUsoAsync()
    {
        IDspEngineClient? eng = _engine;
        if (eng is null || !IsUsoV2Active)
        {
            StatusBanner = "Самокалибровка доступна только в режиме протокола УСО32 V2.";
            return;
        }
        StatusBanner = "Выполняется самокалибровка смещения ADS1262...";
        DspStatusCode rc;
        try
        {
            rc = await Task.Run(() => eng.RecalibrateUso());
        }
        catch (ObjectDisposedException)
        {
            return;
        }
        StatusBanner = rc == DspStatusCode.Ok
            ? "Самокалибровка смещения ADS1262 (6 ступеней PGA) выполнена успешно."
            : $"Ошибка самокалибровки УСО32 V2 ({rc}).";
    }

    [RelayCommand]
    private async Task ResetUsoAdcAsync()
    {
        IDspEngineClient? eng = _engine;
        if (eng is null || !IsUsoV2Active)
        {
            ForceReinitUso();
            return;
        }
        StatusBanner = "Аппаратный сброс ADS1262...";
        DspStatusCode rc;
        try
        {
            rc = await Task.Run(() => eng.ResetUsoAdc());
        }
        catch (ObjectDisposedException)
        {
            return;
        }
        StatusBanner = rc == DspStatusCode.Ok
            ? "Аппаратный сброс ADS1262 выполнен — перезапуск потока..."
            : $"Ошибка сброса АЦП ({rc}) — выполняется переинициализация...";
        _forceReinitRequested = true;
        WakeSupervisor();
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
    private void CloseSmpAndReturnToUso()
    {
        _isSmpMode = false;
        _pendingSmpPath = null;
        IsSmpPlayerActive = false;
        ActiveSmpFileName = string.Empty;
        _forceReinitRequested = true;
        WakeSupervisor();
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
        WakeSupervisor();
        StatusBanner = $"Запуск записи .smp: {ActiveSmpFileName}";
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

        double fsForFft = ActiveSampleRateHz > 1.0 ? ActiveSampleRateHz : 250.0;
        SpectrumSnapshot? spec = validCount >= 64
            ? SignalAnalyzer.ComputeSpectrum(firSnap.AsSpan(Math.Max(0, SignalWindowLength - Math.Min(validCount, FftWindowLength))), fsForFft, FftWindowLength)
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
            StatusBanner = "Автопоиск УСО32 по COM-портам (V2 / Legacy 0x66)...";
        });

        UsoProbeResult probe = _usoScanner();
        if (!probe.Detected || string.IsNullOrEmpty(probe.PortName))
        {
            string details = string.IsNullOrWhiteSpace(probe.Diagnostics)
                ? "порты не ответили"
                : probe.Diagnostics;
            _dispatchToUi(() =>
            {
                DetectedUsoLabel = $"Нет УСО32 ({details})";
                StatusBanner = $"УСО32 не найдено [{details}] — повтор через 1.5 с...";
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
        UsoRuntimeInfo? info = _engine?.GetUsoInfo();
        double negotiatedFs = (info is not null && info.ActiveSampleRateHz > 1.0) ? info.ActiveSampleRateHz : 250.0;
        bool v2 = info?.IsV2 == true;
        string protoLabel = v2
            ? $"V2 CRC16 ({info!.FirmwareVersionText}) • {negotiatedFs:0.#} Гц"
            : $"Legacy uco32 • {negotiatedFs:0.#} Гц";

        _dispatchToUi(() =>
        {
            IsConnected = true;
            IsUsoV2Active = v2;
            ActiveSampleRateHz = negotiatedFs;
            ActiveProtocolLabel = protoLabel;
            DetectedPort = probe.PortName;
            DetectedUsoId = probe.UsoId;
            DetectedUsoLabel = v2
                ? $"{probe.PortName} (УСО32 #{probe.UsoId} V2)"
                : $"{probe.PortName} (УСО32 #{probe.UsoId})";
            ModeBadge = v2 ? "УСО32 V2" : "УСО32 LEGACY";
            StepH = _engine?.CurrentStepH ?? StepH;
            StatusBanner = v2
                ? $"УСО32 #{probe.UsoId} V2 ({info!.FirmwareVersionText}) на {probe.PortName} • Fs={negotiatedFs:0.#} Гц (АЦП {info.HwSampleRateHz:0.#} Гц / ÷{info.Decimation}) • SIMD: {SimdIsa}"
                : $"УСО32 #{probe.UsoId} (Legacy uco32) на {probe.PortName} • 57600 8N1 • SIMD: {SimdIsa}";
        });

        // Watchdog loop: monitor hardware stream health, reflect live Fs/StepH changes & poll V2 diagnostics
        int diagTick = 0;
        while (!ct.IsCancellationRequested && !_forceReinitRequested && !_isSmpMode)
        {
            await WaitWakeupOrDelayAsync(300, ct);
            if (ct.IsCancellationRequested || _forceReinitRequested || _isSmpMode)
                break;

            bool engineAlive = _engine is not null && _engine.IsRunning;
            long silenceMs = Environment.TickCount64 - Interlocked.Read(ref _lastSampleTickMs);

            if (!engineAlive || silenceMs > StreamWatchdogTimeoutMs)
            {
                _dispatchToUi(() =>
                {
                    IsConnected = false;
                    IsUsoV2Active = false;
                    ReinitCount++;
                    ModeBadge = "ПЕРЕИНИЦИАЛИЗАЦИЯ";
                    StatusBanner = $"Потеря потока данных с {probe.PortName} ({silenceMs} мс) — переинициализация УСО32...";
                });
                DisposeEngine();
                await DelayWithWakeupAsync(800, ct);
                return;
            }

            if (_engine is not null)
            {
                if (++diagTick >= 6)
                {
                    diagTick = 0;
                    if (v2) _engine.RefreshUsoDiagnostics();
                }

                UsoRuntimeInfo? liveInfo = _engine.GetUsoInfo();
                double curH = _engine.CurrentStepH;
                if (liveInfo is not null)
                {
                    double liveFs = liveInfo.ActiveSampleRateHz > 1.0 ? liveInfo.ActiveSampleRateHz : ActiveSampleRateHz;
                    string liveProto = liveInfo.IsV2
                        ? $"V2 CRC16 ({liveInfo.FirmwareVersionText}) • {liveFs:0.#} Гц"
                        : $"Legacy uco32 • {liveFs:0.#} Гц";
                    string diagLine = liveInfo.IsV2
                        ? $"Пак: {liveInfo.RxPackets} • CRC: {liveInfo.CrcErrors} • Проп: {liveInfo.DroppedSamples} • DRDY: {liveInfo.MissedDrdy}"
                        : $"Пак: {liveInfo.RxPackets} • Режим: Legacy uco32 ({liveFs:0.#} Гц)";
                    _dispatchToUi(() =>
                    {
                        UsoDiagSummary = diagLine;
                        ActiveSampleRateHz = liveFs;
                        ActiveProtocolLabel = liveProto;
                        StepH = curH;
                    });
                }
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
            IsUsoV2Active = false;
            ActiveSampleRateHz = 250.0;
            ActiveProtocolLabel = "Файл .smp • 250 Гц";
            ModeBadge = "ПЛЕЕР .SMP";
            DetectedUsoLabel = $"Файл: {Path.GetFileName(smpPath)}";
            StepH = _engine?.CurrentStepH ?? StepH;
            StatusBanner = $"Воспроизведение .smp: {Path.GetFileName(smpPath)} ({SelectedSpeed.DisplayName}) • SIMD: {SimdIsa}";
        });

        while (!ct.IsCancellationRequested && !_forceReinitRequested && _isSmpMode)
        {
            await WaitWakeupOrDelayAsync(150, ct);
            if (_forceReinitRequested || !_isSmpMode)
                break;
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
                    await WaitWakeupOrDelayAsync(200, ct);
                }
                break;
            }
        }
    }

    private void WakeSupervisor()
    {
        try
        {
            if (_wakeupSignal.CurrentCount == 0)
                _wakeupSignal.Release();
        }
        catch (ObjectDisposedException)
        {
            // Ignored during shutdown
        }
    }

    private async Task WaitWakeupOrDelayAsync(int timeoutMs, CancellationToken ct)
    {
        try
        {
            await _wakeupSignal.WaitAsync(timeoutMs, ct);
        }
        catch (ObjectDisposedException)
        {
            // Ignored during shutdown
        }
    }

    private async Task DelayWithWakeupAsync(int totalMs, CancellationToken ct)
    {
        if (_forceReinitRequested) return;
        await WaitWakeupOrDelayAsync(totalMs, ct);
    }

    private bool TryStartEngine(DspSettings settings, out DspStatusCode status)
    {
        DisposeEngine();
        _engine = _engineFactory(settings);
        string isa = _engine.SimdIsa;
        _dispatchToUi(() => SimdIsa = isa);

        status = _engine.SetUsoV2Options(BuildUsoV2Options());
        if (status != DspStatusCode.Ok) return false;

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
        IDspEngineClient? eng = _engine;
        if (eng is null)
            return;

        DspSourceType src = _isSmpMode ? DspSourceType.FilePlayer : DspSourceType.LegacySerial;
        string path = _isSmpMode ? (_pendingSmpPath ?? string.Empty) : DetectedPort;
        try
        {
            eng.Configure(BuildSettings(src, path));
            StepH = eng.CurrentStepH;
        }
        catch (ObjectDisposedException)
        {
            // Engine is being reinitialized by supervisor; new session will pick up updated settings.
        }
    }

    private void ApplyV2OptionsLiveIfRunning()
    {
        IDspEngineClient? eng = _engine;
        if (eng is null || _isSmpMode)
            return;

        try
        {
            eng.SetUsoV2Options(BuildUsoV2Options());
            if (IsUsoV2Active)
            {
                StatusBanner = "Применение параметров тракта ADS1262 (УСО32 V2)...";
            }
        }
        catch (ObjectDisposedException)
        {
            // Engine is being reinitialized by supervisor; new session will pick up updated V2 options.
        }
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
        IDspEngineClient? eng = _engine;
        _engine = null;
        if (eng is null)
            return;

        eng.ToolfaceReceived -= OnToolfaceReceived;
        eng.VectorDecoded -= OnVectorDecoded;
        eng.FrameCompleted -= OnFrameCompleted;
        eng.SignalBlockReceived -= OnSignalBlockReceived;
        eng.Stop();
        eng.Dispose();
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
        _wakeupSignal.Dispose();
        DisposeEngine();
    }
}
