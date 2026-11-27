using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace ProvodkaNG.Desktop.Controls;

/// <summary>
/// Dedicated real-time Sync Preamble (СП) Correlation Scope (-100% .. +100%):
/// Displays continuous AVX2 signed Pearson correlation of the 128-chip SP template
/// over ~1.8 full frames, with ±DCP threshold lines and highlighted SP lock peaks.
/// </summary>
public sealed class SyncCorrelationControl : Control
{
    public static readonly StyledProperty<int[]?> LiveSyncCorrTraceProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, int[]?>(nameof(LiveSyncCorrTrace));

    public static readonly StyledProperty<IReadOnlyList<int>?> SyncCorrHistoryProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, IReadOnlyList<int>?>(nameof(SyncCorrHistory));

    public static readonly StyledProperty<int> LiveSyncCorrSignedPctProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, int>(nameof(LiveSyncCorrSignedPct), 0);

    public static readonly StyledProperty<int> LiveSyncPeakPctProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, int>(nameof(LiveSyncPeakPct), 0);

    public static readonly StyledProperty<int> LatestSyncCorrPctProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, int>(nameof(LatestSyncCorrPct), 0);

    public static readonly StyledProperty<int> DcpThresholdProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, int>(nameof(DcpThreshold), 55);

    public static readonly StyledProperty<int> LastPikDriftProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, int>(nameof(LastPikDrift), 0);

    public static readonly StyledProperty<bool> IsDarkThemeProperty =
        AvaloniaProperty.Register<SyncCorrelationControl, bool>(nameof(IsDarkTheme), true);

    static SyncCorrelationControl()
    {
        AffectsRender<SyncCorrelationControl>(
            LiveSyncCorrTraceProperty,
            SyncCorrHistoryProperty,
            LiveSyncCorrSignedPctProperty,
            LiveSyncPeakPctProperty,
            LatestSyncCorrPctProperty,
            DcpThresholdProperty,
            LastPikDriftProperty,
            IsDarkThemeProperty);
    }

    public SyncCorrelationControl()
    {
        MinHeight = 140;
        ClipToBounds = true;
    }

    public int[]? LiveSyncCorrTrace
    {
        get => GetValue(LiveSyncCorrTraceProperty);
        set => SetValue(LiveSyncCorrTraceProperty, value);
    }

    public IReadOnlyList<int>? SyncCorrHistory
    {
        get => GetValue(SyncCorrHistoryProperty);
        set => SetValue(SyncCorrHistoryProperty, value);
    }

    public int LiveSyncCorrSignedPct
    {
        get => GetValue(LiveSyncCorrSignedPctProperty);
        set => SetValue(LiveSyncCorrSignedPctProperty, value);
    }

    public int LiveSyncPeakPct
    {
        get => GetValue(LiveSyncPeakPctProperty);
        set => SetValue(LiveSyncPeakPctProperty, value);
    }

    public int LatestSyncCorrPct
    {
        get => GetValue(LatestSyncCorrPctProperty);
        set => SetValue(LatestSyncCorrPctProperty, value);
    }

    public int DcpThreshold
    {
        get => GetValue(DcpThresholdProperty);
        set => SetValue(DcpThresholdProperty, value);
    }

    public int LastPikDrift
    {
        get => GetValue(LastPikDriftProperty);
        set => SetValue(LastPikDriftProperty, value);
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
        if (w < 80 || h < 60)
            return;

        bool dark = IsDarkTheme;
        var bg = new SolidColorBrush(dark ? Color.Parse("#090D12") : Color.Parse("#FFFFFF"));
        var gridPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#1E293B") : Color.Parse("#E2E8F0")), 1.0);
        var zeroPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#334155") : Color.Parse("#94A3B8")), 1.2);
        var threshPen = new Pen(new SolidColorBrush(Color.Parse("#F59E0B")), 1.3, DashStyle.Dash);
        var tracePen = new Pen(new SolidColorBrush(dark ? Color.Parse("#38BDF8") : Color.Parse("#0284C7")), 1.4);
        var posPeakBrush = new SolidColorBrush(Color.Parse("#10B981"));
        var negPeakBrush = new SolidColorBrush(Color.Parse("#22D3EE"));
        var posPeakPen = new Pen(posPeakBrush, 2.0);
        var negPeakPen = new Pen(negPeakBrush, 2.0);
        var fillBrush = new SolidColorBrush(dark ? Color.FromArgb(42, 56, 189, 248) : Color.FromArgb(32, 2, 132, 199));
        var mutedBrush = new SolidColorBrush(dark ? Color.Parse("#94A3B8") : Color.Parse("#475569"));
        var accentBrush = new SolidColorBrush(dark ? Color.Parse("#10B981") : Color.Parse("#047857"));
        var typeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);

        context.FillRectangle(bg, new Rect(0, 0, w, h));

