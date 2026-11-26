using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace ProvodkaNG.Desktop.Controls;

/// <summary>
/// Real-time dual-trace oscilloscope rendering raw ADC samples vs FIR-filtered output
/// directly on Avalonia DrawingContext with automatic amplitude scaling and 0xFE overflow warning.
/// </summary>
public sealed class OscilloscopeControl : Control
{
    public static readonly StyledProperty<double[]?> RawSamplesProperty =
        AvaloniaProperty.Register<OscilloscopeControl, double[]?>(nameof(RawSamples));

    public static readonly StyledProperty<double[]?> FilteredSamplesProperty =
        AvaloniaProperty.Register<OscilloscopeControl, double[]?>(nameof(FilteredSamples));

    public static readonly StyledProperty<bool> OverflowFlagProperty =
        AvaloniaProperty.Register<OscilloscopeControl, bool>(nameof(OverflowFlag));

    public static readonly StyledProperty<bool> IsDarkThemeProperty =
        AvaloniaProperty.Register<OscilloscopeControl, bool>(nameof(IsDarkTheme), true);

    static OscilloscopeControl()
    {
        AffectsRender<OscilloscopeControl>(
            RawSamplesProperty,
            FilteredSamplesProperty,
            OverflowFlagProperty,
            IsDarkThemeProperty);
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
        var rawPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#38BDF8") : Color.Parse("#0284C7")), 1.2);
        var firPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#F59E0B") : Color.Parse("#D97706")), 1.8);

        context.FillRectangle(bg, new Rect(0, 0, w, h));

        // Background grid (8 vertical x 4 horizontal)
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

        double maxAbs = 1.0;
        if (raw is { Length: > 0 })
        {
            for (int i = 0; i < raw.Length; i++)
                maxAbs = Math.Max(maxAbs, Math.Abs(raw[i]));
        }
        if (fir is { Length: > 0 })
        {
            for (int i = 0; i < fir.Length; i++)
                maxAbs = Math.Max(maxAbs, Math.Abs(fir[i]));
        }

        DrawTrace(context, raw, w, h, maxAbs, rawPen);
        DrawTrace(context, fir, w, h, maxAbs, firPen);

        // Scale & legend overlay
        var typeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);
        var legendText = new FormattedText(
            $"RAW (АЦП) / FIR (КИХ)   ±{maxAbs:G4}",
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            typeface,
            11.0,
            new SolidColorBrush(dark ? Color.Parse("#94A3B8") : Color.Parse("#475569")));
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

            double rx = w - alarmText.Width - 20;
            context.FillRectangle(alarmBg, new Rect(rx, 4, alarmText.Width + 12, alarmText.Height + 6));
            context.DrawText(alarmText, new Point(rx + 6, 7));
        }
    }

    private static void DrawTrace(
        DrawingContext context,
        double[]? samples,
        double width,
        double height,
        double maxAbs,
        Pen pen)
    {
        if (samples is null || samples.Length < 2)
            return;

        int n = samples.Length;
        double midY = height * 0.5;
        double scaleY = (height * 0.44) / maxAbs;
        int step = Math.Max(1, n / (int)Math.Max(200, width));

        var geometry = new StreamGeometry();
        using (var ctx = geometry.Open())
        {
            double x0 = 0;
            double y0 = midY - samples[0] * scaleY;
            ctx.BeginFigure(new Point(x0, y0), isFilled: false);

            for (int i = step; i < n; i += step)
            {
                double x = (double)i / (n - 1) * width;
                double y = midY - samples[i] * scaleY;
                ctx.LineTo(new Point(x, y));
            }
        }

        context.DrawGeometry(null, pen, geometry);
    }
}
