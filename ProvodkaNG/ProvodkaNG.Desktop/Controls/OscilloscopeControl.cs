using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace ProvodkaNG.Desktop.Controls;

/// <summary>
/// Real-time dual-trace oscilloscope rendering raw ADC samples vs FIR-filtered output.
/// Automatically removes DC bias before vertical scaling so 32-bit ADC & legacy .smp
/// recordings with large DC offsets always display crisp full-scale waveforms.
/// </summary>
public sealed class OscilloscopeControl : Control
{
    public static readonly StyledProperty<double[]?> RawSamplesProperty =
        AvaloniaProperty.Register<OscilloscopeControl, double[]?>(nameof(RawSamples));

    public static readonly StyledProperty<double[]?> FilteredSamplesProperty =
        AvaloniaProperty.Register<OscilloscopeControl, double[]?>(nameof(FilteredSamples));

    public static readonly StyledProperty<bool> OverflowFlagProperty =
        AvaloniaProperty.Register<OscilloscopeControl, bool>(nameof(OverflowFlag));

    public static readonly StyledProperty<long> TotalSamplesReceivedProperty =
        AvaloniaProperty.Register<OscilloscopeControl, long>(nameof(TotalSamplesReceived));

    public static readonly StyledProperty<bool> IsDarkThemeProperty =
        AvaloniaProperty.Register<OscilloscopeControl, bool>(nameof(IsDarkTheme), true);

    static OscilloscopeControl()
    {
        AffectsRender<OscilloscopeControl>(
            RawSamplesProperty,
            FilteredSamplesProperty,
            OverflowFlagProperty,
            TotalSamplesReceivedProperty,
            IsDarkThemeProperty);
    }

    public OscilloscopeControl()
    {
        MinHeight = 140;
        ClipToBounds = true;
    }

    public double[]? RawSamples
    {
        get => GetValue(RawSamplesProperty);
        set => SetValue(RawSamplesProperty, value);
    }

    public double[]? FilteredSamples
    {
        get => GetValue(FilteredSamplesProperty);
        set => SetValue(FilteredSamplesProperty, value);
    }

    public bool OverflowFlag
    {
        get => GetValue(OverflowFlagProperty);
        set => SetValue(OverflowFlagProperty, value);
    }

    public long TotalSamplesReceived
    {
        get => GetValue(TotalSamplesReceivedProperty);
        set => SetValue(TotalSamplesReceivedProperty, value);
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
        if (w < 40 || h < 40)
            return;

        bool dark = IsDarkTheme;
        var bg = new SolidColorBrush(dark ? Color.Parse("#090D12") : Color.Parse("#FFFFFF"));
        var gridPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#1E293B") : Color.Parse("#E2E8F0")), 1.0);
        var zeroPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#334155") : Color.Parse("#94A3B8")), 1.2);
        var rawPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#38BDF8") : Color.Parse("#0284C7")), 1.25);
        var firPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#F59E0B") : Color.Parse("#D97706")), 1.85);
        var mutedBrush = new SolidColorBrush(dark ? Color.Parse("#94A3B8") : Color.Parse("#475569"));

        context.FillRectangle(bg, new Rect(0, 0, w, h));

        for (int i = 1; i < 8; i++)
        {
            double x = w * i / 8.0;
            context.DrawLine(gridPen, new Point(x, 0), new Point(x, h));
        }
        for (int j = 1; j < 4; j++)
        {
            double y = h * j / 4.0;
            context.DrawLine(j == 2 ? zeroPen : gridPen, new Point(0, y), new Point(w, y));
        }

        double[]? raw = RawSamples;
        double[]? fir = FilteredSamples;

        (double rawMean, double rawSpan) = ComputeMeanAndPeakDeviation(raw);
        (double firMean, double firSpan) = ComputeMeanAndPeakDeviation(fir);

        double span = Math.Max(1.0, Math.Max(rawSpan, firSpan));

        DrawTrace(context, raw, rawMean, w, h, span, rawPen);
        DrawTrace(context, fir, firMean, w, h, span, firPen);

        var typeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);
        string info = TotalSamplesReceived > 0
            ? $"RAW (голуб.) / КИХ (янт.)   AC ±{span:G4}   DC {rawMean:G4}   Отсч: {TotalSamplesReceived}"
            : "Ожидание сигнала с УСО32 / .smp...";

        var legendText = new FormattedText(
            info,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            typeface,
            11.0,
            mutedBrush);
        context.DrawText(legendText, new Point(8, 6));

        if (OverflowFlag)
        {
            var alarmBg = new SolidColorBrush(Color.Parse("#DC2626"));
            var alarmText = new FormattedText(
                "ПЕРЕПОЛНЕНИЕ АЦП (0xFE)",
                CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight,
                new Typeface("Consolas", FontStyle.Normal, FontWeight.Bold),
                11.0,
                Brushes.White);

            double rx = Math.Max(8, w - alarmText.Width - 20);
            context.FillRectangle(alarmBg, new Rect(rx, 4, alarmText.Width + 12, alarmText.Height + 6));
            context.DrawText(alarmText, new Point(rx + 6, 7));
        }
    }

    private static (double Mean, double MaxDev) ComputeMeanAndPeakDeviation(double[]? samples)
    {
        if (samples is null || samples.Length == 0)
            return (0.0, 0.0);

        double sum = 0.0;
        int valid = 0;
        for (int i = 0; i < samples.Length; i++)
        {
            double v = samples[i];
            if (double.IsFinite(v))
            {
                sum += v;
                valid++;
            }
        }
        if (valid == 0)
            return (0.0, 0.0);

        double mean = sum / valid;
        double maxDev = 0.0;
        for (int i = 0; i < samples.Length; i++)
        {
            double v = samples[i];
            if (double.IsFinite(v))
            {
                double d = Math.Abs(v - mean);
                if (d > maxDev) maxDev = d;
            }
        }
        return (mean, maxDev);
    }

    private static void DrawTrace(
        DrawingContext context,
        double[]? samples,
        double mean,
        double width,
        double height,
        double maxDev,
        Pen pen)
    {
        if (samples is null || samples.Length < 2)
            return;

        int n = samples.Length;
        double midY = height * 0.5;
        double scaleY = (height * 0.42) / maxDev;
        int step = Math.Max(1, n / (int)Math.Max(256, width));

        var geometry = new StreamGeometry();
        using (var ctx = geometry.Open())
        {
            double v0 = double.IsFinite(samples[0]) ? samples[0] - mean : 0.0;
            double y0 = Math.Clamp(midY - v0 * scaleY, 4.0, height - 4.0);
            ctx.BeginFigure(new Point(0, y0), isFilled: false);

            for (int i = step; i < n; i += step)
            {
                double x = (double)i / (n - 1) * width;
                double v = double.IsFinite(samples[i]) ? samples[i] - mean : 0.0;
                double y = Math.Clamp(midY - v * scaleY, 4.0, height - 4.0);
                ctx.LineTo(new Point(x, y));
            }
        }

        context.DrawGeometry(null, pen, geometry);
    }
}
