using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using ProvodkaNG.Core;

namespace ProvodkaNG.Desktop.Controls;

/// <summary>
/// Real-time FFT Spectrum analyzer window (0 .. Fs/2, dB scale)
/// with active carrier frequency marker and peak frequency readout.
/// </summary>
public sealed class SpectrumControl : Control
{
    public static readonly StyledProperty<SpectrumSnapshot?> SpectrumProperty =
        AvaloniaProperty.Register<SpectrumControl, SpectrumSnapshot?>(nameof(Spectrum));

    public static readonly StyledProperty<double> CarrierFreqHzProperty =
        AvaloniaProperty.Register<SpectrumControl, double>(nameof(CarrierFreqHz), 5.0);

    public static readonly StyledProperty<bool> IsDarkThemeProperty =
        AvaloniaProperty.Register<SpectrumControl, bool>(nameof(IsDarkTheme), true);

    static SpectrumControl()
    {
        AffectsRender<SpectrumControl>(
            SpectrumProperty,
            CarrierFreqHzProperty,
            IsDarkThemeProperty);
    }

    public SpectrumControl()
    {
        MinHeight = 140;
        ClipToBounds = true;
    }

    public SpectrumSnapshot? Spectrum
    {
        get => GetValue(SpectrumProperty);
        set => SetValue(SpectrumProperty, value);
    }

    public double CarrierFreqHz
    {
        get => GetValue(CarrierFreqHzProperty);
        set => SetValue(CarrierFreqHzProperty, value);
    }

    public bool IsDarkTheme
    {
        get => GetValue(IsDarkThemeProperty);
        set => SetValue(IsDarkThemeProperty, value);
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);

        double w = Bounds.Width;
        double h = Bounds.Height;
        if (w < 60 || h < 50)
            return;

        bool dark = IsDarkTheme;
        var bg = new SolidColorBrush(dark ? Color.Parse("#090D12") : Color.Parse("#FFFFFF"));
        var gridPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#1E293B") : Color.Parse("#E2E8F0")), 1.0);
        var linePen = new Pen(new SolidColorBrush(dark ? Color.Parse("#22D3EE") : Color.Parse("#0284C7")), 1.6);
        var fillBrush = new SolidColorBrush(dark ? Color.FromArgb(48, 34, 211, 238) : Color.FromArgb(40, 2, 132, 199));
        var carrierPen = new Pen(new SolidColorBrush(Color.Parse("#F59E0B")), 1.5, DashStyle.Dash);
        var mutedBrush = new SolidColorBrush(dark ? Color.Parse("#94A3B8") : Color.Parse("#475569"));
        var accentBrush = new SolidColorBrush(dark ? Color.Parse("#F59E0B") : Color.Parse("#B45309"));
        var typeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);

        context.FillRectangle(bg, new Rect(0, 0, w, h));

        double padLeft = 36;
        double padRight = 10;
        double padTop = 24;
        double padBottom = 20;
        double plotW = Math.Max(10, w - padLeft - padRight);
        double plotH = Math.Max(10, h - padTop - padBottom);

        // Display range: 0 .. 65 Hz band covering MWD carriers (1.8..16 Hz), harmonics, and 50/60 Hz mains noise
        SpectrumSnapshot? spec = Spectrum;
        double nyquist = spec?.SampleRateHz * 0.5 ?? 125.0;
        double maxFreqHz = Math.Min(nyquist, 65.0);
        const double minDb = -80.0;
        const double maxDb = 0.0;

        // Horizontal dB grid (-80, -60, -40, -20, 0 dB)
        for (int db = 0; db >= -80; db -= 20)
        {
            double y = padTop + (maxDb - db) / (maxDb - minDb) * plotH;
            context.DrawLine(gridPen, new Point(padLeft, y), new Point(padLeft + plotW, y));
            var lbl = new FormattedText($"{db}", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 9.5, mutedBrush);
            context.DrawText(lbl, new Point(4, y - lbl.Height * 0.5));
        }

        // Vertical frequency grid (every 5 Hz up to 65 Hz)
        double labelStep = plotW < 340 ? 10.0 : 5.0;
        for (double f = 0; f <= maxFreqHz + 0.1; f += 5.0)
        {
            double x = padLeft + (f / maxFreqHz) * plotW;
            context.DrawLine(gridPen, new Point(x, padTop), new Point(x, padTop + plotH));
            bool showLabel = Math.Abs(f % labelStep) < 0.01 || Math.Abs(f - maxFreqHz) < 0.1;
            if (showLabel)
            {
                var lbl = new FormattedText($"{f:0}", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 9.5, mutedBrush);
                double lx = Math.Clamp(x - lbl.Width * 0.5, padLeft, padLeft + plotW - lbl.Width);
                context.DrawText(lbl, new Point(lx, padTop + plotH + 3));
            }
        }

        // Active carrier frequency vertical marker
        if (CarrierFreqHz > 0 && CarrierFreqHz <= maxFreqHz)
        {
            double cx = padLeft + (CarrierFreqHz / maxFreqHz) * plotW;
            context.DrawLine(carrierPen, new Point(cx, padTop), new Point(cx, padTop + plotH));
        }

        if (spec is { FrequenciesHz.Length: > 1 })
        {
            var lineGeo = new StreamGeometry();
            var fillGeo = new StreamGeometry();
            using (var lctx = lineGeo.Open())
            using (var fctx = fillGeo.Open())
            {
                bool started = false;
                double lastX = padLeft;
                for (int i = 0; i < spec.FrequenciesHz.Length; i++)
                {
                    double f = spec.FrequenciesHz[i];
                    if (f > maxFreqHz) break;

                    double db = Math.Clamp(spec.MagnitudesDb[i], minDb, maxDb);
                    double x = padLeft + (f / maxFreqHz) * plotW;
                    double y = padTop + (maxDb - db) / (maxDb - minDb) * plotH;
                    lastX = x;

                    if (!started)
                    {
                        lctx.BeginFigure(new Point(x, y), false);
                        fctx.BeginFigure(new Point(x, padTop + plotH), true);
                        fctx.LineTo(new Point(x, y));
                        started = true;
                    }
                    else
                    {
                        lctx.LineTo(new Point(x, y));
                        fctx.LineTo(new Point(x, y));
                    }
                }

                if (started)
                {
                    fctx.LineTo(new Point(lastX, padTop + plotH));
                    fctx.EndFigure(true);
                }
            }

            context.DrawGeometry(fillBrush, null, fillGeo);
            context.DrawGeometry(null, linePen, lineGeo);
        }

        string header = spec is not null
            ? $"БПФ (0–{maxFreqHz:0} Гц)   Несущая: {CarrierFreqHz:0.#} Гц   Пик: {spec.PeakFrequencyHz:0.0} Гц   Шум: {spec.NoiseFloorDb:0.0} дБ"
            : $"БПФ (0–{maxFreqHz:0} Гц)   Несущая: {CarrierFreqHz:0.#} Гц   Ожидание данных...";

        var headerText = new FormattedText(header, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 10.5, accentBrush);
        context.DrawText(headerText, new Point(padLeft, 5));
    }
}
