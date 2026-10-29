using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>Issue d'une validation de profil v1.</summary>
public sealed class ProfileValidationOutcome
{
    public bool Valid { get; set; }

    /// <summary>Document normalisé (défauts appliqués) lorsque <see cref="Valid"/> est vrai.</summary>
    public ProfileDocument? Normalized { get; set; }

    public List<Diagnostic> Diagnostics { get; } = new();
}

/// <summary>
/// Validation sémantique et schéma du document de profil v1 (C3). Miroir C# du
/// codec C++ de src/audio-control/profiles ; utilisé par le faux agent pour
/// simuler « validation OK » et « refus de schéma ». Ne valide jamais un champ
/// absent du schéma et ne réécrit pas les DTO générés.
/// </summary>
public static class ProfileValidator
{
    private static readonly Regex UuidRegex = new(Constants.UuidPattern, RegexOptions.CultureInvariant);

    /// <summary>
    /// Valide un document JSON déjà analysé. <paramref name="sampleRate"/> fournit
    /// le contexte Nyquist (fréquence de connexion) ; s'il est null, la règle
    /// Nyquist n'est pas appliquée (validation DSP portable uniquement).
    /// </summary>
    public static ProfileValidationOutcome Validate(JsonNode root, uint? sampleRate = null)
    {
        var outcome = new ProfileValidationOutcome();
        var diags = outcome.Diagnostics;

        if (root is not JsonObject obj)
        {
            diags.Add(MakeDiag("schema.type", "$", "Le document racine doit être un objet JSON."));
            return outcome;
        }

        var normalized = new ProfileDocument();

        // --- champs obligatoires -------------------------------------------------
        if (!TryGetRequired(obj, "schemaVersion", out JsonNode? schemaVersionNode, diags, "$"))
        {
            return outcome;
        }

        if (!TryReadUInt(schemaVersionNode!, "$/schemaVersion", diags, out uint schemaVersion))
        {
            return outcome;
        }

        if (schemaVersion != Constants.ProfileSchemaVersion)
        {
            diags.Add(MakeDiag("version.unsupportedMajor", "$/schemaVersion",
                $"Version majeure {schemaVersion} non prise en charge ; seule la version 1 est acceptée."));
            return outcome;
        }
        normalized.SchemaVersion = schemaVersion;

        if (!TryGetRequired(obj, "profileId", out JsonNode? profileIdNode, diags, "$"))
        {
            return outcome;
        }
        string? profileId = profileIdNode is JsonValue v && v.TryGetValue<string>(out string? s) ? s : null;
        if (profileId is null || !UuidRegex.IsMatch(profileId))
        {
            diags.Add(MakeDiag("schema.pattern", "$/profileId", "profileId doit être un UUID canonique en minuscules."));
            return outcome;
        }
        normalized.ProfileId = profileId;

        if (!TryGetRequired(obj, "revision", out JsonNode? revisionNode, diags, "$"))
        {
            return outcome;
        }
        if (!TryReadUInt(revisionNode!, "$/revision", diags, out ulong revision) || revision < ProfileDocumentLimits.RevisionMin)
        {
            diags.Add(MakeDiag("schema.minimum", "$/revision", "revision doit être un entier ≥ 1."));
            return outcome;
        }
        normalized.Revision = revision;

        if (!TryGetRequired(obj, "endpointRole", out JsonNode? roleNode, diags, "$"))
        {
            return outcome;
        }
        string? role = roleNode is JsonValue rv && rv.TryGetValue<string>(out string? rs) ? rs : null;
        if (role != Wire.ToWire(EndpointRole.Speakers))
        {
            diags.Add(MakeDiag("schema.enum", "$/endpointRole", "endpointRole doit valoir « speakers » pour v1."));
            return outcome;
        }
        normalized.EndpointRole = EndpointRole.Speakers;

        if (!TryGetRequired(obj, "listeningMode", out JsonNode? modeNode, diags, "$"))
        {
            return outcome;
        }
        string? modeWire = modeNode is JsonValue mv && mv.TryGetValue<string>(out string? ms) ? ms : null;
        if (modeWire is null || !Wire.TryFromWire(modeWire, out ListeningMode listeningMode))
        {
            diags.Add(MakeDiag("schema.enum", "$/listeningMode", "listeningMode doit valoir « 5.1 », « upmix », « 2.1 » ou « 2.0 »."));
            return outcome;
        }
        normalized.ListeningMode = listeningMode;

        if (!TryGetRequired(obj, "sourcePolicy", out JsonNode? policyNode, diags, "$"))
        {
            return outcome;
        }
        string? policyWire = policyNode is JsonValue pv && pv.TryGetValue<string>(out string? ps) ? ps : null;
        if (policyWire is null || !Wire.TryFromWire(policyWire, out SourcePolicy sourcePolicy))
        {
            diags.Add(MakeDiag("schema.enum", "$/sourcePolicy", "sourcePolicy doit valoir « preserve », « autoDeclaredLayout » ou « forceStereoFrontPair »."));
            return outcome;
        }
        normalized.SourcePolicy = sourcePolicy;

        // --- champs facultatifs --------------------------------------------------
        if (obj.TryGetPropertyValue("name", out JsonNode? nameNode) && nameNode is not null)
        {
            string? name = nameNode is JsonValue nv && nv.TryGetValue<string>(out string? ns) ? ns : null;
            if (name is null)
            {
                diags.Add(MakeDiag("schema.type", "$/name", "name doit être une chaîne."));
                return outcome;
            }
            if (name.Length < 1)
            {
                diags.Add(MakeDiag("schema.minLength", "$/name", "name doit contenir au moins 1 caractère."));
                return outcome;
            }
            if (name.Length > ProfileDocumentLimits.NameMaxLength)
            {
                diags.Add(MakeDiag("schema.maxLength", "$/name", "name doit contenir au plus 80 caractères."));
                return outcome;
            }
            normalized.Name = name;
        }

        if (obj.TryGetPropertyValue("description", out JsonNode? descriptionNode) && descriptionNode is not null)
        {
            string? description = descriptionNode is JsonValue dv && dv.TryGetValue<string>(out string? ds) ? ds : null;
            if (description is null)
            {
                diags.Add(MakeDiag("schema.type", "$/description", "description doit être une chaîne."));
                return outcome;
            }
            if (description.Length > ProfileDocumentLimits.DescriptionMaxLength)
            {
                diags.Add(MakeDiag("schema.maxLength", "$/description", "description doit contenir au plus 500 caractères."));
                return outcome;
            }
            normalized.Description = description;
        }

        // --- routing -------------------------------------------------------------
        if (!TryGetRequired(obj, "routing", out JsonNode? routingNode, diags, "$"))
        {
            return outcome;
        }
        if (routingNode is not JsonObject routingObj)
        {
            diags.Add(MakeDiag("schema.type", "$/routing", "routing doit être un objet."));
            return outcome;
        }
        if (!ValidateRouting(routingObj, diags, normalized.Routing))
        {
            return outcome;
        }

        // --- output --------------------------------------------------------------
        if (!TryGetRequired(obj, "output", out JsonNode? outputNode, diags, "$"))
        {
            return outcome;
        }
        if (outputNode is not JsonObject outputObj)
        {
            diags.Add(MakeDiag("schema.type", "$/output", "output doit être un objet."));
            return outcome;
        }
        if (!ValidateOutput(outputObj, diags, normalized.Output, sampleRate))
        {
            return outcome;
        }

        // Rejeter les propriétés inconnues (additionalProperties:false).
        if (!RejectUnknown(obj, new HashSet<string>
        {
            "schemaVersion", "profileId", "revision", "name", "description",
            "endpointRole", "listeningMode", "sourcePolicy", "routing", "output",
        }, diags))
        {
            return outcome;
        }

        outcome.Valid = true;
        outcome.Normalized = normalized;
        return outcome;
    }

