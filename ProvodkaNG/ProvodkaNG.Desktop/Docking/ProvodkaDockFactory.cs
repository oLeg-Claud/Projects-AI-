using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using ProvodkaNG.Desktop.Services;
using ProvodkaNG.Desktop.ViewModels;

namespace ProvodkaNG.Desktop.Docking;

/// <summary>
/// Avalonia Dock Manager layout factory (Dock.Model.Mvvm 12.2.0) with JSON layout capture & restore.
/// Default industrial telemetry workspace:
/// Left: USO32 & DSP Settings
/// Center-Top: Live Oscilloscope
/// Center-Mid: Spectrum (0..65 Hz) | Sync Preamble Correlation (СП) | RM(1,4) Correlation
/// Center-Bottom: Inclinometry/NDM & Wrapping 5-bit Vector Strip
/// Right: Zero-Latency 360° Toolface Dial
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
        Dictionary<string, IDockable> pool = CreateAllPanels();

        var leftDock = new ToolDock
        {
            Id = "LeftToolDock",
            Proportion = 0.21,
            Alignment = Alignment.Left,
            IsCollapsable = false,
            ActiveDockable = pool["UsoControlTool"],
            VisibleDockables = CreateList<IDockable>(pool["UsoControlTool"])
        };

        var oscDock = new DocumentDock
        {
            Id = "OscilloscopeDock",
            Proportion = 0.36,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = pool["OscilloscopeDoc"],
            VisibleDockables = CreateList<IDockable>(pool["OscilloscopeDoc"])
        };

        var specDock = new DocumentDock
        {
            Id = "SpectrumDock",
            Proportion = 0.36,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = pool["SpectrumDoc"],
            VisibleDockables = CreateList<IDockable>(pool["SpectrumDoc"])
        };

        var syncCorrDock = new DocumentDock
        {
            Id = "SyncCorrelationDock",
            Proportion = 0.32,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = pool["SyncCorrelationDoc"],
            VisibleDockables = CreateList<IDockable>(pool["SyncCorrelationDoc"])
        };

        var corrDock = new DocumentDock
        {
            Id = "CorrelationDock",
            Proportion = 0.32,
            IsCollapsable = false,
            CanCreateDocument = false,
            ActiveDockable = pool["CorrelationDoc"],
            VisibleDockables = CreateList<IDockable>(pool["CorrelationDoc"])
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
                syncCorrDock,
                new ProportionalDockSplitter(),
                corrDock)
        };

        var bottomDock = new ToolDock
        {
            Id = "BottomToolDock",
            Proportion = 0.30,
            Alignment = Alignment.Bottom,
            IsCollapsable = false,
            ActiveDockable = pool["InclinometryTool"],
            VisibleDockables = CreateList<IDockable>(pool["InclinometryTool"], pool["VectorsTool"])
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
            ActiveDockable = pool["ToolfaceTool"],
            VisibleDockables = CreateList<IDockable>(pool["ToolfaceTool"])
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

        return WrapRoot(mainSplit);
    }

    /// <summary>
    /// Captures the live Dock Manager tree (splits, proportions, tab groupings, active tabs) into a JSON-safe DTO.
    /// </summary>
    public DockNodeDto? CaptureLayout(IRootDock? root)
    {
        if (root?.VisibleDockables is null || root.VisibleDockables.Count == 0)
            return null;

        IDockable? primary = root.ActiveDockable ?? root.VisibleDockables[0];
        return primary is not null ? CaptureNode(primary) : null;
    }

    /// <summary>
    /// Restores a previously saved Dock layout tree. Any newly added panels missing from older saved files
    /// are attached automatically so no panel can ever disappear across upgrades.
    /// </summary>
    public IRootDock? RestoreLayout(DockNodeDto? savedTree)
    {
        if (savedTree is null)
            return null;

        try
        {
            Dictionary<string, IDockable> pool = CreateAllPanels();
            var usedIds = new HashSet<string>(StringComparer.Ordinal);
            DocumentDock? firstDocDock = null;
            ToolDock? firstToolDock = null;

            IDockable? built = BuildNode(savedTree, pool, usedIds, ref firstDocDock, ref firstToolDock);
            if (built is null || usedIds.Count == 0)
                return null;

            // Ensure any panel missing from an older layout file (e.g. newly added SyncCorrelationDoc) is attached cleanly.
            foreach ((string id, IDockable panel) in pool)
            {
                if (usedIds.Contains(id))
                    continue;

                if (panel is Document && firstDocDock is not null)
                {
                    firstDocDock.VisibleDockables ??= CreateList<IDockable>();
                    firstDocDock.VisibleDockables.Add(panel);
                }
                else if (panel is Tool && firstToolDock is not null)
                {
                    firstToolDock.VisibleDockables ??= CreateList<IDockable>();
                    firstToolDock.VisibleDockables.Add(panel);
                }
                else if (firstDocDock is not null)
                {
                    firstDocDock.VisibleDockables ??= CreateList<IDockable>();
                    firstDocDock.VisibleDockables.Add(panel);
                }
                else if (firstToolDock is not null)
                {
                    firstToolDock.VisibleDockables ??= CreateList<IDockable>();
                    firstToolDock.VisibleDockables.Add(panel);
                }
            }

            return WrapRoot(built);
        }
        catch
        {
            return null;
        }
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

    private Dictionary<string, IDockable> CreateAllPanels()
    {
        _panels.Clear();
        IDockable[] all =
        [
            new UsoControlTool(_main),
            new OscilloscopeDocument(_main),
            new SpectrumDocument(_main),
            new SyncCorrelationDocument(_main),
            new CorrelationDocument(_main),
            new InclinometryTool(_main),
            new VectorsTool(_main),
            new ToolfaceTool(_main)
        ];

        foreach (IDockable p in all)
        {
            if (!string.IsNullOrEmpty(p.Id))
                _panels[p.Id] = p;
        }

        return new Dictionary<string, IDockable>(_panels, StringComparer.Ordinal);
    }

    private IRootDock WrapRoot(IDockable mainContent)
    {
        var rootDock = CreateRootDock();
        rootDock.Id = "RootDock";
        rootDock.IsCollapsable = false;
        rootDock.ActiveDockable = mainContent;
        rootDock.DefaultDockable = mainContent;
        rootDock.VisibleDockables = CreateList<IDockable>(mainContent);
        return rootDock;
    }

    private static DockNodeDto? CaptureNode(IDockable dockable)
    {
        if (dockable is IProportionalDockSplitter)
            return null;

        if (dockable is IProportionalDock prop)
        {
            var children = new List<DockNodeDto>();
            if (prop.VisibleDockables is not null)
            {
                foreach (IDockable child in prop.VisibleDockables)
                {
                    DockNodeDto? dto = CaptureNode(child);
                    if (dto is not null)
                        children.Add(dto);
                }
            }
            if (children.Count == 0)
                return null;

            return new DockNodeDto
            {
                Kind = "Proportional",
                Id = prop.Id ?? string.Empty,
                Proportion = SanitizeProportion(prop.Proportion),
                Orientation = prop.Orientation == Orientation.Vertical ? "Vertical" : "Horizontal",
                Children = children
            };
        }

        if (dockable is IToolDock toolDock)
        {
            var children = CaptureLeafPanels(toolDock);
            if (children.Count == 0)
                return null;

            return new DockNodeDto
            {
                Kind = "ToolDock",
                Id = toolDock.Id ?? string.Empty,
                Proportion = SanitizeProportion(toolDock.Proportion),
                Alignment = toolDock.Alignment.ToString(),
                ActiveId = toolDock.ActiveDockable?.Id,
                Children = children
            };
        }

        if (dockable is IDocumentDock docDock)
        {
            var children = CaptureLeafPanels(docDock);
            if (children.Count == 0)
                return null;

            return new DockNodeDto
            {
                Kind = "DocumentDock",
                Id = docDock.Id ?? string.Empty,
                Proportion = SanitizeProportion(docDock.Proportion),
                ActiveId = docDock.ActiveDockable?.Id,
                Children = children
            };
        }

        if (!string.IsNullOrEmpty(dockable.Id))
        {
            return new DockNodeDto
            {
                Kind = "Panel",
                Id = dockable.Id
            };
        }

        return null;
    }

    private static List<DockNodeDto> CaptureLeafPanels(IDock dock)
    {
        var list = new List<DockNodeDto>();
        if (dock.VisibleDockables is null)
            return list;

        foreach (IDockable child in dock.VisibleDockables)
        {
            if (child is IProportionalDockSplitter)
                continue;
            if (!string.IsNullOrEmpty(child.Id))
            {
                list.Add(new DockNodeDto { Kind = "Panel", Id = child.Id });
            }
        }
        return list;
    }

    private IDockable? BuildNode(
        DockNodeDto dto,
        Dictionary<string, IDockable> pool,
        HashSet<string> usedIds,
        ref DocumentDock? firstDocDock,
        ref ToolDock? firstToolDock)
    {
        switch (dto.Kind)
        {
            case "Proportional":
            {
                if (dto.Children is null || dto.Children.Count == 0)
                    return null;

                var builtChildren = new List<IDockable>();
                foreach (DockNodeDto childDto in dto.Children)
                {
                    IDockable? child = BuildNode(childDto, pool, usedIds, ref firstDocDock, ref firstToolDock);
                    if (child is not null)
                        builtChildren.Add(child);
                }
                if (builtChildren.Count == 0)
                    return null;

                var withSplitters = CreateList<IDockable>();
                for (int i = 0; i < builtChildren.Count; i++)
                {
                    if (i > 0)
                        withSplitters.Add(new ProportionalDockSplitter());
                    withSplitters.Add(builtChildren[i]);
                }

                var prop = new ProportionalDock
                {
                    Id = string.IsNullOrWhiteSpace(dto.Id) ? "SplitDock" : dto.Id,
                    Orientation = string.Equals(dto.Orientation, "Vertical", StringComparison.OrdinalIgnoreCase)
                        ? Orientation.Vertical
                        : Orientation.Horizontal,
                    IsCollapsable = false,
                    VisibleDockables = withSplitters
                };
                if (IsValidProportion(dto.Proportion))
                    prop.Proportion = dto.Proportion;
                return prop;
            }

            case "ToolDock":
            {
                var items = ResolveLeafPanels(dto.Children, pool, usedIds);
                if (items.Count == 0)
                    return null;

                Alignment align = Enum.TryParse(dto.Alignment, ignoreCase: true, out Alignment parsedAlign)
                    ? parsedAlign
                    : Alignment.Unset;

                var toolDock = new ToolDock
                {
                    Id = string.IsNullOrWhiteSpace(dto.Id) ? "ToolDock" : dto.Id,
                    Alignment = align,
                    IsCollapsable = false,
                    VisibleDockables = CreateList<IDockable>(items.ToArray()),
                    ActiveDockable = PickActive(items, dto.ActiveId)
                };
                if (IsValidProportion(dto.Proportion))
                    toolDock.Proportion = dto.Proportion;
                firstToolDock ??= toolDock;
                return toolDock;
            }

            case "DocumentDock":
            {
                var items = ResolveLeafPanels(dto.Children, pool, usedIds);
                if (items.Count == 0)
                    return null;

                var docDock = new DocumentDock
                {
                    Id = string.IsNullOrWhiteSpace(dto.Id) ? "DocumentDock" : dto.Id,
                    IsCollapsable = false,
                    CanCreateDocument = false,
                    VisibleDockables = CreateList<IDockable>(items.ToArray()),
                    ActiveDockable = PickActive(items, dto.ActiveId)
                };
                if (IsValidProportion(dto.Proportion))
                    docDock.Proportion = dto.Proportion;
                firstDocDock ??= docDock;
                return docDock;
            }

            default:
                return null;
        }
    }

    private static List<IDockable> ResolveLeafPanels(
        List<DockNodeDto>? children,
        Dictionary<string, IDockable> pool,
        HashSet<string> usedIds)
    {
        var items = new List<IDockable>();
        if (children is null)
            return items;

        foreach (DockNodeDto child in children)
        {
            if (!string.IsNullOrEmpty(child.Id) &&
                !usedIds.Contains(child.Id) &&
                pool.TryGetValue(child.Id, out IDockable? panel))
            {
                usedIds.Add(child.Id);
                items.Add(panel);
            }
        }
        return items;
    }

    private static IDockable PickActive(List<IDockable> items, string? activeId)
    {
        if (!string.IsNullOrEmpty(activeId))
        {
            foreach (IDockable item in items)
            {
                if (string.Equals(item.Id, activeId, StringComparison.Ordinal))
                    return item;
            }
        }
        return items[0];
    }

    private static double SanitizeProportion(double value)
        => IsValidProportion(value) ? value : double.NaN;

    private static bool IsValidProportion(double value)
        => double.IsFinite(value) && value > 0.01 && value <= 1.0;
}
