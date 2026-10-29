using System.Text.Json;
using System.Text.Json.Nodes;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Export de diagnostics EXPURGÉ : aucun PCM utilisateur, aucun nom de machine,
/// aucun numéro de série, aucune clé d'endpoint complète. Ne contient que les
/// éléments de télémétrie C7 nécessaires au diagnostic à distance.
/// </summary>
public static class DiagnosticsExporter
{
    public static string Export(AgentState? state, DspLiveStatus? live = null)
    {
        var root = new JsonObject
        {
            ["schemaVersion"] = 1,
            ["title"] = "Diagnostic expurgé Xonar SE",
            ["generatedAtUtc"] = DateTimeOffset.UtcNow.ToString("o"),
            ["contractVersions"] = new JsonObject
            {
                ["C1"] = "1.0",
                ["C2"] = "1.0",
                ["C3"] = "1.0",
                ["C4"] = "1.0",
                ["C5"] = "1.0",
                ["C6"] = "1.0",
                ["C7"] = "1.0",
                ["C8"] = "1.0",
            },
        };

        if (state is null)
        {
            root["state"] = null;
            return root.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
        }

        var s = new JsonObject
        {
            // La clé d'endpoint est opaque mais potentiellement identifiable :
            // on n'en exporte que la longueur, jamais la valeur.
            ["endpointKeyLength"] = state.EndpointKey?.Length ?? 0,
            ["endpointRole"] = Wire.ToWire(state.EndpointRole),
            ["revision"] = state.Revision,
            ["driver"] = Driver(state.Driver),
            ["negotiatedFormat"] = Format(state.NegotiatedFormat),
            ["listeningMode"] = state.ListeningMode is ListeningMode m ? Wire.ToWire(m) : null,
            ["requestedPolicy"] = state.RequestedPolicy is SourcePolicy p ? Wire.ToWire(p) : null,
            ["resolvedPolicy"] = Wire.ToWire(state.ResolvedPolicy),
            ["desiredGeneration"] = state.DesiredGeneration,
            ["committedGeneration"] = state.CommittedGeneration,
            ["partiallyApplied"] = state.PartiallyApplied,
            ["instances"] = new JsonArray(state.Instances?.Select(Instance).ToArray() ?? Array.Empty<JsonNode>()),
            ["jack"] = new JsonObject
            {
                ["reliability"] = Wire.ToWire(state.Jack?.Reliability ?? JackReliability.Unknown),
                ["publishedConnected"] = state.Jack?.PublishedConnected,
            },
            ["meters"] = Meters(state.Meters),
        };

        root["state"] = s;
        if(live is not null) {
            // Les compteurs non instrumentés ne doivent pas être publiés comme des zéros mesurés.
            if(s["meters"] is JsonObject meters) { meters["nonFiniteFrames"] = null; meters["telemetryLost"] = null; }
            root["signal"] = new JsonObject {
            ["ageBasis"] = "publicationFile",
            ["preLimiterPeak"] = live.PreLimiterPeak, ["clippedSamples"] = live.ClippedSamples,
            ["limiterReductionDb"] = live.LimiterReductionDb,
            ["sources"] = new JsonArray(live.Sources.Select(source => (JsonNode)new JsonObject {
                ["channels"] = source.InputChannels, ["channelMask"] = source.InputMask, ["sampleRate"] = source.SampleRate
            }).ToArray())
            };
        }
        return root.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
    }

    private static JsonObject Driver(DriverInfo driver)
    {
        return new JsonObject
        {
            ["kind"] = Wire.ToWire(driver.Kind),
            ["version"] = driver.Version,
            ["packageSha256"] = driver.PackageSha256,
        };
    }

    private static JsonObject? Format(NegotiatedFormat? format)
    {
        if (format is null)
        {
            return null;
        }
        return new JsonObject
        {
            ["sampleRate"] = format.SampleRate,
            ["channelMask"] = format.ChannelMask,
            ["channelCount"] = format.ChannelCount,
            ["sampleFormat"] = Wire.ToWire(format.SampleFormat),
        };
    }

    private static JsonObject Instance(ApoInstanceState instance)
    {
        return new JsonObject
        {
            ["stage"] = Wire.ToWire(instance.Stage),
            ["epoch"] = instance.Epoch,
            ["appliedGeneration"] = instance.AppliedGeneration,
            ["inputFormat"] = Format(instance.InputFormat),
            ["bypass"] = new JsonObject
            {
                ["kind"] = Wire.ToWire(instance.Bypass?.Kind ?? BypassKind.Unknown),
                ["reason"] = instance.Bypass?.Reason is BypassReason r ? Wire.ToWire(r) : null,
            },
        };
    }

    private static JsonObject? Meters(MeterState? meters)
    {
        if (meters is null)
        {
            return null;
        }
        return new JsonObject
        {
            ["ageMs"] = meters.AgeMs,
            ["stale"] = meters.Stale,
            ["outputPeakDbfs"] = new JsonArray(meters.OutputPeakDbfs?.Select(v => (JsonNode)JsonValue.Create(v)).ToArray() ?? Array.Empty<JsonNode>()),
            ["limiterReductionDb"] = meters.LimiterReductionDb,
            ["nonFiniteFrames"] = meters.NonFiniteFrames,
            ["telemetryLost"] = meters.TelemetryLost,
        };
    }
}
