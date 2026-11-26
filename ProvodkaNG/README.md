# ПРОВОДКА NG — Этап 2 (C# P/Invoke обёртка и каркас Avalonia UI)

Кроссплатформенный (Windows / Linux) управляемый слой .NET 8 поверх нативного C++17 ядра `DspCore.dll` / `libDspCore.so` (Этап 1).

## Структура решения (`ProvodkaNG.sln`)

- **`ProvodkaNG.Core`** — потокобезопасная C# P/Invoke обёртка над `dsp_core_api.h`:
  - `Native/DspNativeTypes.cs` — blittable-структуры (`DspConfigNative` = 624 B, `DspFrameDataNative` = 576 B, `DspToolfaceEventNative` = 40 B, `DspVectorEventNative` = 28 B, `DspSignalBlockNative` = 24 B) с фиксированными буферами `fixed byte`, гарантирующие 100% бинарное совпадение ABI на Windows x64 и Linux x64 без оверхеда маршалинга строк.
  - `Native/DspNativeMethods.cs` — кроссплатформенный `NativeLibrary.SetDllImportResolver` для автоматического поиска `DspCore.dll` / `libDspCore.so` / `libDspCore.dylib`.
  - `DspEngineClient.cs` — `IDisposable`-фасад с закреплением нативных делегатов в памяти (защита от `CallbackOnCollectedDelegate`), перехватом исключений на границе C++/C# и событиями `ToolfaceReceived` (Zero-Latency), `VectorDecoded`, `FrameCompleted`, `SignalBlockReceived`.
- **`ProvodkaNG.Desktop`** — кроссплатформенный каркас UI на **Avalonia UI 11 + CommunityToolkit.Mvvm**:
  - `Themes/ShiftThemes.axaml` — промышленные темы «Ночная вахта» (Dark) и «Дневная вахта» (Light).
  - `Controls/ToolfaceDialControl.cs` — векторный Zero-Latency циферблат отклонителя 360° со следом последних 6 замеров и задержкой ЦОС в мс.
  - `Controls/OscilloscopeControl.cs` — двухлучевой осциллограф (RAW АЦП + КИХ-фильтр) с индикатором переполнения `0xFE`.
  - `Views/MainWindow.axaml` — панель управления УСО (COM-порт, автопоиск `0xAA→0x55→0x66`, выбор `.smp` и `KIH*.dat`, усиление, частота, корректор, порог ДСП, `PIK5/ID`), KPI-плитки инклинометрии и НДМ, лента 5-битных векторов `RM(1,4)` и вкладки-заготовки для Этапов 3 и 4.
- **`ProvodkaNG.Tests`** — модульные и интеграционные xUnit-тесты (проверка точных размеров/смещений структур C ABI, живой P/Invoke к `DspCore`, и headless-тесты `MainWindowViewModel`).

## Сборка и запуск

1. Сначала соберите нативное ядро ЦОС (`dsp_core`):
   - **Windows**: `cmake -S dsp_core -B dsp_core/build -G "Visual Studio 17 2022" -A x64 && cmake --build dsp_core/build --config Release`
   - **Linux**: `make -C dsp_core test`
2. Соберите и запустите C#-решение (.NET 8 SDK):
```bash
dotnet test ProvodkaNG/ProvodkaNG.sln
dotnet run --project ProvodkaNG/ProvodkaNG.Desktop
```
`ProvodkaNG.Core.csproj` автоматически копирует собранную `DspCore.dll` / `libDspCore.so` из `dsp_core/build` в выходную директорию.