        double padL = 38;
        double padR = 10;
        double padTop = 24;
        double padBottom = 14;
        double plotW = Math.Max(10, w - padL - padR);
        double plotH = Math.Max(10, h - padTop - padBottom);
        double midY = padTop + plotH * 0.5;

        double ValToY(double pct) => midY - (Math.Clamp(pct, -100.0, 100.0) / 100.0) * (plotH * 0.48);

        // Plot border & horizontal grid (+100, +50, 0, -50, -100)
        context.DrawRectangle(null, gridPen, new Rect(padL, padTop, plotW, plotH));
        int[] yTicks = [100, 50, 0, -50, -100];
        foreach (int tick in yTicks)
        {
            double y = ValToY(tick);
            context.DrawLine(tick == 0 ? zeroPen : gridPen, new Point(padL, y), new Point(padL + plotW, y));
            string label = tick > 0 ? $"+{tick}" : tick.ToString(CultureInfo.InvariantCulture);
            var lbl = new FormattedText(label, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.5, mutedBrush);
            context.DrawText(lbl, new Point(Math.Max(2, padL - lbl.Width - 4), y - lbl.Height * 0.5));
        }

        // Vertical time divisions
        for (int d = 1; d < 8; d++)
        {
            double gx = padL + plotW * d / 8.0;
            context.DrawLine(gridPen, new Point(gx, padTop), new Point(gx, padTop + plotH));
        }

        // ±DCP threshold dashed lines
        int thr = Math.Clamp(DcpThreshold, 10, 100);
        double posThrY = ValToY(thr);
        double negThrY = ValToY(-thr);
        context.DrawLine(threshPen, new Point(padL, posThrY), new Point(padL + plotW, posThrY));
        context.DrawLine(threshPen, new Point(padL, negThrY), new Point(padL + plotW, negThrY));

        int[]? trace = LiveSyncCorrTrace;
        if (trace is { Length: > 0 })
        {
            int count = trace.Length;
            var lineGeo = new StreamGeometry();
            var fillGeo = new StreamGeometry();
            using (var lctx = lineGeo.Open())
            using (var fctx = fillGeo.Open())
            {
                double lastX = padL;
                for (int i = 0; i < count; i++)
                {
                    double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                    double y = ValToY(trace[i]);
                    lastX = x;

                    if (i == 0)
                    {
                        lctx.BeginFigure(new Point(x, y), false);
                        fctx.BeginFigure(new Point(x, midY), true);
                        fctx.LineTo(new Point(x, y));
                    }
                    else
                    {
                        lctx.LineTo(new Point(x, y));
                        fctx.LineTo(new Point(x, y));
                    }
                }
                fctx.LineTo(new Point(lastX, midY));
                fctx.EndFigure(true);
            }

            context.DrawGeometry(fillBrush, null, fillGeo);
            context.DrawGeometry(null, tracePen, lineGeo);

            // Highlight every point that reaches or exceeds ±DCP threshold
            for (int i = 0; i < count; i++)
            {
                int v = trace[i];
                if (Math.Abs(v) < thr) continue;
                double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                double y = ValToY(v);
                bool pos = v >= 0;
                context.DrawLine(pos ? posPeakPen : negPeakPen, new Point(x, midY), new Point(x, y));
                context.DrawEllipse(pos ? posPeakBrush : negPeakBrush, null, new Point(x, y), 3.0, 3.0);
            }
        }
        else if (SyncCorrHistory is { Count: > 0 } hist)
        {
            // Fallback when running against an older native library without live trace export
            int count = hist.Count;
            if (count > 1)
            {
                var geo = new StreamGeometry();
                using (var ctx = geo.Open())
                {
                    for (int i = 0; i < count; i++)
                    {
                        double x = padL + (double)i / (count - 1) * plotW;
                        double y = ValToY(hist[i]);
                        if (i == 0) ctx.BeginFigure(new Point(x, y), false);
                        else ctx.LineTo(new Point(x, y));
                    }
                }
                context.DrawGeometry(null, posPeakPen, geo);
            }
            for (int i = 0; i < count; i++)
            {
                double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                double y = ValToY(hist[i]);
                context.DrawEllipse(posPeakBrush, null, new Point(x, y), 3.5, 3.5);
            }
        }
        else
        {
            var waitText = new FormattedText(
                "Ожидание накопления окна СП (128 чипов)...",
                CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight,
                typeface,
                11,
                mutedBrush);
            context.DrawText(waitText, new Point(padL + (plotW - waitText.Width) * 0.5, midY - waitText.Height - 6));
        }

        string header =
            $"Корреляция СП   Тек: {LiveSyncCorrSignedPct:+#;-#;0}%   Пик: {LiveSyncPeakPct}%   Захват: {LatestSyncCorrPct}%   Порог: ±{thr}%   PIK5: {LastPikDrift:+#;-#;0}";
        var headerText = new FormattedText(header, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 10.5, accentBrush);
        context.DrawText(headerText, new Point(padL, 5));
    }
}
