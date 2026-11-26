# DspCore (`DspCore.dll` / `libDspCore.so`) — Этап 1 (Ядро ЦОС C++17)

Кроссплатформенное нативное ядро цифровой обработки сигналов (ЦОС) и драйвера УСО для наземного комплекса телесистемы **ПРОВОДКА NG** (ТЗ §3).

## Возможности ядра

- **Единый интерфейс `IUsoSource` (3 реализации)**:
  1. `LegacySerialUsoSource` — COM/USB 57600 8N1 (Win32 `CreateFile`/`DCB` и POSIX `termios`), автопоиск `0xAA → 0x55 → 0x66`, управление усилением (`0x20..0x25` / `0x90..0x95`), частотой (`0..5`), корректором (`0x71/0x72`), чтение 5-байт (24-бит) и 6-байт (32-бит `uco32`) пакетов с контролем `0xFF` и детектором переполнения `0xFE`.
  2. `NewAdc32UsoSource` — новое УСО 32-бит АЦП (8 отсч./бит = 256 отсч./вектор).
  3. `SmpFilePlayerSource` — воспроизведение бинарных записей старого формата (`sun32` `.smp`) со скоростями `x1, x2, x4, x8` (или `0` для пакетной обработки без задержки).
- **КИХ-фильтрация (`FirFilter`)**: загрузка коэффициентов из унаследованных файлов `KIH16_4F.dat .. KIH1_8.dat` + SIMD-свёртка в `double`.
- **Передискретизация и автоподстройка частоты (`VirtualGridResampler`)**: приведение входного потока к виртуальной сетке `DLPOLr = 13` (СП = 1664 отсч., вектор = 416 отсч.) + динамическая компенсация дрейфа кварца по алгоритму `PIK5 / ID`.
- **Мягкая корреляция (`SoftCorrelator`)**: AVX2/FMA → SSE2 → scalar аккумуляторы типа `double` (без потери точности на 32-битном АЦП), 128-чиповая СП (`Gsp`) и 16 ортогональных кодов Рида — Мюллера `RM(1,4)` (`Gcodes`, `Ncod = 0, 1`) + инверсия фазы (5-й бит) + скользящее окно `±iG`.
- **Повекторный декодер (`FrameDecoder`)**: полная поддержка форматов `№1, №2 (НДМ), №11, №12, №13, №30, №31 (Геомаш CRC32)`, пауз ретранслятора, логарифмического ГК, стандартного и НУБТ азимута.
- **Zero-Latency C ABI (`dsp_core_api.h`)**: коллбэк `DspOnToolfaceCallback` вызывается немедленно после декодирования любого вектора `OTK*_L / AZ_OTK*_L / OTKu_L`, не дожидаясь конца кадра.

## Сборка

### Linux (`libDspCore.so`)
```bash
cd dsp_core
make test
```
Или через CMake:
```bash
cmake -S dsp_core -B dsp_core/build -DCMAKE_BUILD_TYPE=Release
cmake --build dsp_core/build
ctest --test-dir dsp_core/build --output-on-failure
```

### Windows (`DspCore.dll`, MSVC / MinGW)
```powershell
cmake -S dsp_core -B dsp_core/build -G "Visual Studio 17 2022" -A x64
cmake --build dsp_core/build --config Release
ctest --test-dir dsp_core/build -C Release --output-on-failure
```
Готовая библиотека: `dsp_core/build/Release/DspCore.dll`.
