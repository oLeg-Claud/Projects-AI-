using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using ProvodkaNG.Desktop.ViewModels;

namespace ProvodkaNG.Desktop.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private async void OnBrowseSmpClick(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainWindowViewModel vm)
            return;

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

        if (files.Count > 0)
        {
            string? localPath = files[0].TryGetLocalPath();
            if (!string.IsNullOrEmpty(localPath))
            {
                vm.SetSmpFilePath(localPath);
            }
        }
    }

    private async void OnBrowseFirClick(object? sender, RoutedEventArgs e)
    {
        if (DataContext is not MainWindowViewModel vm)
            return;

        IReadOnlyList<IStorageFile> files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Выбор файла коэффициентов КИХ-фильтра (KIH*.dat)",
            AllowMultiple = false,
            FileTypeFilter =
            [
                new FilePickerFileType("Коэффициенты КИХ (.dat)") { Patterns = ["*.dat"] },
                FilePickerFileTypes.All
            ]
        });

        if (files.Count > 0)
        {
            string? localPath = files[0].TryGetLocalPath();
            if (!string.IsNullOrEmpty(localPath))
            {
                vm.SetFirFilePath(localPath);
            }
        }
    }
}
