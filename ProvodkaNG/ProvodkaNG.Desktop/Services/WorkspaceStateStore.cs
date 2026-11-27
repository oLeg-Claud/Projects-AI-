using System.Text.Json;
using System.Text.Json.Serialization;

namespace ProvodkaNG.Desktop.Services;

public sealed class WorkspaceStateDto
{
    public int Version { get; set; } = 1;
    public WindowPlacementDto? Window { get; set; }
    public AppSettingsDto? Settings { get; set; }
    public DockNodeDto? DockLayout { get; set; }
    public List<DockFloatingWindowDto>? FloatingWindows { get; set; }
}

public sealed class WindowPlacementDto
{
    public int? Left { get; set; }
    public int? Top { get; set; }
    public double Width { get; set; } = 1500;
    public double Height { get; set; } = 920;
    public bool IsMaximized { get; set; }
}

public sealed class AppSettingsDto
{
    public bool IsDarkTheme { get; set; } = true;
    public int FormatId { get; set; } = 1;
    public int GainIndex { get; set; } = 2;
    public double CarrierFreqHz { get; set; } = 5.0;
    public int Ncod { get; set; } = 0;
    public int DcpThreshold { get; set; } = 55;
    public bool CorrectorEnabled { get; set; } = true;
    public bool EnableStepTrack { get; set; } = true;
    public int PlaybackSpeedMultiplier { get; set; } = 1;
    public int RateModeIndex { get; set; } = 0;
    public int AdcFilterCode { get; set; } = 3;
    public int McuLpfModeCode { get; set; } = 1;
    public int InputMuxCode { get; set; } = 0x89;
}

public sealed class DockNodeDto
{
    public string Kind { get; set; } = string.Empty; // "Proportional", "ToolDock", "DocumentDock", "Panel"
    public string Id { get; set; } = string.Empty;
    public double Proportion { get; set; } = double.NaN;
    public string? Orientation { get; set; } // "Horizontal" / "Vertical"
    public string? Alignment { get; set; }   // "Left" / "Right" / "Top" / "Bottom" / "Unset"
    public string? ActiveId { get; set; }
    public List<DockNodeDto>? Children { get; set; }
}

public sealed class DockFloatingWindowDto
{
    public double X { get; set; }
    public double Y { get; set; }
    public double Width { get; set; } = 420;
    public double Height { get; set; } = 360;
    public DockNodeDto? Content { get; set; }
}

/// <summary>
/// Atomic JSON persistence for MainWindow placement, Dock Manager layout, and user DSP/USO settings.
/// Stored under %LocalAppData%/ProvodkaNG/workspace_state.json (or XDG equivalent on Linux/macOS).
/// </summary>
public static class WorkspaceStateStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    public static string DefaultFilePath
    {
        get
        {
            string root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrWhiteSpace(root))
                root = AppContext.BaseDirectory;
            return Path.Combine(root, "ProvodkaNG", "workspace_state.json");
        }
    }

    public static WorkspaceStateDto? Load(string? filePath = null)
    {
        string path = string.IsNullOrWhiteSpace(filePath) ? DefaultFilePath : filePath;
        try
        {
            if (!File.Exists(path))
                return null;

            string json = File.ReadAllText(path);
            if (string.IsNullOrWhiteSpace(json))
                return null;

            return JsonSerializer.Deserialize<WorkspaceStateDto>(json, JsonOptions);
        }
        catch
        {
            return null;
        }
    }

    public static void Save(WorkspaceStateDto state, string? filePath = null)
    {
        ArgumentNullException.ThrowIfNull(state);
        string path = string.IsNullOrWhiteSpace(filePath) ? DefaultFilePath : filePath;
        try
        {
            string? dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(dir))
                Directory.CreateDirectory(dir);

            string json = JsonSerializer.Serialize(state, JsonOptions);
            string tmpPath = path + ".tmp";
            File.WriteAllText(tmpPath, json);
            File.Move(tmpPath, path, overwrite: true);
        }
        catch
        {
            // Never crash application shutdown on disk/permission errors.
        }
    }
}
