using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using ProvodkaNG.Core.Native;

namespace ProvodkaNG.Desktop.Controls;

/// <summary>
/// High-DPI Zero-Latency 360° Toolface (OTK) dial rendered directly via Avalonia DrawingContext.
/// Displays current angle needle, last 6 history dots, reliability arc, mode label, and DSP latency.
/// </summary>
public sealed class ToolfaceDialControl : Control
{
    public static readonly StyledProperty<double> AngleDegProperty =
        AvaloniaProperty.Register<ToolfaceDialControl, double>(nameof(AngleDeg), 0.0);

    public static readonly StyledProperty<int> ReliabilityPctProperty =
        AvaloniaProperty.Register<ToolfaceDialControl, int>(nameof(ReliabilityPct), 0);

    public static readonly StyledProperty<double> LatencyMsProperty =
        AvaloniaProperty.Register<ToolfaceDialControl, double>(nameof(LatencyMs), 0.0);

    public static readonly StyledProperty<DspToolfaceChannel> ChannelProperty =
        AvaloniaProperty.Register<ToolfaceDialControl, DspToolfaceChannel>(nameof(Channel), DspToolfaceChannel.Gravity);

    public static readonly StyledProperty<IReadOnlyList<double>?> HistoryAnglesProperty =
        AvaloniaProperty.Register<ToolfaceDialControl, IReadOnlyList<double>?>(nameof(HistoryAngles));

    public static readonly StyledProperty<bool> IsDarkThemeProperty =
        AvaloniaProperty.Register<ToolfaceDialControl, bool>(nameof(IsDarkTheme), true);

    static ToolfaceDialControl()
    {
        AffectsRender<ToolfaceDialControl>(
            AngleDegProperty,
            ReliabilityPctProperty,
            LatencyMsProperty,
            ChannelProperty,
            HistoryAnglesProperty,
            IsDarkThemeProperty);
    }

    public double AngleDeg
    {
        get => GetValue(AngleDegProperty);
        set => SetValue(AngleDegProperty, value);
    }

    public int ReliabilityPct
    {
        get => GetValue(ReliabilityPctProperty);
        set => SetValue(ReliabilityPctProperty, value);
    }

    public double LatencyMs
    {
        get => GetValue(LatencyMsProperty);
        set => SetValue(LatencyMsProperty, value);
    }

    public DspToolfaceChannel Channel
    {
        get => GetValue(ChannelProperty);
        set => SetValue(ChannelProperty, value);
    }

