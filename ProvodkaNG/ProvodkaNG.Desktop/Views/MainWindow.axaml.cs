using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform;
using Avalonia.Platform.Storage;
using ProvodkaNG.Desktop.Services;
using ProvodkaNG.Desktop.ViewModels;

namespace ProvodkaNG.Desktop.Views;

public partial class MainWindow : Window
{
    private PixelPoint? _lastNormalPosition;
    private double _lastNormalWidth = 1500;
    private double _lastNormalHeight = 920;
    private bool _placementApplied;

    public MainWindow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        PositionChanged += OnWindowPositionChanged;
        PropertyChanged += OnWindowPropertyChanged;
        Opened += OnWindowOpened;
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            vm.PickSmpFileAsync = PickSmpFileAsync;
            vm.CaptureWindowPlacement = CaptureWindowPlacement;
            vm.RequestExit = Close;
            ApplySavedPlacementPreOpen(vm.SavedWindowPlacement);
        }
    }

    private void OnWindowOpened(object? sender, EventArgs e)
    {
        if (DataContext is MainWindowViewModel vm && vm.SavedWindowPlacement is { } saved)
        {
            ClampPositionToVisibleScreens(saved);
            if (saved.IsMaximized)
            {
                WindowState = WindowState.Maximized;
            }
        }
    }

    private void OnWindowPositionChanged(object? sender, PixelPointEventArgs e)
    {
        if (WindowState == WindowState.Normal)
        {
            _lastNormalPosition = e.Point;
        }
    }

    private void OnWindowPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property == BoundsProperty && WindowState == WindowState.Normal)
        {
            if (Bounds.Width >= MinWidth && Bounds.Height >= MinHeight)
            {
                _lastNormalWidth = Bounds.Width;
                _lastNormalHeight = Bounds.Height;
            }
        }
    }

    protected override void OnClosing(WindowClosingEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            vm.SaveWorkspaceState(CaptureWindowPlacement());
        }
        base.OnClosing(e);
    }

    private void ApplySavedPlacementPreOpen(WindowPlacementDto? saved)
    {
        if (saved is null || _placementApplied)
            return;
        _placementApplied = true;

        double w = double.IsFinite(saved.Width) ? Math.Max(MinWidth, saved.Width) : 1500;
        double h = double.IsFinite(saved.Height) ? Math.Max(MinHeight, saved.Height) : 920;
        Width = w;
        Height = h;
        _lastNormalWidth = w;
        _lastNormalHeight = h;

        if (saved.Left.HasValue && saved.Top.HasValue)
        {
            WindowStartupLocation = WindowStartupLocation.Manual;
            var pt = new PixelPoint(saved.Left.Value, saved.Top.Value);
            Position = pt;
            _lastNormalPosition = pt;
        }
    }

    private void ClampPositionToVisibleScreens(WindowPlacementDto saved)
    {
        if (!saved.Left.HasValue || !saved.Top.HasValue)
            return;

        IReadOnlyList<Screen> screens = Screens.All;
        if (screens.Count == 0)
            return;

        var savedPt = new PixelPoint(saved.Left.Value, saved.Top.Value);
        bool visibleOnAny = false;
        foreach (Screen scr in screens)
        {
            PixelRect area = scr.WorkingArea;
            if (savedPt.X >= area.X - 80 && savedPt.X < area.Right - 80 &&
                savedPt.Y >= area.Y - 20 && savedPt.Y < area.Bottom - 60)
            {
                visibleOnAny = true;
                break;
            }
        }

        if (!visibleOnAny)
        {
            Screen primary = Screens.Primary ?? screens[0];
            PixelRect area = primary.WorkingArea;
            var safePt = new PixelPoint(
                area.X + Math.Max(0, (area.Width - (int)Math.Min(area.Width, Width)) / 2),
                area.Y + Math.Max(0, (area.Height - (int)Math.Min(area.Height, Height)) / 2));
            Position = safePt;
            _lastNormalPosition = safePt;
        }
    }

    private WindowPlacementDto CaptureWindowPlacement()
    {
        bool maximized = WindowState == WindowState.Maximized;
        PixelPoint pos = WindowState == WindowState.Normal
            ? Position
            : (_lastNormalPosition ?? Position);
        double w = WindowState == WindowState.Normal && Bounds.Width >= MinWidth
            ? Bounds.Width
            : _lastNormalWidth;
        double h = WindowState == WindowState.Normal && Bounds.Height >= MinHeight
            ? Bounds.Height
            : _lastNormalHeight;

        return new WindowPlacementDto
        {
            Left = pos.X,
            Top = pos.Y,
            Width = Math.Round(w),
            Height = Math.Round(h),
            IsMaximized = maximized
        };
    }

    private async Task<string?> PickSmpFileAsync()
    {
        IReadOnlyList<IStorageFile> files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Выбор бинарной записи сигнала (.smp)",
            AllowMultiple = false,
            FileTypeFilter =
            [
                new FilePickerFileType("Записи ПРОВОДКА (.smp)") { Patterns = ["*.smp"] },
                FilePickerFileTypes.All
            ]
        });

        return files.Count > 0 ? files[0].TryGetLocalPath() : null;
    }
}
