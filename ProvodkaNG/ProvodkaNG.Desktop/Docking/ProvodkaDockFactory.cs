using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using ProvodkaNG.Desktop.ViewModels;

namespace ProvodkaNG.Desktop.Docking;

/// <summary>
/// Avalonia Dock Manager layout factory (Dock.Model.Mvvm 12.2.0).
/// Builds a multi-pane industrial telemetry workspace:
/// Left: USO32 & DSP Settings
/// Center-Top: Live Oscilloscope
/// Center-Mid: Spectrum (FFT) | Correlation (RM(1,4) + Sync)
/// Center-Bottom: Inclinometry/NDM & 5-bit Vector Strip
/// Right: Zero-Latency 360° Toolface Dial (detachable to driller's 2nd monitor)
/// </summary>
public sealed class ProvodkaDockFactory : Factory
{
    private readonly MainWindowViewModel _main;
    private readonly Dictionary<string, IDockable> _panels = new(StringComparer.Ordinal);

    public ProvodkaDockFactory(MainWindowViewModel main)
    {
        _main = main;
    }

    public override IRootDock CreateLayout()
    {
        _panels.Clear();

        var usoTool = new UsoControlTool(_main);
        var oscDoc = new OscilloscopeDocument(_main);
        var specDoc = new SpectrumDocument(_main);
        var corrDoc = new CorrelationDocument(_main);
        var inclTool = new InclinometryTool(_main);
        var vecTool = new VectorsTool(_main);
        var tfTool = new ToolfaceTool(_main);

        Register(usoTool);
        Register(oscDoc);
        Register(specDoc);
        Register(corrDoc);
        Register(inclTool);
        Register(vecTool);
        Register(tfTool);

        var leftDock = new ToolDock
        {
            Id = "LeftToolDock",
            Proportion = 0.21,
            Alignment = Alignment.Left,
            IsCollapsable = false,
            ActiveDockable = usoTool,
            VisibleDockables = CreateList<IDockable>(usoTool)
        };

        var oscDock = new DocumentDock
        {
            Id = "OscilloscopeDock",
            Proportion = 0.36,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = oscDoc,
            VisibleDockables = CreateList<IDockable>(oscDoc)
        };

        var specDock = new DocumentDock
        {
            Id = "SpectrumDock",
            Proportion = 0.50,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = specDoc,
            VisibleDockables = CreateList<IDockable>(specDoc)
        };

        var corrDock = new DocumentDock
        {
            Id = "CorrelationDock",
            Proportion = 0.50,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = corrDoc,
            VisibleDockables = CreateList<IDockable>(corrDoc)
        };

        var analyticsSplit = new ProportionalDock
        {
            Id = "AnalyticsSplitDock",
            Proportion = 0.34,
            Orientation = Orientation.Horizontal,
            IsCollapsable = false,
            VisibleDockables = CreateList<IDockable>(
                specDock,
                new ProportionalDockSplitter(),
                corrDock)
        };

        var bottomDock = new ToolDock
        {
            Id = "BottomToolDock",
            Proportion = 0.30,
            Alignment = Alignment.Bottom,
            IsCollapsable = false,
            ActiveDockable = inclTool,
            VisibleDockables = CreateList<IDockable>(inclTool, vecTool)
        };

        var centerDock = new ProportionalDock
        {
            Id = "CenterDock",
            Proportion = 0.56,
            Orientation = Orientation.Vertical,
            IsCollapsable = false,
            VisibleDockables = CreateList<IDockable>(
                oscDock,
                new ProportionalDockSplitter(),
                analyticsSplit,
                new ProportionalDockSplitter(),
                bottomDock)
        };

        var rightDock = new ToolDock
        {
            Id = "RightToolDock",
            Proportion = 0.23,
            Alignment = Alignment.Right,
            IsCollapsable = false,
            ActiveDockable = tfTool,
            VisibleDockables = CreateList<IDockable>(tfTool)
        };

        var mainSplit = new ProportionalDock
        {
            Id = "MainSplitDock",
            Orientation = Orientation.Horizontal,
            IsCollapsable = false,
            VisibleDockables = CreateList<IDockable>(
                leftDock,
                new ProportionalDockSplitter(),
                centerDock,
                new ProportionalDockSplitter(),
                rightDock)
        };

        var rootDock = CreateRootDock();
        rootDock.Id = "RootDock";
        rootDock.IsCollapsable = false;
        rootDock.ActiveDockable = mainSplit;
        rootDock.DefaultDockable = mainSplit;
        rootDock.VisibleDockables = CreateList<IDockable>(mainSplit);

        return rootDock;
    }

    public bool TryActivatePanel(string panelId)
    {
        if (!_panels.TryGetValue(panelId, out IDockable? dockable))
            return false;

        SetActiveDockable(dockable);
        if (dockable.Owner is IDock owner)
        {
            owner.ActiveDockable = dockable;
        }
        return true;
    }

    private void Register(IDockable dockable)
    {
        if (!string.IsNullOrEmpty(dockable.Id))
            _panels[dockable.Id] = dockable;
    }
}
