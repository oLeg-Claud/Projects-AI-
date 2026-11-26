using System.Collections.ObjectModel;
using System.Globalization;
using Avalonia;
using Avalonia.Media;
using Avalonia.Styling;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ProvodkaNG.Core;
using ProvodkaNG.Core.Models;
using ProvodkaNG.Core.Native;

namespace ProvodkaNG.Desktop.ViewModels;

public sealed record FormatOption(DspFrameFormat Format, string DisplayName);
public sealed record SourceOption(DspSourceType Source, string DisplayName);
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
/// Bridges Zero-Latency native C++ DspCore callbacks to Avalonia UI controls.
/// </summary>
public sealed partial class MainWindowViewModel : ObservableObject, IDisposable
{
    private const int SignalWindowLength = 1024;
    private const int MaxToolfaceTrail = 6;

    private readonly Action<Action> _dispatchToUi;
    private readonly Func<DspSettings, IDspEngineClient> _engineFactory;
    private readonly object _signalLock = new();
    private readonly double[] _rawRing = new double[SignalWindowLength];
    private readonly double[] _firRing = new double[SignalWindowLength];
    private int _ringHead;
    private long _lastSignalUiTickMs;

    private IDspEngineClient? _engine;
    private bool _disposed;

    public IReadOnlyList<SourceOption> SourceOptions { get; } =
    [
        new(DspSourceType.FilePlayer, "Файл-плеер (.smp)"),
        new(DspSourceType.LegacySerial, "Старое УСО (COM/USB 57600)"),
        new(DspSourceType.NewAdc32, "Новое УСО (32-бит АЦП, 8 отсч/бит)")
    ];

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
        new(0, "MAX (Пакетно)")
    ];

    public IReadOnlyList<double> CarrierFrequencies { get; } = [16.0, 10.0, 7.0, 5.0, 3.6, 2.5, 1.8];
    public IReadOnlyList<int> GainIndices { get; } = [0, 1, 2, 3, 4, 5];
    public IReadOnlyList<int> BitModes { get; } = [24, 32];
    public IReadOnlyList<int> Codebooks { get; } = [0, 1];

    public ObservableCollection<VectorCellViewModel> DecodedVectors { get; } = [];

    // --- Theme & Shell State ---
    [ObservableProperty] private bool _isDarkTheme = true;
    [ObservableProperty] private string _themeButtonText = "Ночная вахта";
    [ObservableProperty] private bool _isRunning;
    [ObservableProperty] private string _statusBanner = "Готов к запуску ЦОС";
    [ObservableProperty] private string _simdIsa = "—";
    [ObservableProperty] private string _usoProbeStatus = "УСО не проверено";

    // --- Acquisition & DSP Configuration ---
    [ObservableProperty] private SourceOption _selectedSource;
    [ObservableProperty] private FormatOption _selectedFormat;
    [ObservableProperty] private SpeedOption _selectedSpeed;
    [ObservableProperty] private string _portOrPath = OperatingSystem.IsWindows() ? "COM3" : "/dev/ttyUSB0";
    [ObservableProperty] private string _firFilePath = string.Empty;
    [ObservableProperty] private int _usoBitMode = 24;
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

    // --- Oscilloscope Signal State ---
    [ObservableProperty] private double[]? _oscRawSamples;
    [ObservableProperty] private double[]? _oscFilteredSamples;
    [ObservableProperty] private bool _adcOverflowFlag;

    // --- Latest Decoded Frame Telemetry ---
    [ObservableProperty] private int _frameNumber;
    [ObservableProperty] private string _modeBadge = "ОЖИДАНИЕ";
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
        Action<Action>? uiDispatcher = null)
    {
        _engineFactory = engineFactory ?? (cfg => new DspEngineClient(cfg));
        _dispatchToUi = uiDispatcher ?? (action => Dispatcher.UIThread.Post(action, DispatcherPriority.Render));

        _selectedSource = SourceOptions[0];
        _selectedFormat = FormatOptions[0];
        _selectedSpeed = SpeedOptions[0];

        TryQueryNativeIsa();
    }

    partial void OnSelectedFormatChanged(FormatOption value)
    {
        IsNdmVisible = value.Format == DspFrameFormat.NdmGkRetr;
        ApplyLiveConfigIfRunning();
    }

    partial void OnGainIndexChanged(int value) => ApplyLiveConfigIfRunning();
    partial void OnCorrectorEnabledChanged(bool value) => ApplyLiveConfigIfRunning();
    partial void OnDcpThresholdChanged(int value) => ApplyLiveConfigIfRunning();
    partial void OnEnableStepTrackChanged(bool value) => ApplyLiveConfigIfRunning();

    public DspSettings BuildSettings()
    {
        double sampleRate = SelectedSource.Source == DspSourceType.NewAdc32
            ? 16.0 * CarrierFreqHz
            : 250.0;

        return new DspSettings
        {
            SourceType = SelectedSource.Source,
            PortOrPath = PortOrPath.Trim(),
            UsoBitMode = UsoBitMode,
            GainIndex = GainIndex,
            CarrierFreqHz = CarrierFreqHz,
            SampleRateHz = sampleRate,
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
    private void ProbeLegacyUso()
    {
        string port = PortOrPath.Trim();
        if (string.IsNullOrEmpty(port))
        {
            UsoProbeStatus = "Укажите COM-порт (например COM3 или /dev/ttyUSB0)";
            return;
        }

        try
        {
            UsoProbeResult result = DspEngineClient.ProbeLegacyUso(port);
            UsoProbeStatus = result.Detected
                ? $"Найдено УСО #{result.UsoId} на {result.PortName} (0xAA→0x55→0x66 OK)"
                : $"УСО не обнаружено на {result.PortName} ({result.Status})";
        }
        catch (DllNotFoundException)
        {
            UsoProbeStatus = "Не найдена библиотека DspCore.dll / libDspCore.so";
        }
        catch (Exception ex)
        {
            UsoProbeStatus = $"Ошибка опроса УСО: {ex.Message}";
        }
    }

    [RelayCommand]
    private void ToggleAcquisition()
    {
        if (IsRunning)
        {
            StopAcquisition();
        }
        else
        {
            StartAcquisition();
        }
    }

    public void StartAcquisition()
    {
        if (IsRunning)
            return;

        try
        {
            DspSettings settings = BuildSettings();
            DisposeEngine();

            _engine = _engineFactory(settings);
            SimdIsa = _engine.SimdIsa;

            if (!string.IsNullOrWhiteSpace(settings.FirFilePath))
            {
                int taps = _engine.LoadFirFile(settings.FirFilePath);
                if (taps < 0)
                {
                    StatusBanner = $"Ошибка загрузки КИХ-фильтра: {settings.FirFilePath}";
                }
            }

            _engine.ToolfaceReceived += OnToolfaceReceived;
            _engine.VectorDecoded += OnVectorDecoded;
            _engine.FrameCompleted += OnFrameCompleted;
            _engine.SignalBlockReceived += OnSignalBlockReceived;

            DspStatusCode status = _engine.Start();
            if (status == DspStatusCode.Ok)
            {
                IsRunning = true;
                StatusBanner = $"Приём активен • {SelectedSource.DisplayName} • {SelectedFormat.DisplayName} • SIMD: {SimdIsa}";
            }
            else
            {
                StatusBanner = $"Не удалось запустить источник ({status}): {settings.PortOrPath}";
                DisposeEngine();
            }
        }
        catch (DllNotFoundException)
        {
            StatusBanner = "Ошибка: не найдена нативная библиотека DspCore.dll / libDspCore.so";
        }
        catch (Exception ex)
        {
            StatusBanner = $"Ошибка запуска ЦОС: {ex.Message}";
        }
    }

    public void StopAcquisition()
    {
        DisposeEngine();
        IsRunning = false;
        StatusBanner = "Приём остановлен";
    }

    public void SetSmpFilePath(string path)
    {
        PortOrPath = path;
        SelectedSource = SourceOptions[0];
    }

    public void SetFirFilePath(string path)
    {
        FirFilePath = path;
        if (IsRunning && _engine is not null)
        {
            int taps = _engine.LoadFirFile(path);
            StatusBanner = taps > 0
                ? $"Загружен КИХ-фильтр ({taps} коэфф.): {Path.GetFileName(path)}"
                : $"Не удалось загрузить КИХ-фильтр: {Path.GetFileName(path)}";
        }
    }

    private void ApplyLiveConfigIfRunning()
    {
        if (!IsRunning || _engine is null)
            return;

        _engine.Configure(BuildSettings());
    }

    private void TryQueryNativeIsa()
    {
        try
        {
            SimdIsa = DspNativeMethods.GetSimdIsa();
        }
        catch
        {
            SimdIsa = "N/A (build DspCore)";
        }
    }

    private void OnToolfaceReceived(object? sender, ToolfaceReading reading)
    {
        // Zero-Latency path: push directly to UI thread at Render priority without waiting for frame end
        _dispatchToUi(() =>
        {
            ToolfaceAngleDeg = reading.AngleDeg;
            ToolfaceReliabilityPct = reading.ReliabilityPct;
            ToolfaceLatencyMs = reading.LatencyMs;
            ToolfaceChannel = reading.Channel;
            ToolfaceUpdatesCount++;

            var trail = new List<double>(ToolfaceHistory);
            trail.Add(reading.AngleDeg);
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
        double[] rawSnap;
        double[] firSnap;

        lock (_signalLock)
        {
            int n = Math.Min(block.RawSamples.Length, block.FilteredSamples.Length);
            for (int i = 0; i < n; i++)
            {
                _rawRing[_ringHead] = block.RawSamples[i];
                _firRing[_ringHead] = block.FilteredSamples[i];
                _ringHead = (_ringHead + 1) % SignalWindowLength;
            }

            // Throttle UI waveform redraws to ~30 FPS so DSP thread never saturates UI dispatcher
            long nowMs = Environment.TickCount64;
            if (nowMs - _lastSignalUiTickMs < 33)
                return;
            _lastSignalUiTickMs = nowMs;

            rawSnap = new double[SignalWindowLength];
            firSnap = new double[SignalWindowLength];
            int tailLen = SignalWindowLength - _ringHead;
            Array.Copy(_rawRing, _ringHead, rawSnap, 0, tailLen);
            Array.Copy(_rawRing, 0, rawSnap, tailLen, _ringHead);
            Array.Copy(_firRing, _ringHead, firSnap, 0, tailLen);
            Array.Copy(_firRing, 0, firSnap, tailLen, _ringHead);
        }

        bool overflow = block.OverflowDetected;
        _dispatchToUi(() =>
        {
            OscRawSamples = rawSnap;
            OscFilteredSamples = firSnap;
            AdcOverflowFlag = overflow;
        });
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
        DisposeEngine();
    }
}
