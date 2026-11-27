using Avalonia.Controls;
using Avalonia.Platform.Storage;
using ProvodkaNG.Desktop.ViewModels;

namespace ProvodkaNG.Desktop.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            vm.PickSmpFileAsync = PickSmpFileAsync;
            vm.RequestExit = Close;
        }
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