    // ---------------------------------------------------------------------------

    private static bool ValidateRouting(JsonObject obj, List<Diagnostic> diags, RoutingSettings normalized)
    {
        if (!RejectUnknown(obj, new HashSet<string> { "upmix" }, diags, "$/routing"))
        {
            return false;
        }

        if (obj.TryGetPropertyValue("upmix", out JsonNode? upmixNode) && upmixNode is not null)
        {
            if (upmixNode is not JsonObject upmixObj)
            {
                diags.Add(MakeDiag("schema.type", "$/routing/upmix", "routing.upmix doit être un objet."));
                return false;
            }
            return ValidateUpmix(upmixObj, diags, normalized.Upmix);
        }
        return true;
    }

    private static bool ValidateUpmix(JsonObject obj, List<Diagnostic> diags, UpmixSettings normalized)
    {
        if (!RejectUnknown(obj, new HashSet<string>
        {
            "centerGainDb", "surroundGainDb", "deriveLfe", "lfeGainDb", "lfeLowPassHz",
        }, diags, "$/routing/upmix"))
        {
            return false;
        }

        if (!ReadDouble(obj, "centerGainDb", "$/routing/upmix/centerGainDb", diags, out double center, -30, 0, -3))
        {
            return false;
        }
        normalized.CenterGainDb = center;

        if (!ReadDouble(obj, "surroundGainDb", "$/routing/upmix/surroundGainDb", diags, out double surround, -30, 0, -6))
        {
            return false;
        }
        normalized.SurroundGainDb = surround;

        if (!ReadBool(obj, "deriveLfe", "$/routing/upmix/deriveLfe", diags, out bool derive, true))
        {
            return false;
        }
        normalized.DeriveLfe = derive;

        if (!ReadDouble(obj, "lfeGainDb", "$/routing/upmix/lfeGainDb", diags, out double lfeGain, -30, 0, -6))
        {
            return false;
        }
        normalized.LfeGainDb = lfeGain;

        if (!ReadDouble(obj, "lfeLowPassHz", "$/routing/upmix/lfeLowPassHz", diags, out double lfeLow, 40, 250, 120))
        {
            return false;
        }
        normalized.LfeLowPassHz = lfeLow;

        return true;
    }

