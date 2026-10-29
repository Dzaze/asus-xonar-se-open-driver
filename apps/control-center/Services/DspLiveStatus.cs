using System.Text.Json;
using System.Text.Json.Serialization;

namespace Xonar.ControlCenter.Services;

/// <summary>Mesures publiées hors callback par l'APO réellement chargé dans audiodg.</summary>
public sealed class DspLiveStatus
{
    public sealed class Source {
        [JsonPropertyName("instance")] public int Instance { get; set; }
        [JsonPropertyName("inputChannels")] public uint InputChannels { get; set; }
        [JsonPropertyName("inputMask")] public uint InputMask { get; set; }
        [JsonPropertyName("sampleRate")] public uint SampleRate { get; set; }
        [JsonPropertyName("configFileTime")] public long ConfigFileTime { get; set; }
        [JsonPropertyName("enabled")] public bool Enabled { get; set; }
    }
    private sealed class CachedStatus {
        public long NextProbe;
        public DateTime WrittenUtc;
        public DspLiveStatus? Value;
    }
    private static readonly object CacheGate = new();
    private static readonly Dictionary<string, CachedStatus> Cache = new(StringComparer.OrdinalIgnoreCase);
    internal static long DeserializationCount { get; private set; }
    internal static void ClearCache() { lock(CacheGate) { Cache.Clear();DeserializationCount = 0; } }
    [JsonPropertyName("configFileTime")] public long ConfigFileTime { get; set; }
    [JsonPropertyName("pid")] public int Pid { get; set; }
    [JsonPropertyName("active")] public bool Active { get; set; }
    [JsonPropertyName("mode")] public int Mode { get; set; }
    [JsonPropertyName("auto")] public bool Auto { get; set; }
    [JsonPropertyName("instance")] public int Instance { get; set; }
    [JsonPropertyName("channels")] public uint Channels { get; set; }
    [JsonPropertyName("sampleRate")] public uint SampleRate { get; set; }
    [JsonPropertyName("inputMask")] public uint InputMask { get; set; }
    [JsonPropertyName("outputMask")] public uint OutputMask { get; set; }
    [JsonPropertyName("enabled")] public bool Enabled { get; set; } = true;
    [JsonPropertyName("sources")] public Source[] Sources { get; set; } = Array.Empty<Source>();
    [JsonPropertyName("outputPeak")] public float[] OutputPeak { get; set; } = Array.Empty<float>();
    [JsonPropertyName("preLimiterPeak")] public float PreLimiterPeak { get; set; }
    [JsonPropertyName("clippedSamples")] public ulong ClippedSamples { get; set; }
    [JsonPropertyName("limiterReductionDb")] public float LimiterReductionDb { get; set; }
    [JsonIgnore] public DateTime WrittenUtc { get; private set; }
    [JsonPropertyName("inputRms")] public float[] InputRms { get; set; } = Array.Empty<float>();
    [JsonPropertyName("outputRms")] public float[] OutputRms { get; set; } = Array.Empty<float>();

    public static DspLiveStatus? Read(string? path = null)
    {
        path ??= Path.Combine(Path.GetDirectoryName(ApoEfxConfig.DefaultPath)!, "apo-live-status.json");
        lock(CacheGate) {
            if(!Cache.TryGetValue(path, out var cached)) {
                if(Cache.Count >= 8) Cache.Clear();
                Cache[path] = cached = new CachedStatus();
            }
            long now = Environment.TickCount64;
            if(now >= cached.NextProbe) {
                cached.NextProbe = now + 100;
                try {
                    DateTime written = File.GetLastWriteTimeUtc(path);
                    if(written != cached.WrittenUtc) {
                        if(DateTime.UtcNow - written > TimeSpan.FromSeconds(2) || new FileInfo(path).Length > 65536) {
                            cached.Value = null;cached.WrittenUtc = written;
                        } else {
                            using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                            var status = JsonSerializer.Deserialize<DspLiveStatus>(file);
                            if(status is not null) {
                                status.Sources ??= Array.Empty<Source>();
                                status.InputRms ??= Array.Empty<float>();
                                status.OutputRms ??= Array.Empty<float>();
                                status.OutputPeak ??= Array.Empty<float>();
                            }
                            if(status is not null) status.WrittenUtc = written;
                            ++DeserializationCount;
                            cached.Value = status;cached.WrittenUtc = written;
                        }
                    }
                } catch {
                    // Un écrivain peut être entre WriteFile et SetEndOfFile : garder
                    // la dernière trame valide, puis réessayer, sans faux acquittement.
                    cached.NextProbe = now + 25;
                }
            }
            return cached.Value is { Active: true } && DateTime.UtcNow - cached.WrittenUtc < TimeSpan.FromSeconds(2)
                ? cached.Value : null;
        }
    }
}
