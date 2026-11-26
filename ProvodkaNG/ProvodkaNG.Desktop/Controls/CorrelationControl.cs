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
        var syncPen = new Pen(new SolidColorBrush(Color.Parse("#10B981")), 1.8);
        var typeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);

        context.FillRectangle(bg, new Rect(0, 0, w, h));

        double padL = 32;
        double padR = 10;
        double plotW = Math.Max(10, w - padL - padR);

        // --- TOP ZONE (35%): Sync Preamble Correlation History ---
        double topY = 22;
        double topH = Math.Max(24, (h - 46) * 0.36);

        var spHeader = new FormattedText(
            $"СП корреляция: {LatestSyncCorrPct}%   Порог ДСП: {DcpThreshold}%   Дрейф PIK5: {LastPikDrift:+#;-#;0}",
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            typeface,
            10.5,
            okBrush);
        context.DrawText(spHeader, new Point(padL, 4));

        context.DrawRectangle(null, gridPen, new Rect(padL, topY, plotW, topH));
        double thrY = topY + (1.0 - Math.Clamp(DcpThreshold, 0, 100) / 100.0) * topH;
        context.DrawLine(threshPen, new Point(padL, thrY), new Point(padL + plotW, thrY));

        if (SyncCorrHistory is { Count: > 0 } hist)
        {
            var geo = new StreamGeometry();
            using (var ctx = geo.Open())
            {
                int count = hist.Count;
                for (int i = 0; i < count; i++)
                {
                    double x = count == 1 ? padL + plotW * 0.5 : padL + (double)i / (count - 1) * plotW;
                    double y = topY + (1.0 - Math.Clamp(hist[i], 0, 100) / 100.0) * topH;
                    if (i == 0) ctx.BeginFigure(new Point(x, y), false);
                    else ctx.LineTo(new Point(x, y));
                }
            }
            context.DrawGeometry(null, syncPen, geo);
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