    private static bool ValidateOutput(JsonObject obj, List<Diagnostic> diags, OutputSettings normalized, uint? sampleRate)
    {
        if (!RejectUnknown(obj, new HashSet<string>
        {
            "masterGainDb", "channelGainDb", "channelDelayMs", "channelMute", "eq",
            "downmix", "twoPointOne", "bassManagement", "limiter", "gainRampMs", "maxDelayMs",
        }, diags, "$/output"))
        {
            return false;
        }

        if (!ReadDouble(obj, "masterGainDb", "$/output/masterGainDb", diags, out double master,
                OutputSettingsLimits.MasterGainDbMin, OutputSettingsLimits.MasterGainDbMax, 0))
        {
            return false;
        }
        normalized.MasterGainDb = master;

        if (!ReadDoubleArray(obj, "channelGainDb", "$/output/channelGainDb", diags, 8,
                OutputSettingsLimits.ChannelGainDbMin, OutputSettingsLimits.ChannelGainDbMax, normalized.ChannelGainDb))
        {
            return false;
        }

        if (!ReadDoubleArray(obj, "channelDelayMs", "$/output/channelDelayMs", diags, 8,
                OutputSettingsLimits.ChannelDelayMsMin, OutputSettingsLimits.ChannelDelayMsMax, normalized.ChannelDelayMs))
        {
            return false;
        }

        if (!ReadBoolArray(obj, "channelMute", "$/output/channelMute", diags, 8, normalized.ChannelMute))
        {
            return false;
        }

        // maxDelayMs est lue avant la règle sémantique channelDelayMs[i] ≤ maxDelayMs.
        if (!ReadDouble(obj, "maxDelayMs", "$/output/maxDelayMs", diags, out double maxDelay,
                OutputSettingsLimits.MaxDelayMsMin, OutputSettingsLimits.MaxDelayMsMax, 50))
        {
            return false;
        }
        normalized.MaxDelayMs = maxDelay;
        for (int i = 0; i < 8; i++)
        {
            if (normalized.ChannelDelayMs[i] > maxDelay)
            {
                diags.Add(MakeDiag("semantic.delayExceedsMax", $"/output/channelDelayMs/{i}",
                    $"Le délai du canal {i} dépasse maxDelayMs ({maxDelay} ms)."));
                return false;
            }
        }

        if (!ReadEq(obj, diags, normalized.Eq, sampleRate))
        {
            return false;
        }

        if (obj.TryGetPropertyValue("downmix", out JsonNode? downmixNode) && downmixNode is not null)
        {
            if (downmixNode is not JsonObject downmixObj)
            {
                diags.Add(MakeDiag("schema.type", "$/output/downmix", "output.downmix doit être un objet."));
                return false;
            }
            if (!ValidateDownmix(downmixObj, diags, normalized.Downmix))
            {
                return false;
            }
        }

        if (obj.TryGetPropertyValue("twoPointOne", out JsonNode? twoPointOneNode) && twoPointOneNode is not null)
        {
            if (twoPointOneNode is not JsonObject twoPointOneObj)
            {
                diags.Add(MakeDiag("schema.type", "$/output/twoPointOne", "output.twoPointOne doit être un objet."));
                return false;
            }
            if (!ValidateTwoPointOne(twoPointOneObj, diags, normalized.TwoPointOne, sampleRate))
            {
                return false;
            }
        }

        if (obj.TryGetPropertyValue("bassManagement", out JsonNode? bassNode) && bassNode is not null)
        {
            if (bassNode is not JsonObject bassObj)
            {
                diags.Add(MakeDiag("schema.type", "$/output/bassManagement", "output.bassManagement doit être un objet."));
                return false;
            }
            if (!ValidateBassManagement(bassObj, diags, normalized.BassManagement, sampleRate))
            {
                return false;
            }
        }

        if (obj.TryGetPropertyValue("limiter", out JsonNode? limiterNode) && limiterNode is not null)
        {
            if (limiterNode is not JsonObject limiterObj)
            {
                diags.Add(MakeDiag("schema.type", "$/output/limiter", "output.limiter doit être un objet."));
                return false;
            }
            if (!ValidateLimiter(limiterObj, diags, normalized.Limiter))
            {
                return false;
            }
        }

        if (!ReadUInt(obj, "gainRampMs", "$/output/gainRampMs", diags, out uint gainRamp,
                OutputSettingsLimits.GainRampMsMin, OutputSettingsLimits.GainRampMsMax, 20))
        {
            return false;
        }
        normalized.GainRampMs = gainRamp;

        return true;
    }

