using Microsoft.Win32;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

public static class InstalledAudioDriver
{
    public static string? SpeakerEndpointId()
    {
        try {
            using var render = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render");
            if(render is null) return null;
            var matches = new List<string>();
            foreach(string guid in render.GetSubKeyNames()) {
                using var endpoint = render.OpenSubKey(guid);
                if(endpoint?.GetValue("DeviceState") is not int state || state != 1) continue;
                using var properties = endpoint.OpenSubKey("Properties");
                string? node = properties?.GetValue("{b3f8fa53-0004-438e-9003-51a46e139bfc},2") as string;
                if(node?.Contains(@"USB\VID_0B05&PID_189D&MI_00\", StringComparison.OrdinalIgnoreCase) == true &&
                   properties?.GetValue("{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0") is int form && form == 1)
                    matches.Add("{0.0.0.00000000}." + guid);
            }
            return matches.Count == 1 ? matches[0] : null;
        } catch { return null; }
    }
    public static bool HasStreamApo()
    {
        try {
            using var render = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render");
            if(render is null) return false;
            int matches = 0;
            foreach(string guid in render.GetSubKeyNames()) {
                using var endpoint = render.OpenSubKey(guid);
                if(endpoint?.GetValue("DeviceState") is not int state || state != 1) continue;
                using var properties = endpoint.OpenSubKey("Properties");
                string? node = properties?.GetValue("{b3f8fa53-0004-438e-9003-51a46e139bfc},2") as string;
                if(node?.Contains(@"USB\VID_0B05&PID_189D&MI_00\", StringComparison.OrdinalIgnoreCase) != true ||
                    properties?.GetValue("{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0") is not int form || form != 1) continue;
                using var fx = endpoint.OpenSubKey("FxProperties");
                if(string.Equals(fx?.GetValue("{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1") as string, "{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}", StringComparison.OrdinalIgnoreCase) &&
                   string.Equals(fx?.GetValue("{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2") as string, "{D2FBC737-00C3-4482-B3DD-02D92C06003E}", StringComparison.OrdinalIgnoreCase)) ++matches;
            }
            return matches == 1;
        } catch { return false; }
    }

    public static bool HasOutputApo()
    {
        try {
            string? id = SpeakerEndpointId();
            if(id is null) return false;
            string guid = id[(id.LastIndexOf('.') + 1)..];
            using var fx = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\" + guid + @"\FxProperties");
            return string.Equals(fx?.GetValue("{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2") as string,
                "{D2FBC737-00C3-4482-B3DD-02D92C06003E}", StringComparison.OrdinalIgnoreCase);
        } catch { return false; }
    }

    // Lire le devnode actif, pas Win32_PnPSignedDriver dont le cache peut être périmé.
    public static DriverInfo Read()
    {
        try {
            using var usb = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\USB\VID_0B05&PID_189D&MI_00");
            if(usb is null) return new DriverInfo { Kind = DriverKind.Unknown, Version = "non détecté" };
            var found = new List<DriverInfo>();
            foreach(string instance in usb.GetSubKeyNames()) {
                using var device = usb.OpenSubKey(instance);
                if(device?.GetValue("Driver") is not string driver) continue;
                using var cls = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\" + driver);
                if(cls?.GetValue("DriverVersion") is not string version) continue;
                var kind = device.GetValue("Service") as string;
                found.Add(new DriverInfo { Version = version, Kind = kind switch { "xonarse" => DriverKind.Xonarse, "usbaudio2" => DriverKind.Usbaudio2, _ => DriverKind.Unknown } });
            }
            return found.Count == 1 ? found[0] : new DriverInfo { Kind = DriverKind.Unknown, Version = "non déterminé" };
        } catch { return new DriverInfo { Kind = DriverKind.Unknown, Version = "non détecté" }; }
    }
}
