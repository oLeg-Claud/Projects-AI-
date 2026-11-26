using Dock.Model.Mvvm.Controls;
using ProvodkaNG.Desktop.ViewModels;

namespace ProvodkaNG.Desktop.Docking;

public sealed class UsoControlTool : Tool
{
    public MainWindowViewModel Main { get; }

    public UsoControlTool(MainWindowViewModel main)
    {
        Main = main;
        Id = "UsoControlTool";
        Title = "УСО32 и ЦОС";
        CanClose = false;
    }
}

public sealed class OscilloscopeDocument : Document
{
    public MainWindowViewModel Main { get; }

    public OscilloscopeDocument(MainWindowViewModel main)
    {
        Main = main;
        Id = "OscilloscopeDoc";
        Title = "Осциллограф";
        CanClose = false;
    }
}

public sealed class SpectrumDocument : Document
{
    public MainWindowViewModel Main { get; }

    public SpectrumDocument(MainWindowViewModel main)
    {
        Main = main;
        Id = "SpectrumDoc";
        Title = "Спектр (БПФ)";
        CanClose = false;
    }
}

public sealed class CorrelationDocument : Document
{
    public MainWindowViewModel Main { get; }

    public CorrelationDocument(MainWindowViewModel main)
    {
        Main = main;
        Id = "CorrelationDoc";
        Title = "Корреляция RM(1,4)";
        CanClose = false;
    }
}

public sealed class InclinometryTool : Tool
{
    public MainWindowViewModel Main { get; }

    public InclinometryTool(MainWindowViewModel main)
    {
        Main = main;
        Id = "InclinometryTool";
        Title = "Инклинометрия и НДМ";
        CanClose = false;
    }
}

public sealed class VectorsTool : Tool
{
    public MainWindowViewModel Main { get; }

    public VectorsTool(MainWindowViewModel main)
    {
        Main = main;
        Id = "VectorsTool";
        Title = "Векторы RM(1,4)";
        CanClose = false;
    }
}

public sealed class ToolfaceTool : Tool
{
    public MainWindowViewModel Main { get; }

    public ToolfaceTool(MainWindowViewModel main)
    {
        Main = main;
        Id = "ToolfaceTool";
        Title = "Отклонитель (OTK)";
        CanClose = false;
    }
}