    private static bool ValidateDownmix(JsonObject obj, List<Diagnostic> diags, DownmixSettings normalized)
    {
        if (!RejectUnknown(obj, new HashSet<string> { "centerGainDb", "surroundGainDb", "includeLfe", "lfeGainDb", "headroomDb" }, diags, "$/output/downmix"))
        {
            return false;
        }

        if (!ReadDouble(obj, "centerGainDb", "$/output/downmix/centerGainDb", diags, out double center, -30, 0, -3))
        {
            return false;
        }
        normalized.CenterGainDb = center;

        if (!ReadDouble(obj, "surroundGainDb", "$/output/downmix/surroundGainDb", diags, out double surround, -30, 0, -3))
        {
            return false;
        }
        normalized.SurroundGainDb = surround;

        if (!ReadBool(obj, "includeLfe", "$/output/downmix/includeLfe", diags, out bool includeLfe, false))
        {
            return false;
        }
        normalized.IncludeLfe = includeLfe;

        if (!ReadDouble(obj, "lfeGainDb", "$/output/downmix/lfeGainDb", diags, out double lfeGain, -30, 0, -6))
        {
            return false;
        }
        normalized.LfeGainDb = lfeGain;

        if (!ReadDouble(obj, "headroomDb", "$/output/downmix/headroomDb", diags, out double headroom, -12, 0, -4.5))
        {
            return false;
        }
        normalized.HeadroomDb = headroom;

        return true;
    }

    private static bool ValidateTwoPointOne(JsonObject obj, List<Diagnostic> diags, TwoPointOneSettings normalized, uint? sampleRate)
    {
        if (!RejectUnknown(obj, new HashSet<string> { "lfeGainDb", "lfeLowPassHz", "includeSourceLfe", "sourceLfeGainDb" }, diags, "$/output/twoPointOne"))
        {
            return false;
        }

        if (!ReadDouble(obj, "lfeGainDb", "$/output/twoPointOne/lfeGainDb", diags, out double lfeGain, -30, 6, 0))
        {
            return false;
        }
        normalized.LfeGainDb = lfeGain;

        if (!ReadDouble(obj, "lfeLowPassHz", "$/output/twoPointOne/lfeLowPassHz", diags, out double lfeLow, 40, 200, 80))
        {
            return false;
        }
        normalized.LfeLowPassHz = lfeLow;
        if (sampleRate is uint sr && !IsBelowNyquist(lfeLow, sr))
        {
            diags.Add(MakeDiag("semantic.nyquist", "$/output/twoPointOne/lfeLowPassHz", $"lfeLowPassHz doit être strictement inférieur à Nyquist ({sr / 2.0} Hz)."));
            return false;
        }

        if (!ReadBool(obj, "includeSourceLfe", "$/output/twoPointOne/includeSourceLfe", diags, out bool include, true))
        {
            return false;
        }
        normalized.IncludeSourceLfe = include;

        if (!ReadDouble(obj, "sourceLfeGainDb", "$/output/twoPointOne/sourceLfeGainDb", diags, out double sourceGain, -30, 6, 0))
        {
            return false;
        }
        normalized.SourceLfeGainDb = sourceGain;

        return true;
    }

