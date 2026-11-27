using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace ProvodkaNG.Desktop.Controls;

/// <summary>
/// Live Correlation Diagnostic Control:
/// - Top strip: Sync Preamble (СП) correlation history (%) + DCP threshold line + PIK5 drift.
/// - Bottom chart: All 16 RM(1,4) orthogonal code correlations (-100%..+100%) from AVX2 SoftCorrelator.
/// </summary>
public sealed class CorrelationControl : Control
{
    public static readonly StyledProperty<int[]?> LiveSyncCorrTraceProperty =
        AvaloniaProperty.Register<CorrelationControl, int[]?>(nameof(LiveSyncCorrTrace));

    public static readonly StyledProperty<int> LiveSyncPeakPctProperty =
        AvaloniaProperty.Register<CorrelationControl, int>(nameof(LiveSyncPeakPct), 0);

    public static readonly StyledProperty<int[]?> CodeScoresPctProperty =
        AvaloniaProperty.Register<CorrelationControl, int[]?>(nameof(CodeScoresPct));

    public static readonly StyledProperty<IReadOnlyList<int>?> SyncCorrHistoryProperty =
        AvaloniaProperty.Register<CorrelationControl, IReadOnlyList<int>?>(nameof(SyncCorrHistory));

    public static readonly StyledProperty<int> WinningCode5BitProperty =
        AvaloniaProperty.Register<CorrelationControl, int>(nameof(WinningCode5Bit), -1);

    public static readonly StyledProperty<int> DcpThresholdProperty =
        AvaloniaProperty.Register<CorrelationControl, int>(nameof(DcpThreshold), 55);

    public static readonly StyledProperty<int> LatestSyncCorrPctProperty =
        AvaloniaProperty.Register<CorrelationControl, int>(nameof(LatestSyncCorrPct), 0);

    public static readonly StyledProperty<int> LastPikDriftProperty =
        AvaloniaProperty.Register<CorrelationControl, int>(nameof(LastPikDrift), 0);

    public static readonly StyledProperty<bool> IsDarkThemeProperty =
        AvaloniaProperty.Register<CorrelationControl, bool>(nameof(IsDarkTheme), true);

    static CorrelationControl()
    {
        AffectsRender<CorrelationControl>(
            LiveSyncCorrTraceProperty,
            LiveSyncPeakPctProperty,
            CodeScoresPctProperty,
            SyncCorrHistoryProperty,
            WinningCode5BitProperty,
            DcpThresholdProperty,
            LatestSyncCorrPctProperty,
            LastPikDriftProperty,
            IsDarkThemeProperty);
    }

    public CorrelationControl()
    {
        MinHeight = 140;
        ClipToBounds = true;
    }

    public int[]? LiveSyncCorrTrace
    {
        get => GetValue(LiveSyncCorrTraceProperty);
        set => SetValue(LiveSyncCorrTraceProperty, value);
    }

    public int LiveSyncPeakPct
    {
        get => GetValue(LiveSyncPeakPctProperty);
        set => SetValue(LiveSyncPeakPctProperty, value);
    }

    public int[]? CodeScoresPct
    {
        get => GetValue(CodeScoresPctProperty);
        set => SetValue(CodeScoresPctProperty, value);
    }

    public IReadOnlyList<int>? SyncCorrHistory
    {
        get => GetValue(SyncCorrHistoryProperty);
        set => SetValue(SyncCorrHistoryProperty, value);
    }

    public int WinningCode5Bit
    {
        get => GetValue(WinningCode5BitProperty);
        set => SetValue(WinningCode5BitProperty, value);
    }

    public int DcpThreshold
    {
        get => GetValue(DcpThresholdProperty);
        set => SetValue(DcpThresholdProperty, value);
    }