    public IReadOnlyList<double>? HistoryAngles
    {
        get => GetValue(HistoryAnglesProperty);
        set => SetValue(HistoryAnglesProperty, value);
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
        double size = Math.Min(w, h);
        if (size < 60)
            return;

        Point center = new(w * 0.5, h * 0.5);
        double outerRadius = (size * 0.5) - 12.0;
        double innerRadius = outerRadius * 0.62;

        bool dark = IsDarkTheme;
        var faceBg = new SolidColorBrush(dark ? Color.Parse("#0F151D") : Color.Parse("#FFFFFF"));
        var ringColor = dark ? Color.Parse("#2B384A") : Color.Parse("#94A3B8");
        var tickMajorColor = dark ? Color.Parse("#CBD5E1") : Color.Parse("#334155");
        var tickMinorColor = dark ? Color.Parse("#475569") : Color.Parse("#94A3B8");
        var textPrimary = new SolidColorBrush(dark ? Color.Parse("#F8FAFC") : Color.Parse("#0F172A"));
        var textMuted = new SolidColorBrush(dark ? Color.Parse("#94A3B8") : Color.Parse("#475569"));

        Color accentColor = Channel switch
        {
            DspToolfaceChannel.Magnetic => Color.Parse("#22D3EE"),
            DspToolfaceChannel.Static => Color.Parse("#10B981"),
            _ => Color.Parse("#F59E0B")
        };

        var accentBrush = new SolidColorBrush(accentColor);
        var ringPen = new Pen(new SolidColorBrush(ringColor), 2.0);
        var majorPen = new Pen(new SolidColorBrush(tickMajorColor), 2.0);
        var minorPen = new Pen(new SolidColorBrush(tickMinorColor), 1.0);
        var crossPen = new Pen(new SolidColorBrush(ringColor), 1.0, DashStyle.Dash);

        // Dial background & outer ring
        context.DrawEllipse(faceBg, ringPen, center, outerRadius, outerRadius);
        context.DrawEllipse(null, new Pen(new SolidColorBrush(ringColor), 1.0), center, innerRadius, innerRadius);

        // Crosshairs
        context.DrawLine(crossPen, new Point(center.X - outerRadius, center.Y), new Point(center.X + outerRadius, center.Y));
        context.DrawLine(crossPen, new Point(center.X, center.Y - outerRadius), new Point(center.X, center.Y + outerRadius));

        // 360° compass ticks (every 10°, major every 30°)
        var labelTypeface = new Typeface("Consolas", FontStyle.Normal, FontWeight.SemiBold);
        for (int deg = 0; deg < 360; deg += 10)
        {
            double rad = (deg - 90.0) * Math.PI / 180.0;
            bool isMajor = deg % 30 == 0;
            double r1 = outerRadius;
            double r2 = outerRadius - (isMajor ? 11.0 : 6.0);

            Point p1 = new(center.X + Math.Cos(rad) * r1, center.Y + Math.Sin(rad) * r1);
            Point p2 = new(center.X + Math.Cos(rad) * r2, center.Y + Math.Sin(rad) * r2);
            context.DrawLine(isMajor ? majorPen : minorPen, p1, p2);

            if (deg % 90 == 0)
            {
                string cardinal = deg switch
                {
                    0 => Channel == DspToolfaceChannel.Magnetic ? "N" : "0°",
                    90 => "90°",
                    180 => "180°",
                    270 => "270°",
                    _ => deg.ToString(CultureInfo.InvariantCulture)
                };

                var ft = new FormattedText(
                    cardinal,
                    CultureInfo.InvariantCulture,
                    FlowDirection.LeftToRight,
                    labelTypeface,
                    11.0,
                    deg == 0 ? accentBrush : textMuted);

                double lr = outerRadius - 22.0;
                Point lp = new(
                    center.X + Math.Cos(rad) * lr - ft.Width * 0.5,
                    center.Y + Math.Sin(rad) * lr - ft.Height * 0.5);
                context.DrawText(ft, lp);
            }
        }

        // History trail dots (oldest -> newest)
        if (HistoryAngles is { Count: > 0 } trail)
        {
            int count = trail.Count;
            for (int i = 0; i < count; i++)
            {
                double rad = (trail[i] - 90.0) * Math.PI / 180.0;
                double t = (double)(i + 1) / (count + 1);
                double dotR = outerRadius * (0.72 + 0.14 * t);
                double dotSize = 2.5 + 3.0 * t;
                byte alpha = (byte)(60 + 170 * t);

                var dotBrush = new SolidColorBrush(Color.FromArgb(alpha, accentColor.R, accentColor.G, accentColor.B));
                Point dp = new(center.X + Math.Cos(rad) * dotR, center.Y + Math.Sin(rad) * dotR);
                context.DrawEllipse(dotBrush, null, dp, dotSize, dotSize);
            }
        }

        // Primary Toolface Needle
        double angleRad = (AngleDeg - 90.0) * Math.PI / 180.0;
        Point tip = new(
            center.X + Math.Cos(angleRad) * (outerRadius - 6.0),
            center.Y + Math.Sin(angleRad) * (outerRadius - 6.0));
        Point innerBase = new(
            center.X + Math.Cos(angleRad) * (innerRadius * 0.85),
            center.Y + Math.Sin(angleRad) * (innerRadius * 0.85));

        context.DrawLine(new Pen(accentBrush, 4.0, lineCap: PenLineCap.Round), innerBase, tip);
        context.DrawEllipse(accentBrush, new Pen(faceBg, 2.0), tip, 5.5, 5.5);

        // Center digital readout
        string modeLabel = Channel switch
        {
            DspToolfaceChannel.Magnetic => "MTF (МАГН.)",
            DspToolfaceChannel.Static => "OTKu (СТАТ.)",
            _ => "GTF (ГРАВ.)"
        };

        var modeText = new FormattedText(
            modeLabel,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            labelTypeface,
            11.0,
            accentBrush);
        context.DrawText(modeText, new Point(center.X - modeText.Width * 0.5, center.Y - 34.0));

        var angleText = new FormattedText(
            $"{AngleDeg:0.0}°",
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            new Typeface("Consolas", FontStyle.Normal, FontWeight.Bold),
            Math.Clamp(size * 0.115, 20.0, 34.0),
            textPrimary);
        context.DrawText(angleText, new Point(center.X - angleText.Width * 0.5, center.Y - angleText.Height * 0.5));

        string metaStr = $"ДСП {ReliabilityPct}%  •  {LatencyMs:0.00} мс";
        var metaText = new FormattedText(
            metaStr,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            labelTypeface,
            10.5,
            textMuted);
        context.DrawText(metaText, new Point(center.X - metaText.Width * 0.5, center.Y + 22.0));
    }
}