    private static bool ValidateBassManagement(JsonObject obj, List<Diagnostic> diags, BassManagementSettings normalized, uint? sampleRate)
    {
        if (!RejectUnknown(obj, new HashSet<string> { "enabled", "crossoverHz", "highPassSatellites", "sumToLfe", "sumToLfeGainDb" }, diags, "$/output/bassManagement"))
        {
            return false;
        }

        if (!ReadBool(obj, "enabled", "$/output/bassManagement/enabled", diags, out bool enabled, false))
        {
            return false;
        }
        normalized.Enabled = enabled;

        if (!ReadDouble(obj, "crossoverHz", "$/output/bassManagement/crossoverHz", diags, out double crossover, 40, 200, 80))
        {
            return false;
        }
        normalized.CrossoverHz = crossover;
        if (sampleRate is uint sr && !IsBelowNyquist(crossover, sr))
        {
            diags.Add(MakeDiag("semantic.nyquist", "$/output/bassManagement/crossoverHz", $"crossoverHz doit être strictement inférieur à Nyquist ({sr / 2.0} Hz)."));
            return false;
        }

        if (!ReadBool(obj, "highPassSatellites", "$/output/bassManagement/highPassSatellites", diags, out bool highPass, true))
        {
            return false;
        }
        normalized.HighPassSatellites = highPass;

        if (!ReadBool(obj, "sumToLfe", "$/output/bassManagement/sumToLfe", diags, out bool sumToLfe, true))
        {
            return false;
        }
        normalized.SumToLfe = sumToLfe;

        if (!ReadDouble(obj, "sumToLfeGainDb", "$/output/bassManagement/sumToLfeGainDb", diags, out double sumGain, -20, 6, 0))
        {
            return false;
        }
        normalized.SumToLfeGainDb = sumGain;

        return true;
    }

    private static bool ValidateLimiter(JsonObject obj, List<Diagnostic> diags, LimiterSettings normalized)
    {
        if (!RejectUnknown(obj, new HashSet<string> { "enabled", "ceilingDb", "releaseMs" }, diags, "$/output/limiter"))
        {
            return false;
        }

        if (!ReadBool(obj, "enabled", "$/output/limiter/enabled", diags, out bool enabled, true))
        {
            return false;
        }
        normalized.Enabled = enabled;

        if (!ReadDouble(obj, "ceilingDb", "$/output/limiter/ceilingDb", diags, out double ceiling, -12, 0, -0.3))
        {
            return false;
        }
        normalized.CeilingDb = ceiling;

        if (!ReadDouble(obj, "releaseMs", "$/output/limiter/releaseMs", diags, out double release, 5, 1000, 80))
        {
            return false;
        }
        normalized.ReleaseMs = release;

        return true;
    }