    public int LatestSyncCorrPct
    {
        get => GetValue(LatestSyncCorrPctProperty);
        set => SetValue(LatestSyncCorrPctProperty, value);
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
        if (w < 80 || h < 70)
            return;

        bool dark = IsDarkTheme;
        var bg = new SolidColorBrush(dark ? Color.Parse("#090D12") : Color.Parse("#FFFFFF"));
        var gridPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#1E293B") : Color.Parse("#E2E8F0")), 1.0);
        var zeroPen = new Pen(new SolidColorBrush(dark ? Color.Parse("#334155") : Color.Parse("#94A3B8")), 1.2);
        var threshPen = new Pen(new SolidColorBrush(Color.Parse("#F59E0B")), 1.3, DashStyle.Dash);
        var mutedBrush = new SolidColorBrush(dark ? Color.Parse("#94A3B8") : Color.Parse("#475569"));
        var okBrush = new SolidColorBrush(Color.Parse("#10B981"));
        var barIdleBrush = new SolidColorBrush(dark ? Color.Parse("#334155") : Color.Parse("#94A3B8"));
        var barWinPosBrush = new SolidColorBrush(Color.Parse("#10B981"));
        var barWinNegBrush = new SolidColorBrush(Color.Parse("#22D3EE"));
        var syncPen = new Pen(new SolidColorBrush(Color.Parse("#10B981")), 1.6);
        var syncFill = new SolidColorBrush(dark ? Color.FromArgb(46, 16, 185, 129) : Color.FromArgb(36, 16, 185, 129));
        var typeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);

        context.FillRectangle(bg, new Rect(0, 0, w, h));

        double padL = 32;
        double padR = 10;
        double plotW = Math.Max(10, w - padL - padR);

        // --- TOP ZONE (38%): Live |СП| Correlation + Locked Peaks ---
        double topY = 22;
        double topH = Math.Max(32, (h - 46) * 0.38);
        int thr = Math.Clamp(DcpThreshold, 0, 100);

        var spHeader = new FormattedText(
            $"СП: захв. {LatestSyncCorrPct}%   Пик: {LiveSyncPeakPct}%   Порог: {thr}%   PIK5: {LastPikDrift:+#;-#;0}",
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            typeface,
            10.5,
            okBrush);
        context.DrawText(spHeader, new Point(padL, 4));

        context.DrawRectangle(null, gridPen, new Rect(padL, topY, plotW, topH));
        double halfTopY = topY + topH * 0.5;
        context.DrawLine(gridPen, new Point(padL, halfTopY), new Point(padL + plotW, halfTopY));
        context.DrawText(new FormattedText("100", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.0, mutedBrush), new Point(4, topY - 4));
        context.DrawText(new FormattedText("50", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.0, mutedBrush), new Point(10, halfTopY - 4));
        context.DrawText(new FormattedText("0", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.0, mutedBrush), new Point(16, topY + topH - 8));

        double thrY = topY + (1.0 - thr / 100.0) * topH;
        context.DrawLine(threshPen, new Point(padL, thrY), new Point(padL + plotW, thrY));

