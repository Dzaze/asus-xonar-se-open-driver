using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Fabriques de documents de profil v1 pour l'interface : preset conservateur
/// restaurable et brouillon neuf. Les défauts proviennent des DTO générés ; le
/// limiteur reste actif par défaut (C3).
/// </summary>
public static class ProfileDefaults
{
    /// <summary>Preset conservateur : 5.1 préservé, politique neutre, limiteur actif.</summary>
    public static ProfileDocument CreateConservative(string name = "Preset conservateur")
    {
        var profile = new ProfileDocument
        {
            SchemaVersion = Constants.ProfileSchemaVersion,
            ProfileId = Guid.NewGuid().ToString("d"),
            Revision = 1,
            Name = name,
            Description = "Preset conservateur restaurable : sortie 5.1 préservée, aucun upmix forcé, limiteur de sécurité actif.",
            EndpointRole = EndpointRole.Speakers,
            ListeningMode = ListeningMode.Surround51,
            SourcePolicy = SourcePolicy.Preserve,
            Routing = new RoutingSettings { Upmix = new UpmixSettings() },
            Output = new OutputSettings
            {
                MasterGainDb = 0,
                ChannelGainDb = new double[] { 0, 0, 0, 0, 0, 0, 0, 0 },
                ChannelDelayMs = new double[] { 0, 0, 0, 0, 0, 0, 0, 0 },
                ChannelMute = new bool[] { false, false, false, false, false, false, false, false },
                Eq = BuildDefaultEq(),
                Downmix = new DownmixSettings(),
                TwoPointOne = new TwoPointOneSettings(),
                BassManagement = new BassManagementSettings { Enabled = false },
                Limiter = new LimiterSettings { Enabled = true, CeilingDb = -0.3, ReleaseMs = 80 },
                GainRampMs = Constants.DefaultGainRampMs,
                MaxDelayMs = Constants.MaxDelayMs,
            },
        };
        return profile;
    }

    private static EqBand[][] BuildDefaultEq()
    {
        var eq = new EqBand[8][];
        for (int ch = 0; ch < 8; ch++)
        {
            eq[ch] = new EqBand[10];
            for (int b = 0; b < 10; b++)
            {
                eq[ch][b] = new EqBand { Enabled = false, FrequencyHz = 1000, Q = 1, GainDb = 0 };
            }
        }
        return eq;
    }

    /// <summary>Copie profonde d'un document de profil (annulation, brouillon).</summary>
    public static ProfileDocument Clone(ProfileDocument source)
    {
        var clone = new ProfileDocument
        {
            SchemaVersion = source.SchemaVersion,
            ProfileId = source.ProfileId,
            Revision = source.Revision,
            Name = source.Name,
            Description = source.Description,
            EndpointRole = source.EndpointRole,
            ListeningMode = source.ListeningMode,
            SourcePolicy = source.SourcePolicy,
            Routing = new RoutingSettings { Upmix = Clone(source.Routing?.Upmix) },
            Output = Clone(source.Output),
        };
        return clone;
    }

    private static UpmixSettings Clone(UpmixSettings? source)
    {
        if (source is null)
        {
            return new UpmixSettings();
        }
        return new UpmixSettings
        {
            CenterGainDb = source.CenterGainDb,
            SurroundGainDb = source.SurroundGainDb,
            DeriveLfe = source.DeriveLfe,
            LfeGainDb = source.LfeGainDb,
            LfeLowPassHz = source.LfeLowPassHz,
        };
    }

    private static OutputSettings Clone(OutputSettings source)
    {
        var output = new OutputSettings
        {
            MasterGainDb = source.MasterGainDb,
            ChannelGainDb = source.ChannelGainDb is null ? new double[8] : (double[])source.ChannelGainDb.Clone(),
            ChannelDelayMs = source.ChannelDelayMs is null ? new double[8] : (double[])source.ChannelDelayMs.Clone(),
            ChannelMute = source.ChannelMute is null ? new bool[8] : (bool[])source.ChannelMute.Clone(),
            Eq = new EqBand[8][],
            Downmix = new DownmixSettings
            {
                CenterGainDb = source.Downmix?.CenterGainDb ?? -3,
                SurroundGainDb = source.Downmix?.SurroundGainDb ?? -3,
                IncludeLfe = source.Downmix?.IncludeLfe ?? false,
                LfeGainDb = source.Downmix?.LfeGainDb ?? -6,
                HeadroomDb = source.Downmix?.HeadroomDb ?? -4.5,
            },
            TwoPointOne = new TwoPointOneSettings
            {
                LfeGainDb = source.TwoPointOne?.LfeGainDb ?? 0,
                LfeLowPassHz = source.TwoPointOne?.LfeLowPassHz ?? 80,
                IncludeSourceLfe = source.TwoPointOne?.IncludeSourceLfe ?? true,
                SourceLfeGainDb = source.TwoPointOne?.SourceLfeGainDb ?? 0,
            },
            BassManagement = new BassManagementSettings
            {
                Enabled = source.BassManagement?.Enabled ?? false,
                CrossoverHz = source.BassManagement?.CrossoverHz ?? 80,
                HighPassSatellites = source.BassManagement?.HighPassSatellites ?? true,
                SumToLfe = source.BassManagement?.SumToLfe ?? true,
                SumToLfeGainDb = source.BassManagement?.SumToLfeGainDb ?? 0,
            },
            Limiter = new LimiterSettings
            {
                Enabled = source.Limiter?.Enabled ?? true,
                CeilingDb = source.Limiter?.CeilingDb ?? -0.3,
                ReleaseMs = source.Limiter?.ReleaseMs ?? 80,
            },
            GainRampMs = source.GainRampMs,
            MaxDelayMs = source.MaxDelayMs,
        };

        for (int ch = 0; ch < 8; ch++)
        {
            output.Eq[ch] = new EqBand[10];
            for (int b = 0; b < 10; b++)
            {
                var src = source.Eq?[ch]?[b];
                output.Eq[ch][b] = new EqBand
                {
                    Enabled = src?.Enabled ?? false,
                    FrequencyHz = src?.FrequencyHz ?? 1000,
                    Q = src?.Q ?? 1,
                    GainDb = src?.GainDb ?? 0,
                };
            }
        }

        return output;
    }
}