    private static bool ReadEq(JsonObject obj, List<Diagnostic> diags, EqBand[][] normalized, uint? sampleRate)
    {
        if (!obj.TryGetPropertyValue("eq", out JsonNode? eqNode) || eqNode is null)
        {
            return true; // absent : défauts déjà posés
        }

        if (eqNode is not System.Text.Json.Nodes.JsonArray eqArray)
        {
            diags.Add(MakeDiag("schema.type", "$/output/eq", "eq doit être un tableau."));
            return false;
        }
        if (eqArray.Count != 8)
        {
            diags.Add(MakeDiag(eqArray.Count < 8 ? "schema.minItems" : "schema.maxItems", "$/output/eq", "eq doit contenir exactement 8 tableaux de canaux."));
            return false;
        }

        for (int ch = 0; ch < 8; ch++)
        {
            if (eqArray[ch] is not System.Text.Json.Nodes.JsonArray bandArray)
            {
                diags.Add(MakeDiag("schema.type", $"/output/eq/{ch}", $"eq[{ch}] doit être un tableau."));
                return false;
            }
            if (bandArray.Count != 10)
            {
                diags.Add(MakeDiag(bandArray.Count < 10 ? "schema.minItems" : "schema.maxItems", $"/output/eq/{ch}", $"eq[{ch}] doit contenir exactement 10 bandes."));
                return false;
            }

            for (int b = 0; b < 10; b++)
            {
                if (bandArray[b] is not JsonObject bandObj)
                {
                    diags.Add(MakeDiag("schema.type", $"/output/eq/{ch}/{b}", $"La bande eq[{ch}][{b}] doit être un objet."));
                    return false;
                }

                if (!RejectUnknown(bandObj, new HashSet<string> { "enabled", "frequencyHz", "q", "gainDb" }, diags, $"/output/eq/{ch}/{b}"))
                {
                    return false;
                }

                var band = normalized[ch][b];

                if (!ReadBool(bandObj, "enabled", $"/output/eq/{ch}/{b}/enabled", diags, out bool enabled, false))
                {
                    return false;
                }
                band.Enabled = enabled;

                if (!ReadDouble(bandObj, "frequencyHz", $"/output/eq/{ch}/{b}/frequencyHz", diags, out double freq,
                        EqBandLimits.FrequencyHzMin, EqBandLimits.FrequencyHzMax, 1000))
                {
                    return false;
                }
                band.FrequencyHz = freq;
                if (sampleRate is uint sr && !IsBelowNyquist(freq, sr))
                {
                    diags.Add(MakeDiag("semantic.nyquist", $"/output/eq/{ch}/{b}/frequencyHz",
                        $"La fréquence de la bande eq[{ch}][{b}] doit être strictement inférieure à Nyquist ({sr / 2.0} Hz)."));
                    return false;
                }

                if (!ReadDouble(bandObj, "q", $"/output/eq/{ch}/{b}/q", diags, out double q,
                        double.NegativeInfinity, EqBandLimits.QMax, 1))
                {
                    return false;
                }
                if (q <= 0)
                {
                    diags.Add(MakeDiag("schema.exclusiveMinimum", $"/output/eq/{ch}/{b}/q", "q doit être strictement supérieur à 0."));
                    return false;
                }
                band.Q = q;

                if (!ReadDouble(bandObj, "gainDb", $"/output/eq/{ch}/{b}/gainDb", diags, out double gain,
                        EqBandLimits.GainDbMin, EqBandLimits.GainDbMax, 0))
                {
                    return false;
                }
                band.GainDb = gain;
            }
        }

        return true;
    }

    // ---------------------------------------------------------------------------
    // Lecteurs élémentaires
    // ---------------------------------------------------------------------------

    private static bool IsBelowNyquist(double frequencyHz, uint sampleRate) =>
        frequencyHz < sampleRate / 2.0;

    private static Diagnostic MakeDiag(string code, string path, string message) =>
        new() { Code = code, Path = path, Message = message };

    private static bool RejectUnknown(JsonObject obj, HashSet<string> allowed, List<Diagnostic> diags, string basePath = "$")
    {
        foreach (var pair in obj)
        {
            if (!allowed.Contains(pair.Key))
            {
                diags.Add(MakeDiag("schema.additionalProperties", basePath + "/" + pair.Key,
                    $"Propriété inconnue « {pair.Key} » : le schéma v1 refuse toute propriété non déclarée."));
                return false;
            }
        }
        return true;
    }

    private static bool TryGetRequired(JsonObject obj, string name, out JsonNode? value, List<Diagnostic> diags, string path)
    {
        if (obj.TryGetPropertyValue(name, out value) && value is not null)
        {
            return true;
        }
        diags.Add(MakeDiag("schema.required", path + "/" + name, $"Champ obligatoire manquant : {name}."));
        return false;
    }

    private static bool TryReadUInt(JsonNode node, string path, List<Diagnostic> diags, out ulong value)
    {
        if (node is JsonValue v && v.TryGetValue<ulong>(out ulong u))
        {
            value = u;
            return true;
        }
        diags.Add(MakeDiag("schema.type", path, "Valeur entière non négative attendue."));
        value = 0;
        return false;
    }

    private static bool TryReadUInt(JsonNode node, string path, List<Diagnostic> diags, out uint value)
    {
        if (node is JsonValue v && v.TryGetValue<uint>(out uint u))
        {
            value = u;
            return true;
        }
        diags.Add(MakeDiag("schema.type", path, "Valeur entière non négative attendue."));
        value = 0;
        return false;
    }