        int[]? liveTrace = LiveSyncCorrTrace;
        if (liveTrace is { Length: > 0 })
        {
            int count = liveTrace.Length;
            var lineGeo = new StreamGeometry();
            var fillGeo = new StreamGeometry();
            using (var lctx = lineGeo.Open())
            using (var fctx = fillGeo.Open())
            {
                double lastX = padL;
                for (int i = 0; i < count; i++)
                {
                    int mag = Math.Clamp(Math.Abs(liveTrace[i]), 0, 100);
                    double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                    double y = topY + (1.0 - mag / 100.0) * topH;
                    lastX = x;
                    if (i == 0)
                    {
                        lctx.BeginFigure(new Point(x, y), false);
                        fctx.BeginFigure(new Point(x, topY + topH), true);
                        fctx.LineTo(new Point(x, y));
                    }
                    else
                    {
                        lctx.LineTo(new Point(x, y));
                        fctx.LineTo(new Point(x, y));
                    }
                }
                fctx.LineTo(new Point(lastX, topY + topH));
                fctx.EndFigure(true);
            }
            context.DrawGeometry(syncFill, null, fillGeo);
            context.DrawGeometry(null, syncPen, lineGeo);

            for (int i = 0; i < count; i++)
            {
                int mag = Math.Abs(liveTrace[i]);
                if (mag < thr) continue;
                double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                double y = topY + (1.0 - Math.Clamp(mag, 0, 100) / 100.0) * topH;
                context.DrawEllipse(liveTrace[i] >= 0 ? barWinPosBrush : barWinNegBrush, null, new Point(x, y), 2.8, 2.8);
            }
        }
        else if (SyncCorrHistory is { Count: > 0 } hist)
        {
            int count = hist.Count;
            if (count > 1)
            {
                var geo = new StreamGeometry();
                using (var ctx = geo.Open())
                {
                    for (int i = 0; i < count; i++)
                    {
                        double x = padL + (double)i / (count - 1) * plotW;
                        double y = topY + (1.0 - Math.Clamp(hist[i], 0, 100) / 100.0) * topH;
                        if (i == 0) ctx.BeginFigure(new Point(x, y), false);
                        else ctx.LineTo(new Point(x, y));
                    }
                }
                context.DrawGeometry(null, syncPen, geo);
            }
            for (int i = 0; i < count; i++)
            {
                double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                double y = topY + (1.0 - Math.Clamp(hist[i], 0, 100) / 100.0) * topH;
                context.DrawEllipse(okBrush, null, new Point(x, y), 3.0, 3.0);
            }
        }

        // --- BOTTOM ZONE (65%): 16 RM(1,4) Orthogonal Code Bars ---
        double botHeaderY = topY + topH + 6;
        string winLabel = WinningCode5Bit >= 0
            ? $"RM(1,4) 16 кодов   Победитель: C{WinningCode5Bit & 0x0F} (5-бит=0x{WinningCode5Bit:X2}, фаза={(WinningCode5Bit >= 16 ? "-" : "+")})"
            : "RM(1,4) 16 кодов — ожидание вектора...";

        var rmHeader = new FormattedText(winLabel, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 10.5, mutedBrush);
        context.DrawText(rmHeader, new Point(padL, botHeaderY));

        double botY = botHeaderY + 16;
        double botH = Math.Max(28, h - botY - 18);
        double midY = botY + botH * 0.5;

        context.DrawRectangle(null, gridPen, new Rect(padL, botY, plotW, botH));
        context.DrawLine(zeroPen, new Point(padL, midY), new Point(padL + plotW, midY));

        // +100% / 0 / -100% axis ticks
        context.DrawText(new FormattedText("+100", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.5, mutedBrush), new Point(2, botY - 4));
        context.DrawText(new FormattedText("0", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.5, mutedBrush), new Point(16, midY - 5));
        context.DrawText(new FormattedText("-100", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 8.5, mutedBrush), new Point(2, botY + botH - 8));

        int[]? scores = CodeScoresPct;
        int winBase = WinningCode5Bit >= 0 ? (WinningCode5Bit & 0x0F) : -1;
        double slotW = plotW / 16.0;
        double barW = Math.Max(4.0, slotW * 0.65);

        for (int c = 0; c < 16; c++)
        {
            double cx = padL + c * slotW + (slotW - barW) * 0.5;
            int val = (scores is not null && c < scores.Length) ? Math.Clamp(scores[c], -100, 100) : 0;
            double barH = Math.Abs(val) / 100.0 * (botH * 0.46);
            double by = val >= 0 ? midY - barH : midY;

            IBrush brush = c == winBase
                ? (val >= 0 ? barWinPosBrush : barWinNegBrush)
                : barIdleBrush;

            if (barH >= 1.0)
            {
                context.FillRectangle(brush, new Rect(cx, by, barW, barH));
            }

            var cLbl = new FormattedText(c.ToString("X", CultureInfo.InvariantCulture), CultureInfo.InvariantCulture, FlowDirection.LeftToRight, typeface, 9.0, c == winBase ? okBrush : mutedBrush);
            context.DrawText(cLbl, new Point(padL + c * slotW + (slotW - cLbl.Width) * 0.5, botY + botH + 2));
        }
    }
}
