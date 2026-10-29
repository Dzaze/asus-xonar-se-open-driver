using System.Text.Json.Nodes;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Écriture des DTO générés vers leur forme filaire JSON v1 (clés camelCase,
/// énumérations en valeur filaire « 5.1 », « upmix », …). Les DTO générés ne
/// portent aucune annotation de sérialisation : cette classe est la seule
/// passerelle de l'interface vers le format filaire des contrats.
/// </summary>
public static class ContractJson
{
    /// <summary>Écrit un document de profil v1 sous sa forme filaire.</summary>
    public static JsonObject Profile(ProfileDocument p)
    {
        var root = new JsonObject
        {
            ["schemaVersion"] = p.SchemaVersion,
            ["profileId"] = p.ProfileId,
            ["revision"] = p.Revision,
        };

        if (p.Name is not null)
        {
            root["name"] = p.Name;
        }
        if (p.Description is not null)
        {
            root["description"] = p.Description;
        }

        root["endpointRole"] = Wire.ToWire(p.EndpointRole);
        root["listeningMode"] = Wire.ToWire(p.ListeningMode);
        root["sourcePolicy"] = Wire.ToWire(p.SourcePolicy);

        var routing = new JsonObject();
        if (p.Routing?.Upmix is not null)
        {
            routing["upmix"] = Upmix(p.Routing.Upmix);
        }
        root["routing"] = routing;

        root["output"] = Output(p.Output);
        return root;
    }

    private static JsonObject Upmix(UpmixSettings u)
    {
        return new JsonObject
        {
            ["centerGainDb"] = u.CenterGainDb,
            ["surroundGainDb"] = u.SurroundGainDb,
            ["deriveLfe"] = u.DeriveLfe,
            ["lfeGainDb"] = u.LfeGainDb,
            ["lfeLowPassHz"] = u.LfeLowPassHz,
        };
    }

    private static JsonObject Output(OutputSettings o)
    {
        var root = new JsonObject
        {
            ["masterGainDb"] = o.MasterGainDb,
            ["channelGainDb"] = ToArray(o.ChannelGainDb),
            ["channelDelayMs"] = ToArray(o.ChannelDelayMs),
            ["channelMute"] = ToArray(o.ChannelMute),
        };

        if (o.Eq is not null)
        {
            var eq = new JsonArray();
            for (int ch = 0; ch < o.Eq.Length; ch++)
            {
                var bands = new JsonArray();
                var channel = o.Eq[ch] ?? Array.Empty<EqBand>();
                for (int b = 0; b < channel.Length; b++)
                {
                    bands.Add(EqBand(channel[b]));
                }
                eq.Add(bands);
            }
            root["eq"] = eq;
        }

        if (o.Downmix is not null)
        {
            root["downmix"] = Downmix(o.Downmix);
        }
        if (o.TwoPointOne is not null)
        {
            root["twoPointOne"] = TwoPointOne(o.TwoPointOne);
        }
        if (o.BassManagement is not null)
        {
            root["bassManagement"] = BassManagement(o.BassManagement);
        }
        if (o.Limiter is not null)
        {
            root["limiter"] = Limiter(o.Limiter);
        }

        root["gainRampMs"] = o.GainRampMs;
        root["maxDelayMs"] = o.MaxDelayMs;
        return root;
    }

    private static JsonObject EqBand(EqBand b)
    {
        return new JsonObject
        {
            ["enabled"] = b.Enabled,
            ["frequencyHz"] = b.FrequencyHz,
            ["q"] = b.Q,
            ["gainDb"] = b.GainDb,
        };
    }

    private static JsonObject Downmix(DownmixSettings d)
    {
        return new JsonObject
        {
            ["centerGainDb"] = d.CenterGainDb,
            ["surroundGainDb"] = d.SurroundGainDb,
            ["includeLfe"] = d.IncludeLfe,
            ["lfeGainDb"] = d.LfeGainDb,
            ["headroomDb"] = d.HeadroomDb,
        };
    }

    private static JsonObject TwoPointOne(TwoPointOneSettings t)
    {
        return new JsonObject
        {
            ["lfeGainDb"] = t.LfeGainDb,
            ["lfeLowPassHz"] = t.LfeLowPassHz,
            ["includeSourceLfe"] = t.IncludeSourceLfe,
            ["sourceLfeGainDb"] = t.SourceLfeGainDb,
        };
    }

    private static JsonObject BassManagement(BassManagementSettings b)
    {
        return new JsonObject
        {
            ["enabled"] = b.Enabled,
            ["crossoverHz"] = b.CrossoverHz,
            ["highPassSatellites"] = b.HighPassSatellites,
            ["sumToLfe"] = b.SumToLfe,
            ["sumToLfeGainDb"] = b.SumToLfeGainDb,
        };
    }

    private static JsonObject Limiter(LimiterSettings l)
    {
        return new JsonObject
        {
            ["enabled"] = l.Enabled,
            ["ceilingDb"] = l.CeilingDb,
            ["releaseMs"] = l.ReleaseMs,
        };
    }

    private static JsonArray ToArray(double[] values)
    {
        var array = new JsonArray();
        foreach (var v in values ?? Array.Empty<double>())
        {
            array.Add(v);
        }
        return array;
    }

    private static JsonArray ToArray(bool[] values)
    {
        var array = new JsonArray();
        foreach (var v in values ?? Array.Empty<bool>())
        {
            array.Add(v);
        }
        return array;
    }
}