    private static bool ReadBool(JsonObject obj, string name, string path, List<Diagnostic> diags, out bool value, bool defaultValue)
    {
        if (!obj.TryGetPropertyValue(name, out JsonNode? node) || node is null)
        {
            value = defaultValue;
            return true;
        }
        if (node is JsonValue v && v.TryGetValue<bool>(out bool b))
        {
            value = b;
            return true;
        }
        diags.Add(MakeDiag("schema.type", path, "Valeur booléenne attendue."));
        value = default;
        return false;
    }

    private static bool ReadDouble(JsonObject obj, string name, string path, List<Diagnostic> diags,
        out double value, double min, double max, double defaultValue)
    {
        if (!obj.TryGetPropertyValue(name, out JsonNode? node) || node is null)
        {
            value = defaultValue;
            return true;
        }
        if (node is not JsonValue v || !v.TryGetValue<double>(out double d) || double.IsNaN(d) || double.IsInfinity(d))
        {
            diags.Add(MakeDiag("schema.type", path, "Valeur numérique finie attendue."));
            value = 0;
            return false;
        }
        if (d < min)
        {
            diags.Add(MakeDiag("schema.minimum", path, $"Valeur sous la borne minimale : {min}."));
            value = 0;
            return false;
        }
        if (d > max)
        {
            diags.Add(MakeDiag("schema.maximum", path, $"Valeur au-dessus de la borne maximale : {max}."));
            value = 0;
            return false;
        }
        value = d;
        return true;
    }

    private static bool ReadUInt(JsonObject obj, string name, string path, List<Diagnostic> diags,
        out uint value, uint min, uint max, uint defaultValue)
    {
        if (!obj.TryGetPropertyValue(name, out JsonNode? node) || node is null)
        {
            value = defaultValue;
            return true;
        }
        if (!TryReadUInt(node, path, diags, out value))
        {
            return false;
        }
        if (value < min)
        {
            diags.Add(MakeDiag("schema.minimum", path, $"Valeur sous la borne minimale : {min}."));
            return false;
        }
        if (value > max)
        {
            diags.Add(MakeDiag("schema.maximum", path, $"Valeur au-dessus de la borne maximale : {max}."));
            return false;
        }
        return true;
    }

    private static bool ReadDoubleArray(JsonObject obj, string name, string path, List<Diagnostic> diags,
        int length, double min, double max, double[] target)
    {
        if (!obj.TryGetPropertyValue(name, out JsonNode? node) || node is null)
        {
            return true; // défauts déjà posés
        }
        if (node is not System.Text.Json.Nodes.JsonArray array)
        {
            diags.Add(MakeDiag("schema.type", path, "Un tableau est attendu."));
            return false;
        }
        if (array.Count != length)
        {
            diags.Add(MakeDiag(array.Count < length ? "schema.minItems" : "schema.maxItems", path, $"Le tableau doit contenir exactement {length} valeurs."));
            return false;
        }
        for (int i = 0; i < length; i++)
        {
            if (array[i] is not JsonValue v || !v.TryGetValue<double>(out double d) || double.IsNaN(d) || double.IsInfinity(d))
            {
                diags.Add(MakeDiag("schema.type", $"{path}/{i}", "Valeur numérique finie attendue."));
                return false;
            }
            if (d < min)
            {
                diags.Add(MakeDiag("schema.minimum", $"{path}/{i}", $"Valeur sous la borne minimale : {min}."));
                return false;
            }
            if (d > max)
            {
                diags.Add(MakeDiag("schema.maximum", $"{path}/{i}", $"Valeur au-dessus de la borne maximale : {max}."));
                return false;
            }
            target[i] = d;
        }
        return true;
    }

    private static bool ReadBoolArray(JsonObject obj, string name, string path, List<Diagnostic> diags, int length, bool[] target)
    {
        if (!obj.TryGetPropertyValue(name, out JsonNode? node) || node is null)
        {
            return true;
        }
        if (node is not System.Text.Json.Nodes.JsonArray array)
        {
            diags.Add(MakeDiag("schema.type", path, "Un tableau est attendu."));
            return false;
        }
        if (array.Count != length)
        {
            diags.Add(MakeDiag(array.Count < length ? "schema.minItems" : "schema.maxItems", path, $"Le tableau doit contenir exactement {length} valeurs."));
            return false;
        }
        for (int i = 0; i < length; i++)
        {
            if (array[i] is not JsonValue v || !v.TryGetValue<bool>(out bool b))
            {
                diags.Add(MakeDiag("schema.type", $"{path}/{i}", "Valeur booléenne attendue."));
                return false;
            }
            target[i] = b;
        }
        return true;
    }
}
